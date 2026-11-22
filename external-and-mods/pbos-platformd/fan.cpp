// fan (was sm8550-fand): fan curve for SM8550 handhelds (Retroid Pocket 6,
// Retroid Pocket Nova, AYN Thor).
//
// ROCKNIX's 0500-set-boot-fanspeed leaves the pwm-fan at a fixed duty (full
// speed on the RP6) and nothing adjusts it. This runs a temperature curve.
//
// Port of the fan loop in Armada's armada-powerd (armada-os/armada,
// GPL-2.0-or-later), with Armada's factory curves and fan settings: average
// of the three hottest cpu/gpu/video/mem zones, exponential smoothing, linear
// curve interpolation quantized to pwm_quantum, ramp limits per 3 s tick, and
// a safe duty on any error.
//
// Config: /usr/share/sm8550-fand/fan.conf, then the device's own file if it
// has one (DEVICE_CONFS: the Retroid Pocket Duo), overridden by /etc/sm8550-fand.conf
// (same keys), then by /var/lib/steamos-sm8550/fan-profile.conf, where the AYN
// Thor's quick controls (sm8550-thor-controlsd) keep the chosen profile. The
// files are read as Python's configparser read them (the Python daemon's
// parser, kept so a file that worked there works here and one that failed
// there fails here). reload() (SIGHUP, `systemctl reload`) reads them again;
// a bad file keeps the previous config.
//
// Thermal zones cannot notify, so the 3 s tick stays; everything else is
// events: the fan's hwmon showing up (a uevent) and reloads. pwm1 and
// pwm1_enable are read back each tick and written only when they differ from
// what the curve wants (the Python version wrote both every tick); a value
// changed behind our back (the thermal framework, a manual echo, a resume)
// is still put right within a tick.
//
// PBOS_PLATFORMD_DRY_RUN=1 is sm8550-fand --dry-run: five ticks printing what
// it would do, writing nothing (works unprivileged), then the loop quits.

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "module.h"
#include "util.h"

namespace pp {

namespace {

const char* const FACTORY_CONF = "/usr/share/sm8550-fand/fan.conf";
// A device's own factory settings, read over fan.conf (same keys).
struct DeviceConf {
    const char* compatible;
    const char* file;
};
const DeviceConf DEVICE_CONFS[] = {
    {"retroidpocket,rpduo", "/usr/share/sm8550-fand/fan-rpduo.conf"},
};
const char* const USER_CONF = "/etc/sm8550-fand.conf";
const char* const QUICK_CONF = "/var/lib/steamos-sm8550/fan-profile.conf";
constexpr double TICK_S = 3;
constexpr int FAN_SAFE_PWM = 128;
constexpr int DRY_RUN_TICKS = 5;
const char* const ZONE_KINDS[] = {"cpu", "gpu", "gpuss", "video", "mem"};

// Python's max(low, min(high, value)), comparisons in its order (a NaN
// smoothing comes out as high, as there).
template <class T>
T clamp(T value, T low, T high) {
    T m = value < high ? value : high;
    return m > low ? m : low;
}

// ---------------------------------------------------------------- Python ---
// The bits of Python the config and the sysfs reads lean on.

// str.strip()'s ASCII whitespace (configparser strips lines and values).
bool py_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' || (c >= 0x1c && c <= 0x1f);
}

std::string py_strip(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && py_space(s[a])) ++a;
    while (b > a && py_space(s[b - 1])) --b;
    return std::string(s.substr(a, b - a));
}

std::string py_rstrip(std::string_view s) {
    size_t b = s.size();
    while (b > 0 && py_space(s[b - 1])) --b;
    return std::string(s.substr(0, b));
}

// Digits with single underscores between them, as int() and float() take.
bool py_digit_run(std::string_view s, size_t& i, std::string& out) {
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    while (i < s.size()) {
        if (std::isdigit(static_cast<unsigned char>(s[i]))) {
            out += s[i++];
        } else if (s[i] == '_' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
            ++i;
        } else {
            break;
        }
    }
    return true;
}

// int(s): base 10, surrounding whitespace, a sign, underscores. Python has
// no limit; past +-10^15 nothing here can tell the difference.
std::optional<long long> py_int(std::string_view text) {
    std::string s = py_strip(text);
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    std::string digits;
    if (!py_digit_run(s, i, digits) || i != s.size()) return std::nullopt;
    long long v = 0;
    for (char c : digits) {
        v = v * 10 + (c - '0');
        if (v > 1000000000000000LL) {
            v = 1000000000000000LL;
            break;
        }
    }
    return neg ? -v : v;
}

// float(s): decimal, exponent, inf/infinity/nan, underscores (not hex).
std::optional<double> py_float(std::string_view text) {
    std::string s = py_strip(text);
    std::string low = pq::lower(s);
    size_t i = 0;
    std::string clean;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) clean += s[i++];
    std::string rest = low.substr(i);
    if (rest == "inf" || rest == "infinity") return clean == "-" ? -HUGE_VAL : HUGE_VAL;
    if (rest == "nan") return std::nan("");
    bool whole = py_digit_run(s, i, clean);
    bool frac = false;
    if (i < s.size() && s[i] == '.') {
        clean += s[i++];
        frac = py_digit_run(s, i, clean);
    }
    if (!whole && !frac) return std::nullopt;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        clean += s[i++];
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) clean += s[i++];
        if (!py_digit_run(s, i, clean)) return std::nullopt;
    }
    if (i != s.size()) return std::nullopt;
    return std::strtod(clean.c_str(), nullptr);
}

// str.isdigit() and int() // 1000 for a sysfs millidegree reading.
bool py_isdigit(std::string_view s) { return pq::is_digits(s); }

// The text decodes as UTF-8 (Python opened the files as UTF-8; anything else
// raised UnicodeDecodeError, a ValueError).
bool valid_utf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        auto c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : 9;
        if (n == 9 || (n == 1 && c < 0xc2) || (n == 3 && c > 0xf4) || i + n >= s.size()) return false;
        for (size_t k = 1; k <= n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xc0) != 0x80) return false;
        if (n == 2) {
            auto c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xe0 && c1 < 0xa0) || (c == 0xed && c1 >= 0xa0)) return false;
        } else if (n == 3) {
            auto c1 = static_cast<unsigned char>(s[i + 1]);
            if ((c == 0xf0 && c1 < 0x90) || (c == 0xf4 && c1 >= 0x90)) return false;
        }
        i += n + 1;
    }
    return true;
}

// ----------------------------------------------------------- configparser ---

// What configparser.Error and ValueError were: the config is not usable.
struct ConfError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// configparser.ConfigParser() as sm8550-fand used it: sections, [DEFAULT],
// = and : separators, # and ; full-line comments (no inline ones), indented
// continuation lines, lowercased keys, strict duplicates within one file,
// %(name)s interpolation, and later files overriding earlier ones.
class Ini {
public:
    // parser.read([...]): files that cannot be opened are skipped; a parse
    // error raises and stops there.
    void read(const std::vector<std::string>& files) {
        for (const auto& f : files) {
            auto text = pq::read_file(f);
            if (!text) continue;
            parse(*text, f);
        }
    }

    // parser.get(section, option, fallback=fallback).
    std::string get(const std::string& section, const std::string& option, const std::string& fallback) const {
        auto v = lookup(section, option);
        return v ? interpolate(section, option, *v) : fallback;
    }
    std::optional<std::string> get(const std::string& section, const std::string& option) const {
        auto v = lookup(section, option);
        if (!v) return std::nullopt;
        return interpolate(section, option, *v);
    }
    // parser.get() without a fallback: a missing section or option raises.
    std::string need(const std::string& section, const std::string& option) const {
        if (!sections_.count(section)) throw ConfError("No section: '" + section + "'");
        auto v = get(section, option);
        if (!v) throw ConfError("No option '" + option + "' in section: '" + section + "'");
        return *v;
    }

private:
    using Lines = std::vector<std::string>;
    using Options = std::map<std::string, Lines>;

    // A value's lines joined, trailing whitespace dropped.
    static std::string joined(const Lines& lines) {
        std::string s;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) s += '\n';
            s += lines[i];
        }
        return py_rstrip(s);
    }

    std::optional<std::string> lookup(const std::string& section, const std::string& option) const {
        auto s = sections_.find(section);
        if (s == sections_.end()) return std::nullopt;
        std::string key = pq::lower(option);
        auto o = s->second.find(key);
        if (o != s->second.end()) return joined(o->second);
        auto d = defaults_.find(key);
        if (d != defaults_.end()) return joined(d->second);
        return std::nullopt;
    }

    // BasicInterpolation.before_get.
    std::string interpolate(const std::string& section, const std::string& option, const std::string& value) const {
        std::string out;
        some(section, option, value, out, 1);
        return out;
    }

    void some(const std::string& section, const std::string& option, std::string rest, std::string& out,
              int depth) const {
        if (depth > 10) throw ConfError("Recursion limit exceeded in value substitution: option '" + option + "'");
        while (!rest.empty()) {
            size_t p = rest.find('%');
            if (p == std::string::npos) {
                out += rest;
                return;
            }
            out += rest.substr(0, p);
            rest = rest.substr(p);
            char c = rest.size() > 1 ? rest[1] : '\0';
            if (c == '%') {
                out += '%';
                rest = rest.substr(2);
            } else if (c == '(') {
                // %\(([^)]+)\)s
                size_t close = rest.find(')', 2);
                if (close == std::string::npos || close == 2 || close + 1 >= rest.size() || rest[close + 1] != 's')
                    throw ConfError("bad interpolation variable reference in option '" + option + "'");
                std::string var = pq::lower(rest.substr(2, close - 2));
                rest = rest.substr(close + 2);
                auto v = lookup(section, var);
                if (!v) throw ConfError("Bad value substitution: option '" + option + "' references '" + var + "'");
                if (v->find('%') != std::string::npos) some(section, option, *v, out, depth + 1);
                else out += *v;
            } else {
                throw ConfError("'%' must be followed by '%' or '(' in option '" + option + "'");
            }
        }
    }

    // RawConfigParser._read.
    void parse(const std::string& text, const std::string& file) {
        if (!valid_utf8(text)) throw ConfError(file + ": not UTF-8");
        // Universal newlines: \n, \r\n and \r end a line.
        std::vector<std::string> lines;
        std::string cur;
        for (size_t i = 0; i < text.size(); ++i) {
            char c = text[i];
            if (c == '\r' || c == '\n') {
                lines.push_back(cur);
                cur.clear();
                if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) lines.push_back(cur);

        std::set<std::string> sections_added;
        std::set<std::pair<std::string, std::string>> options_added;
        Options* cursect = nullptr;
        std::string sectname, optname;  // optname empty: none (Python's falsy)
        size_t indent_level = 0;
        std::string bad;  // non-fatal errors, raised at the end of the file
        int lineno = 0;
        for (const auto& line : lines) {
            ++lineno;
            std::string stripped = py_strip(line);
            bool comment = !stripped.empty() && (stripped[0] == '#' || stripped[0] == ';');
            std::string value = comment ? std::string() : stripped;
            if (value.empty()) {
                // A blank line belongs to the value being read (not a comment).
                if (!comment && cursect && !optname.empty()) (*cursect)[optname].push_back("");
                continue;
            }
            size_t indent = 0;
            while (indent < line.size() && py_space(line[indent])) ++indent;
            if (cursect && !optname.empty() && indent > indent_level) {
                (*cursect)[optname].push_back(value);
                continue;
            }
            indent_level = indent;
            // \[(?P<header>.+)\] matched at the start: up to the last ].
            size_t close = value.rfind(']');
            if (value[0] == '[' && close != std::string::npos && close >= 2) {
                sectname = value.substr(1, close - 1);
                if (sections_.count(sectname)) {
                    if (sections_added.count(sectname))
                        throw ConfError(file + ": section '" + sectname + "' already exists (line " +
                                        std::to_string(lineno) + ")");
                    cursect = &sections_[sectname];
                    sections_added.insert(sectname);
                } else if (sectname == "DEFAULT") {
                    cursect = &defaults_;
                } else {
                    cursect = &sections_[sectname];
                    sections_added.insert(sectname);
                }
                optname.clear();
                continue;
            }
            if (!cursect) throw ConfError(file + ": file contains no section headers (line " +
                                          std::to_string(lineno) + ")");
            // (?P<option>.*?)\s*(?P<vi>[=:])\s*(?P<value>.*)$
            size_t d = value.find_first_of("=:");
            if (d == std::string::npos) {
                bad += " [line " + std::to_string(lineno) + "]";
                continue;
            }
            std::string opt = py_rstrip(value.substr(0, d));
            if (opt.empty()) bad += " [line " + std::to_string(lineno) + "]";
            optname = pq::lower(opt);
            if (!options_added.insert({sectname, optname}).second)
                throw ConfError(file + ": option '" + optname + "' in section '" + sectname +
                                "' already exists (line " + std::to_string(lineno) + ")");
            (*cursect)[optname] = {py_strip(value.substr(d + 1))};
        }
        if (!bad.empty()) throw ConfError("Source contains parsing errors: '" + file + "'" + bad);
    }

    std::map<std::string, Options> sections_;
    Options defaults_;
};

// ----------------------------------------------------------------- config ---

struct Conf {
    std::string profile, curve_name;
    std::vector<std::pair<long long, long long>> curve;  // (temp C, pwm), sorted
    int min_pwm, max_pwm, pwm_quantum;
    long long ramp_up, ramp_down;
    double smoothing;
};

long long need_int(const Ini& ini, const char* key) {
    std::string v = ini.need("fan", key);
    auto n = py_int(v);
    if (!n) throw ConfError("invalid literal for int() with base 10: '" + v + "'");
    return *n;
}

std::vector<std::pair<long long, long long>> parse_curve(const std::string& value) {
    std::vector<std::pair<long long, long long>> curve;
    size_t start = 0;
    while (true) {
        size_t comma = value.find(',', start);
        std::string item = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        size_t colon = item.find(':');
        if (colon != std::string::npos) {
            auto t = py_int(item.substr(0, colon));
            auto p = py_int(item.substr(colon + 1));
            if (t && p) curve.emplace_back(*t, clamp<long long>(*p, 0, 255));
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    std::sort(curve.begin(), curve.end());
    return curve;
}

// The device's own factory file, if it has one ("" otherwise).
std::string device_conf() {
    auto compat = pq::read_file("/sys/firmware/devicetree/base/compatible");
    if (!compat) return "";
    for (const auto& d : DEVICE_CONFS)
        if (compat->find(d.compatible) != std::string::npos) return d.file;
    return "";
}

Conf load_conf() {
    Ini ini;
    ini.read({FACTORY_CONF, device_conf(), USER_CONF, QUICK_CONF});
    Conf c;
    c.profile = pq::lower(py_strip(ini.get("general", "profile", "balanced")));
    c.curve_name = pq::lower(py_strip(ini.get("profile." + c.profile, "fan_curve", "")));
    c.curve = parse_curve(ini.get("fan_curve." + c.curve_name, "curve", ""));
    if (c.curve.empty())
        throw ConfError("profile '" + c.profile + "': no usable fan curve '" + c.curve_name + "'");
    long long min_pwm = need_int(ini, "min_pwm"), max_pwm = need_int(ini, "max_pwm");
    c.ramp_up = need_int(ini, "ramp_up");
    c.ramp_down = need_int(ini, "ramp_down");
    c.min_pwm = static_cast<int>(clamp<long long>(min_pwm, 0, 255));
    c.max_pwm = static_cast<int>(clamp<long long>(max_pwm, c.min_pwm, 255));
    std::string sm = ini.need("fan", "smoothing");
    auto smoothing = py_float(sm);
    if (!smoothing) throw ConfError("could not convert string to float: '" + sm + "'");
    c.smoothing = clamp(*smoothing, 0.0, 0.99);
    long long quantum = 1;
    if (auto q = ini.get("fan", "pwm_quantum")) {
        auto n = py_int(*q);
        if (!n) throw ConfError("invalid literal for int() with base 10: '" + *q + "'");
        quantum = *n;
    }
    c.pwm_quantum = static_cast<int>(clamp<long long>(quantum, 1, 255));
    return c;
}

int quantize(const Conf& c, double pwm) {
    if (c.pwm_quantum > 1) pwm = std::nearbyint(pwm / c.pwm_quantum) * c.pwm_quantum;  // round(): ties to even
    return clamp(static_cast<int>(pwm), c.min_pwm, c.max_pwm);
}

int target_pwm(const Conf& c, long long temp) {
    const auto& curve = c.curve;
    if (temp <= curve.front().first) return quantize(c, static_cast<double>(curve.front().second));
    if (temp >= curve.back().first) return quantize(c, static_cast<double>(curve.back().second));
    for (size_t i = 0; i + 1 < curve.size(); ++i) {
        auto [t0, p0] = curve[i];
        auto [t1, p1] = curve[i + 1];
        if (t0 <= temp && temp <= t1) {
            if (t1 == t0) return quantize(c, static_cast<double>(p1));
            return quantize(c, static_cast<double>(p0) +
                                   static_cast<double>((p1 - p0) * (temp - t0)) / static_cast<double>(t1 - t0));
        }
    }
    return c.min_pwm;
}

std::string find_fan() {
    for (const auto& d : pq::glob("/sys/class/hwmon/hwmon*"))
        if (pq::rd(d + "/name") == "pwmfan" && !pq::rd(d + "/pwm1").empty()) return d;
    return "";
}

std::vector<std::string> temp_paths() {
    std::vector<std::string> paths;
    for (const auto& zone : pq::glob("/sys/class/thermal/thermal_zone*")) {
        std::string type = pq::lower(pq::rd(zone + "/type"));
        for (const char* k : ZONE_KINDS) {
            if (type.rfind(k, 0) == 0) {
                paths.push_back(zone + "/temp");
                break;
            }
        }
    }
    return paths;
}

// Average of the three hottest zones, C (0 if none readable).
long long read_temp(const std::vector<std::string>& paths) {
    std::vector<long long> values;
    for (const auto& p : paths) {
        std::string v = pq::rd(p);
        if (!py_isdigit(v)) continue;
        if (auto n = py_int(v)) values.push_back(*n / 1000);
    }
    std::sort(values.rbegin(), values.rend());
    if (values.size() > 3) values.resize(3);
    if (values.empty()) return 0;
    long long sum = 0;
    for (auto v : values) sum += v;
    return sum / static_cast<long long>(values.size());
}

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

class Fan : public Module {
public:
    const char* name() const override { return "fan"; }

    void start(pq::Loop& loop) override {
        loop_ = &loop;
        dry_run_ = env_set("PBOS_PLATFORMD_DRY_RUN");
        // Test only (tests/fan): a shorter tick, and every tick's line
        // printed as --dry-run prints it, so a harness can follow along.
        if (const char* t = std::getenv("PBOS_PLATFORMD_TEST_FAN_TICK_S")) {
            tick_s_ = std::atof(t) > 0 ? std::atof(t) : TICK_S;
            print_ticks_ = true;
        }
        if (dry_run_) print_ticks_ = true;
        dir_ = find_fan();
        temps_ = temp_paths();
        try {
            conf_ = load_conf();
        } catch (const std::exception& e) {
            // The Python daemon died here and systemd's ExecStopPost left
            // the fan at 128 while it restarted; hold it there until a
            // reload brings a usable config.
            pq::log("fan: config unusable (%s); fan held at %d until a reload", e.what(), FAN_SAFE_PWM);
            if (!dir_.empty()) safe();
            else last_pwm_ = FAN_SAFE_PWM;
        }
        if (conf_) {
            std::string cur = dir_.empty() ? "" : pq::rd(dir_ + "/pwm1");
            last_pwm_ = py_isdigit(cur) ? static_cast<int>(*py_int(cur)) : conf_->min_pwm;
            pq::log("fan: fan %s, %zu zones, profile %s (curve %s)", dir_.empty() ? "not found" : dir_.c_str(),
                temps_.size(), conf_->profile.c_str(), conf_->curve_name.c_str());
        }
        if (dir_.empty() && !dry_run_) {
            // The pwm-fan driver may still be probing: its hwmon's uevent
            // says when it is there.
            pq::log("fan: no pwmfan hwmon; waiting for one");
            loop.on_uevent_env([this](const pq::Loop::UeventEnv& env) {
                if (!dir_.empty()) return;
                auto sub = env.find("SUBSYSTEM"), act = env.find("ACTION");
                bool hwmon_added = sub != env.end() && sub->second == "hwmon" && act != env.end() &&
                                   act->second == "add";
                if (!env.empty() && !hwmon_added) return;  // empty: some were dropped, look anyway
                dir_ = find_fan();
                if (dir_.empty()) return;
                std::string cur = pq::rd(dir_ + "/pwm1");
                if (conf_) last_pwm_ = py_isdigit(cur) ? static_cast<int>(*py_int(cur)) : conf_->min_pwm;
                else safe();
                pq::log("fan: fan %s", dir_.c_str());
                begin();
            });
        }
        begin();
    }

    void stop() override {
        if (timer_) loop_->cancel(timer_);
        timer_ = 0;
        if (dry_run_ || dir_.empty()) return;
        safe();
        pq::log("fan: stopped (fan at %d)", FAN_SAFE_PWM);
    }

    // SIGHUP: read the config files again; a bad one keeps the previous config.
    void reload() override {
        try {
            conf_ = load_conf();
            pq::log("fan: reloaded: profile %s (curve %s)", conf_->profile.c_str(), conf_->curve_name.c_str());
            begin();
        } catch (const std::exception& e) {
            pq::log("fan: reload failed, keeping previous config: %s", e.what());
        }
    }

private:
    // Ticking starts once there is a config and a fan (or a dry run).
    void begin() {
        if (ticking_ || !conf_ || (dir_.empty() && !dry_run_)) return;
        ticking_ = true;
        tick();
    }

    void tick() {
        timer_ = 0;
        try {
            step();
        } catch (const std::exception& e) {  // never leave the fan low on a bad tick
            pq::log("fan: tick failed: %s", e.what());
            safe();
        }
        if (dry_run_ && ++ticks_ >= DRY_RUN_TICKS) {
            loop_->quit();
            return;
        }
        timer_ = loop_->after(tick_s_, [this] { tick(); });
    }

    void step() {
        const Conf& c = *conf_;
        long long temp = read_temp(temps_);
        int target;
        if (temp) {
            double w = c.smoothing;
            smoothed_ = smoothed_ == 0.0 ? static_cast<double>(temp) : smoothed_ * w + static_cast<double>(temp) * (1.0 - w);
            target = target_pwm(c, static_cast<long long>(smoothed_));
        } else {
            target = FAN_SAFE_PWM;
        }
        long long next = target > last_pwm_ ? std::min<long long>(target, last_pwm_ + c.ramp_up)
                                            : std::max<long long>(target, last_pwm_ - c.ramp_down);
        last_pwm_ = static_cast<int>(clamp<long long>(next, c.min_pwm, c.max_pwm));
        set_pwm(last_pwm_, false);
        if (print_ticks_)
            pq::log("fan: temp %lld C (smoothed %.1f) target %d -> pwm %d", temp, smoothed_, target, last_pwm_);
    }

    // pwm-fan mode 3 also cuts the fan supply at duty 0 and in suspend. Both
    // values are written when they are not what they should be (always, when
    // forced: the safe duty).
    void set_pwm(int pwm, bool force) {
        if (dry_run_ || dir_.empty()) return;
        std::string enable = dir_ + "/pwm1_enable", duty = dir_ + "/pwm1", want = std::to_string(pwm);
        if (force || pq::rd(enable) != "3") wr(enable, "3");
        if (force || pq::rd(duty) != want) wr(duty, want);
    }

    void safe() {
        last_pwm_ = FAN_SAFE_PWM;
        set_pwm(FAN_SAFE_PWM, true);
    }

    // Python's open(path, "w").write(value): sysfs ignores the truncation.
    static void wr(const std::string& path, const std::string& value) {
        int fd = open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
        bool ok = fd >= 0 && write(fd, value.data(), value.size()) == static_cast<ssize_t>(value.size());
        int err = errno;
        if (fd >= 0 && close(fd) != 0 && ok) {
            ok = false;
            err = errno;
        }
        if (!ok) pq::log("fan: write %s: %s", path.c_str(), std::strerror(err));
    }

    pq::Loop* loop_ = nullptr;
    bool dry_run_ = false, print_ticks_ = false, ticking_ = false;
    double tick_s_ = TICK_S;
    int ticks_ = 0;
    uint64_t timer_ = 0;
    std::optional<Conf> conf_;
    std::string dir_;
    std::vector<std::string> temps_;
    double smoothed_ = 0.0;
    int last_pwm_ = FAN_SAFE_PWM;
};

}  // namespace

std::unique_ptr<Module> make_fan() { return std::make_unique<Fan>(); }

std::string fan_device_conf() { return device_conf(); }

}  // namespace pp

// thor-controls (was sm8550-thor-controlsd): the root half of Barry
// Launcher's quick controls on the AYN Thor (and the Retroid Pocket Duo,
// which has no stick LEDs): the fan profile and the stick lighting.
//
// Barry Launcher's dashboard runs as the user; its stats service
// (barry_launcher_statsd) and pbos-qamd's Barry section ask this over SOCKET,
// which the input group (the session user) may use. One JSON object per
// connection, one reply line:
//
//   {"op": "get"}
//       {"fan": {"profile": "balanced", "profiles": ["eco", ...]},
//        "lighting": {"enabled": bool, "color": "rrggbb", "brightness": 0-100,
//                     "dimmer": 20-100}
//                    or null when the device has no stick LEDs}
//   {"op": "set", "fanProfile": "performance"}
//   {"op": "set", "lighting": {"enabled": bool, "color": "rrggbb",
//                              "brightness": 0-100, "dimmer": 20-100}}
//                                                     (any of the keys)
//       the new state, as for get; {"error": "..."} on a bad request
//
// The replies are the Python daemon's to the byte (json.dumps: ", " and ": ",
// non-ASCII escaped; its error texts, Python's repr() in them included).
//
// Fan: the fan module's profile, written to FAN_QUICK (read after its own
// config files, so it wins) and applied with `systemctl reload
// sm8550-fand.service` (a SIGHUP to `pbos-platformd --only fan`), started
// and not waited for. Lighting: the multicolor LEDs around the sticks
// (rgb:l1-l4 and rgb:r1-r4 on the Thor, as Armada's armada-rgb lists them),
// saved to LIGHT_STATE and set again at start (DEFAULT_LIGHT until something
// is saved). The LEDs shine at brightness x dimmer: the dashboard's
// brightness buttons are shares of the dimmer, which the Barry Launcher
// plugin sets. The LEDs' PWM level is that to the power LED_GAMMA, so 25%
// looks about a quarter as bright (light output is linear; the eye is not).
//
// The Python version served one connection at a time (a client that sent
// nothing held the others up to 2 s). Here the listening socket and every
// connection are non-blocking: a request is answered once its line is in,
// each connection has its own 2 s idle timeout, and nothing wakes up while
// no one asks.

#include <fcntl.h>
#include <grp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "json.h"
#include "module.h"
#include "util.h"

namespace pp {

namespace {

using pj::Value;

const std::string RUN_DIR = "/run/sm8550-thor";
const std::string SOCKET = RUN_DIR + "/controls.sock";
const std::vector<std::string> FAN_CONFS = {"/usr/share/sm8550-fand/fan.conf", "/etc/sm8550-fand.conf"};
const std::string FAN_QUICK = "/var/lib/steamos-sm8550/fan-profile.conf";
const std::string LIGHT_STATE = "/var/lib/steamos-sm8550/thor-lighting.json";
const std::string LED_GLOB = "/sys/class/leds/rgb:*";
constexpr long long DIMMER_MIN = 20;
constexpr double LED_GAMMA = 2.2;
constexpr size_t REQUEST_MAX = 4096;  // read until a newline or this much
constexpr double IDLE_S = 2;          // a connection that sends nothing for this long
constexpr size_t CLIENTS_MAX = 32;

void log(const std::string& msg) { pq::log("thor-controls: %s", msg.c_str()); }

// What the Python version's ValueError and OSError said: the reply's error.
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// ---------------------------------------------------------------- Python ---

// repr() of a str: single quotes unless the text has one and no double.
std::string py_repr(std::string_view s) {
    char q = (s.find('\'') != std::string_view::npos && s.find('"') == std::string_view::npos) ? '"' : '\'';
    std::string out(1, q);
    for (unsigned char c : s) {
        if (c == '\\' || c == static_cast<unsigned char>(q)) {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (c < 0x20 || c == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + q;
}

// OSError's str(): "[Errno 2] No such file or directory: '/path'".
Error os_error(const std::string& path, int err) {
    return Error("[Errno " + std::to_string(err) + "] " + std::strerror(err) + ": " + py_repr(path));
}

// json.dumps()'s string: ensure_ascii (everything outside space..~ escaped,
// astral characters as surrogate pairs).
void py_quote(std::string& out, std::string_view s) {
    out += '"';
    auto u = [&out](unsigned v) {
        char buf[16];
        std::snprintf(buf, sizeof buf, "\\u%04x", v);
        out += buf;
    };
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
            ++i;
        } else if (c >= 0x20 && c < 0x7f) {
            out += static_cast<char>(c);
            ++i;
        } else if (c < 0x80) {
            switch (c) {
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default: u(c);
            }
            ++i;
        } else {
            // UTF-8 (the request was parsed as JSON text); a bad byte as U+FFFD.
            size_t n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
            unsigned cp = n == 3 ? c & 0x07 : n == 2 ? c & 0x0f : c & 0x1f;
            bool ok = n > 0 && i + n < s.size();
            for (size_t k = 1; ok && k <= n; ++k) {
                unsigned char b = s[i + k];
                if ((b & 0xc0) != 0x80) ok = false;
                else cp = (cp << 6) | (b & 0x3f);
            }
            if (!ok) {
                u(0xfffd);
                ++i;
                continue;
            }
            if (cp >= 0x10000) {
                cp -= 0x10000;
                u(0xd800 + (cp >> 10));
                u(0xdc00 + (cp & 0x3ff));
            } else {
                u(cp);
            }
            i += n + 1;
        }
    }
    out += '"';
}

// json.dumps(v): ", " and ": " between items, numbers as Python wrote them
// (every number here is an int).
void py_dump(std::string& out, const Value& v) {
    switch (v.type()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.as_bool() ? "true" : "false"; break;
        case Value::Type::Number: {
            double d = v.as_double();
            if (d == std::floor(d) && std::fabs(d) < 1e15) out += std::to_string(static_cast<long long>(d));
            else out += Value(d).dump();
            break;
        }
        case Value::Type::String: py_quote(out, v.as_string()); break;
        case Value::Type::Array: {
            out += '[';
            bool first = true;
            for (const auto& x : v.items()) {
                if (!first) out += ", ";
                first = false;
                py_dump(out, x);
            }
            out += ']';
            break;
        }
        case Value::Type::Object: {
            out += '{';
            bool first = true;
            for (const auto& [k, x] : v.members()) {
                if (!first) out += ", ";
                first = false;
                py_quote(out, k);
                out += ": ";
                py_dump(out, x);
            }
            out += '}';
            break;
        }
    }
}

std::string py_dumps(const Value& v) {
    std::string out;
    py_dump(out, v);
    return out;
}

// int(s) of a stripped string: a sign, digits, single underscores between.
long long py_int(const std::string& text) {
    std::string s = pq::trim(text);
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    long long v = 0;
    bool digit = false, ok = i < s.size();
    for (; ok && i < s.size(); ++i) {
        char c = s[i];
        if (c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            if (v > 1000000000000000LL) v = 1000000000000000LL;
            digit = true;
        } else if (c == '_' && digit && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9') {
            digit = false;
        } else {
            ok = false;
        }
    }
    if (!ok) throw Error("invalid literal for int() with base 10: " + py_repr(text));
    return neg ? -v : v;
}

// round(): halves to even, as nearbyint does in the default rounding mode.
long long py_round(double d) { return static_cast<long long>(std::nearbyint(d)); }

std::vector<std::string> py_split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

// open(path, "w").write(value).
void put(const std::string& path, const std::string& value) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) throw os_error(path, errno);
    size_t done = 0;
    while (done < value.size()) {
        ssize_t n = write(fd, value.data() + done, value.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            int err = errno;
            close(fd);
            throw os_error(path, err);
        }
        done += static_cast<size_t>(n);
    }
    close(fd);
}

// The folder made, path.tmp written and renamed over path.
void save(const std::string& path, const std::string& text) {
    std::string dir = pq::dirname(path);
    if (!pq::is_dir(dir) && !pq::makedirs(dir)) throw os_error(dir, errno);
    put(path + ".tmp", text);
    if (rename((path + ".tmp").c_str(), path.c_str()) != 0) throw os_error(path + ".tmp", errno);
}

// ------------------------------------------------------------------- fan ---

// The part of configparser the profile list needs: sections in the order
// they first appear across the files, [DEFAULT], = and : separators, # and ;
// comment lines, indented continuation lines, lowercased keys. A file it
// cannot parse is an error (the Python daemon crashed on one).
class Ini {
public:
    void read(const std::vector<std::string>& files) {
        for (const auto& f : files)
            if (auto text = pq::read_file(f)) parse(*text, f);
    }
    std::vector<std::string> sections() const { return order_; }
    std::string get(const std::string& section, const std::string& key, const std::string& fallback) const {
        auto s = sections_.find(section);
        if (s == sections_.end()) return fallback;
        if (auto o = s->second.find(key); o != s->second.end()) return o->second;
        if (auto d = defaults_.find(key); d != defaults_.end()) return d->second;
        return fallback;
    }

private:
    using Options = std::map<std::string, std::string>;

    void parse(const std::string& text, const std::string& file) {
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
        Options* sect = nullptr;
        std::string name, opt;
        std::set<std::string> seen_sections;
        std::set<std::pair<std::string, std::string>> seen_options;
        size_t indent_level = 0;
        int lineno = 0;
        for (const auto& line : lines) {
            ++lineno;
            std::string value = pq::trim(line);
            if (value.empty() || value[0] == '#' || value[0] == ';') continue;
            size_t indent = line.find_first_not_of(" \t\f\v");
            if (sect && !opt.empty() && indent > indent_level) {
                std::string& v = (*sect)[opt];
                v += "\n" + value;
                continue;
            }
            indent_level = indent;
            size_t close = value.rfind(']');
            if (value[0] == '[' && close != std::string::npos && close >= 2) {
                name = value.substr(1, close - 1);
                if (!seen_sections.insert(name).second)
                    throw Error("While reading from " + py_repr(file) + " [line " + std::to_string(lineno) +
                                "]: section " + py_repr(name) + " already exists");
                if (name == "DEFAULT") {
                    sect = &defaults_;
                } else {
                    if (!sections_.count(name)) order_.push_back(name);
                    sect = &sections_[name];
                }
                opt.clear();
                continue;
            }
            if (!sect) throw Error("File contains no section headers.\nfile: " + py_repr(file) + ", line: " +
                                   std::to_string(lineno) + "\n" + py_repr(line));
            size_t d = value.find_first_of("=:");
            std::string key = d == std::string::npos ? "" : pq::lower(pq::trim(value.substr(0, d)));
            if (key.empty()) throw Error("Source contains parsing errors: " + py_repr(file));
            if (!seen_options.insert({name, key}).second)
                throw Error("While reading from " + py_repr(file) + " [line " + std::to_string(lineno) +
                            "]: option " + py_repr(key) + " in section " + py_repr(name) + " already exists");
            opt = key;
            (*sect)[opt] = pq::trim(value.substr(d + 1));
        }
    }

    std::vector<std::string> order_;
    std::map<std::string, Options> sections_;
    Options defaults_;
};

struct Fan {
    std::string profile;
    std::vector<std::string> profiles;
};

Fan fan_state() {
    Ini ini;
    std::vector<std::string> files = FAN_CONFS;
    // The device's own profiles (the Retroid Pocket Duo's), between the
    // factory file and the user's, as the fan module reads them.
    if (std::string dev = fan_device_conf(); !dev.empty()) files.insert(files.begin() + 1, dev);
    files.push_back(FAN_QUICK);
    ini.read(files);
    Fan f;
    for (auto& s : ini.sections())
        if (s.rfind("profile.", 0) == 0) f.profiles.push_back(s.substr(8));
    f.profile = pq::lower(pq::trim(ini.get("general", "profile", "balanced")));
    return f;
}

void set_fan(const std::string& profile) {
    Fan f = fan_state();
    bool known = false;
    for (auto& p : f.profiles) known |= p == profile;
    if (!known) throw Error("no fan profile " + py_repr(profile));
    save(FAN_QUICK, "# Written by sm8550-thor-controlsd (Barry Launcher's quick controls).\n[general]\nprofile=" +
                        profile + "\n");
    // The fan module reads its files again (SIGHUP); nothing here waits for it.
    pq::spawn({"systemctl", "reload", "sm8550-fand.service"});
    log("fan profile " + profile);
}

// -------------------------------------------------------------- lighting ---

struct Light {
    bool enabled = true;
    std::string color = "8c00ff";
    long long brightness = 50, dimmer = 100;
};
// Out of the box: purple, the color the other pb-os devices start with
// (they breathe; these LEDs only take a steady color).
const Light DEFAULT_LIGHT;

// The keys of a lighting object that hold good values.
struct LightChange {
    std::optional<bool> enabled;
    std::optional<std::string> color;
    std::optional<long long> brightness, dimmer;
    std::set<std::string> keys;
};

std::vector<std::string> leds() {
    std::vector<std::string> out;
    for (auto& d : pq::glob(LED_GLOB))
        if (pq::exists(d + "/multi_intensity")) out.push_back(d);
    return out;
}

// An int or float in lo..hi (not a bool), as int() takes it.
std::optional<long long> in_range(const Value* v, double lo, double hi) {
    if (!v || !v->is_number()) return std::nullopt;
    double d = v->as_double();
    if (!(lo <= d && d <= hi)) return std::nullopt;
    return static_cast<long long>(std::trunc(d));
}

LightChange clean_light(const Value& v) {
    LightChange out;
    if (const Value* e = v.find("enabled"); e && e->is_bool()) out.enabled = e->as_bool(), out.keys.insert("enabled");
    if (const Value* c = v.find("color"); c && c->is_string()) {
        const std::string& s = c->as_string();
        bool hex = s.size() == 6;
        for (char ch : s) hex &= std::isxdigit(static_cast<unsigned char>(ch)) != 0;
        if (hex) out.color = pq::lower(s), out.keys.insert("color");
    }
    if (auto b = in_range(v.find("brightness"), 0, 100)) out.brightness = b, out.keys.insert("brightness");
    if (auto m = in_range(v.find("dimmer"), DIMMER_MIN, 100)) out.dimmer = m, out.keys.insert("dimmer");
    return out;
}

Light merged(Light l, const LightChange& c) {
    if (c.enabled) l.enabled = *c.enabled;
    if (c.color) l.color = *c.color;
    if (c.brightness) l.brightness = *c.brightness;
    if (c.dimmer) l.dimmer = *c.dimmer;
    return l;
}

Value to_value(const Light& l) {
    Value v = Value::object();
    v["enabled"] = l.enabled;
    v["color"] = l.color;
    v["brightness"] = l.brightness;
    v["dimmer"] = l.dimmer;
    return v;
}

// The Python dict's repr, for the log.
std::string repr(const Light& l) {
    return "{'enabled': " + std::string(l.enabled ? "True" : "False") + ", 'color': " + py_repr(l.color) +
           ", 'brightness': " + std::to_string(l.brightness) + ", 'dimmer': " + std::to_string(l.dimmer) + "}";
}

long long led_max(const std::string& d) {
    std::string t = pq::rd(d + "/max_brightness", "255");
    return t.empty() ? 255 : py_int(t);
}

std::optional<Light> light_state() {
    auto found = leds();
    if (found.empty()) return std::nullopt;
    if (auto text = pq::read_file(LIGHT_STATE))
        if (auto saved = Value::parse(*text); saved && saved->is_object()) return merged(DEFAULT_LIGHT, clean_light(*saved));
    // Nothing saved yet: describe what the first LED shows.
    const std::string& d = found[0];
    long long top = led_max(d);
    auto order = py_split(pq::rd(d + "/multi_index"));
    auto values = py_split(pq::rd(d + "/multi_intensity"));
    std::map<std::string, long long> rgb;
    for (size_t i = 0; i < values.size(); ++i) {
        long long v = py_int(values[i]);
        if (i < order.size()) rgb[order[i]] = v;
    }
    if (top == 0) throw Error("division by zero");
    std::string color;
    for (const char* c : {"red", "green", "blue"}) {
        long long n = std::min<long long>(255, py_round(static_cast<double>(rgb.count(c) ? rgb[c] : 0) * 255 / top));
        char buf[24];
        if (n < 0) std::snprintf(buf, sizeof buf, "-%llx", -n);
        else std::snprintf(buf, sizeof buf, "%02llx", n);
        color += buf;
    }
    std::string lt = pq::rd(d + "/brightness", "0");
    long long level = lt.empty() ? 0 : py_int(lt);
    Light l;
    l.enabled = level > 0;
    l.color = color != "000000" ? color : DEFAULT_LIGHT.color;
    l.brightness = level ? py_round(100 * std::pow(static_cast<double>(level) / top, 1 / LED_GAMMA))
                         : DEFAULT_LIGHT.brightness;
    l.dimmer = DEFAULT_LIGHT.dimmer;
    return l;
}

void apply_light(const Light& s) {
    std::map<std::string, long long> rgb = {{"red", std::stoll(s.color.substr(0, 2), nullptr, 16)},
                                            {"green", std::stoll(s.color.substr(2, 2), nullptr, 16)},
                                            {"blue", std::stoll(s.color.substr(4, 2), nullptr, 16)}};
    for (auto& d : leds()) {
        long long top = led_max(d);
        if (!s.enabled || s.brightness == 0) {
            put(d + "/brightness", "0");
            continue;
        }
        std::string values;
        for (auto& c : py_split(pq::rd(d + "/multi_index"))) {
            if (!values.empty()) values += ' ';
            values += std::to_string(py_round(static_cast<double>(rgb.count(c) ? rgb[c] : 0) * top / 255));
        }
        put(d + "/multi_intensity", values);
        double level = static_cast<double>(s.brightness) / 100 * s.dimmer / 100;
        put(d + "/brightness", std::to_string(std::max<long long>(1, py_round(std::pow(level, LED_GAMMA) * top))));
    }
}

void set_light(const Value& v) {
    auto current = light_state();
    if (!current) throw Error("no stick LEDs on this device");
    LightChange change = clean_light(v);
    std::vector<std::string> bad;  // sorted, as the Python's sorted(set)
    for (const auto& [k, _] : v.members())
        if (!change.keys.count(k)) bad.push_back(k);
    std::sort(bad.begin(), bad.end());
    bad.erase(std::unique(bad.begin(), bad.end()), bad.end());
    if (!bad.empty()) {
        std::string list;
        for (auto& k : bad) list += (list.empty() ? "" : ", ") + py_repr(k);
        throw Error("bad lighting values: [" + list + "]");
    }
    Light state = merged(*current, change);
    apply_light(state);
    save(LIGHT_STATE, to_value(state).dump(2) + "\n");
    log("lighting " + repr(state));
}

// --------------------------------------------------------------- service ---

Value state() {
    Fan f = fan_state();
    Value fan = Value::object();
    fan["profile"] = f.profile;
    Value list = Value::array();
    for (auto& p : f.profiles) list.push(p);
    fan["profiles"] = std::move(list);
    Value out = Value::object();
    out["fan"] = std::move(fan);
    auto l = light_state();
    out["lighting"] = l ? to_value(*l) : Value();
    return out;
}

Value error(const std::string& msg) {
    Value v = Value::object();
    v["error"] = msg;
    return v;
}

Value handle(const Value& req) {
    const Value& op = req["op"];
    bool get = op.is_string() && op.as_string() == "get";
    if (get) return state();
    if (!op.is_string() || op.as_string() != "set") return error("op is get or set");
    try {
        if (const Value* f = req.find("fanProfile")) {
            if (!f->is_string()) throw Error("fanProfile is a name");
            set_fan(pq::lower(f->as_string()));
        }
        if (const Value* l = req.find("lighting")) {
            if (!l->is_object()) throw Error("lighting is an object");
            set_light(*l);
        }
    } catch (const Error& e) {
        return error(e.what());
    }
    return state();
}

// The reply line for a request's bytes.
std::string answer(const std::string& data) {
    Value reply;
    try {
        std::string text = data.empty() ? "{}" : data;
        auto req = Value::parse(text);
        if (!req && text.find_first_not_of(" \t\r\n") == std::string::npos) {
            // Only whitespace (an empty line): json's own words for it.
            size_t pos = text.size(), nl = text.rfind('\n');
            long line = 1 + static_cast<long>(std::count(text.begin(), text.end(), '\n'));
            long col = static_cast<long>(nl == std::string::npos ? pos + 1 : pos - nl);
            throw Error("Expecting value: line " + std::to_string(line) + " column " + std::to_string(col) + " (char " +
                        std::to_string(pos) + ")");
        }
        // Python's texts for other JSON errors are not copied.
        if (!req) throw Error("not JSON");
        reply = req->is_object() ? handle(*req) : error("need a JSON object");
    } catch (const Error& e) {
        reply = error(e.what());
    } catch (const std::exception& e) {
        reply = error(e.what());
    }
    return py_dumps(reply) + "\n";
}

class ThorControls : public Module {
public:
    const char* name() const override { return "thor-controls"; }

    void start(pq::Loop& loop) override {
        loop_ = &loop;
        // The saved lighting, or the default until something is saved (not
        // saved itself, so a later default change still reaches it).
        try {
            bool saved = pq::exists(LIGHT_STATE);
            std::optional<Light> l = saved ? light_state() : (leds().empty() ? std::nullopt : std::optional(DEFAULT_LIGHT));
            if (l) {
                apply_light(*l);
                log(std::string("lighting ") + (saved ? "restored" : "default") + ": " + repr(*l));
            }
        } catch (const Error& e) {
            log(std::string("cannot restore the lighting: ") + e.what());
        }
        std::string names;
        for (auto& d : leds()) names += (names.empty() ? "" : ", ") + pq::basename(d);
        log("stick LEDs: " + (names.empty() ? std::string("none") : names));
        listen();
    }

    void stop() override {
        for (auto& [id, c] : clients_) close(c.fd);
        clients_.clear();
        if (listen_fd_ >= 0) close(listen_fd_);
        if (conn_ep_ >= 0) close(conn_ep_);
        listen_fd_ = conn_ep_ = -1;
    }

private:
    struct Client {
        int fd;
        std::string data;
        uint64_t timer = 0;
    };

    void listen() {
        pq::makedirs(RUN_DIR);
        gid_t gid = static_cast<gid_t>(-1);
        if (group* g = getgrnam("input")) gid = g->gr_gid;
        unlink(SOCKET.c_str());
        listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", SOCKET.c_str());
        if (listen_fd_ < 0 || bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            chown(SOCKET.c_str(), static_cast<uid_t>(-1), gid) != 0 || chmod(SOCKET.c_str(), 0660) != 0 ||
            ::listen(listen_fd_, 8) != 0) {
            log("cannot listen on " + SOCKET + ": " + std::strerror(errno));
            if (listen_fd_ >= 0) close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        // The connections live in an epoll set of their own, so each comes
        // off it before its socket closes.
        conn_ep_ = epoll_create1(EPOLL_CLOEXEC);
        loop_->on_readable(conn_ep_, [this] { readable(); });
        loop_->on_readable(listen_fd_, [this] { accept_all(); });
    }

    void accept_all() {
        while (true) {
            int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno == EINTR || errno == ECONNABORTED) continue;
                return;  // EAGAIN, or out of fds: the rest stay queued
            }
            if (clients_.size() >= CLIENTS_MAX) {
                close(fd);
                continue;
            }
            uint64_t id = next_id_++;
            epoll_event ev{};
            ev.events = EPOLLIN | EPOLLRDHUP;
            ev.data.u64 = id;
            if (epoll_ctl(conn_ep_, EPOLL_CTL_ADD, fd, &ev) != 0) {
                close(fd);
                continue;
            }
            clients_[id] = Client{fd, {}, 0};
            arm(id);
        }
    }

    // As Python's socket timeout: per receive, not for the whole request.
    void arm(uint64_t id) {
        Client& c = clients_[id];
        if (c.timer) loop_->cancel(c.timer);
        c.timer = loop_->after(IDLE_S, [this, id] {
            auto it = clients_.find(id);
            if (it == clients_.end()) return;
            it->second.timer = 0;
            finish(id, py_dumps(error("timed out")) + "\n");
        });
    }

    void readable() {
        epoll_event evs[16];
        int n = epoll_wait(conn_ep_, evs, 16, 0);
        for (int i = 0; i < n; ++i) receive(evs[i].data.u64);
    }

    void receive(uint64_t id) {
        auto it = clients_.find(id);
        if (it == clients_.end()) return;
        Client& c = it->second;
        while (c.data.find('\n') == std::string::npos && c.data.size() < REQUEST_MAX) {
            char buf[4096];
            ssize_t n = recv(c.fd, buf, sizeof buf, 0);
            if (n > 0) {
                c.data.append(buf, static_cast<size_t>(n));
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                arm(id);
                return;  // the rest of the line later
            }
            if (n < 0) {
                finish(id, py_dumps(error("[Errno " + std::to_string(errno) + "] " + std::strerror(errno))) + "\n");
                return;
            }
            break;  // the client closed its side: what came is the request
        }
        finish(id, answer(c.data));
    }

    // The reply, then the connection is closed. Replies are small: they fit
    // in the socket's buffer, so one non-blocking send.
    void finish(uint64_t id, const std::string& reply) {
        auto it = clients_.find(id);
        if (it == clients_.end()) return;
        Client c = std::move(it->second);
        clients_.erase(it);
        if (c.timer) loop_->cancel(c.timer);
        (void)send(c.fd, reply.data(), reply.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
        epoll_ctl(conn_ep_, EPOLL_CTL_DEL, c.fd, nullptr);
        close(c.fd);
    }

    pq::Loop* loop_ = nullptr;
    int listen_fd_ = -1, conn_ep_ = -1;
    uint64_t next_id_ = 1;
    std::map<uint64_t, Client> clients_;
};

}  // namespace

std::unique_ptr<Module> make_thor_controls() { return std::make_unique<ThorControls>(); }

}  // namespace pp

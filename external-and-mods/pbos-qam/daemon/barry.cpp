// The Barry Launcher section (AYN Thor, Retroid Pocket Duo): the backend of
// what was the Barry Launcher Decky plugin (folder dual-screen), method for
// method. On the Duo Steam's slider is the top screen's alone (thor-backlight's
// DUO profile): get_state says so ("steamBoth" false), and that the device has
// no Barry button ("button" "").
//
// One switch for the bottom screen, and each screen's brightness, as on
// AYN's Android: Steam's brightness slider sets both screens
// (sm8550-thor-backlightd follows it), the top and bottom sliders here one
// each (SCREENS). The daemon applies and enforces STATE and SCREENS; this
// only records them. Also the stick lighting's dimmer (sm8550-thor-controlsd).
//
// Barry Launcher's Keyboard app in place of Steam's on-screen keyboard (the
// frontend asks whether it can and opens it there), its home apps, user
// apps and apps that open with games (barry_launcher_shelld keeps them), its
// themes (barry_launcher_statsd lists them), its keyboards' language packs
// and its quick settings page's widget packs: added from a USB drive or
// Downloads, unpacked safely into the
// user's ~/.local/share/barry_launcher, removed again. The game selected in
// Steam, for the bottom screen's hero art (heroart.cpp), and the user's
// SteamGridDB key for it.
//
// "Two-screen emulators": the user's own emulators (none ship with PB-OS)
// set to draw their second screen in a window of its own, which gamescope's
// dual-screen branch shows on the bottom screen. Only their config files
// change, a few keys each (TWO_SCREEN); the values found before are kept in
// TWO_SCREEN_STATE and put back when the switch goes off. Nothing changes
// while one of them runs (they write their settings back when they quit):
// the plugin looked every 30 s; here a config file written, an emulator
// installed or an emulator's process ending (inotify, pidfd) is the cue.

#include <pwd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <thread>
#include <tuple>

#include "archive.h"
#include "heroart.h"
#include "section.h"
#include "util.h"

namespace pq {

namespace {

using pj::Value;

const std::string TOP = "/sys/class/backlight/ae96000.dsi.0";
const std::string BOTTOM = "/sys/class/backlight/ae94000.dsi.0";
const std::string STATE = "/var/lib/steamos-sm8550/thor-bottom-screen";
const std::string SCREENS = "/var/lib/steamos-sm8550/thor-screen-brightness.json";
const std::string CONTROLS = "/run/sm8550-thor/controls.sock";
constexpr long long RGB_DIM_MIN = 20;
constexpr int SHELLD = 47824;
constexpr int STATSD = 47823;
constexpr double YIELD_WAIT_S = 2.0;
const std::string TWO_SCREEN_STATE = "/var/lib/steamos-sm8550/thor-two-screen.json";
// After an emulator's config changes, before looking (it may still be quitting).
constexpr double TWO_SCREEN_SETTLE_S = 2.0;

struct Key {
    std::optional<std::string> section;
    std::string key, value;
};
struct Emulator {
    std::string name;
    std::vector<std::string> configs;  // under the user's home; Flatpak ones glob
    std::string kind;                   // ini (Qt, key=value), toml (key = value), xml (<key>value</key>)
    std::string process;                // in the program's name, lowercase
    std::vector<Key> keys;
};

const std::vector<Emulator>& two_screen() {
    static const std::vector<Emulator> list = {
        // View > Screen Layout > Separate Windows (LayoutOption 4).
        {"Azahar", {".config/azahar-emu/qt-config.ini", ".var/app/*/config/azahar-emu/qt-config.ini"}, "ini", "azahar",
         {{"Layout", "layout_option", "4"}, {"Layout", "layout_option\\default", "false"}}},
        {"Lime3DS", {".config/lime3ds-emu/qt-config.ini", ".var/app/*/config/lime3ds-emu/qt-config.ini"}, "ini",
         "lime3ds", {{"Layout", "layout_option", "4"}, {"Layout", "layout_option\\default", "false"}}},
        {"Citra", {".config/citra-emu/qt-config.ini", ".var/app/*/config/citra-emu/qt-config.ini"}, "ini", "citra",
         {{"Layout", "layout_option", "4"}, {"Layout", "layout_option\\default", "false"}}},
        // A second window, kept across launches: the top screen in the first
        // (screenSizing_TopOnly 4), the bottom one in the second (BotOnly 5).
        // Both with the Natural layout (0): Hybrid (3) ignores the sizing and
        // draws a big screen with both small ones in each window.
        {"melonDS", {".config/melonDS/melonDS.toml", ".var/app/*/config/melonDS/melonDS.toml"}, "toml", "melonds",
         {{"Instance0.Window0", "ScreenLayout", "0"},
          {"Instance0.Window0", "ScreenSizing", "4"},
          {"Instance0.Window1", "Enabled", "true"},
          {"Instance0.Window1", "ScreenLayout", "0"},
          {"Instance0.Window1", "ScreenSizing", "5"}}},
        // Options > Separate GamePad view.
        {"Cemu", {".config/Cemu/settings.xml", ".var/app/*/config/Cemu/settings.xml"}, "xml", "cemu",
         {{std::nullopt, "open_pad", "true"}}},
    };
    return list;
}

std::string sec_name(const std::optional<std::string>& s) { return s ? *s : "None"; }  // as Python's f"{None}"

// ------------------------------------------------------- local services --
struct NetError {
    std::string msg;
    int code = 0;  // the HTTP status, 0 when nothing answered
};

Value json_of(const HttpResult& r) {
    auto v = Value::parse(r.body);
    if (!v) throw NetError{"Expecting value: not JSON", r.status};
    return *v;
}

// urllib.urlopen: an answer of 400 or more raises.
Value http_json(const char* method, int port, const std::string& path, const Value* body, double timeout) {
    std::string text = body ? body->dump() : std::string();
    auto r = http(method, port, path, body ? &text : nullptr, timeout);
    if (r.status == 0) throw NetError{"<urlopen error " + r.error + ">", 0};
    if (r.status >= 400) throw NetError{"HTTP Error " + std::to_string(r.status), r.status};
    return json_of(r);
}

// barry_launcher_shelld's JSON answer, an error's included.
Value shelld_answer(const std::string& path, const Value& body, double timeout = 2) {
    std::string text = body.dump();
    auto r = http("POST", SHELLD, path, &text, timeout);
    if (r.status == 0) throw NetError{"<urlopen error " + r.error + ">", 0};
    auto v = Value::parse(r.body);
    if (v) return *v;
    if (r.status >= 400) {
        Value e = Value::object();
        e["ok"] = false;
        e["error"] = "Barry Launcher answered " + std::to_string(r.status) + ".";
        return e;
    }
    throw NetError{"Expecting value: not JSON", r.status};
}

Value not_answering(const NetError& e, const char* tail = ".") {
    Value v = Value::object();
    v["ok"] = false;
    v["error"] = "Barry Launcher isn't answering (" + e.msg + ")" + tail;
    return v;
}

bool is_on() {
    auto s = read_file(STATE);
    return !s || trim(*s) != "off";
}

Value screens() { return read_json(SCREENS); }

Value brightness() {
    // Each screen's slider, 0-100; null until the daemon has set them.
    Value d = screens();
    if (!d.has("top") || !d.has("bottom")) return Value();
    Value out = Value::object();
    for (const char* k : {"top", "bottom"}) {
        double v = d[k].as_double(NAN);
        if (std::isnan(v)) return Value();
        out[k] = static_cast<long long>(std::nearbyint(std::max(0.0, std::min(1.0, v)) * 100));
    }
    return out;
}

Value controlsd(const Value& req) {
    auto line = unix_request(CONTROLS, req.dump(), 2);
    if (!line) {
        log("barry: sm8550-thor-controlsd: no answer");
        return Value();
    }
    auto v = Value::parse(*line);
    if (!v || !v->is_object() || v->has("error")) return Value();
    return *v;
}

Value rgb_dimmer(const Value& state) {
    const Value& light = state["lighting"];
    if (!light.truthy()) return Value();
    return light.has("dimmer") ? light["dimmer"] : Value(100);
}

// ------------------------------------------------- two-screen emulators --
std::string home() { return desktop_user().home; }

std::string eol_of(const std::string& text) { return text.find("\r\n") != std::string::npos ? "\r\n" : "\n"; }

std::vector<std::string> split_by(const std::string& text, const std::string& sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t at = text.find(sep, start);
        if (at == std::string::npos) {
            out.push_back(text.substr(start));
            return out;
        }
        out.push_back(text.substr(start, at - start));
        start = at + sep.size();
    }
}

std::string join_by(const std::vector<std::string>& lines, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += sep;
        out += lines[i];
    }
    return out;
}

struct Sec {
    std::optional<std::string> name;
    size_t start, end;
};

// (name, first line, end) of each [section]; no name for the lines before the first.
std::vector<Sec> sections_of(const std::vector<std::string>& lines) {
    static const std::regex head(R"(^\s*\[([^\]]+)\]\s*$)");
    std::vector<Sec> out;
    std::optional<std::string> name;
    size_t start = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        std::smatch m;
        if (std::regex_match(lines[i], m, head)) {
            out.push_back({name, start, i});
            name = m[1].str();
            start = i + 1;
        }
    }
    out.push_back({name, start, lines.size()});
    return out;
}

bool key_line(const std::string& line, const std::string& key) {
    auto eq = line.find('=');
    return eq != std::string::npos && trim(line.substr(0, eq)) == key;
}

std::string lstrip_tabs(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

// The xml element's whole line, or where it is (as the plugin's regexes).
struct XmlHit {
    size_t start = std::string::npos, end = 0;  // end: after </key>, its blanks and the line end
    std::string line;                            // line start .. after </key> and blanks
};

XmlHit xml_find(const std::string& text, const std::string& key, bool whole_line) {
    std::string open = "<" + key + ">", close = "</" + key + ">";
    size_t ls = 0;
    while (ls <= text.size()) {
        size_t le = text.find('\n', ls);
        if (le == std::string::npos) le = text.size();
        std::string line = text.substr(ls, le - ls);
        size_t lead = line.size() - lstrip_tabs(line).size();
        if (line.compare(lead, open.size(), open) == 0) {
            size_t c = line.find(close, lead + open.size());
            while (c != std::string::npos) {
                size_t after = c + close.size();
                size_t blanks = after;
                while (blanks < line.size() && (line[blanks] == ' ' || line[blanks] == '\t')) ++blanks;
                bool at_end = blanks == line.size() || (blanks + 1 == line.size() && line[blanks] == '\r');
                if (!whole_line || at_end) {
                    XmlHit hit;
                    hit.start = ls;
                    hit.line = line.substr(0, blanks);
                    size_t end = ls + blanks;
                    if (!whole_line) {
                        // (?:\r?\n)? after the blanks.
                        if (end < text.size() && text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n') end += 2;
                        else if (end < text.size() && text[end] == '\n') end += 1;
                    }
                    hit.end = end;
                    return hit;
                }
                if (!whole_line) break;
                c = line.find(close, c + 1);
            }
        }
        if (le == text.size()) break;
        ls = le + 1;
    }
    return {};
}

std::optional<std::string> get_key(const std::string& text, const std::string& kind,
                                   const std::optional<std::string>& section, const std::string& key) {
    if (kind == "xml") {
        auto hit = xml_find(text, key, true);
        if (hit.start == std::string::npos) return std::nullopt;
        return hit.line;
    }
    auto lines = split_by(text, eol_of(text));
    for (const auto& s : sections_of(lines)) {
        if (s.name != section) continue;
        for (size_t i = s.start; i < s.end; ++i)
            if (key_line(lines[i], key)) return lines[i];
    }
    return std::nullopt;
}

std::string line_for(const std::string& kind, const std::string& key, const std::string& value) {
    if (kind == "ini") return key + "=" + value;
    if (kind == "toml") return key + " = " + value;
    return "    <" + key + ">" + value + "</" + key + ">";
}

// Sets the key's line (none: removes it), adding the section if needed.
std::string put_key(const std::string& text, const std::string& kind, const std::optional<std::string>& section,
                    const std::string& key, const std::optional<std::string>& line) {
    std::string eol = eol_of(text);
    if (kind == "xml") {
        auto hit = xml_find(text, key, false);
        if (hit.start != std::string::npos)
            return text.substr(0, hit.start) + (line ? *line + eol : "") + text.substr(hit.end);
        if (!line) return text;
        auto at = text.find("</content>");
        return at == std::string::npos ? text : text.substr(0, at) + *line + eol + text.substr(at);
    }
    auto lines = split_by(text, eol);
    for (const auto& s : sections_of(lines)) {
        if (s.name != section) continue;
        for (size_t i = s.start; i < s.end; ++i) {
            if (key_line(lines[i], key)) {
                if (line) lines[i] = *line;
                else lines.erase(lines.begin() + static_cast<long>(i));
                return join_by(lines, eol);
            }
        }
        if (!line) return text;
        // After the section's last non-blank line.
        size_t at = s.end;
        while (at > s.start && trim(lines[at - 1]).empty()) --at;
        lines.insert(lines.begin() + static_cast<long>(at), *line);
        return join_by(lines, eol);
    }
    if (!line) return text;
    std::string body = text;
    while (!body.empty() && (body.back() == '\r' || body.back() == '\n')) body.pop_back();
    return body + eol + eol + "[" + sec_name(section) + "]" + eol + *line + eol;
}

bool has_section(const std::string& text, const std::optional<std::string>& section) {
    if (!section) return true;
    for (const auto& s : sections_of(split_by(text, eol_of(text))))
        if (s.name == section) return true;
    return false;
}

std::string drop_added_section(const std::string& text, const std::string& section) {
    // Takes away a section put_key appended at the end, once it is empty.
    std::string eol = eol_of(text);
    std::string tail = eol + eol + "[" + section + "]" + eol;
    if (text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0)
        return text.substr(0, text.size() - tail.size()) + eol;
    return text;
}

bool write_as_owner(const std::string& path, const std::string& text) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    std::string tmp = path + ".dual-screen.tmp";
    {
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) return false;
        bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = std::fclose(f) == 0 && ok;
        if (!ok) {
            unlink(tmp.c_str());
            return false;
        }
    }
    if (chown(tmp.c_str(), st.st_uid, st.st_gid) != 0) {}
    chmod(tmp.c_str(), st.st_mode & 07777);
    return rename(tmp.c_str(), path.c_str()) == 0;
}

struct ConfigFile {
    const Emulator* emu;
    std::string path;
};

std::vector<ConfigFile> two_screen_files() {
    std::vector<ConfigFile> out;
    for (const auto& e : two_screen())
        for (const auto& pattern : e.configs)
            for (const auto& path : glob(join(home(), pattern))) out.push_back({&e, path});
    return out;
}

// Emulators of TWO_SCREEN running now, with their processes.
std::map<std::string, std::vector<pid_t>> two_screen_running() {
    std::map<std::string, std::vector<pid_t>> found;
    for (const auto& pid : listdir("/proc")) {
        if (!is_digits(pid)) continue;
        auto cmd = read_file("/proc/" + pid + "/cmdline");
        if (!cmd) continue;
        std::string exe = lower(basename(cmd->substr(0, cmd->find('\0'))));
        for (const auto& e : two_screen())
            if (exe.find(e.process) != std::string::npos) found[e.name].push_back(std::atoi(pid.c_str()));
    }
    return found;
}

std::vector<std::string> running_names() {
    std::vector<std::string> out;
    for (auto& [name, pids] : two_screen_running()) out.push_back(name);
    return out;
}

bool two_screen_enabled(const Value& state) { return state.has("enabled") ? state["enabled"].truthy() : true; }

bool two_screen_applied(const std::string& text, const Emulator& e) {
    for (const auto& k : e.keys) {
        auto got = get_key(text, e.kind, k.section, k.key);
        if (!got || trim(*got) != trim(line_for(e.kind, k.key, k.value))) return false;
    }
    return true;
}

// One map of Steam's binary VDF from data[i]; i ends after it.
Value vdf_map(const std::string& data, size_t& i) {
    Value out = Value::object();
    auto cstr = [&](size_t& at) {
        size_t end = data.find('\0', at);
        if (end == std::string::npos) throw std::runtime_error("VDF: no end");
        std::string s = data.substr(at, end - at);
        at = end + 1;
        return s;
    };
    while (i < data.size()) {
        unsigned char kind = static_cast<unsigned char>(data[i++]);
        if (kind == 0x08) return out;
        std::string key = cstr(i);
        if (kind == 0x00) out[key] = vdf_map(data, i);
        else if (kind == 0x01) out[key] = cstr(i);
        else if (kind == 0x02 || kind == 0x03 || kind == 0x04 || kind == 0x06) i += 4;
        else if (kind == 0x07 || kind == 0x0a) i += 8;
        else throw std::runtime_error("VDF type");
    }
    return out;
}

int ds_on_retroarch() {
    // Steam shortcuts (Steam ROM Manager's) that run DS games in RetroArch,
    // which draws both screens in one window. Only read, never changed.
    static const std::regex core(R"((melonds|desmume)\w*_libretro)");
    static const std::regex nds(R"(\.nds\b)");
    std::set<std::string> seen;
    int count = 0;
    for (const auto& path : glob(home() + "/.local/share/Steam/userdata/*/config/shortcuts.vdf")) {
        std::string real = realpath(path);
        if (!seen.insert(real).second) continue;
        Value shortcuts;
        try {
            auto data = read_file(real);
            if (!data) continue;
            size_t i = 0;
            shortcuts = vdf_map(*data, i);
        } catch (const std::exception&) {
            continue;
        }
        for (const auto& [id, entry] : shortcuts["shortcuts"].members()) {
            if (!entry.is_object()) continue;
            std::string cmd = lower(entry["exe"].str() + " " + entry["Exe"].str() + " " + entry["LaunchOptions"].str());
            bool ra = cmd.find("retroarch") != std::string::npos || cmd.find("libretro") != std::string::npos;
            if (ra && (std::regex_search(cmd, core) || std::regex_search(cmd, nds))) ++count;
        }
    }
    return count;
}

std::mutex two_screen_lock;

// Sets (or puts back) one config file's keys; true if it changed.
bool two_screen_file(const std::string& path, const Emulator& e, bool enabled, Value& originals) {
    auto read = read_file(path);
    if (!read) return false;
    const std::string& text = *read;
    std::string next = text;
    if (enabled) {
        Value& saved = originals[path];
        if (!saved.is_object()) saved = Value::object();
        for (const auto& k : e.keys) {
            if (e.kind != "xml" && !has_section(next, k.section)) {
                std::string mark = sec_name(k.section) + "\t";
                if (!saved.has(mark)) saved[mark] = "added";
            }
            std::string id = sec_name(k.section) + "\t" + k.key;
            if (!saved.has(id)) {
                auto old = get_key(text, e.kind, k.section, k.key);
                saved[id] = old ? Value(*old) : Value();
            }
            next = put_key(next, e.kind, k.section, k.key, line_for(e.kind, k.key, k.value));
        }
    } else if (originals.has(path)) {
        Value saved = originals[path];
        originals.erase(path);
        for (const auto& k : e.keys) {
            std::string id = sec_name(k.section) + "\t" + k.key;
            if (!saved.has(id)) continue;
            const Value& old = saved[id];
            next = put_key(next, e.kind, k.section, k.key,
                           old.is_string() ? std::optional<std::string>(old.as_string()) : std::nullopt);
        }
        // Sections it added, the last one first.
        std::vector<std::string> secs;
        for (const auto& k : e.keys)
            if (k.section && std::find(secs.begin(), secs.end(), *k.section) == secs.end()) secs.push_back(*k.section);
        for (auto it = secs.rbegin(); it != secs.rend(); ++it)
            if (saved[*it + "\t"].as_string() == "added") next = drop_added_section(next, *it);
    }
    if (next == text) return false;
    return write_as_owner(path, next);
}

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (size_t i = 0; i < names.size(); ++i) out += (i ? ", " : "") + names[i];
    return out;
}

void save_two_screen(bool enabled, const Value& originals) {
    Value st = Value::object();
    st["enabled"] = enabled;
    st["originals"] = originals;
    write_text(TWO_SCREEN_STATE, st.dump(1) + "\n");
}

Value set_two_screen(bool enabled) {
    // The switch: sets (or puts back) every present emulator's keys;
    // refuses while one of them runs.
    std::vector<std::string> changed;
    {
        std::lock_guard<std::mutex> g(two_screen_lock);
        auto files = two_screen_files();
        std::vector<std::string> running;
        for (const auto& n : running_names())
            for (const auto& f : files)
                if (f.emu->name == n) {
                    running.push_back(n);
                    break;
                }
        if (!running.empty()) {
            Value e = Value::object();
            e["ok"] = false;
            e["error"] = "Close " + join_names(running) + " first: it writes its settings back when it quits.";
            return e;
        }
        Value state = read_json(TWO_SCREEN_STATE);
        Value originals = state["originals"].is_object() ? state["originals"] : Value::object();
        for (const auto& f : files)
            if (two_screen_file(f.path, *f.emu, enabled, originals)) changed.push_back(f.emu->name);
        save_two_screen(enabled, originals);
    }
    log("barry: two-screen emulators %s: %s", enabled ? "on" : "off",
        changed.empty() ? "nothing to change" : join_names(changed).c_str());
    Value out = Value::object();
    out["ok"] = true;
    Value list = Value::array();
    for (auto& c : changed) list.push(c);
    out["changed"] = list;
    return out;
}

// While the switch is on, gives the settings to each emulator found without
// them that is not running. The running ones' processes are returned, to be
// looked at again when they end.
std::vector<pid_t> keep_two_screen() {
    std::vector<std::string> changed;
    std::vector<pid_t> waiting;
    {
        std::lock_guard<std::mutex> g(two_screen_lock);
        Value state = read_json(TWO_SCREEN_STATE);
        if (!two_screen_enabled(state)) return {};
        auto running = two_screen_running();
        Value originals = state["originals"].is_object() ? state["originals"] : Value::object();
        for (const auto& f : two_screen_files()) {
            auto r = running.find(f.emu->name);
            if (r != running.end()) {
                // Not applied yet: once it has quit.
                auto text = read_file(f.path);
                if (text && !two_screen_applied(*text, *f.emu))
                    waiting.insert(waiting.end(), r->second.begin(), r->second.end());
                continue;
            }
            if (two_screen_file(f.path, *f.emu, true, originals)) changed.push_back(f.emu->name);
        }
        if (!changed.empty() || !state.has("enabled")) save_two_screen(true, originals);
    }
    if (!changed.empty()) log("barry: two-screen emulators: set %s", join_names(changed).c_str());
    std::sort(waiting.begin(), waiting.end());
    waiting.erase(std::unique(waiting.begin(), waiting.end()), waiting.end());
    return waiting;
}

Value two_screen_status() {
    Value state = read_json(TWO_SCREEN_STATE);
    Value emulators = Value::array();
    for (const auto& f : two_screen_files()) {
        auto text = read_file(f.path);
        if (!text) continue;
        Value e = Value::object();
        e["name"] = f.emu->name;
        e["path"] = f.path;
        e["twoScreens"] = two_screen_applied(*text, *f.emu);
        emulators.push(e);
    }
    Value out = Value::object();
    out["enabled"] = two_screen_enabled(state);
    out["emulators"] = emulators;
    Value running = Value::array();
    for (auto& n : running_names()) running.push(n);
    out["running"] = running;
    out["dsOnRetroArch"] = ds_on_retroarch();
    return out;
}

// ---------------------------------------------------------------- themes --
const char* const SKINS_FALLBACK[] = {"steamos", "ayn"};
constexpr size_t SKIN_FILES_MAX = 400;
constexpr uint64_t SKIN_BYTES_MAX = 64ull * 1024 * 1024;
const std::string USB_MOUNTS = "/run/barry-launcher/usb";

std::string skins_dir() { return home() + "/.local/share/barry_launcher/skins"; }

std::pair<uid_t, gid_t> owner() {
    struct stat st;
    if (stat(home().c_str(), &st) != 0) return {desktop_user().uid, desktop_user().gid};
    return {st.st_uid, st.st_gid};
}

void make_user_dirs(const std::string& path) {
    // path and any missing parents under the user's home, theirs.
    auto [uid, gid] = owner();
    std::vector<std::string> missing;
    for (std::string p = path; !p.empty() && !is_dir(p); p = dirname(p)) missing.push_back(p);
    for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
        if (mkdir(it->c_str(), 0755) != 0 && errno != EEXIST) throw std::runtime_error(*it + ": " + std::strerror(errno));
        if (chown(it->c_str(), uid, gid) != 0) {}
    }
}

double added_time(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return 0;
    return st.st_mtim.tv_sec + st.st_mtim.tv_nsec / 1e9;
}

Value skins_state() {
    // Every theme Barry Launcher has (statsd lists them), the one in use,
    // and each theme's color variants with the one chosen.
    Value listed = http_json("GET", STATSD, "/skins", nullptr, 2);
    Value chosen = http_json("GET", STATSD, "/settings", nullptr, 2)["skinVariants"];
    if (!chosen.is_object()) chosen = Value::object();
    std::string current = listed["current"]["name"].str();
    struct Item {
        Value v;
        bool builtin;
        std::string name, title;
        double added;
    };
    std::vector<Item> items;
    for (const auto& s : listed["skins"].items()) {
        std::string dir = s["dir"].str();
        bool builtin = dir.rfind(skins_dir() + "/", 0) != 0;
        Value variants = Value::array();
        std::vector<std::string> ids;
        for (const auto& v : s["variants"].items())
            if (v.is_object() && v["id"].truthy()) {
                variants.push(v);
                ids.push_back(v["id"].str());
            }
        std::string pick = chosen[s["name"].str()].str();
        bool known = std::find(ids.begin(), ids.end(), pick) != ids.end();
        Value t = Value::object();
        t["name"] = s["name"];
        t["title"] = s["title"].truthy() ? s["title"] : s["name"];
        t["author"] = s.has("author") ? s["author"] : Value("");
        t["builtin"] = builtin;
        t["variants"] = variants;
        t["variant"] = known ? pick : (ids.empty() ? "" : ids[0]);
        double added = builtin ? 0 : added_time(dir);
        t["added"] = added;
        items.push_back({t, builtin, s["name"].str(), lower(t["title"].str()), added});
    }
    // Built-in first (Barry Launcher's own, then by title), then the added
    // ones in the order they came onto the device, the newest last.
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        auto key = [](const Item& t) { return std::make_tuple(!t.builtin, t.name != "ayn", t.added, t.title); };
        return key(a) < key(b);
    });
    Value skins = Value::array();
    for (auto& t : items) skins.push(t.v);
    Value out = Value::object();
    out["ok"] = true;
    out["current"] = current;
    out["skins"] = skins;
    return out;
}

std::string skin_name(const std::string& raw) {
    static const std::regex bad("[^a-z0-9_-]+");
    std::string name = std::regex_replace(lower(raw), bad, "-");
    size_t a = name.find_first_not_of('-');
    if (a == std::string::npos) return "";
    size_t b = name.find_last_not_of('-');
    return name.substr(a, b - a + 1).substr(0, 40);
}

std::string strip_dashes(const std::string& s) {
    size_t a = s.find_first_not_of('-');
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of('-') - a + 1);
}

std::vector<Value> usb_partitions() {
    // Partitions on USB drives: lsblk's PATH, FSTYPE, LABEL, MOUNTPOINTS.
    auto r = run({"lsblk", "-J", "-o", "PATH,TRAN,FSTYPE,LABEL,MOUNTPOINTS,TYPE"}, RunOpts{std::nullopt, {}, 10, true});
    if (r.code != 0) throw std::runtime_error("lsblk failed");
    auto doc = Value::parse(r.out);
    if (!doc) throw std::runtime_error("lsblk: not JSON");
    std::vector<Value> found;
    std::function<void(const Value&, const std::string&)> walk = [&](const Value& devs, const std::string& tran) {
        for (const auto& d : devs.items()) {
            std::string t = d["tran"].truthy() ? d["tran"].str() : tran;
            if (t == "usb" && d["type"].str() == "part" && d["fstype"].truthy()) found.push_back(d);
            walk(d["children"], t);
        }
    };
    walk((*doc)["blockdevices"], "");
    // A whole USB disk with a file system and no partitions.
    if (found.empty())
        for (const auto& d : (*doc)["blockdevices"].items())
            if (d["tran"].str() == "usb" && d["fstype"].truthy() && !d["children"].truthy()) found.push_back(d);
    return found;
}

Value usb_mount() {
    // A folder to pick from on the USB drive: where it is mounted already,
    // or mounted read-only here for the pick (usb_unmount after).
    auto parts = usb_partitions();
    Value out = Value::object();
    if (parts.empty()) {
        out["ok"] = false;
        out["error"] = "No USB drive found. Plug one in, then try again.";
        return out;
    }
    for (const auto& p : parts)
        for (const auto& m : p["mountpoints"].items())
            if (m.truthy()) {
                out["ok"] = true;
                out["path"] = m;
                return out;
            }
    const Value& p = parts[0];
    std::string label = skin_name(p["label"].truthy() ? p["label"].str() : basename(p["path"].str()));
    std::string point = USB_MOUNTS + "/" + (label.empty() ? "usb" : label);
    makedirs(point);
    auto r = run({"mount", "-o", "ro,nosuid,nodev,noexec", p["path"].str(), point});
    if (r.code != 0) {
        out["ok"] = false;
        std::string why = trim(r.err);
        out["error"] = "Could not open the USB drive (" + (why.empty() ? std::to_string(r.code) : why) + ").";
        return out;
    }
    out["ok"] = true;
    out["path"] = point;
    return out;
}

bool is_mount(const std::string& path) {
    struct stat a, b;
    if (stat(path.c_str(), &a) != 0 || stat((path + "/..").c_str(), &b) != 0) return false;
    return a.st_dev != b.st_dev || a.st_ino == b.st_ino;
}

void usb_unmount() {
    // Unmount and remove what usb_mount mounted (never other mounts).
    for (const auto& name : listdir(USB_MOUNTS)) {
        std::string point = USB_MOUNTS + "/" + name;
        if (is_mount(point)) run({"umount", point});
        if (!is_mount(point)) rmdir(point.c_str());
    }
}

// What install_theme, install_language and install_widget add: a folder known by the file at
// its top (marker), unpacked from an archive or picked by that file.
struct PackKind {
    const char* noun;
    const char* marker;
    size_t name_max;
};
const PackKind THEME = {"theme", "skin.json", 40};
const PackKind LANGUAGE = {"language", "keyboard.json", 32};
const PackKind WIDGET = {"widget", "widget.json", 40};

struct PackError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The folder in what was picked, unpacked into work if needed, and its name.
std::pair<std::string, std::string> pack_source(const std::string& picked, const std::string& work, const PackKind& kind) {
    std::string path = realpath(picked);
    if (basename(path) == kind.marker) {
        std::string src = dirname(path);
        return {src, basename(src)};
    }
    static const std::regex ext(R"((\.tar)?\.[^.]+$)");
    std::string stem = std::regex_replace(basename(path), ext, "");
    std::string out = work + "/x";
    if (mkdir(out.c_str(), 0700) != 0) throw PackError(std::strerror(errno));
    std::string why;
    switch (unpack_archive(path, out, kind.noun, SKIN_FILES_MAX, SKIN_BYTES_MAX, why)) {
        case Unpack::Ok: break;
        case Unpack::NotAnArchive:
            throw PackError(std::string("pick a ") + kind.noun + "'s .zip or .tar.gz, or the " + kind.marker + " in a " +
                            kind.noun + "'s folder");
        case Unpack::Refused:
        case Unpack::Failed: throw PackError(why);
    }
    // The marker at the top, or in the one folder at the top.
    if (is_file(out + "/" + kind.marker)) return {out, stem};
    std::vector<std::string> entries;
    for (const auto& e : listdir(out))
        if (e[0] != '.' && e != "__MACOSX") entries.push_back(e);
    if (entries.size() == 1 && is_file(out + "/" + entries[0] + "/" + kind.marker)) return {out + "/" + entries[0], entries[0]};
    throw PackError(std::string("no ") + kind.marker + " in it: not a Barry Launcher " + kind.noun);
}

// The folder's marker file, read, and whether it has code in it (QML or
// JavaScript, which runs as the user).
std::pair<Value, bool> check_pack_dir(const std::string& src, const PackKind& kind) {
    size_t files = 0;
    uint64_t total = 0;
    bool code = false;
    std::function<void(const std::string&)> walk = [&](const std::string& dir) {
        for (const auto& n : listdir(dir)) {
            std::string p = dir + "/" + n;
            if (is_link(p)) throw PackError(std::string("the ") + kind.noun + " has links in it");
            if (is_dir(p)) {
                walk(p);
                continue;
            }
            if (!is_file(p)) throw PackError(std::string("the ") + kind.noun + " has special files in it");
            struct stat st;
            stat(p.c_str(), &st);
            total += static_cast<uint64_t>(st.st_size);
            files += 1;
            auto ends = [&](const char* x) {
                size_t l = std::strlen(x);
                return n.size() >= l && n.compare(n.size() - l, l, x) == 0;
            };
            code = code || ends(".qml") || ends(".js") || ends(".mjs");
        }
    };
    walk(src);
    if (total > SKIN_BYTES_MAX || files > SKIN_FILES_MAX) throw PackError(std::string("too big for a ") + kind.noun);
    auto text = read_file(src + "/" + kind.marker);
    if (!text) throw PackError(std::string("cannot read ") + kind.marker);
    auto meta = Value::parse(*text);
    if (!meta) throw PackError(std::string(kind.marker) + ": not JSON");
    if (!meta->is_object()) throw PackError(std::string(kind.marker) + " is not a " + kind.noun + "'s");
    return {*meta, code};
}

bool copy_tree(const std::string& from, const std::string& to, uid_t uid, gid_t gid) {
    if (mkdir(to.c_str(), 0755) != 0) return false;
    if (chown(to.c_str(), uid, gid) != 0) {}
    chmod(to.c_str(), 0755);
    for (const auto& n : listdir(from)) {
        std::string a = from + "/" + n, b = to + "/" + n;
        if (is_dir(a)) {
            if (!copy_tree(a, b, uid, gid)) return false;
            continue;
        }
        auto data = read_file(a);
        if (!data) return false;
        FILE* f = std::fopen(b.c_str(), "wb");
        if (!f) return false;
        bool ok = std::fwrite(data->data(), 1, data->size(), f) == data->size();
        ok = std::fclose(f) == 0 && ok;
        if (!ok) return false;
        if (chown(b.c_str(), uid, gid) != 0) {}
        chmod(b.c_str(), 0644);
    }
    return true;
}

using Describe = std::function<Value(const Value& meta, const std::string& src)>;

Value install_pack(const std::string& path, bool check_only, const PackKind& kind, const std::string& into,
                   const Describe& describe) {
    // Unpack what was picked, check it (describe gives its title, or
    // throws), and put it in into/<name> as the user, in one move.
    std::string noun = kind.noun;
    std::string work;
    Value out = Value::object();
    try {
        make_user_dirs(into);
        std::string tmpl = into + "/.install-XXXXXX";
        if (!mkdtemp(tmpl.data())) throw PackError(std::strerror(errno));
        work = tmpl;
        auto [src, raw_name] = pack_source(path, work, kind);
        auto [meta, code] = check_pack_dir(src, kind);
        Value info = describe(meta, src);
        std::string name = skin_name(raw_name);
        if (name.empty()) name = skin_name(info["title"].str());
        name = strip_dashes(name.substr(0, kind.name_max));
        if (name.empty()) throw PackError("the " + noun + " has no usable name");
        std::string dest = into + "/" + name;
        Value pack = info;
        pack["name"] = name;
        pack["title"] = info["title"].truthy() ? info["title"] : Value(name);
        pack["code"] = code;
        pack["replaces"] = is_dir(dest);
        if (!check_only) {
            std::string stage = work + "/stage";
            auto [uid, gid] = owner();
            if (!copy_tree(src, stage, uid, gid)) throw PackError("cannot copy it: " + std::string(std::strerror(errno)));
            if (is_dir(dest)) remove_tree(dest);
            if (rename(stage.c_str(), dest.c_str()) != 0) throw PackError(std::strerror(errno));
        }
        out["ok"] = true;
        out[noun] = pack;
    } catch (const std::exception& e) {
        out = Value::object();
        out["ok"] = false;
        out["error"] = "Could not add the " + noun + ": " + e.what();
    }
    if (!work.empty()) remove_tree(work);
    return out;
}

Value install_skin(const std::string& path, bool check_only) {
    return install_pack(path, check_only, THEME, skins_dir(), [](const Value& meta, const std::string&) {
        Value v = Value::object();
        v["title"] = meta["title"].truthy() ? meta["title"].str() : "";
        v["author"] = meta["author"].truthy() ? meta["author"].str() : "";
        return v;
    });
}

Value remove_skin(const std::string& name) {
    // Delete a theme the user added (never a built-in one); if it was in
    // use, Barry Launcher changes to SKINS_FALLBACK.
    Value state = skins_state();
    const Value* theme = nullptr;
    std::set<std::string> names;
    for (const auto& t : state["skins"].items()) {
        if (t["name"].str() == name) theme = &t;
        names.insert(t["name"].str());
    }
    if (!theme || (*theme)["builtin"].truthy())
        return *Value::parse(R"({"ok": false, "error": "Only a theme you added can be removed."})");
    remove_tree(skins_dir() + "/" + name);
    if (state["current"].str() == name) {
        names.erase(name);
        std::string pick = "ayn";
        for (const char* f : SKINS_FALLBACK)
            if (names.count(f)) {
                pick = f;
                break;
            }
        Value body = Value::object();
        body["skin"] = pick;
        http_json("POST", STATSD, "/settings", &body, 2);
    }
    Value out = Value::object();
    out["ok"] = true;
    out.update(skins_state());
    return out;
}

// ------------------------------------------------------- language packs --
// Barry.Input's KeyboardLanguages reads the packs from these (the user's
// winning over a shipped one with the same id) and the one in use from
// LANGUAGE_CHOICE; it watches both, so a change here shows at once.
const std::string LANGUAGES_SYSTEM = "/usr/share/barry_launcher/keyboards";
const std::string LANGUAGE_CHOICE = ".config/barry_launcher/keyboard.json";
// As in keyboardlanguages.cpp: keys in a row, characters a key types, and
// the names Barry gives its own keys.
constexpr size_t LANGUAGE_ROW_MAX = 14;
constexpr size_t LANGUAGE_KEY_MAX = 8;
const std::set<std::string> LANGUAGE_RESERVED = {
    "shift", "BackSpace", "space", "Return", "symbols", "keys", "abc", "hide", "lang", "ctrl",
    "alt", "super", "Escape", "Tab", "Home", "End", "Delete", "Left", "Right", "Up",
    "Down", "Prior", "Next", "copy", "paste", "cut", "selectall", "undo", "redo", "numbers", "deleteword"};
// A grid pack's grid and numbers: 3 rows of 3 keys, as keyboardlanguages.cpp.
constexpr size_t LANGUAGE_GRID = 3;
const char* const LANGUAGE_DIRECTIONS[] = {"top", "topRight", "right", "bottomRight", "bottom", "bottomLeft", "left", "topLeft"};
// Packs PB-OS ships but does not turn on (a second layout, like Thumb-Key's):
// the Languages tab offers to add them.
const std::string LANGUAGES_EXTRA = "/usr/share/barry_launcher/keyboards-extra";

std::string languages_dir() { return home() + "/.local/share/barry_launcher/keyboards"; }

// Python's len() of a str counts code points.
size_t code_points(const std::string& s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xc0) != 0x80) ++n;
    return n;
}

std::string first_code_points(const std::string& s, size_t count) {
    size_t n = 0, i = 0;
    for (; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xc0) != 0x80) {
            if (n == count) break;
            ++n;
        }
    }
    return s.substr(0, i);
}

Value check_language(const Value& meta, const std::string& src) {
    // keyboard.json read as Barry.Input reads it (keyboardlanguages.cpp
    // load()): a pack it would leave out is refused here, with the reason.
    std::string name = trim(meta["name"].truthy() ? meta["name"].str() : "");
    if (name.empty()) throw PackError("keyboard.json needs a name");
    auto rows_ok = [](const Value& rows) {
        if (!rows.is_array() || rows.size() < 3 || rows.size() > 4) return false;
        for (const auto& r : rows.items()) {
            if (!r.is_array() || r.size() < 1 || r.size() > LANGUAGE_ROW_MAX) return false;
            for (const auto& k : r.items()) {
                if (!k.is_string()) return false;
                size_t n = code_points(k.as_string());
                if (n == 0 || n > LANGUAGE_KEY_MAX || LANGUAGE_RESERVED.count(k.as_string())) return false;
            }
        }
        return true;
    };
    auto grid_ok = [](const Value& grid) {
        auto text_ok = [](const Value& t, bool swipe) {
            if (!t.is_string()) return false;
            if (swipe && t.as_string() == "shift") return true;
            size_t n = code_points(t.as_string());
            return n > 0 && n <= LANGUAGE_KEY_MAX && !LANGUAGE_RESERVED.count(t.as_string());
        };
        if (!grid.is_array() || grid.size() != LANGUAGE_GRID) return false;
        for (const auto& r : grid.items()) {
            if (!r.is_array() || r.size() != LANGUAGE_GRID) return false;
            for (const auto& k : r.items()) {
                if (!k.is_object() || !text_ok(k["center"], false)) return false;
                for (const char* d : LANGUAGE_DIRECTIONS)
                    if (k.has(d) && !text_ok(k[d], true)) return false;
            }
        }
        return true;
    };
    const bool grid = meta.has("grid");
    if (grid) {
        if (!grid_ok(meta["grid"]))
            throw PackError("grid must be 3 rows of 3 keys, each a center and its swipes (up to " +
                            std::to_string(LANGUAGE_KEY_MAX) + " characters, or \"shift\")");
        if (meta.has("numbers") && !grid_ok(meta["numbers"])) throw PackError("numbers must be a grid, as grid is");
    }
    const Value& rows = meta["rows"];
    if (!grid && !rows_ok(rows))
        throw PackError("rows must be 3 or 4 lists of 1-" + std::to_string(LANGUAGE_ROW_MAX) +
                        " keys, each the text it types");
    if (!grid && meta.has("shiftRows")) {
        const Value& shift = meta["shiftRows"];
        bool same = rows_ok(shift) && shift.size() == rows.size();
        for (size_t i = 0; same && i < rows.size(); ++i) same = shift.at(i).size() == rows.at(i).size();
        if (!same) throw PackError("shiftRows must have the same rows and keys as rows");
    }
    for (const char* key : {"words", "composer"}) {
        const Value& rel = meta[key];
        if (!rel.truthy()) continue;
        std::string p = realpath(src + "/" + rel.str());
        if (p.rfind(realpath(src) + "/", 0) != 0 || !is_file(p)) throw PackError(rel.str() + " is not a file in the pack's folder");
    }
    std::string label = trim(meta["label"].truthy() ? meta["label"].str() : "");
    Value out = Value::object();
    out["title"] = name;
    out["label"] = first_code_points(label, 4);
    return out;
}

std::string language_choice() {
    auto v = Value::parse(read_file(home() + "/" + LANGUAGE_CHOICE).value_or(""));
    if (!v || !v->is_object()) return "";
    return (*v)["language"].truthy() ? (*v)["language"].str() : "";
}

Value languages_state() {
    // Every language pack Barry Launcher's keyboards can type in, and the
    // one in use (the one chosen; else English; else the first, as Barry).
    static const std::regex id_re("^[A-Za-z0-9_-]{1,32}$");
    std::map<std::string, Value> found;
    for (auto [base, builtin] : {std::make_pair(LANGUAGES_SYSTEM, true), std::make_pair(languages_dir(), false)}) {
        for (const auto& pid : listdir(base)) {
            std::string d = base + "/" + pid;
            if (!std::regex_match(pid, id_re) || !is_dir(d)) continue;
            Value info;
            try {
                auto text = read_file(d + "/keyboard.json");
                if (!text) throw PackError("no keyboard.json");
                auto meta = Value::parse(*text);
                if (!meta) throw PackError("keyboard.json: not JSON");
                info = check_language(meta->is_object() ? *meta : Value::object(), d);
            } catch (const std::exception& e) {
                log("barry: languages: %s (%s) left out: %s", pid.c_str(), base.c_str(), e.what());
                continue;
            }
            Value t = Value::object();
            t["name"] = pid;
            t["title"] = info["title"];
            std::string up = pid.substr(0, 3);
            for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            t["label"] = info["label"].truthy() ? info["label"] : Value(up);
            t["builtin"] = builtin;
            t["added"] = builtin ? 0.0 : added_time(d);
            found[pid] = t;
        }
    }
    std::vector<Value> langs;
    for (auto& [k, v] : found) langs.push_back(v);
    std::stable_sort(langs.begin(), langs.end(), [](const Value& a, const Value& b) {
        auto key = [](const Value& t) {
            return std::make_tuple(!t["builtin"].truthy(), t["name"].str() != "en", t["added"].as_double(),
                                   lower(t["title"].str()));
        };
        return key(a) < key(b);
    });
    std::string chosen = language_choice();
    std::string current = langs.empty() ? "en" : langs[0]["name"].str();
    if (found.count(chosen)) current = chosen;
    else if (found.count("en")) current = "en";
    // The shipped extras not added yet.
    Value extras = Value::array();
    for (const auto& pid : listdir(LANGUAGES_EXTRA)) {
        std::string d = LANGUAGES_EXTRA + "/" + pid;
        if (!std::regex_match(pid, id_re) || !is_dir(d) || found.count(lower(pid))) continue;
        try {
            auto text = read_file(d + "/keyboard.json");
            if (!text) continue;
            auto meta = Value::parse(*text);
            if (!meta || !meta->is_object()) continue;
            Value info = check_language(*meta, d);
            Value e = Value::object();
            e["name"] = pid;
            e["title"] = info["title"];
            e["path"] = d + "/keyboard.json";
            extras.push(e);
        } catch (const std::exception& e) {
            log("barry: languages: extra %s left out: %s", pid.c_str(), e.what());
        }
    }
    Value out = Value::object();
    out["ok"] = true;
    out["current"] = current;
    Value list = Value::array();
    for (auto& l : langs) list.push(l);
    out["languages"] = list;
    out["extras"] = extras;
    return out;
}

Value set_language(const std::string& name) {
    // Written where Barry.Input keeps it: both keyboards change.
    Value state = languages_state();
    bool known = false;
    for (const auto& t : state["languages"].items()) known = known || t["name"].str() == name;
    if (!known) return *Value::parse(R"({"ok": false, "error": "That language isn't on this device."})");
    std::string path = home() + "/" + LANGUAGE_CHOICE;
    make_user_dirs(dirname(path));
    auto [uid, gid] = owner();
    std::string tmp = path + ".plugin-tmp";
    Value doc = Value::object();
    doc["language"] = name;
    std::string text = doc.dump(4) + "\n";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) throw std::runtime_error(std::strerror(errno));
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    if (chown(tmp.c_str(), uid, gid) != 0) {}
    chmod(tmp.c_str(), 0644);
    if (rename(tmp.c_str(), path.c_str()) != 0) throw std::runtime_error(std::strerror(errno));
    state["current"] = name;
    return state;
}

Value install_language(const std::string& path, bool check_only) {
    return install_pack(path, check_only, LANGUAGE, languages_dir(), check_language);
}

Value remove_language(const std::string& name) {
    // Delete a language pack the user added (never a shipped one); if it was
    // in use, the keyboards change to English (or the shipped pack with its
    // id, should there be one).
    Value state = languages_state();
    const Value* lang = nullptr;
    for (const auto& t : state["languages"].items())
        if (t["name"].str() == name) lang = &t;
    if (!lang || (*lang)["builtin"].truthy())
        return *Value::parse(R"({"ok": false, "error": "Only a language you added can be removed."})");
    remove_tree(languages_dir() + "/" + name);
    Value after = languages_state();
    bool still = false;
    for (const auto& t : after["languages"].items()) still = still || t["name"].str() == name;
    if (state["current"].str() == name && !still) return set_language(after["current"].str());
    return after;
}

// -------------------------------------------------------------- widgets --
// The quick settings page's widget packs (QML; see the README in
// /usr/share/barry_launcher/widgets). statsd lists them (the user's
// winning over a shipped one) and keeps the page's layout (quickLayout, a
// pack's id there "w-<name>"); Barry Launcher reads both and follows.
std::string widgets_dir() { return home() + "/.local/share/barry_launcher/widgets"; }

Value check_widget(const Value& meta, const std::string& src) {
    // As statsd's widgets(): its QML file at the top, sizes in range.
    std::string main = meta["main"].truthy() ? meta["main"].str() : "Widget.qml";
    static const std::regex file(R"([A-Za-z0-9_-]+\.qml)");
    if (!std::regex_match(main, file)) throw PackError("widget.json's main must be a .qml file in the widget's top folder");
    if (!is_file(src + "/" + main)) throw PackError("no " + main + " in it (widget.json's main)");
    for (const char* key : {"minSpan", "span"})
        if (meta.has(key) && (!meta[key].is_number() || meta[key].as_int(0) < 1 || meta[key].as_int(0) > 4))
            throw PackError(std::string("widget.json's ") + key + " must be 1 to 4");
    if (meta.has("height") && (!meta["height"].is_number() || meta["height"].as_int(0) < 60 || meta["height"].as_int(0) > 800))
        throw PackError("widget.json's height must be 60 to 800");
    Value v = Value::object();
    v["title"] = meta["title"].truthy() ? meta["title"].str() : "";
    v["author"] = meta["author"].truthy() ? meta["author"].str() : "";
    return v;
}

Value quick_layout() {
    Value layout = http_json("GET", STATSD, "/settings", nullptr, 2)["quickLayout"];
    return layout.is_array() ? layout : Value::array();
}

Value widgets_state() {
    // Every widget, Barry's own (kind "barry") and the packs', whether it
    // is on the quick settings page, and whether it is one the user added
    // (only those can be removed).
    Value listed = http_json("GET", STATSD, "/widgets", nullptr, 2);
    // (Each list held here: a temporary's items() would be gone in the loop.)
    std::set<std::string> on;
    Value layout_now = quick_layout();
    for (const auto& w : layout_now.items()) on.insert(w["id"].str());
    Value list = Value::array();
    for (const auto& w : listed["widgets"].items()) {
        Value t = Value::object();
        t["id"] = w["id"];
        t["kind"] = w["kind"];
        t["name"] = w["name"];
        t["title"] = w["title"].truthy() ? w["title"] : w["name"];
        t["author"] = w["author"];
        t["builtin"] = w["builtin"];
        t["on"] = on.count(w["id"].str()) > 0;
        list.push(t);
    }
    Value out = Value::object();
    out["ok"] = true;
    out["widgets"] = list;
    return out;
}

Value set_widget_shown(const std::string& id, bool shown) {
    // Put a widget (by its id) on the quick settings page (at its end, as
    // wide as it comes) or take it off.
    Value listed = http_json("GET", STATSD, "/widgets", nullptr, 2);
    const Value* pack = nullptr;
    for (const auto& w : listed["widgets"].items())
        if (w["id"].str() == id) pack = &w;
    if (!pack) return *Value::parse(R"({"ok": false, "error": "That widget isn't there."})");
    Value layout = Value::array();
    bool had = false;
    Value old_layout = quick_layout();
    for (const auto& w : old_layout.items()) {
        if (w["id"].str() == id) {
            had = true;
            if (!shown) continue;
        }
        layout.push(w);
    }
    if (shown && !had) {
        Value w = Value::object();
        w["id"] = id;
        w["span"] = (*pack)["span"].as_int(2);
        layout.push(w);
    }
    Value body = Value::object();
    body["quickLayout"] = layout;
    http_json("POST", STATSD, "/settings", &body, 2);
    log("barry: widget %s %s the quick settings page", id.c_str(), shown ? "on" : "off");
    return widgets_state();
}

Value install_widget(const std::string& path, bool check_only) {
    return install_pack(path, check_only, WIDGET, widgets_dir(), check_widget);
}

Value remove_widget(const std::string& name) {
    // Delete a widget the user added (never a shipped one), and take it off
    // the page unless a shipped one of that name takes its place.
    Value state = widgets_state();
    const Value* widget = nullptr;
    for (const auto& w : state["widgets"].items())
        if (w["kind"].str() == "pack" && w["name"].str() == name) widget = &w;
    if (!widget || (*widget)["builtin"].truthy())
        return *Value::parse(R"({"ok": false, "error": "Only a widget you added can be removed."})");
    remove_tree(widgets_dir() + "/" + name);
    Value after = widgets_state();
    for (const auto& w : after["widgets"].items())
        if (w["kind"].str() == "pack" && w["name"].str() == name) return after;
    // Gone from statsd's list too: off the page by its id.
    Value layout = Value::array();
    Value old_layout = quick_layout();
    for (const auto& w : old_layout.items())
        if (w["id"].str() != "w-" + name) layout.push(w);
    Value body = Value::object();
    body["quickLayout"] = layout;
    http_json("POST", STATSD, "/settings", &body, 2);
    return widgets_state();
}

// ------------------------------------------------------- the keyboard --
std::string steam_display_user() {
    // The user running Game Mode's main gamescope (Steam's :0), if any.
    for (const auto& pid : listdir("/proc")) {
        if (!is_digits(pid)) continue;
        auto cmd = read_file("/proc/" + pid + "/cmdline");
        if (!cmd) continue;
        std::vector<std::string> args = split_by(*cmd, std::string(1, '\0'));
        if (args.empty() || basename(args[0]) != "gamescope") continue;
        if (std::find(args.begin(), args.end(), "--steam") == args.end()) continue;
        struct stat st;
        if (stat(("/proc/" + pid).c_str(), &st) != 0) continue;
        struct passwd pw, *res = nullptr;
        char buf[4096];
        if (getpwuid_r(st.st_uid, &pw, buf, sizeof buf, &res) == 0 && res) return pw.pw_name;
    }
    return "";
}

bool bottom_screen_showing() {
    // Steam's gamescope draws a game's window on the bottom screen.
    std::string user = steam_display_user();
    if (user.empty()) return false;
    RunOpts o;
    o.timeout = 3;
    auto r = run({"runuser", "-u", user, "--", "env", "DISPLAY=:0", "xprop", "-root", "GAMESCOPE_BOTTOM_SCREEN_SHOWING"}, o);
    auto out = trim(r.out);
    return out.size() >= 3 && out.compare(out.size() - 3, 3, "= 1") == 0;
}

// ---------------------------------------------------------------- section --
class Barry : public Section {
public:
    std::string name() const override { return "barry"; }
    bool here() const override {
        return dt_compatible().find("ayn,thor") != std::string::npos || duo();
    }
    static bool duo() { return dt_compatible().find("retroidpocket,rpduo") != std::string::npos; }

    void start(Host& host) override {
        host_ = &host;
        log("barry: Barry Launcher section ready");
        rewatch();
        keep_soon(0);
    }

    Value call(const std::string& m, const Value& a) override {
        try {
            return dispatch(m, a);
        } catch (const NetError& e) {
            return not_answering(e);
        }
    }

private:
    Value dispatch(const std::string& m, const Value& a) {
        if (m == "get_state") {
            Value out = Value::object();
            out["supported"] = exists(TOP + "/brightness") && exists(BOTTOM + "/brightness");
            out["on"] = is_on();
            out["brightness"] = brightness();
            // The button that opens Barry ("" none), and whether the top
            // screen has a slider here.
            out["button"] = duo() ? "" : "AYN";
            out["topSlider"] = true;
            // Steam's slider sets both screens, or (Duo) the top one alone.
            out["steamBoth"] = !duo();
            Value req = Value::object();
            req["op"] = "get";
            out["rgbDimmer"] = rgb_dimmer(controlsd(req));
            return out;
        }
        if (m == "set_bottom_screen") {
            bool on = arg(a, 0, true).truthy();
            write_text(STATE, on ? "on" : "off");
            log("barry: bottom screen %s", on ? "on" : "off");
            return is_on();
        }
        if (m == "set_brightness") {
            // One screen's brightness slider (0-100), as on Android.
            std::string screen = arg(a, 0, "top").str();
            Value d = screens();
            if ((screen == "top" || screen == "bottom") && d.has("top") && d.has("bottom")) {
                d[screen] = std::max(0LL, std::min(100LL, arg(a, 1, 100).as_int(100))) / 100.0;
                write_text(SCREENS, d.dump() + "\n");
            }
            return brightness();
        }
        if (m == "set_rgb_dimmer") {
            long long v = std::max(RGB_DIM_MIN, std::min(100LL, arg(a, 0, 100).as_int(100)));
            Value req = Value::object();
            req["op"] = "set";
            req["lighting"]["dimmer"] = v;
            return rgb_dimmer(controlsd(req));
        }
        if (m == "get_two_screen") return two_screen_status();
        if (m == "set_two_screen") return set_two_screen(arg(a, 0, true).truthy());
        if (m == "keep_two_screen") {
            // What the watchers do after a config change (testing, pbos-qamd --call).
            Value out = Value::object();
            Value list = Value::array();
            for (pid_t p : keep_two_screen()) list.push(static_cast<long long>(p));
            out["waiting"] = list;
            return out;
        }
        if (m == "barry_keyboard_available") {
            // The bottom screen is on and Barry Launcher can open its keyboard.
            if (!is_on()) return false;
            auto r = http("GET", SHELLD, "/apps", nullptr, 0.5);
            auto apps = Value::parse(r.body);
            if (r.status != 200 || !apps) return false;
            // The Trackpad app has the keyboard (and "keyboard" opens it so).
            if (apps->is_object()) return apps->has("trackpad") || apps->has("keyboard");
            for (const auto& x : apps->items())
                if (x.str() == "trackpad" || x.str() == "keyboard") return true;
            return false;
        }
        if (m == "get_barry_apps") return ok_with(http_json("GET", SHELLD, "/layout", nullptr, 1));
        if (m == "set_barry_apps") {
            Value body = Value::object();
            body["order"] = a.at(0).is_array() ? a.at(0) : Value::array();
            body["hidden"] = a.at(1).is_array() ? a.at(1) : Value::array();
            Value r = http_json("POST", SHELLD, "/layout", &body, 2);
            Value out = Value::object();
            out["ok"] = r.has("apps");
            out.update(r);
            return out;
        }
        if (m == "barry_apps_folder") {
            std::string downloads = home() + "/Downloads";
            return is_dir(downloads) ? downloads : home();
        }
        if (m == "check_barry_app") return shelld_or(m, "/apps/check", "path", arg(a, 0, "").str(), 30);
        if (m == "install_barry_app") return shelld_or(m, "/apps/install", "path", arg(a, 0, "").str(), 60);
        if (m == "remove_barry_app") {
            Value body = Value::object();
            body["app"] = arg(a, 0, "").str();
            return shelld_answer("/apps/remove", body, 10);
        }
        if (m == "get_themes") return skins_state();
        // The bottom screen's game art (heroArt) and its sleep timer
        // (bottomSleepSeconds): Barry Launcher's settings (statsd).
        if (m == "get_bottom_settings") {
            Value st = http_json("GET", STATSD, "/settings", nullptr, 2);
            Value out = Value::object();
            out["heroArt"] = st["heroArt"].is_bool() ? st["heroArt"] : Value(true);
            out["bottomSleepSeconds"] = st["bottomSleepSeconds"].as_int(0);
            return out;
        }
        if (m == "set_bottom_settings") {
            const Value& c = a.at(0);
            Value body = Value::object();
            if (c["heroArt"].is_bool()) body["heroArt"] = c["heroArt"];
            if (c["bottomSleepSeconds"].is_number())
                body["bottomSleepSeconds"] = std::max(0LL, std::min(86400LL, c["bottomSleepSeconds"].as_int(0)));
            if (body.size()) {
                http_json("POST", STATSD, "/settings", &body, 2);
                log("barry: bottom screen settings %s", body.dump().c_str());
            }
            return call("get_bottom_settings", Value::array());
        }
        if (m == "set_theme") {
            Value body = Value::object();
            body["skin"] = arg(a, 0, "").str();
            http_json("POST", STATSD, "/settings", &body, 2);
            return skins_state();
        }
        if (m == "set_theme_color") {
            Value body = Value::object();
            body["skinVariants"][arg(a, 0, "").str()] = arg(a, 1, "").str();
            try {
                http_json("POST", STATSD, "/settings", &body, 2);
            } catch (const NetError& e) {
                if (!e.code) throw;
                Value out = Value::object();
                out["ok"] = false;
                out["error"] = "That color isn't in the theme (" + std::to_string(e.code) + ").";
                return out;
            }
            return skins_state();
        }
        if (m == "themes_folder") {
            // Where the file picker starts, for a theme or a language pack: a
            // USB drive when one is plugged in, else Downloads.
            try {
                Value usb = usb_mount();
                if (usb["ok"].truthy()) return usb;
                if (usb["error"].str().rfind("No USB drive", 0) != 0) log("barry: themes: %s", usb["error"].str().c_str());
            } catch (const std::exception& e) {
                log("barry: themes: cannot look for a USB drive: %s", e.what());
            }
            std::string downloads = home() + "/Downloads";
            Value out = Value::object();
            out["ok"] = true;
            out["path"] = is_dir(downloads) ? downloads : home();
            return out;
        }
        if (m == "themes_done") {
            usb_unmount();
            return Value();
        }
        if (m == "get_widgets") return widgets_state();
        if (m == "set_widget_shown") return set_widget_shown(arg(a, 0, "").str(), arg(a, 1, true).truthy());
        if (m == "check_widget") return install_widget(arg(a, 0, "").str(), true);
        if (m == "install_widget") return install_widget(arg(a, 0, "").str(), false);
        if (m == "remove_widget") {
            try {
                return remove_widget(arg(a, 0, "").str());
            } catch (const NetError& e) {
                return error("Could not remove the widget (" + e.msg + ").");
            } catch (const std::exception& e) {
                return error("Could not remove the widget (" + std::string(e.what()) + ").");
            }
        }
        if (m == "check_theme") return install_skin(arg(a, 0, "").str(), true);
        if (m == "install_theme") return install_skin(arg(a, 0, "").str(), false);
        if (m == "remove_theme") {
            try {
                return remove_skin(arg(a, 0, "").str());
            } catch (const NetError& e) {
                return error("Could not remove the theme (" + e.msg + ").");
            } catch (const std::exception& e) {
                return error("Could not remove the theme (" + std::string(e.what()) + ").");
            }
        }
        if (m == "get_languages") return languages_state();
        if (m == "set_language") {
            try {
                return set_language(arg(a, 0, "").str());
            } catch (const std::exception& e) {
                return error("Could not change the language (" + std::string(e.what()) + ").");
            }
        }
        if (m == "check_language") return install_language(arg(a, 0, "").str(), true);
        if (m == "install_language") return install_language(arg(a, 0, "").str(), false);
        if (m == "remove_language") {
            try {
                return remove_language(arg(a, 0, "").str());
            } catch (const std::exception& e) {
                return error("Could not remove the language (" + std::string(e.what()) + ").");
            }
        }
        if (m == "get_game_links") return ok_with(http_json("GET", SHELLD, "/game-links", nullptr, 1));
        if (m == "set_game_links") {
            Value body = Value::object();
            body["links"] = a.at(0).is_array() ? a.at(0) : Value::array();
            Value r = shelld_answer("/game-links", body);
            Value out = Value::object();
            out["ok"] = r.has("links");
            out.update(r);
            return out;
        }
        if (m == "game_event") {
            // A game started or stopped: Barry Launcher opens or closes its app.
            Value body = Value::object();
            body["game"] = arg(a, 0, "").str();
            body["running"] = arg(a, 1, false).truthy();
            try {
                return shelld_answer("/game-event", body, 5);
            } catch (const NetError& e) {
                return error(e.msg);
            }
        }
        if (m == "open_barry_keyboard") return open_keyboard();
        // The bottom screen's hero art (heroart.cpp).
        if (m == "steam_game") return hero_set_game(a.at(0));
        if (m == "get_sgdb") return hero_sgdb_state();
        if (m == "set_sgdb_key") return hero_set_sgdb_key(arg(a, 0, "").str());
        throw CallError("Barry Launcher has no method " + m);
    }

    static Value error(const std::string& msg) {
        Value v = Value::object();
        v["ok"] = false;
        v["error"] = msg;
        return v;
    }

    static Value ok_with(const Value& r) {
        Value out = Value::object();
        out["ok"] = true;
        out.update(r);
        return out;
    }

    static Value shelld_or(const std::string&, const char* path, const char* key, const std::string& value, double t) {
        Value body = Value::object();
        body[key] = value;
        try {
            return shelld_answer(path, body, t);
        } catch (const NetError& e) {
            return not_answering(e, "; is the bottom screen on?");
        }
    }

    Value open_keyboard() {
        // Opens Barry Launcher's keyboard; false when it cannot be seen, for
        // Steam's keyboard then.
        try {
            Value body = Value::object();
            body["app"] = "keyboard";
            if (!http_json("POST", SHELLD, "/launch", &body, 2)["ok"].truthy()) return false;
        } catch (const NetError& e) {
            log("barry: cannot open Barry Launcher's keyboard: %s", e.msg.c_str());
            return false;
        }
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(YIELD_WAIT_S);
        while (bottom_screen_showing()) {
            if (std::chrono::steady_clock::now() >= deadline) {
                log("barry: the bottom screen stayed with the game; Steam's keyboard instead");
                try {
                    Value body = Value::object();
                    body["app"] = "keyboard";
                    http_json("POST", SHELLD, "/close", &body, 2);
                } catch (const NetError& e) {
                    log("barry: cannot close Barry Launcher's keyboard: %s", e.msg.c_str());
                }
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return true;
    }

    // --------------------------------------- two-screen emulators, kept --
    // The folders where emulators' config files are (or would appear):
    // watched so a config file written, or an emulator installed, is heard.
    std::vector<std::string> watch_dirs() const {
        std::vector<std::string> dirs = {home() + "/.config", home() + "/.var/app"};
        for (const auto& app : glob(home() + "/.var/app/*")) {
            dirs.push_back(app);
            dirs.push_back(app + "/config");
        }
        for (const auto& e : two_screen())
            for (const auto& pattern : e.configs) {
                std::string dir = dirname(pattern);
                for (const auto& d : glob(join(home(), dir))) dirs.push_back(d);
            }
        std::vector<std::string> out;
        for (auto& d : dirs)
            if (is_dir(d) && std::find(out.begin(), out.end(), d) == out.end()) out.push_back(d);
        return out;
    }

    void rewatch() {
        // Loop thread.
        auto dirs = watch_dirs();
        for (auto it = watches_.begin(); it != watches_.end();) {
            if (std::find(dirs.begin(), dirs.end(), it->first) == dirs.end()) {
                host_->loop.unwatch(it->second);
                it = watches_.erase(it);
            } else {
                ++it;
            }
        }
        for (const auto& d : dirs) {
            if (watches_.count(d)) continue;
            int id = host_->loop.watch(d, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE_SELF,
                                       [this, d](uint32_t mask, const std::string& n) {
                                           if (mask & (IN_IGNORED | IN_DELETE_SELF)) {
                                               watches_.erase(d);
                                               return;
                                           }
                                           // Its own temporary file, as it writes a config.
                                           if (n.size() > 16 && n.compare(n.size() - 16, 16, ".dual-screen.tmp") == 0) return;
                                           keep_soon(TWO_SCREEN_SETTLE_S);
                                       });
            if (id) watches_[d] = id;
        }
    }

    void keep_soon(double delay) {
        // Loop thread: one look after the last of a burst of events.
        if (keep_timer_) host_->loop.cancel(keep_timer_);
        keep_timer_ = host_->loop.after(delay, [this] {
            keep_timer_ = 0;
            rewatch();
            Loop::work([this] {
                auto waiting = keep_two_screen();
                host_->loop.post([this, waiting] {
                    for (pid_t pid : waiting) {
                        if (exit_watched_.count(pid)) continue;
                        if (host_->loop.on_exit(pid, [this, pid] {
                                exit_watched_.erase(pid);
                                keep_soon(TWO_SCREEN_SETTLE_S);
                            }))
                            exit_watched_.insert(pid);
                    }
                });
            });
        });
    }

    Host* host_ = nullptr;
    std::map<std::string, int> watches_;
    std::set<pid_t> exit_watched_;
    uint64_t keep_timer_ = 0;
};

}  // namespace

std::unique_ptr<Section> make_barry_section() { return std::make_unique<Barry>(); }

}  // namespace pq

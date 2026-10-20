// pbosd (was /usr/lib/konkr/pbosd): the KONKR Pocket FIT and AYANEO Pocket
// S2 platform daemon. Fixes the platform-side gaps that make Linux slower
// than Android on these handhelds and wires up the extra buttons:
//
//   fan      ROCKNIX pins the pwm-fan at 70/255 forever (0500-set-boot-fanspeed)
//            and the DT trips are not bound to it, so the SoC heat-soaks and
//            LMh throttles CPU/GPU. pbosd runs a real temperature curve.
//   profiles lowpower / balanced: fan curve, GPU and big-core clock ceilings,
//            how hard game threads are boosted. Toggled by the Performance
//            button.
//   games    threads of Steam-launched games are kept off the Cortex-A520
//            little cores and get a uclamp.min boost (pbosd_games.cpp).
//   gpu      devfreq polling 50 ms → 16 ms so the GPU clock follows frame load.
//   buttons  Custom Function (above ABXY) and K (the larger button below the
//            right stick), on the MCU link, are either trackpad clicks that
//            Steam can remap (buttons_mode "steam", default) or F14/F13 that
//            run configurable actions here ("system"). pbosd installs the
//            matching InputPlumber capability map (see
//            /usr/share/pbosd/inputplumber/).
//   power    the power key, as on the Steam Deck (see power_press).
//   touch    Android-style input boost: the CPU floor up while the screen is
//            touched.
//   leds     power LED: profile color flash, then charge state; the stick
//            lights from the state file.
//
// Config: /etc/pbosd.conf   State: /var/lib/pbosd/state.json
// Control: pbosctl and pbos-qamd's PB-OS tab (they edit the state, then send
// SIGHUP); konkr-standby, konkr-sleep and sm8550-charge-rest send SIGHUP too.
//
// The Python version woke every 0.1-0.5 s: it read the keys with a select()
// timeout, looked for key and touch devices every 5 s, ran the fan, the LEDs
// and a /proc scan for the game boost every second, and checked Steam's
// webhelper launcher and the pad mode every 60 s. Here:
//   - key and touch devices come and go with the kernel's input uevents
//     (konkr-sleep stops InputPlumber, so its keyboard goes and comes back);
//   - the keys are read when readable, the long press is a timer;
//   - konkr-standby's, the charge rest's and the charge hold's flags in /run
//     are watched with inotify, and their processes' exits with pidfds;
//   - the power LED follows power_supply uevents, with a timer only while it
//     pulses (low battery) or flashes (a profile switch);
//   - game boost follows the kernel's process events (pbosd_games.cpp);
//   - the webhelper launcher is watched with inotify, the pad mode on USB and
//     serial uevents;
//   - the fan keeps its 1 s tick: thermal zones cannot notify, and the curve's
//     smoothing and ramp are per tick.
// The behavior, the state file and every command run are the Python version's.

#include <fcntl.h>
#include <linux/input.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "json.h"
#include "module.h"
#include "pbosd.h"
#include "util.h"

namespace pp {

namespace pbosd {

std::optional<long long> py_int(std::string_view text) {
    std::string s = pq::trim(text);
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return std::nullopt;
    long long v = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c == '_' && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '9' && s[i - 1] != '_') continue;
        if (c < '0' || c > '9') return std::nullopt;
        if (v < 1000000000000000LL) v = v * 10 + (c - '0');
    }
    return neg ? -v : v;
}

std::set<int> big_cpus() {
    std::map<int, long long> caps;
    for (const auto& c : pq::glob("/sys/devices/system/cpu/cpu[0-9]*")) {
        auto n = py_int(pq::basename(c).substr(3));
        if (!n) continue;
        auto cap = py_int(pq::rd(c + "/cpu_capacity", "0"));
        long long v = 0;
        if (cap && *cap) {
            v = *cap;
        } else if (cap) {
            auto mx = py_int(pq::rd(c + "/cpufreq/cpuinfo_max_freq", "0"));
            v = mx ? *mx : 0;
        }
        caps[static_cast<int>(*n)] = v;
    }
    if (caps.empty()) return {};
    long long low = caps.begin()->second;
    for (const auto& [n, v] : caps) low = std::min(low, v);
    std::set<int> big, all;
    for (const auto& [n, v] : caps) {
        all.insert(n);
        if (v > low) big.insert(n);
    }
    return big.empty() ? all : big;
}

}  // namespace pbosd

namespace {

using pj::Value;
using pbosd::py_int;

const char* const CONF = "/etc/pbosd.conf";
const std::string STATE_DIR = "/var/lib/pbosd";
const std::string STATE = STATE_DIR + "/state.json";
// Before the rename (2026-10) this was konkrd with the KONKR Control plugin.
const std::string LEGACY_STATE_DIR = "/var/lib/konkrd";
const std::string PLUGINS_DIR = "/home/steamos/homebrew/plugins";
const char* const PROFILES[] = {"lowpower", "balanced"};
// Profiles before 2026-10: Silent and Turbo. The Tomb Raider A/B on battery
// showed Turbo (big cores on `performance`, GPU floor raised) and the Turbo
// fan curve add heat and power for no frames: the GPU is heat-capped either
// way. Low Power (GPU <= 680 MHz, big cores <= 2.04 GHz) draws 41-45 % less
// than Balanced for ~25 % fewer frames when GPU-bound.
const std::pair<const char*, const char*> LEGACY_PROFILES[] = {{"silent", "lowpower"}, {"turbo", "balanced"}};
// Stick lighting out of the box: purple breathing, as on the Retroid Pocket 6
// and Nova (PB-OS Utils' DEFAULT_EFFECT / DEFAULT_COLOR).
const char* const RGB_DEFAULT_MODE = "breath";
const char* const RGB_DEFAULT_COLOR = "8c00ff";

// temp °C → pwm 0..255, linearly interpolated
using Curve = std::vector<std::pair<int, int>>;
const Curve& fan_curve(const std::string& profile) {
    static const Curve lowpower = {{55, 0}, {65, 45}, {75, 85}, {85, 150}, {93, 255}};
    static const Curve balanced = {{50, 35}, {60, 60}, {70, 100}, {80, 160}, {90, 255}};
    return profile == "lowpower" ? lowpower : balanced;
}
// clock ceilings (0 = the hardware maximum); the GPU floor is always the
// lowest OPP, big = the CPU clusters other than the Cortex-A520s. Balanced
// stops the GPU at 1000 MHz: ROCKNIX's 1050 MHz step (TURBO_L2) gave about
// 2 % more vkmark than 1000 MHz for 2.3 W and 11 C more on the Pocket FIT
// (2026-10-10); 1000 MHz scales with the clock.
long long gpu_max_mhz(const std::string& p) { return p == "lowpower" ? 680 : 1000; }
long long big_cpu_max_khz(const std::string& p) { return p == "lowpower" ? 2035200 : 0; }

struct Rgb {
    int r, g, b;
};
Rgb profile_rgb(const std::string& p) { return p == "lowpower" ? Rgb{0, 90, 255} : Rgb{0, 255, 60}; }
const char* profile_label(const std::string& p) { return p == "lowpower" ? "Low Power" : "Full Power"; }

// Custom Function / K button mode → capability map variant for the MCU buttons.
const char* const BUTTON_MODES[] = {"steam", "system"};
std::string sysbtn_variant(const std::string& mode) { return "/usr/share/pbosd/inputplumber/sysbtn-" + mode + ".yaml"; }
const std::string SYSBTN_ACTIVE = "/etc/inputplumber/capability_maps.d/konkr_pocketfit_sysbtn.yaml";

constexpr double FAN_TICK_S = 1.0;
constexpr int FIXED_OVERRIDE_ON_C = 90;
constexpr int FIXED_OVERRIDE_OFF_C = 82;

const std::string WEBHELPER_DIR = "/home/steamos/.local/share/Steam/steamrtarm64";
const std::string WEBHELPER_NAME = "steamwebhelper.sh";
const std::string WEBHELPER_SH = WEBHELPER_DIR + "/" + WEBHELPER_NAME;
const std::string WEBHELPER_EXEC =
    R"(exec taskset 0x7c $(pwd)/steamwebhelper "$@" &> ~/.steam/steam/logs/steamwebhelper.log)";
const std::string PBOS_CEF =
    "# SteamOS-ARM-SM8650: draw the Steam UI through ANGLE's Vulkan backend on\n"
    "# Turnip directly. The default (ANGLE -> GL -> zink -> Vulkan) converts every\n"
    "# UI draw call twice on the CPU and made menus/game pages sluggish.\n"
    "PBOS_CEF_FLAGS=\"--use-gl=angle --use-angle=vulkan "
    "--enable-features=Vulkan,DefaultANGLEVulkan,VulkanFromANGLE\"\n"
    R"(exec taskset 0x7c $(pwd)/steamwebhelper "$@" $PBOS_CEF_FLAGS &> ~/.steam/steam/logs/steamwebhelper.log)";

const std::pair<const char*, const char*> RGB_PRESETS[] = {
    {RGB_DEFAULT_MODE, RGB_DEFAULT_COLOR}, {"static", "ff3c00"}, {"static", "00b4ff"}, {"static", "a000ff"},
    {"static", "ffffff"}, {"breath", "ff0040"}, {"off", "000000"}};
constexpr double FLASH_S = 2.0;
constexpr double PULSE_S = 1.0;

const char* const RUN_DIR = "/run";
const char* const STANDBY_NAME = "konkr-standby";
const std::string STANDBY_FLAG = "/run/konkr-standby";
// sm8550-charge-rest (sleep on the charger, Pocket FIT too) holds its PID
// here while it rests or watches deep sleep: it owns the power key then.
const char* const CHARGE_REST_NAME = "sm8550-charge-rest";
const std::string CHARGE_REST_FLAG = "/run/sm8550-charge-rest";
// sm8550-charge-control holds its PID here while it keeps the battery below
// full on purpose (charge limit): the power LED shows "charged" then.
const char* const CHARGE_HELD_NAME = "sm8550-charge-held";
const std::string CHARGE_HELD_FLAG = "/run/sm8550-charge-held";

constexpr double LONG_PRESS_S = 1.0;
// Presses right after a wake are ignored, like powerbuttond on the Deck
// ("still actively resuming"): the wake press was followed by a second one
// that put the device straight back to sleep (journal, 2026-09-26).
constexpr double WAKE_GRACE_S = 2.5;
const char* const USER = "steamos";

// Android-style input boost. At idle schedutil parks the cores near their
// floor, and the first frames after a touch (a tab switch, a scroll) stalled
// while the clocks ramped: on the Steam home screen 5 frames over 34 ms per
// 5 s of scrolling and a 234 ms freeze on a tab switch, vs 0-1 and 117-167 ms
// with the floor raised (measured 2026-09-26; the GPU floor made no
// difference). pbosd owns scaling_min_freq nowhere else.
constexpr long long TOUCH_BOOST_BIG_HZ = 1800000, TOUCH_BOOST_LITTLE_HZ = 1500000;
constexpr double TOUCH_BOOST_HOLD_S = 1.2;
constexpr double TOUCH_SAMPLE_S = 0.3;  // look at the touchscreen at most ~3x a second

// The pad boots in the fake Xbox 360 mode (045e:028e), where the two back
// buttons send nothing. The AYANEO default mode (4001:0428) reports them and
// InputPlumber maps them to the Deck's back paddles. konkr_sysbtn switches
// the MCU; the pad re-enumerates about a second later.
const char* const XBOX_VID = "045e";
const char* const XBOX_PID = "028e";
// USB and serial uevents come in bursts: one look this long after the last.
constexpr double GAMEPAD_SETTLE_S = 1.0;
// A Steam update writes its files in a burst: one look this long after.
constexpr double WEBHELPER_SETTLE_S = 1.0;

double monotonic() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

template <class T, size_t N>
bool one_of(const std::string& s, const T (&list)[N]) {
    for (const auto& x : list)
        if (s == x) return true;
    return false;
}

// Python's str.split(): runs of whitespace.
std::vector<std::string> split_ws(const std::string& s) {
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

// Python's open(path, "w").write(str(value)) in a try (sysfs ignores the
// truncation).
bool wr(const std::string& path, std::string_view value) {
    int fd = open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n;
    do n = write(fd, value.data(), value.size());
    while (n < 0 && errno == EINTR);
    bool ok = n == static_cast<ssize_t>(value.size());
    if (close(fd) != 0) ok = false;
    return ok;
}
bool wr(const std::string& path, long long value) { return wr(path, std::to_string(value)); }

// int(v) of a JSON value as Python does it: numbers truncated, booleans 0/1,
// strings parsed; nothing for anything else (Python raised there).
std::optional<long long> int_of(const Value& v) {
    if (v.is_bool()) return v.as_bool() ? 1 : 0;
    if (v.is_number()) {
        double d = v.as_double();
        if (!std::isfinite(d)) return std::nullopt;
        return static_cast<long long>(std::trunc(d));
    }
    if (v.is_string()) return py_int(v.as_string());
    return std::nullopt;
}

// dict.get(key, def) of an object (def for anything that is not one).
Value get(const Value& obj, std::string_view key, Value def) {
    if (!obj.is_object()) return def;
    const Value* v = obj.find(key);
    return v ? *v : def;
}

bool is_str(const Value& v, std::string_view s) { return v.is_string() && v.as_string() == s; }

// The process is alive: it exists and is not a zombie (its exit, heard
// through a pidfd, already counts as gone).
bool alive(const std::string& pid) {
    auto stat = pq::read_file("/proc/" + pid + "/stat");
    if (!stat) return false;
    size_t paren = stat->rfind(')');
    if (paren == std::string::npos || paren + 2 >= stat->size()) return false;
    char state = (*stat)[paren + 2];
    return state != 'Z' && state != 'X';
}

// The PID in a /run flag while that process lives; empty otherwise.
std::string flag_pid(const std::string& flag) {
    std::string pid = pq::rd(flag);
    if (!pq::is_digits(pid) || pid.size() > 9 || !alive(pid)) return "";
    return pid;
}

// ----------------------------------------------------------------- conf ---
// configparser as pbosd used it: [section]s, key = value or key: value,
// lowercased keys, full-line # and ; comments, indented continuation lines,
// the defaults below under the file.
class Conf {
public:
    std::optional<std::string> get(const std::string& sec, const std::string& opt) const {
        auto s = s_.find(sec);
        if (s == s_.end()) return std::nullopt;
        auto o = s->second.find(pq::lower(opt));
        if (o == s->second.end()) return std::nullopt;
        return o->second;
    }
    std::string get(const std::string& sec, const std::string& opt, const std::string& fallback) const {
        return get(sec, opt).value_or(fallback);
    }
    // getboolean(); a value it cannot read (configparser raised) keeps def.
    bool getbool(const std::string& sec, const std::string& opt, bool def) const {
        std::string v = pq::lower(get(sec, opt, ""));
        if (v == "1" || v == "yes" || v == "true" || v == "on") return true;
        if (v == "0" || v == "no" || v == "false" || v == "off") return false;
        pq::log("pbosd: %s: [%s] %s is not a boolean; using %s", CONF, sec.c_str(), opt.c_str(), def ? "yes" : "no");
        return def;
    }
    long long getint(const std::string& sec, const std::string& opt, long long def) const {
        if (auto n = py_int(get(sec, opt, ""))) return *n;
        pq::log("pbosd: %s: [%s] %s is not a number; using %lld", CONF, sec.c_str(), opt.c_str(), def);
        return def;
    }

    void set(const std::string& sec, const std::string& opt, const std::string& value) {
        s_[sec][pq::lower(opt)] = value;
    }

    void read(const std::string& path) {
        auto text = pq::read_file(path);
        if (!text) return;
        std::string sec, opt;
        size_t opt_indent = 0;
        int lineno = 0;
        for (const auto& line : pq::split_lines(*text)) {
            ++lineno;
            std::string v = pq::trim(line);
            if (v.empty() || v[0] == '#' || v[0] == ';') continue;
            size_t indent = line.find_first_not_of(" \t");
            if (!sec.empty() && !opt.empty() && indent > opt_indent) {
                s_[sec][opt] += "\n" + v;
                continue;
            }
            if (v[0] == '[' && v.back() == ']' && v.size() > 2) {
                sec = v.substr(1, v.size() - 2);
                s_[sec];
                opt.clear();
                continue;
            }
            size_t d = v.find_first_of("=:");
            if (sec.empty() || d == std::string::npos || d == 0) {
                pq::log("pbosd: %s line %d: not understood, skipped", path.c_str(), lineno);
                continue;
            }
            opt = pq::lower(pq::trim(v.substr(0, d)));
            opt_indent = indent;
            s_[sec][opt] = pq::trim(v.substr(d + 1));
        }
    }

private:
    std::map<std::string, std::map<std::string, std::string>> s_;
};

Conf load_conf() {
    Conf c;
    c.set("buttons", "F13", "rgb-next");
    c.set("buttons", "F14", "profile-next");
    c.set("buttons", "F15", "none");
    c.set("fan", "enabled", "yes");
    c.set("fan", "min_pwm", "0");
    c.set("fan", "failsafe_pwm", "180");
    c.set("games", "boost", "yes");
    c.set("games", "avoid_little_cores", "yes");
    c.set("controller", "mode", "default");
    c.read(CONF);
    return c;
}

// ---------------------------------------------------------------- state ---

// Button actions saved before the Silent/Turbo/fan-boost removal.
std::string legacy_action(const std::string& act) {
    if (act == "fan-boost") return "none";
    if (act.rfind("profile-", 0) == 0)
        for (const auto& [old, now] : LEGACY_PROFILES)
            if (act.substr(8) == old) return std::string("profile-") + now;
    return act;
}

Value load_state() {
    Value st = pq::read_json(STATE);  // {} when missing, unreadable or not an object
    if (!st.has("profile")) st["profile"] = "balanced";
    if (!st.has("rgb")) {
        Value rgb = Value::object();
        rgb["mode"] = RGB_DEFAULT_MODE;
        rgb["color"] = RGB_DEFAULT_COLOR;
        rgb["brightness"] = 160;
        st["rgb"] = rgb;
    }
    if (!st.has("fan")) {
        Value fan = Value::object();
        fan["mode"] = "auto";
        fan["fixed"] = 50;
        st["fan"] = fan;
    }
    if (st["fan"].is_object()) st["fan"].erase("boost");  // fan boost was removed
    if (!st.has("power_led")) st["power_led"] = true;
    if (!st.has("buttons")) {
        Value b = Value::object();
        b["F13"] = "rgb-next";
        b["F14"] = "profile-next";
        st["buttons"] = b;
    }
    const Value& bm = std::as_const(st)["buttons_mode"];
    if (!(bm.is_string() && one_of(bm.as_string(), BUTTON_MODES))) st["buttons_mode"] = "steam";
    if (st["buttons"].is_object())
        for (auto& [key, act] : st["buttons"].members())
            if (act.is_string()) act = legacy_action(act.as_string());
    const Value& p = std::as_const(st)["profile"];
    std::string prof = p.is_string() ? p.as_string() : "";
    for (const auto& [old, now] : LEGACY_PROFILES)
        if (prof == old) prof = now;
    st["profile"] = one_of(prof, PROFILES) ? prof : "balanced";
    return st;
}

void save_state(const Value& st) {
    if (!pq::write_json(STATE, st, 2)) pq::log("pbosd: cannot write %s: %s", STATE.c_str(), std::strerror(errno));
}

// Keep the settings of a konkrd install, and drop the old panels (KONKR
// Control, then PB-OS Control) once PB-OS Utils, which has their tabs now,
// is there: two panels would both drive the daemon. The updater removes
// them too; this covers anything that brought PB-OS Utils another way.
void migrate_from_konkrd() {
    auto fail = [](const std::string& what) {
        pq::log("pbosd: konkrd migration: %s: %s", what.c_str(), std::strerror(errno));
    };
    if (pq::is_dir(LEGACY_STATE_DIR) && !pq::exists(STATE)) {
        if (!pq::makedirs(STATE_DIR)) return fail(STATE_DIR);
        for (const auto& name : pq::listdir(LEGACY_STATE_DIR)) {
            std::string from = LEGACY_STATE_DIR + "/" + name, to = STATE_DIR + "/" + name;
            if (rename(from.c_str(), to.c_str()) != 0) return fail(from);
        }
        pq::log("pbosd: moved %s to %s", LEGACY_STATE_DIR.c_str(), STATE_DIR.c_str());
    }
    if (pq::is_dir(LEGACY_STATE_DIR) && pq::listdir(LEGACY_STATE_DIR).empty() &&
        rmdir(LEGACY_STATE_DIR.c_str()) != 0)
        return fail(LEGACY_STATE_DIR);
    // PB-OS Utils itself became the PB-OS tab in Quick Access
    // (pbos-qamd), and goes too.
    bool qam = pq::exists("/usr/lib/pbos-qam/pbos-qamd");
    if (!qam && !pq::is_dir(PLUGINS_DIR + "/pbos-utils")) return;
    std::vector<std::string> old = {"konkr-control", "pbos-control"};
    if (qam) old.push_back("pbos-utils");
    bool gone = false;
    for (const auto& name : old) {
        if (!pq::is_dir(PLUGINS_DIR + "/" + name)) continue;
        pq::remove_tree(PLUGINS_DIR + "/" + name);
        pq::log("pbosd: removed the old %s plugin (the PB-OS tab has its settings)", name.c_str());
        gone = true;
    }
    if (gone)
        pq::Loop::work([] {
            pq::RunOpts o;
            o.capture = false;
            o.timeout = 300;
            pq::run({"systemctl", "try-restart", "plugin_loader.service"}, o);
        });
}

// ------------------------------------------------------------- commands ---

pq::RunResult run_quiet(const std::vector<std::string>& argv, std::vector<std::string> env = {}) {
    pq::RunOpts o;
    o.capture = false;
    o.timeout = 300;
    o.env = std::move(env);
    return pq::run(argv, o);
}

void run_as_user(std::vector<std::string> cmd, std::vector<std::string> env) {
    std::vector<std::string> argv = {"runuser", "-u", USER, "--"};
    argv.insert(argv.end(), cmd.begin(), cmd.end());
    run_quiet(argv, std::move(env));
}

// The session environments the Python version gave each command.
std::vector<std::string> steam_env(uid_t uid) {
    return {"HOME=/home/steamos", "USER=steamos", "DISPLAY=:0", "XDG_RUNTIME_DIR=/run/user/" + std::to_string(uid),
            "PATH=/usr/bin:/bin"};
}
std::vector<std::string> session_env(uid_t uid) {
    std::string u = std::to_string(uid);
    return {"HOME=/home/steamos", "USER=steamos", "PATH=/usr/bin:/bin", "XDG_RUNTIME_DIR=/run/user/" + u,
            "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/" + u + "/bus"};
}

bool desktop_running() { return run_quiet({"pgrep", "-x", "plasmashell"}).code == 0; }

// Ask the running Steam client (user steamos) to handle a steam:// URL.
void steam_url(const std::string& url) {
    pq::User u = pq::user_named(USER);
    if (!u.ok) return;
    run_as_user({"/usr/bin/steam", "-ifrunning", url}, steam_env(u.uid));
}

// On a worker thread. Steam Deck behavior. Game Mode: Steam's own short
// (sleep) / long (power menu) press handling. Desktop Mode: sleep, or
// Plasma's shut down / restart / log out prompt.
void power_press(bool long_press) {
    if (!desktop_running()) {
        steam_url(long_press ? "steam://longpowerpress" : "steam://shortpowerpress");
        return;
    }
    if (!long_press) {
        run_quiet({"systemctl", "suspend"});
        return;
    }
    pq::User u = pq::user_named(USER);
    if (!u.ok) return;
    run_as_user({"qdbus6", "org.kde.LogoutPrompt", "/LogoutPrompt", "org.kde.LogoutPrompt.promptShutDown"},
                session_env(u.uid));
}

// Desktop Mode popup for button presses. Game Mode gets a Steam toast from
// the PB-OS tab instead (it watches the state file), so only notify while
// Plasma runs.
void notify_desktop(const std::string& title, const std::string& body) {
    pq::Loop::work([title, body] {
        if (!desktop_running()) return;
        pq::User u = pq::user_named(USER);
        if (!u.ok) return;
        run_as_user({"notify-send", "-a", "PB-OS", "-i", "preferences-system-power", "-t", "2000", "-h",
                     "string:x-canonical-private-synchronous:konkr-mode", title, body},
                    session_env(u.uid));
    });
}

// ------------------------------------------------------------- hardware ---

std::string find_fan() {
    for (const auto& d : pq::glob("/sys/class/hwmon/hwmon*"))
        if (pq::rd(d + "/name") == "pwmfan") return d;
    return "";
}

bool soc_zone(const std::string& type) {
    return type.rfind("cpu", 0) == 0 || type.rfind("gpu", 0) == 0 || type.find("gpuss") != std::string::npos;
}

// The CPU/GPU tsens zones: (folder, type).
std::vector<std::pair<std::string, std::string>> soc_zones() {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& z : pq::glob("/sys/class/thermal/thermal_zone*")) {
        std::string t = pq::rd(z + "/type");
        if (soc_zone(t)) out.emplace_back(z, t);
    }
    return out;
}

int curve_pwm(const Curve& curve, double temp) {
    if (temp <= curve.front().first) return curve.front().second;
    for (size_t i = 0; i + 1 < curve.size(); ++i) {
        auto [t0, p0] = curve[i];
        auto [t1, p1] = curve[i + 1];
        if (temp <= t1) return static_cast<int>(p0 + (p1 - p0) * (temp - t0) / (t1 - t0));
    }
    return curve.back().second;
}

std::string gpu_devfreq() {
    for (const auto& d : pq::glob("/sys/class/devfreq/*"))
        if (pq::basename(d).find("gpu") != std::string::npos || d.find("3d00000") != std::string::npos) return d;
    return "";
}

std::vector<long long> int_tokens(const std::string& text) {
    std::vector<long long> out;
    for (const auto& t : split_ws(text))
        if (pq::is_digits(t))
            if (auto n = py_int(t)) out.push_back(*n);
    return out;
}

void apply_gpu(const std::string& profile) {
    std::string d = gpu_devfreq();
    if (d.empty()) return;
    auto freqs = int_tokens(pq::rd(d + "/available_frequencies"));
    std::sort(freqs.begin(), freqs.end());
    if (freqs.empty()) return;
    long long cap = gpu_max_mhz(profile) * 1000000;
    long long fmax = 0;
    bool found = false;
    for (long long f : freqs)
        if (f <= cap && (!found || f > fmax)) fmax = f, found = true;
    if (!found) fmax = freqs.front();
    // order matters: never set min above the current max
    wr(d + "/max_freq", freqs.back());
    wr(d + "/min_freq", freqs.front());
    wr(d + "/max_freq", fmax);
    wr(d + "/polling_interval", 16);
}

std::set<int> related_cpus(const std::string& pol) {
    std::set<int> cpus;
    for (long long c : int_tokens(pq::rd(pol + "/related_cpus"))) cpus.insert(static_cast<int>(c));
    return cpus;
}

bool subset(const std::set<int>& a, const std::set<int>& b) {
    return std::includes(b.begin(), b.end(), a.begin(), a.end());
}

// schedutil everywhere (the ROCKNIX default is `performance`, which holds
// all eight cores — little A520s included — at max clock: heat for
// nothing). Low Power caps the big clusters' clock.
void apply_cpu(const std::string& profile) {
    std::set<int> big = pbosd::big_cpus();
    long long cap = big_cpu_max_khz(profile);
    for (const auto& pol : pq::glob("/sys/devices/system/cpu/cpufreq/policy*")) {
        std::string govs = pq::rd(pol + "/scaling_available_governors");
        std::set<int> cpus = related_cpus(pol);
        if (govs.find("schedutil") != std::string::npos) {
            wr(pol + "/scaling_governor", "schedutil");
            wr(pol + "/schedutil/rate_limit_us", 500);
        }
        std::string fmax = pq::rd(pol + "/cpuinfo_max_freq");
        if (cap && !cpus.empty() && subset(cpus, big)) {
            bool found = false;
            long long best = 0;
            for (long long f : int_tokens(pq::rd(pol + "/scaling_available_frequencies")))
                if (f <= cap && (!found || f > best)) best = f, found = true;
            if (found) fmax = std::to_string(best);
        }
        wr(pol + "/scaling_max_freq", fmax);
    }
}

std::string led(const std::string& pattern) {
    for (const auto& d : pq::glob("/sys/class/leds/*" + pattern + "*"))
        if (pq::exists(d + "/multi_intensity")) return d;
    return "";
}

void led_rgb(const std::string& d, Rgb c, long long brightness) {
    if (d.empty()) return;
    wr(d + "/multi_intensity", std::to_string(c.r) + " " + std::to_string(c.g) + " " + std::to_string(c.b));
    wr(d + "/brightness", brightness);
}

// (capacity, status) of the first battery; (-1, "") without one.
std::pair<long long, std::string> battery() {
    for (const auto& b : pq::glob("/sys/class/power_supply/*")) {
        if (pq::rd(b + "/type") != "Battery") continue;
        if (auto cap = py_int(pq::rd(b + "/capacity", "0"))) return {*cap, pq::rd(b + "/status")};
    }
    return {-1, ""};
}

bool charge_held() { return !flag_pid(CHARGE_HELD_FLAG).empty(); }

// InputPlumber keyboard (F13/F14 from the MCU buttons) + the PMIC power
// key. The Frame's steamos-powerbuttond is Requisite=steamvr.service (VR
// only), so nothing forwards the power button to Steam in Game Mode.
std::vector<std::string> hotkey_devices() {
    std::vector<std::string> out;
    for (const auto& ev : pq::glob("/sys/class/input/event*")) {
        std::string name = pq::rd(ev + "/device/name");
        if ((name.find("InputPlumber") != std::string::npos && name.find("Keyboard") != std::string::npos) ||
            name == "pmic_pwrkey")
            out.push_back("/dev/input/" + pq::basename(ev));
    }
    return out;
}

// The last word of a sysfs bitmap (its lowest bits), as int(…, 16).
std::optional<unsigned long long> low_bits(const std::string& path) {
    auto words = split_ws(pq::rd(path, "0"));
    if (words.empty()) return std::nullopt;
    const std::string& w = words.back();
    char* end = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(w.c_str(), &end, 16);
    if (errno || end == w.c_str() || *end) return std::nullopt;
    return v;
}

// Direct-touch devices with multitouch positions (the panel).
std::vector<std::string> touchscreens() {
    std::vector<std::string> out;
    for (const auto& ev : pq::glob("/sys/class/input/event*")) {
        auto props = low_bits(ev + "/device/properties");
        auto absbits = low_bits(ev + "/device/capabilities/abs");
        if (!props || !absbits) continue;
        if ((*props & (1ULL << INPUT_PROP_DIRECT)) && (*absbits & (1ULL << ABS_MT_POSITION_X)))
            out.push_back("/dev/input/" + pq::basename(ev));
    }
    return out;
}

// Install the MCU button map for `mode`; restart InputPlumber only when it
// changed. InputPlumber profiles cannot translate to or from trackpad clicks,
// so this cannot be a runtime profile switch.
void apply_buttons_mode(const std::string& mode) {
    std::string want = pq::rd(sysbtn_variant(one_of(mode, BUTTON_MODES) ? mode : "steam"));
    if (want.empty()) {
        pq::log("pbosd: buttons: missing %s", sysbtn_variant(mode).c_str());
        return;
    }
    if (pq::rd(SYSBTN_ACTIVE) == want) return;
    std::string tmp = SYSBTN_ACTIVE + ".tmp", text = want + "\n";
    pq::makedirs(pq::dirname(SYSBTN_ACTIVE));
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    bool ok = fd >= 0 && write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    if (fd >= 0 && close(fd) != 0) ok = false;
    if (!ok || chmod(tmp.c_str(), 0644) != 0 || rename(tmp.c_str(), SYSBTN_ACTIVE.c_str()) != 0) {
        pq::log("pbosd: buttons: cannot write %s: %s", SYSBTN_ACTIVE.c_str(), std::strerror(errno));
        return;
    }
    pq::Loop::work([mode] {
        run_quiet({"systemctl", "try-restart", "inputplumber.service"});
        pq::log("pbosd: buttons: %s mode (InputPlumber restarted)", mode.c_str());
    });
}

void ensure_gamepad_mode(const Conf& conf) {
    std::string want = conf.get("controller", "mode", "default");
    if (want != "default" && want != "xbox") return;
    if (pq::rd("/sys/firmware/devicetree/base/model").find("KONKR Pocket FIT") == std::string::npos) return;
    auto attrs = pq::glob("/sys/bus/serial/devices/*/gamepad_mode");
    if (attrs.empty()) return;
    bool in_xbox = false;
    for (const auto& d : pq::glob("/sys/bus/usb/devices/*"))
        if (pq::rd(d + "/idVendor") == XBOX_VID && pq::rd(d + "/idProduct") == XBOX_PID) in_xbox = true;
    if ((want == "default") == in_xbox && wr(attrs.front(), want))
        pq::log("pbosd: controller switched to %s mode", want.c_str());
}

// Re-apply the ANGLE-Vulkan flags if a Steam client update rewrote the
// launcher (Steam restarts the UI on its own after updates). A launcher
// patched before the PB-OS rename gets the flags' new name.
void ensure_webhelper_vulkan() {
    const std::string old_flags = "KONKR_CEF_FLAGS";
    std::string s = pq::rd(WEBHELPER_SH);
    std::string out;
    if (s.find(old_flags) != std::string::npos) {
        out = s;
        for (size_t at; (at = out.find(old_flags)) != std::string::npos;)
            out.replace(at, old_flags.size(), "PBOS_CEF_FLAGS");
    } else {
        if (s.empty() || s.find("PBOS_CEF_FLAGS") != std::string::npos || s.find(WEBHELPER_EXEC) == std::string::npos)
            return;
        for (size_t i = 0;;) {
            size_t at = s.find(WEBHELPER_EXEC, i);
            if (at == std::string::npos) {
                out += s.substr(i);
                break;
            }
            out += s.substr(i, at - i) + PBOS_CEF;
            i = at + WEBHELPER_EXEC.size();
        }
        out += "\n";
    }
    std::string tmp = WEBHELPER_SH + ".tmp";
    struct stat st;
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    bool ok = fd >= 0 && write(fd, out.data(), out.size()) == static_cast<ssize_t>(out.size());
    if (fd >= 0 && close(fd) != 0) ok = false;
    if (ok && stat(WEBHELPER_SH.c_str(), &st) == 0 && chown(tmp.c_str(), st.st_uid, st.st_gid) == 0 &&
        chmod(tmp.c_str(), st.st_mode & 07777) == 0 && rename(tmp.c_str(), WEBHELPER_SH.c_str()) == 0) {
        pq::log("pbosd: steamwebhelper.sh: re-applied ANGLE-Vulkan UI flags");
        return;
    }
    pq::log("pbosd: steamwebhelper.sh: %s", std::strerror(errno));
}

// ---------------------------------------------------------------- module ---

class Pbosd : public Module {
public:
    const char* name() const override { return "pbosd"; }

    ~Pbosd() override {
        for (auto& [fd, path] : keys_) close(fd);
        for (auto& [fd, path] : touch_) close(fd);
        if (tep_ >= 0) close(tep_);
        if (kep_ >= 0) close(kep_);
    }

    void start(pq::Loop& loop) override {
        loop_ = &loop;
        if (geteuid() != 0) pq::log("pbosd: not root: the hardware writes will fail");
        // Test only (tests/pbosd): the fan tick (and the LEDs' look at the
        // battery, which a fake sysfs cannot announce) runs each time this
        // file is written, instead of every second, and says when it ran.
        if (const char* t = std::getenv("PBOS_PLATFORMD_TEST_PBOSD_TICK")) test_tick_ = t;
        migrate_from_konkrd();
        conf_ = load_conf();
        st_ = load_state();
        fan_init();
        boost_ = std::make_unique<pbosd::GameBoost>(loop, conf_.getbool("games", "boost", true),
                                                    conf_.getbool("games", "avoid_little_cores", true));
        power_ = led("power-led");
        sticks_ = led("joysticks");
        touch_floors();
        // The key and touch devices each in an epoll of our own: one can be
        // taken out before it is closed, and the touch panel paused.
        kep_ = epoll_create1(EPOLL_CLOEXEC);
        loop.on_readable(kep_, [this] { keys_ready(); });
        tep_ = epoll_create1(EPOLL_CLOEXEC);
        loop.on_readable(tep_, [this] { touched(); });

        apply_profile(false);
        apply_sticks();
        apply_buttons_mode(st_["buttons_mode"].str());
        ensure_gamepad_mode(conf_);

        // Events.
        loop.watch(RUN_DIR, IN_CREATE | IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM | IN_Q_OVERFLOW,
                   [this](uint32_t mask, const std::string& name) {
                       if ((mask & IN_Q_OVERFLOW) || name == STANDBY_NAME || name == CHARGE_REST_NAME ||
                           name == CHARGE_HELD_NAME)
                           flags_changed();
                   });
        loop.on_uevent_env([this](const pq::Loop::UeventEnv& env) { uevent(env); });
        if (!test_tick_.empty()) {
            if (!loop.watch(test_tick_, IN_CLOSE_WRITE, [this](uint32_t, const std::string&) { test_tick(); }))
                pq::log("pbosd: cannot watch %s", test_tick_.c_str());
        }
        open_inputs();
        flags_changed();

        // What the Python version's first pass of its loop did.
        if (test_tick_.empty()) fan_tick();
        led_eval(true);
        boost_->start(st_["profile"].str());
        webhelper_arm();
        ensure_webhelper_vulkan();
        if (!standby_) ensure_gamepad_mode(conf_);
    }

    void stop() override {
        for (uint64_t* t : {&fan_timer_, &flash_timer_, &pulse_timer_, &long_timer_, &touch_timer_, &rearm_timer_,
                            &gamepad_timer_, &webhelper_timer_})
            if (*t) loop_->cancel(*t), *t = 0;
        // Never leave the fan at a low fixed speed: full speed, which is
        // where the unit's ExecStopPost left it after the Python version's
        // failsafe_pwm.
        if (!fan_dir_.empty()) wr(fan_dir_ + "/pwm1", 255);
        // Nor the CPU floor raised by a touch.
        if (touch_active_) set_touch_floors(false);
        pq::log("pbosd: stopped (fan at failsafe)");
    }

    // SIGHUP: read the settings again and put them all back (konkr-standby,
    // konkr-sleep and the charge rest send it after a wake; pbosctl and the
    // PB-OS tab after editing the state).
    void reload() override {
        conf_ = load_conf();
        st_ = load_state();
        apply_profile(true);
        apply_sticks();
        apply_buttons_mode(st_["buttons_mode"].str());
        ensure_gamepad_mode(conf_);
    }

private:
    // ------------------------------------------------------------- state --
    std::string profile() const { return std::as_const(st_)["profile"].str(); }

    void apply_profile(bool flash_led) {
        std::string p = profile();
        apply_cpu(p);
        apply_gpu(p);
        if (boost_) boost_->set_profile(p);
        if (flash_led) flash(profile_rgb(p));
        pq::log("pbosd: profile %s", p.c_str());
    }

    void action(std::string name) {
        name = legacy_action(name);
        if (name == "profile-next" || name == "profile-lowpower" || name == "profile-balanced") {
            std::string p = profile();
            if (name == "profile-next") {
                size_t i = p == PROFILES[0] ? 0 : 1;
                st_["profile"] = PROFILES[(i + 1) % 2];
            } else {
                st_["profile"] = name.substr(8);
            }
            save_state(st_);
            apply_profile(true);
            notify_desktop(profile_label(profile()), "Performance profile");
        } else if (name == "sticks-toggle") {
            Value& rgb = st_["rgb"];
            if (!rgb.is_object()) return;
            const Value& mode = std::as_const(rgb)["mode"];
            if (is_str(mode, "off")) {
                Value m = get(rgb, "last_mode", Value(RGB_DEFAULT_MODE));
                Value c = get(rgb, "last_color", Value(RGB_DEFAULT_COLOR));
                rgb["mode"] = m;
                rgb["color"] = c;
            } else {
                Value m = get(rgb, "mode", Value()), c = get(rgb, "color", Value());
                rgb["last_mode"] = m;
                rgb["last_color"] = c;
                rgb["mode"] = "off";
            }
            save_state(st_);
            apply_sticks();
        } else if (name == "rgb-next") {
            Value& rgb = st_["rgb"];
            if (!rgb.is_object()) return;
            Value m = get(rgb, "mode", Value()), c = get(rgb, "color", Value());
            size_t n = std::size(RGB_PRESETS), i = 0;
            for (size_t k = 0; k < n; ++k)
                if (is_str(m, RGB_PRESETS[k].first) && is_str(c, RGB_PRESETS[k].second)) {
                    i = k + 1;
                    break;
                }
            auto [mode, color] = RGB_PRESETS[i % n];
            rgb["mode"] = mode;
            rgb["color"] = color;
            save_state(st_);
            apply_sticks();
            if (sticks_.empty()) {
                std::string h = color;
                flash({std::stoi(h.substr(0, 2), nullptr, 16), std::stoi(h.substr(2, 2), nullptr, 16),
                       std::stoi(h.substr(4, 2), nullptr, 16)});
            }
        }
    }

    // ----------------------------------------------------------------- fan --
    void fan_init() {
        fan_dir_ = find_fan();
        fan_wanted_ = conf_.getbool("fan", "enabled", true);
        min_pwm_ = conf_.getint("fan", "min_pwm", 0);
        zones_ = soc_zones();
        if (fan_dir_.empty()) {
            // The pwm-fan driver may still be probing: its hwmon's uevent
            // says when it is there (the Python version gave up).
            pq::log("pbosd: fan: no pwmfan hwmon found");
            return;
        }
        fan_found();
    }

    void fan_found() {
        if (!fan_wanted_) return;
        wr(fan_dir_ + "/pwm1_enable", 1);
        pq::log("pbosd: fan: %s", fan_dir_.c_str());
        if (test_tick_.empty() && !fan_timer_) fan_timer_ = loop_->after(FAN_TICK_S, [this] { fan_timer(); });
    }

    void fan_timer() {
        fan_timer_ = loop_->after(FAN_TICK_S, [this] { fan_timer(); });
        fan_tick();
    }

    // Hottest CPU/GPU tsens zone, °C, and its name; nothing when none reads.
    std::optional<std::pair<double, std::string>> soc_temp() const {
        std::optional<std::pair<double, std::string>> hottest;
        for (const auto& [z, type] : zones_) {
            auto v = py_int(pq::rd(z + "/temp"));
            if (!v) continue;
            double value = static_cast<double>(*v) / 1000.0;
            if (value >= -20 && value <= 150 && (!hottest || value > hottest->first)) hottest = {{value, type}};
        }
        return hottest;
    }

    void fan_tick() {
        if (fan_dir_.empty() || !fan_wanted_) return;
        static const Value none = Value::object();
        const Value& st_fan = std::as_const(st_)["fan"];
        const Value& fan = st_fan.is_object() ? st_fan : none;
        auto hot = soc_temp();
        if (!hot) {
            wr(fan_dir_ + "/pwm1", 255);
            cur_ = 255;
            smooth_.reset();
            return;
        }
        double raw = hot->first;
        // Safety: never hold a low fixed speed when hot. Latched with
        // hysteresis: under load the SoC sits right at 90 °C, and flipping
        // every tick between the fixed speed and the curve (100 % there) made
        // the fan pulse (user report, v1.1).
        bool fixed = is_str(get(fan, "mode", Value()), "fixed");
        if (fixed) {
            if (!hot_override_ && raw >= FIXED_OVERRIDE_ON_C) {
                hot_override_ = true;
                pq::log("pbosd: fan: %.0f C, overriding the fixed speed until %d C", raw, FIXED_OVERRIDE_OFF_C);
            } else if (hot_override_ && raw <= FIXED_OVERRIDE_OFF_C) {
                hot_override_ = false;
                pq::log("pbosd: fan: %.0f C, back to the fixed speed", raw);
            }
            if (hot_override_) fixed = false;
        } else {
            hot_override_ = false;
        }
        if (fixed) {
            const Value& pct = get(fan, "fixed", Value(50));
            long long n = int_of(pct).value_or(50);
            long long step = std::max(0LL, std::min(255LL, static_cast<long long>(static_cast<double>(n) * 2.55)));
            if (step != cur_ && wr(fan_dir_ + "/pwm1", step)) {
                pq::log("pbosd: fan: fixed %s%% -> pwm %lld", pct.dump().c_str(), step);
                cur_ = step;
            }
            return;
        }
        // Smooth short spikes (boot, shader compiles) but react at once to
        // genuinely hot readings.
        if (!smooth_ || raw >= 85) smooth_ = raw;
        else smooth_ = 0.75 * *smooth_ + 0.25 * raw;
        double temp = *smooth_;
        long long want = std::max<long long>(min_pwm_, curve_pwm(fan_curve(profile()), temp));
        long long step;
        if (want > cur_) {
            step = want;  // up: immediately
            set_temp_ = temp;
        } else if (want < cur_ && temp <= set_temp_ - 3) {
            step = std::max(want, cur_ - 15);  // down: once 3 °C below, ramp
        } else {
            return;
        }
        if (step != cur_ && wr(fan_dir_ + "/pwm1", step)) {
            if (std::llabs(step - cur_) >= 40 || cur_ < 0)
                pq::log("pbosd: fan: %.1f C (%s) -> pwm %lld", temp, hot->second.c_str(), step);
            cur_ = step;
        }
    }

    // ---------------------------------------------------------------- leds --
    enum class Led { Unknown, Dark, Charged, Charging, Low };

    Led led_want() const {
        const Value& on = std::as_const(st_)["power_led"];
        if ((st_.has("power_led") && !on.truthy()) || pq::exists(STANDBY_FLAG)) return Led::Dark;
        auto [cap, status] = battery();
        if (status == "Full" || (status == "Not charging" && cap >= 99) || charge_held()) return Led::Charged;
        if (status == "Charging") return Led::Charging;
        if (cap >= 0 && cap < 15) return Led::Low;
        return Led::Dark;
    }

    // The power LED shows what led_want() says: written when that changed
    // (the Python version wrote it every second), or always when forced
    // (after a flash, a wake, a reload).
    void led_eval(bool force) {
        if (power_.empty() || flash_timer_) return;
        Led k = led_want();
        if (k == Led::Low) {
            if (led_ != Led::Low || force) {
                led_ = Led::Low;
                pulse();
            }
            return;
        }
        if (pulse_timer_) loop_->cancel(pulse_timer_), pulse_timer_ = 0;
        if (k == led_ && !force) return;
        led_ = k;
        if (k == Led::Charged) led_rgb(power_, {0, 255, 0}, 30);
        else if (k == Led::Charging) led_rgb(power_, {255, 120, 0}, 40);
        else wr(power_ + "/brightness", 0);
    }

    // Low battery: red, on and off a second each.
    void pulse() {
        pulse_on_ = !pulse_on_;
        led_rgb(power_, {255, 0, 0}, pulse_on_ ? 60 : 0);
        if (pulse_timer_) loop_->cancel(pulse_timer_);
        pulse_timer_ = 0;
        if (test_tick_.empty())
            pulse_timer_ = loop_->after(PULSE_S, [this] {
                pulse_timer_ = 0;
                led_eval(false);
                if (led_ == Led::Low && !flash_timer_ && !pulse_timer_) pulse();
            });
    }

    void flash(Rgb c) {
        if (power_.empty()) return;
        led_rgb(power_, c, 255);
        if (flash_timer_) loop_->cancel(flash_timer_);
        if (pulse_timer_) loop_->cancel(pulse_timer_), pulse_timer_ = 0;
        flash_timer_ = loop_->after(FLASH_S, [this] {
            flash_timer_ = 0;
            led_eval(true);
        });
    }

    void apply_sticks() {
        if (sticks_.empty()) return;
        static const Value none = Value::object();
        const Value& rgb = std::as_const(st_)["rgb"];
        const Value& cfg = rgb.is_object() ? rgb : none;
        const Value& mode = get(cfg, "mode", Value(RGB_DEFAULT_MODE));
        if (is_str(mode, "off")) {
            wr(sticks_ + "/brightness", 0);
            return;
        }
        std::string color = RGB_DEFAULT_COLOR;
        const Value& cv = get(cfg, "color", Value(RGB_DEFAULT_COLOR));
        if (cv.is_string() && cv.as_string().size() == 6 &&
            cv.as_string().find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
            color = cv.as_string();
        Rgb c{std::stoi(color.substr(0, 2), nullptr, 16), std::stoi(color.substr(2, 2), nullptr, 16),
              std::stoi(color.substr(4, 2), nullptr, 16)};
        long long brightness = int_of(get(cfg, "brightness", Value(160))).value_or(160);
        std::string eff = sticks_ + "/effect";
        if (is_str(mode, "breath")) {
            // The MCU pulses the color it is given: brightness scales it.
            long long level = std::max(0LL, std::min(255LL, brightness));
            wr(eff, "breath " + std::to_string(c.r * level / 255) + " " + std::to_string(c.g * level / 255) + " " +
                        std::to_string(c.b * level / 255));
        } else if (is_str(mode, "rainbow")) {
            wr(eff, "rainbow");  // the MCU's own hue cycle
        } else {
            wr(eff, "static");
            led_rgb(sticks_, c, brightness);
        }
    }

    // --------------------------------------------------------------- flags --
    // konkr-standby, the charge rest or the charge hold started or ended.
    void flags_changed() {
        bool standby = pq::exists(STANDBY_FLAG);
        std::string rest = flag_pid(CHARGE_REST_FLAG), held = flag_pid(CHARGE_HELD_FLAG);
        watch_exit(rest, rest_watched_);
        watch_exit(held, held_watched_);
        bool quiet = standby || !rest.empty();
        if (quiet && !quiet_) cancel_press();
        if (!quiet && quiet_) grace_until_ = monotonic() + WAKE_GRACE_S;  // still waking up
        quiet_ = quiet;
        if (standby != standby_) {
            standby_ = standby;
            touch_tick();
            led_eval(true);
            if (!standby) gamepad_soon();  // the pad may have changed meanwhile
        } else if (held != held_) {
            led_eval(false);
        }
        held_ = held;
    }

    // The flag's process ending is the flag ending (its file may stay).
    void watch_exit(const std::string& pid, std::string& watched) {
        if (pid.empty() || pid == watched) return;
        watched = pid;
        if (!loop_->on_exit(static_cast<pid_t>(std::stol(pid)), [this, pid, &watched] {
                if (watched == pid) watched.clear();
                flags_changed();
            }))
            watched.clear();
    }

    // ---------------------------------------------------------------- keys --
    void open_inputs() {
        std::set<std::string> have;
        for (auto& [fd, path] : keys_) have.insert(path);
        for (const auto& path : hotkey_devices()) {
            if (have.count(path)) continue;
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            keys_[fd] = path;
            epoll_event ev{};
            ev.events = EPOLLIN;
            ev.data.fd = fd;
            epoll_ctl(kep_, EPOLL_CTL_ADD, fd, &ev);
            pq::log("pbosd: hotkeys: listening on %s", path.c_str());
        }
        have.clear();
        for (auto& [fd, path] : touch_) have.insert(path);
        for (const auto& path : touchscreens()) {
            if (have.count(path)) continue;
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) continue;
            touch_[fd] = path;
            epoll_event ev{};
            ev.events = sampling() ? 0 : EPOLLIN | EPOLLONESHOT;
            ev.data.fd = fd;
            epoll_ctl(tep_, EPOLL_CTL_ADD, fd, &ev);
            pq::log("pbosd: touch boost: watching %s", path.c_str());
        }
    }

    void keys_ready() {
        epoll_event evs[8];
        int n = epoll_wait(kep_, evs, 8, 0);
        for (int i = 0; i < n; ++i) read_keys(evs[i].data.fd);
    }

    void read_keys(int fd) {
        input_event evs[64];
        while (keys_.count(fd)) {
            ssize_t n = read(fd, evs, sizeof evs);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
            if (n < 0 && errno != ENODEV && errno != EBADF) return;
            if (n <= 0) {
                // Gone (InputPlumber stopped for sleep, or restarted): its
                // uevent brings it back, and a look now catches one that came
                // back already.
                pq::log("pbosd: hotkeys: lost %s", keys_[fd].c_str());
                epoll_ctl(kep_, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                keys_.erase(fd);
                open_inputs();
                return;
            }
            flags_changed();  // as the Python version looked at the flags on each key
            for (ssize_t i = 0; i < n / static_cast<ssize_t>(sizeof(input_event)); ++i) {
                const auto& e = evs[i];
                if (e.type != EV_KEY || (e.value != 0 && e.value != 1)) continue;
                key(e.code, e.value);
            }
        }
    }

    void key(int code, int value) {
        double now = monotonic();
        if (quiet_) {
            // konkr-standby / the charge rest own the buttons
            grace_until_ = now + WAKE_GRACE_S;
            return;
        }
        if (code == KEY_POWER) {
            if (now < grace_until_) {
                cancel_press();  // still waking up
                return;
            }
            if (value == 1) {
                cancel_press();
                down_ = true;
                long_timer_ = loop_->after(LONG_PRESS_S, [this] {
                    long_timer_ = 0;
                    if (!down_ || long_sent_) return;
                    long_sent_ = true;
                    pq::Loop::work([] { power_press(true); });
                });
            } else if (down_) {
                if (!long_sent_) pq::Loop::work([] { power_press(false); });
                cancel_press();
            }
            return;
        }
        const char* name = code == KEY_F13 ? "F13" : code == KEY_F14 ? "F14" : code == KEY_F15 ? "F15" : nullptr;
        if (!name || value != 1) return;
        static const Value none = Value::object();
        const Value& buttons = std::as_const(st_)["buttons"];
        const Value& mine = get(buttons.is_object() ? buttons : none, name, Value());
        std::string act = mine.truthy() ? (mine.is_string() ? mine.as_string() : "none")
                                        : conf_.get("buttons", name, "none");
        action(act);
    }

    void cancel_press() {
        down_ = false;
        long_sent_ = false;
        if (long_timer_) loop_->cancel(long_timer_);
        long_timer_ = 0;
    }

    // --------------------------------------------------------------- touch --
    void touch_floors() {
        std::set<int> big = pbosd::big_cpus();
        for (const auto& pol : pq::glob("/sys/devices/system/cpu/cpufreq/policy*")) {
            std::set<int> cpus = related_cpus(pol);
            long long want = !cpus.empty() && subset(cpus, big) ? TOUCH_BOOST_BIG_HZ : TOUCH_BOOST_LITTLE_HZ;
            auto freqs = int_tokens(pq::rd(pol + "/scaling_available_frequencies"));
            std::sort(freqs.begin(), freqs.end());
            long long pick = freqs.empty() ? 0 : freqs.back();
            for (long long f : freqs)
                if (f >= want) {
                    pick = f;
                    break;
                }
            if (pick) floors_.emplace_back(pol, pick);
        }
    }

    // Right after a touch the panel is not read for TOUCH_SAMPLE_S: the
    // boost is on, and at ~280 Hz it must not wake us each time.
    bool sampling() const { return monotonic() - last_touch_ < TOUCH_SAMPLE_S; }

    void touched() {
        epoll_event evs[8];
        int n = epoll_wait(tep_, evs, 8, 0);
        bool any = false;
        for (int i = 0; i < n; ++i) {
            int fd = evs[i].data.fd;
            if (!touch_.count(fd)) continue;
            input_event buf[256];
            bool gone = false;
            while (true) {
                ssize_t r = read(fd, buf, sizeof buf);
                if (r > 0 || (r < 0 && errno == EINTR)) continue;
                if (r == 0 || errno == EAGAIN) any = true;  // drained: a touch
                else if (errno == ENODEV || errno == EBADF) gone = true;
                break;  // another error: not a touch
            }
            if (gone) {
                epoll_ctl(tep_, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                touch_.erase(fd);
                open_inputs();
            }
        }
        if (!any) {
            arm_touch(true);  // nothing read: keep listening
            return;
        }
        last_touch_ = monotonic();
        arm_touch(false);
        if (rearm_timer_) loop_->cancel(rearm_timer_);
        rearm_timer_ = loop_->after(TOUCH_SAMPLE_S, [this] {
            rearm_timer_ = 0;
            arm_touch(true);
        });
        touch_tick();
    }

    void arm_touch(bool on) {
        for (auto& [fd, path] : touch_) {
            epoll_event ev{};
            ev.events = on ? EPOLLIN | EPOLLONESHOT : 0;
            ev.data.fd = fd;
            epoll_ctl(tep_, EPOLL_CTL_MOD, fd, &ev);
        }
    }

    void touch_tick() {
        double now = monotonic();
        bool want = !standby_ && now - last_touch_ < TOUCH_BOOST_HOLD_S;
        if (touch_timer_) loop_->cancel(touch_timer_), touch_timer_ = 0;
        if (want) touch_timer_ = loop_->after(last_touch_ + TOUCH_BOOST_HOLD_S - now, [this] {
            touch_timer_ = 0;
            touch_tick();
        });
        if (want == touch_active_) return;
        touch_active_ = want;
        set_touch_floors(want);
    }

    void set_touch_floors(bool on) {
        for (const auto& [pol, floor] : floors_)
            wr(pol + "/scaling_min_freq", on ? std::to_string(floor) : pq::rd(pol + "/cpuinfo_min_freq"));
    }

    // ------------------------------------------------------------- uevents --
    void uevent(const pq::Loop::UeventEnv& env) {
        if (env.empty()) {  // some were dropped: look at everything
            open_inputs();
            led_eval(false);
            gamepad_soon();
            zones_ = soc_zones();
            if (fan_dir_.empty() && !(fan_dir_ = find_fan()).empty()) fan_found();
            return;
        }
        auto val = [&env](const char* k) {
            auto it = env.find(k);
            return it == env.end() ? std::string() : it->second;
        };
        std::string sub = val("SUBSYSTEM"), act = val("ACTION");
        if (sub == "input") {
            if (act == "add" && val("DEVNAME").rfind("input/event", 0) == 0) open_inputs();
        } else if (sub == "power_supply") {
            led_eval(false);
        } else if ((sub == "usb" && val("DEVTYPE") == "usb_device") || sub == "serial") {
            if (act != "change") gamepad_soon();
        } else if (sub == "hwmon") {
            if (act == "add" && fan_dir_.empty() && !(fan_dir_ = find_fan()).empty()) fan_found();
        } else if (sub == "thermal") {
            if (act == "add" || act == "remove") zones_ = soc_zones();
        }
    }

    void gamepad_soon() {
        if (gamepad_timer_) loop_->cancel(gamepad_timer_);
        gamepad_timer_ = loop_->after(GAMEPAD_SETTLE_S, [this] {
            gamepad_timer_ = 0;
            if (!standby_) ensure_gamepad_mode(conf_);
        });
    }

    // ----------------------------------------------------------- webhelper --
    // Watches for Steam's launcher being written (inotify), from the
    // deepest folder of its path that exists: a fresh home has no Steam yet.
    void webhelper_arm() {
        for (int id : webhelper_watches_) loop_->unwatch(id);
        webhelper_watches_.clear();
        std::vector<std::string> chain;
        for (std::string p = WEBHELPER_DIR; p.size() > 1; p = pq::dirname(p)) chain.insert(chain.begin(), p);
        size_t deepest = 0;
        bool any = false;
        for (size_t i = 0; i < chain.size() && pq::is_dir(chain[i]); ++i) deepest = i, any = true;
        if (!any) return;
        auto rearm = [this] { webhelper_soon(true); };
        uint32_t self = IN_DELETE_SELF | IN_MOVE_SELF;
        if (deepest + 1 == chain.size()) {
            add_webhelper_watch(chain[deepest], IN_CLOSE_WRITE | IN_MOVED_TO | IN_Q_OVERFLOW | self,
                                [this](uint32_t mask, const std::string& name) {
                                    if (mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) webhelper_soon(true);
                                    else if (name == WEBHELPER_NAME || (mask & IN_Q_OVERFLOW)) webhelper_soon(false);
                                });
            if (deepest == 0) return;
            --deepest;  // and its parent, for the folder being replaced
        }
        std::string next = pq::basename(chain[deepest + 1]);
        add_webhelper_watch(chain[deepest], IN_CREATE | IN_MOVED_TO | IN_Q_OVERFLOW | self,
                            [rearm, next](uint32_t mask, const std::string& name) {
                                if ((mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_Q_OVERFLOW)) ||
                                    name == next)
                                    rearm();
                            });
    }

    void add_webhelper_watch(const std::string& path, uint32_t mask, pq::Loop::WatchFn cb) {
        if (int id = loop_->watch(path, mask, std::move(cb))) webhelper_watches_.push_back(id);
    }

    void webhelper_soon(bool rearm) {
        webhelper_rearm_ = webhelper_rearm_ || rearm;
        if (webhelper_timer_) return;
        webhelper_timer_ = loop_->after(WEBHELPER_SETTLE_S, [this] {
            webhelper_timer_ = 0;
            if (webhelper_rearm_) webhelper_arm();
            webhelper_rearm_ = false;
            ensure_webhelper_vulkan();
        });
    }

    // ---------------------------------------------------------------- test --
    void test_tick() {
        fan_tick();
        // What a power_supply uevent does, and the pulse's own step.
        if (!power_.empty() && !flash_timer_) {
            if (led_ == Led::Low && led_want() == Led::Low) pulse();
            else led_eval(false);
        }
        pq::log("pbosd: test tick");
    }

    pq::Loop* loop_ = nullptr;
    std::string test_tick_;
    Conf conf_;
    Value st_;
    std::unique_ptr<pbosd::GameBoost> boost_;

    // fan
    std::string fan_dir_;
    bool fan_wanted_ = true;
    long long min_pwm_ = 0, cur_ = -1;
    std::optional<double> smooth_;
    double set_temp_ = 0;
    bool hot_override_ = false;
    std::vector<std::pair<std::string, std::string>> zones_;
    uint64_t fan_timer_ = 0;

    // leds
    std::string power_, sticks_;
    Led led_ = Led::Unknown;
    bool pulse_on_ = false;
    uint64_t flash_timer_ = 0, pulse_timer_ = 0;

    // flags
    bool standby_ = false, quiet_ = false;
    std::string held_, rest_watched_, held_watched_;
    double grace_until_ = 0;

    // keys
    std::map<int, std::string> keys_;  // fd -> /dev/input path, in kep_
    int kep_ = -1;
    bool down_ = false, long_sent_ = false;
    uint64_t long_timer_ = 0;

    // touch
    std::map<int, std::string> touch_;  // fd -> /dev/input path, in tep_
    int tep_ = -1;
    std::vector<std::pair<std::string, long long>> floors_;  // cpufreq policy -> boost floor
    double last_touch_ = -1e9;
    bool touch_active_ = false;
    uint64_t touch_timer_ = 0, rearm_timer_ = 0;

    uint64_t gamepad_timer_ = 0, webhelper_timer_ = 0;
    std::vector<int> webhelper_watches_;
    bool webhelper_rearm_ = false;
};

}  // namespace

// The unit's ExecCondition: the KONKR Pocket FIT and the AYANEO Pocket S2.
bool pbosd_here() {
    std::string model = pq::dt_model();
    return model.find("KONKR Pocket FIT") != std::string::npos || model.find("AYANEO Pocket S2") != std::string::npos;
}

std::unique_ptr<Module> make_pbosd() { return std::make_unique<Pbosd>(); }

}  // namespace pp

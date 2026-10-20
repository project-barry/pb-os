// The PB-OS Tools section: the backend of what was the PB-OS Tools Decky
// plugin (pbos-utils), method for method. Every tab is a front for a
// system helper that keeps its own settings; this runs them and reads
// their answers.
//
// Games    per-game profiles (port of Armada's per-game compatibility
//          settings, armada-os/armada, GPL-2.0-or-later), kept in
//          ~/.config/pb-os/game-profiles.json; /usr/bin/pbos-game-launch
//          applies them when the game starts.
// Update   pb-os updates: a front for /usr/share/konkr-update/konkr-update.py.
//          The download and staging run as the transient unit pbos-update;
//          this reads its log. An update on a drive or in ~/Downloads is
//          looked for when a drive comes or goes (kernel uevents) or the
//          Downloads folder changes (inotify), and installed with
//          local-update.
// Install  moves SteamOS between the microSD card and internal storage with
//          the Easy UFS Installer's scripts, as the transient unit
//          pbos-utils-move; its end is heard from systemd (D-Bus).
// Lights   stick lighting: pbosd's state on the KONKR Pocket FIT / AYANEO
//          Pocket S2; the light engine (lights.cpp, pbos-qamd --lights as
//          its own unit) on the Retroid Pocket 6 / Nova and AYN Odin 2s.
// Performance  a mode for when no game says otherwise and one per game
//          (Full Power, Low Power): pbosd's profile on the KONKR Pocket FIT /
//          AYANEO Pocket S2, clock caps written here everywhere else. The
//          frontend says which game runs; a profile change from anywhere
//          shows a toast (pbosd's state file is watched with inotify).
// Hardware pbosd's buttons and MCU link, the fan.
// Power    the charging indicator (sm8550-charge-rest) and charge control
//          (sm8550-charge-control).
// Proton   which PB-OS twin stands for which of Valve's Proton tools
//          (/usr/lib/steamos/pbos-proton); the frontend sets them per game.
// Face buttons, Sticks, Barry Launcher in Desktop Mode: fronts for
//          sm8550-face-buttons, sm8550-stickcal and
//          barry_launcher_desktop_switch.

#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <mutex>
#include <regex>
#include <utility>

#include "section.h"
#include "util.h"

namespace pq {

namespace {

using pj::Value;

const std::string UPDATER = "/usr/share/konkr-update/konkr-update.py";
const std::string UNIT = "pbos-update";
const std::string LOG = "/run/pbos-update.log";
const std::string PENDING = "/var/lib/konkr-update/pending";
const std::string VERSION = "/usr/share/pb-os/version";
constexpr double CHECK_EVERY = 6 * 3600;
// A drive put in settles (partitions, SteamOS's own mount) this long first.
constexpr double DRIVE_SETTLE = 2;

const std::string UFS = "/usr/share/easy-ufs-install";
const std::string TO_INTERNAL = UFS + "/install-masios-to-internal.sh";
const std::string TO_SD = UFS + "/copy-to-sd.sh";
const std::string PROBE_INTERNAL = UFS + "/ufs-probe-sizes.sh";
const std::string HOME_COPY = UFS + "/home-copy.sh";
const std::string MOVE_UNIT = "pbos-utils-move";
const std::string MOVE_LOG = "/run/pbos-utils-move.log";
const std::string MOVE_ARGS = "/run/pbos-utils-move.json";
constexpr double SIZES_TTL = 600;

const std::string PBOSD_STATE = "/var/lib/pbosd/state.json";
const std::string LIGHTS_STATE = "/var/lib/pbos-utils/lights.json";
const std::string ENGINE_UNIT = "pbos-utils-lights";
const std::string ENGINE = "/usr/lib/pbos-qam/pbos-qamd";
const std::string MCU_BLACKLIST = "/etc/modprobe.d/konkr-mcu.conf";
const std::string CHARGE_REST = "/usr/lib/steamos-sm8550/sm8550-charge-rest";
const std::string PREVIEW_UNIT = "pbos-utils-charge-preview";
const std::string CHARGE_CONTROL = "/usr/lib/steamos-sm8550/sm8550-charge-control";
const std::string FACE_BUTTONS = "/usr/lib/steamos/sm8550-face-buttons";
const std::string STICKCAL = "/usr/lib/steamos-sm8550/sm8550-stickcal";
const std::string BARRY_SWITCH = "/usr/lib/barry_launcher/barry_launcher_desktop_switch";
const std::string PBOS_MTP = "/usr/lib/pbos-mtp/pbos-mtp";
const std::string MTP_UNIT = "pbos-mtp";
const std::string PBOS_PROTON = "/usr/lib/steamos/pbos-proton";
const std::string GAME_LAUNCH = "/usr/bin/pbos-game-launch";
const std::string COMMUNITY_HINTS = "/usr/share/pb-os/game-launch/protondb-hints.json";

const char* const STICK_AXES[] = {"left_x", "left_y", "right_x", "right_y"};
constexpr double STICK_REST_S = 1.5;
constexpr double STICK_REST_SPREAD = 80;  // a resting stick moving more than this was touched
constexpr int STICK_EDGES_MAX_S = 180;

const char* const DEFAULT_EFFECT = "breathing";
const char* const DEFAULT_COLOR = "8c00ff";
const char* const DEFAULT_COLOR2 = "ffffff";

double epoch() {
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string last_line(const std::string& text, const std::string& def) {
    auto lines = split_lines(trim(text));
    return lines.empty() ? def : lines.back();
}

std::string remove_prefix(std::string s, std::string_view p) {
    if (s.rfind(p, 0) == 0) s.erase(0, p.size());
    return s;
}

const std::regex& ansi() {
    static const std::regex re("\x1b\\[[0-9;]*[A-Za-z]");
    return re;
}

std::string strip_ansi(const std::string& s) { return std::regex_replace(s, ansi(), ""); }

Value keyvals(const std::string& text) {
    static const std::regex key("^[A-Z_]+$");
    Value out = Value::object();
    for (const auto& line : split_lines(text)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        if (std::regex_match(k, key)) out[k] = line.substr(eq + 1);
    }
    return out;
}

Value parse_or(const std::string& text, Value def) {
    auto v = Value::parse(trim(text));
    return v ? *v : def;
}

// ------------------------------------------------------------------ lights --
// As lights.cpp (the engine keeps its own copy of these).
Value effects_json(const char* kind) {
    if (std::string(kind) == "pbosd")
        return *Value::parse(R"([
            {"id": "static", "label": "Static", "color": true, "speed": false},
            {"id": "breathing", "label": "Breathing", "color": true, "speed": false},
            {"id": "rainbow", "label": "Rainbow", "color": false, "speed": false}])");
    return *Value::parse(R"([
        {"id": "static", "label": "Static", "color": true, "speed": false},
        {"id": "breathing", "label": "Breathing", "color": true, "speed": true},
        {"id": "cycle", "label": "Color Cycle", "color": false, "speed": true},
        {"id": "wave", "label": "Rainbow Wave", "color": false, "speed": true, "turns": true},
        {"id": "spin", "label": "Spin", "color": true, "speed": true, "turns": true},
        {"id": "starlight", "label": "Starlight", "color": true, "speed": true}])");
}

Value audio_effects() {
    return *Value::parse(R"([
        {"id": "audio", "label": "Audio Meter", "color": true, "speed": false, "experimental": true},
        {"id": "spectrum", "label": "Audio Spectrum", "color": true, "speed": false, "experimental": true}])");
}

bool is_audio_mode(const std::string& e) { return e == "audio" || e == "spectrum"; }

// pbosd's state names for the MCU's modes.
const std::pair<const char*, const char*> PBOSD_MODES[] = {
    {"static", "static"}, {"breathing", "breath"}, {"rainbow", "rainbow"}};

std::string pbosd_mode(const std::string& effect) {
    for (auto& [e, m] : PBOSD_MODES)
        if (effect == e) return m;
    return "static";
}

size_t led_count() {
    size_t n = glob("/sys/class/leds/rgb:[lr][1-4]").size();
    for (const char* z : {"left-joystick", "left-side", "right-joystick", "right-side"})
        if (exists(std::string("/sys/class/leds/") + z + "/multi_intensity")) ++n;
    return n;
}

std::string lights_kind() {
    if (dt_compatible().find("ayn,thor") != std::string::npos) return "";  // Barry Launcher's Lights tab
    static const std::regex pbosd("KONKR Pocket FIT|AYANEO Pocket S2");
    if (std::regex_search(rd("/sys/firmware/devicetree/base/model"), pbosd)) return "pbosd";
    if (led_count()) return "multicolor";
    return "";
}

std::string clean_color(const Value& color) {
    std::string c = lower(color.str());
    while (!c.empty() && c[0] == '#') c.erase(0, 1);
    c = c.substr(0, 6);
    static const std::regex hex("^[0-9a-f]{6}$");
    return std::regex_match(c, hex) ? c : DEFAULT_COLOR;
}

Value multicolor_effects(bool audio) {
    Value effects = effects_json("multicolor");
    if (audio)
        for (const auto& e : audio_effects().items()) effects.push(e);
    if (led_count() < 6)  // two zones per stick (Odin 2): nothing to turn either way round
        for (auto& e : effects.items()) e["turns"] = false;
    return effects;
}

bool effect_known(const Value& effects, const std::string& id) {
    for (const auto& e : effects.items())
        if (e["id"].as_string() == id) return true;
    return false;
}

Value pbosd_lights() {
    Value st = read_json(PBOSD_STATE);
    Value rgb = st["rgb"].is_object() ? st["rgb"] : Value::object();
    std::string mode = rgb.has("mode") ? rgb["mode"].str() : pbosd_mode(DEFAULT_EFFECT);
    bool on = mode != "off";
    // Off keeps what was on in last_mode/last_color, as pbosd's K button does.
    std::string shown = on ? mode : (rgb.has("last_mode") ? rgb["last_mode"].str() : pbosd_mode(DEFAULT_EFFECT));
    std::string effect = "static";
    for (auto& [e, m] : PBOSD_MODES)
        if (shown == m) {
            effect = e;
            break;
        }
    Value color = on ? rgb["color"] : (rgb.has("last_color") ? rgb["last_color"] : rgb["color"]);
    Value out = Value::object();
    out["on"] = on;
    out["effect"] = effect;
    out["color"] = clean_color(color.truthy() ? color : Value(DEFAULT_COLOR));
    out["brightness"] = rgb.has("brightness") ? rgb["brightness"].as_int(160) : 160;
    out["speed"] = 5;
    out["power_led"] = !(st.has("power_led") && st["power_led"].is_bool() && !st["power_led"].as_bool());
    out["available"] = !glob("/sys/class/leds/*joysticks*").empty();
    out["daemon"] = run({"systemctl", "is-active", "--quiet", "pbosd"}).code == 0;
    return out;
}

void pbosd_set(const Value& changes) {
    // Only the keys given: pbosd, pbosctl and the other tabs share the file.
    Value st = read_json(PBOSD_STATE);
    st.update(changes);
    write_json(PBOSD_STATE, st);
    run({"systemctl", "kill", "-s", "HUP", "pbosd.service"});
}

void pbosd_lights_set(bool on, const std::string& effect, const std::string& color, long long brightness) {
    Value old = read_json(PBOSD_STATE)["rgb"];
    std::string mode = pbosd_mode(effect);
    Value rgb = Value::object();
    rgb["mode"] = on ? mode : "off";
    rgb["color"] = color;
    rgb["brightness"] = brightness;
    if (!on) {
        rgb["last_mode"] = mode;
        rgb["last_color"] = color;
    } else {
        for (const char* k : {"last_mode", "last_color"})
            if (old.has(k)) rgb[k] = old[k];
    }
    Value changes = Value::object();
    changes["rgb"] = rgb;
    pbosd_set(changes);
}

Value multicolor_lights() {
    Value st = read_json(LIGHTS_STATE);
    // Before effects there was a mode: static or off.
    bool on = st.has("on") ? st["on"].truthy() : st["mode"].str() != "off";
    bool audio = st["experimental_audio"].truthy();
    std::string effect = st.has("effect") ? st["effect"].str() : DEFAULT_EFFECT;
    if (!effect_known(multicolor_effects(audio), effect)) effect = "static";
    auto clamp = [](long long v, long long lo, long long hi) { return std::max(lo, std::min(hi, v)); };
    Value out = Value::object();
    out["on"] = on;
    out["effect"] = effect;
    out["color"] = clean_color(st.has("color") ? st["color"] : Value(DEFAULT_COLOR));
    out["color2"] = clean_color(st.has("color2") ? st["color2"] : Value(DEFAULT_COLOR2));
    out["experimental_audio"] = audio;
    out["effect_before_audio"] = st.has("effect_before_audio") ? st["effect_before_audio"] : Value("static");
    out["brightness"] = clamp(st.has("brightness") ? st["brightness"].as_int(160) : 160, 0, 255);
    out["speed"] = clamp(st.has("speed") ? st["speed"].as_int(5) : 5, 1, 10);
    out["reverse_left"] = st["reverse_left"].truthy();
    out["reverse_right"] = st["reverse_right"].truthy();
    out["available"] = led_count() > 0;
    return out;
}

void start_engine() {
    // (Re)started, so it runs this image's code.
    run({"systemctl", "stop", ENGINE_UNIT + ".service"});
    run({"systemctl", "reset-failed", ENGINE_UNIT + ".service"});
    RunOpts o;
    o.env = clean_env();
    auto r = run({"systemd-run", "--unit=" + ENGINE_UNIT, "--collect", "-p", "Restart=on-failure", "-p",
                  "RestartSec=2", ENGINE, "--lights"},
                 o);
    if (r.code != 0) log("tools: light engine: %s", trim(r.err).c_str());
}

void stop_engine() { run({"systemctl", "stop", ENGINE_UNIT + ".service"}); }

void nudge_engine() {
    // Show the saved settings now.
    if (!unit_active(ENGINE_UNIT)) {
        start_engine();
        return;
    }
    run({"systemctl", "kill", "-s", "HUP", ENGINE_UNIT + ".service"});
}

// ------------------------------------------------------------------ update --
Value update_job() {
    // What the last download/staging run printed.
    Value st = *Value::parse(
        R"({"step": "", "have": 0, "total": 0, "dl_have": 0, "dl_total": 0, "error": "", "staged": false, "note": "", "drive_done": false})");
    for (const auto& line : split_lines(rd(LOG))) {
        if (line.rfind("PROGRESS ", 0) == 0) {
            std::vector<std::string> parts;
            size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) ++i;
                size_t j = i;
                while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) ++j;
                if (j > i) parts.push_back(line.substr(i, j - i));
                i = j;
            }
            if (parts.size() == 3) {
                st["have"] = std::atoll(parts[1].c_str());
                st["total"] = std::atoll(parts[2].c_str());
                if (st["step"].as_string().rfind("Downloading", 0) == 0) {  // what a resumed download continues from
                    st["dl_have"] = st["have"];
                    st["dl_total"] = st["total"];
                }
            }
        } else if (line.rfind("STEP ", 0) == 0) {
            // Each step (download, comparing, unpacking, backing up) has its own bar.
            st["step"] = line.substr(5);
            st["have"] = 0;
            st["total"] = 0;
        } else if (line.rfind("ERROR: ", 0) == 0) {
            st["error"] = line.substr(7);
        } else if (line.rfind("Update staged", 0) == 0) {
            st["staged"] = true;
        } else if (line == "DRIVE_DONE") {
            st["drive_done"] = true;
        } else if (line.rfind("pb-os ", 0) == 0 && line.size() >= 20 &&
                   line.compare(line.size() - 20, 20, "is already installed") == 0) {
            st["note"] = line;
        }
    }
    return st;
}

Value ask_updater(const std::string& command, double timeout) {
    // The updater runs on the system's Python with a clean environment.
    RunOpts o;
    o.env = clean_env();
    o.timeout = timeout;
    auto r = run({"/usr/bin/python3", UPDATER, command}, o);
    Value out = Value::object();
    if (r.code != 0) {
        out["ok"] = false;
        out["error"] = r.timed_out ? "check failed (timed out)"
                                   : remove_prefix(last_line(r.err.empty() ? r.out : r.err, "check failed"), "ERROR: ");
        out["time"] = epoch();
        return out;
    }
    out["ok"] = true;
    out.update(parse_or(r.out, Value::object()));
    out["time"] = epoch();
    return out;
}

void cleanup() {
    // What finished updates (from any version) left on HOME; the updater
    // skips it while an update is being prepared.
    RunOpts o;
    o.env = clean_env();
    o.timeout = 600;
    auto r = run({"/usr/bin/python3", UPDATER, "cleanup"}, o);
    auto out = trim(r.out + r.err);
    if (!out.empty()) log("tools: %s", out.c_str());
}

bool update_file(const std::string& name) {
    return name.rfind("pb-os-", 0) == 0 || name.rfind("SHA256SUMS", 0) == 0;
}

// ----------------------------------------------------------------- install --
struct Step {
    std::regex pattern;
    const char* label;
};

const std::vector<Step>& steps_for(const std::string& direction) {
    static const std::vector<Step> internal = {
        {std::regex(R"(\[ufs\] .*: internal disk)"), "Checking the device and internal storage"},
        {std::regex(R"(\[ufs\] repartitioning)"), "Repartitioning internal storage"},
        {std::regex(R"(\[ufs\] formatting)"), "Formatting the new partitions"},
        {std::regex(R"(\[ufs\] copying the system)"), "Copying the system"},
        {std::regex(R"(\[ufs\] copying .*/home)"), "Copying the home folder"},
        {std::regex(R"(\[ufs\] installing KERNEL)"), "Installing the boot kernel"},
        {std::regex(R"(\[ufs\] checking the result)"), "Checking the result"},
    };
    static const std::vector<Step> sd = {
        {std::regex(R"(\[ufs\] .*: microSD card)"), "Checking the device and the card"},
        {std::regex(R"(\[ufs\] preparing the card)"), "Unmounting the card"},
        {std::regex(R"(\[ufs\] partitioning the card)"), "Partitioning the card"},
        {std::regex(R"(\[ufs\] formatting)"), "Formatting the card"},
        {std::regex(R"(\[ufs\] copying the system)"), "Copying the system"},
        {std::regex(R"(\[ufs\] copying .*/home)"), "Copying the home folder"},
        {std::regex(R"(\[ufs\] installing KERNEL)"), "Installing the boot kernel"},
        {std::regex(R"(\[ufs\] checking the result)"), "Checking the result"},
    };
    static const std::vector<Step> none;
    if (direction == "internal") return internal;
    if (direction == "sd") return sd;
    return none;
}

std::string running_from() {
    // "sd" or "internal": the disk / is on.
    auto src = trim(run({"findmnt", "-no", "SOURCE", "/"}).out);
    auto out = run({"lsblk", "-no", "PKNAME", src}).out;
    auto disk = trim(out.substr(0, out.find_first_of(" \n\t")));
    return disk.rfind("mmcblk", 0) == 0 ? "sd" : "internal";
}

Value run_probe(const std::vector<std::string>& argv) {
    RunOpts o;
    o.env = clean_env();
    o.timeout = 60;
    auto r = run(argv, o);
    if (r.timed_out) {
        Value v = Value::object();
        v["ERROR"] = "the check took too long";
        return v;
    }
    Value out = keyvals(r.out);
    if (r.code != 0 && !out.has("ERROR")) {
        std::string err = strip_ansi(last_line(r.err.empty() ? r.out : r.err, "exit " + std::to_string(r.code)));
        auto at = err.find("ERROR:");
        out["ERROR"] = trim(at == std::string::npos ? err : err.substr(at + 6));
    }
    return out;
}

Value home_sizes() {
    RunOpts o;
    o.env = clean_env();
    o.timeout = 900;
    Value kv = keyvals(run({"/usr/bin/bash", HOME_COPY, "sizes"}, o).out);
    long long total = kv["HOME_ALL_BYTES"].as_int(0);
    long long games = kv["HOME_GAMES_BYTES"].as_int(0);
    Value out = Value::object();
    out["all"] = total;
    out["essentials"] = std::max(0LL, total - games);
    out["none"] = std::max(0LL, total - games);
    out["time"] = epoch();
    return out;
}

// Follows MOVE_LOG as it grows: rsync's progress lines arrive by the
// thousand, so each read starts where the last stopped.
class MoveLog {
public:
    std::mutex lock;

    void reset() {
        ino_ = 0;
        pos_ = 0;
        buf_.clear();
        step_ = 0;
        pct_ = 0;
        error_.clear();
        exit_ = Value();
        lines_.clear();
    }

    Value read(const std::string& direction) {
        std::lock_guard<std::mutex> g(lock);
        struct stat st;
        if (stat(MOVE_LOG.c_str(), &st) != 0) {
            reset();
            return state();
        }
        if (st.st_ino != ino_ || static_cast<size_t>(st.st_size) < pos_) {
            reset();
            ino_ = st.st_ino;
        }
        if (static_cast<size_t>(st.st_size) > pos_) {
            auto data = read_file(MOVE_LOG).value_or("");
            if (data.size() > pos_) {
                buf_ += data.substr(pos_);
                pos_ = data.size();
                parse(direction);
            }
        }
        return state();
    }

private:
    void parse(const std::string& direction) {
        static const std::regex percent(R"(\s(\d{1,3})%\s)");
        const auto& steps = steps_for(direction);
        size_t start = 0;
        std::vector<std::string> done;
        for (size_t i = 0; i < buf_.size(); ++i) {
            if (buf_[i] == '\r' || buf_[i] == '\n') {
                done.push_back(buf_.substr(start, i - start));
                start = i + 1;
            }
        }
        buf_.erase(0, start);
        for (auto seg : done) {
            seg = strip_ansi(seg);
            std::smatch m;
            std::string padded = " " + seg + " ";
            if (std::regex_search(padded, m, percent) && seg.find("[ufs]") == std::string::npos) {
                pct_ = std::min(std::atoi(m[1].str().c_str()), 100);
                continue;
            }
            for (size_t i = 0; i < steps.size(); ++i) {
                if (static_cast<int>(i + 1) > step_ && std::regex_search(seg, steps[i].pattern)) {
                    step_ = static_cast<int>(i + 1);
                    pct_ = 0;
                }
            }
            auto err = seg.find("ERROR:");
            if (err != std::string::npos) error_ = trim(seg.substr(err + 6));
            if (seg.rfind("PBOS_EXIT=", 0) == 0) {
                std::string n = seg.substr(10);
                exit_ = n.empty() ? 1 : std::atoi(n.c_str());
            } else if (!trim(seg).empty()) {
                lines_.push_back(seg);
                if (lines_.size() > 6) lines_.erase(lines_.begin());
            }
        }
    }

    Value state() const {
        Value out = Value::object();
        out["step"] = step_;
        out["pct"] = pct_;
        out["error"] = error_;
        out["exit"] = exit_;
        Value lines = Value::array();
        for (auto& l : lines_) lines.push(l);
        out["lines"] = lines;
        return out;
    }

    ino_t ino_ = 0;
    size_t pos_ = 0;
    std::string buf_;
    int step_ = 0;
    int pct_ = 0;
    std::string error_;
    Value exit_;
    std::vector<std::string> lines_;
};

// ------------------------------------------------- performance, hardware --
const char* const PROFILES[] = {"lowpower", "balanced"};
const char* const BUTTON_ACTIONS[] = {"profile-next", "rgb-next", "sticks-toggle", "none"};

template <size_t N>
bool one_of(const std::string& s, const char* const (&list)[N]) {
    for (const char* x : list)
        if (s == x) return true;
    return false;
}

Value control_state() {
    Value st = read_json(PBOSD_STATE);
    std::string profile = st["profile"].str();
    if (profile == "silent") profile = "lowpower";
    if (profile == "turbo") profile = "balanced";
    Value fan = st["fan"].is_object() ? st["fan"] : Value::object();
    Value buttons = *Value::parse(R"({"F13": "rgb-next", "F14": "profile-next"})");
    if (st["buttons"].is_object()) buttons.update(st["buttons"]);
    std::string mode = st["buttons_mode"].str();
    Value out = Value::object();
    out["profile"] = one_of(profile, PROFILES) ? profile : "balanced";
    Value f = Value::object();
    f["mode"] = fan["mode"].str() == "fixed" ? "fixed" : "auto";
    f["fixed"] = fan.has("fixed") ? fan["fixed"].as_int(50) : 50;
    out["fan"] = f;
    out["buttons"] = buttons;
    out["buttons_mode"] = (mode == "steam" || mode == "system") ? mode : "steam";
    return out;
}

Value telemetry() {
    Value out = *Value::parse(R"({"fan_rpm": null, "fan_pwm": null, "gpu_mhz": null, "temp_c": null})");
    for (const auto& d : glob("/sys/class/hwmon/hwmon*")) {
        if (rd(d + "/name") == "pwmfan") {
            out["fan_rpm"] = Value(rd(d + "/fan1_input", "0")).as_int(0);
            out["fan_pwm"] = Value(rd(d + "/pwm1", "0")).as_int(0);
        }
    }
    for (const auto& d : glob("/sys/class/devfreq/*gpu*")) {
        auto cur = rd(d + "/cur_freq");
        if (is_digits(cur)) out["gpu_mhz"] = std::atoll(cur.c_str()) / 1000000;
    }
    double best = NAN;
    for (const auto& z : glob("/sys/class/thermal/thermal_zone*")) {
        auto type = rd(z + "/type");
        if (type.rfind("cpu", 0) != 0 && type.rfind("gpu", 0) != 0) continue;
        auto v = rd(z + "/temp");
        std::string digits = v.size() && v[0] == '-' ? v.substr(1) : v;
        if (!is_digits(digits)) continue;
        double t = std::atoll(v.c_str()) / 1000.0;
        if (std::isnan(best) || t > best) best = t;
    }
    if (!std::isnan(best)) out["temp_c"] = std::round(best * 10) / 10;
    return out;
}

void set_mcu_link(bool enabled) {
    if (enabled) {
        remove_file(MCU_BLACKLIST);
        run({"modprobe", "konkr_sysbtn"});
    } else {
        write_text(MCU_BLACKLIST, "# Pocket FIT MCU UART driver — opt-in (pbosctl mcu enable)\nblacklist konkr_sysbtn\n");
        run({"modprobe", "-r", "konkr_sysbtn"});
    }
    run({"systemctl", "restart", "inputplumber.service"});
}

Value unsupported() {
    Value v = Value::object();
    v["supported"] = false;
    return v;
}

// A helper that answers with JSON on its standard output.
Value json_tool(std::vector<std::string> argv, const std::vector<std::string>& args, double timeout, const char* what) {
    argv.insert(argv.end(), args.begin(), args.end());
    RunOpts o;
    o.env = clean_env();
    o.timeout = timeout;
    auto r = run(argv, o);
    auto v = Value::parse(trim(r.out));
    if (!v) {
        log("tools: %s %s: %s", what, args.empty() ? "" : args[0].c_str(), trim(r.err).c_str());
        return Value();
    }
    return *v;
}

// The same for a helper on the system's Python.
Value python_json(const std::string& script, const std::vector<std::string>& args, double timeout, const char* what) {
    return json_tool({"/usr/bin/python3", script}, args, timeout, what);
}

Value charge_rest(const std::vector<std::string>& args) {
    if (!can_exec(CHARGE_REST)) return unsupported();
    // pbos-platformd's charge-rest, through the script's old path.
    Value out = json_tool({CHARGE_REST}, args, 20, "charge rest");
    if (!out.is_object()) return unsupported();
    Value res = Value::object();
    res["supported"] = out["lights"].truthy();
    res.update(out);
    return res;
}

Value charge_control(const std::vector<std::string>& args) {
    if (!can_exec(CHARGE_CONTROL)) return unsupported();
    Value out = python_json(CHARGE_CONTROL, args, 20, "charge control");
    return out.is_null() ? unsupported() : out;
}

bool charge_preview() {
    run({"systemctl", "stop", PREVIEW_UNIT + ".service"});
    run({"systemctl", "reset-failed", PREVIEW_UNIT + ".service"});
    RunOpts o;
    o.env = clean_env();
    return run({"systemd-run", "--unit=" + PREVIEW_UNIT, "--collect", CHARGE_REST, "preview"}, o)
               .code == 0;
}

Value face_buttons(const std::vector<std::string>& args) {
    // sm8550-face-buttons get / set nintendo|xbox.
    if (!can_exec(FACE_BUTTONS)) return unsupported();
    std::vector<std::string> argv = {FACE_BUTTONS};
    argv.insert(argv.end(), args.begin(), args.end());
    RunOpts o;
    o.env = clean_env();
    auto r = run(argv, o);
    auto v = Value::parse(last_line(r.out, ""));
    if (!v) {
        log("tools: face buttons %s: %s", args.empty() ? "" : args[0].c_str(), trim(r.err).c_str());
        return unsupported();
    }
    return *v;
}

Value stickcal(const std::vector<std::string>& args, const std::optional<std::string>& input = std::nullopt,
               double timeout = 30) {
    if (!can_read(STICKCAL)) {
        return *Value::parse(R"({"ok": false, "supported": false, "error": "not on this device"})");
    }
    std::vector<std::string> argv = {"/usr/bin/python3", STICKCAL};
    argv.insert(argv.end(), args.begin(), args.end());
    RunOpts o;
    o.input = input;
    o.timeout = timeout;
    auto r = run(argv, o);
    Value res = Value::object();
    if (!r.started || r.timed_out) {
        res["ok"] = false;
        res["error"] = r.timed_out ? "timed out" : r.err;
        return res;
    }
    auto lines = split_lines(trim(r.out));
    if (!lines.empty()) {
        auto v = Value::parse(lines.back());
        if (v && v->is_object()) res = *v;
    }
    if (r.code != 0 || res.size() == 0) {
        if (!res.has("ok")) res["ok"] = false;
        if (!res.has("error")) res["error"] = trim(r.err).empty() ? "exit " + std::to_string(r.code) : trim(r.err);
    }
    return res;
}

Value barry_desktop(const std::vector<std::string>& args) {
    if (!can_exec(BARRY_SWITCH)) return unsupported();
    const User& u = desktop_user();
    std::string runtime = "/run/user/" + std::to_string(u.uid);
    std::vector<std::string> argv = {"runuser", "-u", u.name, "--", BARRY_SWITCH};
    argv.insert(argv.end(), args.begin(), args.end());
    RunOpts o;
    o.env = clean_env({"HOME=" + u.home, "XDG_RUNTIME_DIR=" + runtime,
                       "DBUS_SESSION_BUS_ADDRESS=unix:path=" + runtime + "/bus"});
    auto r = run(argv, o);
    auto v = Value::parse(last_line(r.out, ""));
    if (!v) {
        log("tools: barry desktop %s failed", args.empty() ? "" : args[0].c_str());
        return unsupported();
    }
    return *v;
}

// ------------------------------------------------------------------ mtp --
// MTP over the USB-C cable (pbos-mtp.service, uMTP-Responder): a PC browses
// and copies the user's files with nothing to install on either side. Off by
// default, like SSH, so the switch enables the unit as well as starting it
// and the choice survives a reboot. The PC always sees Home and Steam.
Value mtp_state() {
    Value out = Value::object();
    const auto udcs = glob("/sys/class/udc/*");
    out["available"] = can_exec(PBOS_MTP) && !udcs.empty();
    out["enabled"] = unit_active(MTP_UNIT);
    // "configured" means a host has the gadget: the cable is in and the PC
    // has accepted it, which is the only part the user cannot see.
    bool connected = false;
    for (const auto& udc : udcs) {
        if (trim(rd(udc + "/state")) == "configured") connected = true;
    }
    out["connected"] = connected;
    return out;
}

Value set_mtp(bool enabled) {
    // mtp_state() already says available: false when the helper is missing,
    // so the tab gets the same shape either way.
    if (!can_exec(PBOS_MTP)) return mtp_state();
    log("tools: mtp over usb -> %s", enabled ? "on" : "off");
    if (enabled) {
        run({"systemctl", "enable", "--now", MTP_UNIT + ".service"});
    } else {
        run({"systemctl", "disable", "--now", MTP_UNIT + ".service"});
        run({"systemctl", "reset-failed", MTP_UNIT + ".service"});
    }
    return mtp_state();
}

// --------------------------------------------------------------- proton --
Value proton_state() {
    Value out = Value::object();
    out["tools"] = Value::object();
    if (!can_exec(PBOS_PROTON)) return out;
    const User& u = desktop_user();
    RunOpts o;
    o.env = clean_env({"HOME=" + u.home});
    auto r = run({"runuser", "-u", u.name, "--", "/usr/bin/python3", PBOS_PROTON, "status", "--json"}, o);
    auto st = Value::parse(trim(r.out));
    if (!st) return out;
    // Valve tool name -> PB-OS twin, for twins that are built and patched.
    for (const auto& [name, s] : st->members()) {
        if (s.is_object() && s["tool"].truthy() && s["patched"].truthy() && s["valve_name"].truthy())
            out["tools"][s["valve_name"].str()] = name;
    }
    return out;
}

std::string pbos_config(const std::string& file) { return desktop_user().home + "/.config/pb-os/" + file; }

Value proton_games_read() {
    Value st = read_json(pbos_config("proton-games.json"));
    Value out = Value::object();
    out["switched"] = st.has("switched") ? st["switched"] : Value::object();
    out["hands_off"] = st.has("hands_off") ? st["hands_off"] : Value::array();
    out["global_set"] = st["global_set"].truthy() ? st["global_set"] : Value("");
    return out;
}

void chown_config(const std::string& path) {
    const User& u = desktop_user();
    run({"chown", "-R", u.name + ":" + u.name, dirname(path)});
}

void proton_games_save(const Value& state) {
    // A recorded global check stays recorded, whatever the frontend sends.
    Value old = proton_games_read();
    Value st = Value::object();
    st["switched"] = state.has("switched") ? state["switched"] : Value::object();
    st["hands_off"] = state.has("hands_off") ? state["hands_off"] : Value::array();
    st["global_set"] = old["global_set"].truthy() ? old["global_set"].str()
                                                  : (state["global_set"].truthy() ? state["global_set"].str() : "");
    std::string path = pbos_config("proton-games.json");
    makedirs(dirname(path));
    write_json(path, st);
    chown_config(path);
}

// ---------------------------------------------------------------- games --
Value game_info() {
    // The wrapper knows the presets and this device's core groups.
    if (!can_exec(GAME_LAUNCH)) return Value::object();
    RunOpts o;
    o.env = clean_env();
    o.timeout = 15;
    auto v = Value::parse(trim(run({"/usr/bin/python3", GAME_LAUNCH, "--info"}, o).out));
    return v && v->is_object() ? *v : Value::object();
}

Value game_profiles() {
    Value info = game_info();
    Value games = read_json(pbos_config("game-profiles.json"))["games"];
    Value out = info;
    out["available"] = info.size() > 0;
    out["games"] = games.is_object() ? games : Value::object();
    return out;
}

Value community_hint(const std::string& appid) {
    // ProtonDB's reports for one game (tools/protondb-hints.py), read on
    // each call: the page asks once per game picked, nothing stays in memory.
    Value out = Value::object();
    out["available"] = false;
    if (!is_digits(appid)) return out;
    const Value doc = read_json(COMMUNITY_HINTS);
    if (!doc["games"].is_object()) return out;
    out["available"] = true;
    out["since"] = doc["reports_since"];
    out["until"] = doc["reports_until"];
    if (doc["games"].has(appid)) out["game"] = doc["games"][appid];
    return out;
}

Value clean_game_profile(const Value& p, const Value& info) {
    // Known settings only, and none at their defaults (an empty profile
    // means no profile).
    static const std::regex env_name("^[A-Za-z_][A-Za-z0-9_]*$");
    static const std::regex cpulist(R"(^\d+(-\d+)?(,\d+(-\d+)?)*$)");
    const Value& doc = info["presets"];
    const Value& presets = doc["presets"];
    std::vector<std::string> options;
    for (const auto& o : doc["options"].items())
        if (o.is_object()) options.push_back(o["key"].str());
    Value out = Value::object();
    std::string fex = p["fex"].str();
    if (fex == "custom" && p["fexConfig"].is_object()) {
        out["fex"] = "custom";
        Value cfg = Value::object();
        for (const auto& [k, v] : p["fexConfig"].members())
            if (std::find(options.begin(), options.end(), k) != options.end()) cfg[k] = v.str() == "1" ? "1" : "0";
        out["fexConfig"] = cfg;
    } else if (!fex.empty() && presets.has(fex) && fex != "default") {
        out["fex"] = fex;
    }
    std::string cores = p["cores"].truthy() ? p["cores"].str() : "";
    cores.erase(std::remove(cores.begin(), cores.end(), ' '), cores.end());
    if (!cores.empty() && (info["cores"].has(cores) || std::regex_match(cores, cpulist))) {
        out["cores"] = cores;
        if (p["wineTopology"].is_bool() && !p["wineTopology"].as_bool()) out["wineTopology"] = false;
    }
    if (p["env"].is_object()) {
        Value clean = Value::object();
        for (const auto& [k, v] : p["env"].members()) {
            std::string val = v.str();
            if (std::regex_match(k, env_name) && val.find('\0') == std::string::npos) clean[k] = val;
        }
        if (clean.size()) out["env"] = clean;
    }
    if (p["args"].is_array()) {
        // Game arguments after the command line, like -dx11.
        Value clean = Value::array();
        for (const auto& v : p["args"].items()) {
            if (!v.is_string()) continue;
            const std::string& val = v.as_string();
            if (!val.empty() && val.size() <= 200 && val.find('\0') == std::string::npos && clean.size() < 20) clean.push(val);
        }
        if (clean.size()) out["args"] = clean;
    }
    std::string mode = p["mode"].str();
    if (one_of(mode, PROFILES)) out["mode"] = mode;
    if (out.size()) out["name"] = (p["name"].truthy() ? p["name"].str() : "").substr(0, 200);
    return out;
}

Value game_profile_save(const std::string& appid, const Value& profile) {
    if (!is_digits(appid)) return *Value::parse(R"({"ok": false, "error": "bad app id"})");
    std::string path = pbos_config("game-profiles.json");
    Value st = read_json(path);
    Value games = st["games"].is_object() ? st["games"] : Value::object();
    Value clean = profile.is_object() ? clean_game_profile(profile, game_info()) : Value::object();
    if (clean.size()) games[appid] = clean;
    else games.erase(appid);
    Value doc = Value::object();
    doc["games"] = games;
    makedirs(dirname(path));
    write_json(path, doc);
    chown_config(path);
    Value out = Value::object();
    out["ok"] = true;
    out["profile"] = clean.size() ? clean : Value();
    return out;
}

// ---------------------------------------------------------- performance --
// Full Power ("balanced") has no caps. Low Power keeps the GPU at most at the step nearest
// 70 % of its top and the CPU cores at most at ~2.0 GHz (pbosd's Low Power
// has the same CPU cap; the little cores' top is below it). The default
// mode is kept here; a game's own is "mode" in its profile.
const std::string PERF_STATE = "/var/lib/pbos-qam/performance.json";
constexpr long long LOW_CPU_KHZ = 2035200;
constexpr double LOW_GPU_SHARE = 0.7;

std::vector<long long> sorted_numbers(const std::string& text) {
    std::vector<long long> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find_first_of(" \n", i);
        if (j == std::string::npos) j = text.size();
        std::string w = text.substr(i, j - i);
        if (is_digits(w)) out.push_back(std::atoll(w.c_str()));
        i = j + 1;
    }
    std::sort(out.begin(), out.end());
    return out;
}

// The highest step at or below limit (the lowest step when none is).
long long step_at_most(const std::vector<long long>& steps, double limit) {
    long long best = steps.front();
    for (long long f : steps)
        if (f <= limit) best = f;
    return best;
}

std::string gpu_devfreq() {
    for (const auto& d : glob("/sys/class/devfreq/*gpu*"))
        if (!sorted_numbers(rd(d + "/available_frequencies")).empty()) return d;
    return "";
}

// The caps each mode sets here, in MHz: {gpu, cpu}; 0 = none.
std::pair<long long, long long> low_caps_mhz() {
    long long gpu = 0, cpu = 0;
    std::string d = gpu_devfreq();
    if (!d.empty()) {
        auto f = sorted_numbers(rd(d + "/available_frequencies"));
        gpu = step_at_most(f, f.back() * LOW_GPU_SHARE) / 1000000;
    }
    for (const auto& pol : glob("/sys/devices/system/cpu/cpufreq/policy*")) {
        auto f = sorted_numbers(rd(pol + "/scaling_available_frequencies"));
        if (!f.empty() && f.back() > LOW_CPU_KHZ) cpu = std::max(cpu, step_at_most(f, LOW_CPU_KHZ) / 1000);
    }
    return {gpu, cpu};
}

void apply_clock_caps(bool low) {
    std::string d = gpu_devfreq();
    if (!d.empty()) {
        auto f = sorted_numbers(rd(d + "/available_frequencies"));
        long long cap = low ? step_at_most(f, f.back() * LOW_GPU_SHARE) : f.back();
        if (!write_sysfs(d + "/max_freq", std::to_string(cap))) log("tools: cannot cap the GPU at %lld", cap);
    }
    for (const auto& pol : glob("/sys/devices/system/cpu/cpufreq/policy*")) {
        // The listed steps leave out boost frequencies, which stay off.
        auto f = sorted_numbers(rd(pol + "/scaling_available_frequencies"));
        if (f.empty()) continue;
        long long cap = low && f.back() > LOW_CPU_KHZ ? step_at_most(f, LOW_CPU_KHZ) : f.back();
        if (!write_sysfs(pol + "/scaling_max_freq", std::to_string(cap)))
            log("tools: cannot cap %s at %lld", basename(pol).c_str(), cap);
    }
}

// ---------------------------------------------------------------- section --
class Tools : public Section {
public:
    std::string name() const override { return "tools"; }

    void start(Host& host) override {
        host_ = &host;
        Loop& loop = host.loop;
        Loop::work([this] {
            bool moving = unit_active(MOVE_UNIT);
            std::string kind = lights_kind();
            {
                std::lock_guard<std::mutex> g(lock_);
                move_was_running_ = moving;
                lights_ = kind;
                kind_known_ = true;
            }
            if (kind == "multicolor") start_engine();
            host_->loop.post([this, kind] {
                if (kind == "pbosd") watch_profile();
            });
            // The default mode's caps from boot on; pbosd keeps its own.
            if (kind != "pbosd") apply_mode();
        });
        // Look now and then; a new release gets one toast per version.
        loop.after(60, [this] { periodic_check(); });
        // Once per start, after Game Mode has settled.
        loop.after(30, [] { Loop::work(cleanup); });
        // Drives coming and going, and update files in Downloads.
        loop.on_uevent([this](const std::string& subsystem, const std::string& action) {
            if (subsystem != "block" || (action != "add" && action != "remove" && action != "change")) return;
            {
                std::lock_guard<std::mutex> g(lock_);
                card_ = Value();  // the Install tab looks again
            }
            drives_changed();
        });
        watch_downloads();
        // The end of a move, with Quick Access closed too.
        loop.on_unit_change(MOVE_UNIT, [this] { Loop::work([this] { move_changed(); }); });
    }

    void stop() override {
        stick_stop();
        if (lights_ == "multicolor") stop_engine();
        // Full clocks while nothing keeps the mode, a reboot included; the
        // default mode is set again at the next start.
        bool capped;
        {
            std::lock_guard<std::mutex> g(lock_);
            capped = lights_ != "pbosd" && active_ == "lowpower";
            active_.clear();
        }
        if (capped) {
            apply_clock_caps(false);
            log("tools: performance caps lifted (stopping)");
        }
    }

    Value call(const std::string& m, const Value& a) override {
        if (m == "check") return check();
        if (m == "set_channel") return set_channel(arg(a, 0, "prod").str(), arg(a, 1, "").str());
        if (m == "check_local") return check_local();
        if (m == "get_state") return get_state();
        if (m == "start") return run_update("update");
        if (m == "start_local") return run_update("local-update");
        if (m == "pause") return pause();
        if (m == "restart") return restart();
        if (m == "get_install") return get_install(arg(a, 0, false).truthy());
        if (m == "start_move")
            return start_move(arg(a, 0, "").str(), arg(a, 1, "all").str(), arg(a, 2, 0).as_int(0), arg(a, 3, "").str());
        if (m == "power_off") return power_off();
        if (m == "get_lights") return get_lights();
        if (m == "set_lights")
            return set_lights(arg(a, 0, true).truthy(), arg(a, 1, "static").str(), arg(a, 2, DEFAULT_COLOR),
                              arg(a, 3, 160).as_int(160), arg(a, 4, 5).as_int(5), arg(a, 5, false).truthy(),
                              arg(a, 6, false).truthy(), arg(a, 7, "").str());
        if (m == "set_audio_pulse") return set_audio_pulse(arg(a, 0, false).truthy());
        if (m == "get_charging") return charge_rest({"settings"});
        if (m == "set_charging") {
            const Value& c = a.at(0);
            return c.is_object() ? charge_rest({"save", c.dump()}) : charge_rest({"settings"});
        }
        if (m == "get_charge_control") return charge_control({"settings"});
        if (m == "set_charge_control") {
            const Value& c = a.at(0);
            return c.is_object() ? charge_control({"save", c.dump()}) : charge_control({"settings"});
        }
        if (m == "preview_charging") return charge_preview();
        if (m == "set_power_led") {
            if (kind() == "pbosd") {
                Value c = Value::object();
                c["power_led"] = arg(a, 0, true).truthy();
                pbosd_set(c);
            }
            return get_lights();
        }
        if (m == "get_control") return get_control();
        if (m == "set_profile") {
            std::string p = arg(a, 0, "balanced").str();
            if (kind() == "pbosd" && one_of(p, PROFILES)) set_pbosd("profile", p);
            return get_control();
        }
        if (m == "set_fan") {
            if (kind() == "pbosd") {
                Value fan = Value::object();
                fan["mode"] = arg(a, 0, "auto").str() == "fixed" ? "fixed" : "auto";
                fan["fixed"] = std::max(0LL, std::min(100LL, arg(a, 1, 50).as_int(50)));
                set_pbosd("fan", fan);
            }
            return get_control();
        }
        if (m == "set_button") {
            std::string key = arg(a, 0, "F13").str(), action = arg(a, 1, "none").str();
            if (kind() == "pbosd" && (key == "F13" || key == "F14") && one_of(action, BUTTON_ACTIONS)) {
                Value buttons = control_state()["buttons"];
                buttons[key] = action;
                set_pbosd("buttons", buttons);
            }
            return get_control();
        }
        if (m == "set_buttons_mode") {
            // "steam": Custom Function and K are trackpad clicks Steam can
            // remap; "system": they run the actions above.
            std::string mode = arg(a, 0, "steam").str();
            if (kind() == "pbosd" && (mode == "steam" || mode == "system")) set_pbosd("buttons_mode", mode);
            return get_control();
        }
        if (m == "set_mcu") {
            if (kind() == "pbosd") set_mcu_link(arg(a, 0, false).truthy());
            return get_control();
        }
        if (m == "get_face_buttons") return face_buttons({"get"});
        if (m == "set_face_buttons") {
            std::string layout = arg(a, 0, "nintendo").str();
            if (layout != "nintendo" && layout != "xbox") return face_buttons({"get"});
            log("tools: face buttons -> %s", layout.c_str());
            return face_buttons({"set", layout});
        }
        if (m == "get_mtp") return mtp_state();
        if (m == "set_mtp") return set_mtp(arg(a, 0, false).truthy());
        if (m == "get_barry_desktop") return barry_desktop({"get"});
        if (m == "set_barry_desktop") {
            bool on = arg(a, 0, true).truthy();
            log("tools: barry launcher in desktop mode -> %s", on ? "on" : "off");
            return barry_desktop({on ? "on" : "off"});
        }
        if (m == "stick_state") {
            if (!can_read(STICKCAL)) return unsupported();
            Value out = Value::object();
            out["supported"] = true;
            out.update(stickcal({"status"}));
            return out;
        }
        if (m == "stick_rest") return stick_rest();
        if (m == "stick_edges") return stick_edges();
        if (m == "stick_live_values") {
            std::lock_guard<std::mutex> g(stick_lock_);
            return stick_live_;
        }
        if (m == "stick_cancel") {
            stick_stop();
            std::lock_guard<std::mutex> g(lock_);
            stick_rest_ = Value();
            return Value();
        }
        if (m == "stick_save") return stick_save();
        if (m == "stick_switch") {
            std::string which = arg(a, 0, "").str();
            if (which != "calibration" && which != "fine") {
                Value e = Value::object();
                e["ok"] = false;
                e["error"] = "unknown switch " + which;
                return e;
            }
            bool on = arg(a, 1, false).truthy();
            Value res = stickcal({"set", which, on ? "on" : "off"});
            log("tools: sticks %s %s: %s", which.c_str(), on ? "on" : "off", res.dump().c_str());
            return res;
        }
        if (m == "stick_delete") {
            Value res = stickcal({"delete"});
            log("tools: stick calibration deleted: %s", res.dump().c_str());
            return res;
        }
        if (m == "get_game_profiles") return game_profiles();
        if (m == "get_community_hint") return community_hint(arg(a, 0, "").str());
        if (m == "set_game_profile") {
            const Value res = game_profile_save(arg(a, 0, "").str(), a.at(1));
            log("tools: game profile %s: %s", arg(a, 0, "").str().c_str(), res["profile"].dump().c_str());
            if (arg(a, 0, "").str() == running()) apply_mode();
            return res;
        }
        if (m == "get_performance") return performance();
        if (m == "set_performance_default") {
            std::string mode = arg(a, 0, "balanced").str();
            if (one_of(mode, PROFILES)) {
                Value st = read_json(PERF_STATE);
                st["default"] = mode;
                write_json(PERF_STATE, st);
                apply_mode();
            }
            return performance();
        }
        if (m == "game_running") {
            // The game Steam runs now ("" = none), from the frontend.
            std::string appid = arg(a, 0, "").str();
            if (!appid.empty() && !is_digits(appid)) appid.clear();
            {
                std::lock_guard<std::mutex> g(lock_);
                if (appid == running_) return true;
                running_ = appid;
            }
            apply_mode();
            return true;
        }
        if (m == "get_proton") return proton_state();
        if (m == "get_proton_games") return proton_games_read();
        if (m == "set_proton_games") {
            if (a.at(0).is_object()) proton_games_save(a.at(0));
            std::string note = arg(a, 1, "").str();
            if (!note.empty()) log("tools: Proton PB-OS: %s", note.c_str());
            return true;
        }
        throw CallError("PB-OS Tools has no method " + m);
    }

private:
    std::string kind() {
        // Calls can come before start()'s look at the device has finished.
        {
            std::lock_guard<std::mutex> g(lock_);
            if (kind_known_) return lights_;
        }
        std::string k = lights_kind();
        std::lock_guard<std::mutex> g(lock_);
        lights_ = k;
        kind_known_ = true;
        return k;
    }

    void emit(const char* event, Value args) {
        if (host_) host_->emit(name(), event, std::move(args));  // none for pbos-qamd --call
    }

    void set_pbosd(const char* key, Value v) {
        Value c = Value::object();
        c[key] = std::move(v);
        pbosd_set(c);
    }

    // -------------------------------------------------------------- update --
    void periodic_check() {
        Loop::work([this] {
            Value res = check();
            if (!res["ok"].truthy()) log("tools: update check failed: %s", res["error"].str().c_str());
        });
        host_->loop.after(CHECK_EVERY, [this] { periodic_check(); });
    }

    Value check() {
        Value last = ask_updater("check", 90);
        const Value& up = std::as_const(last)["update"];
        bool toast = false;
        {
            std::lock_guard<std::mutex> g(lock_);
            last_ = last;
            if (last["available"].truthy() && up.is_object() && up["version"].str() != announced_ && !exists(PENDING)) {
                announced_ = up["version"].str();
                toast = true;
            }
        }
        if (toast) emit("pbos_update_available", Value(Value::Array{up["title"]}));
        return last;
    }

    Value set_channel(const std::string& channel, const std::string& passphrase) {
        // Dev updates on (dev, needs a tester's passphrase) or off (prod),
        // then look again. The passphrase goes to the updater on stdin,
        // never on its command line.
        Value out = Value::object();
        if (channel != "dev" && channel != "prod") {
            out["ok"] = false;
            out["error"] = "unknown channel " + channel;
            return out;
        }
        RunOpts o;
        o.env = clean_env();
        o.timeout = 60;
        o.input = passphrase + "\n";
        auto r = run({"/usr/bin/python3", UPDATER, "channel", channel}, o);
        if (r.code != 0) {
            out["ok"] = false;
            out["error"] = remove_prefix(last_line(r.err.empty() ? r.out : r.err, "failed"), "ERROR: ");
            return out;
        }
        {
            std::lock_guard<std::mutex> g(lock_);
            announced_.clear();
        }
        out["ok"] = true;
        out["release"] = check();
        return out;
    }

    Value check_local() {
        if (unit_active(UNIT)) {
            std::lock_guard<std::mutex> g(lock_);
            return local_;  // it may be copying from the drive right now
        }
        Value local = ask_updater("local-check", 60);
        const Value& up = std::as_const(local)["update"];
        std::string key = up.is_object() ? up["version"].str() + " " + up["drive"].str() : "";
        bool toast = false;
        {
            std::lock_guard<std::mutex> g(lock_);
            local_ = local;
            toast = up.is_object() && key != announced_local_ && !exists(PENDING);
            announced_local_ = key;
        }
        if (toast) {
            std::string place = up["place"].truthy() ? up["place"].str() : "on the " + up["where"].str();
            emit("pbos_local_available", Value(Value::Array{up["version"], place}));
        }
        return local;
    }

    void drives_changed() {
        // Several events come for one drive: one look, once it has settled.
        if (drive_timer_) host_->loop.cancel(drive_timer_);
        drive_timer_ = host_->loop.after(DRIVE_SETTLE, [this] {
            drive_timer_ = 0;
            Loop::work([this] {
                if (!unit_active(MOVE_UNIT)) {
                    Value r = check_local();
                    if (!r["ok"].truthy() && r.has("error")) log("tools: drive check failed: %s", r["error"].str().c_str());
                }
            });
        });
    }

    void watch_downloads() {
        std::string downloads = desktop_user().home + "/Downloads";
        if (downloads_watch_) host_->loop.unwatch(downloads_watch_);
        downloads_watch_ = host_->loop.watch(
            downloads, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE_SELF,
            [this](uint32_t mask, const std::string& file) {
                if (mask & (IN_IGNORED | IN_DELETE_SELF)) {
                    downloads_watch_ = 0;
                    watch_home_for_downloads();
                    return;
                }
                if (update_file(file)) drives_changed();
            });
        if (!downloads_watch_) watch_home_for_downloads();
    }

    void watch_home_for_downloads() {
        // No Downloads folder yet: wait for it to be made.
        if (home_watch_) return;
        home_watch_ = host_->loop.watch(desktop_user().home, IN_CREATE | IN_MOVED_TO, [this](uint32_t, const std::string& n) {
            if (n != "Downloads") return;
            host_->loop.unwatch(home_watch_);
            home_watch_ = 0;
            watch_downloads();
        });
    }

    Value get_state() {
        Value out = Value::object();
        std::string installed = rd(VERSION);
        out["installed"] = installed.empty() ? "unknown" : installed;
        {
            std::lock_guard<std::mutex> g(lock_);
            out["release"] = last_.is_null() ? Value::object() : last_;
            out["local"] = local_.is_null() ? Value::object() : local_;
        }
        out["running"] = unit_active(UNIT);
        out["pending"] = exists(PENDING);
        out["job"] = update_job();
        out["lights"] = kind();
        return out;
    }

    Value run_update(const std::string& command) {
        if (unit_active(UNIT) || exists(PENDING) || unit_active(MOVE_UNIT)) return false;
        remove_file(LOG);
        run({"systemctl", "reset-failed", UNIT + ".service"});
        auto r = run({"systemd-run", "--unit=" + UNIT, "--collect", "-p", "StandardOutput=file:" + LOG, "-p",
                      "StandardError=inherit", "/usr/bin/python3", "-u", UPDATER, command});
        return r.code == 0;
    }

    Value pause() {
        // Only while downloading: the download resumes on the next start.
        // Staging is not interrupted (it writes the boot image at the end).
        if (update_job()["step"].as_string().rfind("Downloading", 0) == 0) {
            run({"systemctl", "stop", UNIT + ".service"});
            return true;
        }
        return false;
    }

    Value restart() {
        // Staging writes the pending marker before the boot image: never
        // restart before it finishes.
        if (!exists(PENDING) || unit_active(UNIT)) return false;
        spawn({"systemctl", "reboot"});
        return true;
    }

    // ------------------------------------------------------------- install --
    void move_changed() {
        bool now = unit_active(MOVE_UNIT);
        bool was;
        {
            std::lock_guard<std::mutex> g(lock_);
            was = move_was_running_;
            move_was_running_ = now;
        }
        if (was && !now) {
            Value args = read_json(MOVE_ARGS);
            Value job = movelog_.read(args["direction"].str());
            {
                std::lock_guard<std::mutex> g(lock_);
                internal_ = Value();
                card_ = Value();
            }
            emit("pbos_move_done",
                        Value(Value::Array{args["direction"].str(), job["exit"].is_number() && job["exit"].as_int() == 0}));
        }
    }

    Value get_install(bool refresh) {
        std::string where = running_from();
        bool running = unit_active(MOVE_UNIT);
        const Value args = read_json(MOVE_ARGS);
        std::string direction = args["direction"].str();
        Value job = movelog_.read(direction);
        const auto& steps = steps_for(direction);
        long long step = job["step"].as_int();
        job["count"] = steps.size();
        job["label"] = step > 0 && step <= static_cast<long long>(steps.size()) ? steps[step - 1].label : "Starting";
        Value out = Value::object();
        out["from"] = where;
        out["running"] = running;
        out["args"] = args;
        out["job"] = job;
        {
            std::lock_guard<std::mutex> g(lock_);
            out["sizes"] = sizes_;
        }
        out["updating"] = unit_active(UNIT);
        if (running) return out;
        if (where == "sd") {
            Value internal;
            {
                std::lock_guard<std::mutex> g(lock_);
                internal = internal_;
            }
            if (refresh || internal.is_null()) {
                internal = run_probe({"/usr/bin/bash", PROBE_INTERNAL});
                std::lock_guard<std::mutex> g(lock_);
                internal_ = internal;
            }
            out["internal"] = internal;
        } else {
            Value card;
            {
                std::lock_guard<std::mutex> g(lock_);
                card = card_;
            }
            if (refresh || card.is_null()) {
                card = run_probe({"/usr/bin/bash", TO_SD, "--probe"});
                std::lock_guard<std::mutex> g(lock_);
                card_ = card;
            }
            out["card"] = card;
        }
        bool size_now = false;
        {
            std::lock_guard<std::mutex> g(lock_);
            bool stale = !sizes_.is_object() || epoch() - sizes_["time"].as_double(0) > SIZES_TTL;
            if ((refresh || stale) && !sizing_) {
                sizing_ = true;
                size_now = true;
            }
        }
        if (size_now) {
            Loop::work([this] {
                Value sizes = home_sizes();
                std::lock_guard<std::mutex> g(lock_);
                sizes_ = sizes;
                sizing_ = false;
            });
        }
        return out;
    }

    Value start_move(const std::string& direction, const std::string& home, long long android_gb,
                     const std::string& expect) {
        auto fail = [](const std::string& e) {
            Value v = Value::object();
            v["ok"] = false;
            v["error"] = e;
            return v;
        };
        if (unit_active(MOVE_UNIT)) return fail("a copy is already running");
        if (unit_active(UNIT)) return fail("an update is being prepared; wait until it is ready");
        if (home != "all" && home != "essentials" && home != "none") return fail("unknown home choice");
        std::string where = running_from();
        std::vector<std::string> argv;
        if (direction == "internal" && where == "sd") {
            argv = {"/usr/bin/bash", TO_INTERNAL, "--force", "--android-gb", std::to_string(android_gb),
                    "--expect", expect, "--home", home};
        } else if (direction == "sd" && where == "internal") {
            argv = {"/usr/bin/bash", TO_SD, "--force", "--expect", expect, "--home", home};
        } else {
            return fail("SteamOS moved since this was checked; check again");
        }
        remove_file(MOVE_LOG);
        remove_file(MOVE_ARGS);
        char stamp[32];
        time_t t = time(nullptr);
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
        std::string saved = "/home/steamos/pbos-move-" + std::string(stamp) + ".log";
        Value margs = Value::object();
        margs["direction"] = direction;
        margs["home"] = home;
        margs["android_gb"] = android_gb;
        margs["started"] = epoch();
        margs["saved_log"] = saved;
        write_json(MOVE_ARGS, margs);
        // The log goes to HOME afterwards too, for a report; sleep, the power
        // button and the lid are held off until the script ends.
        std::string wrapper =
            "systemd-inhibit --what=sleep:idle:shutdown:handle-power-key:handle-suspend-key:handle-lid-switch "
            "--who=\"PB-OS Tools\" --why=\"Copying SteamOS\" --mode=block \"$@\"; rc=$?; "
            "echo \"PBOS_EXIT=$rc\"; "
            "install -o 1000 -g 1000 -m 0644 " + MOVE_LOG + " " + saved + " 2>/dev/null; "
            "exit $rc";
        run({"systemctl", "reset-failed", MOVE_UNIT + ".service"});
        std::vector<std::string> cmd = {"systemd-run", "--unit=" + MOVE_UNIT, "--collect",
                                        "-p", "StandardOutput=file:" + MOVE_LOG, "-p", "StandardError=inherit",
                                        "-E", "PATH=/usr/sbin:/usr/bin:/sbin:/bin", "-E", "LANG=C.UTF-8",
                                        "/usr/bin/bash", "-c", wrapper, "pbos-utils-move"};
        cmd.insert(cmd.end(), argv.begin(), argv.end());
        auto r = run(cmd);
        if (r.code != 0) return fail(last_line(r.err.empty() ? "could not start" : r.err, "could not start"));
        {
            std::lock_guard<std::mutex> g(movelog_.lock);
            movelog_.reset();
        }
        std::lock_guard<std::mutex> g(lock_);
        move_was_running_ = true;
        Value ok = Value::object();
        ok["ok"] = true;
        return ok;
    }

    Value power_off() {
        if (unit_active(MOVE_UNIT)) return false;
        spawn({"systemctl", "poweroff"});
        return true;
    }

    // -------------------------------------------------------------- lights --
    Value get_lights() {
        std::string k = kind();
        Value out = Value::object();
        out["kind"] = k;
        if (k == "pbosd") {
            out["effects"] = effects_json("pbosd");
            out.update(pbosd_lights());
        } else if (k == "multicolor") {
            Value st = multicolor_lights();
            out["effects"] = multicolor_effects(st["experimental_audio"].truthy());
            out.update(st);
        }
        return out;
    }

    Value set_lights(bool on, std::string effect, const Value& color_in, long long brightness, long long speed,
                     bool reverse_left, bool reverse_right, const std::string& color2) {
        std::string k = kind();
        if (k != "pbosd" && k != "multicolor") {
            Value v = Value::object();
            v["kind"] = "";
            return v;
        }
        Value known = k == "multicolor" ? multicolor_effects(read_json(LIGHTS_STATE)["experimental_audio"].truthy())
                                        : effects_json("pbosd");
        if (!effect_known(known, effect)) effect = "static";
        std::string color = clean_color(color_in);
        brightness = std::max(0LL, std::min(255LL, brightness));
        speed = std::max(1LL, std::min(10LL, speed));
        if (k == "pbosd") {
            pbosd_lights_set(on, effect, color, brightness);
        } else {
            Value st = read_json(LIGHTS_STATE);  // keeps the Experimental switches
            st["on"] = on;
            st["effect"] = effect;
            st["color"] = color;
            st["brightness"] = brightness;
            st["speed"] = speed;
            st["reverse_left"] = reverse_left;
            st["reverse_right"] = reverse_right;
            if (!color2.empty()) st["color2"] = clean_color(color2);  // the audio effects' intensity color
            st.erase("mode");
            write_json(LIGHTS_STATE, st);
            nudge_engine();
        }
        return get_lights();
    }

    Value set_audio_pulse(bool enabled) {
        // The Experimental switch (Audio Effects): on adds Audio Meter and
        // Audio Spectrum to the effects and picks Audio Meter, keeping the
        // effect it replaced; off takes them away and, if one was showing,
        // brings that effect back.
        if (kind() != "multicolor") return get_lights();
        Value st = read_json(LIGHTS_STATE);
        std::string cur = st.has("effect") ? st["effect"].str() : DEFAULT_EFFECT;
        if (enabled) {
            if (!is_audio_mode(cur)) st["effect_before_audio"] = cur;
            st["experimental_audio"] = true;
            st["effect"] = "audio";
            st["on"] = true;
        } else {
            if (is_audio_mode(cur)) st["effect"] = st.has("effect_before_audio") ? st["effect_before_audio"] : Value("static");
            st["experimental_audio"] = false;
        }
        st.erase("mode");
        write_json(LIGHTS_STATE, st);
        nudge_engine();
        return get_lights();
    }

    // ------------------------------------------- performance, hardware --
    void watch_profile() {
        // The Performance button goes straight to pbosd, so the panel would
        // only see a change once opened: pbosd's state file is watched and
        // the frontend told, which shows a toast over whatever is running.
        profile_ = control_state()["profile"].str();
        int id = host_->loop.watch(dirname(PBOSD_STATE), IN_CLOSE_WRITE | IN_MOVED_TO, [this](uint32_t, const std::string& n) {
            if (n != basename(PBOSD_STATE)) return;
            std::string now = control_state()["profile"].str();
            if (now == profile_) return;
            log("tools: profile %s -> %s", profile_.c_str(), now.c_str());
            profile_ = now;
            emit("pbos_mode", Value(Value::Array{now}));
            // Not set by a game's mode (the button, pbosctl): the new default.
            bool ours;
            {
                std::lock_guard<std::mutex> g(lock_);
                ours = now == active_ && game_mode(running_) == now;
            }
            if (!ours) {
                Value st = read_json(PERF_STATE);
                st["default"] = now;
                write_json(PERF_STATE, st);
                std::lock_guard<std::mutex> g(lock_);
                active_ = now;
            }
        });
        if (!id) log("tools: cannot watch %s; profile toasts are off", PBOSD_STATE.c_str());
    }

    std::string running() {
        std::lock_guard<std::mutex> g(lock_);
        return running_;
    }

    // The mode a game's profile sets ("" = none, or no game).
    static std::string game_mode(const std::string& appid) {
        if (appid.empty()) return "";
        std::string mode = read_json(pbos_config("game-profiles.json"))["games"][appid]["mode"].str();
        return one_of(mode, PROFILES) ? mode : "";
    }

    std::string default_mode() {
        std::string mode = read_json(PERF_STATE)["default"].str();
        if (one_of(mode, PROFILES)) return mode;
        // pbosd devices start from pbosd's own profile.
        return kind() == "pbosd" ? control_state()["profile"].str() : "balanced";
    }

    // The running game's mode, or the default: pbosd's profile on its
    // devices (its fan curve goes with it), the clock caps everywhere else.
    void apply_mode() {
        std::string game = running();
        std::string mode = game_mode(game);
        if (mode.empty()) mode = default_mode();
        std::string was;
        {
            std::lock_guard<std::mutex> g(lock_);
            was = active_;
            active_ = mode;
        }
        if (kind() == "pbosd") {
            if (control_state()["profile"].str() != mode) set_pbosd("profile", mode);  // its watcher toasts
            return;
        }
        apply_clock_caps(mode == "lowpower");
        if (mode == was) return;
        log("tools: performance %s%s", mode.c_str(), game.empty() ? "" : (" for " + game).c_str());
        if (!was.empty()) emit("pbos_mode", Value(Value::Array{mode}));
    }

    Value performance() {
        Value out = Value::object();
        bool pbosd = kind() == "pbosd";
        out["kind"] = pbosd ? "pbosd" : "clocks";
        out["default"] = default_mode();
        {
            std::lock_guard<std::mutex> g(lock_);
            out["active"] = active_;
            out["game"] = running_;
        }
        if (!pbosd) {
            auto [gpu, cpu] = low_caps_mhz();
            std::string desc = "Lower clocks for longer play:";
            if (gpu) desc += " GPU up to " + std::to_string(gpu) + " MHz";
            if (cpu) desc += std::string(gpu ? "," : "") + " CPU cores up to " + std::to_string(cpu) + " MHz";
            out["lowpower"] = desc;
        }
        return out;
    }

    Value get_control() {
        if (kind() != "pbosd") {
            Value v = Value::object();
            v["available"] = false;
            return v;
        }
        Value out = Value::object();
        out["available"] = true;
        out.update(control_state());
        out["mcu_enabled"] = !exists(MCU_BLACKLIST);
        out["daemon"] = unit_active("pbosd");
        out.update(telemetry());
        return out;
    }

    // --------------------------------------------------------------- sticks --
    Value stick_rest() {
        // The sticks at rest; refuses if one was touched.
        const Value res = stickcal({"rest", std::to_string(STICK_REST_S)});
        if (!res["ok"].truthy()) return res;
        for (const auto& [n, ax] : res["axes"].members()) {
            if (ax["hi"].as_double() - ax["lo"].as_double() > STICK_REST_SPREAD) {
                return *Value::parse(R"({"ok": false, "error": "A stick moved. Let go of both sticks and try again."})");
            }
        }
        Value rest = Value::object();
        std::string note;
        for (const auto& [n, ax] : res["axes"].members()) {
            rest[n] = ax["mean"];
            char buf[64];
            std::snprintf(buf, sizeof buf, "%s%s %+.1f", note.empty() ? "" : ", ", n.c_str(), ax["mean"].as_double());
            note += buf;
        }
        {
            std::lock_guard<std::mutex> g(lock_);
            stick_rest_ = rest;
        }
        log("tools: sticks rest: %s", note.c_str());
        Value out = Value::object();
        out["ok"] = true;
        out["rest"] = rest;
        return out;
    }

    Value stick_edges() {
        stick_stop();
        {
            std::lock_guard<std::mutex> g(stick_lock_);
            stick_live_ = Value();
        }
        bool ok = edges_.start({"/usr/bin/python3", STICKCAL, "edges", std::to_string(STICK_EDGES_MAX_S)},
                               [this](const std::string& line) {
                                   auto v = Value::parse(line);
                                   if (!v) return;
                                   std::lock_guard<std::mutex> g(stick_lock_);
                                   stick_live_ = *v;
                               });
        Value out = Value::object();
        out["ok"] = ok;
        if (!ok) out["error"] = "cannot run " + STICKCAL;
        return out;
    }

    void stick_stop() {
        std::lock_guard<std::mutex> g(edges_lock_);
        edges_.stop(3);
    }

    Value stick_save() {
        // Stop measuring, keep the result and turn it on.
        stick_stop();
        Value live, rest;
        {
            std::lock_guard<std::mutex> g(stick_lock_);
            live = stick_live_;
        }
        {
            std::lock_guard<std::mutex> g(lock_);
            rest = stick_rest_;
        }
        if (!live.truthy() || !rest.truthy()) return *Value::parse(R"({"ok": false, "error": "Nothing measured yet."})");
        // The typical reach of each direction (the helper's last line); the
        // furthest values only if the helper died before writing it.
        Value reach = live["reach"];
        if (!reach.truthy()) {
            reach = Value::object();
            for (const char* n : STICK_AXES) {
                reach[n]["lo"] = live["lo"][n];
                reach[n]["hi"] = live["hi"][n];
            }
        }
        Value cal = Value::object();
        for (const char* n : STICK_AXES) {
            Value c = Value::object();
            c["rest"] = rest[n];
            c["lo"] = reach[n]["lo"];
            c["hi"] = reach[n]["hi"];
            c["furthest"] = Value(Value::Array{live["lo"][n], live["hi"][n]});
            cal[n] = c;
        }
        Value res = stickcal({"save"}, cal.dump());
        if (!res["ok"].truthy()) {
            res["error"] = "Not saved: " + res["error"].str("None") + ". Push each stick all the way round the edge.";
            return res;
        }
        {
            std::lock_guard<std::mutex> g(lock_);
            stick_rest_ = Value();
        }
        log("tools: sticks saved and on: %s", std::as_const(res)["plan"].dump().c_str());
        return res;
    }

    Host* host_ = nullptr;
    std::mutex lock_;
    std::string lights_;
    bool kind_known_ = false;
    Value last_, local_;
    std::string announced_, announced_local_;
    Value card_, internal_, sizes_;
    bool sizing_ = false;
    bool move_was_running_ = false;
    MoveLog movelog_;
    uint64_t drive_timer_ = 0;
    int downloads_watch_ = 0, home_watch_ = 0;
    std::string profile_;
    std::string running_, active_;  // the game Steam runs, the mode in force
    std::mutex stick_lock_, edges_lock_;
    Value stick_live_, stick_rest_;
    LineChild edges_;
};

}  // namespace

std::unique_ptr<Section> make_tools_section() { return std::make_unique<Tools>(); }

}  // namespace pq

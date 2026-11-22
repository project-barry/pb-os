// charge-control (was the daemon of sm8550-charge-control): charge limit,
// bypass charging and slow charging on the Retroid Pocket 6 and Nova, the
// AYN Thor, Odin 2 and Odin 2 Portal, and the KONKR Pocket FIT.
//
// Two ways to hold the battery below full, picked by what the kernel offers:
//
//   OEM switches (Retroid Pocket 6, Nova, AYN Thor, Odin 2, Odin 2 Portal):
//   their battery firmware has two switches that kernel patch 0545 exposes
//   on the battery manager device as
//     retroid_charge_now      0 = run from USB power and leave the battery
//                             alone (bypass), 1 = charge
//     retroid_limit_capacity  1 = the firmware stops charging at 80 %
//   The Odin 2's and Portal's firmware (patch 0551) only has the first: their
//   80 % limit is a hold at 80 % done here, like "hold".
//
//   Charge thresholds (KONKR Pocket FIT): the standard power-supply pair
//   charge_control_start_threshold / charge_control_end_threshold, which its
//   firmware enforces and keeps across reboots.
//
// Slow charging (patch 0549 / 0022): the battery's constant_charge_current_max
// is the firmware's fast-charge current limit; a large value gives full speed
// back (the kernel cuts it to the firmware's ceiling).
//
// The firmware may drop the OEM switches and the current limit when the
// charger is plugged in again, after a sleep or when the ADSP restarts, so
// this holds the chosen settings and writes them again at start, after a wake
// and on every plug-in. While the battery is held on purpose, HELD (holding
// this process's PID) tells sm8550-charge-rest's charging indicator and
// pbosd's power LED to show "charged".
//
// The settings live in /var/lib/pbos-utils/charge-control.json, written by
// the Python script's CLI (`sm8550-charge-control settings | save JSON`, what
// PB-OS Tools' Power tab runs); that script stays the CLI. A save is picked
// up from the file itself (inotify) or from SIGHUP (reload()).
//
// What it listens to, instead of the Python version's 10 s ticks:
//   - power_supply uevents (the battery's level and status, the charger
//     coming and going), and auxiliary ones while it waits for the battery
//     manager after boot (the Python version looked every second for 60 s);
//   - logind's PrepareForSleep(false) (the wake), and on every event the
//     BOOTTIME - MONOTONIC gap (it grows when the device slept);
//   - one safety check 60 s after the last one: the firmware sends the
//     battery's level when it likes, not on every percent, and an ADSP
//     restart may drop the switches without a plug-in; the Python version
//     looked every 10 s.
//
// On stop the firmware's defaults come back exactly as the unit's
// ExecStopPost puts them (charging, no 80 % limit, full-speed current) and
// HELD goes, so an update or a crash never leaves the device unable to
// charge. The thresholds stay (the firmware keeps them anyway).
//
// Tests: SM8550_CHARGE_CONTROL_ROOT (as for the Python script) prefixes every
// /sys, /run and /var path. With it set, a file written to ROOT/test/uevent
// (KEY=value lines) is taken as a uevent, RESUME=1 as a wake, DROPPED=1 as
// dropped uevents, and SM8550_CHARGE_CONTROL_WAIT_S shortens the wait for
// the battery manager.

#include <fcntl.h>
#include <sys/inotify.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "json.h"
#include "module.h"
#include "util.h"

namespace pp {

namespace {

using pj::Value;

// A prefix for every /sys, /run and /var path, for tests on a fake tree.
std::string root() {
    const char* r = std::getenv("SM8550_CHARGE_CONTROL_ROOT");
    return r ? r : "";
}

struct Paths {
    std::string root = pp::root();
    std::string state = root + "/var/lib/pbos-utils/charge-control.json";
    std::string held = root + "/run/sm8550-charge-held";
    std::string battmgr = root + "/sys/bus/auxiliary/devices/pmic_glink.power-supply.*";
    std::string battery = root + "/sys/class/power_supply/battery";
    std::string usb = root + "/sys/class/power_supply/qcom-battmgr-usb";
    std::string start_thr = battery + "/charge_control_start_threshold";
    std::string end_thr = battery + "/charge_control_end_threshold";
    std::string current_max = battery + "/constant_charge_current_max";
};

const Paths& P() {
    static const Paths p;
    return p;
}

constexpr const char* MODES[] = {"normal", "limit80", "hold", "bypass"};
constexpr int HOLD_LO = 50, HOLD_HI = 95;
constexpr int THR_HOLD_LO = 55, THR_HOLD_HI = 95;  // the kernel clamps the end threshold to 55..100
constexpr int SLOW_LO = 500, SLOW_HI = 3000;      // mA; the kernel cuts it to the firmware's ceiling
constexpr long FULL_UA = 10'000'000;              // "no limit": cut to the firmware's ceiling
constexpr int HOLD_GAP = 3;
constexpr int BYPASS_FLOOR = 15;
constexpr int LIMIT = 80;
constexpr double WAIT_S = 60;    // for the battery manager after boot
constexpr double SAFETY_S = 60;  // a check after this long without events
constexpr double RELOAD_S = 0.1; // a save writes the file twice and sends HUP: one reload

void log(const std::string& msg) { pq::log("charge-control: %s", msg.c_str()); }

// Python's int() of a stripped string: an optional sign, then digits.
std::optional<long> py_int(std::string_view t) {
    std::string s = pq::trim(t);
    size_t i = (!s.empty() && (s[0] == '+' || s[0] == '-')) ? 1 : 0;
    if (!pq::is_digits(std::string_view(s).substr(i))) return std::nullopt;
    try {
        return std::stol(s);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<long> rd_int(const std::string& p) { return py_int(pq::rd(p)); }

// "None" where Python would print None.
std::string str(std::optional<long> v) { return v ? std::to_string(*v) : "None"; }

// As Python's open(p, "w").write(): truncated first (a sysfs attribute
// ignores that; a test's plain file needs it).
bool put(const std::string& p, std::string_view v) {
    int fd = open(p.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n;
    do n = write(fd, v.data(), v.size());
    while (n < 0 && errno == EINTR);
    int err = errno;
    close(fd);
    errno = err;
    return n == static_cast<ssize_t>(v.size());
}

bool wr(const std::string& p, long v) {
    if (put(p, std::to_string(v))) return true;
    log("write " + p + ": " + std::strerror(errno));
    return false;
}

// The battery manager device with the OEM switches (patch 0545).
std::string battmgr() {
    for (auto& d : pq::glob(P().battmgr))
        if (pq::exists(d + "/retroid_charge_now")) return d;
    return "";
}

bool has_thresholds() { return access(P().end_thr.c_str(), W_OK) == 0 && access(P().start_thr.c_str(), W_OK) == 0; }
bool has_current_limit() { return access(P().current_max.c_str(), W_OK) == 0; }

bool held_by_us() {
    std::string pid = pq::rd(P().held);
    return pq::is_digits(pid) && pq::exists("/proc/" + pid);
}

// ------------------------------------------------------------- settings --
struct Settings {
    std::string mode = "normal";
    int hold_at = 80;
    bool slow = false;
    int slow_ma = 1000;
};

// Python's max(lo, min(hi, int(v))), or default when int(v) fails.
int clamp(const Value& v, int lo, int hi, int def) {
    std::optional<long> n;
    if (v.is_bool()) n = v.as_bool() ? 1 : 0;
    else if (v.is_number()) n = static_cast<long>(std::trunc(std::clamp(v.as_double(), -1e9, 1e9)));  // int() truncates
    else if (v.is_string()) n = py_int(v.as_string());
    if (!n) return def;
    return static_cast<int>(std::max<long>(lo, std::min<long>(hi, *n)));
}

// The mode that matches the thresholds the firmware holds right now.
std::string firmware_mode(Value& raw) {
    auto end = rd_int(P().end_thr);
    if (!end || *end >= 100) return "normal";
    if (*end == LIMIT) return "limit80";
    raw["hold_at"] = static_cast<long long>(std::max<long>(THR_HOLD_LO, std::min<long>(THR_HOLD_HI, *end)));
    return "hold";
}

Settings settings(const std::string& kind) {
    Value raw = pq::read_json(P().state);
    Settings def;
    Value s = Value::object();
    s["mode"] = def.mode;
    s["hold_at"] = def.hold_at;
    s["slow"] = def.slow;
    s["slow_ma"] = def.slow_ma;
    for (const char* k : {"mode", "hold_at", "slow", "slow_ma"})
        if (const Value* v = raw.find(k)) s[k] = *v;
    if (!raw.has("mode") && kind == "thresholds") {
        // First run on a thresholds device: keep what its firmware does now
        // (the Pocket FIT ships stopping at 80 %).
        s["mode"] = firmware_mode(s);
    }
    Settings out;
    const Value& mode = std::as_const(s)["mode"];
    bool known = false;
    if (mode.is_string())
        for (const char* m : MODES) known |= mode.as_string() == m;
    out.mode = known && !(mode.as_string() == "bypass" && kind != "oem") ? mode.as_string() : def.mode;
    bool thr = kind == "thresholds";
    out.hold_at = clamp(std::as_const(s)["hold_at"], thr ? THR_HOLD_LO : HOLD_LO, thr ? THR_HOLD_HI : HOLD_HI,
                        def.hold_at);
    out.slow = std::as_const(s)["slow"].truthy();
    out.slow_ma = clamp(std::as_const(s)["slow_ma"], SLOW_LO, SLOW_HI, def.slow_ma);
    return out;
}

// -------------------------------------------------------------- backends --
class Control {
public:
    virtual ~Control() = default;
    virtual const char* name() const = 0;
    virtual void reset() {}
    // Returns whether the battery is held below full on purpose.
    virtual bool apply(const Settings& s, std::optional<long> cap, bool plugged, bool force) = 0;
};

// retroid_charge_now / retroid_limit_capacity (patch 0545).
class Oem : public Control {
public:
    explicit Oem(std::string dev)
        : dev_(std::move(dev)),
          // The Odin 2 and Portal have no firmware limit (no retroid_limit_capacity).
          has_limit_(pq::exists(dev_ + "/retroid_limit_capacity")) {}
    const char* name() const override { return "oem"; }
    void reset() override { wanted_charge_ = 1; }

    bool apply(const Settings& s, std::optional<long> cap, bool plugged, bool force) override {
        auto [charge, limit] = want(s, cap);
        if (has_limit_ && (force || limit != limit_)) {
            if (wr(dev_ + "/retroid_limit_capacity", limit)) {
                if (limit != limit_) log(std::string("80 % limit ") + (limit ? "on" : "off") + " at " + str(cap) + " %");
                limit_ = limit;
            }
        }
        if (force || charge != charge_) {
            if (wr(dev_ + "/retroid_charge_now", charge)) {
                if (charge != charge_)
                    log(std::string(charge ? "charging allowed" : "running from USB") + " at " + str(cap) + " %");
                charge_ = charge;
            }
        }
        return plugged && cap && (charge == 0 || (limit == 1 && *cap >= LIMIT - 1));
    }

private:
    // (charge_now, limit_capacity) for the mode and battery level.
    std::pair<int, int> want(const Settings& s, std::optional<long> cap) {
        if (s.mode == "normal") return {1, 0};
        if (s.mode == "limit80" && has_limit_) return {1, 1};
        if (!cap) return {1, 0};
        if (s.mode == "hold" || s.mode == "limit80") {
            int at = s.mode == "hold" ? s.hold_at : LIMIT;
            if (*cap >= at) wanted_charge_ = 0;
            else if (*cap <= at - HOLD_GAP) wanted_charge_ = 1;
        } else {  // bypass, with a floor
            if (*cap <= BYPASS_FLOOR) wanted_charge_ = 1;
            else if (*cap >= BYPASS_FLOOR + HOLD_GAP) wanted_charge_ = 0;
        }
        return {wanted_charge_, 0};
    }

    std::string dev_;
    bool has_limit_;
    std::optional<int> charge_, limit_;  // last values written (none = write again)
    int wanted_charge_ = 1;              // hysteresis memory for hold/bypass
};

// charge_control_start/end_threshold, enforced by the firmware.
class Thresholds : public Control {
public:
    const char* name() const override { return "thresholds"; }

    bool apply(const Settings& s, std::optional<long> cap, bool plugged, bool force) override {
        auto [start, end] = want(s);
        if (force || end != end_ || rd_int(P().end_thr) != end || rd_int(P().start_thr) != start) {
            // End first: the kernel moves the end up when a start is written
            // above it, and moves the start down when the end is written
            // below it, so this order lands on (start, end) either way.
            if (wr(P().end_thr, end) && wr(P().start_thr, start)) {
                auto got_start = rd_int(P().start_thr), got_end = rd_int(P().end_thr);
                if (end != end_)
                    log("charge thresholds " + std::to_string(start) + "/" + std::to_string(end) + " % (firmware: " +
                        str(got_start) + "/" + str(got_end) + ")");
                end_ = end;
            }
        }
        // Held: at the end threshold, or in the firmware's window below it
        // (it lets the battery drop to the start threshold before charging).
        std::string status = pq::rd(P().battery + "/status");
        return plugged && end < 100 && cap && (*cap >= end - 1 || (status != "Charging" && *cap >= start));
    }

private:
    // (start, end) for the mode.
    static std::pair<int, int> want(const Settings& s) {
        if (s.mode == "limit80") return {70, LIMIT};  // what the Pocket FIT's firmware ships with
        if (s.mode == "hold") {
            int end = std::max(THR_HOLD_LO, std::min(THR_HOLD_HI, s.hold_at));
            return {end - HOLD_GAP, end};
        }
        return {95, 100};
    }

    std::optional<int> end_;  // last end threshold written
};

// constant_charge_current_max: slow charging (patch 0549 / 0022).
class CurrentLimit {
public:
    void apply(const Settings& s, bool force) {
        long ua = s.slow ? s.slow_ma * 1000L : FULL_UA;
        if (force || ua != ua_) {
            if (wr(P().current_max, ua)) {
                auto got = rd_int(P().current_max);
                if (ua != ua_) {
                    std::string fw = got && *got ? " (firmware: " + std::to_string(*got / 1000) + " mA)" : "";
                    log(s.slow ? "slow charging: " + std::to_string(s.slow_ma) + " mA" + fw : "full-speed charging" + fw);
                }
                ua_ = ua;
            }
        }
    }

private:
    std::optional<long> ua_;  // last value written (none = write again)
};

// BOOTTIME runs on through suspend, MONOTONIC doesn't: a growing gap means
// the device slept since the last look.
double boot_gap() {
    timespec b{}, m{};
    clock_gettime(CLOCK_BOOTTIME, &b);
    clock_gettime(CLOCK_MONOTONIC, &m);
    return (b.tv_sec - m.tv_sec) + (b.tv_nsec - m.tv_nsec) / 1e9;
}

// --------------------------------------------------------------- module --
class ChargeControl : public Module {
public:
    const char* name() const override { return "charge-control"; }

    void start(pq::Loop& loop) override {
        loop_ = &loop;
        gap_ = boot_gap();
        loop.on_uevent_env([this](const pq::Loop::UeventEnv& env) { uevent(env); });
        watch_resume();
        // A save from the CLI: the state file is replaced (written twice,
        // then HUP); one reload for all of it. The folder is made so it can
        // be watched before the first save.
        std::string dir = pq::dirname(P().state), file = pq::basename(P().state);
        pq::makedirs(dir);
        loop.watch(dir, IN_CLOSE_WRITE | IN_MOVED_TO, [this, file](uint32_t, const std::string& name) {
            if (name == file) reload();
        });
        if (!P().root.empty()) test_hook();
        if (find()) return;
        double wait = WAIT_S;
        if (!P().root.empty())
            if (const char* w = std::getenv("SM8550_CHARGE_CONTROL_WAIT_S")) wait = std::atof(w);
        wait_timer_ = loop.after(wait, [this] {
            wait_timer_ = 0;
            if (control_) return;
            gave_up_ = true;
            log("no charge controls (kernel patch 0545 with Retroid, Thor, Odin 2 or Portal firmware, or charge "
                "thresholds): nothing to do");
            // What the Python daemon's exit did: the unit's ExecStopPost ran.
            restore();
        });
    }

    void stop() override {
        if (!loop_) return;
        if (bus_) {
            sd_bus_slot_unref(slot_);
            sd_bus_flush_close_unref(bus_);
            bus_ = nullptr;
        }
        restore();
    }

    // SIGHUP (the CLI's save): read the settings again and write them.
    void reload() override {
        if (!loop_ || !control_ || reload_timer_) return;
        reload_timer_ = loop_->after(RELOAD_S, [this] {
            reload_timer_ = 0;
            s_ = settings(control_->name());
            control_->reset();
            log(describe());
            check(true);
        });
    }

private:
    // The battery manager (or the thresholds) showed up: start holding.
    bool find() {
        if (control_ || gave_up_) return control_ != nullptr;
        if (std::string dev = battmgr(); !dev.empty()) control_ = std::make_unique<Oem>(dev);
        else if (has_thresholds()) control_ = std::make_unique<Thresholds>();
        else return false;
        if (wait_timer_) loop_->cancel(wait_timer_), wait_timer_ = 0;
        if (has_current_limit()) current_ = std::make_unique<CurrentLimit>();
        else log("no charge current limit (kernel patch 0549 / 0022): no slow charging");
        s_ = settings(control_->name());
        log(std::string(control_->name()) + ": " + describe());
        check(true);
        return true;
    }

    std::string describe() const {
        std::string out = "mode " + s_.mode + (s_.mode == "hold" ? " at " + std::to_string(s_.hold_at) + " %" : "");
        if (current_)
            out += s_.slow ? ", slow charging " + std::to_string(s_.slow_ma) + " mA" : ", full-speed charging";
        return out;
    }

    void uevent(const pq::Loop::UeventEnv& env) {
        std::string subsystem;
        if (!env.empty()) {  // empty: some were dropped, so look again
            auto it = env.find("SUBSYSTEM");
            subsystem = it == env.end() ? "" : it->second;
            if (subsystem != "power_supply" && subsystem != "auxiliary") return;
        }
        if (!control_) {
            find();
            return;
        }
        if (subsystem != "auxiliary") check(false);
    }

    // A look at the battery and the charger, settings written as needed.
    void check(bool force) {
        if (!control_) return;
        double g = boot_gap();
        if (g - gap_ > 1) force = true;  // slept since the last look
        gap_ = g;
        apply(force);
        if (safety_timer_) loop_->cancel(safety_timer_);
        safety_timer_ = loop_->after(SAFETY_S, [this] {
            safety_timer_ = 0;
            check(false);
        });
    }

    void apply(bool force) {
        auto cap = rd_int(P().battery + "/capacity");
        bool plugged = pq::rd(P().usb + "/online") == "1";
        if (plugged && !was_plugged_) force = true;  // firmware may have dropped the switches
        was_plugged_ = plugged;
        bool held = control_->apply(s_, cap, plugged, force);
        if (current_) current_->apply(s_, force);
        if (held) {
            if (!held_by_us()) pq::write_text(P().held, std::to_string(getpid()));
        } else if (pq::exists(P().held)) {
            pq::remove_file(P().held);
        }
    }

    // The firmware's defaults, exactly as the unit's ExecStopPost writes
    // them: charging and no 80 % limit on every battery manager, full-speed
    // current, no HELD.
    void restore() {
        for (auto& d : pq::glob(P().battmgr)) {
            if (pq::exists(d + "/retroid_charge_now")) put(d + "/retroid_charge_now", "1");
            if (pq::exists(d + "/retroid_limit_capacity")) put(d + "/retroid_limit_capacity", "0");
        }
        if (has_current_limit()) put(P().current_max, std::to_string(FULL_UA));
        pq::remove_file(P().held);
    }

    // logind's PrepareForSleep(false): the device woke, the firmware may
    // have dropped the switches. A connection of its own until the loop
    // shares its bus for signals.
    void watch_resume() {
        if (!P().root.empty()) return;  // tests: RESUME=1 instead
        if (sd_bus_open_system(&bus_) < 0) {
            bus_ = nullptr;
            log("no system bus: wakes found by the clock gap only");
            return;
        }
        auto on_sleep = [](sd_bus_message* m, void* self, sd_bus_error*) -> int {
            int going = 1;
            if (sd_bus_message_read(m, "b", &going) >= 0 && !going) static_cast<ChargeControl*>(self)->resumed();
            return 0;
        };
        if (sd_bus_match_signal(bus_, &slot_, "org.freedesktop.login1", "/org/freedesktop/login1",
                                "org.freedesktop.login1.Manager", "PrepareForSleep", on_sleep, this) < 0) {
            log("cannot watch logind's PrepareForSleep: wakes found by the clock gap only");
            sd_bus_flush_close_unref(bus_);
            bus_ = nullptr;
            return;
        }
        loop_->on_readable(sd_bus_get_fd(bus_), [this] {
            int r = 0;
            while (bus_ && (r = sd_bus_process(bus_, nullptr)) > 0) {}
            if (bus_ && r < 0) {
                // The bus went away: closing the socket takes it off the loop.
                log("system bus lost: wakes found by the clock gap only");
                sd_bus_slot_unref(slot_);
                slot_ = nullptr;
                sd_bus_close_unref(bus_);
                bus_ = nullptr;
            }
        });
    }

    void resumed() {
        if (control_) check(true);
    }

    // Tests: ROOT/test/uevent, KEY=value lines, taken as one uevent.
    void test_hook() {
        std::string dir = P().root + "/test";
        pq::makedirs(dir);
        loop_->watch(dir, IN_CLOSE_WRITE | IN_MOVED_TO, [this, dir](uint32_t, const std::string& name) {
            if (name != "uevent") return;
            pq::Loop::UeventEnv env;
            for (auto& line : pq::split_lines(pq::rd(dir + "/uevent"))) {
                auto eq = line.find('=');
                if (eq != std::string::npos) env[line.substr(0, eq)] = line.substr(eq + 1);
            }
            if (env.count("RESUME")) resumed();
            else if (env.count("DROPPED")) uevent({});
            else uevent(env);
        });
    }

    pq::Loop* loop_ = nullptr;
    std::unique_ptr<Control> control_;
    std::unique_ptr<CurrentLimit> current_;
    Settings s_;
    bool was_plugged_ = false;
    double gap_ = 0;
    bool gave_up_ = false;
    uint64_t wait_timer_ = 0, safety_timer_ = 0, reload_timer_ = 0;
    sd_bus* bus_ = nullptr;
    sd_bus_slot* slot_ = nullptr;
};

}  // namespace

// The unit's ExecCondition: the devices with charge controls.
bool charge_control_here() {
    for (const char* c : {"retroidpocket,rp6", "retroidpocket,rpnova", "retroidpocket,rpduo", "ayn,thor", "ayn,odin2",
                          "konkr,pocket-fit"})
        if (dt_has(c)) return true;
    return false;
}

std::unique_ptr<Module> make_charge_control() { return std::make_unique<ChargeControl>(); }

}  // namespace pp

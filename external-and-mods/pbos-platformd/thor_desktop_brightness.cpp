// thor-desktop-brightness (was sm8550-thor-desktop-brightness): AYN Thor,
// Desktop Mode. Plasma's two brightness sliders share each screen's
// brightness with Game Mode (SCREENS, kept by sm8550-thor-backlightd), as
// each screen has one brightness on AYN's Android.
//
// Plasma's PowerDevil lists both panels as "Built-in Screen", each 0..10000:
// the top one (display0) dims in software, the bottom one (display1) goes to
// the bottom panel's backlight, which sm8550-thor-backlightd takes over. When
// Plasma has started (org.kde.ScreenBrightness gets an owner on the session
// bus), this sets both sliders to SCREENS as Desktop Mode began
// (DESKTOP_START; Plasma first applies its own last levels), says so
// (DESKTOP_READY), and from then on writes the top slider's position to
// DESKTOP_TOP on each change (PowerDevil's BrightnessChanged signal). The
// bottom slider's changes reach the daemon through the panel itself.
//
// Which display is the bottom one is checked, not assumed: setting it must
// show in the bottom panel's brightness (Steam's file there, bind-mounted
// over the sysfs attribute by sm8550-thor-backlightd, on the top panel's
// scale while Desktop Mode is up). The Python version read that file every
// 50 ms for up to CHECK_S; here it is watched (inotify: PowerDevil's helper
// writes it, and a write to a file, a bind-mounted plain one or a sysfs
// attribute alike, is an IN_MODIFY) and looked at once more when CHECK_S is
// up. Should the file not take a watch, it is looked at every 50 ms as
// before, for at most CHECK_S.
//
// Retroid Pocket Duo: the other way round. The hardware slider is the top
// screen's (KWin gives it to DSI-1) and comes to the daemon through Steam's
// file over the top panel's; the bottom one dims in software. The top
// panel's file is the one checked, with a level a little off the one wanted,
// as it may hold the wanted one already; then both sliders are set, and both
// positions written on each change (the daemon takes DESKTOP_BOTTOM, whose
// first is also the sign to leave the bottom screen's dimming to Plasma).
// Should neither display move the panel, both sliders go back to where they
// were.
//
// The session bus is a connection of this module's own (sd-bus, the user's
// bus). The D-Bus calls are synchronous, as they were in Python; they happen
// once, when Plasma starts.

#include <sys/inotify.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
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

const char* const SERVICE = "org.kde.ScreenBrightness";
const std::string PATH = "/org/kde/ScreenBrightness";
const char* const DISPLAY = "org.kde.ScreenBrightness.Display";
const std::string RUN_DIR = "/run/sm8550-thor";
const std::string DESKTOP_START = RUN_DIR + "/desktop-start.json";
const std::string DESKTOP_READY = RUN_DIR + "/desktop-ready";
const std::string DESKTOP_TOP = RUN_DIR + "/desktop-top";
const std::string SCREENS = "/var/lib/steamos-sm8550/thor-screen-brightness.json";
const std::string DESKTOP_BOTTOM = RUN_DIR + "/desktop-bottom";  // the Duo
// The panel behind Plasma's hardware slider: the Thor's bottom one, the
// Duo's top one (ae94000 is DSI-1 on both).
const std::string PANEL_FILE = "/sys/class/backlight/ae94000.dsi.0/brightness";
const std::string PANEL_FILE_MAX = "/sys/class/backlight/ae94000.dsi.0/max_brightness";
// The Duo's bottom panel at its lowest (duo_bottom's 10 of 3445), for its
// software slider, as the Thor's top one's.
constexpr double DUO_BOTTOM_LOWEST = 10.0 / 3445;
// How far off the wanted level the Duo's check is.
constexpr double DUO_PROBE = 0.05;
// The daemon's: the top panel's lowest and highest, as on AYN's Android.
constexpr double PANEL_MIN = 10, TOP_CAP = 3413;
constexpr double CHECK_S = 1.0;   // how long the bottom panel may take to show a new level
constexpr double POLL_S = 0.05;   // only when the file takes no watch

void log(const std::string& msg) { pq::log("thor-desktop-brightness: %s", msg.c_str()); }

std::string fmt(const char* f, double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

// The file's text, NULs as spaces, stripped; "" when unreadable.
std::string read(const std::string& path) {
    auto t = pq::read_file(path);
    if (!t) return "";
    std::replace(t->begin(), t->end(), '\0', ' ');
    return pq::trim(*t);
}

// path.tmp, renamed over path (the folder is the daemon's).
void write(const std::string& path, const std::string& text) {
    std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    bool ok = f && std::fputs(text.c_str(), f) >= 0;
    if (f && std::fclose(f) != 0) ok = false;
    if (!ok || std::rename(tmp.c_str(), path.c_str()) != 0) log("cannot write " + path + ": " + std::strerror(errno));
}

// float(v): numbers, booleans and numeric strings.
std::optional<double> py_float(const Value& v) {
    if (v.is_bool()) return v.as_bool() ? 1.0 : 0.0;
    if (v.is_number()) return v.as_double();
    if (!v.is_string()) return std::nullopt;
    std::string s = pq::trim(v.as_string());
    if (s.empty()) return std::nullopt;
    char* end = nullptr;
    double d = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size() || s.find_first_of("xX") != std::string::npos) return std::nullopt;
    return d;
}

// int(s) for a stripped file: an optional sign, then digits.
std::optional<long long> py_int(const std::string& t) {
    std::string s = pq::trim(t);
    size_t i = (!s.empty() && (s[0] == '+' || s[0] == '-')) ? 1 : 0;
    if (!pq::is_digits(std::string_view(s).substr(i)) || s.size() - i > 15) return std::nullopt;
    return std::stoll(s);
}

struct Want {
    double top, bottom;
};

// DESKTOP_START, else SCREENS: each screen's position, 0..1.
std::optional<Want> screens() {
    for (const std::string& path : {DESKTOP_START, SCREENS}) {
        auto d = Value::parse(read(path));
        if (!d || !d->is_object()) continue;
        std::optional<double> v[2];
        const char* keys[] = {"top", "bottom"};
        bool ok = true;
        for (int i = 0; i < 2 && ok; ++i) {
            const Value* x = d->find(keys[i]);
            if (x) v[i] = py_float(*x);
            ok = x && v[i];
        }
        if (!ok) continue;
        // max(0.0, min(1.0, x)), NaN coming out as 1.0 as there.
        auto clamp = [](double x) {
            double m = x < 1.0 ? x : 1.0;
            return m > 0.0 ? m : 0.0;
        };
        return Want{clamp(*v[0]), clamp(*v[1])};
    }
    return std::nullopt;
}

struct BusError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A D-Bus error as dbus-python's DBusException printed it.
BusError bus_error(const sd_bus_error& e, int r) {
    if (e.name) return BusError(std::string(e.name) + ": " + (e.message ? e.message : ""));
    return BusError(std::strerror(-r));
}

struct Display {
    std::string name;
    long long max = 0;
};

class ThorDesktopBrightness : public Module {
public:
    explicit ThorDesktopBrightness(bool duo) : duo_(duo) {}
    const char* name() const override { return "thor-desktop-brightness"; }

    void start(pq::Loop& loop) override {
        loop_ = &loop;
        if (!pq::is_dir(RUN_DIR)) {
            log(RUN_DIR + " missing: sm8550-thor-backlightd is not running");
            return;
        }
        if (sd_bus_open_user(&bus_) < 0) {
            bus_ = nullptr;
            log("no session bus");
            return;
        }
        // PowerDevil may come up after this: wait for its brightness service.
        std::string match = std::string("type='signal',sender='org.freedesktop.DBus',path='/org/freedesktop/DBus',"
                                        "interface='org.freedesktop.DBus',member='NameOwnerChanged',arg0='") +
                            SERVICE + "'";
        auto on_change = [](sd_bus_message* m, void* self, sd_bus_error*) -> int {
            const char *name = nullptr, *old = nullptr, *now = nullptr;
            if (sd_bus_message_read(m, "sss", &name, &old, &now) >= 0 && now)
                static_cast<ThorDesktopBrightness*>(self)->owner(now);
            return 0;
        };
        auto on_owner = [](sd_bus_message* m, void* self, sd_bus_error*) -> int {
            const char* now = nullptr;
            if (!sd_bus_message_is_method_error(m, nullptr) && sd_bus_message_read(m, "s", &now) >= 0 && now)
                static_cast<ThorDesktopBrightness*>(self)->owner(now);
            return 0;
        };
        if (sd_bus_add_match(bus_, &owner_slot_, match.c_str(), on_change, this) < 0 ||
            sd_bus_call_method_async(bus_, nullptr, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                     "org.freedesktop.DBus", "GetNameOwner", on_owner, this, "s", SERVICE) < 0) {
            log("cannot watch the session bus for " + std::string(SERVICE));
            close_bus();
            return;
        }
        loop.on_readable(sd_bus_get_fd(bus_), [this] { process(); });
        process();
    }

    void stop() override {
        end_check();
        close_bus();
    }

private:
    void close_bus() {
        if (!bus_) return;
        sd_bus_slot_unref(owner_slot_);
        sd_bus_slot_unref(changed_slot_);
        sd_bus_slot_unref(added_slot_);
        owner_slot_ = changed_slot_ = added_slot_ = nullptr;
        sd_bus_flush_close_unref(bus_);
        bus_ = nullptr;
    }

    // Everything the bus has for us, the messages a synchronous call left
    // queued included (the socket no longer shows them).
    void process() {
        int r = 0;
        while (bus_ && (r = sd_bus_process(bus_, nullptr)) > 0) {}
        if (bus_ && r < 0) {
            log("session bus lost");
            end_check();
            close_bus();  // closing the socket takes it off the loop
        }
    }

    void owner(const std::string& now) {
        if (now.empty() || started_) return;
        started_ = true;
        // Off the bus callback: the setup makes calls of its own.
        loop_->post([this] {
            try {
                setup();
            } catch (const BusError& e) {
                failed(e);
            }
            loop_->post([this] { process(); });
        });
    }

    // Setup again on PowerDevil's DisplayAdded (false: cannot watch it).
    bool watch_added() {
        if (added_slot_) return true;
        std::string match = std::string("type='signal',sender='") + SERVICE + "',path='" + PATH +
                            "',interface='" + SERVICE + "',member='DisplayAdded'";
        auto on_added = [](sd_bus_message*, void* self, sd_bus_error*) -> int {
            auto* me = static_cast<ThorDesktopBrightness*>(self);
            me->loop_->post([me] {
                try {
                    me->setup();
                } catch (const BusError& e) {
                    me->failed(e);
                }
                me->loop_->post([me] { me->process(); });
            });
            return 0;
        };
        return sd_bus_add_match(bus_, &added_slot_, match.c_str(), on_added, this) >= 0;
    }

    void failed(const BusError& e) {
        log(std::string("PowerDevil: ") + e.what());
        end_check();
        write(DESKTOP_READY, "1\n");
    }

    // ---------------------------------------------------------- D-Bus ---
    Value get_property(const std::string& path, const char* iface, const char* prop, const char* type) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        int r = sd_bus_call_method(bus_, SERVICE, path.c_str(), "org.freedesktop.DBus.Properties", "Get", &err,
                                   &reply, "ss", iface, prop);
        if (r < 0) {
            BusError e = bus_error(err, r);
            sd_bus_error_free(&err);
            throw e;
        }
        Value out;
        char t = 0;
        const char* contents = nullptr;
        if (sd_bus_message_peek_type(reply, &t, &contents) >= 0 && t == 'v' && contents &&
            sd_bus_message_enter_container(reply, 'v', contents) >= 0) {
            if (std::string(type) == "as" && std::string(contents) == "as") {
                out = Value::array();
                if (sd_bus_message_enter_container(reply, 'a', "s") >= 0) {
                    const char* s = nullptr;
                    while (sd_bus_message_read(reply, "s", &s) > 0) out.push(s);
                }
            } else if (auto n = read_int(reply)) {
                out = *n;
            }
        }
        sd_bus_message_unref(reply);
        if (out.is_null()) throw BusError(std::string("unexpected ") + prop + (contents ? " type " : "") +
                                          (contents ? contents : ""));
        return out;
    }

    // An integer of any D-Bus type (dbus-python's int() of it).
    static std::optional<long long> read_int(sd_bus_message* m) {
        char t = 0;
        if (sd_bus_message_peek_type(m, &t, nullptr) <= 0) return std::nullopt;
        union {
            uint8_t y;
            int16_t n;
            uint16_t q;
            int32_t i;
            uint32_t u;
            int64_t x;
            uint64_t tt;
            double d;
        } v{};
        if (std::strchr("ynqiuxtd", t) == nullptr || sd_bus_message_read_basic(m, t, &v) < 0) return std::nullopt;
        switch (t) {
            case 'y': return v.y;
            case 'n': return v.n;
            case 'q': return v.q;
            case 'i': return v.i;
            case 'u': return v.u;
            case 'x': return v.x;
            case 't': return static_cast<long long>(v.tt);
            default: return static_cast<long long>(std::trunc(v.d));
        }
    }

    void set(const Display& d, double ratio) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        int32_t level = static_cast<int32_t>(std::nearbyint(ratio * d.max));  // round(): halves to even
        int r = sd_bus_call_method(bus_, SERVICE, (PATH + "/" + d.name).c_str(), DISPLAY, "SetBrightness", &err,
                                   nullptr, "iu", level, 0u);
        if (r < 0) {
            BusError e = bus_error(err, r);
            sd_bus_error_free(&err);
            throw e;
        }
    }

    // ---------------------------------------------------------- setup ---
    void setup() {
        auto w = screens();
        if (!w) {
            log("no brightness to share yet; Plasma's sliders stay as they are");
            write(DESKTOP_READY, "1\n");
            return;
        }
        want_ = *w;
        Value names = get_property(PATH, SERVICE, "DisplaysDBusNames", "as");
        displays_.clear();
        std::string listed;
        for (const auto& n : names.items()) {
            Display d{n.as_string(), 0};
            d.max = get_property(PATH + "/" + d.name, DISPLAY, "MaxBrightness", "i").as_int();
            displays_.push_back(d);
            listed += (listed.empty() ? "'" : ", '") + d.name + "'";
        }
        if (displays_.size() < 2 && watch_added()) {
            // PowerDevil lists each screen as KWin hands it over: the
            // bottom one can come a moment after the service (this starts
            // well before the Python did, which never saw one alone).
            log("found [" + listed + "]; waiting for Plasma's second display");
            return;
        }
        // Set up once: later displays (a monitor plugged in) change nothing.
        added_slot_ = sd_bus_slot_unref(added_slot_);
        if (displays_.size() != 2) {
            log("expected two displays, found [" + listed + "]; leaving Plasma's sliders alone");
            write(DESKTOP_READY, "1\n");
            return;
        }
        if (duo_) return duo_check();
        // The bottom one is display1 on a Thor; checked by its panel.
        top_ = 0, bottom_ = 1;
        begin_check();
        probe_ = want_.bottom;
        set(displays_[bottom_], want_.bottom);
        check([this](bool shows) {
            if (shows) return finish();
            std::swap(top_, bottom_);
            set(displays_[bottom_], want_.bottom);
            check([this](bool shows) {
                log(shows ? displays_[top_].name + " turned out to be the bottom screen's"
                          : "cannot tell the screens apart: the bottom panel did not follow either");
                finish();
            });
        });
    }

    // The Duo: which display moves the top panel?
    void duo_check() {
        top_ = 0, bottom_ = 1;
        before_.clear();
        for (const Display& d : displays_) {
            std::optional<long long> was;
            try {
                was = get_property(PATH + "/" + d.name, DISPLAY, "Brightness", "i").as_int();
            } catch (const BusError&) {
            }
            before_.push_back(was);
        }
        begin_check();
        probe_ = want_.top >= 0.5 ? want_.top - DUO_PROBE : want_.top + DUO_PROBE;
        set(displays_[top_], probe_);
        check([this](bool shows) {
            if (shows) return duo_finish();
            put_back(top_);
            std::swap(top_, bottom_);
            set(displays_[top_], probe_);
            check([this](bool shows) {
                if (shows) {
                    log(displays_[top_].name + " turned out to be the top screen's");
                    return duo_finish();
                }
                log("cannot tell the screens apart: the top panel did not follow either; sliders left alone");
                put_back(top_);
                end_check();
                write(DESKTOP_READY, "1\n");
            });
        });
    }

    void put_back(size_t i) {
        const Display& d = displays_[i];
        if (i < before_.size() && before_[i] && d.max != 0)
            set(d, static_cast<double>(*before_[i]) / static_cast<double>(d.max));
    }

    void duo_finish() {
        end_check();
        const Display& top = displays_[top_];
        const Display& bottom = displays_[bottom_];
        set(top, want_.top);
        set(bottom, std::max(want_.bottom, DUO_BOTTOM_LOWEST));
        log("top (" + top.name + ") " + fmt("%.3f", want_.top) + ", bottom (" + bottom.name + ") " +
            fmt("%.3f", want_.bottom));
        write(DESKTOP_BOTTOM, fmt("%.5f", want_.bottom) + "\n");
        write(DESKTOP_READY, "1\n");
        watch_sliders();
    }

    void finish() {
        end_check();
        const Display& top = displays_[top_];
        const Display& bottom = displays_[bottom_];
        // Plasma's top slider dims in software; never lower than the panel's
        // own lowest, as on Android.
        set(top, std::max(want_.top, PANEL_MIN / TOP_CAP));
        log("top (" + top.name + ") " + fmt("%.3f", want_.top) + ", bottom (" + bottom.name + ") " +
            fmt("%.3f", want_.bottom));
        write(DESKTOP_READY, "1\n");
        watch_sliders();
    }

    void watch_sliders() {
        if (changed_slot_) return;
        auto on_changed = [](sd_bus_message* m, void* self, sd_bus_error*) -> int {
            auto* me = static_cast<ThorDesktopBrightness*>(self);
            const char* name = nullptr;
            if (sd_bus_message_read(m, "s", &name) < 0 || !name) return 0;
            auto level = read_int(m);
            auto moved = [&](const Display& d, const std::string& path) {
                if (level && name == d.name && d.max != 0)
                    write(path, fmt("%.5f", static_cast<double>(*level) / static_cast<double>(d.max)) + "\n");
            };
            moved(me->displays_[me->top_], DESKTOP_TOP);
            if (me->duo_) moved(me->displays_[me->bottom_], DESKTOP_BOTTOM);
            return 0;
        };
        std::string match = "type='signal',interface='" + std::string(SERVICE) +
                            "',member='BrightnessChanged',path='" + PATH + "'";
        if (sd_bus_add_match(bus_, &changed_slot_, match.c_str(), on_changed, this) < 0)
            log("cannot watch PowerDevil's BrightnessChanged");
    }

    // -------------------------------------------------- the panel's check ---
    // Watched from before the first SetBrightness, so no write is missed.
    void begin_check() {
        watch_ = loop_->watch(PANEL_FILE, IN_MODIFY | IN_CLOSE_WRITE,
                              [this](uint32_t, const std::string&) { look(false); });
    }

    void end_check() {
        if (watch_) loop_->unwatch(watch_), watch_ = 0;
        if (timer_) loop_->cancel(timer_), timer_ = 0;
        if (poll_) loop_->cancel(poll_), poll_ = 0;
        checking_ = nullptr;
    }

    // Has the panel's brightness come to probe_ of its range?
    // done(bool) runs once, on the loop, within CHECK_S.
    void check(std::function<void(bool)> done) {
        auto max = py_int(read(PANEL_FILE_MAX));
        if (!max) {
            loop_->post([done] { done(false); });  // as soon as Python's gave up
            return;
        }
        panel_max_ = *max;
        checking_ = std::move(done);
        if (look(false)) return;
        timer_ = loop_->after(CHECK_S, [this] {
            timer_ = 0;
            look(true);
        });
        if (!watch_) poll_ = loop_->after(POLL_S, [this] { poll(); });
    }

    void poll() {
        poll_ = 0;
        if (!checking_ || look(false)) return;
        poll_ = loop_->after(POLL_S, [this] { poll(); });
    }

    // Looks at the file; true once the check is over.
    bool look(bool last) {
        if (!checking_) return false;
        auto words = py_split(read(PANEL_FILE));
        bool shows = false;
        if (!words.empty() && pq::is_digits(words.back()) && words.back().size() <= 15) {
            double maximum = static_cast<double>(panel_max_);
            double slack = std::max(2.0, maximum / 500);
            shows = std::fabs(std::stoll(words.back()) - probe_ * maximum) <= slack;
        }
        if (!shows && !last) return false;
        auto done = std::move(checking_);
        checking_ = nullptr;
        if (timer_) loop_->cancel(timer_), timer_ = 0;
        if (poll_) loop_->cancel(poll_), poll_ = 0;
        // On the loop: done may set a display and start the next check.
        loop_->post([this, done, shows] {
            try {
                done(shows);
            } catch (const BusError& e) {
                failed(e);
            }
            process();
        });
        return true;
    }

    static std::vector<std::string> py_split(const std::string& s) {
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

    const bool duo_;
    pq::Loop* loop_ = nullptr;
    sd_bus* bus_ = nullptr;
    sd_bus_slot* owner_slot_ = nullptr;
    sd_bus_slot* changed_slot_ = nullptr;
    sd_bus_slot* added_slot_ = nullptr;
    bool started_ = false;
    Want want_{};
    std::vector<Display> displays_;
    size_t top_ = 0, bottom_ = 1;
    int watch_ = 0;
    uint64_t timer_ = 0, poll_ = 0;
    long long panel_max_ = 0;
    double probe_ = 0;  // the level the panel is to show
    std::vector<std::optional<long long>> before_;  // the Duo: the sliders before the check
    std::function<void(bool)> checking_;
};

}  // namespace

std::unique_ptr<Module> make_thor_desktop_brightness() {
    return std::make_unique<ThorDesktopBrightness>(dt_has("retroidpocket,rpduo"));
}

}  // namespace pp

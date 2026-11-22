// thor-backlight (was sm8550-thor-backlightd): the AYN Thor's bottom screen.
// One brightness for both panels, the Barry Launcher plugin's bottom-screen
// switch, and the AYN button. The Retroid Pocket Duo's bottom screen too, as
// a profile of its own (Profile, DUO; below).
//
// Brightness: Steam's Game Mode slider adjusts the first backlight it finds,
// the bottom panel's (ae94000.dsi.0), and writes it directly (the video group
// may), about ten times a second while the slider moves, from a file it keeps
// open. Game Mode shows the top panel (ae96000.dsi.0). So that Steam's writes
// never reach the bottom panel, this opens the panel's brightness file and
// then bind-mounts STEAM_BOTTOM, a plain file, over it before Game Mode
// starts (the unit runs before the display manager); Steam writes its level
// there and this is the only writer of the bottom panel. Steam's level, a
// fraction of the range (from STEAM_BOTTOM, or from the top panel should
// gamescope write that), is kept in LEVEL. As on AYN's Android, each screen
// has a brightness of its own (SCREENS, a slider position 0-1): Steam's
// slider is Android's "both screens" one and sets both to its position, the
// Barry Launcher plugin's top and bottom sliders set one each. The panels
// then get what Android gives them for those positions (thor_backlight.h).
//
// Bottom-screen switch: the Barry Launcher Decky plugin writes "off" or "on"
// to STATE. While the bottom screen is off its backlight is powered down
// (bl_power; the brightness Steam sets is kept underneath, so the top panel
// still follows) and its touchscreen is inhibited. The panel driver powers
// the backlight up again on each modeset and wake (drm_panel_enable ->
// backlight_enable, which tells no one), and the Game Mode / Desktop start
// scripts re-enable the touchscreen: both are put back on every pass, and
// while the bottom screen should be dark a pass also comes every
// DARK_RECHECK_S, there being no event for either.
//
// AYN button: InputPlumber leaves gpio-keys-ayn alone on the Thor; this
// grabs it. In Game Mode a short press of KEY_F24 opens or closes Barry
// Launcher (OVERLAY, which barry_launcher_shelld watches and also writes);
// holding it LONG_PRESS_S turns the bottom screen off or on (STATE), and
// turning it off also closes Barry. A short press while it is off turns it
// on again, with Barry open. While Barry is open the bottom screen is on even
// if the switch says off (from OVERLAY_DELAY_S after, once Barry has drawn).
// While OVERLAY or TOP_INPUT is "1", Steam's gamescope gets
// GAMESCOPE_BOTTOM_SCREEN_YIELD on its root (a DS/3DS emulator's second
// window hands the bottom screen back to Barry). In Desktop Mode a short
// press turns Barry Launcher off or on there (DESKTOP_SWITCH).
//
// Sleep: with Barry Launcher's bottomSleepSeconds set, no touch on the bottom
// screen and no AYN press for that long turns the bottom backlight off, its
// touchscreen left on: the next touch, or an AYN press, wakes it. Not in
// Desktop Mode, nor while a game draws on the bottom screen
// (GAMESCOPE_BOTTOM_SCREEN_SHOWING).
//
// Charge rest: while sm8550-charge-rest has the device resting on the
// charger (CHARGE_REST), both screens are its to power off and back on, and
// this does nothing (input that comes meanwhile is acted on afterwards, as
// the Python's unread device files had it).
//
// Idle dim: touches on the bottom screen go to the bottom session's
// gamescope, not Steam's, so this reads the bottom touchscreen (without
// grabbing it) and, at most every NUDGE_S, taps right Shift on Steam's
// display.
//
// The Python woke every 100 ms (16 ms in a glide) and looked at everything.
// This runs the same pass, in the same order, only when something happened:
// inotify on /run/sm8550-thor (the flags, Steam's file, Desktop Mode's
// files), STATE and SCREENS, Barry's settings and the charge-rest flag;
// evdev; input, backlight and DRM uevents (the backlight core sends one, and
// notifies actual_brightness, on every sysfs write to brightness, so
// gamescope's writes to the top panel are heard; devices are found when
// they come rather than by a 5 s retry); the kernel's process events for
// Desktop Mode and Steam's display (else a /proc look every MODE_CHECK_S, as
// before); and a one-shot timer for the next thing due (a follow step, the
// long press, the overlay delay, the sleep deadline, a refused panel's
// retry, Desktop Mode's 10 s hold). Root window properties go through xprop,
// as before, but only when they change.
//
// Retroid Pocket Duo (DUO): the panels are the other way round (top
// ae94000, bottom ae96000) and Steam's slider finds the top panel first, so
// it is the top screen's own and nothing goes over it; the top panel is
// never written here. The bottom panel follows SCREENS' "bottom" alone (the
// PB-OS tab's and Barry's bottom sliders), on a straight line
// (linear_bottom). No AYN button: Barry's quick settings put the bottom
// screen to sleep through SLEEP (dark, its touchscreen on: a touch wakes it;
// Barry closes, as the AYN button's hold closes it); the switch (STATE),
// Barry's flags, the yield to Barry, the sleep timer, touch wake and the
// idle-dim nudge are as on the Thor. Desktop Mode changes nothing here yet.
//
// Two departures from the Python, for a smooth slider: a new brightness is
// followed continuously rather than glided to from scratch on every new
// value (thor_backlight.h's follow), and each panel is written by a thread
// of its own that always takes the newest value (a write to the bottom
// panel waits for a DSI frame, 17-33 ms, and held up everything else).

#include "thor_backlight.h"

#include <fcntl.h>
#include <grp.h>
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <pwd.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "module.h"
#include "util.h"

namespace pp {

namespace {

using namespace thor_bl;

// The device's panels and what this does with them.
struct Profile {
    const char* bottom;  // the bottom panel's backlight
    const char* top;     // the top panel's
    // Steam's slider writes the bottom panel's file (STEAM_BOTTOM goes over
    // it) and sets both screens; the top panel is written here too (TOP_CAP),
    // and Desktop Mode shares SCREENS through Plasma's sliders.
    bool steam_both;
    bool ayn_button;  // gpio-keys-ayn's KEY_F24
    bool sleep_flag;  // SLEEP: Barry's Screen off
};
const Profile THOR{"/sys/class/backlight/ae94000.dsi.0", "/sys/class/backlight/ae96000.dsi.0", true, true, false};
const Profile DUO{"/sys/class/backlight/ae96000.dsi.0", "/sys/class/backlight/ae94000.dsi.0", false, false, true};
// This device's (make_thor_backlight).
const Profile* P = &THOR;
const char* const VAR_DIR = "/var/lib/steamos-sm8550";
const char* const STATE_NAME = "thor-bottom-screen";
const char* const STATE = "/var/lib/steamos-sm8550/thor-bottom-screen";
const char* const CHARGE_REST_DIR = "/run";
const char* const CHARGE_REST_NAME = "sm8550-charge-rest";
const char* const CHARGE_REST = "/run/sm8550-charge-rest";  // sm8550-charge-rest: resting on the charger
// Settings of earlier ways to set the screens (the plugin's dimmers, and a
// test build's floors and screen match): both screens follow Steam's slider
// alone now, so any left over are removed.
const char* const LEFTOVERS[] = {"/var/lib/steamos-sm8550/thor-screen-dimmers.json",
                                 "/var/lib/steamos-sm8550/thor-screen-floors.json",
                                 "/var/lib/steamos-sm8550/thor-screen-match.json"};
const char* const RUN_DIR = "/run/sm8550-thor";
const char* const LEVEL = "/run/sm8550-thor/steam-brightness";
// Each screen's brightness, a slider position (0-1) as on Android:
// {"top": f, "bottom": f, "steam": the last value Steam wrote}. Steam
// writes its level again at every start; only a different value is a move
// of its slider, so the screens keep their own settings across restarts.
const char* const SCREENS_NAME = "thor-screen-brightness.json";
const char* const SCREENS = "/var/lib/steamos-sm8550/thor-screen-brightness.json";
const char* const STEAM_BOTTOM = "/run/sm8550-thor/steam-bottom-brightness";
const char* const FIRMWARE_TYPE = "/run/sm8550-thor/bottom-backlight-type";
const char* const DESKTOP_MAX = "/run/sm8550-thor/bottom-backlight-max";
const char* const OVERLAY = "/run/sm8550-thor/overlay";  // "1": Barry Launcher is open
const char* const TOP_INPUT = "/run/sm8550-thor/top-input";
// "1": put the bottom screen to sleep now (Barry's Screen off; DUO only).
// Read back as "0" once taken.
const char* const SLEEP = "/run/sm8550-thor/sleep";
// A touch this soon after Screen off is that tap's own finger leaving: it
// does not wake the screen.
constexpr double SLEEP_GRACE_S = 0.5;
// Desktop Mode shares SCREENS with Game Mode, through
// sm8550-thor-desktop-brightness in the Plasma session: on entering it this
// leaves DESKTOP_START (SCREENS as they were) and holds both panels there
// until the helper has set Plasma's sliders to it and made DESKTOP_READY
// (Plasma first applies its own last levels), or DESKTOP_READY_S passes.
// Plasma's top slider dims in software, so the top panel then stays at its
// highest and the helper writes that slider's position to DESKTOP_TOP; its
// bottom slider writes into Steam's file (bottom_alone).
const char* const DESKTOP_START = "/run/sm8550-thor/desktop-start.json";
const char* const DESKTOP_READY = "/run/sm8550-thor/desktop-ready";
const char* const DESKTOP_TOP = "/run/sm8550-thor/desktop-top";
// The /run/sm8550-thor entries a pass looks at (Steam's file has a watch
// of its own, on the file, as Steam writes it through the bind mount).
const char* const RUN_NAMES[] = {"overlay", "top-input", "desktop-ready", "desktop-top", "sleep"};
constexpr double DESKTOP_READY_S = 10;
constexpr double LONG_PRESS_S = 0.6;
// Barry Launcher on or off in Desktop Mode (as its menu entry and the PB-OS
// tab do), run as the desktop user.
const char* const DESKTOP_SWITCH = "/usr/lib/barry_launcher/barry_launcher_desktop_switch";
// Asleep is due, but a game draws on the bottom screen: looked at again after.
constexpr double SLEEP_CHECK_S = 5;
const char* const TOUCH_NAME = "bottom_touchscreen";
const char* const AYN_KEYS_NAME = "gpio-keys-ayn";
const char* const BL_ON = "0";   // FB_BLANK_UNBLANK
const char* const BL_OFF = "4";  // FB_BLANK_POWERDOWN
// Barry Launcher hears the flag, then comes forward and draws.
constexpr double OVERLAY_DELAY_S = 0.5;
// Steam's shortest idle dim is minutes; one nudge per this many seconds of
// touching keeps it awake.
constexpr double NUDGE_S = 20;
// Steam's display is looked for again this often after xprop failed on it.
constexpr double YIELD_RETRY_S = 2;
// A panel that refused its brightness (off, or still coming up) is tried
// again this often.
constexpr double PANEL_RETRY_S = 0.5;
// Without the kernel's process events: whether Desktop Mode (Plasma) is up
// is looked at this often, in /proc.
constexpr double MODE_CHECK_S = 2;
// While the bottom screen should be dark, a pass this often: the panel
// driver's re-powering on a modeset or wake, and a start script's re-enabled
// touchscreen, come with no event. (The Python looked every 0.1 s, always.)
constexpr double DARK_RECHECK_S = 0.5;
// The soonest a timed pass comes after the last (a guard: whatever is due
// is due at least this far ahead).
constexpr double MIN_PASS_S = 0.005;
// The charge-rest process ended but is not reaped yet (its /proc entry
// still there, which the Python counted as resting): looked at again after.
constexpr double REST_REAP_S = 0.1;
// Game Mode's start: Steam puts its own level back, fading in from half
// over half a minute or more, through the same file its slider writes.
// Until its file shows exactly the level SCREENS has from it (or this long
// has passed since this or Game Mode started), that is not a move of its
// slider, so each screen keeps its own brightness.
constexpr double STEAM_RESTORE_S = 120;
// The kernel's evdev queue for a small device; input that comes while
// resting on the charger is kept up to this many events, as it was there.
constexpr size_t KEY_QUEUE = 64;

double monotonic() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

bool env_set(const char* name) {
    const char* v = std::getenv(name);
    return v && *v && std::strcmp(v, "0") != 0;
}

// Python's OSError text, for the log.
std::string os_error(int err) { return "[Errno " + std::to_string(err) + "] " + std::strerror(err); }

// Python's int(text) for a sysfs number (0 when it is not one).
long long to_int(const std::string& s) {
    if (s.empty()) return 0;
    char* end = nullptr;
    errno = 0;
    long long v = std::strtoll(s.c_str(), &end, 10);
    return (errno || *end) ? 0 : v;
}

// Python's float(text): surrounding blanks allowed, nothing else after.
std::optional<double> py_float(const std::string& text) {
    std::string s = pq::trim(text);
    if (s.empty() || s.find_first_of("xX") != std::string::npos) return std::nullopt;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (*end) return std::nullopt;
    return v;
}

std::optional<double> py_float(const pj::Value& v) {
    if (v.is_number()) return v.as_double();
    if (v.is_bool()) return v.as_bool() ? 1.0 : 0.0;
    if (v.is_string()) return py_float(v.as_string());
    return std::nullopt;
}

// Python's int(value): numbers toward zero, strings of digits.
std::optional<long long> py_int(const pj::Value& v) {
    if (v.is_bool()) return v.as_bool() ? 1 : 0;
    if (v.is_number()) {
        double d = v.as_double();
        if (!std::isfinite(d) || std::fabs(d) > 9e18) return std::nullopt;
        return static_cast<long long>(d);
    }
    if (v.is_string()) {
        std::string s = pq::trim(v.as_string());
        size_t i = (!s.empty() && (s[0] == '-' || s[0] == '+')) ? 1 : 0;
        if (i >= s.size() || !pq::is_digits(s.substr(i)) || s.size() > 18) return std::nullopt;
        return std::stoll(s);
    }
    return std::nullopt;
}

// Python's repr of a float, as json.dump writes it (the values are 0-1).
std::string py_float_repr(double d) {
    if (std::isnan(d)) return "NaN";
    if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
    char buf[40];
    for (int prec = 1; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof buf, "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    std::string s = buf;
    if (s.find_first_of(".e") == std::string::npos) s += ".0";
    return s;
}

struct Screens {
    double top, bottom;
    long long steam;
};

// json.dumps(screens), as the Python wrote it.
std::string screens_json(const Screens& s) {
    return "{\"top\": " + py_float_repr(s.top) + ", \"bottom\": " + py_float_repr(s.bottom) +
           ", \"steam\": " + std::to_string(s.steam) + "}";
}

// Python's open(path, "w").write(value): false (and logged unless quiet)
// when the open or the write fails.
bool write(const std::string& path, const std::string& value, bool quiet = false) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    int err = 0;
    if (fd < 0) {
        err = errno;
    } else {
        ssize_t n;
        do n = ::write(fd, value.data(), value.size());
        while (n < 0 && errno == EINTR);
        if (n != static_cast<ssize_t>(value.size())) err = n < 0 ? errno : EIO;
        close(fd);
    }
    if (err && !quiet) pq::log("thor-backlight: cannot write %s to %s: %s", value.c_str(), path.c_str(), os_error(err).c_str());
    return !err;
}

// A file's identity for "read again only when it changes" (the Python
// compared mtimes alone; a replaced file with the same mtime counts too).
struct Stamp {
    long long mtime_ns = 0;
    ino_t ino = 0;
    off_t size = 0;
    bool operator==(const Stamp& o) const { return mtime_ns == o.mtime_ns && ino == o.ino && size == o.size; }
};

std::optional<Stamp> stamp(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return std::nullopt;
    return Stamp{static_cast<long long>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec, st.st_ino, st.st_size};
}

bool mounted(const std::string& path) {
    auto info = pq::read_file("/proc/self/mountinfo");
    if (!info) return false;
    for (const auto& line : pq::split_lines(*info)) {
        // ID PARENT MAJ:MIN ROOT MOUNTPOINT ...
        size_t pos = 0;
        for (int field = 0; field < 4 && pos != std::string::npos; ++field) {
            pos = line.find(' ', pos);
            if (pos != std::string::npos) ++pos;
        }
        if (pos == std::string::npos) continue;
        size_t end = line.find(' ', pos);
        if (line.compare(pos, end == std::string::npos ? std::string::npos : end - pos, path) == 0) return true;
    }
    return false;
}

// Lazily: Steam keeps STEAM_BOTTOM open through the mount, and goes on
// writing to it once a new mount is in place.
void unshadow(const std::string& path) {
    while (mounted(path)) {
        if (umount2(path.c_str(), MNT_DETACH) != 0) {
            pq::log("thor-backlight: cannot unmount %s", path.c_str());
            return;
        }
    }
}

gid_t group_id(const char* name) {
    struct group gr, *res = nullptr;
    char buf[4096];
    if (getgrnam_r(name, &gr, buf, sizeof buf, &res) == 0 && res) return res->gr_gid;
    return static_cast<gid_t>(-1);
}

// The last value Steam wrote. It may write at a growing offset without
// truncating, so take the last number and trim the file now and then.
std::optional<long long> steam_bottom() {
    auto data = pq::read_file(STEAM_BOTTOM);
    if (!data) return std::nullopt;
    if (data->size() > 4096 && truncate(STEAM_BOTTOM, 0) != 0) {
        // As os.truncate's error would have ended the Python, but quietly.
    }
    std::string& s = *data;
    std::replace(s.begin(), s.end(), '\0', ' ');
    size_t end = s.size();
    while (end > 0) {
        while (end > 0 && std::strchr(" \t\n\r\v\f", s[end - 1])) --end;
        size_t start = end;
        while (start > 0 && !std::strchr(" \t\n\r\v\f", s[start - 1])) --start;
        if (start == end) break;
        std::string word = s.substr(start, end - start);
        if (pq::is_digits(word)) return word.size() > 18 ? LLONG_MAX : std::stoll(word);
        end = start;
    }
    return std::nullopt;
}

// /sys/class/input/inputN folders whose device has this name.
std::vector<std::string> input_dirs(const char* name) {
    std::vector<std::string> out;
    for (const auto& d : pq::glob("/sys/class/input/input*"))
        if (pq::rd(d + "/name") == name) out.push_back(d);
    return out;
}

// The kernel's process events over netlink; -1 when this kernel has none
// (as pbosd_games.cpp's).
int open_proc_events() {
    int fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_CONNECTOR);
    if (fd < 0) return -1;
    sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = CN_IDX_PROC;
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    alignas(nlmsghdr) char msg[NLMSG_SPACE(sizeof(cn_msg) + sizeof(proc_cn_mcast_op))]{};
    auto* nl = reinterpret_cast<nlmsghdr*>(msg);
    nl->nlmsg_len = NLMSG_LENGTH(sizeof(cn_msg) + sizeof(proc_cn_mcast_op));
    nl->nlmsg_type = NLMSG_DONE;
    nl->nlmsg_pid = static_cast<uint32_t>(getpid());
    auto* cn = static_cast<cn_msg*>(NLMSG_DATA(nl));
    cn->id.idx = CN_IDX_PROC;
    cn->id.val = CN_VAL_PROC;
    cn->len = sizeof(proc_cn_mcast_op);
    proc_cn_mcast_op op = PROC_CN_MCAST_LISTEN;
    std::memcpy(cn->data, &op, sizeof op);
    if (send(fd, msg, nl->nlmsg_len, 0) != static_cast<ssize_t>(nl->nlmsg_len)) {
        close(fd);
        return -1;
    }
    return fd;
}

// The processes that tell the session apart: Game Mode's main gamescope
// (Steam's :0, `gamescope ... --steam`) and Plasma's KWin.
enum class Proc { Other, SteamGamescope, Kwin };

Proc classify(pid_t pid) {
    auto cmdline = pq::read_file("/proc/" + std::to_string(pid) + "/cmdline");
    if (!cmdline || cmdline->empty()) return Proc::Other;
    std::vector<std::string> args;
    size_t pos = 0;
    while (pos <= cmdline->size()) {
        size_t end = cmdline->find('\0', pos);
        if (end == std::string::npos) end = cmdline->size();
        args.push_back(cmdline->substr(pos, end - pos));
        pos = end + 1;
    }
    std::string name = pq::basename(args[0]);
    if (name == "gamescope" && std::find(args.begin(), args.end(), "--steam") != args.end()) return Proc::SteamGamescope;
    if (name == "kwin_wayland") return Proc::Kwin;
    return Proc::Other;
}

// Who owns /proc/PID, as pwd.getpwuid(os.stat(...).st_uid).
std::optional<pq::User> proc_user(pid_t pid) {
    struct stat st;
    if (stat(("/proc/" + std::to_string(pid)).c_str(), &st) != 0) return std::nullopt;
    struct passwd pw, *res = nullptr;
    char buf[4096];
    if (getpwuid_r(st.st_uid, &pw, buf, sizeof buf, &res) != 0 || !res) return std::nullopt;
    pq::User u;
    u.ok = true;
    u.name = pw.pw_name;
    u.home = pw.pw_dir;
    u.uid = pw.pw_uid;
    u.gid = pw.pw_gid;
    return u;
}

// A folder watched for its entries, also while it (or a parent) does not
// exist yet: then its nearest existing parent is watched for it to appear.
class DirWatch {
public:
    using Fn = std::function<void(uint32_t mask, const std::string& name)>;
    void start(pq::Loop& loop, std::string dir, uint32_t mask, Fn cb) {
        loop_ = &loop;
        dir_ = std::move(dir);
        mask_ = mask;
        cb_ = std::move(cb);
        arm(false);
    }

private:
    // notify: tell the owner (name empty), as the folder may have changed
    // while it was not watched.
    void arm(bool notify) {
        int gen = ++gen_;  // events for an earlier watch are dropped
        if (id_) loop_->unwatch(id_);
        id_ = 0;
        auto again = [this, gen] {
            if (gen != gen_) return false;
            ++gen_;
            loop_->after(0, [this] { arm(true); });
            return true;
        };
        if (pq::is_dir(dir_)) {
            id_ = loop_->watch(dir_, mask_ | IN_DELETE_SELF | IN_MOVE_SELF,
                               [this, gen, again](uint32_t m, const std::string& n) {
                                   if (gen != gen_) return;
                                   if (m & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                                       if (again()) cb_(m, "");
                                       return;
                                   }
                                   cb_(m, n);
                               });
            if (id_) {
                if (notify) cb_(IN_CREATE, "");
                return;
            }
        }
        // The nearest parent there is: watched for the next folder down.
        std::string parent = dir_;
        do parent = pq::dirname(parent);
        while (parent.size() > 1 && !pq::is_dir(parent));
        id_ = loop_->watch(parent, IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR,
                           [again](uint32_t, const std::string&) { again(); });
    }

    pq::Loop* loop_ = nullptr;
    std::string dir_;
    uint32_t mask_ = 0;
    Fn cb_;
    int id_ = 0, gen_ = 0;
};

// One panel's writes, on a thread of its own: a write to the bottom panel
// waits for its DSI link's next frame (17-33 ms). Only the newest value
// waits to be written (values that came meanwhile are dropped, never
// queued); each write's outcome comes back on the loop thread.
class PanelWriter {
public:
    using WriteFn = std::function<int(int value)>;  // 0, or the errno
    // On the loop thread: the write's number, value, errno and how long it took.
    using DoneFn = std::function<void(uint64_t seq, int value, int err, double took)>;

    ~PanelWriter() { stop(); }

    void start(pq::Loop& loop, WriteFn write, DoneFn done) {
        loop_ = &loop;
        write_ = std::move(write);
        done_ = std::move(done);
        thread_ = std::thread([this] { run(); });
    }

    // The value to write next, in place of any still waiting; its number.
    uint64_t submit(int value) {
        std::lock_guard<std::mutex> g(lock_);
        pending_ = {++seq_, value};
        wake_.notify_one();
        return seq_;
    }

    // Waits for a write under way; what waits is dropped.
    void stop() {
        {
            std::lock_guard<std::mutex> g(lock_);
            quit_ = true;
        }
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

private:
    void run() {
        while (true) {
            std::unique_lock<std::mutex> l(lock_);
            wake_.wait(l, [this] { return quit_ || pending_; });
            if (quit_) return;
            auto [seq, value] = *pending_;
            pending_.reset();
            l.unlock();
            double t0 = monotonic();
            int err = write_(value);
            double took = monotonic() - t0;
            loop_->post([this, seq = seq, value = value, err, took] { done_(seq, value, err, took); });
        }
    }

    pq::Loop* loop_ = nullptr;
    WriteFn write_;
    DoneFn done_;
    std::thread thread_;
    std::mutex lock_;
    std::condition_variable wake_;
    std::optional<std::pair<uint64_t, int>> pending_;
    uint64_t seq_ = 0;
    bool quit_ = false;
};

// A panel as the loop sees it: where it is going and what is under way.
struct Panel {
    PanelWriter writer;
    int shown = 0;  // the last value given to the writer (or found there)
    std::vector<std::pair<uint64_t, int>> sent;  // given, no outcome yet
    std::optional<Follow> way;

    bool busy() const { return !sent.empty(); }
    bool sending(int value) const {
        return std::any_of(sent.begin(), sent.end(), [value](const auto& s) { return s.second == value; });
    }
};

class ThorBacklight : public Module {
public:
    const char* name() const override { return "thor-backlight"; }

    ~ThorBacklight() override {
        top_.writer.stop();
        bottom_.writer.stop();
        for (int fd : {bl_fd_, key_fd_, touch_fd_, cn_})
            if (fd >= 0) close(fd);
    }

    void start(pq::Loop& loop) override {
        loop_ = &loop;
        test_sysfs_ = env_set("PBOS_PLATFORMD_TEST_THOR_SYSFS");
        if (!P->steam_both) return start_duo(loop);
        // A copy left over a panel's attributes by a run that died goes first.
        for (const char* name : {"type", "max_brightness"}) unshadow(pq::realpath(std::string(P->bottom) + "/" + name));
        bottom_max_ = static_cast<int>(to_int(pq::rd(std::string(P->bottom) + "/max_brightness", "0")));
        top_max_ = static_cast<int>(std::min<long long>(TOP_CAP, to_int(pq::rd(std::string(P->top) + "/max_brightness", "0"))));
        if (bottom_max_ <= 0 || top_max_ <= 0) fail("no usable backlights");
        setup_overlay();
        clear_desktop();
        for (const char* path : LEFTOVERS)
            if (unlink(path) == 0) pq::log("thor-backlight: removed %s: both screens follow Steam's slider", path);

        // The real bottom panel, opened before Steam's file goes over it.
        bottom_path_ = pq::realpath(std::string(P->bottom) + "/brightness");
        type_path_ = pq::realpath(std::string(P->bottom) + "/type");
        max_path_ = pq::realpath(std::string(P->bottom) + "/max_brightness");
        unshadow(bottom_path_);
        if (mounted(bottom_path_)) fail("Steam's file is still over the bottom panel; cannot reach the panel");
        bl_fd_ = open(bottom_path_.c_str(), O_RDWR | O_CLOEXEC);
        if (bl_fd_ < 0) fail(("cannot open " + bottom_path_ + ": " + os_error(errno)).c_str());
        shadowed_ = true;

        open_ayn_key();
        open_touch();
        double level;
        if (auto v = py_float(pq::rd(LEVEL))) level = *v;
        else level = level_of(bottom_set(), bottom_max_);
        write(LEVEL, fmt_level(level));
        steam_value_ = std::max<long long>(1, py_round(level * bottom_max_));
        auto screens = read_screens();
        if (!screens) {
            double f = steam_position(level);
            screens_ = {f, f, steam_value_};
            save_screens();
        } else {
            screens_ = *screens;
            if (screens_.steam > 0) steam_value_ = screens_.steam;
        }
        shadow_bottom(steam_value_);
        // Steam's writes, through the mount (and Plasma's in Desktop Mode).
        if (!loop.watch(STEAM_BOTTOM, IN_MODIFY, [this](uint32_t, const std::string&) { tick(); }))
            pq::log("thor-backlight: cannot watch Steam's brightness file");
        restoring_until_ = monotonic() + STEAM_RESTORE_S;  // see STEAM_RESTORE_S
        // What the top panel held after this last wrote it (or found it), so
        // a different value there is a change from gamescope.
        wrote_top_ = top_set();
        top_.shown = wrote_top_;
        bottom_.shown = bottom_set();
        bottom_.writer.start(
            loop,
            [this](int value) {
                std::string v = std::to_string(value);
                return pwrite(bl_fd_, v.data(), v.size(), 0) >= 0 ? 0 : errno;
            },
            [this](uint64_t seq, int value, int err, double took) { written(false, seq, value, err, took); });
        top_.writer.start(
            loop,
            [](int value) {
                std::string v = std::to_string(value);
                int fd = open((std::string(P->top) + "/brightness").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
                if (fd < 0) return errno;
                int err = ::write(fd, v.data(), v.size()) == static_cast<ssize_t>(v.size()) ? 0 : (errno ? errno : EIO);
                close(fd);
                return err;
            },
            [this](uint64_t seq, int value, int err, double took) { written(true, seq, value, err, took); });
        watch_all(loop);
        pq::log("thor-backlight: following %s (max %d) -> %s (max %d)", P->bottom, bottom_max_, P->top, top_max_);
        tick();
    }

    // The Retroid Pocket Duo (DUO): the bottom panel alone, from SCREENS
    // (see the top); Steam keeps the top panel.
    void start_duo(pq::Loop& loop) {
        bottom_max_ = static_cast<int>(to_int(pq::rd(std::string(P->bottom) + "/max_brightness", "0")));
        top_max_ = static_cast<int>(to_int(pq::rd(std::string(P->top) + "/max_brightness", "0")));
        if (bottom_max_ <= 0) fail("no usable bottom backlight");
        setup_overlay();
        bottom_path_ = pq::realpath(std::string(P->bottom) + "/brightness");
        bl_fd_ = open(bottom_path_.c_str(), O_RDWR | O_CLOEXEC);
        if (bl_fd_ < 0) fail(("cannot open " + bottom_path_ + ": " + os_error(errno)).c_str());
        open_touch();
        // First start: both sliders where the bottom panel is now.
        if (auto screens = read_screens()) {
            screens_ = *screens;
        } else {
            double f = level_of(bottom_set(), bottom_max_);
            screens_ = {f, f, 0};
            save_screens();
        }
        bottom_.shown = bottom_set();
        bottom_.writer.start(
            loop,
            [this](int value) {
                std::string v = std::to_string(value);
                return pwrite(bl_fd_, v.data(), v.size(), 0) >= 0 ? 0 : errno;
            },
            [this](uint64_t seq, int value, int err, double took) { written(false, seq, value, err, took); });
        watch_all(loop);
        pq::log("thor-backlight: following SCREENS on %s (max %d); %s is Steam's", P->bottom, bottom_max_, P->top);
        tick();
    }

    // What brings a pass, both profiles.
    void watch_all(pq::Loop& loop) {
        loop.watch(RUN_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_Q_OVERFLOW,
                   [this](uint32_t mask, const std::string& name) {
                       if (mask & IN_Q_OVERFLOW) return tick();
                       for (const char* n : RUN_NAMES)
                           if (name == n) return tick();
                   });
        var_watch_.start(loop, VAR_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM,
                         [this](uint32_t, const std::string& name) {
                             if (name.empty() || name == STATE_NAME || name == SCREENS_NAME) tick();
                         });
        pq::User steamos = pq::user_named("steamos");
        barry_settings_ = (steamos.ok ? steamos.home : std::string("~steamos")) + "/.config/barry_launcher/settings.json";
        settings_watch_.start(loop, pq::dirname(barry_settings_),
                              IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM,
                              [this](uint32_t, const std::string& name) {
                                  if (name.empty() || name == "settings.json") tick();
                              });
        loop.watch(CHARGE_REST_DIR, IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM,
                   [this](uint32_t, const std::string& name) {
                       if (name == CHARGE_REST_NAME) tick();
                   });
        loop.on_uevent_env([this](const pq::Loop::UeventEnv& env) { uevent(env); });
        if (test_sysfs_) {
            // Test only (tests/thor_backlight): a fake sysfs sends no uevents;
            // its files' writes stand in for them.
            for (std::string f : {std::string(P->top) + "/brightness", std::string(P->bottom) + "/bl_power"})
                loop.watch(f, IN_MODIFY | IN_CLOSE_WRITE, [this](uint32_t, const std::string&) { backlight_changed(); });
        }
        if (!env_set("PBOS_PLATFORMD_TEST_THOR_SCAN")) {
            cn_ = open_proc_events();
            if (cn_ < 0)
                pq::log("thor-backlight: no process events from this kernel (%s), looking at /proc every %g s",
                        std::strerror(errno), MODE_CHECK_S);
        }
        scan_procs();
        if (cn_ >= 0) loop.on_readable(cn_, [this] { drain_procs(); });
    }

    void stop() override {
        if (timer_) loop_->cancel(timer_);
        timer_ = 0;
        if (top_settle_) loop_->cancel(top_settle_);
        top_settle_ = 0;
        top_.writer.stop();
        bottom_.writer.stop();
        if (!shadowed_) return;
        unshadow(type_path_);
        unshadow(max_path_);
        unshadow(bottom_path_);
    }

private:
    // As the Python's exit 1 (the unit restarts it), with its finally.
    [[noreturn]] void fail(const char* why) {
        pq::log("thor-backlight: %s", why);
        stop();
        std::exit(1);
    }

    static std::string fmt_level(double level) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.5f", level);
        return buf;
    }

    // Session-writable flags: barry_launcher_shelld opens and closes Barry
    // (OVERLAY) too, and keeps TOP_INPUT.
    void setup_overlay() {
        pq::makedirs(RUN_DIR);
        gid_t gid = group_id("input");
        if (chown(RUN_DIR, static_cast<uid_t>(-1), gid) != 0 || chmod(RUN_DIR, 02775) != 0)
            pq::log("thor-backlight: cannot set up %s: %s", RUN_DIR, std::strerror(errno));
        for (const char* flag : {OVERLAY, TOP_INPUT, SLEEP}) {
            if (flag == SLEEP && !P->sleep_flag) continue;
            write(flag, "0");
            if (chown(flag, static_cast<uid_t>(-1), gid) != 0 || chmod(flag, 0664) != 0)
                pq::log("thor-backlight: cannot set up %s: %s", flag, std::strerror(errno));
        }
    }

    // A Desktop Mode session's files, gone once it has ended.
    static void clear_desktop() {
        for (const char* path : {DESKTOP_START, DESKTOP_READY, DESKTOP_TOP}) unlink(path);
    }

    // Steam's writes to the bottom panel go to STEAM_BOTTOM from now on.
    void shadow_bottom(long long value) {
        if (!write(STEAM_BOTTOM, std::to_string(value) + "\n")) fail("cannot write Steam's file");
        if (chown(STEAM_BOTTOM, static_cast<uid_t>(-1), group_id("video")) != 0 || chmod(STEAM_BOTTOM, 0664) != 0)
            fail(("cannot set up " + std::string(STEAM_BOTTOM) + ": " + os_error(errno)).c_str());
        if (mount(STEAM_BOTTOM, bottom_path_.c_str(), nullptr, MS_BIND, nullptr) != 0)
            fail(("cannot put Steam's file over " + bottom_path_ + ": " + os_error(errno)).c_str());
    }

    // Desktop Mode: PowerDevil's backlight helper takes every backlight of
    // the best type it finds and writes them all; both panels are "raw", so
    // the bottom slider moved the top screen too, and the helper read its
    // value back from the top panel. With "firmware" over the bottom panel's
    // type it takes that one alone. PowerDevil reads the range once, from the
    // top panel if it came first: the bottom's max_brightness shows the top's
    // too, so the slider's values come on that one scale either way. Off for
    // Game Mode, whose gamescope picks its backlight by type as well.
    void bottom_alone(bool on) {
        const std::pair<const std::string*, std::pair<const char*, std::string>> files[] = {
            {&type_path_, {FIRMWARE_TYPE, "firmware"}}, {&max_path_, {DESKTOP_MAX, std::to_string(top_max_)}}};
        for (const auto& [path, what] : files) {
            const auto& [src, value] = what;
            if (!on) {
                unshadow(*path);
                continue;
            }
            if (mounted(*path)) continue;
            write(src, value + "\n");
            chmod(src, 0644);
            if (mount(src, path->c_str(), nullptr, MS_BIND, nullptr) != 0)
                pq::log("thor-backlight: cannot put %s over %s", value.c_str(), path->c_str());
        }
    }

    // SCREENS, read again only when the file changes; nullopt if unset.
    std::optional<Screens> read_screens() {
        auto st = stamp(SCREENS);
        if (!st) return std::nullopt;
        if (!screens_stamp_ || !(*screens_stamp_ == *st)) {
            screens_stamp_ = st;
            screens_cache_.reset();
            auto text = pq::read_file(SCREENS);
            auto d = text ? pj::Value::parse(*text) : std::nullopt;
            if (d && d->is_object() && d->has("top") && d->has("bottom")) {
                auto top = py_float((*d)["top"]), bottom = py_float((*d)["bottom"]);
                std::optional<long long> steam = -1;
                if (d->has("steam")) steam = py_int((*d)["steam"]);
                if (top && bottom && steam) screens_cache_ = Screens{clamp01(*top), clamp01(*bottom), *steam};
            }
        }
        return screens_cache_;
    }

    void save_screens() {
        std::string tmp = std::string(SCREENS) + ".tmp";
        int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        int err = fd < 0 ? errno : 0;
        if (fd >= 0) {
            std::string text = screens_json(screens_) + "\n";
            if (::write(fd, text.data(), text.size()) != static_cast<ssize_t>(text.size())) err = errno ? errno : EIO;
            close(fd);
            if (!err && rename(tmp.c_str(), SCREENS) != 0) err = errno;
        }
        if (err) pq::log("thor-backlight: cannot save %s: %s", SCREENS, os_error(err).c_str());
    }

    // Barry Launcher's bottomSleepSeconds (0: never), read again only when
    // its settings file changes.
    int sleep_seconds() {
        auto st = stamp(barry_settings_);
        if (!st) return 0;
        if (!settings_stamp_ || !(*settings_stamp_ == *st)) {
            settings_stamp_ = st;
            sleep_limit_ = 0;
            auto text = pq::read_file(barry_settings_);
            auto d = text ? pj::Value::parse(*text) : std::nullopt;
            if (d && d->is_object()) {
                const pj::Value* v = d->find("bottomSleepSeconds");
                if (v && v->is_number() && v->as_double() > 0)
                    sleep_limit_ = static_cast<int>(std::min(v->as_double(), 1e9));
            }
        }
        return sleep_limit_;
    }

    // sm8550-charge-rest is resting (CHARGE_REST holds its live PID).
    bool charge_rest() {
        std::string pid = pq::rd(CHARGE_REST);
        if (!pq::is_digits(pid) || pid.size() > 9 || !pq::exists("/proc/" + pid)) {
            rest_pid_.clear();
            return false;
        }
        // Its end (pidfd) brings the next pass; until it is reaped (its
        // /proc entry gone), a look every REST_REAP_S.
        if (pid != rest_pid_) {
            rest_pid_ = pid;
            rest_ended_ = !loop_->on_exit(static_cast<pid_t>(std::stol(pid)), [this, pid] {
                if (rest_pid_ != pid) return;
                rest_ended_ = true;
                tick();
            });
        }
        if (rest_ended_ && !rest_timer_)
            rest_timer_ = loop_->after(REST_REAP_S, [this] {
                rest_timer_ = 0;
                tick();
            });
        return true;
    }

    // --- Devices ---

    // /dev/input/eventN of the first device with this name that opens.
    int open_input(const char* name, bool grab) {
        for (const auto& d : input_dirs(name)) {
            for (const auto& ev : pq::glob(d + "/event*")) {
                std::string dev = "/dev/input/" + pq::basename(ev);
                int fd = open(dev.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd < 0) continue;
                if (grab && ioctl(fd, EVIOCGRAB, 1) != 0) {
                    pq::log("thor-backlight: cannot grab %s: %s", name, os_error(errno).c_str());
                    close(fd);
                    return -2;
                }
                return fd;
            }
        }
        return -1;
    }

    void open_ayn_key() {
        if (key_fd_ >= 0) return;
        int fd = open_input(AYN_KEYS_NAME, true);
        if (fd == -2 && !key_retry_) {
            // Grabbed by someone else: no event says when that ends.
            key_retry_ = loop_->after(5, [this] {
                key_retry_ = 0;
                open_ayn_key();
            });
        }
        if (fd < 0) return;
        key_fd_ = fd;
        pq::log("thor-backlight: AYN button on %s", fd_name(fd).c_str());
        loop_->on_readable(fd, [this] { read_key(); });
    }

    void open_touch() {
        touch_dirs_ = input_dirs(TOUCH_NAME);
        if (touch_fd_ >= 0) return;
        int fd = open_input(TOUCH_NAME, false);
        if (fd < 0) return;
        touch_fd_ = fd;
        pq::log("thor-backlight: bottom touchscreen on %s", fd_name(fd).c_str());
        loop_->on_readable(fd, [this] { read_touch(); });
    }

    static std::string fd_name(int fd) {
        char buf[PATH_MAX];
        ssize_t n = readlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), buf, sizeof buf - 1);
        return n > 0 ? pq::basename(std::string(buf, static_cast<size_t>(n))) : "?";
    }

    // Reads what is there; false when the device is gone (closed here,
    // which also takes it out of the loop; its uevent brings it back).
    bool drain(int& fd, std::vector<input_event>& out) {
        input_event evs[64];
        while (fd >= 0) {
            ssize_t n = read(fd, evs, sizeof evs);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) return true;
            if (n <= 0) {
                close(fd);
                fd = -1;
                return false;
            }
            out.insert(out.end(), evs, evs + n / static_cast<ssize_t>(sizeof(input_event)));
        }
        return false;
    }

    // AYN button: a short press opens or closes Barry Launcher (on release);
    // holding it turns the bottom screen off or on (once held long enough).
    void read_key() {
        std::vector<input_event> evs;
        if (!drain(key_fd_, evs)) {
            pressed_at_.reset();
            key_queue_.clear();
        }
        if (charge_rest()) {
            // Not read while resting, as the Python's unread device file.
            key_queue_.insert(key_queue_.end(), evs.begin(), evs.end());
            if (key_queue_.size() > KEY_QUEUE) key_queue_.erase(key_queue_.begin(), key_queue_.end() - KEY_QUEUE);
            return;
        }
        if (!key_queue_.empty()) {
            evs.insert(evs.begin(), key_queue_.begin(), key_queue_.end());
            key_queue_.clear();
        }
        keys(evs);
        tick();
    }

    void keys(const std::vector<input_event>& evs) {
        for (const auto& e : evs) {
            if (e.type != EV_KEY || e.code != KEY_F24) continue;
            if (e.value == 1) {
                last_active_ = monotonic();
                if (asleep_) {
                    // Only wakes the bottom screen.
                    asleep_ = false;
                    pressed_at_.reset();
                    pq::log("thor-backlight: AYN button: bottom screen wakes");
                    continue;
                }
                pressed_at_ = monotonic();
            } else if (e.value == 0 && pressed_at_) {
                pressed_at_.reset();
                if (desktop_) {
                    if (switching_ && *switching_) {
                        pq::log("thor-backlight: AYN button: short press (Barry Launcher still switching)");
                    } else {
                        switching_ = toggle_desktop_barry();
                        pq::log(switching_ ? "thor-backlight: AYN button: Barry Launcher on or off in Desktop Mode"
                                           : "thor-backlight: AYN button: short press (no desktop user)");
                    }
                } else {
                    bool opening = pq::rd(OVERLAY, "0") != "1";
                    // Pressed while the bottom screen is off: it wakes (the
                    // switch goes on) with Barry open, so Barry's standby
                    // then shows the game art, not black.
                    if (opening && pq::rd(STATE, "on") == "off") {
                        write(STATE, "on");
                        pq::log("thor-backlight: AYN button: bottom screen on");
                    }
                    write(OVERLAY, opening ? "1" : "0");
                    pq::log("thor-backlight: AYN button: Barry Launcher %s", opening ? "opens" : "closes");
                }
            }
        }
    }

    // Bottom-screen touches count as activity for Steam's idle dim.
    void read_touch() {
        std::vector<input_event> evs;
        drain(touch_fd_, evs);
        bool touched = std::any_of(evs.begin(), evs.end(), [](const input_event& e) {
            return e.type == EV_KEY || e.type == EV_ABS;
        });
        if (charge_rest()) {
            touch_queued_ = touch_queued_ || touched;
            return;
        }
        touched = touched || touch_queued_;
        touch_queued_ = false;
        if (touched && slept_by_hand_ && monotonic() - slept_at_ < SLEEP_GRACE_S) touched = false;
        if (touched) {
            last_active_ = monotonic();
            if (asleep_) {
                asleep_ = false;
                pq::log("thor-backlight: bottom touch: bottom screen wakes");
            }
        }
        if (touched && !(nudge_ && *nudge_) && monotonic() - nudged_at_ >= NUDGE_S) {
            nudged_at_ = monotonic();
            nudge_ = nudge_steam();
            pq::log("thor-backlight: bottom touch: %s", nudge_ ? "nudged Steam" : "Game Mode not running");
        }
        tick();
    }

    void uevent(const pq::Loop::UeventEnv& env) {
        auto get = [&env](const char* key) {
            auto it = env.find(key);
            return it == env.end() ? std::string() : it->second;
        };
        std::string sub = get("SUBSYSTEM"), action = get("ACTION");
        if (env.empty() || sub == "input") {
            // Empty: some were dropped; look anyway.
            if (env.empty() || action == "add" || action == "remove") {
                if (key_fd_ < 0 && P->ayn_button) open_ayn_key();
                open_touch();
            }
            if (env.empty() || action == "add") tick();
        } else if (sub == "backlight") {
            backlight_changed();
        } else if (sub == "drm") {
            // A connector or modeset change, after which the switch is put
            // back.
            tick();
        }
    }

    // A write to either panel: a pass for gamescope's to the top one, or
    // for a dark panel powered up. This one's own writes come back as
    // events too, and a pass for those would write the next follow step at
    // once, over and over. gamescope's top write comes with a move of
    // Steam's slider, often a few ms before Steam's file does: its pass
    // waits a follow step (GLIDE_MS, the Python's pace too), so Steam's
    // level, read first, wins as it did there. Taken at once, the top
    // level and Steam's took turns and the panels jumped.
    void backlight_changed() {
        if (!on_ && pq::rd(std::string(P->bottom) + "/bl_power") != BL_OFF) {
            tick();
            return;
        }
        if (!P->steam_both) return;  // the top panel is Steam's alone
        int top_now = top_set();
        if (top_now != wrote_top_ && !top_.sending(top_now) && !top_settle_)
            top_settle_ = loop_->after(GLIDE_MS / 1000.0, [this] {
                top_settle_ = 0;
                tick();
            });
    }

    // --- The session's processes ---

    // Everything in /proc, as the Python's desktop_mode() looked.
    void scan_procs() {
        procs_.clear();
        for (const auto& p : pq::listdir("/proc")) {
            if (!pq::is_digits(p) || p.size() > 9) continue;
            pid_t pid = static_cast<pid_t>(std::atol(p.c_str()));
            Proc k = classify(pid);
            if (k != Proc::Other) procs_[pid] = k;
        }
        mode_checked_ = monotonic();
    }

    void drain_procs() {
        alignas(nlmsghdr) char buf[16384];
        bool changed = false;
        while (true) {
            ssize_t n = recv(cn_, buf, sizeof buf, 0);
            if (n < 0) {
                if (errno == ENOBUFS) {
                    scan_procs();  // events were dropped
                    changed = true;
                    continue;
                }
                break;
            }
            if (n == 0) break;
            for (auto* nl = reinterpret_cast<nlmsghdr*>(buf); NLMSG_OK(nl, static_cast<unsigned>(n));
                 nl = NLMSG_NEXT(nl, n)) {
                if (nl->nlmsg_type == NLMSG_ERROR || nl->nlmsg_type == NLMSG_NOOP) continue;
                auto* cn = static_cast<cn_msg*>(NLMSG_DATA(nl));
                if (cn->id.idx != CN_IDX_PROC || cn->id.val != CN_VAL_PROC) continue;
                changed = on_proc_event(*reinterpret_cast<const struct proc_event*>(cn->data)) || changed;
            }
        }
        if (changed) tick();
    }

    // True when the set of Game Mode / Plasma processes changed.
    bool on_proc_event(const struct proc_event& ev) {
        switch (ev.what) {
        case PROC_EVENT_FORK: {
            // A child before its exec has its parent's command line, as a
            // look through /proc would see it.
            pid_t parent = ev.event_data.fork.parent_tgid, child = ev.event_data.fork.child_pid;
            if (child != ev.event_data.fork.child_tgid) return false;
            auto it = procs_.find(parent);
            if (it == procs_.end()) return false;
            procs_[child] = it->second;
            return true;
        }
        case PROC_EVENT_EXEC: {
            pid_t pid = ev.event_data.exec.process_tgid;
            if (ev.event_data.exec.process_pid != pid) return false;
            Proc k = classify(pid);
            if (k == Proc::Other) return procs_.erase(pid) > 0;
            procs_[pid] = k;
            return true;
        }
        case PROC_EVENT_EXIT: {
            pid_t pid = ev.event_data.exit.process_pid;
            if (pid != ev.event_data.exit.process_tgid) return false;
            return procs_.erase(pid) > 0;
        }
        default:
            return false;
        }
    }

    // Without process events the Python's look at /proc: every MODE_CHECK_S
    // for the mode, at once for a user to run something as.
    void fresh_procs(bool for_user) {
        if (cn_ >= 0) return;
        if (for_user || monotonic() - mode_checked_ >= MODE_CHECK_S) scan_procs();
    }

    // Plasma's KWin runs and Game Mode's gamescope does not.
    bool desktop_mode() {
        fresh_procs(false);
        bool kwin = false;
        for (const auto& [pid, k] : procs_) {
            if (k == Proc::SteamGamescope) return false;
            kwin = kwin || k == Proc::Kwin;
        }
        return kwin;
    }

    // The user running the first process of this kind.
    std::optional<pq::User> user_of(Proc kind) {
        fresh_procs(true);
        for (const auto& [pid, k] : procs_)
            if (k == kind)
                if (auto u = proc_user(pid)) return u;
        return std::nullopt;
    }

    // --- Steam's display and the desktop session ---

    // Started and left to run; the flag is true until it has ended.
    static std::shared_ptr<std::atomic<bool>> launch(std::vector<std::string> argv, const char* what) {
        auto running = std::make_shared<std::atomic<bool>>(true);
        pq::Loop::work([argv = std::move(argv), running, what] {
            pq::RunOpts o;
            o.capture = false;
            o.timeout = 3600;
            if (!pq::run(argv, o).started) pq::log("thor-backlight: cannot %s", what);
            *running = false;
        });
        return running;
    }

    // Barry Launcher off (the bottom screen the desktop's) or on again.
    std::shared_ptr<std::atomic<bool>> toggle_desktop_barry() {
        auto user = user_of(Proc::Kwin);
        if (!user || !pq::can_exec(DESKTOP_SWITCH)) return nullptr;
        std::string runtime = "/run/user/" + std::to_string(user->uid);
        return launch({"runuser", "-u", user->name, "--", "env", "HOME=" + user->home, "XDG_RUNTIME_DIR=" + runtime,
                      "DBUS_SESSION_BUS_ADDRESS=unix:path=" + runtime + "/bus", "BARRY_SWITCH_QUIET=1",
                      DESKTOP_SWITCH, "toggle"},
                     "switch Barry Launcher");
    }

    std::shared_ptr<std::atomic<bool>> nudge_steam() {
        auto user = user_of(Proc::SteamGamescope);
        if (!user) return nullptr;
        return launch({"runuser", "-u", user->name, "--", "env", "DISPLAY=:0", "xdotool", "key", "Shift_R"}, "nudge Steam");
    }

    // xprop -root ARGS on Steam's display; nullopt if Game Mode is not
    // running (or xprop could not be run).
    std::optional<pq::RunResult> steam_xprop(const std::vector<std::string>& args, bool* no_display = nullptr) {
        auto user = user_of(Proc::SteamGamescope);
        if (no_display) *no_display = !user;
        if (!user) return std::nullopt;
        std::vector<std::string> argv = {"runuser", "-u", user->name, "--", "env", "DISPLAY=:0", "xprop", "-root"};
        argv.insert(argv.end(), args.begin(), args.end());
        pq::RunOpts o;
        o.timeout = 5;
        pq::RunResult r = pq::run(argv, o);
        if (!r.started || r.timed_out) {
            pq::log("thor-backlight: cannot reach Steam's display: %s", r.timed_out ? "timed out" : r.err.c_str());
            return std::nullopt;
        }
        return r;
    }

    // Steam's gamescope shows a game's window on the bottom screen
    // (GAMESCOPE_BOTTOM_SCREEN_SHOWING 1).
    bool bottom_screen_game() {
        auto r = steam_xprop({"GAMESCOPE_BOTTOM_SCREEN_SHOWING"});
        if (!r || r->code != 0) return false;
        size_t eq = r->out.rfind('=');
        return pq::trim(eq == std::string::npos ? r->out : r->out.substr(eq + 1)) == "1";
    }

    // GAMESCOPE_BOTTOM_SCREEN_YIELD on Steam's display; false if Game Mode
    // is not running (no_display: no Game Mode at all).
    bool set_bottom_screen_yield(bool on, bool& no_display) {
        const std::string prop = "GAMESCOPE_BOTTOM_SCREEN_YIELD";
        auto r = steam_xprop(on ? std::vector<std::string>{"-f", prop, "32c", "-set", prop, "1"}
                                : std::vector<std::string>{"-remove", prop},
                             &no_display);
        return r && r->code == 0;
    }

    // --- The panels ---

    // Not actual_brightness: that reads 0 while bl_power is off.
    int bottom_set() {
        char buf[32];
        ssize_t n = pread(bl_fd_, buf, sizeof buf - 1, 0);
        if (n <= 0) return 0;
        return static_cast<int>(to_int(pq::trim(std::string_view(buf, static_cast<size_t>(n)))));
    }

    // The value to the panel's writer (which takes the newest).
    uint64_t submit(Panel& p, int value) {
        p.shown = value;
        uint64_t seq = p.writer.submit(value);
        p.sent.emplace_back(seq, value);
        return seq;
    }

    // A write's outcome, on the loop thread: what the Python did right
    // after its own write.
    void written(bool top, uint64_t seq, int value, int err, double took) {
        Panel& p = top ? top_ : bottom_;
        // Values given before this one and dropped for it never come.
        p.sent.erase(std::remove_if(p.sent.begin(), p.sent.end(), [seq](const auto& s) { return s.first <= seq; }),
                     p.sent.end());
        bool refused = err != 0;
        bool& was = top ? top_refused_ : bottom_refused_;
        std::string path = top ? std::string(P->top) + "/brightness" : bottom_path_;
        if (refused && !was)
            pq::log("thor-backlight: cannot write %d to %s: %s", value, path.c_str(), os_error(err).c_str());
        if (top) {
            // Even refused, the value is now in sysfs: this one's own, not a
            // level gamescope set. Taking it for one fed each refused write
            // back in as the new level, down to 1, and the screens stayed
            // black once they came back on.
            wrote_top_ = value;
            if (desktop_) {
                if (refused) top_refused_ = true;  // as the Python: not cleared there
            } else {
                if (refused != top_refused_)
                    pq::log("thor-backlight: top panel %s brightness %d", refused ? "refuses" : "took", value);
                top_refused_ = refused;
            }
        } else {
            if (refused != bottom_refused_)
                pq::log("thor-backlight: bottom panel %s brightness %d", refused ? "refuses" : "took", value);
            bottom_refused_ = refused;
            if (power_on_seq_ && seq >= power_on_seq_) {
                // Back on: at its level, then power up and allow touch.
                power_on_seq_ = 0;
                if (on_) {
                    write(std::string(P->bottom) + "/bl_power", BL_ON);
                    set_touch_inhibited(false, false);
                }
            }
        }
        if (refused) panel_retry_at_ = monotonic() + PANEL_RETRY_S;
        // A slow write (the bottom panel's take a frame) on the way, nothing
        // waiting: the next step at once, not at the next pass, so the panel
        // is never left idle. Quick writes keep the passes' GLIDE_MS pace.
        if (took >= GLIDE_MS / 1000.0 && !p.busy() && p.way && !charge_rest() && (top || !desktop_ || on_)) {
            int want = top ? (desktop_ ? (ready_ ? top_max_ : stock_top(screens_.top, top_max_))
                                       : stock_top(screens_.top, top_max_))
                           : bottom_target();
            int step = follow(p.way, want, p.shown, monotonic());
            if (step != p.shown) submit(p, step);
        }
        arm();
    }

    // Where the bottom panel goes for its slider: AYN's curve on the Thor, a
    // straight line on the Duo.
    int bottom_target() const {
        return P->steam_both ? stock_bottom(screens_.bottom, bottom_max_) : linear_bottom(screens_.bottom, bottom_max_);
    }

    static int top_set() { return static_cast<int>(to_int(pq::rd(std::string(P->top) + "/brightness", "0"))); }

    void set_touch_inhibited(bool on, bool only_if_differs) {
        for (const auto& d : touch_dirs_) {
            std::string p = d + "/inhibited";
            if (only_if_differs && pq::rd(p) == (on ? "1" : "0")) continue;
            write(p, on ? "1" : "0");
        }
    }

    // --- One pass (the Python loop's body after reading its devices) ---

    void tick() {
        if (charge_rest()) {
            // Resting on the charger: sm8550-charge-rest has the screens off
            // and puts them back. Hands off until then.
            if (timer_) loop_->cancel(timer_);
            timer_ = 0;
            return;
        }
        if (!key_queue_.empty() || touch_queued_) {
            // Input that came while resting, now.
            if (!key_queue_.empty()) {
                std::vector<input_event> evs;
                evs.swap(key_queue_);
                keys(evs);
            }
            if (touch_queued_) return read_touch();  // which comes back here
        }
        if (key_fd_ < 0) pressed_at_.reset();

        if (pressed_at_ && monotonic() - *pressed_at_ >= LONG_PRESS_S) {
            pressed_at_.reset();  // the release does nothing more
            // Off as the switch would be: Barry closes too, or it would keep
            // the screen lit.
            bool turning_off = pq::rd(STATE, "on") != "off";
            write(STATE, turning_off ? "off" : "on");
            if (turning_off && pq::rd(OVERLAY, "0") == "1") write(OVERLAY, "0");
            pq::log("thor-backlight: AYN button held: bottom screen %s", turning_off ? "off" : "on");
        }

        bool overlay = pq::rd(OVERLAY, "0") == "1";
        want_yield_ = overlay || pq::rd(TOP_INPUT, "0") == "1";
        if ((!yield_sent_ || *yield_sent_ != want_yield_) && monotonic() >= yield_retry_at_) {
            bool no_display = false;
            if (set_bottom_screen_yield(want_yield_, no_display)) {
                yield_sent_ = want_yield_;
                pq::log("thor-backlight: bottom screen %s Barry Launcher", want_yield_ ? "yielded to" : "back from");
            } else if (want_yield_) {
                // No Game Mode: its gamescope's start (a process event) is
                // the next try; without those, and after a failed xprop,
                // YIELD_RETRY_S.
                yield_retry_at_ = (no_display && cn_ >= 0) ? 0 : monotonic() + YIELD_RETRY_S;
            } else {
                yield_sent_ = false;  // no Game Mode, nothing to take back
            }
        }
        if (overlay && !overlay_since_) overlay_since_ = monotonic();
        else if (!overlay) overlay_since_.reset();
        switch_on_ = pq::rd(STATE, "on") != "off" ||
                     (overlay_since_ && monotonic() - *overlay_since_ >= OVERLAY_DELAY_S);
        // Sleep: untouched for bottomSleepSeconds (not in Desktop Mode, nor
        // while a game draws on the bottom screen).
        limit_ = sleep_seconds();
        // Barry's Screen off (DUO): asleep now, Barry closed (as the AYN
        // button's hold closes it); a touch wakes it, whatever the timer.
        if (P->sleep_flag && pq::rd(SLEEP, "0") == "1") {
            write(SLEEP, "0");
            if (switch_on_ && !desktop_ && !asleep_) {
                asleep_ = true;
                slept_by_hand_ = true;
                slept_at_ = monotonic();
                if (overlay) write(OVERLAY, "0");
                pq::log("thor-backlight: Screen off: bottom screen asleep until a touch");
            }
        }
        if (asleep_ && limit_ == 0 && !slept_by_hand_) {
            asleep_ = false;  // the timer was turned off: awake at once
            last_active_ = monotonic();
            pq::log("thor-backlight: bottom screen wakes: sleep turned off");
        }
        if (!asleep_ && switch_on_ && limit_ > 0 && !desktop_ && monotonic() - last_active_ >= limit_ &&
            monotonic() - game_checked_ >= SLEEP_CHECK_S) {
            game_checked_ = monotonic();
            if (bottom_screen_game()) {
                last_active_ = monotonic();
            } else {
                asleep_ = true;
                pq::log("thor-backlight: bottom screen asleep: untouched for %d s", limit_);
            }
        }
        if (!switch_on_) {
            asleep_ = false;
            last_active_ = monotonic();  // turned on again: a fresh start
        }
        if (!asleep_) slept_by_hand_ = false;
        on_ = switch_on_ && !asleep_;
        std::string bl_power = std::string(P->bottom) + "/bl_power";
        if (!switch_on_) {
            power_on_seq_ = 0;
            if (pq::rd(bl_power) != BL_OFF) write(bl_power, BL_OFF);
            set_touch_inhibited(true, true);
        } else if (asleep_) {
            // Dark, but touch stays on: a touch wakes it.
            power_on_seq_ = 0;
            if (pq::rd(bl_power) != BL_OFF) write(bl_power, BL_OFF);
        } else if (was_on_ && !*was_on_) {
            // Back on: at its level, then (once written, written()) power up
            // and allow touch.
            bottom_.way.reset();
            power_on_seq_ = submit(bottom_, bottom_target());
        }
        if (!was_on_ || *was_on_ != on_) pq::log("thor-backlight: bottom screen %s", on_ ? "on" : "off");
        was_on_ = on_;

        bool desktop_now = desktop_mode();
        if (desktop_now != desktop_ && !P->steam_both) {
            desktop_ = !desktop_;
            pq::log(desktop_ ? "thor-backlight: Desktop Mode" : "thor-backlight: Game Mode");
        } else if (desktop_now != desktop_) {
            desktop_ = !desktop_;
            bottom_alone(desktop_);
            if (desktop_) {
                // The helper may have been quicker; it then read SCREENS.
                desktop_since_ = monotonic();
                desktop_top_seen_.reset();
                if (!pq::exists(DESKTOP_READY)) write(DESKTOP_START, screens_json(screens_) + "\n");
            } else {
                clear_desktop();
                restoring_until_ = monotonic() + STEAM_RESTORE_S;
            }
            if (!desktop_ && screens_.steam > 0) {
                // Plasma's bottom slider wrote into Steam's file: it gets
                // back what Steam last wrote, which a move of Steam's slider
                // is told from.
                steam_value_ = screens_.steam;
                write(STEAM_BOTTOM, std::to_string(steam_value_) + "\n");
            }
            pq::log(desktop_ ? "thor-backlight: Desktop Mode: Plasma's sliders, sharing Game Mode's brightness"
                             : "thor-backlight: Game Mode: Steam's level on both, as on Android");
        }
        if (!P->steam_both) duo_pass();
        else if (desktop_) desktop_pass();
        else game_pass();
        arm();
    }

    // Plasma has a slider per screen, and they set SCREENS as the Barry
    // Launcher plugin's do in Game Mode: the bottom one writes into Steam's
    // file (STEAM_BOTTOM) on the top panel's scale (bottom_alone), the top
    // one dims in software and comes through DESKTOP_TOP. Until the helper
    // is ready, both panels hold SCREENS. The top never goes above TOP_CAP,
    // whoever writes it.
    void desktop_pass() {
        ready_ = pq::exists(DESKTOP_READY) || monotonic() - desktop_since_ >= DESKTOP_READY_S;
        auto steam_now = steam_bottom();
        if (ready_) {
            bool changed = false;
            if (steam_now && *steam_now != steam_value_) {
                screens_.bottom = level_of(*steam_now, top_max_);
                changed = true;
            }
            auto st = stamp(DESKTOP_TOP);
            if (st && !(desktop_top_seen_ && *desktop_top_seen_ == *st)) {
                desktop_top_seen_ = st;
                if (auto v = py_float(pq::rd(DESKTOP_TOP))) {
                    screens_.top = clamp01(*v);
                    changed = true;
                }
            }
            if (changed) save_screens();
        }
        if (steam_now) steam_value_ = *steam_now;
        // The bottom panel follows only while lit (once lit again, it is
        // written at its level at once).
        if (on_) {
            int want_bottom = follow(bottom_.way, bottom_target(), bottom_.shown, monotonic());
            if (want_bottom != bottom_.shown || (bottom_refused_ && monotonic() >= panel_retry_at_))
                submit(bottom_, want_bottom);
        } else {
            bottom_.way.reset();
        }
        int top_now = top_set();
        if (!top_.busy()) {
            resync_top(top_now);
            wrote_top_ = top_now;  // Plasma's: no level for Game Mode
        }
        int want_top = follow(top_.way, ready_ ? top_max_ : stock_top(screens_.top, top_max_), top_.shown, monotonic());
        if (want_top != top_.shown) submit(top_, want_top);
    }

    // Someone else's value on the top panel (nothing of this one's under
    // way): followed on from there.
    void resync_top(int top_now) {
        if (top_now == top_.shown) return;
        top_.shown = top_now;
        top_.way.reset();
    }

    // Steam's level: a new value in Steam's file (or on the top panel) sets
    // both screens, as Android's "both screens" slider does; the plugin's
    // sliders change one screen in SCREENS.
    void game_pass() {
        if (auto s = read_screens()) screens_ = *s;
        auto steam_now = steam_bottom();
        int top_now = top_set();
        bool moved = false;
        double level = 0;
        if (steam_now && *steam_now != steam_value_) {
            steam_value_ = *steam_now;
            if (restoring_until_ != 0 && *steam_now == screens_.steam) {
                pq::log("thor-backlight: Steam's level back at %lld: each screen keeps its own", *steam_now);
                restoring_until_ = 0;
            } else if (restoring_until_ != 0 && monotonic() < restoring_until_) {
                // Steam still putting its level back.
            } else {
                if (restoring_until_ != 0) {
                    pq::log("thor-backlight: Steam's level not back after Game Mode's start: its slider sets both again");
                    restoring_until_ = 0;
                }
                level = level_of(*steam_now, bottom_max_);
                moved = true;
            }
        } else if (top_now != wrote_top_ && !top_.sending(top_now) && top_now > 0) {
            level = level_of(top_now, top_max_);
            wrote_top_ = top_now;
            moved = true;
        }
        if (moved) {
            write(LEVEL, fmt_level(level));
            double f = steam_position(level);
            screens_ = {f, f, steam_value_};
            save_screens();
        }
        bool retry = (top_refused_ || bottom_refused_) && monotonic() >= panel_retry_at_;
        // Each panel's writes go to its writer; their outcomes (refused or
        // taken, and wrote_top_) come back in written().
        int want_bottom = follow(bottom_.way, bottom_target(), bottom_.shown, monotonic());
        if (want_bottom != bottom_.shown || (bottom_refused_ && retry)) submit(bottom_, want_bottom);
        if (!top_.busy()) resync_top(top_now);
        int want_top = follow(top_.way, stock_top(screens_.top, top_max_), top_.shown, monotonic());
        if (want_top != top_.shown || (top_refused_ && retry)) submit(top_, want_top);
        if ((top_refused_ || bottom_refused_) && monotonic() >= panel_retry_at_)
            panel_retry_at_ = monotonic() + PANEL_RETRY_S;
    }

    // DUO, both modes: the bottom panel follows SCREENS' bottom (dark
    // underneath while off, as game_pass); the top is Steam's.
    void duo_pass() {
        if (auto s = read_screens()) screens_ = *s;
        bool retry = bottom_refused_ && monotonic() >= panel_retry_at_;
        int want_bottom = follow(bottom_.way, bottom_target(), bottom_.shown, monotonic());
        if (want_bottom != bottom_.shown || retry) submit(bottom_, want_bottom);
        if (retry) panel_retry_at_ = monotonic() + PANEL_RETRY_S;
    }

    // The next pass, when the soonest thing is due (nothing due: none).
    void arm() {
        double now = monotonic();
        double at = INFINITY;
        auto due = [&at](double t) { at = std::min(at, t); };
        if (bottom_.way || top_.way) due(now + GLIDE_MS / 1000.0);
        if (pressed_at_) due(*pressed_at_ + LONG_PRESS_S);
        if (overlay_since_ && !switch_on_) due(*overlay_since_ + OVERLAY_DELAY_S);
        if (!asleep_ && switch_on_ && limit_ > 0 && !desktop_)
            due(std::max(last_active_ + limit_, game_checked_ + SLEEP_CHECK_S));
        // A refused panel's retry, where the pass makes one (in Desktop
        // Mode only the lit bottom panel's, as in the Python).
        if (desktop_ ? (bottom_refused_ && on_) : (top_refused_ || bottom_refused_)) due(panel_retry_at_);
        if ((!yield_sent_ || *yield_sent_ != want_yield_) && yield_retry_at_ > now) due(yield_retry_at_);
        if (desktop_ && !ready_) due(desktop_since_ + DESKTOP_READY_S);
        if (!on_) due(now + DARK_RECHECK_S);
        if (cn_ < 0) due(mode_checked_ + MODE_CHECK_S);
        if (timer_) loop_->cancel(timer_);
        timer_ = 0;
        // A hair late, so what is due is due (the loop fires up to 0.5 ms
        // early), and never sooner than MIN_PASS_S from now.
        if (std::isfinite(at))
            timer_ = loop_->after(std::max(MIN_PASS_S, at - now + 0.001), [this] {
                timer_ = 0;
                tick();
            });
    }

    pq::Loop* loop_ = nullptr;
    bool test_sysfs_ = false;
    int bottom_max_ = 0, top_max_ = 0;
    std::string bottom_path_, type_path_, max_path_, barry_settings_;
    bool shadowed_ = false;
    int bl_fd_ = -1, key_fd_ = -1, touch_fd_ = -1, cn_ = -1;
    uint64_t timer_ = 0, key_retry_ = 0, top_settle_ = 0;
    std::vector<std::string> touch_dirs_;
    std::vector<input_event> key_queue_;
    bool touch_queued_ = false;
    DirWatch var_watch_, settings_watch_;
    std::string rest_pid_;
    bool rest_ended_ = false;
    uint64_t rest_timer_ = 0;

    std::map<pid_t, Proc> procs_;
    double mode_checked_ = 0;

    Screens screens_{};
    std::optional<Stamp> screens_stamp_, settings_stamp_, desktop_top_seen_;
    std::optional<Screens> screens_cache_;
    int sleep_limit_ = 0, limit_ = 0;
    long long steam_value_ = 0;
    double restoring_until_ = 0;
    int wrote_top_ = 0;
    // A panel refused its last write (EINVAL while it is off or coming up,
    // as after Desktop Mode turned the screens off): the value sits in sysfs
    // all the same, but not on the panel. Retried until it takes.
    bool top_refused_ = false, bottom_refused_ = false;
    double panel_retry_at_ = 0;
    Panel top_, bottom_;
    uint64_t power_on_seq_ = 0;  // the bottom panel powers up once this is written
    std::optional<bool> was_on_;
    bool on_ = true, switch_on_ = true, asleep_ = false, ready_ = false;
    bool slept_by_hand_ = false;  // asleep from SLEEP (Barry's Screen off), not the timer
    double slept_at_ = 0;
    double last_active_ = monotonic();  // a bottom touch or an AYN press
    double game_checked_ = 0;
    std::optional<double> overlay_since_;
    std::optional<double> pressed_at_;  // AYN held down since, until handled
    std::shared_ptr<std::atomic<bool>> switching_;  // DESKTOP_SWITCH running
    std::shared_ptr<std::atomic<bool>> nudge_;      // xdotool running
    double nudged_at_ = -NUDGE_S;
    std::optional<bool> yield_sent_;  // GAMESCOPE_BOTTOM_SCREEN_YIELD as last set
    bool want_yield_ = false;
    double yield_retry_at_ = 0;  // after Game Mode's display failed
    bool desktop_ = false;
    double desktop_since_ = 0;
};

}  // namespace

bool thor_backlight_here() { return dt_has("ayn,thor") || dt_has("retroidpocket,rpduo"); }

std::unique_ptr<Module> make_thor_backlight() {
    P = dt_has("retroidpocket,rpduo") ? &DUO : &THOR;
    return std::make_unique<ThorBacklight>();
}

}  // namespace pp

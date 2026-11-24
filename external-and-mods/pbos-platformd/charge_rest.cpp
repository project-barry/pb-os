// charge-rest (was sm8550-charge-rest): sleep on the charger for the SM8550
// handhelds (Retroid Pocket 6 / Nova, AYN Thor), the Retroid Pocket Duo
// (SM7750) and the KONKR Pocket FIT (SM8650), and the stick lights' charging indicator. A one-shot program,
// its own process as before: `pbos-platformd charge-rest suspend|preview|
// settings|save JSON`.
//
// Kernel deep sleep (full SoC sleep) charges at about a quarter of the awake
// rate: the charger firmware slows down while the SoC is collapsed (AYN Thor,
// 2026-10-08: 1.0-1.1 A asleep vs 4.7 A awake; s2idle 4.8 A). So on the
// charger there is no kernel suspend: Steam's Sleep goes through logind ->
// systemd-suspend.service, which runs this (`suspend`) instead of
// systemd-sleep. With the charger connected it:
//
//   - pauses the running game (SIGSTOP on the Steam-launched process tree),
//   - powers the screens' backlights down and ignores touch,
//   - freezes the user session (Steam, gamescope, Barry Launcher, games);
//     PipeWire, WirePlumber and D-Bus keep running (konkr-standby found
//     frozen PipeWire woke hours late and audio lagged),
//   - takes the big cores offline and caps the CPU and GPU clocks,
//   - shows the charging indicator on the stick lights (CHARGING_STATE):
//     a dim pulse while charging, a dim steady light once full,
//   - until the power button is pressed, the lid opens (AYN Thor, Duo) or the
//     charger is pulled. Pulled: kernel sleep from there (below), until the
//     next wake. Then everything comes back: CPUs, apps, screens.
//
// Without a charger it is deep sleep, watched: a charger plugged in meanwhile
// wakes it (kernel patch 0505: the ADSP's doorbell, which deep sleep used to
// mask, wakes the system once it is down; each such wake is counted on the
// "ipcc-wake" wakeup source), and it goes on into the rest above (screens
// still off, session still frozen), as if it had been put to sleep on the
// charger. A doorbell wake without a charger goes straight back to deep
// sleep. Any other wake (power button, lid, ...) wakes the device as before.
// A kernel without 0505 (no "ipcc-wake"): plain systemd-sleep, and a charger
// plugged in during it charges slowly until the next wake. sm8550-sleep (the
// unit's ExecStartPre/ExecStopPost) runs around all of it: Bluetooth off,
// audio parked, the stick lights faded out before and in after.
//
// While resting or watching the kernel sleep, FLAG (holding this process's
// PID, so a leftover from a crash counts for nothing) tells the power-button
// module (the wake press must not also ask Steam to sleep again), pbosd and
// sm8550-thor-backlightd (it must not power the bottom screen back up) to
// stand aside; INDICATOR tells pbos-qamd's light engine that the stick
// lights are taken.
//
// Stick lights: the multicolor zones rgb:l1..r4 over two HTR3212
// controllers. At the dim levels of an indicator the PWM has only a few
// steps, so while the indicator shows, the controllers run their LEDs at 1/4
// current (LED control 0x07; the driver sets 0x03, 1/2): twice the steps for
// the same light. Restored before the lights go back to their owner. The AYN
// Odin 2's four PWM multicolor zones (left-joystick, left-side,
// right-joystick, right-side, on two SN3112s) show the same indicator at
// their own current; the SN3112 has no current setting to change.
//
// KONKR Pocket FIT (in deep sleep mode, konkr-sleep-mode "kernel"; standby
// and the AYANEO Pocket S2 stay with konkr-suspend): the same, after
// konkr-sleep.service has prepared the sleep (Wi-Fi, touch, gamepad and the
// stick-light MCU let go; konkr-sleep post brings them back after the wake).
// The doorbell wake is kernel patch 0019 (the same "ipcc-wake" count; on
// SM8650 a doorbell always woke deep sleep). The stick lights belong to the
// MCU (konkr_sysbtn, "konkr:rgb:joysticks"): it is bound again for the
// indicator while resting and let go again before deep sleep. The MCU draws
// the pulse itself ("breath"), at its own pace; pbosd keeps the power LED's
// charge colors and leaves the power key to this while FLAG holds.
//
// For the QAM's Power tab: `preview` shows the indicator once (a pulse, its
// gap, then the full light) with the screens on; `settings` prints the
// settings in use and the ranges as JSON; `save JSON` stores changes
// (merged, then held to the ranges).
//
// What changed from the Python, none of it in what is written where or in
// what order:
//   - no interpreter start (most of the ~1.3 s of CPU per rest on the Nova);
//   - the end of systemd-sleep is seen at once (pidfd), not by Python's
//     wait loop that looks up to 50 ms late;
//   - the freeze is followed on cgroup.events (inotify; the kernel notifies
//     every change), the charger after a doorbell wake and the MCU's light
//     after its bind on uevents; each still looks again at the Python's pace
//     (50 ms, 250 ms, 50 ms) in case no event comes, with the same limits;
//   - the preview sleeps through the parts of the pulse that do not change
//     (its gap, the full light) instead of drawing unchanged frames at 50
//     FPS; the rest already did;
//   - SIGTERM/SIGINT stop the preview at once instead of after the frame.
// Timeouts are rounded up to whole microseconds where the Python's select()
// did, so the pulse's frames fall on the same times.

#include <fcntl.h>
#include <glob.h>
#include <linux/netlink.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "charge_rest_json.h"
#include "module.h"

extern char** environ;

namespace pp {

namespace {

using pyjson::Value;

const std::string FLAG = "/run/sm8550-charge-rest";
const std::string INDICATOR = "/run/pbos-charge-indicator";
const std::string HELD = "/run/sm8550-charge-held";  // sm8550-charge-control holds the battery
const std::string CHARGING_STATE = "/var/lib/pbos-utils/charging.json";
const std::string SYSTEMD_SLEEP = "/usr/lib/systemd/systemd-sleep";
const std::string KONKR_SUSPEND = "/usr/lib/konkr/konkr-suspend";
const std::string BATTERY = "/sys/class/power_supply/battery";
const std::string USB = "/sys/class/power_supply/qcom-battmgr-usb";
const std::string ZONES = "/sys/class/leds/rgb:*";
const char* const ODIN2_ZONES[] = {"left-joystick", "left-side", "right-joystick", "right-side"};
const std::string HTR3212 = "/sys/bus/i2c/drivers/htr3212";
const std::string KONKR_LED = "/sys/class/leds/konkr:rgb:joysticks";  // Pocket FIT stick lights (MCU)
const std::string KONKR_MCU = "/sys/bus/serial/drivers/konkr_sysbtn";
const std::string KONKR_MCU_MARK = "/run/konkr-sleep-mcu";  // konkr-sleep pre: the MCUs it let go
const std::string KONKR_SLEEP_MODE = "/usr/lib/konkr/konkr-sleep-mode";
const char* const KONKR_MODEL = "KONKR Pocket FIT";
constexpr int LED_CONTROL_FIRST = 0x32, LED_CONTROL_END = 0x3E;  // one per channel, 12 per controller
constexpr int LED_UPDATE = 0x25;
constexpr int LED_QUARTER = 0x07, LED_DRIVER = 0x03;  // on + 1/4 current; on + 1/2 (driver)
constexpr unsigned long I2C_SLAVE_FORCE = 0x0706;

// The indicator's settings (the Power tab writes CHARGING_STATE). Levels are
// 0-255 at 1/4 LED current; the defaults are the lowest the tab offers
// (PULSE_MAX_MIN, SOLID_MIN). Both colors default to the stick lights' own
// default purple (pbos-qamd lights.cpp DEFAULT_COLOR, pbosd).
constexpr bool DEF_ENABLED = true;
const char* const DEF_COLOR = "8c00ff";
constexpr long long DEF_PULSE_MIN = 0, DEF_PULSE_MAX = 20, DEF_SOLID_LEVEL = 1;
constexpr double DEF_ON_S = 4.0, DEF_OFF_S = 8.0;
const char* const KEYS[] = {"enabled",  "pulse_color", "pulse_min",   "pulse_max",
                            "on_s",     "off_s",       "solid_color", "solid_level"};
constexpr long long PULSE_MAX_MIN = 20, SOLID_MIN = 1;
constexpr double ON_LO = 1.0, ON_HI = 10.0, OFF_LO = 0.0, OFF_HI = 30.0;
constexpr int FPS = 50;
constexpr double GAMMA = 2.2;
constexpr double UNPLUG_S = 2.0;  // after the unplug event, still gone: really unplugged
const std::string MEM_SLEEP = "/sys/power/mem_sleep";
const std::string IPCC = "/sys/module/qcom_ipcc/parameters";  // deep_wake, idle_wake (0505)
const char* const IPCC_WAKE = "ipcc-wake";  // wakeup source: wakes by an ADSP doorbell (0505)
constexpr double USER_WAKE_S = 0.3;         // a wake press / lid open shows up within this
constexpr double CHARGER_SETTLE_S = 3.0;    // after a doorbell wake: the charger may still come up
constexpr size_t DOORBELL_LIMIT = 3;        // this many doorbell wakes without a charger
constexpr double DOORBELL_WINDOW_S = 120.0; // within this: doorbell wakes off until the next wake
constexpr uint16_t EV_KEY = 1, EV_SW = 5, KEY_POWER = 116, SW_LID = 0;

// struct input_event on 64-bit (Python's struct "llHHi").
struct InputEvent {
    long sec, usec;
    uint16_t type, code;
    int32_t value;
};
static_assert(sizeof(InputEvent) == 24);

// ------------------------------------------------- Python's exceptions --
// SystemExit (the SIGTERM handler), KeyboardInterrupt (SIGINT) and what
// Python would end with a traceback: they unwind through the same finally
// blocks (guards) as in the script.
struct SysExit {
    int code;
};
struct KbdInterrupt {};
struct Crash {
    std::string what;
};

// Runs fn when the scope ends, also on the way out of an exception
// (Python's finally).
class Finally {
public:
    explicit Finally(std::function<void()> fn) : fn_(std::move(fn)) {}
    ~Finally() noexcept(false) {
        if (fn_) fn_();
    }
    Finally(const Finally&) = delete;
    Finally& operator=(const Finally&) = delete;

private:
    std::function<void()> fn_;
};

// ------------------------------------------------------------ signals --
// SIGTERM/SIGINT: suspend unwinds (as the script's sys.exit(0) handler and
// Python's KeyboardInterrupt did), the preview stops. The handlers only
// mark it and wake the waits through a pipe.
enum class OnStop { Default, Raise, Stop };
OnStop on_stop = OnStop::Default;
volatile sig_atomic_t got_term = 0, got_int = 0;
int stop_pipe[2] = {-1, -1};
int wake_pipe[2] = {-1, -1};  // SIGUSR1 -> "wake requested"

void on_stop_signal(int sig) {
    int e = errno;
    if (sig == SIGTERM) got_term = 1;
    else got_int = 1;
    if (stop_pipe[1] >= 0) (void)!write(stop_pipe[1], "s", 1);
    errno = e;
}

void on_usr1(int) {
    int e = errno;
    (void)!write(wake_pipe[1], "x", 1);
    errno = e;
}

void handle(int sig, void (*fn)(int)) {
    struct sigaction sa {};
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
}

bool stopped() { return got_term || got_int; }

// After a wait: SIGTERM/SIGINT as Python would have it.
void check_signals() {
    if (on_stop != OnStop::Raise) return;
    if (got_term) {
        got_term = 0;
        char b[64];
        while (read(stop_pipe[0], b, sizeof b) > 0) {}
        throw SysExit{0};
    }
    if (got_int) {
        got_int = 0;
        char b[64];
        while (read(stop_pipe[0], b, sizeof b) > 0) {}
        throw KbdInterrupt{};
    }
}

// ------------------------------------------------------------- clocks --
// time.monotonic() exactly as Python makes it from the nanoseconds.
double monotonic() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t ns = static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
    if (ns % 1000000000LL == 0) return static_cast<double>(ns / 1000000000LL);
    return static_cast<double>(ns) / 1e9;
}

int64_t monotonic_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// Seconds to nanoseconds, rounded up (Python's timeouts).
int64_t to_ns(double s) {
    double d = std::ceil(s * 1e9);
    if (d > 9e18) return INT64_MAX / 2;
    return static_cast<int64_t>(d);
}

// Waits for any of fds (and the stop pipe) for up to ns nanoseconds (< 0:
// forever); the ready ones in fds' order (readable, hung up or failed, as
// select() reports them), empty when the time ran out.
std::vector<int> poll_ns(const std::vector<int>& fds, int64_t ns) {
    int64_t deadline = ns < 0 ? -1 : monotonic_ns() + ns;
    while (true) {
        std::vector<pollfd> p;
        for (int fd : fds) p.push_back({fd, POLLIN, 0});
        bool watch_stop = on_stop != OnStop::Default && stop_pipe[0] >= 0;
        if (watch_stop) p.push_back({stop_pipe[0], POLLIN, 0});
        timespec ts{}, *tsp = nullptr;
        if (deadline >= 0) {
            int64_t left = std::max<int64_t>(0, deadline - monotonic_ns());
            ts.tv_sec = left / 1000000000LL;
            ts.tv_nsec = left % 1000000000LL;
            tsp = &ts;
        }
        int r = ppoll(p.data(), p.size(), tsp, nullptr);
        if (r < 0 && errno != EINTR) return {};
        if (on_stop == OnStop::Raise) check_signals();
        if (on_stop == OnStop::Stop && stopped()) return {};
        if (r < 0) {  // EINTR: again, for what is left (PEP 475)
            if (deadline >= 0 && monotonic_ns() >= deadline) return {};
            continue;
        }
        std::vector<int> ready;
        for (size_t k = 0; k < fds.size(); ++k)
            if (p[k].revents & (POLLIN | POLLHUP | POLLERR)) ready.push_back(fds[k]);
        if (!ready.empty() || r == 0) return ready;
        if (deadline >= 0 && monotonic_ns() >= deadline) return {};
    }
}

// select.select(fds, [], [], timeout): its timeout in whole microseconds,
// rounded up; timeout < 0 waits for ever.
std::vector<int> select_fds(const std::vector<int>& fds, double timeout) {
    if (timeout < 0) return poll_ns(fds, -1);
    int64_t us = (to_ns(timeout) + 999) / 1000;
    return poll_ns(fds, us * 1000);
}

// time.sleep(s).
void sleep_s(double s) { poll_ns({}, to_ns(s)); }

// -------------------------------------------------------------- files --
void log(const std::string& msg) {
    std::string line = "sm8550-charge-rest: " + msg + "\n";
    std::fwrite(line.data(), 1, line.size(), stdout);
    std::fflush(stdout);
}

// Python's str.strip() (ASCII and Latin-1 whitespace; sysfs has no other).
std::string py_strip(std::string_view s) {
    auto space = [](unsigned char c) { return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20); };
    size_t a = 0, b = s.size();
    while (a < b && space(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && space(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> py_split(std::string_view s) {
    std::vector<std::string> out;
    auto space = [](unsigned char c) { return (c >= 0x09 && c <= 0x0d) || (c >= 0x1c && c <= 0x20); };
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && space(static_cast<unsigned char>(s[i]))) ++i;
        size_t j = i;
        while (j < s.size() && !space(static_cast<unsigned char>(s[j]))) ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

std::optional<std::string> read_all(const std::string& p) {
    int fd = open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    std::string out;
    char buf[8192];
    while (true) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return std::nullopt;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return out;
}

// Text mode's universal newlines.
std::string newlines(std::string s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\r') {
            out.push_back('\n');
            if (i + 1 < s.size() && s[i + 1] == '\n') ++i;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// The file's text, stripped (and trailing NULs: device tree strings), or d.
std::string rd(const std::string& p, const std::string& d = "") {
    auto s = read_all(p);
    if (!s) return d;
    std::string t = py_strip(newlines(std::move(*s)));
    while (!t.empty() && t.back() == '\0') t.pop_back();
    return t;
}

// As open(p, "w").write(v): created or truncated, then one write (none for
// an empty value).
bool wr(const std::string& p, const std::string& v) {
    int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) return false;
    bool ok = true;
    size_t done = 0;
    while (done < v.size()) {
        ssize_t n = write(fd, v.data() + done, v.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            ok = false;
            break;
        }
        done += static_cast<size_t>(n);
    }
    if (close(fd) != 0) ok = false;
    return ok;
}
bool wr(const std::string& p, long long v) { return wr(p, std::to_string(v)); }

// A flag file holding this PID: honored only while this process lives.
void claim(const std::string& p) { wr(p, static_cast<long long>(getpid())); }

void rm(const std::string& p) { unlink(p.c_str()); }

bool exists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

bool isdir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// glob.glob(): the folder's order (sorted=false) or sorted().
std::vector<std::string> glob_py(const std::string& pattern, bool sorted) {
    std::vector<std::string> out;
    glob_t g{};
    if (::glob(pattern.c_str(), sorted ? 0 : GLOB_NOSORT, nullptr, &g) == 0)
        for (size_t i = 0; i < g.gl_pathc; ++i) out.emplace_back(g.gl_pathv[i]);
    globfree(&g);
    if (sorted) std::sort(out.begin(), out.end());
    return out;
}

// os.listdir(): the folder's order.
std::vector<std::string> listdir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    while (auto* e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        out.emplace_back(e->d_name);
    }
    closedir(d);
    return out;
}

std::string basename(const std::string& p) {
    auto at = p.find_last_of('/');
    return at == std::string::npos ? p : p.substr(at + 1);
}

// posixpath.split and join.
std::pair<std::string, std::string> path_split(const std::string& p) {
    size_t i = p.rfind('/') == std::string::npos ? 0 : p.rfind('/') + 1;
    std::string head = p.substr(0, i), tail = p.substr(i);
    if (!head.empty() && head.find_first_not_of('/') != std::string::npos)
        while (!head.empty() && head.back() == '/') head.pop_back();
    return {head, tail};
}

std::string path_join(const std::string& a, const std::string& b) {
    if (!b.empty() && b[0] == '/') return b;
    if (a.empty() || a.back() == '/') return a + b;
    return a + "/" + b;
}

// os.path.realpath (not strict): links resolved as far as they go, missing
// parts kept as written.
std::pair<std::string, bool> join_real(std::string path, std::string rest,
                                       std::map<std::string, std::optional<std::string>>& seen) {
    if (!rest.empty() && rest[0] == '/') {
        rest = rest.substr(1);
        path = "/";
    }
    while (!rest.empty()) {
        auto slash = rest.find('/');
        std::string name = rest.substr(0, slash);
        rest = slash == std::string::npos ? "" : rest.substr(slash + 1);
        if (name.empty() || name == ".") continue;
        if (name == "..") {
            if (!path.empty()) {
                auto [h, t] = path_split(path);
                path = h;
                if (t == "..") path = path_join(path_join(path, ".."), "..");
            } else {
                path = "..";
            }
            continue;
        }
        std::string newpath = path_join(path, name);
        struct stat st;
        bool is_link = lstat(newpath.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
        if (!is_link) {
            path = newpath;
            continue;
        }
        auto it = seen.find(newpath);
        if (it != seen.end()) {
            if (it->second) {
                path = *it->second;
                continue;
            }
            return {path_join(newpath, rest), false};
        }
        seen[newpath] = std::nullopt;
        char buf[4096];
        ssize_t n = readlink(newpath.c_str(), buf, sizeof buf - 1);
        std::string target = n < 0 ? "" : std::string(buf, static_cast<size_t>(n));
        auto [p, ok] = join_real(path, target, seen);
        if (!ok) return {path_join(p, rest), false};
        seen[newpath] = p;
        path = p;
    }
    return {path, true};
}

std::string realpath_py(const std::string& p) {
    std::map<std::string, std::optional<std::string>> seen;
    return join_real("", p, seen).first;
}

// "[Errno N] text" (+ ": 'path'"), as Python prints an OSError.
std::string oserror(int err, const std::string& path = "") {
    std::string s = "[Errno " + std::to_string(err) + "] " + std::strerror(err);
    if (!path.empty()) s += ": '" + path + "'";
    return s;
}

// --------------------------------------------- Python's int() and float() --
bool u_space(char32_t c) { return pyjson::is_space(c); }

std::u32string u_strip(const std::u32string& s) {
    size_t a = 0, b = s.size();
    while (a < b && u_space(s[a])) ++a;
    while (b > a && u_space(s[b - 1])) --b;
    return s.substr(a, b - a);
}

// Digits with single underscores between them (Python's numeric strings).
std::optional<std::string> digits_underscored(const std::u32string& s, bool hex = false) {
    std::string out;
    auto isdig = [&](char32_t c) {
        return (c >= '0' && c <= '9') || (hex && ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')));
    };
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t c = s[i];
        if (c == '_') {
            if (i == 0 || i + 1 >= s.size() || !isdig(s[i - 1]) || !isdig(s[i + 1])) return std::nullopt;
            continue;
        }
        if (!isdig(c)) return std::nullopt;
        out.push_back(static_cast<char>(c));
    }
    if (out.empty()) return std::nullopt;
    return out;
}

constexpr long long SAT = 1000000000000000000LL;  // past every bound here

long long saturate(const std::string& digits, bool neg, int base = 10) {
    long long v = 0;
    for (char c : digits) {
        int d = (c >= '0' && c <= '9') ? c - '0' : (std::tolower(static_cast<unsigned char>(c)) - 'a' + 10);
        if (v > SAT / base) {
            v = SAT;
            break;
        }
        v = v * base + d;
        if (v > SAT) {
            v = SAT;
            break;
        }
    }
    return neg ? -v : v;
}

// int(text, base) for a str; nullopt for ValueError. Saturated far past
// every range it is held to.
std::optional<long long> int_of_str(const std::u32string& text, int base = 10) {
    std::u32string s = u_strip(text);
    bool neg = false;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) {
        neg = s[0] == '-';
        s = s.substr(1);
    }
    if (base == 16 && s.size() > 1 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s = s.substr(2);
        if (!s.empty() && s[0] == '_') s = s.substr(1);
    }
    auto d = digits_underscored(s, base == 16);
    if (!d) return std::nullopt;
    if (base == 10 && d->size() > 4300) return std::nullopt;  // the int digit limit
    return saturate(*d, neg, base);
}

std::optional<double> float_of_str(const std::u32string& text) {
    std::u32string s = u_strip(text);
    std::u32string body = s;
    size_t at = 0;
    if (!body.empty() && (body[0] == '+' || body[0] == '-')) at = 1;
    std::string low;
    for (size_t i = at; i < body.size(); ++i) {
        if (body[i] > 0x7f) return std::nullopt;
        low.push_back(static_cast<char>(std::tolower(static_cast<int>(body[i]))));
    }
    bool neg = at && body[0] == '-';
    if (low == "inf" || low == "infinity") return neg ? -HUGE_VAL : HUGE_VAL;
    if (low == "nan") return neg ? -std::nan("") : std::nan("");
    // [digits][.digits][e[sign]digits], underscores only between digits.
    std::string clean;
    for (size_t i = 0; i < s.size(); ++i) {
        char32_t c = s[i];
        if (c == '_') {
            bool ok = i > 0 && i + 1 < s.size() && s[i - 1] >= '0' && s[i - 1] <= '9' && s[i + 1] >= '0' &&
                      s[i + 1] <= '9';
            if (!ok) return std::nullopt;
            continue;
        }
        if (c > 0x7f) return std::nullopt;
        clean.push_back(static_cast<char>(c));
    }
    size_t i = 0, n = clean.size();
    if (i < n && (clean[i] == '+' || clean[i] == '-')) ++i;
    size_t mant = 0;
    while (i < n && std::isdigit(static_cast<unsigned char>(clean[i]))) ++i, ++mant;
    if (i < n && clean[i] == '.') {
        ++i;
        while (i < n && std::isdigit(static_cast<unsigned char>(clean[i]))) ++i, ++mant;
    }
    if (!mant) return std::nullopt;
    if (i < n && (clean[i] == 'e' || clean[i] == 'E')) {
        ++i;
        if (i < n && (clean[i] == '+' || clean[i] == '-')) ++i;
        size_t ed = 0;
        while (i < n && std::isdigit(static_cast<unsigned char>(clean[i]))) ++i, ++ed;
        if (!ed) return std::nullopt;
    }
    if (i != n) return std::nullopt;
    return std::strtod(clean.c_str(), nullptr);
}

// int(x): nullopt for TypeError/ValueError (num() falls back to the
// default); OverflowError (int(inf)) is the script's crash.
std::optional<long long> py_int(const Value& v) {
    switch (v.type) {
        case Value::Type::Bool: return v.b ? 1 : 0;
        case Value::Type::Int: {
            bool neg = v.i[0] == '-';
            return saturate(neg ? v.i.substr(1) : v.i, neg);
        }
        case Value::Type::Float: {
            if (std::isnan(v.f)) return std::nullopt;
            if (std::isinf(v.f)) throw Crash{"OverflowError: cannot convert float infinity to integer"};
            double t = std::trunc(v.f);
            if (t >= static_cast<double>(SAT)) return SAT;
            if (t <= -static_cast<double>(SAT)) return -SAT;
            return static_cast<long long>(t);
        }
        case Value::Type::Str: return int_of_str(v.s);
        default: return std::nullopt;
    }
}

std::optional<double> py_float(const Value& v) {
    switch (v.type) {
        case Value::Type::Bool: return v.b ? 1.0 : 0.0;
        case Value::Type::Int: {
            double d = std::strtod(v.i.c_str(), nullptr);
            if (std::isinf(d)) throw Crash{"OverflowError: int too large to convert to float"};
            return d;
        }
        case Value::Type::Float: return v.f;
        case Value::Type::Str: return float_of_str(v.s);
        default: return std::nullopt;
    }
}

bool py_bool(const Value& v) {
    switch (v.type) {
        case Value::Type::Null: return false;
        case Value::Type::Bool: return v.b;
        case Value::Type::Int: return v.i != "0";
        case Value::Type::Float: return v.f != 0;
        case Value::Type::Str: return !v.s.empty();
        case Value::Type::Array: return !v.a.empty();
        case Value::Type::Object: return !v.o.empty();
    }
    return false;
}

// ------------------------------------------------------------- settings --
struct Settings {
    bool enabled = DEF_ENABLED;
    std::string pulse_color = DEF_COLOR;
    long long pulse_min = DEF_PULSE_MIN, pulse_max = DEF_PULSE_MAX;
    double on_s = DEF_ON_S, off_s = DEF_OFF_S;
    std::string solid_color = DEF_COLOR;
    long long solid_level = DEF_SOLID_LEVEL;
};

Value defaults() {
    Value d = Value::object();
    d.set(U"enabled", Value::boolean(DEF_ENABLED));
    d.set(U"pulse_color", Value::string(DEF_COLOR));
    d.set(U"pulse_min", Value::integer(DEF_PULSE_MIN));
    d.set(U"pulse_max", Value::integer(DEF_PULSE_MAX));
    d.set(U"on_s", Value::number(DEF_ON_S));
    d.set(U"off_s", Value::number(DEF_OFF_S));
    d.set(U"solid_color", Value::string(DEF_COLOR));
    d.set(U"solid_level", Value::integer(DEF_SOLID_LEVEL));
    return d;
}

bool is_key(const std::u32string& k) {
    for (const char* key : KEYS)
        if (k == pyjson::widen(key)) return true;
    return false;
}

std::string clean_color(const Value& c, const std::string& def) {
    std::u32string s;
    if (c.type == Value::Type::Str) s = c.s;
    else if (c.type == Value::Type::Int) s = pyjson::widen(c.i);
    else return def;  // str() of anything else is never six hex digits
    size_t at = 0;
    while (at < s.size() && s[at] == '#') ++at;
    s = s.substr(at);
    if (s.size() != 6) return def;
    std::string out;
    for (char32_t ch : s) {
        if (ch > 0x7f || !std::isxdigit(static_cast<int>(ch))) return def;
        out.push_back(static_cast<char>(std::tolower(static_cast<int>(ch))));
    }
    return out;
}

// The settings file as Python's json.load read it: {} when missing, not
// UTF-8, not JSON or not an object.
Value read_state() {
    auto bytes = read_all(CHARGING_STATE);
    if (!bytes) return Value::object();
    auto text = pyjson::decode_utf8(newlines(*bytes), false);
    if (!text) return Value::object();
    std::optional<Value> v;
    try {
        v = pyjson::loads(*text);
    } catch (const pyjson::NotValueError& e) {
        throw Crash{e.what};
    }
    if (!v || v->type != Value::Type::Object) return Value::object();
    return *v;
}

// CHARGING_STATE over the defaults, each value clamped to what the tab
// offers: as a JSON object (key order and types as the script's dict) and
// typed.
std::pair<Value, Settings> settings() {
    Value raw = read_state();
    Value s = defaults();
    for (auto& [k, v] : raw.o)
        if (is_key(k)) s.set(k, v);
    const Value D = defaults();
    auto num_int = [&](const char* key, long long lo, long long hi) -> long long {
        auto x = py_int(*s.get(pyjson::widen(key)));
        if (!x) return std::stoll(D.get(pyjson::widen(key))->i);
        long long m = *x < hi ? *x : hi;  // min(hi, x)
        return m > lo ? m : lo;           // max(lo, ...)
    };
    auto num_float = [&](const char* key, double lo, double hi) -> double {
        auto x = py_float(*s.get(pyjson::widen(key)));
        if (!x) return D.get(pyjson::widen(key))->f;
        double m = *x < hi ? *x : hi;
        return m > lo ? m : lo;
    };
    Settings t;
    t.enabled = py_bool(*s.get(U"enabled"));
    t.pulse_color = clean_color(*s.get(U"pulse_color"), DEF_COLOR);
    t.solid_color = clean_color(*s.get(U"solid_color"), DEF_COLOR);
    t.pulse_max = num_int("pulse_max", PULSE_MAX_MIN, 255);
    long long pmin = num_int("pulse_min", 0, 254);
    t.pulse_min = t.pulse_max - 1 < pmin ? t.pulse_max - 1 : pmin;
    t.solid_level = num_int("solid_level", SOLID_MIN, 255);
    t.on_s = num_float("on_s", ON_LO, ON_HI);
    t.off_s = num_float("off_s", OFF_LO, OFF_HI);
    s.set(U"enabled", Value::boolean(t.enabled));
    s.set(U"pulse_color", Value::string(t.pulse_color));
    s.set(U"solid_color", Value::string(t.solid_color));
    s.set(U"pulse_max", Value::integer(t.pulse_max));
    s.set(U"pulse_min", Value::integer(t.pulse_min));
    s.set(U"solid_level", Value::integer(t.solid_level));
    s.set(U"on_s", Value::number(t.on_s));
    s.set(U"off_s", Value::number(t.off_s));
    return {s, t};
}

Value limits() {
    auto pair_i = [](long long a, long long b) {
        Value v = Value::array();
        v.a = {Value::integer(a), Value::integer(b)};
        return v;
    };
    auto pair_f = [](double a, double b) {
        Value v = Value::array();
        v.a = {Value::number(a), Value::number(b)};
        return v;
    };
    Value l = Value::object();
    l.set(U"pulse_max", pair_i(PULSE_MAX_MIN, 255));
    l.set(U"pulse_min", pair_i(0, 254));
    l.set(U"solid_level", pair_i(SOLID_MIN, 255));
    l.set(U"on_s", pair_f(ON_LO, ON_HI));
    l.set(U"off_s", pair_f(OFF_LO, OFF_HI));
    l.set(U"defaults", defaults());
    return l;
}

// json.dump(s, open(CHARGING_STATE + ".tmp", "w"), indent=2), then
// os.replace; failures end the program as the script's traceback did.
void store(const Value& s) {
    std::string dir = CHARGING_STATE.substr(0, CHARGING_STATE.rfind('/'));
    // os.makedirs(dir, exist_ok=True)
    for (size_t at = 1; at != std::string::npos;) {
        at = dir.find('/', at + 1);
        std::string part = dir.substr(0, at);
        if (mkdir(part.c_str(), 0777) != 0 && errno != EEXIST) throw Crash{oserror(errno, part)};
    }
    if (!isdir(dir)) throw Crash{oserror(EEXIST, dir)};
    std::string tmp = CHARGING_STATE + ".tmp";
    std::string text = pyjson::dumps(s, 2);
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) throw Crash{oserror(errno, tmp)};
    size_t done = 0;
    while (done < text.size()) {
        ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            close(fd);
            throw Crash{oserror(e)};
        }
        done += static_cast<size_t>(n);
    }
    if (close(fd) != 0) throw Crash{oserror(errno)};
    if (rename(tmp.c_str(), CHARGING_STATE.c_str()) != 0) throw Crash{oserror(errno, tmp)};
}

// changes over the settings in use, held to the ranges, stored.
Value save(const Value& changes) {
    Value s = settings().first;
    for (auto& [k, v] : changes.o)
        if (is_key(k)) s.set(k, v);
    store(s);
    s = settings().first;  // as held to the ranges
    store(s);
    return s;
}

// --------------------------------------------------------- stick lights --
std::vector<std::string> zones() {
    std::vector<std::string> out, all = glob_py(ZONES, true);
    for (const char* n : ODIN2_ZONES) all.push_back(std::string("/sys/class/leds/") + n);
    for (auto& d : all) {
        std::string drv = basename(realpath_py(d + "/device/driver"));
        if ((drv == "leds_group_multicolor" || drv == "leds_pwm_multicolor") && exists(d + "/multi_intensity"))
            out.push_back(d);
    }
    return out;
}

// (bus, address) of each HTR3212, from its I2C device name (2-003c).
std::vector<std::pair<long long, long long>> controllers() {
    std::vector<std::pair<long long, long long>> out;
    for (auto& p : glob_py(HTR3212 + "/*-*", false)) {
        std::string name = basename(p);
        auto dash = name.find('-');
        auto bus = int_of_str(pyjson::widen(name.substr(0, dash)));
        auto addr = int_of_str(pyjson::widen(name.substr(dash + 1)), 16);
        if (bus && addr) out.emplace_back(*bus, *addr);
    }
    return out;
}

// Every channel's LED control register on every controller, then the
// update register (the driver only writes them when it starts).
void led_current(int value) {
    for (auto [bus, addr] : controllers()) {
        std::string dev = "/dev/i2c-" + std::to_string(bus);
        int fd = open(dev.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            log("i2c-" + std::to_string(bus) + ": " + oserror(errno, dev));
            continue;
        }
        char where[64];
        std::snprintf(where, sizeof where, "i2c-%lld 0x%02llx: ", bus, addr);
        auto put = [&](int reg, int v) {
            unsigned char b[2] = {static_cast<unsigned char>(reg), static_cast<unsigned char>(v)};
            ssize_t n;
            do n = write(fd, b, 2);
            while (n < 0 && errno == EINTR);
            return n >= 0;
        };
        bool ok = ioctl(fd, I2C_SLAVE_FORCE, static_cast<unsigned long>(addr)) >= 0;
        for (int reg = LED_CONTROL_FIRST; ok && reg < LED_CONTROL_END; ++reg) ok = put(reg, value);
        if (ok) ok = put(LED_UPDATE, 0);
        if (!ok) log(where + oserror(errno));
        close(fd);
    }
}

int hex2(const std::string& color, size_t at) { return static_cast<int>(std::strtol(color.substr(at, 2).c_str(), nullptr, 16)); }

// round(c * level / 255): ties to even, as Python's round().
long long scale(int c, double level) { return static_cast<long long>(std::nearbyint(static_cast<double>(c) * level / 255.0)); }

class Indicator {
public:
    virtual ~Indicator() = default;
    virtual bool mcu_pulse() const { return false; }
    virtual void take() = 0;
    virtual void show(const std::string& color, double level) = 0;
    virtual void breath(const std::string&, double) {}
    virtual std::optional<double> resend_in() { return std::nullopt; }
    virtual void give_back() = 0;
};

// The stick zones while the indicator has them: what they showed is kept
// and put back exactly (color, then brightness) at the end.
class Lights : public Indicator {
public:
    Lights() : zones_(zones()) {
        for (auto& z : zones_) saved_.push_back({z, rd(z + "/multi_intensity"), rd(z + "/brightness", "0")});
    }

    void take() override {
        claim(INDICATOR);
        led_current(LED_QUARTER);
        quarter_ = true;
        for (auto& z : zones_) {
            wr(z + "/brightness", 0);
            wr(z + "/multi_intensity", "0 0 0");
            wr(z + "/brightness", rd(z + "/max_brightness", "255"));
        }
    }

    // color (RRGGBB) at level (0-255 of the 1/4-current range).
    void show(const std::string& color, double level) override {
        long long ch[3];
        for (int i = 0; i < 3; ++i) ch[i] = scale(hex2(color, static_cast<size_t>(i) * 2), level);
        for (auto& z : zones_) {
            std::string v;
            for (int i : channels(z)) v += (v.empty() ? "" : " ") + std::to_string(ch[i]);
            auto it = shown_.find(z);
            if (it == shown_.end() || it->second != v) {
                wr(z + "/multi_intensity", v);
                shown_[z] = v;
            }
        }
    }

    void give_back() override {
        for (auto& z : zones_) wr(z + "/brightness", 0);
        if (quarter_) {
            led_current(LED_DRIVER);
            quarter_ = false;
        }
        for (auto& s : saved_) {
            wr(s.zone + "/multi_intensity", s.intensity);
            wr(s.zone + "/brightness", s.brightness);
        }
        shown_.clear();
        rm(INDICATOR);
    }

private:
    const std::vector<int>& channels(const std::string& z) {
        auto it = order_.find(z);
        if (it != order_.end()) return it->second;
        auto names = py_split(rd(z + "/multi_index"));
        auto sorted = names;
        std::sort(sorted.begin(), sorted.end());
        std::vector<int> o = {0, 1, 2};
        if (sorted == std::vector<std::string>{"blue", "green", "red"}) {
            o.clear();
            for (auto& n : names) o.push_back(n == "red" ? 0 : n == "green" ? 1 : 2);
        }
        return order_[z] = o;
    }

    struct Saved {
        std::string zone, intensity, brightness;
    };
    std::vector<std::string> zones_;
    std::vector<Saved> saved_;
    std::map<std::string, std::vector<int>> order_;
    std::map<std::string, std::string> shown_;
    bool quarter_ = false;
};

int uevent_socket();

// The Pocket FIT's stick lights while the indicator has them. The MCU
// behind them (konkr_sysbtn) is let go for deep sleep by konkr-sleep pre:
// bound again here and let go again in give_back(). The pulse is the MCU's
// own "breath" (no frames from here); pbosd puts the owner's lighting back
// (SIGHUP from konkr-sleep post, or here).
class KonkrLights : public Indicator {
public:
    bool mcu_pulse() const override { return true; }

    static bool available() { return isdir(KONKR_LED) || !py_split(rd(KONKR_MCU_MARK)).empty(); }

    // Seconds until the indicator is to be written again (none: never); at
    // 0 the next breath()/show() writes it again.
    std::optional<double> resend_in() override {
        if (resend_.empty()) return std::nullopt;
        double left = resend_.front() - monotonic();
        if (left <= 0) {
            resend_.erase(resend_.begin());
            shown_.clear();
            return 0.0;
        }
        return left;
    }

    void take() override {
        if (!isdir(KONKR_LED)) {
            // The LED's class device comes with a uevent: looked at then, and
            // every 50 ms as before, for up to 3 s.
            int ue = uevent_socket();
            for (auto& dev : py_split(rd(KONKR_MCU_MARK)))
                if (wr(KONKR_MCU + "/bind", dev)) bound_.push_back(dev);
            double end = monotonic() + 3.0;
            while (!exists(KONKR_LED + "/effect") && monotonic() < end) {
                if (ue < 0) {
                    sleep_s(0.05);
                    continue;
                }
                if (!select_fds({ue}, std::min(0.05, std::max(0.0, end - monotonic()))).empty()) {
                    char buf[16384];
                    while (recv(ue, buf, sizeof buf, MSG_DONTWAIT) > 0) {}
                }
            }
            if (ue >= 0) close(ue);
            sleep_s(0.2);  // the MCU's enable frame, before the first light frame
            double now = monotonic();
            // After a bind the MCU goes back to its own last mode (the owner's
            // awake lighting) and the first frames can be lost: shown again
            // after these.
            resend_ = {now + 1.0, now + 3.0};
        }
        claim(INDICATOR);
        std::string b;
        for (auto& d : bound_) b += (b.empty() ? "" : " ") + d;
        log(std::string("stick lights: mcu ") + (bound_.empty() ? "already bound" : "bound " + b));
    }

    // The MCU pulses color at level (0-255) on its own.
    void breath(const std::string& color, double level) override {
        std::string v = "breath";
        for (int i = 0; i < 3; ++i) v += " " + std::to_string(scale(hex2(color, static_cast<size_t>(i) * 2), level));
        if (shown_ != v && wr(KONKR_LED + "/effect", v)) shown_ = v;
    }

    // color steady at level (0-255).
    void show(const std::string& color, double level) override {
        std::string v;
        for (int i = 0; i < 3; ++i)
            v += (i ? " " : "") + std::to_string(scale(hex2(color, static_cast<size_t>(i) * 2), level));
        if (shown_ != v) {
            wr(KONKR_LED + "/effect", "static");
            wr(KONKR_LED + "/multi_intensity", v);
            wr(KONKR_LED + "/brightness", 255);
            shown_ = v;
        }
    }

    void give_back() override;

private:
    std::vector<std::string> bound_;
    std::string shown_;
    std::vector<double> resend_;
};

// Python's float %.
double py_mod(double x, double y) {
    double m = std::fmod(x, y);
    if (m != 0) {
        if ((y < 0) != (m < 0)) m += y;
    } else {
        m = std::copysign(0.0, y);
    }
    return m;
}

// The pulse at t seconds: a breath over on_s (raised cosine, gamma for the
// eye) from pulse_min up to pulse_max and back, then off_s at pulse_min.
// (Each step as the script's; the build's -std=c++20 keeps them unfused.)
double pulse_level(const Settings& s, double t) {
    double tc = py_mod(t, s.on_s + s.off_s);
    if (tc >= s.on_s) return static_cast<double>(s.pulse_min);
    double angle = 2 * M_PI * tc / s.on_s;
    double k = std::pow(0.5 - 0.5 * std::cos(angle), GAMMA);
    return static_cast<double>(s.pulse_min) + static_cast<double>(s.pulse_max - s.pulse_min) * k;
}

bool charger_online() { return rd(USB + "/online", "0") == "1"; }

// Full, or held below full on purpose by sm8550-charge-control (charge
// limit or bypass; HELD holds its PID), or stopped by the charger firmware
// with the charger in ("Not charging": the Pocket FIT's own 80 % limit).
bool battery_full() {
    std::string st = rd(BATTERY + "/status");
    if (st == "Full" || st == "Not charging" || rd(BATTERY + "/capacity", "0") == "100") return true;
    std::string pid = rd(HELD);
    bool digits = !pid.empty() && std::all_of(pid.begin(), pid.end(), [](char c) { return c >= '0' && c <= '9'; });
    return digits && exists("/proc/" + pid);
}

// ------------------------------------------------------------ programs --
// subprocess.run(argv, timeout=...): fds 0-2 shared, SIGPIPE and SIGXFSZ
// back to their defaults (Python ignores them); with out, the program's
// stdout is read into it (and its stderr goes nowhere: capture_output). The
// exit code (-N: signal N); nullopt when it could not start or ran out of
// time (killed). SIGTERM/SIGINT kill it and go on unwinding, as Python's
// run() did. Its end is seen at once (pidfd).
std::optional<int> spawn_wait(const std::vector<std::string>& argv, double timeout, std::string* out = nullptr) {
    std::fflush(stdout);
    int pipefd[2] = {-1, -1};
    if (out && pipe2(pipefd, O_CLOEXEC) != 0) return std::nullopt;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (out) {
        posix_spawn_file_actions_adddup2(&fa, pipefd[1], 1);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    }
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t def;
    sigemptyset(&def);
    for (int sig : {SIGPIPE, SIGXFSZ, SIGTERM, SIGINT, SIGUSR1}) sigaddset(&def, sig);
    posix_spawnattr_setsigdefault(&attr, &def);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGDEF);
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0].c_str(), &fa, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (out) close(pipefd[1]);
    int rfd = out ? pipefd[0] : -1;
    if (rc != 0) {
        if (rfd >= 0) close(rfd);
        return std::nullopt;
    }
    int pfd = static_cast<int>(syscall(SYS_pidfd_open, pid, 0));
    bool exited = false;
    int status = 0;
    auto finish = [&](bool kill_it) {
        if (!exited) {
            if (kill_it) kill(pid, SIGKILL);
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            exited = true;
        }
        if (pfd >= 0) close(pfd);
        if (rfd >= 0) close(rfd);
        pfd = rfd = -1;
    };
    int64_t deadline = monotonic_ns() + to_ns(timeout);
    try {
        while (!exited || rfd >= 0) {
            int64_t left = deadline - monotonic_ns();
            if (left <= 0) {
                finish(true);
                return std::nullopt;  // TimeoutExpired
            }
            std::vector<int> fds;
            if (rfd >= 0) fds.push_back(rfd);
            if (!exited && pfd >= 0) fds.push_back(pfd);
            if (!exited && pfd < 0) left = std::min<int64_t>(left, 5000000);  // no pidfd: look every 5 ms
            auto ready = poll_ns(fds, left);
            if (rfd >= 0 && std::find(ready.begin(), ready.end(), rfd) != ready.end()) {
                char buf[4096];
                ssize_t n = read(rfd, buf, sizeof buf);
                if (n > 0) {
                    out->append(buf, static_cast<size_t>(n));
                } else if (n == 0 || errno != EINTR) {
                    close(rfd);
                    rfd = -1;
                }
            }
            if (!exited && waitpid(pid, &status, WNOHANG) == pid) exited = true;
        }
    } catch (...) {
        finish(true);
        throw;
    }
    finish(false);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return 1;
}

int run(const std::vector<std::string>& argv) {
    auto rc = spawn_wait(argv, 60);
    return rc ? *rc : 1;
}

// os.execv: the process becomes the program (same PID).
[[noreturn]] void exec(const std::vector<std::string>& argv) {
    std::fflush(stdout);
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    execv(argv[0].c_str(), args.data());
    throw Crash{"OSError: " + oserror(errno)};
}

void KonkrLights::give_back() {
    wr(KONKR_LED + "/effect", "static");
    wr(KONKR_LED + "/brightness", 0);
    if (!bound_.empty()) {
        // Let go again, as konkr-sleep pre left it (its UART keeps the XO
        // clock on in deep sleep); konkr-sleep post binds it after the wake.
        for (auto& dev : bound_) wr(KONKR_MCU + "/unbind", dev);
        bound_.clear();
    } else {
        run({"systemctl", "kill", "-s", "HUP", "pbosd.service"});
    }
    shown_.clear();
    rm(INDICATOR);
}

// -------------------------------------------------------------- the rest --
// The handhelds that rest by themselves (the Retroid Pocket Duo, SM7750, has
// the same charger firmware interface, power key, lid and doorbell count).
bool rests() {
    std::string c = rd("/sys/firmware/devicetree/base/compatible");
    return c.find("qcom,sm8550") != std::string::npos || c.find("retroidpocket,rpduo") != std::string::npos;
}

// The KONKR Pocket FIT in deep sleep mode (konkr-sleep prepared it).
bool pocket_fit() {
    if (rd("/sys/firmware/devicetree/base/model").find(KONKR_MODEL) == std::string::npos) return false;
    std::string out;
    auto rc = spawn_wait({KONKR_SLEEP_MODE}, 10, &out);
    if (!rc) return false;
    return py_strip(newlines(out)) == "kernel";
}

std::vector<long> game_pids() {
    std::map<long, std::vector<long>> children;
    std::vector<long> roots;
    static const std::string marker("reaper\0SteamLaunch\0", 19);
    for (auto& p : listdir("/proc")) {
        if (p.empty() || !std::all_of(p.begin(), p.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        auto st = read_all("/proc/" + p + "/stat");
        if (!st) continue;
        size_t paren = st->rfind(')');
        size_t from = paren == std::string::npos ? 1 : paren + 2;
        std::vector<std::string> f;
        {
            // bytes.split(): ASCII whitespace
            auto sp = [](char c) { return c == ' ' || (c >= '\t' && c <= '\r'); };
            size_t i = std::min(from, st->size());
            while (i < st->size()) {
                while (i < st->size() && sp((*st)[i])) ++i;
                size_t j = i;
                while (j < st->size() && !sp((*st)[j])) ++j;
                if (j > i) f.push_back(st->substr(i, j - i));
                i = j;
            }
        }
        if (f.size() < 2) continue;
        auto ppid = int_of_str(pyjson::widen(f[1]));
        if (!ppid) continue;
        auto cmd = read_all("/proc/" + p + "/cmdline");
        if (!cmd) continue;
        long pid = std::stol(p);
        children[static_cast<long>(*ppid)].push_back(pid);
        if (cmd->find(marker) != std::string::npos) roots.push_back(pid);
    }
    std::vector<long> out, stack = roots;
    while (!stack.empty()) {
        long top = stack.back();
        stack.pop_back();
        auto it = children.find(top);
        if (it == children.end()) continue;
        for (long c : it->second) {
            out.push_back(c);
            stack.push_back(c);
        }
    }
    return out;
}

// Keep audio and D-Bus running while freezing the graphical user.
bool keep_running(const std::string& unit) {
    for (const char* k : {"pipewire.service", "pipewire-pulse.service", "wireplumber.service", "dbus-broker.service",
                          "at-spi-dbus-bus.service", "init.scope"})
        if (unit == k) return true;
    return false;
}

std::vector<std::string> sorted_listdir(const std::string& p) {
    auto v = listdir(p);
    std::sort(v.begin(), v.end());
    return v;
}

// cgroups to freeze (as konkr-standby): each session scope, every
// user-manager slice but session.slice, and every session.slice unit but
// the ones that keep running.
std::vector<std::string> freeze_targets() {
    std::vector<std::string> out;
    passwd* pw = getpwnam("steamos");
    if (!pw) return out;
    for (auto& user : glob_py("/sys/fs/cgroup/user.slice/user-" + std::to_string(pw->pw_uid) + ".slice", false)) {
        for (auto& child : sorted_listdir(user)) {
            std::string path = path_join(user, child);
            if (!isdir(path)) continue;
            bool manager = child.rfind("user@", 0) == 0 && child.size() >= 8 &&
                           child.compare(child.size() - 8, 8, ".service") == 0;
            if (!manager) {
                out.push_back(path);
                continue;
            }
            for (auto& sub : sorted_listdir(path)) {
                std::string spath = path_join(path, sub);
                if (!isdir(spath) || keep_running(sub)) continue;
                if (sub != "session.slice") {
                    out.push_back(spath);
                    continue;
                }
                for (auto& unit : sorted_listdir(spath)) {
                    std::string upath = path_join(spath, unit);
                    if (isdir(upath) && !keep_running(unit)) out.push_back(upath);
                }
            }
        }
    }
    return out;
}

// cgroup.freeze on each; frozen: up to 2 s until each says "frozen 1" in
// cgroup.events (watched: the kernel notifies its changes; looked at every
// 50 ms too, as before).
std::vector<std::string> set_frozen(const std::vector<std::string>& cgroups, bool frozen) {
    std::vector<std::string> done;
    for (auto& cg : cgroups)
        if (wr(cg + "/cgroup.freeze", frozen ? 1 : 0)) done.push_back(cg);
    if (!frozen) return done;
    double end = monotonic() + 2.0;
    int in = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (in >= 0)
        for (auto& cg : done) inotify_add_watch(in, (cg + "/cgroup.events").c_str(), IN_MODIFY);
    Finally close_in([&] {
        if (in >= 0) close(in);
    });
    auto all_frozen = [&] {
        for (auto& cg : done)
            if (rd(cg + "/cgroup.events").find("frozen 1") == std::string::npos) return false;
        return true;
    };
    while (monotonic() < end && !all_frozen()) {
        if (in < 0) {
            sleep_s(0.05);
            continue;
        }
        if (!select_fds({in}, std::min(0.05, std::max(0.0, end - monotonic()))).empty()) {
            char buf[4096];
            while (read(in, buf, sizeof buf) > 0) {}
        }
    }
    return done;
}

std::vector<int> big_cpus() {
    std::map<int, long long> caps;
    for (auto& c : glob_py("/sys/devices/system/cpu/cpu[0-9]*", false)) {
        std::string v = rd(c + "/cpu_capacity", "0");
        bool digits = !v.empty() && std::all_of(v.begin(), v.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
        auto n = int_of_str(pyjson::widen(basename(c).substr(3)));
        if (!n) throw Crash{"ValueError: invalid literal for int() with base 10: '" + basename(c).substr(3) + "'"};
        caps[static_cast<int>(*n)] = digits ? *int_of_str(pyjson::widen(v)) : 0;
    }
    std::vector<int> out;
    if (caps.empty()) return out;
    long long low = caps.begin()->second;
    for (auto& [n, v] : caps) low = std::min(low, v);
    for (auto& [n, v] : caps)
        if (v > low && n != 0) out.push_back(n);
    return out;
}

// The AYN Thor's bottom brightness file is sm8550-thor-backlightd's shadow
// (Steam's level), not the panel: never write it.
bool bind_mounted(const std::string& path) {
    std::string real = realpath_py(path);
    std::string info = newlines(read_all("/proc/self/mountinfo").value_or(""));
    size_t start = 0;
    while (start < info.size()) {
        size_t nl = info.find('\n', start);
        std::string line = info.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        start = nl == std::string::npos ? info.size() : nl + 1;
        auto f = py_split(line);
        if (f.size() > 4 && f[4] == real) return true;
    }
    return false;
}

int input_dev(const std::string& name) {
    for (auto& e : glob_py("/sys/class/input/event*", true)) {
        if (rd(e + "/device/name") == name) {
            int fd = open(("/dev/input/" + basename(e)).c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            return fd < 0 ? -1 : fd;
        }
    }
    return -1;
}

// inhibited files of the direct-touch devices (INPUT_PROP_DIRECT).
std::vector<std::string> touchscreens() {
    std::vector<std::string> out;
    for (auto& d : glob_py("/sys/class/input/input*", false)) {
        auto props = int_of_str(pyjson::widen(rd(d + "/properties", "0")), 16).value_or(0);
        if ((props & 0x2) && exists(d + "/inhibited")) out.push_back(d + "/inhibited");
    }
    return out;
}

void drain(int fd) {
    if (fd < 0) return;
    char buf[sizeof(InputEvent) * 64];
    while (true) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0) continue;
        if (n < 0 && errno == EINTR) continue;
        return;
    }
}

std::vector<InputEvent> read_events(int fd, bool& ok) {
    std::vector<InputEvent> out;
    char buf[sizeof(InputEvent) * 64];
    ssize_t n;
    do n = read(fd, buf, sizeof buf);
    while (n < 0 && errno == EINTR);
    ok = n >= 0;
    for (ssize_t off = 0; n > 0 && off + static_cast<ssize_t>(sizeof(InputEvent)) <= n; off += sizeof(InputEvent)) {
        InputEvent e;
        std::memcpy(&e, buf + off, sizeof e);
        out.push_back(e);
    }
    return out;
}

// The power key went down or up, or the lid opened, within timeout.
bool user_woke(int key, int lid, double timeout) {
    std::vector<int> fds;
    for (int f : {key, lid})
        if (f >= 0) fds.push_back(f);
    double end = monotonic() + timeout;
    while (!fds.empty()) {
        auto r = select_fds(fds, std::max(0.0, end - monotonic()));
        if (r.empty()) return false;
        for (int fd : r) {
            bool ok;
            auto evs = read_events(fd, ok);
            if (!ok) continue;
            for (auto& e : evs) {
                if (fd == key && e.type == EV_KEY && e.code == KEY_POWER) return true;
                if (fd == lid && e.type == EV_SW && e.code == SW_LID && e.value == 0) return true;
            }
        }
    }
    return false;
}

// The kernel's sleep for "mem" now ("s2idle [deep]" -> "deep").
std::string mem_sleep() {
    for (auto& w : py_split(rd(MEM_SLEEP)))
        if (!w.empty() && w[0] == '[') {
            size_t a = w.find_first_not_of("[]"), b = w.find_last_not_of("[]");
            return a == std::string::npos ? "" : w.substr(a, b - a + 1);
        }
    return "";
}

std::optional<long long> count_of(const std::string& v) {
    if (v.empty() || !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; })) return std::nullopt;
    return saturate(v, false);
}

// How often an ADSP doorbell woke the system (0505), none without 0505.
std::optional<long long> ipcc_wakes() {
    for (auto& w : glob_py("/sys/class/wakeup/wakeup*", false))
        if (rd(w + "/name") == IPCC_WAKE) return count_of(rd(w + "/event_count"));
    return std::nullopt;
}

// Power key events the kernel kept from userspace around a wake (Pocket FIT
// 0018 + 0020: counted on the pon key's wakeup source); 0 elsewhere.
long long key_wakes() {
    long long n = 0;
    for (auto& w : glob_py("/sys/class/wakeup/wakeup*", false)) {
        std::string name = rd(w + "/name");
        if (name.size() >= 7 && name.compare(name.size() - 7, 7, ":pwrkey") == 0)
            n += count_of(rd(w + "/event_count")).value_or(0);
    }
    return n;
}

// Deep sleep is in use (s2idle charges at full speed anyway) and the kernel
// lets a charger plugged in wake it (0505).
bool can_watch() { return mem_sleep() == "deep" && ipcc_wakes() && rd(IPCC + "/deep_wake") == "Y"; }

// The kernel's uevents: a charger plugged in or pulled, the battery
// charging or full (power_supply, from the charger firmware's
// notifications) and USB-C partner changes (typec). -1 (and why, when
// loud) when it cannot be had.
int uevent_socket(bool loud) {
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (fd >= 0) {
        sockaddr_nl sa{};
        sa.nl_family = AF_NETLINK;
        sa.nl_pid = 0;
        sa.nl_groups = 1;
        if (bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0) return fd;
        int e = errno;
        close(fd);
        errno = e;
    }
    if (loud) log("uevents: " + oserror(errno));
    return -1;
}
int uevent_socket() { return uevent_socket(false); }

// Reads what the socket holds: whether a charger/battery/USB-C change is
// among it.
bool charger_event(int sock) {
    bool hit = false;
    char buf[16384];
    while (true) {
        ssize_t n = recv(sock, buf, sizeof buf, MSG_DONTWAIT);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return hit;
        std::string_view msg(buf, static_cast<size_t>(n));
        size_t start = 0;
        while (start <= msg.size()) {
            size_t z = msg.find('\0', start);
            std::string_view f = msg.substr(start, z == std::string_view::npos ? std::string_view::npos : z - start);
            if (f == "SUBSYSTEM=power_supply" || f == "SUBSYSTEM=typec") hit = true;
            if (z == std::string_view::npos) break;
            start = z + 1;
        }
    }
}

// The charger after a doorbell wake: the doorbell can come before the
// charger firmware reports it online, so it is waited for a moment (looked
// at on every uevent, and every 250 ms as before).
bool charger_after_doorbell() {
    int ue = uevent_socket();
    Finally close_ue([&] {
        if (ue >= 0) close(ue);
    });
    double end = monotonic() + CHARGER_SETTLE_S;
    while (!charger_online()) {
        if (monotonic() >= end) return false;
        if (ue < 0) {
            sleep_s(0.25);
            continue;
        }
        if (!select_fds({ue}, std::min(0.25, std::max(0.0, end - monotonic()))).empty()) {
            char buf[16384];
            while (recv(ue, buf, sizeof buf, MSG_DONTWAIT) > 0) {}
        }
    }
    return true;
}

std::string list_repr(const std::vector<int>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) s += (i ? ", " : "") + std::to_string(v[i]);
    return s + "]";
}

struct Backlight {
    std::string dir, power;
    std::optional<std::string> brightness;  // none: bind-mounted, never written
};

class Rest {
public:
    std::unique_ptr<Indicator> lights;

    // --- going down
    // clocks: also the big cores offline and the clocks capped (resting;
    // kernel sleep wants them as they are, to go down and come back fast).
    void enter(bool clocks) {
        claim(FLAG);
        for (long pid : game_pids())
            if (kill(static_cast<pid_t>(pid), SIGSTOP) == 0) paused_.push_back(pid);
        for (auto& bl : glob_py("/sys/class/backlight/*", true)) {
            if (rd(bl + "/bl_power") != "0") continue;  // already off (the Thor's bottom switch)
            std::optional<std::string> br;
            if (!bind_mounted(bl + "/brightness")) br = rd(bl + "/brightness");
            backlights_.push_back({bl, "0", br});
            if (br) wr(bl + "/brightness", 0);
            wr(bl + "/bl_power", 4);
        }
        for (auto& p : touchscreens())
            if (rd(p) == "0" && wr(p, 1)) inhibited_.push_back(p);
        sleep_s(0.3);
        frozen_ = set_frozen(freeze_targets(), true);
        if (clocks) limit_clocks();
        log("resting: " + std::to_string(paused_.size()) + " game processes paused, " + std::to_string(frozen_.size()) +
            " cgroups frozen, cpus " + list_repr(offlined_) + " offline, " + std::to_string(backlights_.size()) +
            " screens off");
    }

    void limit_clocks() {
        for (auto& pol : glob_py("/sys/devices/system/cpu/cpufreq/policy*", false)) {
            save_(pol + "/scaling_min_freq", rd(pol + "/scaling_min_freq"));
            save_(pol + "/scaling_max_freq", rd(pol + "/scaling_max_freq"));
            wr(pol + "/scaling_min_freq", rd(pol + "/cpuinfo_min_freq"));
            wr(pol + "/scaling_max_freq", rd(pol + "/cpuinfo_min_freq"));
        }
        for (int cpu : big_cpus())
            if (wr("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/online", 0)) offlined_.push_back(cpu);
        for (auto& gpu : glob_py("/sys/class/devfreq/*gpu*", false)) {
            std::optional<std::string> lowest;  // the smallest number, as int() writes it
            for (auto& f : py_split(rd(gpu + "/available_frequencies"))) {
                if (!std::all_of(f.begin(), f.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
                size_t nz = f.find_first_not_of('0');
                std::string n = nz == std::string::npos ? "0" : f.substr(nz);
                if (!lowest || n.size() < lowest->size() || (n.size() == lowest->size() && n < *lowest)) lowest = n;
            }
            if (lowest) {
                save_(gpu + "/max_freq", rd(gpu + "/max_freq"));
                wr(gpu + "/max_freq", *lowest);
            }
        }
    }

    void restore_clocks() {
        for (int cpu : offlined_) wr("/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/online", 1);
        offlined_.clear();
        // max before min: a min above the current max is refused.
        std::vector<std::pair<std::string, std::string>> order = saved_;
        std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
            return a.first.find("scaling_min_freq") == std::string::npos &&
                   b.first.find("scaling_min_freq") != std::string::npos;
        });
        for (auto& [key, v] : order)
            if (!v.empty()) wr(key, v);
        saved_.clear();
    }

    // --- coming back
    // Everything back, each step on its own: one failing must not leave the
    // session frozen or the screens off.
    void leave() {
        Finally flag([] { rm(FLAG); });
        std::pair<const char*, std::function<void()>> steps[] = {
            {"_lights_back", [this] { lights_back(); }}, {"restore_clocks", [this] { restore_clocks(); }},
            {"_thaw", [this] { thaw(); }},               {"_touch_back", [this] { touch_back(); }},
            {"_screens_back", [this] { screens_back(); }}, {"_games_back", [this] { games_back(); }}};
        for (auto& [name, step] : steps) {
            try {
                step();
            } catch (const std::exception& e) {  // keep going
                log(std::string(name) + ": " + e.what());
            }
        }
        sleep_s(0.5);  // the wake press's release, before the power-button module looks again
    }

    // --- kernel sleep
    // Deep sleep, watched, until a wake that is not a stray doorbell:
    // "plugged" (a charger plugged in woke it, 0505) or "wake".
    std::string deep_sleep() {
        int key = input_dev("pmic_pwrkey");
        int lid = input_dev("gpio-keys-lid");
        drain(key);
        drain(lid);
        claim(INDICATOR);  // the QAM's light engine leaves the lights dark
        bool switched_off = false;
        std::vector<double> doorbells;
        Finally after([&] {
            for (int f : {key, lid})
                if (f >= 0) close(f);
            if (switched_off) wr(IPCC + "/deep_wake", "Y");
            rm(INDICATOR);
        });
        while (true) {
            auto before = ipcc_wakes();
            long long keys = key_wakes();
            log("deep sleep");
            run({SYSTEMD_SLEEP, "suspend"});
            if (user_woke(key, lid, USER_WAKE_S) || key_wakes() > keys) return "wake";
            auto after_wakes = ipcc_wakes();
            if (!before || !after_wakes || *after_wakes <= *before) return "wake";  // not a doorbell: a normal wake
            dark_again();
            if (charger_after_doorbell()) {
                log("charger plugged in");
                return "plugged";
            }
            // Another ADSP doorbell. Too many of them: deep sleep no longer
            // wakes on doorbells (0504's behavior) this time.
            double t = monotonic();
            std::vector<double> recent;
            for (double d : doorbells)
                if (t - d < DOORBELL_WINDOW_S) recent.push_back(d);
            recent.push_back(t);
            doorbells = recent;
            log("doorbell wake, no charger (" + std::to_string(doorbells.size()) + ")");
            if (doorbells.size() >= DOORBELL_LIMIT && !switched_off && wr(IPCC + "/deep_wake", "N")) {
                switched_off = true;
                log("deep_wake off until the device wakes: too many doorbell wakes");
            }
            // A press while it was awake for the doorbell wakes it.
            if (user_woke(key, lid, 0) || key_wakes() > keys) return "wake";
        }
    }

    // After a doorbell wake: the display came back with the kernel (screens
    // on, last frame) and systemd-sleep thawed the session.
    void dark_again() {
        for (auto& b : backlights_) {
            if (b.brightness) wr(b.dir + "/brightness", 0);
            wr(b.dir + "/bl_power", 4);
        }
        for (auto& p : touchscreens())
            if (rd(p) == "0" && wr(p, 1) && std::find(inhibited_.begin(), inhibited_.end(), p) == inhibited_.end())
                inhibited_.push_back(p);
        auto again = set_frozen(freeze_targets(), true);
        std::vector<std::string> all = frozen_;
        all.insert(all.end(), again.begin(), again.end());
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        frozen_ = all;
    }

    // --- while resting
    // Shows the indicator until a wake: "power", "lid" or "unplugged".
    std::string wait() {
        Settings s = settings().second;
        if (s.enabled && !zones().empty()) {
            lights = std::make_unique<Lights>();
            lights->take();
        } else if (s.enabled && KonkrLights::available()) {
            lights = std::make_unique<KonkrLights>();
            lights->take();
        }
        int key = input_dev("pmic_pwrkey");
        int lid = input_dev("gpio-keys-lid");
        drain(key);
        drain(lid);
        arm_usr1();
        int ev = uevent_socket(true);
        Finally after([&] {
            for (int f : {key, lid})
                if (f >= 0) close(f);
            if (ev >= 0) close(ev);
        });
        std::vector<int> fds;
        for (int f : {key, lid, wake_pipe[0], ev})
            if (f >= 0) fds.push_back(f);
        bool pressed = false;
        double t0 = monotonic();
        bool full = battery_full();
        // The charger and the battery are read again on their own events
        // only (ev). Pulled: still gone UNPLUG_S after that event (a USB PD
        // renegotiation drops it for a moment) is unplugged.
        std::optional<double> unplugged_at;
        if (!charger_online()) unplugged_at = t0 + UNPLUG_S;
        auto shorter = [](std::optional<double> a, double b) { return !a ? b : (b < *a ? b : *a); };
        while (true) {
            double now = monotonic();
            if (unplugged_at && now >= *unplugged_at) {
                if (!charger_online()) {
                    log("charger pulled");
                    return "unplugged";
                }
                unplugged_at.reset();
            }
            std::optional<double> timeout;
            if (lights) {
                if (full) {
                    lights->show(s.solid_color, static_cast<double>(s.solid_level));
                } else if (lights->mcu_pulse()) {
                    lights->breath(s.pulse_color, static_cast<double>(s.pulse_max));
                } else {
                    lights->show(s.pulse_color, pulse_level(s, now - t0));
                    double tc = py_mod(now - t0, s.on_s + s.off_s);
                    timeout = tc < s.on_s ? 1.0 / FPS : s.on_s + s.off_s - tc;
                }
            }
            if (lights && lights->mcu_pulse())
                if (auto again = lights->resend_in()) timeout = shorter(timeout, *again);
            if (unplugged_at) {
                double left = *unplugged_at - now;
                timeout = shorter(timeout, left > 0.0 ? left : 0.0);
            }
            auto r = select_fds(fds, timeout ? *timeout : -1);
            auto in = [&](int fd) { return fd >= 0 && std::find(r.begin(), r.end(), fd) != r.end(); };
            if (in(wake_pipe[0])) {
                char buf[64];
                (void)!read(wake_pipe[0], buf, sizeof buf);
                log("wake requested (SIGUSR1)");
                return "power";
            }
            if (in(ev) && charger_event(ev)) {
                full = battery_full();
                if (charger_online()) {
                    unplugged_at.reset();
                } else if (!unplugged_at) {
                    log("charger unplug event");
                    unplugged_at = monotonic() + UNPLUG_S;
                }
            }
            for (int fd : r) {
                if (fd == wake_pipe[0] || fd == ev) continue;
                bool ok;
                auto evs = read_events(fd, ok);
                if (!ok) continue;
                for (auto& e : evs) {
                    if (fd == key && e.type == EV_KEY && e.code == KEY_POWER) {
                        if (e.value == 1) {
                            pressed = true;
                        } else if (e.value == 0 && pressed) {
                            log("wake: power key");
                            return "power";
                        }
                    } else if (fd == lid && e.type == EV_SW && e.code == SW_LID && e.value == 0) {
                        log("wake: lid opened");
                        return "lid";
                    }
                }
            }
        }
    }

private:
    // SIGUSR1 from here on writes to the wake pipe (and stays so, as the
    // script's handler did).
    static void arm_usr1() {
        if (wake_pipe[0] < 0) {
            if (pipe2(wake_pipe, O_CLOEXEC) != 0) return;
            fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);
        }
        handle(SIGUSR1, on_usr1);
    }

    void save_(const std::string& key, const std::string& v) {
        for (auto& kv : saved_)
            if (kv.first == key) {
                kv.second = v;
                return;
            }
        saved_.emplace_back(key, v);
    }

    void lights_back() {
        if (lights) {
            lights->give_back();
            lights.reset();
        }
    }

    void thaw() {
        if (!frozen_.empty()) set_frozen(frozen_, false);
    }

    void touch_back() {
        for (auto& p : inhibited_) wr(p, 0);
    }

    void screens_back() {
        for (auto& b : backlights_) {
            wr(b.dir + "/bl_power", b.power);
            if (b.brightness) wr(b.dir + "/brightness", *b.brightness);
        }
    }

    void games_back() {
        for (long pid : paused_) kill(static_cast<pid_t>(pid), SIGCONT);
    }

    std::vector<std::pair<std::string, std::string>> saved_;
    std::vector<long> paused_;
    std::vector<std::string> frozen_;
    std::vector<int> offlined_;
    std::vector<std::string> inhibited_;
    std::vector<Backlight> backlights_;
};

int deep_sleep() { return run({SYSTEMD_SLEEP, "suspend"}); }

// systemd-suspend.service's ExecStart.
int suspend() {
    if (!rests() && !pocket_fit()) {
        if (access(KONKR_SUSPEND.c_str(), X_OK) == 0) exec({KONKR_SUSPEND});
        exec({SYSTEMD_SLEEP, "suspend"});
    }
    bool plugged = charger_online();
    if (!plugged && !can_watch()) exec({SYSTEMD_SLEEP, "suspend"});
    // Stopped (shutdown while resting): unwind through leave().
    if (pipe2(stop_pipe, O_CLOEXEC | O_NONBLOCK) != 0) stop_pipe[0] = stop_pipe[1] = -1;
    on_stop = OnStop::Raise;
    handle(SIGTERM, on_stop_signal);
    handle(SIGINT, on_stop_signal);
    Rest rest;
    try {
        rest.enter(false);
        while (true) {
            if (plugged) {
                rest.limit_clocks();
                log("resting on the charger");
                if (rest.wait() != "unplugged") break;
                // The lights go back to their owner (dark: sm8550-sleep fades
                // them in after) and the clocks too, then real sleep.
                if (rest.lights) {
                    rest.lights->give_back();
                    rest.lights.reset();
                }
                rest.restore_clocks();
            }
            if (!can_watch()) {
                log("deep sleep");
                deep_sleep();
                break;
            }
            if (rest.deep_sleep() != "plugged") break;
            plugged = true;
        }
        log("waking");
    } catch (...) {
        rest.leave();
        throw;
    }
    rest.leave();
    return 0;
}

// The indicator once, screens on: a pulse and its gap, then full.
int preview() {
    Settings s = settings().second;
    if (zones().empty()) {
        log("no stick lights");
        return 1;
    }
    Lights lights;
    if (pipe2(stop_pipe, O_CLOEXEC | O_NONBLOCK) != 0) stop_pipe[0] = stop_pipe[1] = -1;
    on_stop = OnStop::Stop;
    handle(SIGTERM, on_stop_signal);
    handle(SIGINT, on_stop_signal);
    Finally back([&] { lights.give_back(); });
    lights.take();
    double t0 = monotonic();
    double cycle = s.on_s + s.off_s;
    while (!stopped() && monotonic() - t0 < cycle) {
        double t = monotonic() - t0;
        lights.show(s.pulse_color, pulse_level(s, t));
        if (py_mod(t, cycle) >= s.on_s) {
            // The gap: pulse_min until the cycle ends, nothing to draw.
            poll_ns({}, std::max<int64_t>(0, to_ns(t0 + cycle - monotonic())));
            break;
        }
        sleep_s(1.0 / FPS);
    }
    if (!stopped()) {
        lights.show(s.solid_color, static_cast<double>(s.solid_level));
        sleep_s(4.0);  // the full light, unchanged
    }
    return 0;
}

std::string answer(const Value& settings_v) {
    Value out = Value::object();
    out.set(U"settings", settings_v);
    out.set(U"limits", limits());
    out.set(U"lights", Value::boolean(!zones().empty()));
    return pyjson::dumps(out) + "\n";
}

int main_cmd(int argc, char** argv) {
    std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "suspend") return suspend();
    if (cmd == "preview") return preview();
    if (cmd == "settings") {
        std::string out = answer(settings().first);
        std::fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }
    if (cmd == "save" && argc > 2) {
        auto text = pyjson::decode_utf8(argv[2], true);
        std::optional<Value> changes;
        try {
            if (text) changes = pyjson::loads(*text);
        } catch (const pyjson::NotValueError& e) {
            throw Crash{e.what};
        }
        if (!changes || changes->type != Value::Type::Object) {
            std::fputs("save: expected a JSON object\n", stderr);
            return 2;
        }
        std::string out = answer(save(*changes));
        std::fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }
    std::fputs("usage: sm8550-charge-rest suspend | preview | settings | save JSON\n", stderr);
    return 2;
}

}  // namespace

int charge_rest_main(int argc, char** argv) {
    // The name the script had (its comm: "sm8550-charge-r").
    prctl(PR_SET_NAME, "sm8550-charge-rest", 0, 0, 0);
    // As the Python interpreter: these ignored (the programs it execs keep
    // that; the ones it runs get the defaults back).
    signal(SIGPIPE, SIG_IGN);
    signal(SIGXFSZ, SIG_IGN);
    int rc;
    try {
        rc = main_cmd(argc, argv);
    } catch (const SysExit& e) {
        rc = e.code;
    } catch (const KbdInterrupt&) {
        std::fflush(stdout);
        std::fputs("KeyboardInterrupt\n", stderr);
        signal(SIGINT, SIG_DFL);
        raise(SIGINT);
        rc = 130;
    } catch (const Crash& e) {
        std::fflush(stdout);
        std::fprintf(stderr, "sm8550-charge-rest: %s\n", e.what.c_str());
        rc = 1;
    }
    std::fflush(stdout);
    return rc;
}

}  // namespace pp

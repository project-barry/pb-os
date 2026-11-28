// sm8550-boostd: uclamp boost for the game and the Steam UI (SM8550).
//
// With mainline EAS/schedutil the frame-critical threads run at low clocks
// and land on the Cortex-A510s (cpu0-2). Android's game mode / WALT raises
// their utilization floor; this does the same:
//
//   game threads  (descendants of Steam's `reaper SteamLaunch`): uclamp.min 256,
//                  or just above the little cores' capacity where that is
//                  higher (see game_uclamp())
//   UI threads    (gamescope, Xwayland, Steam, its webhelper, mangoapp,
//                  InputPlumber): uclamp.min 200
//
// No CPU is picked for anyone: the floor makes EAS choose a core with room
// and raises schedutil's clock, but placement stays the scheduler's. The one
// affinity change undoes a pin: Steam holds its webhelper to cpu2-6 on its
// own (not via STEAM_DESKTOP_APP_AFFINITY_LIST), so UI threads narrower than
// all CPUs are widened back. Games' own thread pins are left alone.
//
// The uclamp.min values are pbosd's "balanced" profile (SM8650 overlay).
// Measured on the Retroid Pocket 6 in Easy Delivery Co. with the old
// big-core pin included (90 s runs, against the mean of the runs before and
// after without it): 45.3 -> 49.2 fps average, 1 % low 25.6 -> 29.9 fps,
// frame-time SD 5.5 -> 4.3 ms, 5.1 -> 6.1 W.
//
// The image defaults to EAS. If a sched_ext scheduler is picked anyway
// (steamosctl set-cpu-scheduler lavd), this stands aside and clears the
// floors, since sched_ext ignores uclamp. It takes over again when the
// scheduler is switched back to the kernel default.
//
// Stopping the service clears every floor it set.
//
// How it hears of processes: the kernel's process events (the proc
// connector, CONFIG_PROC_EVENTS): a new thread of a boosted process is
// boosted as it is made, and /proc is looked through again only when a
// process that matters starts, changes or ends. Without the connector (older
// kernels) it looks through /proc once a second, as the Python version did
// (750 ms of CPU a minute on the Nova).
//
// sm8550-boostd --scan   the once-a-second scan even with the connector.

#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/netlink.h>
#include <sched.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "loop.h"
#include "util.h"

namespace {

using namespace pq;

constexpr double TICK_S = 1;
// Process events come in bursts (a game starting forks and execs dozens of
// processes): one look through /proc this long after the first.
constexpr double RESCAN_DELAY_S = 0.1;
constexpr unsigned GAME_UCLAMP_BASE = 256;
constexpr unsigned UI_UCLAMP = 200;
const std::set<std::string> UI_COMMS = {"gamescope-wl", "Xwayland", "steam", "steamwebhelper",
                                        "gamescopereaper", "mangoapp", "inputplumber"};
const std::string SCX_STATE = "/sys/kernel/sched_ext/state";

// The kernel's struct sched_attr (with uclamp, SCHED_ATTR_SIZE_VER1).
struct SchedAttr {
    uint32_t size;
    uint32_t sched_policy;
    uint64_t sched_flags;
    int32_t sched_nice;
    uint32_t sched_priority;
    uint64_t sched_runtime, sched_deadline, sched_period;
    uint32_t sched_util_min, sched_util_max;
};
constexpr uint64_t SCHED_FLAG_KEEP_ALL = 0x08 | 0x10;
constexpr uint64_t SCHED_FLAG_UTIL_CLAMP_MIN = 0x20;

// The game floor. 256 is above the SM8550's Cortex-A510s (cpu_capacity 206),
// so EAS only fully fits a game thread on a mid or big core, and below its
// mid cores (610). On the Retroid Pocket Duo the Cortex-A520s are 364
// (1.84 GHz, the mid cores 829): 256 fits them, and game threads could stay
// there. The floor is the lowest capacity + 1 where that is above 256: 365
// on the Duo, still 256 on the SM8550. The mid cores' clock floor scales the
// same way (256 of 610 there, 365 of 829 here).
unsigned lowest_capacity_floor() {
    unsigned low = 0;
    for (const auto& c : glob("/sys/devices/system/cpu/cpu[0-9]*/cpu_capacity")) {
        std::string v = rd(c);
        if (!is_digits(v)) continue;
        unsigned cap = static_cast<unsigned>(std::atol(v.c_str()));
        if (low == 0 || cap < low) low = cap;
    }
    return (low >= GAME_UCLAMP_BASE && low < 1024) ? low + 1 : GAME_UCLAMP_BASE;
}

unsigned game_uclamp() {
    static const unsigned value = lowest_capacity_floor();
    return value;
}

bool set_uclamp_min(pid_t tid, unsigned value) {
    SchedAttr attr{};
    attr.size = sizeof attr;
    attr.sched_flags = SCHED_FLAG_KEEP_ALL | SCHED_FLAG_UTIL_CLAMP_MIN;
    attr.sched_util_min = value;
    return syscall(SYS_sched_setattr, tid, &attr, 0) == 0;
}

// Widen a thread that something narrowed to a CPU subset.
void unpin(pid_t tid) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    cpu_set_t all, now;
    CPU_ZERO(&all);
    for (long c = 0; c < n && c < CPU_SETSIZE; ++c) CPU_SET(c, &all);
    if (sched_getaffinity(tid, sizeof now, &now) != 0) return;  // thread gone
    if (!CPU_EQUAL(&now, &all)) sched_setaffinity(tid, sizeof all, &all);
}

bool scx_active() {
    // A sched_ext scheduler (e.g. scx_lavd) is running. It ignores uclamp and
    // does its own placement (LAVD + boost measured slightly worse than LAVD
    // alone).
    return rd(SCX_STATE) == "enabled";
}

std::vector<pid_t> tasks(pid_t pid) {
    std::vector<pid_t> out;
    for (const auto& t : listdir("/proc/" + std::to_string(pid) + "/task"))
        if (is_digits(t)) out.push_back(static_cast<pid_t>(std::atol(t.c_str())));
    return out;
}

// One /proc pass: what each game and UI process gets. comm comes from stat;
// cmdline is read only for `reaper`. roots: Steam's launch reapers (not
// boosted themselves).
std::map<pid_t, unsigned> scan_procs(std::set<pid_t>& roots) {
    std::map<pid_t, std::vector<pid_t>> children;
    std::vector<pid_t> ui;
    roots.clear();
    for (const auto& p : listdir("/proc")) {
        if (!is_digits(p)) continue;
        auto stat = read_file("/proc/" + p + "/stat");
        if (!stat) continue;
        auto open = stat->find('('), close = stat->rfind(')');
        if (open == std::string::npos || close == std::string::npos || close + 2 >= stat->size()) continue;
        std::string comm = stat->substr(open + 1, close - open - 1);
        // After ") ": the state, then the parent.
        const char* rest = stat->c_str() + close + 2;
        const char* sp = std::strchr(rest, ' ');
        if (!sp) continue;
        pid_t pid = static_cast<pid_t>(std::atol(p.c_str()));
        pid_t ppid = static_cast<pid_t>(std::atol(sp + 1));
        children[ppid].push_back(pid);
        if (UI_COMMS.count(comm)) {
            ui.push_back(pid);
        } else if (comm == "reaper") {
            auto cmdline = read_file("/proc/" + p + "/cmdline");
            static const std::string launch("reaper\0SteamLaunch\0", 19);
            if (cmdline && cmdline->find(launch) != std::string::npos) roots.insert(pid);
        }
    }
    std::map<pid_t, unsigned> out;
    std::vector<pid_t> stack(roots.begin(), roots.end());
    while (!stack.empty()) {
        pid_t p = stack.back();
        stack.pop_back();
        auto it = children.find(p);
        if (it == children.end()) continue;
        for (pid_t c : it->second)
            if (out.emplace(c, game_uclamp()).second) stack.push_back(c);
    }
    for (pid_t u : ui) out.emplace(u, UI_UCLAMP);  // a game's own stays the game's
    return out;
}

// The kernel's process events over netlink; -1 when this kernel has none.
int open_proc_events() {
    int fd = socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_CONNECTOR);
    if (fd < 0) return -1;
    sockaddr_nl addr{};
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = CN_IDX_PROC;
    addr.nl_pid = 0;  // the kernel picks
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    int rcvbuf = 4 << 20;  // a game starting is a burst
    setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &rcvbuf, sizeof rcvbuf);
    // nlmsghdr, cn_msg, then the request (cn_msg ends in a flexible array).
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

class Boost {
public:
    explicit Boost(Loop& loop, bool events) : loop_(loop), events_(events) {}

    void start() {
        log("sm8550-boostd: game uclamp.min %u, UI %u; Steam UI pins widened to all CPUs", game_uclamp(), UI_UCLAMP);
        if (events_) {
            cn_ = open_proc_events();
            if (cn_ < 0) log("sm8550-boostd: no process events from this kernel (%s), looking once a second",
                             std::strerror(errno));
        }
        if (cn_ >= 0) {
            log("sm8550-boostd: following process events");
            loop_.on_readable(cn_, [this] { drain(); });
            // The scheduler switched: sched_ext's own kobject comes and goes.
            loop_.on_uevent([this](const std::string& subsystem, const std::string&) {
                if (subsystem == "sched_ext" || subsystem.empty()) rescan();
            });
            rescan();
        } else {
            tick();
        }
    }

    void stop() {
        undo();
        log("sm8550-boostd: stopped, uclamp floors cleared");
    }

private:
    // The once-a-second look, without process events.
    void tick() {
        look();
        loop_.after(TICK_S, [this] { tick(); });
    }

    void rescan() {
        if (pending_) return;
        pending_ = loop_.after(RESCAN_DELAY_S, [this] {
            pending_ = 0;
            look();
        });
    }

    void look() {
        if (scx_active()) {
            if (!yielded_) {
                undo();
                log("sm8550-boostd: sched_ext scheduler active, standing aside");
                yielded_ = true;
            }
            return;
        }
        if (yielded_) {
            log("sm8550-boostd: sched_ext scheduler gone, boosting again");
            yielded_ = false;
        }
        targets_ = scan_procs(roots_);
        std::set<pid_t> live;
        for (const auto& [pid, value] : targets_)
            for (pid_t tid : tasks(pid)) {
                live.insert(tid);
                boost(tid, value);
            }
        for (auto it = done_.begin(); it != done_.end();)
            it = live.count(it->first) ? std::next(it) : done_.erase(it);
    }

    // A thread gets its process's floor once (again if that changed: a UI
    // process that became part of a game).
    void boost(pid_t tid, unsigned value) {
        auto it = done_.find(tid);
        if (it != done_.end() && it->second == value) return;
        bool fresh = it == done_.end();
        if (!set_uclamp_min(tid, value)) return;
        done_[tid] = value;
        if (fresh && value == UI_UCLAMP) unpin(tid);
    }

    void undo() {
        for (const auto& [tid, value] : done_) set_uclamp_min(tid, 0);
        done_.clear();
    }

    bool matters(pid_t pid) const { return targets_.count(pid) || roots_.count(pid); }

    void drain() {
        alignas(nlmsghdr) char buf[16384];
        while (true) {
            ssize_t n = recv(cn_, buf, sizeof buf, 0);
            if (n < 0) {
                if (errno == ENOBUFS) {
                    rescan();  // events were dropped
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
                if (!yielded_) event(*reinterpret_cast<proc_event*>(cn->data));
            }
        }
    }

    void event(const proc_event& ev) {
        switch (ev.what) {
        case PROC_EVENT_FORK: {
            pid_t ptgid = ev.event_data.fork.parent_tgid;
            pid_t cpid = ev.event_data.fork.child_pid, ctgid = ev.event_data.fork.child_tgid;
            if (cpid != ctgid) {
                // A new thread: its process's floor, now.
                auto it = targets_.find(ctgid);
                if (it != targets_.end()) boost(cpid, it->second);
            } else if (matters(ptgid)) {
                // A child of a game, a launch reaper or a UI process (same comm).
                rescan();
            }
            break;
        }
        case PROC_EVENT_EXEC: {
            pid_t pid = ev.event_data.exec.process_tgid;
            if (ev.event_data.exec.process_pid != pid) break;
            if (matters(pid)) {
                rescan();
                break;
            }
            std::string comm = rd("/proc/" + std::to_string(pid) + "/comm");
            if (UI_COMMS.count(comm) || comm == "reaper") rescan();
            break;
        }
        case PROC_EVENT_COMM: {
            pid_t pid = ev.event_data.comm.process_tgid;
            if (ev.event_data.comm.process_pid != pid) break;
            std::string comm(ev.event_data.comm.comm, strnlen(ev.event_data.comm.comm, sizeof ev.event_data.comm.comm));
            if (matters(pid) || UI_COMMS.count(comm) || comm == "reaper") rescan();
            break;
        }
        case PROC_EVENT_EXIT: {
            pid_t pid = ev.event_data.exit.process_pid, tgid = ev.event_data.exit.process_tgid;
            done_.erase(pid);
            // A launch reaper ending can leave its game's daemons elsewhere.
            if (pid == tgid && matters(pid)) rescan();
            break;
        }
        default:
            break;
        }
    }

    Loop& loop_;
    bool events_;
    int cn_ = -1;
    uint64_t pending_ = 0;
    bool yielded_ = false;
    std::map<pid_t, unsigned> targets_;
    std::set<pid_t> roots_;
    std::map<pid_t, unsigned> done_;  // tid -> the floor it was given
};

}  // namespace

int main(int argc, char** argv) {
    bool events = !(argc > 1 && !std::strcmp(argv[1], "--scan"));
    pq::Loop loop;
    Boost boost(loop, events);
    loop.on_signal([&](int sig) {
        if (sig == SIGHUP) return;
        loop.quit();
    });
    boost.start();
    loop.run();
    boost.stop();
    return 0;
}

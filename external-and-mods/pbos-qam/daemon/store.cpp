// The App Store section: emulators and apps from a catalog that ships with
// PB-OS (/usr/share/pbos-qam/store/catalog.json), installed on request.
// PB-OS ships none of them; this downloads the one the user picks.
//
// Two kinds of install, as in Armada's Armada Store (armada-os/armada,
// GPL-2.0-or-later, decky/armada-store), which this follows:
// - flatpak: from Flathub, system-wide (flatpak install --system), then given
//   the user's home and removable drives (flatpak override).
// - appimage: the newest release asset matching a pattern, from a GitHub,
//   GitLab or Forgejo releases feed, into ~/Applications, with a desktop
//   entry for Desktop Mode. "linksInNotes" takes the downloads from links in
//   the release notes, for feeds that attach none (Eden's nightlies).
// Unlike Armada's, the DS and 3DS emulators are the stock ones (Flathub's
// melonDS and Azahar): gamescope's dual-screen branch puts their second
// window on the AYN Thor's bottom screen, and the Barry section's
// "Two-screen emulators" switch sets them up.
//
// The root daemon never writes in the user's home itself: every file there
// is made, moved or removed by a program run as the user (setpriv), so a
// link planted in the home cannot point a root write elsewhere.
//
// Steam shortcuts are made by the frontend (SteamClient.Apps.AddShortcut);
// an install leaves its app on a pending list here until the frontend
// records the shortcut, so an install that ends with Quick Access closed
// still gets one. Each app's Steam artwork ships with the catalog
// (store/art/ID/: capsule, wide, hero, logo, icon); the frontend gives it
// to Steam with the shortcut, and once to shortcuts made before it shipped
// unless the user has art of their own there ("art" in the state).
//
// Nothing polls: one job runs at a time on a worker thread, and its phase
// and progress go to the frontend as "jobs" events. Updates are looked up
// when the page asks (at most hourly, unless the user asks again).

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <thread>
#include <tuple>
#include <utility>

#include "section.h"
#include "util.h"

extern char** environ;

namespace pq {

namespace {

using pj::Value;

const std::string STATE = "/var/lib/pbos-qam/store.json";
constexpr double UPDATES_TTL_S = 3600;
constexpr double FLATPAK_LIST_TTL_S = 3;
// Finished jobs stay listed this long, failed ones longer (their error shows).
constexpr double DONE_TTL_S = 10;
constexpr double ERROR_TTL_S = 300;
constexpr double DOWNLOAD_TIMEOUT_S = 3600;

std::string share_dir() {
    const char* s = std::getenv("PBOS_QAM_SHARE");
    return s && *s ? s : "/usr/share/pbos-qam";
}

std::string catalog_path() { return share_dir() + "/store/catalog.json"; }
std::string art_dir(const std::string& id) { return share_dir() + "/store/art/" + id; }

double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string apps_dir() { return desktop_user().home + "/Applications"; }
std::string desktop_dir() { return desktop_user().home + "/.local/share/applications"; }
std::string icons_dir() { return desktop_user().home + "/.local/share/icons/hicolor/128x128/apps"; }

// A file name from the catalog: no folders, no dot files.
bool plain_name(const std::string& n) {
    return !n.empty() && n[0] != '.' && n.find('/') == std::string::npos && n.size() < 200;
}

// ---------------------------------------------------------- as the user ---
std::vector<std::string> user_env() {
    const User& u = desktop_user();
    return clean_env({"HOME=" + u.home, "USER=" + u.name, "LOGNAME=" + u.name});
}

std::vector<std::string> as_user(std::vector<std::string> argv) {
    const User& u = desktop_user();
    std::vector<std::string> full = {"setpriv", "--reuid=" + std::to_string(u.uid), "--regid=" + std::to_string(u.gid),
                                     "--init-groups", "--"};
    full.insert(full.end(), argv.begin(), argv.end());
    return full;
}

RunResult run_user(std::vector<std::string> argv, std::optional<std::string> input = std::nullopt, double timeout = 60) {
    RunOpts o;
    o.env = user_env();
    o.input = std::move(input);
    o.timeout = timeout;
    return run(as_user(std::move(argv)), o);
}

void need(const RunResult& r, const std::string& what) {
    if (r.started && !r.timed_out && r.code == 0) return;
    std::string why = trim(r.err.empty() ? r.out : r.err);
    throw std::runtime_error(what + (why.empty() ? "" : ": " + why.substr(0, 300)));
}

// --------------------------------------------------- watched child runs ---
struct Cancelled {};

// Runs argv in a terminal of its own (flatpak and curl draw progress only
// on one), giving its output to on_chunk as it comes, until it ends or
// cancel is set (then its process group gets SIGTERM, 10 s later SIGKILL).
// Returns the exit code; tail keeps the output's last 16 KB.
int run_watched(const std::vector<std::string>& argv, const std::vector<std::string>& env,
                const std::atomic<bool>& cancel, const std::function<void(std::string_view)>& on_chunk,
                std::string& tail, double timeout) {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    char name[128];
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0 || ptsname_r(master, name, sizeof name) != 0) {
        if (master >= 0) close(master);
        throw std::runtime_error("no terminal for " + argv[0]);
    }
    winsize ws{24, 100, 0, 0};
    ioctl(master, TIOCSWINSZ, &ws);
    // Held open here too: with no slave open, reading the master fails
    // (EIO) before the child has opened its end.
    int slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0) {
        close(master);
        throw std::runtime_error("no terminal for " + argv[0]);
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, slave, 0);
    posix_spawn_file_actions_adddup2(&fa, slave, 1);
    posix_spawn_file_actions_adddup2(&fa, slave, 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    std::vector<char*> envp;
    for (auto& e : env) envp.push_back(const_cast<char*>(e.c_str()));
    envp.push_back(nullptr);
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0].c_str(), &fa, &attr, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    if (rc != 0) {
        close(slave);
        close(master);
        throw std::runtime_error("cannot start " + argv[0]);
    }
    auto read_some = [&](int wait_ms) {
        pollfd p{master, POLLIN, 0};
        if (poll(&p, 1, wait_ms) <= 0 || !(p.revents & POLLIN)) return false;
        char buf[8192];
        ssize_t got = read(master, buf, sizeof buf);
        if (got <= 0) return false;
        std::string_view chunk(buf, static_cast<size_t>(got));
        tail.append(chunk);
        if (tail.size() > 16384) tail.erase(0, tail.size() - 16384);
        on_chunk(chunk);
        return true;
    };
    double deadline = now_s() + timeout;
    double kill_at = 0;
    bool killed = false;
    int st = 0;
    while (true) {
        if (!killed && (cancel.load() || now_s() > deadline)) {
            kill(-pid, SIGTERM);
            killed = true;
            kill_at = now_s() + 10;
        }
        if (killed && now_s() > kill_at) kill(-pid, SIGKILL);
        if (read_some(250)) continue;
        if (waitpid(pid, &st, WNOHANG) == pid) break;
    }
    while (read_some(0)) {
    }
    close(slave);
    close(master);
    if (killed && cancel.load()) throw Cancelled{};
    if (killed) throw std::runtime_error(argv[0] + " took too long");
    return WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
}

// The last line of a program's output, without terminal codes.
std::string last_line(const std::string& raw) {
    static const std::regex ansi("\x1b\\[[0-9;?]*[A-Za-z]");
    std::string text = std::regex_replace(raw, ansi, "");
    for (auto& c : text)
        if (c == '\r') c = '\n';
    auto lines = split_lines(text);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        std::string t = trim(*it);
        if (!t.empty()) return t.substr(0, 300);
    }
    return "";
}

// ------------------------------------------------------------- releases ---
struct Release {
    std::string tag, url, stamp;
};

Value fetch_json(const std::string& url) {
    RunOpts o;
    o.env = clean_env();
    o.timeout = 40;
    auto r = run({"curl", "-fsSL", "--proto", "=https", "--proto-redir", "=https", "--max-time", "30", "-A",
                  "pbos-app-store/1.0", "-H", "Accept: application/vnd.github+json, application/json;q=0.9", url},
                 o);
    need(r, "Release list not reachable");
    auto v = Value::parse(r.out);
    if (!v) throw std::runtime_error("Release list unreadable");
    return *v;
}

// The newest release (by its date) with an asset matching pattern. A
// prerelease only when the project publishes nothing else. links_in_notes:
// a release without assets offers the HTTPS links in its notes instead,
// named by their last path part.
Release resolve_release(const std::string& feed, const std::string& pattern, bool links_in_notes = false) {
    static const std::regex link_re("https://[^\\s()<>\\[\\]\"'`]+");
    Value data = fetch_json(feed);
    std::vector<Value> releases;
    if (data.is_object()) releases.push_back(data);
    else if (data.is_array()) releases = data.items();
    std::regex re;
    try {
        re = std::regex(pattern);
    } catch (const std::regex_error&) {
        throw std::runtime_error("Bad asset pattern in the catalog");
    }
    for (bool skip_pre : {true, false}) {
        std::optional<std::pair<std::string, Release>> best;
        for (const auto& rel : releases) {
            if (rel["draft"].truthy() || (skip_pre && rel["prerelease"].truthy())) continue;
            std::string date = rel["published_at"].str(rel["released_at"].str(rel["created_at"].str()));
            const Value& assets = rel["assets"];
            // GitLab: assets.links[] {name, url}; GitHub: assets[] {name, browser_download_url}.
            std::vector<std::tuple<std::string, std::string, std::string>> list;
            if (assets.is_object())
                for (const auto& l : assets["links"].items()) list.emplace_back(l["name"].str(), l["url"].str(), "");
            else
                for (const auto& a : assets.items())
                    list.emplace_back(a["name"].str(), a["browser_download_url"].str(),
                                      a["updated_at"].str(a["created_at"].str()));
            if (list.empty() && links_in_notes) {
                const std::string body = rel["body"].str();
                for (auto it = std::sregex_iterator(body.begin(), body.end(), link_re); it != std::sregex_iterator(); ++it) {
                    std::string url = it->str();
                    list.emplace_back(url.substr(url.find_last_of('/') + 1), url, "");
                }
            }
            for (const auto& [name, url, adate] : list) {
                if (url.empty() || !std::regex_search(name, re)) continue;
                // A fixed tag ("nightly") keeps its date when its assets are
                // replaced: the asset's own date tells the build.
                if (!best || date > best->first) best = {date, Release{rel["tag_name"].str(), url, adate.empty() ? date : adate}};
                break;
            }
        }
        if (best) return best->second;
    }
    throw std::runtime_error("No download found for this device");
}

// ---------------------------------------------------------------- jobs ---
const std::set<std::string> ACTIVE = {"queued", "resolving", "downloading", "installing", "removing"};

struct Job {
    std::string app_id, action, phase = "queued", error;
    std::optional<int> percent;
    double finished_at = 0;
    std::atomic<bool> cancel{false};

    Value snapshot() const {
        Value v = Value::object();
        v["appId"] = app_id;
        v["action"] = action;
        v["phase"] = phase;
        v["percent"] = percent ? Value(*percent) : Value();
        v["error"] = error;
        return v;
    }
};

class Store : public Section {
public:
    std::string name() const override { return "store"; }
    bool here() const override { return is_file(catalog_path()); }

    ~Store() override { stop(); }

    void start(Host& h) override { host_ = &h; }

    void stop() override {
        {
            std::lock_guard<std::mutex> g(mu_);
            stopping_ = true;
            for (auto& [_, j] : jobs_) j->cancel = true;
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    Value call(const std::string& m, const Value& a) override {
        if (m == "get_catalog") return catalog_payload();
        if (m == "get_status") return status();
        if (m == "check_updates") return updates(arg(a, 0, false).truthy());
        if (m == "install") return start_job(a.at(0).str(), "install");
        if (m == "uninstall") return start_job(a.at(0).str(), "uninstall");
        // Tests (pbos-qamd --call, no Steam): the job runs before the answer.
        if (m == "install_now" || m == "uninstall_now") {
            std::string id = a.at(0).str();
            if (!find_app(id)) throw CallError("Unknown app: " + id);
            Job j;
            j.app_id = id;
            j.action = m == "install_now" ? "install" : "uninstall";
            execute(j);
            return j.snapshot();
        }
        if (m == "cancel") return cancel(a.at(0).str());
        if (m == "dismiss") return dismiss(a.at(0).str());
        if (m == "record_shortcut") return record_shortcut(a.at(0).str(), a.at(1).as_int());
        if (m == "clear_shortcut") return clear_shortcut(a.at(0).str(), a.at(1));
        if (m == "prepare_shortcut") return prepare_shortcut(a.at(0).str());
        if (m == "artwork") return artwork(a.at(0).str(), a.at(1).as_int());
        if (m == "record_art") return record_art(a.at(0).str(), a.at(1).as_int());
        throw CallError("store has no method " + m);
    }

private:
    // ------------------------------------------------------ catalog ---
    static std::vector<Value> apps() {
        Value c = read_json(catalog_path());
        return c["apps"].is_array() ? c["apps"].items() : std::vector<Value>{};
    }

    static std::optional<Value> find_app(const std::string& id) {
        for (auto& a : apps())
            if (a["id"].str() == id) return a;
        return std::nullopt;
    }

    static Value launch_spec(const Value& app) {
        const Value& in = app["install"];
        if (app["desktopOnly"].truthy()) return Value();
        std::string kind = in["type"].str();
        std::string name = app["name"].str(app["id"].str());
        std::string extra = trim(in["launchOptions"].str());
        Value v = Value::object();
        v["name"] = name;
        if (kind == "flatpak" && !in["ref"].str().empty()) {
            v["exe"] = "/usr/bin/flatpak";
            v["startDir"] = desktop_user().home;
            v["launchOptions"] = trim("run " + in["ref"].str() + " " + extra);
            return v;
        }
        if (kind == "appimage" && plain_name(in["filename"].str())) {
            v["exe"] = apps_dir() + "/" + in["filename"].str();
            v["startDir"] = apps_dir();
            v["launchOptions"] = extra;
            return v;
        }
        return Value();
    }

    Value catalog_payload() {
        Value list = Value::array();
        for (const auto& app : apps()) {
            Value v = Value::object();
            v["id"] = app["id"].str();
            v["name"] = app["name"].str();
            v["summary"] = app["summary"].str();
            v["category"] = app["category"].str();
            v["icon"] = app["icon"].str();
            v["note"] = app["note"].str();
            v["installType"] = app["install"]["type"].str();
            v["desktopOnly"] = app["desktopOnly"].truthy();
            v["launch"] = launch_spec(app);
            list.push(v);
        }
        Value out = Value::object();
        out["apps"] = list;
        out["home"] = desktop_user().home;
        return out;
    }

    // ------------------------------------------------------- state ---
    Value load_state() {
        std::lock_guard<std::mutex> g(state_mu_);
        return read_json(STATE);
    }

    void mutate_state(const std::function<void(Value&)>& fn) {
        std::lock_guard<std::mutex> g(state_mu_);
        Value s = read_json(STATE);
        fn(s);
        makedirs(dirname(STATE), 0755);
        write_json(STATE, s);
    }

    // ---------------------------------------------------- installed ---
    struct Flatpaks {
        std::set<std::string> system, user;
    };

    // What Flatpak has (system-wide and the user's own, as EmuDeck installs
    // them), asked as the user, so both installations are seen.
    Flatpaks flatpaks(bool fresh = false) {
        std::lock_guard<std::mutex> g(fp_mu_);
        if (!fresh && fp_at_ > 0 && now_s() - fp_at_ <= FLATPAK_LIST_TTL_S) return fp_;
        auto r = run_user({"flatpak", "list", "--app", "--columns=application,installation"}, std::nullopt, 20);
        if (r.started && r.code == 0) {
            Flatpaks f;
            for (const auto& line : split_lines(r.out)) {
                auto tab = line.find('\t');
                std::string ref = trim(line.substr(0, tab));
                std::string where = tab == std::string::npos ? "" : trim(line.substr(tab + 1));
                if (ref.empty()) continue;
                (where == "system" ? f.system : f.user).insert(ref);
            }
            fp_ = f;
        }
        fp_at_ = now_s();
        return fp_;
    }

    // installed; managed: installed by the App Store (it may update and
    // uninstall it); version: the release tag it installed.
    Value installed_info(const Value& app, const Value& state, const Flatpaks& fp) {
        const Value& in = app["install"];
        std::string kind = in["type"].str();
        Value v = Value::object();
        if (kind == "flatpak") {
            std::string ref = in["ref"].str();
            bool sys = fp.system.count(ref) > 0;
            v["installed"] = sys || fp.user.count(ref) > 0;
            // System-wide: ours to manage (the App Store installs there).
            // The user's own installation (EmuDeck's): left alone.
            v["managed"] = sys;
            return v;
        }
        if (kind == "appimage") {
            std::string id = app["id"].str();
            std::string file = in["filename"].str();
            bool there = plain_name(file) && is_file(apps_dir() + "/" + file);
            const Value* rec = state["appimages"].find(id);
            bool ours = rec && rec->is_object() && (*rec)["filename"].str() == file;
            v["installed"] = there;
            // A file of that name that the App Store didn't put there (EmuDeck
            // keeps its AppImages in ~/Applications too) is never replaced.
            v["managed"] = there && ours;
            if (there && ours && !(*rec)["tag"].str().empty()) v["version"] = (*rec)["tag"].str();
            return v;
        }
        v["installed"] = false;
        v["managed"] = false;
        return v;
    }

    Value status() {
        Value state = load_state();
        Flatpaks fp = flatpaks();
        Value installed = Value::object();
        for (const auto& app : apps()) {
            std::string id = app["id"].str();
            if (!id.empty()) installed[id] = installed_info(app, state, fp);
        }
        Value out = Value::object();
        out["jobs"] = jobs_snapshot();
        out["installed"] = installed;
        out["shortcuts"] = state["shortcuts"].is_object() ? state["shortcuts"] : Value::object();
        out["pending"] = state["pendingShortcuts"].is_array() ? state["pendingShortcuts"] : Value::array();
        out["art"] = state["art"].is_object() ? state["art"] : Value::object();
        return out;
    }

    // ------------------------------------------------------ updates ---
    Value updates(bool force) {
        Value state = load_state();
        Flatpaks fp = flatpaks();
        {
            std::lock_guard<std::mutex> g(up_mu_);
            if (force || up_at_ <= 0 || now_s() - up_at_ > UPDATES_TTL_S) refresh_updates(state, fp);
        }
        std::lock_guard<std::mutex> g(up_mu_);
        Value out = Value::object();
        for (const auto& app : apps()) {
            std::string id = app["id"].str();
            const Value& in = app["install"];
            std::string kind = in["type"].str();
            Value info = installed_info(app, state, fp);
            if (!info["managed"].truthy()) continue;
            if (kind == "flatpak" && up_flatpak_.count(in["ref"].str())) {
                out[id]["latest"] = "";
            } else if (kind == "appimage") {
                const Value& rec = std::as_const(state)["appimages"][id];
                std::string cur = rec["stamp"].str(), latest = up_stamps_.count(id) ? up_stamps_[id] : "";
                if (cur.empty() || latest.empty()) {
                    cur = rec["tag"].str();
                    latest = up_tags_.count(id) ? up_tags_[id] : "";
                }
                if (!cur.empty() && !latest.empty() && cur != latest) out[id]["latest"] = up_tags_[id];
            }
        }
        return out;
    }

    // up_mu_ held. A failed lookup is never cached as "nothing new".
    void refresh_updates(const Value& state, const Flatpaks& fp) {
        bool complete = true;
        RunOpts o;
        o.env = clean_env();
        o.timeout = 90;
        auto r = run({"flatpak", "remote-ls", "--updates", "--system", "--app", "--columns=application", "flathub"}, o);
        if (r.started && r.code == 0) {
            up_flatpak_.clear();
            for (const auto& l : split_lines(r.out))
                if (!trim(l).empty()) up_flatpak_.insert(trim(l));
        } else {
            complete = false;
        }
        for (const auto& app : apps()) {
            const Value& in = app["install"];
            if (in["type"].str() != "appimage" || in["releases"].str().empty()) continue;
            if (!installed_info(app, state, fp)["managed"].truthy()) continue;
            try {
                Release rel = resolve_release(in["releases"].str(), in["asset"].str(), in["linksInNotes"].truthy());
                up_tags_[app["id"].str()] = rel.tag;
                up_stamps_[app["id"].str()] = rel.stamp;
            } catch (const std::exception&) {
                complete = false;
            }
        }
        up_at_ = complete ? now_s() : 0;
    }

    // --------------------------------------------------------- jobs ---
    Value jobs_snapshot() {
        std::lock_guard<std::mutex> g(mu_);
        double now = now_s();
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            auto& j = it->second;
            double ttl = j->phase == "error" ? ERROR_TTL_S : DONE_TTL_S;
            if (j->finished_at > 0 && now - j->finished_at > ttl) it = jobs_.erase(it);
            else ++it;
        }
        Value list = Value::array();
        for (auto& [_, j] : jobs_) list.push(j->snapshot());
        return list;
    }

    void changed() {
        if (host_) host_->emit(name(), "jobs", Value(Value::Array{jobs_snapshot()}));
    }

    void set_phase(Job& j, const std::string& phase, std::optional<int> percent = std::nullopt) {
        {
            std::lock_guard<std::mutex> g(mu_);
            if (j.phase == phase && j.percent == percent) return;
            j.phase = phase;
            j.percent = percent;
        }
        changed();
    }

    Value start_job(const std::string& id, const std::string& action) {
        if (!find_app(id)) throw CallError("Unknown app: " + id);
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = jobs_.find(id);
            if (it != jobs_.end() && ACTIVE.count(it->second->phase)) throw CallError("Already in progress");
            auto j = std::make_shared<Job>();
            j->app_id = id;
            j->action = action;
            jobs_[id] = j;
            queue_.push_back(id);
            if (!worker_.joinable()) worker_ = std::thread([this] { work(); });
        }
        cv_.notify_all();
        changed();
        return true;
    }

    Value cancel(const std::string& id) {
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = jobs_.find(id);
            if (it == jobs_.end() || !ACTIVE.count(it->second->phase)) return false;
            auto q = std::find(queue_.begin(), queue_.end(), id);
            if (it->second->phase == "queued" && q != queue_.end()) {
                queue_.erase(q);
                it->second->phase = "cancelled";
                it->second->finished_at = now_s();
            } else {
                it->second->cancel = true;
            }
        }
        changed();
        return true;
    }

    Value dismiss(const std::string& id) {
        {
            std::lock_guard<std::mutex> g(mu_);
            auto it = jobs_.find(id);
            if (it != jobs_.end() && !ACTIVE.count(it->second->phase)) jobs_.erase(it);
        }
        changed();
        return true;
    }

    void work() {
        while (true) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> l(mu_);
                cv_.wait(l, [this] { return stopping_ || !queue_.empty(); });
                if (stopping_) return;
                std::string id = queue_.front();
                queue_.pop_front();
                auto it = jobs_.find(id);
                if (it == jobs_.end()) continue;
                job = it->second;
            }
            execute(*job);
            {
                std::lock_guard<std::mutex> g(mu_);
                job->finished_at = now_s();
            }
            changed();
        }
    }

    void execute(Job& j) {
        try {
            auto app = find_app(j.app_id);
            if (!app) throw std::runtime_error("No longer in the catalog");
            if (j.action == "install") install(j, *app);
            else uninstall(j, *app);
            std::lock_guard<std::mutex> g(mu_);
            j.phase = "done";
            j.percent.reset();
        } catch (const Cancelled&) {
            if (auto app = find_app(j.app_id)) cleanup_partial(*app);
            std::lock_guard<std::mutex> g(mu_);
            j.phase = "cancelled";
            j.percent.reset();
        } catch (const std::exception& e) {
            if (auto app = find_app(j.app_id)) cleanup_partial(*app);
            log("store: %s %s failed: %s", j.action.c_str(), j.app_id.c_str(), e.what());
            std::lock_guard<std::mutex> g(mu_);
            j.phase = "error";
            j.percent.reset();
            j.error = e.what();
        }
    }

    // ------------------------------------------------------ install ---
    void install(Job& j, const Value& app) {
        std::string kind = app["install"]["type"].str();
        Value info = installed_info(app, load_state(), flatpaks(true));
        if (info["installed"].truthy() && !info["managed"].truthy())
            throw std::runtime_error("Already installed outside the App Store; update it where it came from");
        if (kind == "flatpak") install_flatpak(j, app);
        else if (kind == "appimage") install_appimage(j, app);
        else throw std::runtime_error("Unknown install type: " + kind);
        post_install(app);
        if (!launch_spec(app).is_null()) add_pending(app["id"].str());
    }

    void install_flatpak(Job& j, const Value& app) {
        const Value& in = app["install"];
        std::string ref = in["ref"].str();
        set_phase(j, "installing");
        static const std::regex op_re("(?:Installing|Updating)\\s+(\\d+)/(\\d+)");
        static const std::regex pct_re("(\\d{1,3})%");
        int op = 0, ops = 0;
        std::string tail;
        int code = run_watched(
            {"flatpak", "install", "--system", "--or-update", "-y", "flathub", ref},
            clean_env({"TERM=xterm", "LC_ALL=C.UTF-8"}), j.cancel,
            [&](std::string_view chunk) {
                std::string s(chunk);
                for (auto it = std::sregex_iterator(s.begin(), s.end(), op_re); it != std::sregex_iterator(); ++it) {
                    op = std::stoi((*it)[1]);
                    ops = std::stoi((*it)[2]);
                }
                int last = -1;
                for (auto it = std::sregex_iterator(s.begin(), s.end(), pct_re); it != std::sregex_iterator(); ++it)
                    last = std::min(100, std::stoi((*it)[1]));
                if (last < 0) return;
                // flatpak restarts its percentage for each runtime and the app.
                int pct = ops > 1 && op > 0 ? static_cast<int>(((op - 1) + last / 100.0) / ops * 100) : last;
                set_phase(j, "installing", std::clamp(pct, 0, 100));
            },
            tail, DOWNLOAD_TIMEOUT_S);
        if (code != 0) {
            std::string why = last_line(tail);
            if (why.find("already installed") == std::string::npos)
                throw std::runtime_error(why.empty() ? "flatpak install failed (" + std::to_string(code) + ")" : why);
        }
        // Emulators' manifests rarely grant removable drives (some no files at
        // all); persistent, so a frontend that runs the Flatpak directly
        // (ES-DE) has it too.
        std::vector<std::string> ov = {"flatpak", "override", "--system", ref, "--filesystem=" + desktop_user().home,
                                       "--filesystem=/run/media", "--filesystem=/media"};
        for (const auto& o : in["overrides"].items()) ov.push_back(o.str());
        RunOpts o;
        o.env = clean_env();
        need(run(ov, o), "flatpak override failed");
        std::lock_guard<std::mutex> g(fp_mu_);
        fp_at_ = 0;
    }

    void install_appimage(Job& j, const Value& app) {
        const Value& in = app["install"];
        std::string id = app["id"].str();
        std::string file = in["filename"].str();
        if (!plain_name(file)) throw std::runtime_error("Bad file name in the catalog");
        set_phase(j, "resolving");
        Release rel = resolve_release(in["releases"].str(), in["asset"].str(), in["linksInNotes"].truthy());
        if (rel.url.rfind("https://", 0) != 0) throw std::runtime_error("Refusing a download that isn't HTTPS");
        const Value before = load_state();
        std::string previous = before["appimages"][id]["filename"].str();
        need(run_user({"mkdir", "-p", apps_dir()}), "Cannot make ~/Applications");
        std::string part = apps_dir() + "/." + file + ".pbos-part";
        set_phase(j, "downloading", 0);
        static const std::regex pct_re("(\\d{1,3})(?:\\.\\d)?%");
        std::string tail;
        int code = run_watched(
            as_user({"curl", "-fL", "--proto", "=https", "--proto-redir", "=https", "--retry", "2", "-#", "-A",
                     "pbos-app-store/1.0", "-o", part, rel.url}),
            user_env(), j.cancel,
            [&](std::string_view chunk) {
                std::string s(chunk);
                int last = -1;
                for (auto it = std::sregex_iterator(s.begin(), s.end(), pct_re); it != std::sregex_iterator(); ++it)
                    last = std::min(100, std::stoi((*it)[1]));
                if (last >= 0) set_phase(j, "downloading", last);
            },
            tail, DOWNLOAD_TIMEOUT_S);
        if (code != 0) {
            std::string why = last_line(tail);
            throw std::runtime_error("Download failed" + (why.empty() ? "" : ": " + why));
        }
        set_phase(j, "installing");
        need(run_user({"chmod", "0755", part}), "Cannot make it executable");
        need(run_user({"mv", "-f", "-T", part, apps_dir() + "/" + file}), "Cannot put it in ~/Applications");
        mutate_state([&](Value& s) {
            Value rec = Value::object();
            rec["filename"] = file;
            rec["tag"] = rel.tag;
            rec["stamp"] = rel.stamp;
            s["appimages"][id] = rec;
        });
        // A catalog file name change would otherwise strand the old download.
        if (!previous.empty() && previous != file && plain_name(previous))
            run_user({"rm", "-f", "--", apps_dir() + "/" + previous});
        try {
            write_desktop_entry(app, apps_dir() + "/" + file);
        } catch (const std::exception& e) {
            log("store: %s: no desktop entry: %s", id.c_str(), e.what());
        }
    }

    void cleanup_partial(const Value& app) {
        std::string file = app["install"]["filename"].str();
        if (app["install"]["type"].str() == "appimage" && plain_name(file))
            run_user({"rm", "-f", "--", apps_dir() + "/." + file + ".pbos-part"});
    }

    // An AppImage is only a file, so Desktop Mode's menu needs an entry
    // (Flatpaks bring their own).
    static std::string desktop_name(const std::string& id) { return "pbos-store-" + id + ".desktop"; }

    static std::string desktop_value(std::string s) {
        std::string out;
        for (char c : s) {
            if (c == '\n' || c == '\r') out += ' ';
            else if (c == '\\') out += "\\\\";
            else out += c;
        }
        return trim(out);
    }

    void write_desktop_entry(const Value& app, const std::string& exec) {
        std::string id = app["id"].str();
        std::string icon;
        std::string url = app["icon"].str();
        if (url.rfind("https://", 0) == 0) {
            std::string path = icons_dir() + "/pbos-store-" + id + ".png";
            if (run_user({"mkdir", "-p", icons_dir()}).code == 0 &&
                run_user({"curl", "-fsSL", "--proto", "=https", "--proto-redir", "=https", "--max-time", "30",
                          "--max-filesize", "5242880", "-o", path, url})
                        .code == 0)
                icon = path;
        }
        std::string cat = app["category"].str() == "emulators" ? "Game;Emulator;" : "Game;";
        std::string text = "[Desktop Entry]\nType=Application\nName=" + desktop_value(app["name"].str(id)) +
                           "\nComment=" + desktop_value(app["summary"].str()) + "\nExec=\"" + exec + "\"\n" +
                           (icon.empty() ? "" : "Icon=" + icon + "\n") + "Terminal=false\nCategories=" + cat +
                           "\nX-PBOS-App-Store=" + id + "\n";
        need(run_user({"mkdir", "-p", desktop_dir()}), "Cannot make the applications folder");
        RunOpts o;
        o.env = user_env();
        o.input = text;
        need(run(as_user({"tee", desktop_dir() + "/" + desktop_name(id)}), o), "Cannot write the desktop entry");
    }

    void remove_desktop_entry(const std::string& id) {
        std::string entry = desktop_dir() + "/" + desktop_name(id);
        // Only an entry the App Store wrote.
        if (run_user({"cat", "--", entry}).out.find("X-PBOS-App-Store=" + id) != std::string::npos)
            run_user({"rm", "-f", "--", entry});
        run_user({"rm", "-f", "--", icons_dir() + "/pbos-store-" + id + ".png"});
    }

    // -------------------------------------------------- post-install ---
    // Named by a catalog app's "postInstall". Updates reinstall, so each one
    // runs again: it must never undo a choice the user made.
    void post_install(const Value& app) {
        std::string handler = app["postInstall"].str();
        if (handler.empty()) return;
        if (handler == "retroarch-cores-url") return retroarch_cores_url();
        if (handler == "srm-defaults") return srm_defaults();
        throw std::runtime_error("Unknown postInstall handler: " + handler);
    }

    // Flathub's RetroArch is built from a RetroArch whose config.def.h has
    // no Linux aarch64 buildbot address, so its Online Updater has nowhere
    // to download cores from. Filled in only while the user's setting is
    // empty (found by Armada's store).
    void retroarch_cores_url() {
        const std::string key = "core_updater_buildbot_cores_url";
        const std::string url = "https://buildbot.libretro.com/nightly/linux/aarch64/latest/";
        std::string dir = desktop_user().home + "/.var/app/org.libretro.RetroArch/config/retroarch";
        std::string cfg = dir + "/retroarch.cfg";
        // Read as the user too: a link there must not show the user a root-only file.
        std::optional<std::string> text;
        if (auto r = run_user({"cat", "--", cfg}); r.code == 0) text = r.out;
        if (!text) {
            // RetroArch's own first-run config (its paths), as RetroArch copies it.
            RunOpts o;
            o.env = clean_env();
            auto r = run({"flatpak", "info", "--show-location", "--system", "org.libretro.RetroArch"}, o);
            auto skel = r.code == 0 ? read_file(trim(r.out) + "/files/etc/retroarch.cfg") : std::nullopt;
            if (!skel) return;  // first run is left to RetroArch
            text = *skel + "\nconfig_save_on_exit = \"true\"\n";
        }
        std::string out;
        for (const auto& line : split_lines(*text)) {
            auto eq = line.find('=');
            if (eq != std::string::npos && trim(line.substr(0, eq)) == key) {
                std::string v = trim(line.substr(eq + 1));
                if (v.size() >= 2 && v.front() == '"') v = v.substr(1, v.size() - 2);
                if (!v.empty()) return;  // the user's (or RetroArch's) own value
                continue;
            }
            out += line + "\n";
        }
        out += key + " = \"" + url + "\"\n";
        need(run_user({"mkdir", "-p", dir}), "Cannot make RetroArch's config folder");
        std::string tmp = dir + "/.retroarch.cfg.pbos-tmp";
        RunOpts o;
        o.env = user_env();
        o.input = out;
        need(run(as_user({"tee", tmp}), o), "Cannot write RetroArch's config");
        need(run_user({"mv", "-f", "-T", tmp, cfg}), "Cannot write RetroArch's config");
    }

    // PB-OS's Steam ROM Manager parsers, only while SRM has no config of its
    // own and EmuDeck is not set up (pbos-home-defaults decides both).
    void srm_defaults() {
        need(run_user({"/usr/lib/steamos/pbos-home-defaults", "srm"}), "Cannot set up Steam ROM Manager");
    }

    // ---------------------------------------------------- uninstall ---
    void uninstall(Job& j, const Value& app) {
        set_phase(j, "removing");
        std::string id = app["id"].str();
        std::string kind = app["install"]["type"].str();
        Value info = installed_info(app, load_state(), flatpaks(true));
        if (!info["managed"].truthy()) throw std::runtime_error("Not installed by the App Store");
        clear_pending(id);
        if (kind == "flatpak") {
            std::string tail;
            int code = run_watched({"flatpak", "uninstall", "--system", "-y", app["install"]["ref"].str()},
                                   clean_env({"TERM=xterm", "LC_ALL=C.UTF-8"}), j.cancel,
                                   [](std::string_view) {}, tail, 600);
            std::string why = last_line(tail);
            if (code != 0 && why.find("not installed") == std::string::npos)
                throw std::runtime_error(why.empty() ? "flatpak uninstall failed" : why);
            std::lock_guard<std::mutex> g(fp_mu_);
            fp_at_ = 0;
        } else if (kind == "appimage") {
            std::string file = app["install"]["filename"].str();
            if (plain_name(file)) need(run_user({"rm", "-f", "--", apps_dir() + "/" + file}), "Cannot remove it");
            remove_desktop_entry(id);
            mutate_state([&](Value& s) { s["appimages"].erase(id); });
        }
    }

    // ---------------------------------------------------- shortcuts ---
    void add_pending(const std::string& id) {
        mutate_state([&](Value& s) {
            if (std::as_const(s)["shortcuts"].has(id)) return;  // an update keeps its shortcut
            // Taken after any other key is added: adding one moves the others.
            Value& p = s["pendingShortcuts"];
            if (!p.is_array()) p = Value::array();
            for (const auto& x : p.items())
                if (x.str() == id) return;
            p.push(id);
        });
    }

    void clear_pending(const std::string& id) {
        mutate_state([&](Value& s) {
            if (!s["pendingShortcuts"].is_array()) return;
            auto& items = s["pendingShortcuts"].items();
            items.erase(std::remove_if(items.begin(), items.end(), [&](const Value& x) { return x.str() == id; }),
                        items.end());
        });
    }

    Value record_shortcut(const std::string& id, long long appid) {
        if (!find_app(id) || appid <= 0) throw CallError("Bad shortcut");
        mutate_state([&](Value& s) { s["shortcuts"][id] = appid; });
        clear_pending(id);
        return true;
    }

    // expected: the shortcut the frontend removed; a record changed since
    // (a new shortcut) stays.
    Value clear_shortcut(const std::string& id, const Value& expected) {
        mutate_state([&](Value& s) {
            const Value* cur = std::as_const(s)["shortcuts"].find(id);
            if (!cur) return;
            if (!expected.is_null() && cur->as_int() != expected.as_int()) return;
            s["shortcuts"].erase(id);
        });
        return true;
    }

    // The app's artwork for its shortcut: Steam's asset types (0 capsule,
    // 1 hero, 2 logo, 3 wide) with file type and base64 data, the icon's
    // path, and whether the user already has a capsule of their own for
    // that shortcut in Steam's grid folder (then nothing is replaced).
    Value artwork(const std::string& id, long long appid) {
        if (!find_app(id) || appid <= 0) throw CallError("Bad shortcut");
        static const std::pair<int, const char*> kinds[] = {{0, "capsule"}, {1, "hero"}, {2, "logo"}, {3, "wide"}};
        const std::string dir = art_dir(id);
        Value images = Value::array();
        for (const auto& [type, name] : kinds) {
            for (const char* ext : {"jpg", "png"}) {
                auto data = read_file(dir + "/" + name + "." + ext);
                if (!data) continue;
                Value img = Value::object();
                img["type"] = type;
                img["ext"] = ext;
                img["data"] = base64(*data);
                images.push(std::move(img));
                break;
            }
        }
        // Only names are looked at in the user's Steam folder, nothing is read.
        const std::string home = desktop_user().home;
        std::string steam = realpath(home + "/.steam/steam");
        if (!is_dir(steam)) steam = home + "/.local/share/Steam";
        Value v = Value::object();
        v["images"] = std::move(images);
        v["icon"] = is_file(dir + "/icon.png") ? dir + "/icon.png" : "";
        v["hasOwn"] = !glob(steam + "/userdata/*/config/grid/" + std::to_string(appid) + "p.*").empty();
        return v;
    }

    Value record_art(const std::string& id, long long appid) {
        if (!find_app(id) || appid <= 0) throw CallError("Bad shortcut");
        mutate_state([&](Value& s) { s["art"][id] = appid; });
        return true;
    }

    // A file the user picked, as a Steam shortcut ("Add a Non-Steam Game":
    // Steam's own browse button does nothing on the ARM client).
    Value prepare_shortcut(const std::string& path) {
        if (path.empty() || path[0] != '/') throw CallError("Pick a file");
        std::string real = realpath(path);
        const User& u = desktop_user();
        bool allowed = false;
        for (const std::string& r : {u.home, std::string("/run/media"), std::string("/media"), std::string("/mnt")})
            if (real.rfind(r + "/", 0) == 0) allowed = true;
        struct stat st;
        if (!allowed || lstat(real.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) throw CallError("Not a file that can be added");
        // A download often lacks the execute bit; only the user's own file gets it.
        if (st.st_uid == u.uid && !(st.st_mode & S_IXUSR)) run_user({"chmod", "u+x", "--", real});
        std::string base = basename(real);
        static const std::regex ext("\\.(appimage|sh|bin|x86_64|aarch64)$", std::regex::icase);
        static const std::regex seps("[-_.]+");
        std::string name = trim(std::regex_replace(std::regex_replace(base, ext, ""), seps, " "));
        Value v = Value::object();
        v["name"] = name.empty() ? base : name;
        v["exe"] = real;
        v["startDir"] = dirname(real);
        v["launchOptions"] = "";
        return v;
    }

    Host* host_ = nullptr;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::string, std::shared_ptr<Job>> jobs_;
    std::deque<std::string> queue_;
    std::thread worker_;
    bool stopping_ = false;
    std::mutex state_mu_;
    std::mutex fp_mu_;
    Flatpaks fp_;
    double fp_at_ = 0;
    std::mutex up_mu_;
    double up_at_ = 0;
    std::set<std::string> up_flatpak_;
    std::map<std::string, std::string> up_tags_, up_stamps_;
};

}  // namespace

std::unique_ptr<Section> make_store_section() { return std::make_unique<Store>(); }

}  // namespace pq

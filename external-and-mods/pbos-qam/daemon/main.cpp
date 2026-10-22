// pbos-qamd: PB-OS's own section of Steam's Quick Access menu, without
// Decky. One native process: it puts the PB-OS tab into Steam's Game Mode
// page over CEF's DevTools port (as Decky Loader puts its own), and is the
// backend for every section in it (PB-OS Tools, Barry Launcher on the Thor),
// which the Decky plugins' Python did under box64.
//
// The frontend reaches it through a DevTools binding (Runtime.addBinding):
// window.__pbosQamCall(json) arrives here as Runtime.bindingCalled, and the
// answer goes back with Runtime.evaluate. No port of its own is opened; on
// the AYN Thor, Barry Launcher reaches a few settings through a local socket
// only its user may use (barrylink.cpp).
//
// Steam coming up is heard, not polled for: the web helper's log in Steam's
// logs folder is written as it starts (inotify, only while not connected);
// then the page is looked for, briefly backing off, until it is there.
// The connection closing is Steam (or its web helper) going away.
//
// pbos-qamd --check   runs the frontend's Steam-internals probe in the
//                     running Steam and prints what it found (the release
//                     check, see STEAM-INTERNALS.md).
// pbos-qamd --call SECTION METHOD [ARGS]
//                     one backend call without Steam, for testing.
// pbos-qamd --lights  the stick light engine (lights.cpp), the unit
//                     pbos-utils-lights the tools section starts.

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

#include "barrylink.h"
#include "cdp.h"
#include "lights.h"
#include "loop.h"
#include "section.h"
#include "util.h"

namespace pq {

namespace {

const char* const kVersion = "1.0.0";
const char* const kBinding = "__pbosQamCall";
// How long a Steam start is waited for after a hint that it is starting.
constexpr double kStartWindow = 180;

double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string share_dir() {
    const char* s = std::getenv("PBOS_QAM_SHARE");
    return s && *s ? s : "/usr/share/pbos-qam";
}

bool decky_installed() {
    auto r = run({"systemctl", "is-enabled", "plugin_loader.service"});
    return trim(r.out) == "enabled" || exists(desktop_user().home + "/homebrew/services/PluginLoader");
}

std::string evaluate(int id, const std::string& expr, bool await_value = false) {
    pj::Value msg = pj::Value::object();
    msg["id"] = id;
    msg["method"] = "Runtime.evaluate";
    msg["params"]["expression"] = expr;
    if (await_value) {
        msg["params"]["awaitPromise"] = true;
        msg["params"]["returnByValue"] = true;
    }
    return msg.dump();
}

std::string command(int id, const char* method, pj::Value params = pj::Value::object()) {
    pj::Value msg = pj::Value::object();
    msg["id"] = id;
    msg["method"] = method;
    msg["params"] = std::move(params);
    return msg.dump();
}

class Daemon : public Host {
public:
    explicit Daemon(Loop& loop) : Host(loop) {
        kick_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        stop_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    }

    ~Daemon() override {
        close(kick_);
        close(stop_);
    }

    bool start() {
        for (auto make : {make_tools_section, make_barry_section, make_files_section, make_store_section, make_notices_section}) {
            auto s = make();
            if (s->here()) sections_.push_back(std::move(s));
        }
        if (!build_script()) return false;
        for (auto& s : sections_) s->start(*this);
        // Barry Launcher's quick settings (AYN Thor only).
        for (auto& s : sections_)
            if (s->name() == "barry")
                barry_link_start(
                    [this](const std::string& name) -> Section* {
                        for (auto& t : sections_)
                            if (t->name() == name) return t.get();
                        return nullptr;
                    },
                    *this);
        Loop::work(remove_old_plugins);
        watch_steam();
        cdp_ = std::thread([this] { cdp_thread(); });
        kick();  // Steam may be running already
        return true;
    }

    void stop() {
        stopping_ = true;
        uint64_t one = 1;
        ssize_t r = write(stop_, &one, sizeof one);
        (void)r;
        if (cdp_.joinable()) cdp_.join();
        barry_link_stop();
        for (auto& s : sections_) s->stop();
    }

    void emit(const std::string& section, const std::string& event, pj::Value args) override {
        if (!connected_) return;  // nobody to tell; the frontend asks again when it loads
        std::string expr = "window.__pbosQam&&window.__pbosQam._emit(" + pj::quote(section) + "," + pj::quote(event) +
                           "," + args.dump() + ")";
        ws_.send_text(evaluate(next_id(), expr));
    }

private:
    int next_id() { return ++msg_id_; }

    bool build_script() {
        // The loader (with @decky/ui), then each section's script.
        std::string dir = share_dir();
        auto loader = read_file(dir + "/pbos-qam.js");
        if (!loader) {
            log("pbos-qamd: %s/pbos-qam.js is missing", dir.c_str());
            return false;
        }
        pj::Value boot = pj::Value::object();
        boot["version"] = kVersion;
        boot["decky"] = decky_installed();
        pj::Value names = pj::Value::array();
        std::string sections;
        for (auto& s : sections_) {
            names.push(s->name());
            auto code = read_file(dir + "/sections/" + s->name() + ".js");
            if (!code) continue;  // a backend only (files)
            sections += "\n;(function(){\n" + *code + "\n})();\n";
        }
        boot["sections"] = names;
        script_ = "(function(){\nif (window.__pbosQamLoaded) return;\nwindow.__pbosQamLoaded = true;\nwindow.__pbosQamBoot = " +
                  boot.dump() + ";\n" + *loader + "\n" + sections + "\n})();";
        log("pbos-qamd %s: sections %s; Decky %s", kVersion, names.dump().c_str(),
            boot["decky"].truthy() ? "installed (its toasts are used)" : "not installed");
        return true;
    }

    static void remove_old_plugins() {
        // The Decky plugins this replaces, should something have brought them
        // back (a hand install, an old home): two backends would drive the
        // same settings, and Quick Access would show both.
        std::string plugins = desktop_user().home + "/homebrew/plugins";
        bool gone = false;
        for (const char* name : {"pbos-utils", "dual-screen"}) {
            if (!is_dir(plugins + "/" + name)) continue;
            remove_tree(plugins + "/" + name);
            log("pbos-qamd: removed the old %s Decky plugin (the PB-OS tab has it)", name);
            gone = true;
        }
        if (gone) run({"systemctl", "try-restart", "plugin_loader.service"});
    }

    // ---------------------------------------------------- Steam starting --
    void watch_steam() {
        // Loop thread. The web helper's log (steamwebhelper.sh writes it as
        // the helper starts) and steam.pid (steam.sh, at Steam's start): a
        // hint that Steam's page will be there soon. Watched only while not
        // connected; Steam writes its logs a lot while it runs.
        if (watching_) return;
        const std::string home = desktop_user().home;
        std::string logs = home + "/.local/share/Steam/logs";
        int a = loop.watch(logs, IN_MODIFY | IN_CREATE | IN_CLOSE_WRITE, [this](uint32_t, const std::string& n) {
            if (n.find("webhelper") != std::string::npos || n.find("cef") != std::string::npos) kick();
        });
        int b = loop.watch(home + "/.steam", IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE, [this](uint32_t, const std::string& n) {
            if (n == "steam.pid") kick();
        });
        // Before Steam's first start its logs folder is not there yet.
        int c = loop.watch(home + "/.local/share/Steam", IN_CREATE, [this](uint32_t, const std::string& n) {
            if (n == "logs") {
                unwatch_steam();
                watch_steam();
            }
        });
        watch_ids_ = {a, b, c};
        watching_ = true;
    }

    void unwatch_steam() {
        for (int id : watch_ids_)
            if (id) loop.unwatch(id);
        watch_ids_.clear();
        watching_ = false;
    }

    void kick() {
        uint64_t one = 1;
        ssize_t r = write(kick_, &one, sizeof one);
        (void)r;
    }

    // ----------------------------------------------------- the CDP thread --
    bool wait_stop(double seconds) {
        pollfd p = {stop_, POLLIN, 0};
        return poll(&p, 1, static_cast<int>(seconds * 1000)) > 0;
    }

    void cdp_thread() {
        while (!stopping_) {
            pollfd p[2] = {{kick_, POLLIN, 0}, {stop_, POLLIN, 0}};
            if (poll(p, 2, -1) < 0) continue;
            if (stopping_ || (p[1].revents & POLLIN)) break;
            uint64_t n;
            while (read(kick_, &n, sizeof n) > 0) {}
            double until = now_s() + kStartWindow;
            double backoff = 0.25;
            while (!stopping_ && now_s() < until) {
                auto page = find_gamepadui_page(1.0);
                if (page && ws_.connect(kCefPort, page->ws_path, 3)) {
                    session(*page);
                    if (stopping_) break;
                    // Steam's web helper restarts after a crash; Steam itself
                    // may be quitting. Either way: look again for a while.
                    until = now_s() + kStartWindow;
                    backoff = 0.25;
                    continue;
                }
                if (wait_stop(backoff)) break;
                backoff = std::min(backoff * 2, 4.0);
            }
        }
    }

    void inject() { ws_.send_text(evaluate(next_id(), script_)); }

    void session(const CefPage& page) {
        log("pbos-qamd: connected to Steam's %s", page.title.c_str());
        connected_ = true;
        loop.post([this] { unwatch_steam(); });
        ws_.send_text(command(next_id(), "Page.enable"));
        pj::Value binding = pj::Value::object();
        binding["name"] = kBinding;
        ws_.send_text(command(next_id(), "Runtime.addBinding", binding));
        // A frontend left from before (pbos-qamd restarted): its calls in
        // flight will not be answered now.
        ws_.send_text(evaluate(next_id(), "window.__pbosQam&&window.__pbosQam._reconnected()"));
        inject();
        // Whether the binding reaches us without the Runtime domain on (it
        // keeps Steam's console chatter off this socket); else it is turned on.
        auto probe = [this] {
            ws_.send_text(evaluate(next_id(), std::string("typeof window.") + kBinding + "==='function'&&window." +
                                                  kBinding + "('{\"id\":0,\"s\":\"_\",\"m\":\"ping\"}')"));
        };
        probe();
        double probed_at = now_s();
        bool binding_heard = false, runtime_on = false;
        std::string msg;
        while (!stopping_) {
            auto r = ws_.read(msg, 1000, stop_);
            if (r == WebSocket::Read::Closed || r == WebSocket::Read::Woken) break;
            if (r == WebSocket::Read::Timeout) {
                if (!binding_heard && !runtime_on && now_s() - probed_at > 3) {
                    log("pbos-qamd: the binding needs the Runtime domain here; turning it on");
                    ws_.send_text(command(next_id(), "Runtime.enable"));
                    runtime_on = true;
                    probe();
                }
                continue;
            }
            // Most of what comes is answers to evaluate (and with the Runtime
            // domain on, Steam's console); only these matter.
            bool call = msg.find("\"Runtime.bindingCalled\"") != std::string::npos;
            bool page_event = msg.find("\"Page.domContentEventFired\"") != std::string::npos;
            bool detached = msg.find("\"Inspector.detached\"") != std::string::npos;
            if (!call && !page_event && !detached) continue;
            auto v = pj::Value::parse(msg);
            if (!v) continue;
            const std::string& method = (*v)["method"].as_string();
            if (method == "Runtime.bindingCalled" && (*v)["params"]["name"].as_string() == kBinding) {
                binding_heard = true;
                handle((*v)["params"]["payload"].as_string());
            } else if (method == "Page.domContentEventFired") {
                // Steam's page loaded again (a Steam UI restart): the tab
                // goes back in.
                log("pbos-qamd: Steam's page reloaded");
                pj::Value b = pj::Value::object();
                b["name"] = kBinding;
                ws_.send_text(command(next_id(), "Runtime.addBinding", b));
                inject();
            } else if (method == "Inspector.detached") {
                log("pbos-qamd: Steam detached (%s)", (*v)["params"]["reason"].str().c_str());
                break;
            }
        }
        ws_.close();
        connected_ = false;
        log("pbos-qamd: Steam's page went away");
        loop.post([this] { watch_steam(); });
    }

    void reply(long long id, bool ok, const pj::Value& value) {
        if (!connected_) return;
        std::string expr = "window.__pbosQam&&window.__pbosQam._reply(" + std::to_string(id) + "," +
                           (ok ? "true" : "false") + "," + value.dump() + ")";
        ws_.send_text(evaluate(next_id(), expr));
    }

    void handle(const std::string& payload) {
        auto v = pj::Value::parse(payload);
        if (!v || !v->is_object()) return;
        long long id = (*v)["id"].as_int(0);
        std::string section = (*v)["s"].str();
        std::string method = (*v)["m"].str();
        pj::Value args = (*v)["a"].is_array() ? (*v)["a"] : pj::Value::array();
        if (section == "_") {
            // The loader's own: its log lines (frontend errors) to the journal.
            if (method == "log") log("frontend: %s", args.at(0).str().c_str());
            if (id) reply(id, true, pj::Value());
            return;
        }
        Section* target = nullptr;
        for (auto& s : sections_)
            if (s->name() == section) target = s.get();
        if (!target) {
            reply(id, false, pj::Value("no section " + section));
            return;
        }
        Loop::work([this, target, id, method, args] {
            try {
                reply(id, true, target->call(method, args));
            } catch (const std::exception& e) {
                log("pbos-qamd: %s.%s: %s", target->name().c_str(), method.c_str(), e.what());
                reply(id, false, pj::Value(std::string(e.what())));
            }
        });
    }

    std::vector<std::unique_ptr<Section>> sections_;
    std::string script_;
    std::thread cdp_;
    int kick_ = -1, stop_ = -1;
    std::atomic<bool> stopping_{false}, connected_{false};
    std::atomic<int> msg_id_{0};
    WebSocket ws_;
    bool watching_ = false;
    std::vector<int> watch_ids_;
};

// --check: the Steam-internals probe, once, in the running Steam.
int check(bool as_json) {
    auto page = find_gamepadui_page(3);
    if (!page) {
        std::fprintf(stderr, "Steam's Game Mode page is not up (is Steam running in Game Mode?)\n");
        return 2;
    }
    WebSocket ws;
    if (!ws.connect(kCefPort, page->ws_path, 5)) {
        std::fprintf(stderr, "cannot connect to Steam's page\n");
        return 2;
    }
    // The probe runs in the PB-OS tab pbos-qamd loaded, with the tab's own
    // @decky/ui; nothing new is loaded into Steam's page.
    ws.send_text(evaluate(2,
                          "window.__pbosQam && window.__pbosQam.probe ? window.__pbosQam.probe() : "
                          "({ok: false, steam: '', items: [{name: 'PB-OS tab loaded', ok: false, "
                          "detail: 'pbos-qamd has not loaded the tab in this Steam (is pbos-qamd running?)'}]})",
                          true));
    std::string msg;
    double until = now_s() + 60;
    while (now_s() < until) {
        auto r = ws.read(msg, 1000, -1);
        if (r == WebSocket::Read::Closed) break;
        if (r != WebSocket::Read::Message) continue;
        auto v = pj::Value::parse(msg);
        if (!v || (*v)["id"].as_int() != 2) continue;
        const pj::Value& result = (*v)["result"]["result"]["value"];
        if (!result.is_object()) {
            std::fprintf(stderr, "the probe did not answer: %s\n", msg.substr(0, 400).c_str());
            return 2;
        }
        if (as_json) {
            std::printf("%s\n", result.dump(2).c_str());
        } else {
            std::printf("Steam: %s\n", result["steam"].str().c_str());
            for (const auto& item : result["items"].items())
                std::printf("%s  %-28s %s\n", item["ok"].truthy() ? "PASS" : "FAIL", item["name"].str().c_str(),
                            item["detail"].str().c_str());
        }
        return result["ok"].truthy() ? 0 : 1;
    }
    std::fprintf(stderr, "the probe timed out\n");
    return 2;
}

// --call SECTION METHOD [ARGS]: one backend call, without Steam (testing on
// a device); ARGS is the JSON list the frontend would send.
int call(const std::string& section, const std::string& method, const std::string& args_json) {
    auto args = pj::Value::parse(args_json.empty() ? "[]" : args_json);
    if (!args || !args->is_array()) {
        std::fprintf(stderr, "ARGS must be a JSON list\n");
        return 2;
    }
    for (auto make : {make_tools_section, make_barry_section, make_files_section, make_store_section, make_notices_section}) {
        auto s = make();
        if (s->name() != section) continue;
        try {
            std::printf("%s\n", s->call(method, *args).dump(2).c_str());
            return 0;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            return 1;
        }
    }
    std::fprintf(stderr, "no section %s (tools, barry, files)\n", section.c_str());
    return 2;
}

}  // namespace

}  // namespace pq

int main(int argc, char** argv) {
    if (argc > 3 && !std::strcmp(argv[1], "--call")) return pq::call(argv[2], argv[3], argc > 4 ? argv[4] : "");
    if (argc > 1 && (!std::strcmp(argv[1], "--check") || !std::strcmp(argv[1], "check"))) {
        bool as_json = argc > 2 && !std::strcmp(argv[2], "--json");
        return pq::check(as_json);
    }
    if (argc > 1 && !std::strcmp(argv[1], "--lights")) return pq::run_lights();
    if (argc > 1 && !std::strcmp(argv[1], "--version")) {
        std::printf("pbos-qamd %s\n", pq::kVersion);
        return 0;
    }
    pq::Loop loop;
    pq::Daemon daemon(loop);
    loop.on_signal([&](int sig) {
        pq::log("pbos-qamd: signal %d, stopping", sig);
        loop.quit();
    });
    if (!daemon.start()) return 1;
    loop.run();
    daemon.stop();
    return 0;
}

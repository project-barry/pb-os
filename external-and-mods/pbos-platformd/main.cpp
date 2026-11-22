// pbos-platformd: PB-OS's platform daemons (fan, charging, buttons, volume)
// as modules of one event-driven C++ process, replacing a Python daemon per
// job. Each module listens to what changes (uevents, evdev, inotify, the
// sound server's events) rather than polling.
//
// pbos-platformd --system            root's modules for this device
// pbos-platformd --user              the session's modules
// pbos-platformd --only NAME[,NAME]  just these (still checked against the device)
// pbos-platformd --list              which modules would run here
// pbos-platformd charge-rest|konkr-standby|led-fade ARGS
//                                    the sleep path's one-shot programs

#include <signal.h>

#include <cstring>
#include <set>
#include <sstream>

#include "module.h"
#include "util.h"

namespace pp {

bool dt_has(const char* s) { return pq::dt_compatible().find(s) != std::string::npos; }

const std::vector<ModuleInfo>& modules() {
    static const std::vector<ModuleInfo> all = {
        // The Retroid Pocket Duo (SM7750, qcom,eliza) has the same pwm-fan and
        // pmic power key as the SM8550 handhelds.
        {"fan", Instance::System, [] { return dt_has("qcom,sm8550") || dt_has("retroidpocket,rpduo"); }, make_fan},
        {"power-button", Instance::System, [] { return dt_has("qcom,sm8550") || dt_has("retroidpocket,rpduo"); },
         make_power_button},
        {"charge-control", Instance::System, charge_control_here, make_charge_control},
        {"pbosd", Instance::System, pbosd_here, make_pbosd},
        {"thor-backlight", Instance::System, thor_backlight_here, make_thor_backlight},
        {"thor-controls", Instance::System, [] { return dt_has("ayn,thor") || dt_has("retroidpocket,rpduo"); },
         make_thor_controls},
        // Every device: memory pressure is the same everywhere.
        {"memguard", Instance::System, [] { return true; }, make_memguard},
        {"volume-keeper", Instance::User, [] { return dt_has("qcom,sm8550"); }, make_volume_keeper},
        // Every image (the Python ran everywhere); it looks for its keys itself.
        {"volume-keys", Instance::User, [] { return true; }, make_volume_keys},
        {"konkr-volume", Instance::User, konkr_volume_here, make_konkr_volume},
        {"audio-dp", Instance::User, [] { return dt_has("qcom,sm8550"); }, make_audio_dp},
        {"barry-audio", Instance::User, [] { return dt_has("ayn,thor"); }, make_barry_audio},
        {"thor-desktop-brightness", Instance::User, [] { return dt_has("ayn,thor"); }, make_thor_desktop_brightness},
    };
    return all;
}

}  // namespace pp

int main(int argc, char** argv) {
    using namespace pp;
    // pbos-platformd TOOL ARGS...: argv[1] becomes the tool's argv[0].
    static const std::pair<const char*, int (*)(int, char**)> tools[] = {
        {"charge-rest", charge_rest_main}, {"konkr-standby", konkr_standby_main}, {"led-fade", led_fade_main}};
    if (argc > 1)
        for (auto& [name, fn] : tools)
            if (!std::strcmp(argv[1], name)) return fn(argc - 1, argv + 1);
    Instance inst = Instance::System;
    bool list = false;
    std::set<std::string> only;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--user")) inst = Instance::User;
        else if (!std::strcmp(argv[i], "--system")) inst = Instance::System;
        else if (!std::strcmp(argv[i], "--list")) list = true;
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            for (std::string n; std::getline(ss, n, ',');) only.insert(n);
        } else {
            std::fprintf(stderr, "usage: pbos-platformd [--system|--user] [--only NAME,...] [--list]\n");
            return 2;
        }
    }
    pq::Loop loop;  // blocks the signals it handles, for every thread made after
    std::vector<std::unique_ptr<Module>> running;
    for (const auto& m : modules()) {
        if (m.instance != inst || (!only.empty() && !only.count(m.name)) || !m.here()) continue;
        if (list) {
            std::printf("%s\n", m.name);
            continue;
        }
        running.push_back(m.make());
    }
    if (list) return 0;
    if (running.empty()) {
        pq::log("pbos-platformd: no modules for this device");
        return 0;
    }
    loop.on_signal([&](int sig) {
        if (sig == SIGHUP) {
            for (auto& m : running) m->reload();
            return;
        }
        if (sig == SIGUSR1 || sig == SIGUSR2) {
            for (auto& m : running) m->user_signal(sig);
            return;
        }
        loop.quit();
    });
    for (auto& m : running) {
        pq::log("pbos-platformd: %s", m->name());
        m->start(loop);
    }
    loop.run();
    for (auto it = running.rbegin(); it != running.rend(); ++it) (*it)->stop();
    return 0;
}

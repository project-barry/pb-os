// pbos-platformd: PB-OS's per-device platform daemons in one process, one
// module each, picked by the device tree. Two instances: --system (root)
// and --user (the session's: PipeWire, the session bus).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "loop.h"

namespace pp {

class Module {
public:
    virtual ~Module() = default;
    virtual const char* name() const = 0;
    // On the loop thread, once.
    virtual void start(pq::Loop& loop) = 0;
    // On the loop thread, at shutdown (put the hardware's safe state back).
    virtual void stop() {}
    // SIGHUP: read the settings again.
    virtual void reload() {}
    // SIGUSR1/SIGUSR2: the module's own meaning (default: ignored).
    virtual void user_signal(int /*sig*/) {}
};

enum class Instance { System, User };

struct ModuleInfo {
    const char* name;
    Instance instance;
    // Whether this device has it (the device tree's compatible and model).
    std::function<bool()> here;
    std::function<std::unique_ptr<Module>()> make;
};

// Every module, in start order.
const std::vector<ModuleInfo>& modules();

// The device tree's compatible strings contain s.
bool dt_has(const char* s);

// Each module's factory (one source file per module).
std::unique_ptr<Module> make_volume_keeper();
bool charge_control_here();
std::unique_ptr<Module> make_charge_control();
std::unique_ptr<Module> make_fan();
// The device's own fan file read over fan.conf (fan.cpp's DEVICE_CONFS: the
// Retroid Pocket Duo's), "" when it has none; thor-controls reads it too.
std::string fan_device_conf();
std::unique_ptr<Module> make_power_button();
std::unique_ptr<Module> make_volume_keys();
bool konkr_volume_here();
std::unique_ptr<Module> make_konkr_volume();
std::unique_ptr<Module> make_audio_dp();
// One-shot tools run by name (pbos-platformd TOOL ARGS...): the sleep
// path's programs, each its own process as before.
int charge_rest_main(int argc, char** argv);    // was sm8550-charge-rest
int konkr_standby_main(int argc, char** argv);  // was konkr-standby
int led_fade_main(int argc, char** argv);       // was sm8550-sleep's python fade

bool pbosd_here();
std::unique_ptr<Module> make_pbosd();
bool thor_backlight_here();
std::unique_ptr<Module> make_thor_backlight();
std::unique_ptr<Module> make_barry_audio();
std::unique_ptr<Module> make_thor_controls();
std::unique_ptr<Module> make_thor_desktop_brightness();
std::unique_ptr<Module> make_memguard();

}  // namespace pp

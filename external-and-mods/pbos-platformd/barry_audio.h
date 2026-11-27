// barry-audio's parts that need no sound server: the bottom screen's apps
// (from /proc), their volume (quick.json) and the volumes pactl reads and
// writes. Apart from the module so the tests can call them.
#pragma once

#include <pulse/volume.h>

#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "json.h"

namespace pp::barry_audio {

// (process id, program) for every process under Barry Launcher's gamescope,
// by its id outside its sandbox and inside it (barry_quick.bottom_apps,
// looked up from outside).
using Apps = std::set<std::pair<long long, std::string>>;
Apps bottom_apps(const std::string& proc = "/proc");

// A stream (its application.process.id and application.process.binary) is
// one of theirs (barry_quick.is_app_stream).
bool is_app_stream(const char* process_id, const char* process_binary, const Apps& apps);

// The bottom screen's apps' volume, 0-100 (100 when never set): quick.json's
// appsVolume (barry_quick.apps_volume).
int apps_volume(const std::string& path);

// The bottom screen's volume is the top one's: settings.json's volumeLinked
// (false when never set).
bool volume_linked(const std::string& path);

// A real output's port: its card (-1 none), whether it is a headphone jack,
// and what the jack says.
struct Port {
    std::string output;
    long long card = -1;
    bool headphones = false;
    enum { Unknown, No, Yes } plugged = Unknown;
};

// The output to use in place of real: its card's headphones when plugged in,
// its card's other output when real is headphones unplugged; "" to stay.
std::string jack_choice(const std::vector<Port>& ports, const std::string& real);

// pactl's value_percent of one channel (rounded), and the volume its "N%"
// sets (cut).
int percent(pa_volume_t v);
std::optional<int> percent(const pa_cvolume& v);
pa_volume_t from_percent(int pct);

// Python's json.dump(): ", " and ": " between, non-ASCII as \u escapes.
std::string py_json(const pj::Value& v);

}  // namespace pp::barry_audio

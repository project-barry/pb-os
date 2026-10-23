// barry-audio (was barry_launcher_audio): the AYN Thor's two volumes, one
// per screen.
//
// PipeWire has a "Top screen" output (60-pbos-top-screen.conf), a loopback
// into the real output. This keeps:
//   - "Top screen" the default output, so Steam, games, Steam's volume bar
//     and the volume keys use it: its volume is the top screen's;
//   - its loopback on the real output (the speakers, HDMI or Bluetooth),
//     which stays at 100 %;
//   - each bottom-screen app's stream straight on the real output, at the
//     bottom screen's volume (quick.json's appsVolume) as its own volume.
//     Neither volume moves the other.
// When the default output becomes a real one (sm8550-audio-pipewire on an
// HDMI or DisplayPort change, an output picked in Steam, Bluetooth
// connecting), that one is the real output from then on: the top volume
// becomes the one it had there last (or that output's own, the first time),
// it goes to 100 % and "Top screen" is the default again. Streams moved onto
// the real output that are not the bottom apps' go back to "Top screen".
//
// The sound server's events (outputs, streams, the default output) and
// SIGUSR1 (barry_quick, when the bottom volume moves) start a look at
// everything 150 ms later, as the Python did. A stream is checked against
// the bottom screen's processes once, when it first shows up; the Python
// walked /proc and ran pactl 3-10 times on every event. Without the "Top
// screen" output (the config is not linked) it does nothing.

#include "barry_audio.h"

#include <dirent.h>
#include <fcntl.h>
#include <pulse/pulseaudio.h>
#include <signal.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include "module.h"
#include "util.h"

namespace pp {

namespace barry_audio {

namespace {

// gamescope's process name ("gamescope-wl" since it names its threads).
bool is_gamescope(const std::string& comm) { return comm == "gamescope" || comm == "gamescope-wl"; }

std::string comm_of(const std::string& proc, long long pid) {
    auto s = pq::read_file(proc + "/" + std::to_string(pid) + "/comm");
    return s ? pq::trim(*s) : std::string();
}

// pid -> parent pid, for every process we can see, by pid.
std::map<long long, long long> proc_tree(const std::string& proc) {
    std::map<long long, long long> out;
    DIR* d = opendir(proc.c_str());
    if (!d) return out;
    while (dirent* e = readdir(d)) {
        if (!pq::is_digits(e->d_name)) continue;
        auto stat = pq::read_file(proc + "/" + e->d_name + "/stat");
        if (!stat) continue;
        size_t close_paren = stat->rfind(')');
        if (close_paren == std::string::npos) continue;
        // After the name: state, then the parent's id.
        const char* p = stat->c_str() + close_paren + 1;
        char state[8];
        long long ppid;
        if (std::sscanf(p, " %7s %lld", state, &ppid) != 2) continue;
        out[std::atoll(e->d_name)] = ppid;
    }
    closedir(d);
    return out;
}

// The process's id in its own pid namespace (a sandbox's), if other.
std::optional<long long> ns_pid(const std::string& proc, long long pid) {
    auto s = pq::read_file(proc + "/" + std::to_string(pid) + "/status");
    if (!s) return std::nullopt;
    for (const auto& line : pq::split_lines(*s)) {
        if (line.rfind("NSpid:", 0) != 0) continue;
        std::vector<long long> ids;
        const char* p = line.c_str() + 6;
        char* end;
        while (true) {
            long long v = std::strtoll(p, &end, 10);
            if (end == p) break;
            ids.push_back(v);
            p = end;
        }
        if (ids.size() > 1) return ids.back();
        return std::nullopt;
    }
    return std::nullopt;
}

// The gamescope pid runs in (pid itself or an ancestor).
std::optional<long long> gamescope_of(const std::string& proc, const std::map<long long, long long>& tree,
                                      long long pid) {
    while (pid > 1) {
        if (is_gamescope(comm_of(proc, pid))) return pid;
        auto it = tree.find(pid);
        pid = it == tree.end() ? 1 : it->second;
    }
    return std::nullopt;
}

// Barry Launcher's gamescope: the one barry_launcher_session runs in (the
// first such process by id, as /proc lists them).
std::optional<long long> barry_gamescope(const std::string& proc, const std::map<long long, long long>& tree) {
    for (const auto& [pid, ppid] : tree) {
        auto cmdline = pq::read_file(proc + "/" + std::to_string(pid) + "/cmdline");
        if (!cmdline || cmdline->find("/barry_launcher_session") == std::string::npos) continue;
        if (auto root = gamescope_of(proc, tree, pid)) return root;
    }
    return std::nullopt;
}

}  // namespace

Apps bottom_apps(const std::string& proc) {
    Apps out;
    auto tree = proc_tree(proc);
    auto root = barry_gamescope(proc, tree);
    if (!root) return out;
    std::map<long long, std::vector<long long>> children;
    for (const auto& [pid, ppid] : tree) children[ppid].push_back(pid);
    std::vector<long long> todo = children[*root];
    while (!todo.empty()) {
        long long pid = todo.back();
        todo.pop_back();
        auto it = children.find(pid);
        if (it != children.end()) todo.insert(todo.end(), it->second.begin(), it->second.end());
        std::string name = comm_of(proc, pid);
        out.emplace(pid, name);
        if (auto inner = ns_pid(proc, pid)) out.emplace(*inner, name);
    }
    return out;
}

bool is_app_stream(const char* process_id, const char* process_binary, const Apps& apps) {
    // Python's int(): spaces around, a sign, digits.
    std::string id = pq::trim(process_id ? process_id : "");
    size_t digits = id.size() && (id[0] == '+' || id[0] == '-') ? 1 : 0;
    if (!pq::is_digits(std::string_view(id).substr(digits)) || id.size() > 18) return false;
    long long pid = std::atoll(id.c_str());
    // /proc's comm is the program's name cut to 15 characters.
    std::string program = pq::basename(process_binary ? process_binary : "").substr(0, 15);
    if (apps.count({pid, program})) return true;
    if (!program.empty()) return false;
    for (const auto& a : apps)
        if (a.first == pid) return true;
    return false;
}

int apps_volume(const std::string& path) {
    pj::Value data = pq::read_json(path);
    const pj::Value* v = data.find("appsVolume");
    if (!v) return 100;
    // Python's bool is an int (true: 1).
    double d = v->is_bool() ? (v->as_bool() ? 1 : 0) : v->is_number() ? v->as_double() : -1;
    return d >= 0 && d <= 100 ? static_cast<int>(d) : 100;
}

int percent(pa_volume_t v) {
    return static_cast<int>((static_cast<uint64_t>(v) * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
}

std::optional<int> percent(const pa_cvolume& v) {
    if (v.channels == 0 || v.channels > PA_CHANNELS_MAX) return std::nullopt;
    int most = 0;
    for (unsigned c = 0; c < v.channels; ++c) most = std::max(most, percent(v.values[c]));
    return most;
}

pa_volume_t from_percent(int pct) {
    return static_cast<pa_volume_t>(static_cast<double>(pct) * PA_VOLUME_NORM / 100);
}

namespace {

void py_quote(std::string& out, std::string_view s) {
    out += '"';
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0x80) {
            len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
            if (i + len > s.size()) len = 1;
            cp = len == 4 ? c & 0x07 : len == 3 ? c & 0x0f : c & 0x1f;
            for (size_t k = 1; k < len; ++k) cp = cp << 6 | (s[i + k] & 0x3f);
        }
        i += len;
        char buf[16];
        switch (cp) {
        case '"': out += "\\\""; continue;
        case '\\': out += "\\\\"; continue;
        case '\n': out += "\\n"; continue;
        case '\r': out += "\\r"; continue;
        case '\t': out += "\\t"; continue;
        case '\b': out += "\\b"; continue;
        case '\f': out += "\\f"; continue;
        default: break;
        }
        if (cp < 0x20 || (cp >= 0x7f && cp < 0x10000)) {
            std::snprintf(buf, sizeof buf, "\\u%04x", cp);
            out += cp == 0x7f ? std::string(1, '\x7f') : buf;
        } else if (cp >= 0x10000) {
            cp -= 0x10000;
            std::snprintf(buf, sizeof buf, "\\u%04x\\u%04x", 0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff));
            out += buf;
        } else {
            out += static_cast<char>(cp);
        }
    }
    out += '"';
}

void py_dump(std::string& out, const pj::Value& v) {
    if (v.is_object()) {
        out += '{';
        bool first = true;
        for (const auto& [k, val] : v.members()) {
            if (!first) out += ", ";
            first = false;
            py_quote(out, k);
            out += ": ";
            py_dump(out, val);
        }
        out += '}';
    } else if (v.is_array()) {
        out += '[';
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) out += ", ";
            py_dump(out, v.at(i));
        }
        out += ']';
    } else if (v.is_string()) {
        py_quote(out, v.as_string());
    } else {
        out += v.dump();
    }
}

}  // namespace

std::string py_json(const pj::Value& v) {
    std::string out;
    py_dump(out, v);
    return out;
}

}  // namespace barry_audio

namespace {

using namespace barry_audio;
using pj::Value;

constexpr const char* TOP = "pbos_top_screen";
constexpr const char* TOP_OUT = "pbos_top_screen_out";
// Events come in bursts (our own changes make more); act once they settle.
constexpr pa_usec_t SETTLE_US = 150 * 1000;
constexpr pa_usec_t RECONNECT_US = 3 * 1000 * 1000;

std::string home() {
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

std::string xdg(const char* var, const char* under_home) {
    const char* x = std::getenv(var);
    return x && *x ? x : home() + under_home;
}

// Written and flushed before it replaces the old one.
void save_state(const std::string& path, const Value& data) {
    pq::makedirs(pq::dirname(path));
    std::string tmp = path + ".tmp";
    std::string text = py_json(data);
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        pq::log("cannot write %s", tmp.c_str());
        return;
    }
    bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) pq::log("cannot save %s", path.c_str());
}

struct Sink {
    uint32_t index;
    std::string name;
    bool real;  // a hardware output (ALSA, Bluetooth), not a virtual one
    std::optional<int> pct;
    bool mute;
    uint8_t channels;
};

struct Input {
    uint32_t index;
    uint32_t sink;
    std::string node_name;
    bool link_group;
    std::string pid, binary;
    std::optional<int> pct;
    uint8_t channels;
};

class BarryAudio : public Module {
public:
    const char* name() const override { return "barry-audio"; }

    void start(pq::Loop&) override {
        path_ = xdg("XDG_STATE_HOME", "/.local/state") + "/barry_launcher_audio.json";
        apps_path_ = xdg("XDG_CONFIG_HOME", "/.config") + "/barry_launcher/quick.json";
        state_ = pq::read_json(path_);
        if (!state_["top"].is_object()) state_["top"] = Value::object();  // real output -> top volume there
        if (const Value* r = state_.find("real"); r && r->is_string()) real_ = r->as_string();
        ml_ = pa_threaded_mainloop_new();
        api_ = pa_threaded_mainloop_get_api(ml_);
        pa_threaded_mainloop_lock(ml_);
        connect();
        pa_threaded_mainloop_unlock(ml_);
        pa_threaded_mainloop_start(ml_);
    }

    void stop() override {
        if (!ml_) return;
        pa_threaded_mainloop_stop(ml_);
        if (ctx_) {
            pa_context_set_state_callback(ctx_, nullptr, nullptr);
            pa_context_set_subscribe_callback(ctx_, nullptr, nullptr);
            pa_context_disconnect(ctx_);
            pa_context_unref(ctx_);
        }
        pa_threaded_mainloop_free(ml_);
        ml_ = nullptr;
    }

    // barry_quick: the bottom volume moved.
    void user_signal(int sig) override {
        if (sig != SIGUSR1 || !ml_) return;
        pa_threaded_mainloop_lock(ml_);
        wake();
        pa_threaded_mainloop_unlock(ml_);
    }

private:
    // The mainloop thread from here on (pulse's callbacks hold its lock).

    void connect() {
        ctx_ = pa_context_new(api_, "pbos-platformd barry-audio");
        pa_context_set_state_callback(ctx_, &BarryAudio::on_state, this);
        pa_context_set_subscribe_callback(ctx_, &BarryAudio::on_event, this);
        if (pa_context_connect(ctx_, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) retry();
    }

    void retry() {
        if (ctx_) {
            pa_context_set_state_callback(ctx_, nullptr, nullptr);
            pa_context_set_subscribe_callback(ctx_, nullptr, nullptr);
            pa_context_disconnect(ctx_);
            pa_context_unref(ctx_);
            ctx_ = nullptr;
        }
        if (settle_) api_->time_free(settle_);
        settle_ = nullptr;
        phase_ = Phase::Idle;
        asked_ = pending_ = 0;  // their answers never come
        apps_of_.clear();  // stream ids start over with the server
        ++generation_;     // answers still on their way are dropped
        api_->time_new(api_, in(RECONNECT_US),
                       [](pa_mainloop_api* a, pa_time_event* e, const timeval*, void* self) {
                           a->time_free(e);
                           static_cast<BarryAudio*>(self)->connect();
                       },
                       this);
    }

    static void on_state(pa_context* c, void* p) {
        auto* self = static_cast<BarryAudio*>(p);
        switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            self->op(pa_context_subscribe(c,
                                          static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK |
                                                                              PA_SUBSCRIPTION_MASK_SINK_INPUT |
                                                                              PA_SUBSCRIPTION_MASK_SERVER),
                                          nullptr, nullptr));
            self->wake();  // (re)connected: look at everything once
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            self->retry();  // PipeWire restarted, or not up yet
            break;
        default:
            break;
        }
    }

    static void on_event(pa_context*, pa_subscription_event_type_t t, uint32_t idx, void* p) {
        auto* self = static_cast<BarryAudio*>(p);
        auto facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
        if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT &&
            (t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE)
            self->apps_of_.erase(idx);
        // Output, stream and default-output changes (the mask has no others).
        self->wake();
    }

    // As the Python: the first event starts the 150 ms wait, those during it
    // are part of it, those while looking start another look after it.
    void wake() {
        if (phase_ == Phase::Idle) {
            phase_ = Phase::Waiting;
            settle_ = api_->time_new(api_, in(SETTLE_US),
                                     [](pa_mainloop_api* a, pa_time_event* e, const timeval*, void* self) {
                                         auto* b = static_cast<BarryAudio*>(self);
                                         a->time_free(e);
                                         b->settle_ = nullptr;
                                         b->look();
                                     },
                                     this);
        } else if (phase_ == Phase::Running) {
            again_ = true;
        }
    }

    void look() {
        if (!ctx_ || pa_context_get_state(ctx_) != PA_CONTEXT_READY) {
            phase_ = Phase::Idle;
            return;
        }
        phase_ = Phase::Running;
        again_ = false;
        sinks_.clear();
        inputs_.clear();
        default_.clear();
        asked_ = 3;
        gen_ = generation_;
        ask(pa_context_get_server_info(
            ctx_,
            [](pa_context*, const pa_server_info* i, void* p) {
                auto* self = static_cast<BarryAudio*>(p);
                if (i && i->default_sink_name) self->default_ = i->default_sink_name;
                self->answered();
            },
            this));
        ask(pa_context_get_sink_info_list(
            ctx_,
            [](pa_context*, const pa_sink_info* i, int eol, void* p) {
                auto* self = static_cast<BarryAudio*>(p);
                if (eol) return self->answered();
                const char* api = pa_proplist_gets(i->proplist, "device.api");
                self->sinks_.push_back({i->index, i->name ? i->name : "", api && *api, percent(i->volume),
                                        static_cast<bool>(i->mute), i->volume.channels});
            },
            this));
        ask(pa_context_get_sink_input_info_list(
            ctx_,
            [](pa_context*, const pa_sink_input_info* i, int eol, void* p) {
                auto* self = static_cast<BarryAudio*>(p);
                if (eol) return self->answered();
                auto get = [&](const char* k) {
                    const char* v = pa_proplist_gets(i->proplist, k);
                    return std::string(v ? v : "");
                };
                self->inputs_.push_back({i->index, i->sink, get("node.name"), !get("node.link-group").empty(),
                                         get("application.process.id"), get("application.process.binary"),
                                         percent(i->volume), i->volume.channels});
            },
            this));
        if (asked_ == 0) finish();  // the connection went meanwhile
    }

    void ask(pa_operation* o) {
        if (o)
            pa_operation_unref(o);
        else
            --asked_;
    }

    void answered() {
        if (gen_ != generation_) return;
        if (--asked_ == 0) {
            reconcile();
            if (pending_ == 0) finish();
        }
    }

    void done_one() {
        if (pending_ > 0 && --pending_ == 0 && phase_ == Phase::Running && asked_ == 0) finish();
    }

    void finish() {
        phase_ = Phase::Idle;
        if (again_) wake();
    }

    // An operation, counted until the server answers when it is one of ours.
    void op(pa_operation* o) {
        if (o) pa_operation_unref(o);
    }
    static void on_done(pa_context*, int, void* p) { static_cast<BarryAudio*>(p)->done_one(); }
    void act(pa_operation* o) {
        if (!o) return;  // refused at once (e.g. an invalid volume), as pactl failing
        ++pending_;
        pa_operation_unref(o);
    }

    void set_volume(bool sink, uint32_t idx, const std::string& name, uint8_t channels, int pct) {
        pa_cvolume cv;
        pa_cvolume_set(&cv, channels, from_percent(pct));
        if (sink)
            act(pa_context_set_sink_volume_by_name(ctx_, name.c_str(), &cv, on_done, this));
        else
            act(pa_context_set_sink_input_volume(ctx_, idx, &cv, on_done, this));
    }
    void move(uint32_t input, const std::string& sink) {
        act(pa_context_move_sink_input_by_name(ctx_, input, sink.c_str(), on_done, this));
    }

    void remember(const std::string& real, std::optional<int> top_pct) {
        bool changed = false;
        Value& top = state_["top"];
        if (top_pct) {
            const Value* had = top.find(real);
            if (!had || !had->is_number() || had->as_double() != *top_pct) {
                top[real] = *top_pct;
                changed = true;
            }
        }
        const Value* r = state_.find("real");
        if (!r || !r->is_string() || r->as_string() != real) {
            state_["real"] = real;
            changed = true;
        }
        if (changed) save_state(path_, state_);
    }

    const Sink* sink_named(const std::string& name) const {
        for (const auto& s : sinks_)
            if (s.name == name) return &s;
        return nullptr;
    }
    std::string sink_at(uint32_t index) const {
        for (const auto& s : sinks_)
            if (s.index == index) return s.name;
        return {};
    }
    bool is_real(const std::string& name) const {
        const Sink* s = sink_named(name);
        return s && s->real;
    }

    void reconcile() {
        if (gen_ != generation_) return;
        const Sink* top = sink_named(TOP);
        if (!top) {
            if (!missing_logged_) {
                pq::log("no %s output: nothing to do", TOP);
                missing_logged_ = true;
            }
            return;
        }
        missing_logged_ = false;
        const Input* out = nullptr;
        for (const auto& i : inputs_)
            if (i.node_name == TOP_OUT) {
                out = &i;
                break;
            }
        std::optional<int> top_pct = top->pct;
        uint8_t top_channels = top->channels;
        // One line when the loopback's output changes or its stream goes: a
        // "Top screen" with nowhere to play is silent (problem reports).
        std::string out_on = out ? sink_at(out->sink) : "";
        std::string out_state = out ? "on " + out_on : "gone";
        if (out_state != loop_on_) {
            if (out)
                pq::log("%s plays on %s", TOP_OUT, out_on.empty() ? "nothing" : out_on.c_str());
            else
                pq::log("%s stream gone", TOP_OUT);
            loop_on_ = out_state;
        }

        // A real output became the default: it is the real output now.
        if (default_ != TOP && is_real(default_)) {
            std::string fresh = default_;
            if (!real_.empty() && real_ != fresh && sink_named(real_)) remember(real_, top_pct);
            std::optional<int> want;
            if (const Value* had = state_["top"].find(fresh); had && had->is_number())
                want = static_cast<int>(had->as_int());
            if (!want) {
                std::optional<int> own = sink_named(fresh)->pct;
                want = own && *own < 99 ? own : top_pct;
            }
            act(pa_context_set_default_sink(ctx_, TOP, on_done, this));
            if (want && want != top_pct) {
                set_volume(true, 0, TOP, top_channels, *want);
                top_pct = want;
            }
            if (top_pct)
                pq::log("real output %s; top volume %d%%", fresh.c_str(), *top_pct);
            else
                pq::log("real output %s; top volume None%%", fresh.c_str());
            real_ = fresh;
        } else if (!is_real(real_) && out) {
            // The real output went (Bluetooth disconnecting): WirePlumber
            // moved the loopback on to another; that one is it now.
            std::string here = sink_at(out->sink);
            if (is_real(here)) {
                pq::log("real output %s (the loopback's)", here.c_str());
                real_ = here;
            }
        }
        if (!is_real(real_)) {
            if (!real_.empty() && real_ != gone_) pq::log("real output %s gone, none in its place", real_.c_str());
            gone_ = real_;
            return;
        }
        gone_.clear();
        const Sink& real = *sink_named(real_);
        remember(real_, top_pct);

        // The real output: at 100 %, not muted, under the loopback.
        if (real.pct != 100) set_volume(true, 0, real_, real.channels, 100);
        if (real.mute) {
            pq::log("real output %s was muted: unmuted", real_.c_str());
            act(pa_context_set_sink_mute_by_name(ctx_, real_.c_str(), 0, on_done, this));
        }
        if (out && out_on != real_) move(out->index, real_);

        // The streams: the bottom apps' on the real output at their volume;
        // others moved onto it go back to "Top screen". Filters' and
        // loopbacks' own streams (node.link-group) are left alone. Each is
        // checked against the bottom screen's processes when first seen.
        std::map<uint32_t, Seen> seen;
        std::optional<Apps> apps;
        std::optional<int> apps_pct;
        for (const auto& i : inputs_) {
            if (i.link_group || i.node_name == TOP_OUT) continue;
            auto known = apps_of_.find(i.index);
            bool app;
            if (known != apps_of_.end() && known->second.pid == i.pid && known->second.binary == i.binary) {
                app = known->second.app;
            } else {
                if (!apps) apps = bottom_apps();
                app = is_app_stream(i.pid.c_str(), i.binary.c_str(), *apps);
            }
            seen[i.index] = {i.pid, i.binary, app};
            std::string on = sink_at(i.sink);
            if (app) {
                if (on != real_) move(i.index, real_);
                if (!apps_pct) apps_pct = apps_volume(apps_path_);
                if (i.pct != apps_pct) set_volume(false, i.index, "", i.channels, *apps_pct);
            } else if (on == real_) {
                move(i.index, TOP);
            }
        }
        apps_of_ = std::move(seen);
    }

    // A time this far from now, for the mainloop's timers (wall clock).
    const timeval* in(pa_usec_t us) {
        pa_gettimeofday(&tv_);
        pa_timeval_add(&tv_, us);
        return &tv_;
    }

    enum class Phase { Idle, Waiting, Running };
    struct Seen {
        std::string pid, binary;
        bool app;
    };

    std::string path_, apps_path_;
    Value state_;
    std::string real_;
    std::string loop_on_, gone_;  // last logged: the loopback ("on <output>", "gone"), a real output gone
    bool missing_logged_ = false;
    Phase phase_ = Phase::Idle;
    bool again_ = false;
    int asked_ = 0, pending_ = 0;
    uint64_t generation_ = 0, gen_ = 0;
    std::vector<Sink> sinks_;
    std::vector<Input> inputs_;
    std::string default_;
    std::map<uint32_t, Seen> apps_of_;  // stream -> a bottom app's or not
    pa_threaded_mainloop* ml_ = nullptr;
    pa_mainloop_api* api_ = nullptr;
    pa_context* ctx_ = nullptr;
    pa_time_event* settle_ = nullptr;
    timeval tv_{};
};

}  // namespace

std::unique_ptr<Module> make_barry_audio() { return std::make_unique<BarryAudio>(); }

}  // namespace pp

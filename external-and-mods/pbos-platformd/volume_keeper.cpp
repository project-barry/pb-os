// volume-keeper (was sm8550-volume-keeper): keep the output volume across
// reboots.
//
// SM8550 audio runs in PipeWire's pro-audio profile, whose outputs have no
// routes. WirePlumber keeps their volume only in its stream-properties state
// (a normal reboot of the Retroid Pocket 6 kept it), but outputs came back at
// 100% after some shutdowns or hard power-offs, and an output that was never
// the default (e.g. the second pro-output) sits at 100%.
//
// On the Retroid Pocket Duo it is for "Top screen", an output without routes
// too (the card itself is in UCM HiFi).
//
// This restores each output's last volume and mute state when it first shows
// up as the default output (one never seen before keeps its volume, or gets
// half if it is at the boot-time 100%), then saves them whenever they change,
// flushed to disk at once, so holding the power button seconds after a change
// does not lose it.
//
// The sound server tells it what changed (PipeWire's PulseAudio interface,
// subscribed to the default output and the outputs' volumes); the Python
// version ran wpctl twice every 2 s, 1.7 s of CPU a minute on the Nova.

#include <fcntl.h>
#include <pulse/pulseaudio.h>
#include <unistd.h>

#include <cmath>
#include <cstdlib>
#include <set>
#include <string>

#include "json.h"
#include "module.h"
#include "util.h"

namespace pp {

namespace {

using pj::Value;

constexpr double FIRST_VOLUME = 0.5;
// A slider being dragged changes the volume many times a second: saved once
// it has been still this long (the Python version saw it every 2 s).
constexpr pa_usec_t SAVE_AFTER_US = 300 * 1000;
constexpr pa_usec_t RECONNECT_US = 2 * 1000 * 1000;

std::string state_path() {
    const char* x = std::getenv("XDG_STATE_HOME");
    std::string dir = x && *x ? x : std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/.local/state";
    return dir + "/sm8550-volume.json";
}

// As wpctl reports it: the volume rounded to two places.
double round2(double v) { return std::round(v * 100) / 100; }

// Write and flush to disk before replacing: this has to survive a hard
// power-off seconds later, which is when the volume gets lost.
void save_durably(const std::string& path, const Value& data) {
    std::string dir = pq::dirname(path);
    pq::makedirs(dir);
    std::string tmp = path + ".tmp";
    std::string text = data.dump();
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        pq::log("volume-keeper: cannot write %s", tmp.c_str());
        return;
    }
    bool ok = write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size()) && fsync(fd) == 0;
    close(fd);
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        pq::log("volume-keeper: cannot save %s", path.c_str());
        return;
    }
    int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
}

class VolumeKeeper : public Module {
public:
    const char* name() const override { return "volume-keeper"; }

    void start(pq::Loop&) override {
        path_ = state_path();
        saved_ = pq::read_json(path_);
        ml_ = pa_threaded_mainloop_new();
        api_ = pa_threaded_mainloop_get_api(ml_);
        pa_threaded_mainloop_lock(ml_);
        connect();
        pa_threaded_mainloop_unlock(ml_);
        pa_threaded_mainloop_start(ml_);
    }

    void stop() override {
        if (!ml_) return;
        pa_threaded_mainloop_lock(ml_);
        if (save_timer_) flush();
        pa_threaded_mainloop_unlock(ml_);
        pa_threaded_mainloop_stop(ml_);
        if (ctx_) {
            // Disconnecting calls the state callback at once, which would
            // reconnect (retry() drops the context): no callbacks first.
            pa_context_set_state_callback(ctx_, nullptr, nullptr);
            pa_context_set_subscribe_callback(ctx_, nullptr, nullptr);
            pa_context_disconnect(ctx_);
            pa_context_unref(ctx_);
        }
        pa_threaded_mainloop_free(ml_);
        ml_ = nullptr;
    }

private:
    // The mainloop thread from here on (pulse's callbacks hold its lock).

    void connect() {
        ctx_ = pa_context_new(api_, "pbos-platformd volume-keeper");
        pa_context_set_state_callback(ctx_, &VolumeKeeper::on_state, this);
        pa_context_set_subscribe_callback(ctx_, &VolumeKeeper::on_event, this);
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
        default_.clear();
        api_->time_new(api_, in(RECONNECT_US),
                       [](pa_mainloop_api* a, pa_time_event* e, const timeval*, void* self) {
                           a->time_free(e);
                           static_cast<VolumeKeeper*>(self)->connect();
                       },
                       this);
    }

    static void on_state(pa_context* c, void* p) {
        auto* self = static_cast<VolumeKeeper*>(p);
        switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            pa_operation_unref(pa_context_subscribe(
                c, static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER), nullptr,
                nullptr));
            self->ask_default();
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
        auto* self = static_cast<VolumeKeeper*>(p);
        auto facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
        auto kind = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;
        if (facility == PA_SUBSCRIPTION_EVENT_SERVER) {
            self->ask_default();  // the default output may have changed
        } else if (facility == PA_SUBSCRIPTION_EVENT_SINK && kind == PA_SUBSCRIPTION_EVENT_NEW) {
            self->ask_default();  // the default's sink may only now exist
        } else if (facility == PA_SUBSCRIPTION_EVENT_SINK && kind == PA_SUBSCRIPTION_EVENT_CHANGE &&
                   idx == self->default_idx_) {
            self->ask_sink();
        }
    }

    void ask_default() {
        pa_operation_unref(pa_context_get_server_info(
            ctx_,
            [](pa_context*, const pa_server_info* i, void* p) {
                auto* self = static_cast<VolumeKeeper*>(p);
                std::string name = i && i->default_sink_name ? i->default_sink_name : "";
                if (name.empty()) return;
                if (name != self->default_) {
                    self->default_ = name;
                    self->default_idx_ = PA_INVALID_INDEX;
                }
                self->ask_sink();
            },
            this));
    }

    void ask_sink() {
        pa_operation_unref(pa_context_get_sink_info_by_name(ctx_, default_.c_str(), &VolumeKeeper::on_sink, this));
    }

    static void on_sink(pa_context*, const pa_sink_info* i, int eol, void* p) {
        if (eol || !i) return;
        static_cast<VolumeKeeper*>(p)->sink(*i);
    }

    void sink(const pa_sink_info& i) {
        if (default_ != i.name) return;  // the default moved on meanwhile
        default_idx_ = i.index;
        double volume = round2(static_cast<double>(pa_cvolume_avg(&i.volume)) / PA_VOLUME_NORM);
        bool muted = i.mute;
        if (!restored_.count(default_)) {
            restored_.insert(default_);
            // Never seen: keep what it has, unless that is the 100% a fresh
            // boot starts with.
            const Value& had = saved_[default_];
            double want = had.truthy() ? had["volume"].as_double(volume) : (volume >= 0.99 ? FIRST_VOLUME : volume);
            bool want_muted = had.truthy() ? had["muted"].truthy() : muted;
            pa_cvolume cv;
            pa_cvolume_set(&cv, i.volume.channels, static_cast<pa_volume_t>(std::lround(want * PA_VOLUME_NORM)));
            pa_operation_unref(pa_context_set_sink_volume_by_index(ctx_, i.index, &cv, nullptr, nullptr));
            pa_operation_unref(pa_context_set_sink_mute_by_index(ctx_, i.index, want_muted, nullptr, nullptr));
            pq::log("%s: volume %.2f%s", default_.c_str(), want, want_muted ? " (muted)" : "");
            return;  // its change event brings the result back here
        }
        const Value& had = saved_[default_];
        if (had.is_object() && had["volume"].is_number() && had["volume"].as_double() == volume &&
            had["muted"].is_bool() && had["muted"].as_bool() == muted)
            return;
        Value now = Value::object();
        now["volume"] = volume;
        now["muted"] = muted;
        saved_[default_] = now;
        if (save_timer_) api_->time_free(save_timer_);
        save_timer_ = api_->time_new(api_, in(SAVE_AFTER_US),
                                     [](pa_mainloop_api* a, pa_time_event* e, const timeval*, void* self) {
                                         auto* k = static_cast<VolumeKeeper*>(self);
                                         a->time_free(e);
                                         k->save_timer_ = nullptr;
                                         save_durably(k->path_, k->saved_);
                                     },
                                     this);
    }

    // A time this far from now, for the mainloop's timers (wall clock).
    const timeval* in(pa_usec_t us) {
        pa_gettimeofday(&tv_);
        pa_timeval_add(&tv_, us);
        return &tv_;
    }

    void flush() {
        api_->time_free(save_timer_);
        save_timer_ = nullptr;
        save_durably(path_, saved_);
    }

    std::string path_;
    Value saved_;
    std::set<std::string> restored_;  // outputs handled this session
    std::string default_;
    uint32_t default_idx_ = PA_INVALID_INDEX;
    pa_threaded_mainloop* ml_ = nullptr;
    pa_mainloop_api* api_ = nullptr;
    pa_context* ctx_ = nullptr;
    pa_time_event* save_timer_ = nullptr;
    timeval tv_{};
};

}  // namespace

std::unique_ptr<Module> make_volume_keeper() { return std::make_unique<VolumeKeeper>(); }

}  // namespace pp

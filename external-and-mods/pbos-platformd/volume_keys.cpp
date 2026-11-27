// volume-keys (was sm8550-volume-keys): the volume rocker → the default
// output's volume. gpio-keys = VOLUP, pmic_resin = VOLDOWN (Odin 2 and
// friends), read as they are: nothing is grabbed, and USB/Bluetooth
// keyboards, mice and the gamepad are left alone.
//
// On SM8550 the key can also reach us only through InputPlumber: on the
// Retroid Pocket 6 gpio-keys carries the back paddles too, so InputPlumber
// owns it and passes volume up on through its virtual keyboard. Steam shows
// its volume indicator for that key but does not change the volume, so this
// also reads "InputPlumber Keyboard": only for keys no physical device above
// already gives us, so nothing is applied twice.
//
// Holding volume up or down keeps stepping until the button is let go: none
// of these devices sends key repeats, so this repeats the step itself after
// SM8550_VOLUME_HOLD_DELAY seconds, every SM8550_VOLUME_HOLD_INTERVAL seconds.
//
// The Python version woke every 2 s, looked for InputPlumber's keyboard every
// 3 s while it was missing, and ran pactl two or three times per step. Here
// devices come and go with the kernel's input uevents, the keys are read when
// they are readable, a held key repeats on a timer, and the steps go to the
// sound server over one PulseAudio connection, with pactl's own volume math
// (the same requests pactl sends, so the same volumes come out).

#include <fcntl.h>
#include <linux/input.h>
#include <pulse/pulseaudio.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <string>

#include "module.h"
#include "util.h"

namespace pp {

namespace {

constexpr int REPEAT_KEYS[] = {KEY_VOLUMEUP, KEY_VOLUMEDOWN};
constexpr const char* ALLOW[] = {"gpio-keys", "pmic_resin"};
constexpr const char* IP_KEYBOARD = "inputplumber keyboard";
constexpr const char* SINK = "@DEFAULT_SINK@";

double mono_now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string env_or(const char* name, const char* def) {
    const char* v = std::getenv(name);
    return v ? v : def;
}

// Python's float() and int(): the whole string, spaces around allowed.
bool parse_double(const std::string& s, double& out) {
    std::string t = pq::trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(t.c_str(), &end);
    if (*end || errno) return false;
    out = v;
    return true;
}

bool parse_int(const std::string& s, long& out) {
    std::string t = pq::trim(s);
    if (t.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long v = std::strtol(t.c_str(), &end, 10);
    if (*end || errno) return false;
    out = v;
    return true;
}

// pactl's volume argument ("5%", "-5%", "0.5", "-3dB", "65536"), as pactl
// parses it: relative when it starts with + or -, percent/dB/linear/raw.
// Relative percent and raw volumes are added (offset by PA_VOLUME_NORM),
// relative linear and dB ones multiplied.
enum VolFlags { VOL_UINT = 0, VOL_PERCENT = 1, VOL_LINEAR = 2, VOL_DECIBEL = 3, VOL_ABSOLUTE = 0, VOL_RELATIVE = 0x10 };

struct VolSpec {
    bool ok = false;
    int flags = 0;
    pa_volume_t v = 0;
};

static bool ends_with(const std::string& s, const char* e) {
    size_t n = std::strlen(e);
    return s.size() >= n && s.compare(s.size() - n, n, e) == 0;
}

VolSpec parse_volume(const std::string& spec) {
    VolSpec r;
    std::string vs = spec;
    r.flags = (!vs.empty() && (vs[0] == '+' || vs[0] == '-')) ? VOL_RELATIVE : VOL_ABSOLUTE;
    if (vs.find('.') != std::string::npos) r.flags |= VOL_LINEAR;
    if (ends_with(vs, "%")) {
        r.flags |= VOL_PERCENT;
        vs.pop_back();
    }
    if (ends_with(vs, "db") || ends_with(vs, "dB")) {
        r.flags |= VOL_DECIBEL;
        vs.resize(vs.size() - 2);
    }
    const char* in = vs.c_str();
    if (*in == '+') ++in;  // pa_atod() takes no leading '+'
    char* end = nullptr;
    errno = 0;
    double v = std::strtod(in, &end);
    if (!*in || *end || errno) return r;  // "Invalid volume specification"
    int kind = r.flags & 0x0F;
    if (r.flags & VOL_RELATIVE) {
        if (kind == VOL_UINT) v += PA_VOLUME_NORM;
        if (kind == VOL_PERCENT) v += 100.0;
        if (kind == VOL_LINEAR) v += 1.0;
    }
    if (kind == VOL_PERCENT) v = v * static_cast<double>(PA_VOLUME_NORM) / 100;
    if (kind == VOL_LINEAR) v = pa_sw_volume_from_linear(v);
    if (kind == VOL_DECIBEL) v = pa_sw_volume_from_dB(v);
    // (pa_volume_t) of a negative double: 0 on arm64, as pactl runs here.
    pa_volume_t pv = v <= 0 ? 0 : v >= 4294967295.0 ? UINT32_MAX : static_cast<pa_volume_t>(v);
    if (!PA_VOLUME_IS_VALID(pv)) return r;  // "Volume outside permissible range"
    r.v = pv;
    r.ok = true;
    return r;
}

// pactl set-sink-volume: one value fills every channel; relative ones adjust
// the sink's current volume channel by channel.
pa_cvolume fill_volume(const VolSpec& s, const pa_cvolume& current, unsigned channels) {
    pa_cvolume vol;
    pa_cvolume_set(&vol, channels, s.v);
    if (!(s.flags & VOL_RELATIVE)) return vol;
    pa_cvolume cv = current;
    int kind = s.flags & 0x0F;
    if (kind == VOL_UINT || kind == VOL_PERCENT) {
        for (unsigned i = 0; i < cv.channels; ++i) {
            if (cv.values[i] + vol.values[i] < PA_VOLUME_NORM)
                cv.values[i] = PA_VOLUME_MUTED;
            else
                cv.values[i] = cv.values[i] + vol.values[i] - PA_VOLUME_NORM;
        }
    } else {
        pa_sw_cvolume_multiply(&cv, &cv, &vol);
    }
    return cv;
}

// What `pactl get-sink-volume` prints for the first channel ("37%").
long first_channel_percent(const pa_cvolume& cv) {
    if (!cv.channels) return 0;
    return static_cast<long>((static_cast<uint64_t>(cv.values[0]) * 100 + PA_VOLUME_NORM / 2) / PA_VOLUME_NORM);
}

// The sound server side: the steps, in order, one at a time (as the Python
// ran pactl: a step reads the volume the previous one set). Its own thread,
// as volume-keeper's; connects when first needed and again after PipeWire
// restarts (at the next key press, not on a clock).
class Sound {
public:
    struct Config {
        bool up_ok = false;
        long step_pct = 5;   // volume up: int(STEP.rstrip('%'))
        long limit_pct = 100;  // int(float(LIMIT) * 100)
        VolSpec down;          // volume down: pactl's "-<STEP>"
    };

    void start(const Config& cfg) {
        cfg_ = cfg;
        ml_ = pa_threaded_mainloop_new();
        api_ = pa_threaded_mainloop_get_api(ml_);
        pa_threaded_mainloop_lock(ml_);
        connect();
        pa_threaded_mainloop_unlock(ml_);
        pa_threaded_mainloop_start(ml_);
    }

    void stop() {
        if (!ml_) return;
        pa_threaded_mainloop_stop(ml_);
        drop_context();
        pa_threaded_mainloop_free(ml_);
        ml_ = nullptr;
    }

    // From the loop thread. A repeat is skipped while a step is still on its
    // way (the Python's repeats waited for pactl the same way).
    void submit(int code, bool repeat) {
        if (!ml_) return;
        pa_threaded_mainloop_lock(ml_);
        if (!(repeat && (busy_ || !queue_.empty()))) {
            queue_.push_back(code);
            if (!ctx_) connect();
            next();
        }
        pa_threaded_mainloop_unlock(ml_);
    }

private:
    // The mainloop thread from here on (pulse's callbacks hold its lock).

    void connect() {
        ctx_ = pa_context_new(api_, "pbos-platformd volume-keys");
        pa_context_set_state_callback(ctx_, &Sound::on_state, this);
        // As the Python's pactl: XDG_RUNTIME_DIR defaults to /run/user/<uid>.
        std::string server;
        if (!std::getenv("XDG_RUNTIME_DIR") && !std::getenv("PULSE_SERVER"))
            server = "unix:/run/user/" + std::to_string(getuid()) + "/pulse/native";
        if (pa_context_connect(ctx_, server.empty() ? nullptr : server.c_str(), PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
            lost();
    }

    void drop_context() {
        if (!ctx_) return;
        pa_context_set_state_callback(ctx_, nullptr, nullptr);
        pa_context_disconnect(ctx_);
        pa_context_unref(ctx_);
        ctx_ = nullptr;
    }

    // Not up yet, or PipeWire went away: what was asked is dropped (pactl
    // would have failed too); the next key press connects again.
    void lost() {
        drop_context();
        queue_.clear();
        busy_ = false;
    }

    static void on_state(pa_context* c, void* p) {
        auto* self = static_cast<Sound*>(p);
        switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
            self->next();
            break;
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            // Not from inside the context's own callback: on the next turn.
            self->api_->defer_new(self->api_,
                                  [](pa_mainloop_api* a, pa_defer_event* e, void* s) {
                                      a->defer_free(e);
                                      auto* me = static_cast<Sound*>(s);
                                      if (me->ctx_ && !PA_CONTEXT_IS_GOOD(pa_context_get_state(me->ctx_))) me->lost();
                                  },
                                  self);
            break;
        default:
            break;
        }
    }

    void next() {
        if (busy_ || queue_.empty() || !ctx_ || pa_context_get_state(ctx_) != PA_CONTEXT_READY) return;
        int code = queue_.front();
        queue_.pop_front();
        busy_ = true;
        pa_operation* o = nullptr;
        if (code == KEY_VOLUMEUP) {
            // Step in the same (PulseAudio/cubic) scale as Steam's volume
            // bar. wpctl steps in a different scale: from ~20 % on the bar
            // one "5 %-" step hit zero and muted the sink.
            pa_operation_unref(pa_context_set_sink_mute_by_name(ctx_, SINK, 0, nullptr, nullptr));
            o = pa_context_get_sink_info_by_name(ctx_, SINK, &Sound::on_up, this);
        } else if (code == KEY_VOLUMEDOWN) {
            o = pa_context_get_sink_info_by_name(ctx_, SINK, &Sound::on_down, this);
        } else if (code == KEY_MUTE) {
            o = pa_context_get_sink_info_by_name(ctx_, SINK, &Sound::on_toggle_mute, this);
        }
        if (o) pa_operation_unref(o);
        else done();
    }

    void done() {
        busy_ = false;
        next();
    }

    static void on_set(pa_context*, int, void* p) { static_cast<Sound*>(p)->done(); }

    // Each step, for the PB-OS tab (its snap, as Steam's sliders make).
    static void on_stepped(pa_context*, int, void* p) {
        auto* self = static_cast<Sound*>(p);
        const char* run = std::getenv("XDG_RUNTIME_DIR");
        std::string dir = (run && *run ? run : "/run/user/" + std::to_string(getuid())) + std::string("/pbos");
        pq::write_text(dir + "/volume-key", self->up_ ? "up" : "down");
        self->done();
    }

    // pactl get-sink-volume, then pactl set-sink-volume "<min(LIMIT, now + STEP)>%".
    static void on_up(pa_context* c, const pa_sink_info* i, int, void* p) {
        auto* self = static_cast<Sound*>(p);
        if (!i) {
            // pactl found no sink: the Python read 0 % and set STEP %, which
            // failed the same way. (eol < 0: an error; > 0: the end.)
            if (!self->acted_) self->done();
            self->acted_ = false;
            return;
        }
        self->acted_ = true;
        const Config& k = self->cfg_;
        if (!k.up_ok) {
            self->done();
            return;
        }
        long pct = std::min(k.limit_pct, first_channel_percent(i->volume) + k.step_pct);
        VolSpec s = parse_volume(std::to_string(pct) + "%");
        if (!s.ok) {
            self->done();
            return;
        }
        pa_cvolume cv = fill_volume(s, i->volume, i->channel_map.channels);
        self->up_ = true;
        pa_operation_unref(pa_context_set_sink_volume_by_name(c, SINK, &cv, &Sound::on_stepped, self));
    }

    // pactl set-sink-volume "-<STEP>".
    static void on_down(pa_context* c, const pa_sink_info* i, int, void* p) {
        auto* self = static_cast<Sound*>(p);
        if (!i) {
            if (!self->acted_) self->done();
            self->acted_ = false;
            return;
        }
        self->acted_ = true;
        if (!self->cfg_.down.ok) {
            self->done();
            return;
        }
        pa_cvolume cv = fill_volume(self->cfg_.down, i->volume, i->channel_map.channels);
        self->up_ = false;
        pa_operation_unref(pa_context_set_sink_volume_by_name(c, SINK, &cv, &Sound::on_stepped, self));
    }

    // pactl set-sink-mute toggle.
    static void on_toggle_mute(pa_context* c, const pa_sink_info* i, int, void* p) {
        auto* self = static_cast<Sound*>(p);
        if (!i) {
            if (!self->acted_) self->done();
            self->acted_ = false;
            return;
        }
        self->acted_ = true;
        pa_operation_unref(pa_context_set_sink_mute_by_name(c, SINK, !i->mute, &Sound::on_set, self));
    }

    Config cfg_;
    pa_threaded_mainloop* ml_ = nullptr;
    pa_mainloop_api* api_ = nullptr;
    pa_context* ctx_ = nullptr;
    std::deque<int> queue_;
    bool busy_ = false;
    bool up_ = false;     // the step under way
    bool acted_ = false;  // the sink info came (its end-of-list call follows)
};

class VolumeKeys : public Module {
public:
    const char* name() const override { return "volume-keys"; }

    void start(pq::Loop& loop) override {
        loop_ = &loop;
        read_config();
        use_ip_ = dt_has("qcom,sm8550");
        ep_ = epoll_create1(EPOLL_CLOEXEC);
        auto paths = event_nodes();
        for (const auto& path : paths) add_physical(path);
        if (fds_.empty() && !use_ip_) {
            log("no gpio-keys/pmic_resin volume devices");
            return;
        }
        for (const auto& path : paths)
            if (add_ip(path)) break;
        sound_.start(sound_cfg_);
        loop.on_readable(ep_, [this] { readable(); });
        loop.on_uevent_env([this](const pq::Loop::UeventEnv& env) { uevent(env); });
        // A node is root's when the kernel announces it; udev then gives it
        // to the session (group, ACL): retried when its attributes change.
        loop.watch("/dev/input", IN_ATTRIB, [this](uint32_t, const std::string& name) {
            std::string path = "/dev/input/" + name;
            if (pending_.erase(path)) consider(path);
        });
    }

    void stop() override {
        cancel_repeat();
        sound_.stop();
        for (auto& [fd, d] : fds_) close(fd);
        fds_.clear();
        // ep_ stays open: the loop still has it (no way to take it back).
    }

private:
    struct Device {
        std::string path;
        std::set<int> keys;  // the keys acted on from this device
    };

    static void log(const std::string& msg) { pq::log("volume-keys: %s", msg.c_str()); }

    void read_config() {
        std::string step = env_or("SM8550_VOLUME_STEP", "5%");
        std::string limit = env_or("SM8550_VOLUME_LIMIT", "1.0");
        Sound::Config& c = sound_cfg_;
        std::string up_step = step;
        while (!up_step.empty() && up_step.back() == '%') up_step.pop_back();
        double lim = 0;
        c.up_ok = parse_int(up_step, c.step_pct) && parse_double(limit, lim) && std::isfinite(lim);
        if (c.up_ok) c.limit_pct = static_cast<long>(lim * 100);  // int(): toward zero
        else log("SM8550_VOLUME_STEP/LIMIT unreadable (" + step + ", " + limit + "): volume up does nothing");
        c.down = parse_volume("-" + step);
        if (!c.down.ok) log("SM8550_VOLUME_STEP unreadable (" + step + "): volume down does nothing");
        double d;
        std::string s = env_or("SM8550_VOLUME_HOLD_DELAY", "0.4");
        if (parse_double(s, d)) hold_delay_ = d;
        else log("SM8550_VOLUME_HOLD_DELAY unreadable (" + s + "), 0.4");
        s = env_or("SM8550_VOLUME_HOLD_INTERVAL", "0.1");
        if (parse_double(s, d)) hold_interval_ = d;
        else log("SM8550_VOLUME_HOLD_INTERVAL unreadable (" + s + "), 0.1");
    }

    // /dev/input/event*, sorted as the Python's glob was.
    static std::vector<std::string> event_nodes() {
        auto v = pq::glob("/dev/input/event*");
        std::sort(v.begin(), v.end());
        return v;
    }

    static std::string device_name(const std::string& path) {
        return pq::lower(pq::trim(pq::rd("/sys/class/input/" + pq::basename(path) + "/device/name")));
    }

    static bool allowed(const std::string& dname) {
        for (const char* a : ALLOW)
            if (dname.find(a) != std::string::npos) return true;
        return false;
    }

    // -1 when it cannot be opened; a node udev has not handed over yet is
    // remembered and tried again when its attributes change.
    int open_node(const std::string& path) {
        int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0 && (errno == EACCES || errno == EPERM)) pending_.insert(path);
        return fd;
    }

    static std::set<int> volume_keys(int fd) {
        unsigned char bits[96] = {};
        std::set<int> keys;
        if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return keys;
        for (int c : {KEY_VOLUMEUP, KEY_VOLUMEDOWN, KEY_MUTE})
            if (bits[c / 8] & (1 << (c % 8))) keys.insert(c);
        return keys;
    }

    static std::string key_list(const std::set<int>& keys) {
        std::string s = "[";
        for (int k : keys) s += (s.size() > 1 ? ", " : "") + std::to_string(k);
        return s + "]";
    }

    void watch_fd(int fd, const std::string& path, const std::set<int>& keys) {
        fds_[fd] = {path, keys};
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &ev);
    }

    bool add_physical(const std::string& path) {
        std::string dname = device_name(path);
        if (!allowed(dname)) return false;
        int fd = open_node(path);
        if (fd < 0) return false;
        std::set<int> keys = volume_keys(fd);
        if (keys.empty()) {
            close(fd);
            return false;
        }
        log("watch " + path + " (" + dname + ") " + key_list(keys));
        watch_fd(fd, path, keys);
        physical_.insert(keys.begin(), keys.end());
        // One that came late takes its keys from InputPlumber's keyboard.
        if (ip_fd_ >= 0) {
            auto& ip = fds_[ip_fd_].keys;
            for (int k : keys) ip.erase(k);
            if (ip.empty()) drop(ip_fd_);
        }
        return true;
    }

    // (Re)attach InputPlumber's keyboard; it comes and goes with its targets.
    bool add_ip(const std::string& path) {
        if (!use_ip_ || ip_fd_ >= 0 || device_name(path) != IP_KEYBOARD) return false;
        int fd = open_node(path);
        if (fd < 0) return false;
        std::set<int> keys;
        for (int k : volume_keys(fd))
            if (!physical_.count(k)) keys.insert(k);
        if (keys.empty()) {
            close(fd);
            return true;  // the Python stopped looking here too
        }
        log("watch " + path + " (inputplumber keyboard) " + key_list(keys));
        watch_fd(fd, path, keys);
        ip_fd_ = fd;
        return true;
    }

    void consider(const std::string& path) {
        for (auto& [fd, d] : fds_)
            if (d.path == path) return;
        if (!add_physical(path)) add_ip(path);
    }

    int fd_of(const std::string& path) const {
        for (const auto& [fd, d] : fds_)
            if (d.path == path) return fd;
        return -1;
    }

    void drop(int fd) {
        auto it = fds_.find(fd);
        if (it == fds_.end()) return;
        log("lost " + it->second.path);
        epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr);
        close(fd);
        fds_.erase(it);
        held_from_.erase(fd);
        if (fd == ip_fd_) ip_fd_ = -1;
        check_held();
    }

    // SUBSYSTEM=input with DEVNAME=input/eventN (the kernel's; udev's carry
    // /dev/). An empty map: some were dropped, so look at them all again.
    void uevent(const pq::Loop::UeventEnv& env) {
        if (env.empty()) {
            auto nodes = event_nodes();
            std::set<std::string> now(nodes.begin(), nodes.end());
            std::vector<int> gone;
            for (const auto& [fd, d] : fds_)
                if (!now.count(d.path)) gone.push_back(fd);
            for (int fd : gone) drop(fd);
            for (auto it = pending_.begin(); it != pending_.end();)
                it = now.count(*it) ? std::next(it) : pending_.erase(it);
            for (const auto& p : nodes) consider(p);
            return;
        }
        auto sub = env.find("SUBSYSTEM");
        auto dev = env.find("DEVNAME");
        auto act = env.find("ACTION");
        if (sub == env.end() || sub->second != "input" || dev == env.end() || act == env.end()) return;
        std::string path = dev->second;
        if (path.rfind("/dev/", 0) != 0) path = "/dev/" + path;
        if (path.rfind("/dev/input/event", 0) != 0) return;
        if (act->second == "add") {
            int old = fd_of(path);  // a number reused before we saw it go
            if (old >= 0) drop(old);
            consider(path);
        } else if (act->second == "remove") {
            pending_.erase(path);
            int fd = fd_of(path);
            if (fd >= 0) drop(fd);
        }
    }

    void readable() {
        epoll_event evs[16];
        int n = epoll_wait(ep_, evs, 16, 0);
        for (int i = 0; i < n; ++i) read_device(evs[i].data.fd);
    }

    void read_device(int fd) {
        input_event evs[64];
        while (fds_.count(fd)) {
            ssize_t n = read(fd, evs, sizeof evs);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
            if (n <= 0) {
                drop(fd);  // unplugged (ENODEV)
                return;
            }
            const auto& keys = fds_[fd].keys;
            for (size_t i = 0; i < static_cast<size_t>(n) / sizeof(input_event); ++i)
                if (evs[i].type == EV_KEY && keys.count(evs[i].code)) key_event(fd, evs[i].code, evs[i].value);
        }
    }

    static bool is_repeat_key(int code) { return std::find(std::begin(REPEAT_KEYS), std::end(REPEAT_KEYS), code) != std::end(REPEAT_KEYS); }

    void key_event(int fd, int code, int value) {
        if (value == 1) {
            double now = mono_now();
            sound_.submit(code, false);
            if (is_repeat_key(code)) {
                held_ = code;
                held_from_[fd] = code;
                next_repeat_ = now + hold_delay_;
                arm_repeat();
            }
        } else if (value == 0) {
            auto it = held_from_.find(fd);
            if (it != held_from_.end() && it->second == code) {
                held_from_.erase(it);
                if (held_ == code) {
                    held_ = 0;
                    cancel_repeat();
                }
            }
        }
        check_held();
    }

    // The held key's device went away while held.
    void check_held() {
        if (!held_) return;
        for (const auto& [fd, code] : held_from_)
            if (code == held_) return;
        held_ = 0;
        cancel_repeat();
    }

    void arm_repeat() {
        cancel_repeat();
        repeat_timer_ = loop_->after(std::max(0.0, next_repeat_ - mono_now()), [this] {
            repeat_timer_ = 0;
            if (!held_) return;
            double now = mono_now();
            sound_.submit(held_, true);
            next_repeat_ = std::max(next_repeat_ + hold_interval_, now);
            arm_repeat();
        });
    }

    void cancel_repeat() {
        if (repeat_timer_) loop_->cancel(repeat_timer_);
        repeat_timer_ = 0;
    }

    pq::Loop* loop_ = nullptr;
    Sound::Config sound_cfg_;
    Sound sound_;
    double hold_delay_ = 0.4, hold_interval_ = 0.1;
    bool use_ip_ = false;
    int ep_ = -1;  // the devices' own epoll set: the loop cannot forget an fd
    std::map<int, Device> fds_;
    std::set<int> physical_;  // keys the physical devices give (never shrinks, as before)
    int ip_fd_ = -1;
    std::set<std::string> pending_;  // nodes not ours yet (udev at work)
    // The held volume key and when it next repeats; at most one at a time.
    int held_ = 0;
    std::map<int, int> held_from_;
    double next_repeat_ = 0;
    uint64_t repeat_timer_ = 0;
};

}  // namespace

std::unique_ptr<Module> make_volume_keys() { return std::make_unique<VolumeKeys>(); }

}  // namespace pp

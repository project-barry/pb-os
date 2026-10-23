// The notices section: a message Steam shows once it starts (sections/
// notices.js), for something the user should fix. It has no page in the
// PB-OS tab.
//
// The one notice: an older boot menu PB-OS knows (OpenABL 1.0/1.1, ROCKNIX
// ABL) instead of the OpenABL release PB-OS ships (/usr/share/pb-os/openabl,
// approved hashes in its releases.tsv). Both slots' boot loaders (abl_a,
// abl_b) are read and hashed as PB-OS Installer does (the SHA-256 of a
// menu's first bytes; the rest of the partition keeps what was there
// before), so the notice names the menu the device has. Devices already on
// the shipped release, or with a boot loader PB-OS can't identify, never see
// it. It comes back at every boot and Steam restart until both slots hold
// the shipped release.
//
// "update" installs the shipped release from here, as Armada's
// armada-abl-update (armada-os/armada, GPL-2.0-or-later) and PB-OS
// Installer's Repair do, and only where both slots hold a boot menu PB-OS
// knows for this chip: never over a stock or unknown boot loader (the stock
// one is needed for Android updates and can't be downloaded again). Both
// slots are backed up to /home/.pbos-abl-backup first; then slot b, then
// slot a, each written, flushed and read back; a slot that doesn't read back
// right gets its backup again.
//
// The second notice, only when there is no boot menu one to show: while PB-OS
// Sound Check ships, a short ask to run it if the sound goes, on every device
// (most reports come from the AYN Thor, which most users have). Once per
// device ("seen", in /var/lib/pbos-qam/notices.json).

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <ctime>
#include <mutex>
#include <optional>

#include "section.h"
#include "util.h"

namespace pq {

namespace {

using pj::Value;

const std::string OPENABL = "/usr/share/pb-os/openabl";
const std::string BACKUPS = "/home/.pbos-abl-backup";
const std::string ABL_NOTICE = "abl-openabl-1.2";
const std::string SOUND_NOTICE = "sound-check-help";
const std::string SOUND_CHECK = "/usr/lib/pbos-sound-check/pbos-sound-check";
const std::string SEEN = "/var/lib/pbos-qam/notices.json";

struct Menu {
    const char* name;
    const char* soc;
    const char* sha256;
    size_t bytes;  // hashed from the start of the slot
};

// PB-OS Installer's list (abl/AblSetup.kt). The shipped release is found by
// its releases.tsv hash, whatever its name here.
const Menu MENUS[] = {
    {"OpenABL 1.2", "SM8550", "c7d681004563b69d892b5f93d3fac66a5f555c273c39ebf05febc72f95699df8", 532480},
    {"OpenABL 1.2", "SM8650", "c32434666ad5c725caa149151cfc5a66918c40f05e3f692c93795481910a6422", 532480},
    {"OpenABL 1.1", "SM8550", "cd97748bbd1c5dc994b8a73a05e88955297f4c88384e3ca4028d043fc1b16848", 532480},
    {"OpenABL 1.1", "SM8650", "8fd6a18ec30a8c24751b7db73a6ecb0d9df9f2e1432973273d572841ff3f6216", 532480},
    {"OpenABL 1.0", "SM8650", "23046d198dddaab53d7afc17bd726de4d2c65b6eb24d984a6b72e9a0963ba32c", 532480},
    {"ROCKNIX ABL 1.2", "SM8550", "8c27a70961476b327071ada2fd9b9c55dd2dcdc913eff3d33f2576133d7e398c", 258048},
    {"ROCKNIX ABL 1.2", "SM8650", "053e1e9e8300c29de68866c6c09e5527b1222d2ee774757d9cfbe8346137d6b6", 258048},
    {"ROCKNIX ABL 1.1.8", "SM8550", "5f1018211feaa109563d5890ab52dccfb31db90f4bbe9e6b7fb39e94f860f8d4", 258048},
    {"ROCKNIX ABL 1.1.8", "SM8650", "4ed04dda4da2ac76bc27925ae68094c055a043697f1a1bee4b958b979905bed5", 258048},
};

std::string slot_path(char s) { return std::string("/dev/disk/by-partlabel/abl_") + s; }

std::string sha256(const std::string& data) {
    RunOpts o;
    o.input = data;
    o.env = clean_env();
    auto r = run({"sha256sum"}, o);
    if (!r.started || r.code != 0) return "";
    return r.out.substr(0, r.out.find(' '));
}

// "SM8550" (also the QCS8550 handhelds) or "SM8650", from the device tree.
std::string soc() {
    std::string c = lower(dt_compatible());
    if (c.find("sm8650") != std::string::npos) return "SM8650";
    if (c.find("sm8550") != std::string::npos || c.find("qcs8550") != std::string::npos) return "SM8550";
    return "";
}

// The release PB-OS ships for this chip, checked against releases.tsv.
struct Target {
    std::string version, sha256, payload;
    size_t bytes = 0;
};

std::optional<Target> target() {
    std::string s = soc();
    std::string version = trim(rd(OPENABL + "/VERSION"));
    if (s.empty() || version.empty()) return std::nullopt;
    for (const auto& line : split_lines(rd(OPENABL + "/releases.tsv"))) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        size_t start = 0;
        for (size_t tab; (tab = line.find('\t', start)) != std::string::npos; start = tab + 1) f.push_back(line.substr(start, tab - start));
        f.push_back(line.substr(start));
        if (f.size() < 4 || f[0] != version || f[1] != s || !is_digits(f[2])) continue;
        Target t{version, trim(f[3]), OPENABL + "/" + s + "/abl_signed-" + s + ".elf", std::stoul(f[2])};
        auto data = read_file(t.payload);
        if (!data || data->size() != t.bytes || sha256(*data) != t.sha256) {
            log("pbos-qamd: %s does not match releases.tsv", t.payload.c_str());
            return std::nullopt;
        }
        return t;
    }
    return std::nullopt;
}

struct Slot {
    bool readable = false;
    std::string menu;     // a name from MENUS, or "" for one PB-OS doesn't know
    bool known = false;   // a menu from MENUS for this chip
    bool current = false; // the shipped release
};

Slot read_slot(char s, const std::optional<Target>& t, const std::string& chip) {
    Slot out;
    auto data = read_file(slot_path(s));
    if (!data || data->empty()) return out;
    out.readable = true;
    if (t && data->size() >= t->bytes && sha256(data->substr(0, t->bytes)) == t->sha256) {
        out.menu = "OpenABL " + t->version;
        out.known = out.current = true;
        return out;
    }
    for (const auto& m : MENUS) {
        if (m.bytes > data->size() || sha256(data->substr(0, m.bytes)) != m.sha256) continue;
        out.menu = m.name;
        out.known = chip == m.soc;
        return out;
    }
    return out;
}

// On the charger, or a battery at least 30% full.
bool enough_power() {
    for (const auto& d : listdir("/sys/class/power_supply")) {
        std::string p = "/sys/class/power_supply/" + d;
        std::string type = trim(rd(p + "/type"));
        if (type != "Battery" && trim(rd(p + "/online")) == "1") return true;
    }
    for (const auto& d : listdir("/sys/class/power_supply")) {
        std::string p = "/sys/class/power_supply/" + d;
        if (trim(rd(p + "/type")) != "Battery") continue;
        std::string c = trim(rd(p + "/capacity"));
        if (is_digits(c) && std::stoi(c) >= 30) return true;
        if (trim(rd(p + "/status")) == "Charging") return true;
    }
    return false;
}

// Writes data at the start of the slot, flushes it and drops the cached copy.
bool write_slot(char s, const std::string& data) {
    int fd = open(slot_path(s).c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = pwrite(fd, data.data() + done, data.size() - done, static_cast<off_t>(done));
        if (n <= 0) break;
        done += static_cast<size_t>(n);
    }
    bool ok = done == data.size() && fsync(fd) == 0 && ioctl(fd, BLKFLSBUF, 0) == 0;
    close(fd);
    return ok;
}

class Notices : public Section {
public:
    std::string name() const override { return "notices"; }
    bool here() const override { return exists(slot_path('a')); }

    Value call(const std::string& m, const Value& a) override {
        if (m == "get") return get();
        if (m == "update") return update();
        if (m == "seen") return seen(arg(a, 0, "").str());
        throw CallError("notices has no method " + m);
    }

private:
    void check() {
        if (checked_) return;
        checked_ = true;
        target_ = target();
        std::string chip = soc();
        slots_[0] = read_slot('a', target_, chip);
        slots_[1] = read_slot('b', target_, chip);
        for (const auto& s : slots_)
            if (s.readable && !s.current) log("pbos-qamd: boot menu notice (%s)", s.menu.empty() ? "unknown menu" : s.menu.c_str());
    }

    bool outdated() const {
        for (const auto& s : slots_)
            if (s.readable && !s.current) return true;
        return false;
    }

    // Whether "update" may run: the shipped release is here and checked, and
    // both slots hold it or a boot menu PB-OS knows for this chip.
    bool can_update() const {
        if (!target_) return false;
        for (const auto& s : slots_)
            if (!s.readable || !s.known) return false;
        return true;
    }

    // {} when there is nothing to say; else {id, menu (of the first slot that
    // isn't current), canUpdate, version, device}. Only a device known to have an
    // older boot menu hears about it: one PB-OS can't identify (stock, a test
    // build) is left alone, as "update" would refuse it.
    Value get() {
        std::lock_guard<std::mutex> g(mu_);
        check();
        if (!outdated() || !can_update()) return sound_notice();
        Value v = Value::object();
        v["id"] = ABL_NOTICE;
        for (const auto& s : slots_)
            if (s.readable && !s.current) {
                v["menu"] = s.menu;
                break;
            }
        v["canUpdate"] = can_update();
        v["version"] = target_ ? target_->version : "";
        v["device"] = dt_model();
        return v;
    }

    // The Sound Check ask: the app in the image, not seen yet.
    Value sound_notice() {
        Value v = Value::object();
        if (!can_exec(SOUND_CHECK)) return v;
        if (read_json(SEEN)[SOUND_NOTICE].truthy()) return v;
        v["id"] = SOUND_NOTICE;
        return v;
    }

    // A notice shown once: not again on this device.
    Value seen(const std::string& id) {
        std::lock_guard<std::mutex> g(mu_);
        if (id != SOUND_NOTICE) throw CallError("notices: unknown notice " + id);
        Value state = read_json(SEEN);
        state[id] = true;
        return Value(write_json(SEEN, state));
    }

    Value update() {
        std::lock_guard<std::mutex> g(mu_);
        check();
        if (!outdated()) return Value(true);
        if (!can_update()) throw CallError("This boot menu can't be updated from PB-OS: use PB-OS Installer's Repair");
        if (!enough_power()) throw CallError("Connect the charger first (or charge to 30%)");
        auto payload = read_file(target_->payload);
        if (!payload || payload->size() != target_->bytes || sha256(*payload) != target_->sha256)
            throw CallError("The boot menu file in PB-OS is damaged");

        // Both slots, whole, before anything is written.
        char stamp[32];
        time_t now = time(nullptr);
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
        std::string dir = BACKUPS + "/" + stamp;
        if (!makedirs(dir, 0700)) throw CallError("Couldn't make the backup folder");
        std::string backup[2];
        for (int i = 0; i < 2; ++i) {
            char s = i == 0 ? 'a' : 'b';
            auto data = read_file(slot_path(s));
            if (!data || data->empty() || !write_text(dir + "/abl_" + s + ".img", *data))
                throw CallError("Couldn't back up the boot menu");
            backup[i] = *data;
        }
        write_text(dir + "/README", "Boot menu slots abl_a and abl_b before PB-OS installed OpenABL " + target_->version +
                                        ". Each .img is the whole partition.\n");
        log("pbos-qamd: boot menu backed up to %s", dir.c_str());

        for (char s : {'b', 'a'}) {
            const Slot& slot = slots_[s == 'a' ? 0 : 1];
            if (slot.current) continue;
            const std::string& before = backup[s == 'a' ? 0 : 1];
            bool ok = write_slot(s, *payload);
            auto back = ok ? read_file(slot_path(s)) : std::nullopt;
            ok = back && back->size() >= target_->bytes && sha256(back->substr(0, target_->bytes)) == target_->sha256;
            if (!ok) {
                bool restored = write_slot(s, before);
                auto again = restored ? read_file(slot_path(s)) : std::nullopt;
                restored = again && sha256(*again) == sha256(before);
                log("pbos-qamd: boot menu slot %c failed; backup %s", s, restored ? "restored" : "NOT restored");
                checked_ = false;
                throw CallError(restored ? std::string("Updating the boot menu failed; it was put back as it was")
                                         : "Updating the boot menu failed and slot " + std::string(1, s) +
                                               " couldn't be put back. Don't restart: ask for help (backup in " + dir + ")");
            }
            log("pbos-qamd: boot menu slot %c now OpenABL %s", s, target_->version.c_str());
        }
        sync();
        checked_ = false;
        check();
        return Value(!outdated());
    }

    std::mutex mu_;
    bool checked_ = false;
    std::optional<Target> target_;
    Slot slots_[2];
};

}  // namespace

std::unique_ptr<Section> make_notices_section() { return std::make_unique<Notices>(); }

}  // namespace pq

// The notices section: messages Steam shows once it starts (sections/
// notices.js), each once, and the boot menu's update on the PB-OS tab's System
// page, there as long as there is one.
//
// The first: an older boot menu PB-OS knows (the PB-OS fork's OpenABL 1.2,
// OpenABL 1.0/1.1, ROCKNIX ABL) instead of the OpenABL release PB-OS ships, the
// same for every chip (/usr/share/pb-os/openabl, approved hashes in its
// releases.tsv). Both slots' boot loaders (abl_a, abl_b) are read and hashed as
// PB-OS Installer does (the SHA-256 of a menu's first bytes; the rest of the
// partition keeps what was there before), so the notice names the menu the
// device has. Releases are told apart by hash and chip, never by their names.
// Devices already on the shipped release, or with a boot loader PB-OS can't
// identify, never see it. The message is shown once for each shipped release
// ("seen", in /var/lib/pbos-qam/notices.json); the System page offers the update
// until both slots hold it ("offer").
//
// "update" installs the shipped release from here, as Armada's
// armada-abl-update (armada-os/armada, GPL-2.0-or-later) and PB-OS
// Installer's Repair do, and only where both slots hold a boot menu PB-OS
// knows for this chip: never over a stock or unknown boot loader (the stock
// one is needed for Android updates and can't be downloaded again). Both
// slots are backed up to /home/.pbos-abl-backup first; then the inactive slot,
// then the active one, each written, flushed and read back; a slot that doesn't
// read back right gets its backup again, and the other one isn't touched. When
// the active slot can't be told, nothing is offered or written.
//
// The second notice, when there is no boot menu one left to show: while PB-OS
// Sound Check ships, a short ask to run it if the sound goes, on every device
// (most reports come from the AYN Thor, which most users have). Once per
// device ("seen").

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <optional>
#include <sstream>

#include "section.h"
#include "util.h"

namespace pq {

namespace {

using pj::Value;

const std::string OPENABL = "/usr/share/pb-os/openabl";
const std::string BACKUPS = "/home/.pbos-abl-backup";
const std::string ABL_NOTICE = "abl-update";
const std::string SOUND_NOTICE = "sound-check-help";
const std::string SOUND_CHECK = "/usr/lib/pbos-sound-check/pbos-sound-check";
const std::string CMDLINE = "/proc/cmdline";
const std::string DEV = "/dev";
const std::string SYS_BLOCK = "/sys/class/block";
const std::string SEEN = "/var/lib/pbos-qam/notices.json";

struct Menu {
    const char* name;
    const char* soc;
    const char* sha256;
    size_t bytes;  // hashed from the start of the slot
};

// PB-OS Installer's list (abl/AblSetup.kt). The shipped release is found by
// its releases.tsv hash, whatever its name here. SM7750: none, as OpenABL-PB
// 1.2 is its first.
// When a release after OpenABL-PB 1.2 ships, OpenABL-PB 1.2's three payloads
// (releases.tsv) go here, or the new one is never offered over it: the image
// build refuses until they do.
const Menu MENUS[] = {
    {"OpenABL 1.2 (PB-OS fork)", "SM8550", "c7d681004563b69d892b5f93d3fac66a5f555c273c39ebf05febc72f95699df8", 532480},
    {"OpenABL 1.2 (PB-OS fork)", "SM8650", "c32434666ad5c725caa149151cfc5a66918c40f05e3f692c93795481910a6422", 532480},
    {"OpenABL 1.1", "SM8550", "cd97748bbd1c5dc994b8a73a05e88955297f4c88384e3ca4028d043fc1b16848", 532480},
    {"OpenABL 1.1", "SM8650", "8fd6a18ec30a8c24751b7db73a6ecb0d9df9f2e1432973273d572841ff3f6216", 532480},
    {"OpenABL 1.0", "SM8650", "23046d198dddaab53d7afc17bd726de4d2c65b6eb24d984a6b72e9a0963ba32c", 532480},
    {"ROCKNIX ABL 1.2", "SM8550", "8c27a70961476b327071ada2fd9b9c55dd2dcdc913eff3d33f2576133d7e398c", 258048},
    {"ROCKNIX ABL 1.2", "SM8650", "053e1e9e8300c29de68866c6c09e5527b1222d2ee774757d9cfbe8346137d6b6", 258048},
    {"ROCKNIX ABL 1.1.8", "SM8550", "5f1018211feaa109563d5890ab52dccfb31db90f4bbe9e6b7fb39e94f860f8d4", 258048},
    {"ROCKNIX ABL 1.1.8", "SM8650", "4ed04dda4da2ac76bc27925ae68094c055a043697f1a1bee4b958b979905bed5", 258048},
};

std::string slot_path(char s) { return DEV + "/disk/by-partlabel/abl_" + s; }

std::string sha256(const std::string& data) {
    RunOpts o;
    o.input = data;
    o.env = clean_env();
    auto r = run({"sha256sum"}, o);
    if (!r.started || r.code != 0) return "";
    return r.out.substr(0, r.out.find(' '));
}

// A SHA-256 as sha256() gives it: never "", which is its failure.
bool is_sha256(const std::string& s) {
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

// "SM8550" (also the QCS8550 handhelds), "SM8650" or "SM7750" (Linux calls it
// Eliza), from the device tree.
std::string soc() {
    std::string c = lower(dt_compatible());
    if (c.find("sm8650") != std::string::npos) return "SM8650";
    if (c.find("sm8550") != std::string::npos || c.find("qcs8550") != std::string::npos) return "SM8550";
    if (c.find("qcom,eliza") != std::string::npos) return "SM7750";
    return "";
}

// The release PB-OS ships for this chip, checked against releases.tsv; never
// another chip's payload.
struct Target {
    std::string name, sha256, payload;
    size_t bytes = 0;
};

std::optional<Target> target() {
    std::string s = soc();
    std::string version = trim(rd(OPENABL + "/VERSION"));
    if (s.empty() || version.empty()) return std::nullopt;
    std::optional<Target> t;
    std::vector<std::string> foreign;  // other chips' payloads
    for (const auto& m : MENUS)
        if (s != m.soc) foreign.push_back(m.sha256);
    for (const auto& line : split_lines(rd(OPENABL + "/releases.tsv"))) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        size_t start = 0;
        for (size_t tab; (tab = line.find('\t', start)) != std::string::npos; start = tab + 1) f.push_back(line.substr(start, tab - start));
        f.push_back(line.substr(start));
        if (f.size() < 4) continue;
        if (f[1] != s) foreign.push_back(trim(f[3]));
        if (f[0] != version || f[1] != s) continue;
        std::string sum = trim(f[3]);
        // One valid row: no hash, a size no boot loader has or a second row means no offer.
        if (t || !is_digits(f[2]) || f[2].size() > 7 || std::stoul(f[2]) == 0 || !is_sha256(sum)) {
            log("pbos-qamd: releases.tsv has no single valid row for %s", s.c_str());
            return std::nullopt;
        }
        // "OpenABL-PB 1.2", or an older folder's plain "1.2".
        t = Target{is_digits(version.substr(0, 1)) ? "OpenABL " + version : version, sum,
                   OPENABL + "/" + s + "/abl_signed-" + s + ".elf", std::stoul(f[2])};
    }
    if (!t) return std::nullopt;
    if (std::find(foreign.begin(), foreign.end(), t->sha256) != foreign.end()) {
        log("pbos-qamd: releases.tsv gives %s another chip's boot menu", s.c_str());
        return std::nullopt;
    }
    auto data = read_file(t->payload);
    if (!data || data->size() != t->bytes || sha256(*data) != t->sha256) {
        log("pbos-qamd: %s does not match releases.tsv", t->payload.c_str());
        return std::nullopt;
    }
    return t;
}

struct Slot {
    bool readable = false;
    size_t size = 0;
    std::string menu;     // a name from MENUS, or "" for one PB-OS doesn't know
    bool known = false;   // a menu from MENUS for this chip
    bool current = false; // the shipped release
};

Slot read_slot(char s, const std::optional<Target>& t, const std::string& chip) {
    Slot out;
    auto data = read_file(slot_path(s));
    if (!data || data->empty()) return out;
    out.readable = true;
    out.size = data->size();
    if (t && data->size() >= t->bytes && sha256(data->substr(0, t->bytes)) == t->sha256) {
        out.menu = t->name;
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

std::string read_at(const std::string& path, uint64_t off, size_t n) {
    std::string out(n, '\0');
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    ssize_t got = pread(fd, out.data(), n, static_cast<off_t>(off));
    close(fd);
    return got == static_cast<ssize_t>(n) ? out : "";
}

uint64_t le(const std::string& s, size_t at, size_t n) {
    uint64_t v = 0;
    for (size_t i = n; i-- > 0;) v = v << 8 | static_cast<unsigned char>(s[at + i]);
    return v;
}

// How the boot loader (Qualcomm's GetActiveSlot) sees a slot: 0 when its boot
// partition isn't marked active (GPT attribute bit 50), else the partition's
// priority (bits 48-49), the higher one wins.
std::optional<int> gpt_priority(char slot) {
    std::string name = std::string("boot_") + slot;
    std::string part = basename(realpath(DEV + "/disk/by-partlabel/" + name));
    std::string sys = realpath(SYS_BLOCK + "/" + part);
    std::string num = trim(rd(sys + "/partition")), lbs = trim(rd(dirname(sys) + "/queue/logical_block_size"));
    if (!is_digits(num) || !is_digits(lbs) || num.size() > 4 || lbs.size() > 6 || std::stoul(num) == 0) return std::nullopt;
    std::string disk = DEV + "/" + basename(dirname(sys));
    uint64_t block = std::stoul(lbs);
    std::string hdr = read_at(disk, block, 92);
    if (hdr.empty() || hdr.compare(0, 8, "EFI PART") != 0) return std::nullopt;
    uint64_t size = le(hdr, 84, 4);
    if (size < 128 || std::stoul(num) > le(hdr, 80, 4)) return std::nullopt;
    std::string e = read_at(disk, le(hdr, 72, 8) * block + (std::stoul(num) - 1) * size, 128);
    if (e.empty()) return std::nullopt;
    // The right entry: its name (UTF-16) is the partition's.
    for (size_t i = 0; i <= name.size(); ++i)
        if (le(e, 56 + 2 * i, 2) != (i < name.size() ? static_cast<unsigned char>(name[i]) : 0)) return std::nullopt;
    uint64_t attrs = le(e, 48, 8);
    return attrs >> 50 & 1 ? static_cast<int>(attrs >> 48 & 3) : 0;
}

// 'a' or 'b': the slot in use, which is also the one the device starts from
// next, or 0 when that can't be told (or androidboot.slot_suffix, where the boot
// loader passes it, says another).
char active_slot() {
    auto a = gpt_priority('a'), b = gpt_priority('b');
    if (!a || !b || *a == *b) return 0;
    char active = *a > *b ? 'a' : 'b';
    std::istringstream in(rd(CMDLINE));
    for (std::string w; in >> w;)
        if (w.rfind("androidboot.slot_suffix=", 0) == 0 && w != std::string("androidboot.slot_suffix=_") + active) return 0;
    return active;
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

// A file or folder, on the drive.
bool flush(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

class Notices : public Section {
public:
    std::string name() const override { return "notices"; }
    bool here() const override { return exists(slot_path('a')); }

    Value call(const std::string& m, const Value& a) override {
        if (m == "get") return get();
        if (m == "offer") return offer_now();
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
        active_ = active_slot();
        if (!active_ && outdated()) log("pbos-qamd: the active boot menu slot can't be told: no update");
        for (const auto& s : slots_)
            if (s.readable && !s.current) log("pbos-qamd: boot menu notice (%s)", s.menu.empty() ? "unknown menu" : s.menu.c_str());
    }

    bool outdated() const {
        for (const auto& s : slots_)
            if (s.readable && !s.current) return true;
        return false;
    }

    // Whether "update" may run: the shipped release is here and checked, the
    // active slot is known, and both slots hold the release or a boot menu PB-OS
    // knows for this chip, and have room.
    bool can_update() const {
        if (!target_ || !active_) return false;
        for (const auto& s : slots_)
            if (!s.readable || !s.known || s.size < target_->bytes) return false;
        return true;
    }

    // {} when there is no update; else {id, menu (of the first slot that isn't
    // current), canUpdate, version, device}. Only a device known to have an older
    // boot menu hears about it: one PB-OS can't identify (stock, a test build) is
    // left alone, as "update" would refuse it.
    Value offer() {
        check();
        Value v = Value::object();
        if (!outdated() || !can_update()) return v;
        v["id"] = ABL_NOTICE;
        for (const auto& s : slots_)
            if (s.readable && !s.current) {
                v["menu"] = s.menu;
                break;
            }
        v["canUpdate"] = true;
        v["version"] = target_->name;
        v["device"] = dt_model();
        return v;
    }

    // For the System page: whether or not the message was shown.
    Value offer_now() {
        std::lock_guard<std::mutex> g(mu_);
        return offer();
    }

    // The message to show: the update's, once for each release, else Sound Check's.
    Value get() {
        std::lock_guard<std::mutex> g(mu_);
        Value v = offer();
        if (v["id"].str().empty() || told_ == target_->name || read_json(SEEN)[ABL_NOTICE].str() == target_->name) return sound_notice();
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
        if (id != SOUND_NOTICE && id != ABL_NOTICE) throw CallError("notices: unknown notice " + id);
        Value state = read_json(SEEN);
        if (id == SOUND_NOTICE) {
            state[id] = true;
        } else {
            if (!target_) return Value(false);
            // Also here, so one that can't be saved doesn't keep Sound Check's away.
            told_ = target_->name;
            state[id] = told_;
        }
        return Value(write_json(SEEN, state));
    }

    Value update() {
        std::lock_guard<std::mutex> g(mu_);
        check();
        if (!outdated()) return Value(true);
        if (!active_) throw CallError("Couldn't tell which boot menu slot is in use. Nothing was changed");
        if (!can_update()) throw CallError("This boot menu can't be updated from PB-OS: use PB-OS Installer's Repair");
        if (active_slot() != active_) throw CallError("The boot menu slot in use changed. Restart and try again");
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
            std::string file = dir + "/abl_" + s + ".img";
            if (!data || data->size() < target_->bytes || !write_text(file, *data) || !flush(file) || read_file(file) != data)
                throw CallError("Couldn't back up the boot menu");
            backup[i] = *data;
        }
        write_text(dir + "/README", "Boot menu slots abl_a and abl_b before PB-OS installed " + target_->name +
                                        ". Each .img is the whole partition.\n");
        if (!flush(dir) || !flush(BACKUPS) || !flush(dirname(BACKUPS))) throw CallError("Couldn't back up the boot menu");
        log("pbos-qamd: boot menu backed up to %s", dir.c_str());

        // The inactive slot first: until it reads back right, the one in use is as it was.
        for (char s : {active_ == 'a' ? 'b' : 'a', active_}) {
            const Slot& slot = slots_[s == 'a' ? 0 : 1];
            if (slot.current) continue;
            const std::string& before = backup[s == 'a' ? 0 : 1];
            bool ok = write_slot(s, *payload);
            auto back = ok ? read_file(slot_path(s)) : std::nullopt;
            ok = back && back->size() >= target_->bytes && sha256(back->substr(0, target_->bytes)) == target_->sha256;
            if (!ok) {
                bool restored = write_slot(s, before);
                auto again = restored ? read_file(slot_path(s)) : std::nullopt;
                restored = again && *again == before;
                log("pbos-qamd: boot menu slot %c failed; backup %s", s, restored ? "restored" : "NOT restored");
                checked_ = false;
                throw CallError(restored ? std::string("Updating the boot menu failed; it was put back as it was")
                                         : "Updating the boot menu failed and slot " + std::string(1, s) +
                                               " couldn't be put back. Don't restart: ask for help (backup in " + dir + ")");
            }
            log("pbos-qamd: boot menu slot %c now %s", s, target_->name.c_str());
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
    char active_ = 0;   // the slot in use, or 0
    std::string told_;  // the release whose message was shown since this started
};

}  // namespace

std::unique_ptr<Section> make_notices_section() { return std::make_unique<Notices>(); }

}  // namespace pq

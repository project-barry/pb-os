// pbos-motiond: the built-in motion sensors (accelerometer and gyroscope)
// of the SM8550 handhelds as one uinput motion device, "PB-OS Motion
// Sensors", which InputPlumber folds into the Steam Deck controller it
// makes (its patch 0006), so Steam's gyro settings work on them.
//
// The sensors are not on a bus Linux drives: Qualcomm's sensor hub (SSC)
// on the ADSP owns them. With the ADSP's sensors process up (patch 0560,
// hexagonrpcd serving its files: sm8550-sensors), the hub offers a QMI
// service (400) over QRTR. This talks to it directly: QMI framing plus the
// few protobuf messages of Qualcomm's sensor API, so there is no libqmi.
//
//   1. the QRTR name service tells us where service 400 is (and when it
//      goes away and comes back: an ADSP restart);
//   2. per sensor, a SUID lookup ("accel", "gyro") asks for the default
//      sensor of that type;
//   3. a std sensor config (sample rate) starts its stream; each report
//      carries x, y, z (m/s², rad/s) in the sensor's own frame.
//
// Each device's profile turns the sensor frame into the Steam Deck's
// (x right, y toward the top of the screen, z out of the screen), and the
// output follows hid-playstation's motion sensors: accel 8192 per g,
// gyro 1024 per deg/s. The gyro's zero is learned while the device lies
// still and kept per device (Steam has its own calibration on top).
//
// Around suspend the streams are closed, so the sensors (sampling on the
// ADSP) don't run while the device sleeps: a logind delay lock holds the
// suspend until they are off, and they start again on resume.
//
// pbos-motiond --dump   print the samples instead of making the uinput
//                       device, for testing: the sensor's own frame (raw)
//                       and the Deck frame (a new device's profile).

#include <fcntl.h>
#include <linux/input.h>
#include <linux/qrtr.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <endian.h>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr uint32_t kSnsClientService = 400;
constexpr uint16_t kQmiSnsClientReq = 0x20;
constexpr uint16_t kQmiSnsClientReport = 0x21;
constexpr uint16_t kQmiSnsClientJumboReport = 0x22;
constexpr uint8_t kQmiRequest = 0, kQmiResponse = 2, kQmiIndication = 4;

// Message ids of Qualcomm's sensor API (sns_suid.proto, sns_std.proto,
// sns_std_sensor.proto, sns_client.proto).
constexpr uint32_t kMsgSuidReq = 512, kMsgSuidEvent = 768;
constexpr uint32_t kMsgStdSensorConfig = 513, kMsgStdSensorEvent = 1025;
constexpr uint32_t kMsgClientDisable = 10;
constexpr uint64_t kSuidLookup = 0xababababababababULL;
constexpr int kProcessorApss = 1, kDeliveryNoWakeup = 1;

constexpr float kGravity = 9.80665f;
constexpr float kRadToDeg = 57.29577951f;
constexpr int kAccelResPerG = 8192;
constexpr int kGyroResPerDps = 1024;
constexpr int kAccelRange = 4 * kAccelResPerG;
constexpr int kGyroRange = 2048 * kGyroResPerDps;

bool g_dump = false;

void logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void logf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

// ---- device profiles -----------------------------------------------------

struct Profile {
  const char *compatible;
  const char *id;
  // Deck frame = rot * sensor frame (a proper rotation, so it serves
  // the accelerometer and the gyroscope alike).
  int rot[3][3];
};

// AYN Thor (SH5001): its panels are portrait, so the stock Android frame
// is the device turned a quarter: Deck x = -sensor y, Deck y = sensor x.
// Matches the frame MaSi's and Batocera's bridges validated on a Thor.
constexpr Profile kProfiles[] = {
    {"ayn,thor", "thor", {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}},
    // Retroid Pocket Nova (QMI8658): the same quarter turn, measured on a
    // Nova (upright = sensor +x, right side down = sensor +y). The RP6 has
    // the same sensor and stock orientation (-x/-y/+z); not yet measured.
    {"retroidpocket,rpnova", "rpnova", {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}},
    {"retroidpocket,rp6", "rp6", {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}},
    // Retroid Pocket Duo (QMI8658, +y/+x/-z in its stock config, which the
    // ADSP applies; Android's landscape is the same quarter turn): the same;
    // the directions checked on a Duo.
    {"retroidpocket,rpduo", "rpduo", {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}},
};

std::optional<Profile> detect_profile() {
  std::ifstream f("/sys/firmware/devicetree/base/compatible", std::ios::binary);
  std::string all((std::istreambuf_iterator<char>(f)), {});
  size_t pos = 0;
  while (pos < all.size()) {
    std::string c = all.c_str() + pos;
    for (const auto &p : kProfiles)
      if (c == p.compatible) return p;
    pos += c.size() + 1;
  }
  return std::nullopt;
}

// ---- protobuf (just what the sensor API needs) ---------------------------

struct PbOut {
  std::string b;
  void varint(uint64_t v) {
    while (v >= 0x80) {
      b.push_back(char(v | 0x80));
      v >>= 7;
    }
    b.push_back(char(v));
  }
  void key(int field, int wire) { varint(uint64_t(field) << 3 | wire); }
  void u(int field, uint64_t v) { key(field, 0); varint(v); }
  void fixed32(int field, uint32_t v) {
    key(field, 5);
    for (int i = 0; i < 4; i++) b.push_back(char(v >> (8 * i)));
  }
  void fixed64(int field, uint64_t v) {
    key(field, 1);
    for (int i = 0; i < 8; i++) b.push_back(char(v >> (8 * i)));
  }
  void f32(int field, float v) {
    uint32_t x;
    memcpy(&x, &v, 4);
    fixed32(field, x);
  }
  void bytes(int field, const std::string &s) {
    key(field, 2);
    varint(s.size());
    b += s;
  }
};

struct PbField {
  int field = 0, wire = 0;
  uint64_t num = 0;      // varint, fixed32, fixed64
  std::string_view buf;  // length-delimited
};

// Reads one field; false at the end or on a malformed buffer.
bool pb_next(std::string_view &in, PbField &f) {
  auto varint = [&](uint64_t &v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (in.empty()) return false;
      uint8_t c = uint8_t(in[0]);
      in.remove_prefix(1);
      v |= uint64_t(c & 0x7f) << shift;
      if (!(c & 0x80)) return true;
    }
    return false;
  };
  auto fixed = [&](int n, uint64_t &v) {
    if (in.size() < size_t(n)) return false;
    v = 0;
    for (int i = 0; i < n; i++) v |= uint64_t(uint8_t(in[i])) << (8 * i);
    in.remove_prefix(n);
    return true;
  };
  if (in.empty()) return false;
  uint64_t k;
  if (!varint(k)) return false;
  f.field = int(k >> 3);
  f.wire = int(k & 7);
  switch (f.wire) {
    case 0: return varint(f.num);
    case 1: return fixed(8, f.num);
    case 5: return fixed(4, f.num);
    case 2: {
      uint64_t n;
      if (!varint(n) || n > in.size()) return false;
      f.buf = in.substr(0, n);
      in.remove_prefix(n);
      return true;
    }
    default: return false;
  }
}

float as_float(uint64_t raw) {
  uint32_t x = uint32_t(raw);
  float v;
  memcpy(&v, &x, 4);
  return v;
}

struct Suid {
  uint64_t low = 0, high = 0;
  bool operator==(const Suid &) const = default;
};

std::string pb_suid(const Suid &s) {
  PbOut p;
  p.fixed64(1, s.low);
  p.fixed64(2, s.high);
  return p.b;
}

Suid parse_suid(std::string_view in) {
  Suid s;
  PbField f;
  while (pb_next(in, f)) {
    if (f.field == 1 && f.wire == 1) s.low = f.num;
    if (f.field == 2 && f.wire == 1) s.high = f.num;
  }
  return s;
}

// sns_client_request_msg
std::string client_request(const Suid &suid, uint32_t msg_id, const std::string &payload) {
  PbOut susp;
  susp.u(1, kProcessorApss);
  susp.u(2, kDeliveryNoWakeup);
  PbOut req;
  if (!payload.empty()) req.bytes(2, payload);
  PbOut m;
  m.bytes(1, pb_suid(suid));
  m.fixed32(2, msg_id);
  m.bytes(3, susp.b);
  m.bytes(4, req.b);
  return m.b;
}

// ---- QRTR and QMI --------------------------------------------------------

struct Service {
  uint32_t node = 0, port = 0;
};

int qrtr_socket() {
  int fd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (fd < 0) logf("QRTR socket: %s", strerror(errno));
  return fd;
}

// Asks the name service to tell this socket about service 400 coming and
// going (NEW_SERVER / DEL_SERVER); one lookup for the daemon's life.
bool qrtr_lookup(int fd, uint32_t service) {
  sockaddr_qrtr me{};
  socklen_t len = sizeof(me);
  if (getsockname(fd, reinterpret_cast<sockaddr *>(&me), &len) < 0) return false;
  qrtr_ctrl_pkt pkt{};
  pkt.cmd = htole32(QRTR_TYPE_NEW_LOOKUP);
  pkt.server.service = htole32(service);
  sockaddr_qrtr ns{};
  ns.sq_family = AF_QIPCRTR;
  ns.sq_node = me.sq_node;
  ns.sq_port = QRTR_PORT_CTRL;
  return sendto(fd, &pkt, sizeof(pkt), 0, reinterpret_cast<sockaddr *>(&ns), sizeof(ns)) ==
         ssize_t(sizeof(pkt));
}

void put16(std::string &b, uint16_t v) {
  b.push_back(char(v));
  b.push_back(char(v >> 8));
}
uint16_t get16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint64_t getle(const uint8_t *p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v |= uint64_t(p[i]) << (8 * i);
  return v;
}

struct QmiTlvs {
  std::vector<std::pair<uint8_t, std::string_view>> tlvs;
  std::optional<std::string_view> get(uint8_t type) const {
    for (auto &[t, v] : tlvs)
      if (t == type) return v;
    return std::nullopt;
  }
};

bool parse_tlvs(std::string_view in, QmiTlvs &out) {
  while (in.size() >= 3) {
    auto p = reinterpret_cast<const uint8_t *>(in.data());
    uint16_t n = get16(p + 1);
    if (in.size() < 3u + n) return false;
    out.tlvs.emplace_back(p[0], in.substr(3, n));
    in.remove_prefix(3 + n);
  }
  return in.empty();
}

// One client of the sensor hub: one QRTR socket, one sensor stream.
class SscClient {
 public:
  ~SscClient() { close_(); }
  int fd() const { return fd_; }

  bool open(const Service &svc) {
    close_();
    fd_ = qrtr_socket();
    svc_ = svc;
    return fd_ >= 0;
  }

  void close_() {
    if (fd_ >= 0) {
      if (streaming_) send_request(client_request(sensor_, kMsgClientDisable, {}));
      ::close(fd_);
    }
    fd_ = -1;
    streaming_ = false;
  }

  bool send_request(const std::string &pb) {
    std::string tlv;
    tlv.push_back(char(0x01));
    put16(tlv, uint16_t(pb.size() + 2));
    put16(tlv, uint16_t(pb.size()));
    tlv += pb;
    std::string msg;
    msg.push_back(char(kQmiRequest));
    put16(msg, ++txn_);
    put16(msg, kQmiSnsClientReq);
    put16(msg, uint16_t(tlv.size()));
    msg += tlv;
    sockaddr_qrtr to{};
    to.sq_family = AF_QIPCRTR;
    to.sq_node = svc_.node;
    to.sq_port = svc_.port;
    if (sendto(fd_, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr *>(&to), sizeof(to)) < 0) {
      logf("QMI send: %s", strerror(errno));
      return false;
    }
    return true;
  }

  // Reads one datagram. Responses are checked here; a report's protobuf
  // payload (sns_client_event_msg) is handed back.
  enum class Got { Nothing, Report, Error };
  Got read(std::string &report) {
    uint8_t buf[65536];
    sockaddr_qrtr from{};
    socklen_t flen = sizeof(from);
    ssize_t n = recvfrom(fd_, buf, sizeof(buf), 0, reinterpret_cast<sockaddr *>(&from), &flen);
    if (n < 0) return errno == EAGAIN ? Got::Nothing : Got::Error;
    if (n < 7) return Got::Nothing;
    uint8_t type = buf[0];
    uint16_t msg_id = get16(buf + 3), len = get16(buf + 5);
    if (7u + len > size_t(n)) return Got::Nothing;
    QmiTlvs t;
    if (!parse_tlvs({reinterpret_cast<char *>(buf + 7), len}, t)) return Got::Nothing;
    if (type == kQmiResponse && msg_id == kQmiSnsClientReq) {
      auto r = t.get(0x02);
      if (r && r->size() >= 4) {
        auto p = reinterpret_cast<const uint8_t *>(r->data());
        if (get16(p) != 0) {
          logf("sensor hub refused a request (error %u)", get16(p + 2));
          return Got::Error;
        }
      }
      return Got::Nothing;
    }
    if (type == kQmiIndication &&
        (msg_id == kQmiSnsClientReport || msg_id == kQmiSnsClientJumboReport)) {
      auto d = t.get(0x02);
      if (!d || d->size() < 2) return Got::Nothing;
      uint16_t plen = get16(reinterpret_cast<const uint8_t *>(d->data()));
      if (plen + 2u > d->size()) return Got::Nothing;
      report.assign(d->data() + 2, plen);
      return Got::Report;
    }
    return Got::Nothing;
  }

  // Looks up the default sensor of a data type ("accel", "gyro"), then
  // starts it at `rate` Hz. Waits at most `timeout_ms` for the lookup.
  bool start(const char *data_type, float rate, int timeout_ms) {
    PbOut q;
    q.bytes(1, data_type);
    q.u(2, 1);  // register_updates
    q.u(3, 1);  // default_only
    if (!send_request(client_request({kSuidLookup, kSuidLookup}, kMsgSuidReq, q.b))) return false;
    timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
      timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      int left = timeout_ms - int((now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000);
      if (left <= 0) {
        logf("no %s sensor answered", data_type);
        return false;
      }
      pollfd p{fd_, POLLIN, 0};
      if (poll(&p, 1, left) <= 0) continue;
      std::string report;
      Got g;
      while ((g = read(report)) != Got::Nothing) {
        if (g == Got::Error) return false;
        auto suid = suid_from_lookup(report, data_type);
        if (!suid) continue;
        sensor_ = *suid;
        PbOut cfg;
        cfg.f32(1, rate);
        if (!send_request(client_request(sensor_, kMsgStdSensorConfig, cfg.b))) return false;
        streaming_ = true;
        logf("%s: sensor %016llx%016llx at %.0f Hz", data_type, (unsigned long long)sensor_.high,
             (unsigned long long)sensor_.low, rate);
        return true;
      }
    }
  }

  // Calls `on_sample(x, y, z)` for each std sensor event in a report.
  template <typename F>
  static void samples(std::string_view report, F &&on_sample) {
    PbField f;
    while (pb_next(report, f)) {
      if (f.field != 2 || f.wire != 2) continue;
      std::string_view ev = f.buf;
      uint32_t msg_id = 0;
      std::string_view payload;
      PbField e;
      while (pb_next(ev, e)) {
        if (e.field == 1 && e.wire == 5) msg_id = uint32_t(e.num);
        if (e.field == 3 && e.wire == 2) payload = e.buf;
      }
      if (msg_id != kMsgStdSensorEvent) continue;
      float v[3];
      int n = 0;
      PbField d;
      while (pb_next(payload, d)) {
        if (d.field != 1) continue;
        if (d.wire == 5 && n < 3) v[n++] = as_float(d.num);
        if (d.wire == 2)  // packed
          for (size_t i = 0; i + 4 <= d.buf.size() && n < 3; i += 4)
            v[n++] = as_float(getle(reinterpret_cast<const uint8_t *>(d.buf.data() + i), 4));
      }
      if (n == 3) on_sample(v[0], v[1], v[2]);
    }
  }

 private:
  static std::optional<Suid> suid_from_lookup(std::string_view report, const char *data_type) {
    PbField f;
    while (pb_next(report, f)) {
      if (f.field != 2 || f.wire != 2) continue;
      std::string_view ev = f.buf;
      uint32_t msg_id = 0;
      std::string_view payload;
      PbField e;
      while (pb_next(ev, e)) {
        if (e.field == 1 && e.wire == 5) msg_id = uint32_t(e.num);
        if (e.field == 3 && e.wire == 2) payload = e.buf;
      }
      if (msg_id != kMsgSuidEvent) continue;
      std::string_view type;
      std::optional<Suid> first;
      PbField s;
      while (pb_next(payload, s)) {
        if (s.field == 1 && s.wire == 2) type = s.buf;
        if (s.field == 2 && s.wire == 2 && !first) first = parse_suid(s.buf);
      }
      if (type == data_type && first) return first;
    }
    return std::nullopt;
  }

  int fd_ = -1;
  Service svc_;
  uint16_t txn_ = 0;
  Suid sensor_;
  bool streaming_ = false;
};

// ---- gyro zero -----------------------------------------------------------

// Learns the gyro's zero (deg/s, Deck frame) from half-second windows in
// which the device lies still, and keeps it across restarts. Still means
// both: the gyro steady (a slow, steady turn is steady too) and gravity
// not moving, which a turn of any speed would show.
class GyroZero {
 public:
  explicit GyroZero(std::string path) : path_(std::move(path)) {
    std::ifstream f(path_);
    float b[3];
    if (f >> b[0] >> b[1] >> b[2] && std::isfinite(b[0] + b[1] + b[2]))
      for (int i = 0; i < 3; i++) bias_[i] = b[i];
  }
  const float *bias() const { return bias_; }

  // w: gyro (deg/s), a: accelerometer (g), both Deck frame.
  void add(const float w[3], const float a[3], int rate) {
    for (int i = 0; i < 3; i++) {
      if (std::fabs(w[i]) > 6.0f) return reset();
      sum_[i] += w[i];
      sq_[i] += double(w[i]) * w[i];
      if (n_ == 0 || a[i] < amin_[i]) amin_[i] = a[i];
      if (n_ == 0 || a[i] > amax_[i]) amax_[i] = a[i];
      if (amax_[i] - amin_[i] > 0.03f) return reset();  // noise ~0.02 g; 3° of turn ~0.05 g
    }
    if (++n_ < rate / 2) return;
    float mean[3];
    for (int i = 0; i < 3; i++) {
      mean[i] = float(sum_[i] / n_);
      double var = sq_[i] / n_ - double(mean[i]) * mean[i];
      if (var > 0.25 * 0.25) return reset();
    }
    for (int i = 0; i < 3; i++) bias_[i] = learned_ ? 0.8f * bias_[i] + 0.2f * mean[i] : mean[i];
    learned_ = true;
    reset();
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - saved_ >= 60) {
      saved_ = now.tv_sec;
      std::ofstream f(path_ + ".new");
      f << bias_[0] << ' ' << bias_[1] << ' ' << bias_[2] << '\n';
      if (f.good()) rename((path_ + ".new").c_str(), path_.c_str());
    }
  }

 private:
  void reset() {
    n_ = 0;
    for (int i = 0; i < 3; i++) sum_[i] = sq_[i] = 0;
  }
  std::string path_;
  float bias_[3] = {0, 0, 0};
  double sum_[3] = {0, 0, 0}, sq_[3] = {0, 0, 0};
  float amin_[3] = {0, 0, 0}, amax_[3] = {0, 0, 0};
  int n_ = 0;
  bool learned_ = false;
  time_t saved_ = -3600;
};

// ---- uinput --------------------------------------------------------------

class MotionDevice {
 public:
  ~MotionDevice() {
    if (fd_ >= 0) {
      ioctl(fd_, UI_DEV_DESTROY);
      close(fd_);
    }
  }
  bool create() {
    fd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0) {
      logf("/dev/uinput: %s", strerror(errno));
      return false;
    }
    ioctl(fd_, UI_SET_EVBIT, EV_ABS);
    ioctl(fd_, UI_SET_EVBIT, EV_MSC);
    ioctl(fd_, UI_SET_MSCBIT, MSC_TIMESTAMP);
    ioctl(fd_, UI_SET_PROPBIT, INPUT_PROP_ACCELEROMETER);
    auto axis = [&](int code, int range, int res) {
      uinput_abs_setup a{};
      a.code = uint16_t(code);
      a.absinfo.minimum = -range;
      a.absinfo.maximum = range;
      a.absinfo.resolution = res;
      ioctl(fd_, UI_SET_ABSBIT, code);
      return ioctl(fd_, UI_ABS_SETUP, &a) == 0;
    };
    bool ok = axis(ABS_X, kAccelRange, kAccelResPerG) && axis(ABS_Y, kAccelRange, kAccelResPerG) &&
              axis(ABS_Z, kAccelRange, kAccelResPerG) && axis(ABS_RX, kGyroRange, kGyroResPerDps) &&
              axis(ABS_RY, kGyroRange, kGyroResPerDps) && axis(ABS_RZ, kGyroRange, kGyroResPerDps);
    uinput_setup s{};
    s.id.bustype = BUS_HOST;
    snprintf(s.name, sizeof(s.name), "PB-OS Motion Sensors");
    if (!ok || ioctl(fd_, UI_DEV_SETUP, &s) < 0 || ioctl(fd_, UI_DEV_CREATE) < 0) {
      logf("uinput device: %s", strerror(errno));
      return false;
    }
    return true;
  }
  // accel in g, gyro in deg/s, both Deck frame.
  void emit(const float a[3], const float w[3], uint32_t usec) {
    input_event ev[8]{};
    auto clamp = [](float v, int r) { return int(std::lround(std::fmax(-r, std::fmin(r, v)))); };
    int codes[6] = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ};
    for (int i = 0; i < 6; i++) {
      ev[i].type = EV_ABS;
      ev[i].code = uint16_t(codes[i]);
      ev[i].value = i < 3 ? clamp(a[i] * kAccelResPerG, kAccelRange) : clamp(w[i - 3] * kGyroResPerDps, kGyroRange);
    }
    ev[6].type = EV_MSC;
    ev[6].code = MSC_TIMESTAMP;
    ev[6].value = int(usec);
    ev[7].type = EV_SYN;
    ev[7].code = SYN_REPORT;
    if (write(fd_, ev, sizeof(ev)) < 0 && errno != EAGAIN) logf("uinput write: %s", strerror(errno));
  }

 private:
  int fd_ = -1;
};

uint32_t usec_now() {
  timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint32_t(uint64_t(t.tv_sec) * 1000000 + t.tv_nsec / 1000);
}

// logind's PrepareForSleep, with a delay lock so the sensors are off before
// the suspend goes on.
class SleepWatch {
 public:
  ~SleepWatch() {
    release();
    if (bus_) sd_bus_flush_close_unref(bus_);
  }
  bool open() {
    int r = sd_bus_open_system(&bus_);
    if (r < 0) {
      logf("system bus: %s (sensors stay on in sleep)", strerror(-r));
      return false;
    }
    r = sd_bus_match_signal(bus_, nullptr, "org.freedesktop.login1", "/org/freedesktop/login1",
                            "org.freedesktop.login1.Manager", "PrepareForSleep", on_signal, this);
    if (r < 0) {
      logf("PrepareForSleep: %s (sensors stay on in sleep)", strerror(-r));
      return false;
    }
    take();
    return true;
  }
  int fd() const { return bus_ ? sd_bus_get_fd(bus_) : -1; }
  short events() const {
    int e = bus_ ? sd_bus_get_events(bus_) : 0;
    return short(e > 0 ? e : POLLIN);
  }
  // Handles what the bus has; true when the sleep state changed.
  bool process() {
    changed_ = false;
    while (bus_ && sd_bus_process(bus_, nullptr) > 0) {
    }
    return changed_;
  }
  bool asleep() const { return asleep_; }
  void take() {
    if (lock_ >= 0 || !bus_) return;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = nullptr;
    int r = sd_bus_call_method(bus_, "org.freedesktop.login1", "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager", "Inhibit", &err, &reply, "ssss", "sleep",
                               "pbos-motiond", "Turning the motion sensors off", "delay");
    int fd = -1;
    if (r >= 0 && sd_bus_message_read(reply, "h", &fd) >= 0) lock_ = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    if (lock_ < 0) logf("sleep delay lock: %s", err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    sd_bus_message_unref(reply);
  }
  void release() {
    if (lock_ >= 0) close(lock_);
    lock_ = -1;
  }

 private:
  static int on_signal(sd_bus_message *m, void *data, sd_bus_error *) {
    auto *self = static_cast<SleepWatch *>(data);
    int start = 0;
    if (sd_bus_message_read(m, "b", &start) >= 0) {
      self->asleep_ = start;
      self->changed_ = true;
    }
    return 0;
  }
  sd_bus *bus_ = nullptr;
  int lock_ = -1;
  bool asleep_ = false, changed_ = false;
};

}  // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--dump")) g_dump = true;
    else {
      fprintf(stderr, "usage: pbos-motiond [--dump]\n");
      return 2;
    }
  }
  setvbuf(stderr, nullptr, _IOLBF, 0);
  auto profile = detect_profile();
  if (!profile) {
    logf("no motion sensor profile for this device");
    return 0;
  }
  constexpr int kRate = 200;
  GyroZero zero(std::string("/var/lib/sm8550-sensors/") + profile->id + "/gyro-zero");

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGINT);
  sigprocmask(SIG_BLOCK, &mask, nullptr);
  int sfd = signalfd(-1, &mask, SFD_CLOEXEC);

  MotionDevice dev;
  if (!g_dump && !dev.create()) return 1;

  int ns = qrtr_socket();
  if (ns < 0 || !qrtr_lookup(ns, kSnsClientService)) {
    logf("QRTR name service lookup failed");
    return 1;
  }
  logf("%s: waiting for the sensor hub", profile->id);

  SscClient accel, gyro;
  bool up = false, hub = false;
  Service svc;
  auto start_streams = [&] {
    up = accel.open(svc) && gyro.open(svc) && accel.start("accel", kRate, 3000) &&
         gyro.start("gyro", kRate, 3000);
    if (!up) {
      // The hub can list the service before its sensors are ready; it is
      // offered again when the sensors process restarts.
      accel.close_();
      gyro.close_();
    }
  };
  auto stop_streams = [&] {
    accel.close_();
    gyro.close_();
    up = false;
  };
  SleepWatch sleep;
  if (!g_dump) sleep.open();
  float a[3] = {0, 0, 1}, w[3] = {0, 0, 0}, raw_a[3] = {0, 0, 0}, raw_w[3] = {0, 0, 0};
  auto rotate = [&](float x, float y, float z, float out[3], float scale) {
    float in[3] = {x, y, z};
    for (int i = 0; i < 3; i++)
      out[i] = scale * (profile->rot[i][0] * in[0] + profile->rot[i][1] * in[1] + profile->rot[i][2] * in[2]);
  };
  unsigned dumped = 0;

  for (;;) {
    pollfd p[5] = {{sfd, POLLIN, 0},
                   {ns, POLLIN, 0},
                   {accel.fd(), POLLIN, 0},
                   {gyro.fd(), POLLIN, 0},
                   {sleep.fd(), sleep.events(), 0}};
    if (poll(p, 5, -1) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (p[0].revents) break;

    if (p[4].revents && sleep.process()) {
      if (sleep.asleep()) {
        if (up) logf("suspending: motion sensors off");
        stop_streams();
        sleep.release();
      } else {
        sleep.take();
        if (hub) {
          logf("resumed: motion sensors on");
          start_streams();
        }
      }
    }

    if (p[1].revents & POLLIN) {
      qrtr_ctrl_pkt pkt{};
      while (recv(ns, &pkt, sizeof(pkt), 0) == ssize_t(sizeof(pkt))) {
        uint32_t cmd = le32toh(pkt.cmd);
        if (le32toh(pkt.server.service) != kSnsClientService) continue;
        Service s{le32toh(pkt.server.node), le32toh(pkt.server.port)};
        if (cmd == QRTR_TYPE_NEW_SERVER && !up) {
          svc = s;
          hub = true;
          if (!sleep.asleep()) start_streams();
        } else if (cmd == QRTR_TYPE_DEL_SERVER && hub && s.node == svc.node && s.port == svc.port) {
          logf("sensor hub went away");
          stop_streams();
          hub = false;
        }
      }
    }

    std::string report;
    if (p[2].revents & POLLIN)
      while (accel.read(report) == SscClient::Got::Report)
        SscClient::samples(report, [&](float x, float y, float z) {
          raw_a[0] = x / kGravity, raw_a[1] = y / kGravity, raw_a[2] = z / kGravity;
          rotate(x, y, z, a, 1.0f / kGravity);
        });
    if (p[3].revents & POLLIN)
      while (gyro.read(report) == SscClient::Got::Report)
        SscClient::samples(report, [&](float x, float y, float z) {
          float raw[3];
          raw_w[0] = x * kRadToDeg, raw_w[1] = y * kRadToDeg, raw_w[2] = z * kRadToDeg;
          rotate(x, y, z, raw, kRadToDeg);
          zero.add(raw, a, kRate);
          for (int i = 0; i < 3; i++) w[i] = raw[i] - zero.bias()[i];
          if (g_dump) {
            if (dumped++ % (kRate / 10) == 0)
              printf("raw  accel %+6.3f %+6.3f %+6.3f g  gyro %+8.2f %+8.2f %+8.2f deg/s\n"
                     "deck accel %+6.3f %+6.3f %+6.3f g  gyro %+8.2f %+8.2f %+8.2f deg/s\n",
                     raw_a[0], raw_a[1], raw_a[2], raw_w[0], raw_w[1], raw_w[2], a[0], a[1], a[2], w[0],
                     w[1], w[2]);
            fflush(stdout);
          } else {
            dev.emit(a, w, usec_now());
          }
        });
  }
  logf("stopping");
  return 0;
}

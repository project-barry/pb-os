#include "cdp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "util.h"

namespace pq {

namespace {

double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// The titles Steam's shared page has had (Decky Loader's SHARED_CTX_NAMES).
const char* const kSharedTitles[] = {"SharedJSContext", "Steam Shared Context presented by Valve™", "Steam", "SP"};

}  // namespace

bool WebSocket::connect(int port, const std::string& path, double timeout) {
    close();
    double deadline = now_s() + timeout;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 && errno != EINPROGRESS) {
        ::close(fd);
        return false;
    }
    pollfd p = {fd, POLLOUT, 0};
    int err = 0;
    socklen_t elen = sizeof err;
    if (poll(&p, 1, static_cast<int>(timeout * 1000)) <= 0 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err) {
        ::close(fd);
        return false;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    unsigned char key[16];
    if (getrandom(key, sizeof key, 0) != sizeof key) {
        ::close(fd);
        return false;
    }
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
                      base64(std::string_view(reinterpret_cast<const char*>(key), sizeof key)) + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    size_t done = 0;
    while (done < req.size()) {
        ssize_t n = send(fd, req.data() + done, req.size() - done, MSG_NOSIGNAL);
        if (n > 0) {
            done += static_cast<size_t>(n);
        } else if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            pollfd w = {fd, POLLOUT, 0};
            if (poll(&w, 1, 1000) <= 0) {
                ::close(fd);
                return false;
            }
        } else {
            ::close(fd);
            return false;
        }
    }
    std::string head;
    while (head.find("\r\n\r\n") == std::string::npos) {
        double left = deadline - now_s();
        pollfd r = {fd, POLLIN, 0};
        if (left <= 0 || poll(&r, 1, static_cast<int>(left * 1000) + 1) <= 0) {
            ::close(fd);
            return false;
        }
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof buf, 0);
        if (n <= 0 && !(n < 0 && (errno == EAGAIN || errno == EINTR))) {
            ::close(fd);
            return false;
        }
        if (n > 0) head.append(buf, static_cast<size_t>(n));
    }
    auto end = head.find("\r\n\r\n");
    if (head.compare(0, 12, "HTTP/1.1 101") != 0) {
        ::close(fd);
        return false;
    }
    fd_ = fd;
    in_ = head.substr(end + 4);
    partial_.clear();
    return true;
}

void WebSocket::close() {
    std::lock_guard<std::mutex> g(send_lock_);
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    in_.clear();
    partial_.clear();
}

bool WebSocket::send_frame(int opcode, const std::string& payload) {
    std::lock_guard<std::mutex> g(send_lock_);
    if (fd_ < 0) return false;
    std::string frame;
    frame += static_cast<char>(0x80 | opcode);
    size_t n = payload.size();
    if (n < 126) {
        frame += static_cast<char>(0x80 | n);
    } else if (n < 65536) {
        frame += static_cast<char>(0x80 | 126);
        frame += static_cast<char>((n >> 8) & 0xff);
        frame += static_cast<char>(n & 0xff);
    } else {
        frame += static_cast<char>(0x80 | 127);
        for (int i = 7; i >= 0; --i) frame += static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xff);
    }
    unsigned char mask[4];
    if (getrandom(mask, sizeof mask, 0) != sizeof mask) return false;
    frame.append(reinterpret_cast<char*>(mask), 4);
    size_t at = frame.size();
    frame += payload;
    for (size_t i = 0; i < n; ++i) frame[at + i] = static_cast<char>(frame[at + i] ^ mask[i & 3]);
    size_t done = 0;
    while (done < frame.size()) {
        ssize_t w = send(fd_, frame.data() + done, frame.size() - done, MSG_NOSIGNAL);
        if (w > 0) {
            done += static_cast<size_t>(w);
        } else if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            pollfd p = {fd_, POLLOUT, 0};
            if (poll(&p, 1, 5000) <= 0) return false;
        } else {
            return false;
        }
    }
    return true;
}

bool WebSocket::send_text(const std::string& text) { return send_frame(0x1, text); }

bool WebSocket::fill(int timeout_ms, int wake_fd, Read& why) {
    pollfd p[2] = {{fd_, POLLIN, 0}, {wake_fd, POLLIN, 0}};
    int r = poll(p, wake_fd >= 0 ? 2 : 1, timeout_ms);
    if (r < 0 && errno == EINTR) {
        why = Read::Timeout;
        return false;
    }
    if (r == 0) {
        why = Read::Timeout;
        return false;
    }
    if (r < 0) {
        why = Read::Closed;
        return false;
    }
    if (wake_fd >= 0 && (p[1].revents & POLLIN)) {
        why = Read::Woken;
        return false;
    }
    char buf[65536];
    ssize_t n = recv(fd_, buf, sizeof buf, 0);
    if (n > 0) {
        in_.append(buf, static_cast<size_t>(n));
        return true;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return true;
    why = Read::Closed;
    return false;
}

WebSocket::Read WebSocket::read(std::string& msg, int timeout_ms, int wake_fd) {
    if (fd_ < 0) return Read::Closed;
    double deadline = timeout_ms < 0 ? 0 : now_s() + timeout_ms / 1000.0;
    while (true) {
        // A whole frame in in_?
        if (in_.size() >= 2) {
            auto b0 = static_cast<unsigned char>(in_[0]);
            auto b1 = static_cast<unsigned char>(in_[1]);
            bool fin = b0 & 0x80;
            int op = b0 & 0x0f;
            bool masked = b1 & 0x80;
            uint64_t len = b1 & 0x7f;
            size_t at = 2;
            bool have = true;
            if (len == 126) {
                if (in_.size() < 4) have = false;
                else {
                    len = (static_cast<unsigned char>(in_[2]) << 8) | static_cast<unsigned char>(in_[3]);
                    at = 4;
                }
            } else if (len == 127) {
                if (in_.size() < 10) have = false;
                else {
                    len = 0;
                    for (int i = 0; i < 8; ++i) len = (len << 8) | static_cast<unsigned char>(in_[2 + i]);
                    at = 10;
                }
            }
            if (have && masked) at += 4;
            if (have && in_.size() >= at + len) {
                std::string payload = in_.substr(at, len);
                if (masked) {
                    const char* m = in_.data() + at - 4;
                    for (size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ m[i & 3]);
                }
                in_.erase(0, at + len);
                if (op == 0x8) {
                    close();
                    return Read::Closed;
                }
                if (op == 0x9) {
                    send_frame(0xA, payload);
                    continue;
                }
                if (op == 0xA) continue;
                if (op == 0x0 || op == 0x1 || op == 0x2) {
                    partial_ += payload;
                    if (fin) {
                        msg.swap(partial_);
                        partial_.clear();
                        return Read::Message;
                    }
                }
                continue;
            }
        }
        int wait = -1;
        if (timeout_ms >= 0) {
            double left = deadline - now_s();
            if (left <= 0) return Read::Timeout;
            wait = static_cast<int>(left * 1000) + 1;
        }
        Read why = Read::Timeout;
        if (!fill(wait, wake_fd, why)) {
            if (why == Read::Closed) close();
            if (why == Read::Timeout && timeout_ms < 0) continue;
            return why;
        }
    }
}

std::optional<CefPage> find_gamepadui_page(double timeout) {
    auto r = http("GET", kCefPort, "/json", nullptr, timeout);
    if (r.status != 200) return std::nullopt;
    auto list = pj::Value::parse(r.body);
    if (!list || !list->is_array()) return std::nullopt;
    for (const auto& t : list->items()) {
        const auto& url = t["url"].as_string();
        const auto& title = t["title"].as_string();
        bool gamepadui = url.find("https://steamloopback.host/routes/") != std::string::npos ||
                         url.find("https://steamloopback.host/index.html") != std::string::npos;
        bool shared = false;
        for (const char* name : kSharedTitles) shared = shared || title == name;
        if (!gamepadui || !shared) continue;
        std::string ws = t["webSocketDebuggerUrl"].as_string();
        // ws://127.0.0.1:8080/devtools/page/<id> -> the path.
        auto slash = ws.find('/', ws.find("://") == std::string::npos ? 0 : ws.find("://") + 3);
        if (slash == std::string::npos) continue;
        return CefPage{title, url, ws.substr(slash)};
    }
    return std::nullopt;
}

}  // namespace pq

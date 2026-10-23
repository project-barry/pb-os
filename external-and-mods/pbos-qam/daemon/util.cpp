#include "util.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <glob.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

extern char** environ;

namespace pq {

namespace {

std::mutex log_lock;

double now_s() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void set_cloexec_nonblock(int fd) {
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

}  // namespace

void log(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> g(log_lock);
    std::fprintf(stderr, "%s\n", buf);
    std::fflush(stderr);
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return std::string(s.substr(a, b - a));
}

std::vector<std::string> split_lines(std::string_view s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n') {
            std::string_view line = s.substr(start, i - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (i < s.size() || !line.empty()) out.emplace_back(line);
            start = i + 1;
        }
    }
    return out;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

std::string base64(std::string_view in) {
    const auto* data = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < n) v |= data[i + 1] << 8;
        if (i + 2 < n) v |= data[i + 2];
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < n ? t[(v >> 6) & 63] : '=';
        out += i + 2 < n ? t[v & 63] : '=';
    }
    return out;
}

std::optional<std::string> read_file(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    std::string out;
    char buf[65536];
    while (true) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return std::nullopt;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    return out;
}

std::string rd(const std::string& path, std::string_view def) {
    auto s = read_file(path);
    return s ? trim(*s) : std::string(def);
}

bool exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

bool is_dir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool is_link(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

bool can_exec(const std::string& path) { return access(path.c_str(), X_OK) == 0; }
bool can_read(const std::string& path) { return access(path.c_str(), R_OK) == 0; }

std::string dirname(const std::string& path) {
    auto at = path.find_last_of('/');
    if (at == std::string::npos) return "";
    if (at == 0) return "/";
    return path.substr(0, at);
}

std::string basename(const std::string& path) {
    auto at = path.find_last_of('/');
    return at == std::string::npos ? path : path.substr(at + 1);
}

std::string join(const std::string& a, const std::string& b) {
    if (b.empty()) return a;
    if (b[0] == '/' || a.empty()) return b;
    return a.back() == '/' ? a + b : a + "/" + b;
}

std::string realpath(const std::string& path) {
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf)) return buf;
    return path;
}

pj::Value read_json(const std::string& path) {
    auto text = read_file(path);
    if (!text) return pj::Value::object();
    auto v = pj::Value::parse(*text);
    if (!v || !v->is_object()) return pj::Value::object();
    return *v;
}

bool makedirs(const std::string& path, mode_t mode) {
    if (path.empty() || is_dir(path)) return true;
    std::string parent = dirname(path);
    if (!parent.empty() && parent != path && !makedirs(parent, mode)) return false;
    return mkdir(path.c_str(), mode) == 0 || errno == EEXIST;
}

bool write_text(const std::string& path, const std::string& text) {
    makedirs(dirname(path));
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    size_t done = 0;
    while (done < text.size()) {
        ssize_t n = write(fd, text.data() + done, text.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp.c_str());
            return false;
        }
        done += static_cast<size_t>(n);
    }
    close(fd);
    return rename(tmp.c_str(), path.c_str()) == 0;
}

bool write_sysfs(const std::string& path, std::string_view value) {
    int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n;
    do n = write(fd, value.data(), value.size());
    while (n < 0 && errno == EINTR);
    close(fd);
    return n == static_cast<ssize_t>(value.size());
}

bool write_json(const std::string& path, const pj::Value& v, int indent) {
    return write_text(path, v.dump(indent));
}

bool remove_file(const std::string& path) { return unlink(path.c_str()) == 0 || errno == ENOENT; }

bool remove_tree(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
    if (!S_ISDIR(st.st_mode)) return unlink(path.c_str()) == 0;
    auto fn = [](const char* p, const struct stat*, int, struct FTW*) -> int { return remove(p); };
    return nftw(path.c_str(), fn, 32, FTW_DEPTH | FTW_PHYS) == 0;
}

std::vector<std::string> listdir(const std::string& path) {
    std::vector<std::string> out;
    DIR* d = opendir(path.c_str());
    if (!d) return out;
    while (auto* e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        out.emplace_back(e->d_name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> glob(const std::string& pattern) {
    std::vector<std::string> out;
    glob_t g{};
    if (::glob(pattern.c_str(), 0, nullptr, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; ++i) out.emplace_back(g.gl_pathv[i]);
    }
    globfree(&g);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> clean_env(std::vector<std::string> extra) {
    std::vector<std::string> env = {"PATH=/usr/bin:/usr/sbin", "LANG=C.UTF-8"};
    for (auto& e : extra) {
        auto key = e.substr(0, e.find('=') + 1);
        env.erase(std::remove_if(env.begin(), env.end(), [&](const std::string& x) { return x.rfind(key, 0) == 0; }),
                  env.end());
        env.push_back(std::move(e));
    }
    return env;
}

RunResult run(const std::vector<std::string>& argv, const RunOpts& opts) {
    RunResult res;
    if (argv.empty()) return res;
    int in_pipe[2] = {-1, -1}, out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    if (opts.input && pipe2(in_pipe, O_CLOEXEC) != 0) return res;
    if (opts.capture && (pipe2(out_pipe, O_CLOEXEC) != 0 || pipe2(err_pipe, O_CLOEXEC) != 0)) return res;

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (opts.input) {
        posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
    } else {
        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    }
    if (opts.capture) {
        posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
        posix_spawn_file_actions_adddup2(&fa, err_pipe[1], 2);
    } else {
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    }
    // The daemon blocks its signals for signalfd; the child gets a clean set.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    std::vector<char*> envp;
    char** env = environ;
    if (!opts.env.empty()) {
        for (auto& e : opts.env) envp.push_back(const_cast<char*>(e.c_str()));
        envp.push_back(nullptr);
        env = envp.data();
    }
    pid_t pid = -1;
    int rc = posix_spawnp(&pid, argv[0].c_str(), &fa, &attr, args.data(), env);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    for (int fd : {in_pipe[0], out_pipe[1], err_pipe[1]})
        if (fd >= 0) close(fd);
    if (rc != 0) {
        for (int fd : {in_pipe[1], out_pipe[0], err_pipe[0]})
            if (fd >= 0) close(fd);
        res.err = std::string("cannot run ") + argv[0] + ": " + std::strerror(rc);
        return res;
    }
    res.started = true;

    size_t in_done = 0;
    int in_fd = in_pipe[1], out_fd = out_pipe[0], err_fd = err_pipe[0];
    for (int fd : {in_fd, out_fd, err_fd})
        if (fd >= 0) set_cloexec_nonblock(fd);
    if (in_fd >= 0 && opts.input->empty()) {
        close(in_fd);
        in_fd = -1;
    }
    double deadline = now_s() + opts.timeout;
    while (in_fd >= 0 || out_fd >= 0 || err_fd >= 0) {
        pollfd fds[3];
        int n = 0;
        if (in_fd >= 0) fds[n++] = {in_fd, POLLOUT, 0};
        if (out_fd >= 0) fds[n++] = {out_fd, POLLIN, 0};
        if (err_fd >= 0) fds[n++] = {err_fd, POLLIN, 0};
        double left = deadline - now_s();
        if (left <= 0) {
            res.timed_out = true;
            break;
        }
        int r = poll(fds, static_cast<nfds_t>(n), static_cast<int>(left * 1000) + 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i) {
            if (!fds[i].revents) continue;
            int fd = fds[i].fd;
            if (fd == in_fd) {
                ssize_t w = write(fd, opts.input->data() + in_done, opts.input->size() - in_done);
                if (w > 0) in_done += static_cast<size_t>(w);
                if (w < 0 && errno != EAGAIN && errno != EINTR) in_done = opts.input->size();
                if (in_done >= opts.input->size()) {
                    close(in_fd);
                    in_fd = -1;
                }
                continue;
            }
            char buf[16384];
            ssize_t got = read(fd, buf, sizeof buf);
            if (got > 0) {
                (fd == out_fd ? res.out : res.err).append(buf, static_cast<size_t>(got));
            } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
                close(fd);
                if (fd == out_fd) out_fd = -1;
                else err_fd = -1;
            }
        }
    }
    for (int fd : {in_fd, out_fd, err_fd})
        if (fd >= 0) close(fd);
    int status = 0;
    if (res.timed_out) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        res.code = -SIGKILL;
        return res;
    }
    // Pipes closed: the program is (nearly) done; wait for it, within the time.
    while (true) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0 && errno != EINTR) break;
        if (now_s() > deadline) {
            res.timed_out = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            res.code = -SIGKILL;
            return res;
        }
        struct timespec ts = {0, 5 * 1000 * 1000};
        nanosleep(&ts, nullptr);
    }
    if (WIFEXITED(status)) res.code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) res.code = -WTERMSIG(status);
    return res;
}

void spawn(const std::vector<std::string>& argv) {
    std::thread([argv] {
        RunOpts o;
        o.capture = false;
        o.timeout = 3600;
        run(argv, o);
    }).detach();
}

bool LineChild::start(const std::vector<std::string>& argv, std::function<void(const std::string&)> on_line) {
    stop(0);
    int out[2];
    if (pipe2(out, O_CLOEXEC) != 0) return false;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    std::vector<char*> args;
    for (auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    int rc = posix_spawnp(&pid_, argv[0].c_str(), &fa, &attr, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(out[1]);
    if (rc != 0) {
        close(out[0]);
        pid_ = -1;
        return false;
    }
    int fd = out[0];
    reader_ = std::thread([fd, on_line = std::move(on_line)] {
        std::string buf;
        char tmp[4096];
        while (true) {
            ssize_t n = read(fd, tmp, sizeof tmp);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            buf.append(tmp, static_cast<size_t>(n));
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                on_line(buf.substr(0, nl));
                buf.erase(0, nl + 1);
            }
        }
        if (!buf.empty()) on_line(buf);
        close(fd);
    });
    return true;
}

bool LineChild::running() const {
    return pid_ > 0 && waitpid(pid_, nullptr, WNOHANG) == 0;
}

void LineChild::stop(double wait) {
    if (pid_ > 0) {
        int status;
        if (waitpid(pid_, &status, WNOHANG) == 0) {
            kill(pid_, SIGTERM);
            double until = now_s() + wait;
            while (waitpid(pid_, &status, WNOHANG) == 0) {
                if (now_s() >= until) {
                    kill(pid_, SIGKILL);
                    waitpid(pid_, &status, 0);
                    break;
                }
                struct timespec ts = {0, 20 * 1000 * 1000};
                nanosleep(&ts, nullptr);
            }
        }
        pid_ = -1;
    }
    if (reader_.joinable()) reader_.join();
}

bool unit_active(const std::string& unit) {
    auto r = run({"systemctl", "is-active", unit + ".service"});
    auto s = trim(r.out);
    return s == "active" || s == "activating" || s == "deactivating";
}

namespace {

int connect_timeout(int fd, const sockaddr* addr, socklen_t len, double timeout) {
    set_cloexec_nonblock(fd);
    if (connect(fd, addr, len) == 0) return 0;
    if (errno != EINPROGRESS && errno != EAGAIN) return -1;
    pollfd p = {fd, POLLOUT, 0};
    if (poll(&p, 1, static_cast<int>(timeout * 1000)) <= 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    int err = 0;
    socklen_t elen = sizeof err;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
    if (err) {
        errno = err;
        return -1;
    }
    return 0;
}

bool send_all(int fd, const std::string& data, double deadline) {
    size_t done = 0;
    while (done < data.size()) {
        ssize_t n = send(fd, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
            pollfd p = {fd, POLLOUT, 0};
            double left = deadline - now_s();
            if (left <= 0 || poll(&p, 1, static_cast<int>(left * 1000) + 1) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

// Reads until the peer closes, stop(buf) says enough, or the deadline.
bool recv_until(int fd, std::string& buf, double deadline, bool (*stop)(const std::string&)) {
    while (true) {
        if (stop && stop(buf)) return true;
        double left = deadline - now_s();
        if (left <= 0) return false;
        pollfd p = {fd, POLLIN, 0};
        int r = poll(&p, 1, static_cast<int>(left * 1000) + 1);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        char tmp[16384];
        ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n > 0) {
            buf.append(tmp, static_cast<size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        return true;  // closed
    }
}

std::string unchunk(const std::string& body) {
    std::string out;
    size_t i = 0;
    while (i < body.size()) {
        size_t eol = body.find("\r\n", i);
        if (eol == std::string::npos) break;
        size_t n = std::strtoul(body.substr(i, eol - i).c_str(), nullptr, 16);
        if (n == 0) break;
        out += body.substr(eol + 2, n);
        i = eol + 2 + n + 2;
    }
    return out;
}

}  // namespace

HttpResult http(std::string_view method, int port, std::string_view path, const std::string* body, double timeout) {
    HttpResult res;
    double deadline = now_s() + timeout;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        res.error = std::strerror(errno);
        return res;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect_timeout(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr, timeout) != 0) {
        res.error = errno == ECONNREFUSED ? "[Errno 111] Connection refused" : std::strerror(errno);
        close(fd);
        return res;
    }
    std::string req = std::string(method) + " " + std::string(path) + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                      std::to_string(port) + "\r\nConnection: close\r\nAccept-Encoding: identity\r\n";
    if (body) {
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body->size()) + "\r\n\r\n" + *body;
    } else {
        req += "\r\n";
    }
    if (!send_all(fd, req, deadline)) {
        res.error = "timed out";
        close(fd);
        return res;
    }
    std::string buf;
    bool done = recv_until(fd, buf, deadline, [](const std::string& b) {
        // Complete once the headers say how long the body is and it is there.
        auto head_end = b.find("\r\n\r\n");
        if (head_end == std::string::npos) return false;
        std::string head = lower(b.substr(0, head_end));
        auto cl = head.find("content-length:");
        if (cl == std::string::npos) return false;
        size_t len = std::strtoul(head.c_str() + cl + 15, nullptr, 10);
        return b.size() >= head_end + 4 + len;
    });
    close(fd);
    auto head_end = buf.find("\r\n\r\n");
    if (!done || head_end == std::string::npos || buf.compare(0, 5, "HTTP/") != 0) {
        res.error = done ? "bad answer" : "timed out";
        return res;
    }
    auto sp = buf.find(' ');
    res.status = std::atoi(buf.c_str() + sp + 1);
    std::string head = lower(buf.substr(0, head_end));
    res.body = buf.substr(head_end + 4);
    if (head.find("transfer-encoding: chunked") != std::string::npos) res.body = unchunk(res.body);
    auto cl = head.find("content-length:");
    if (cl != std::string::npos) {
        size_t len = std::strtoul(head.c_str() + cl + 15, nullptr, 10);
        if (res.body.size() > len) res.body.resize(len);
    }
    return res;
}

std::optional<std::string> unix_request(const std::string& path, const std::string& line, double timeout) {
    double deadline = now_s() + timeout;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::nullopt;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
    if (connect_timeout(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr, timeout) != 0 ||
        !send_all(fd, line + "\n", deadline)) {
        close(fd);
        return std::nullopt;
    }
    std::string buf;
    recv_until(fd, buf, deadline, [](const std::string& b) { return b.find('\n') != std::string::npos; });
    close(fd);
    if (buf.find('\n') == std::string::npos && buf.empty()) return std::nullopt;
    return buf.substr(0, buf.find('\n'));
}

User user_named(const std::string& name) {
    User u;
    struct passwd pw, *res = nullptr;
    char buf[4096];
    if (getpwnam_r(name.c_str(), &pw, buf, sizeof buf, &res) == 0 && res) {
        u.ok = true;
        u.name = pw.pw_name;
        u.home = pw.pw_dir;
        u.uid = pw.pw_uid;
        u.gid = pw.pw_gid;
    }
    return u;
}

const User& desktop_user() {
    static const User u = [] {
        const char* name = std::getenv("PBOS_QAM_USER");
        User found = user_named(name && *name ? name : "steamos");
        if (!found.ok) {
            found.name = "steamos";
            found.home = "/home/steamos";
            found.uid = 1000;
            found.gid = 1000;
        }
        return found;
    }();
    return u;
}

std::string dt_model() {
    auto s = read_file("/sys/firmware/devicetree/base/model").value_or("");
    while (!s.empty() && (s.back() == '\0' || std::isspace(static_cast<unsigned char>(s.back())))) s.pop_back();
    return s;
}

std::string dt_compatible() {
    auto s = read_file("/sys/firmware/devicetree/base/compatible").value_or("");
    std::replace(s.begin(), s.end(), '\0', ' ');
    return s;
}

}  // namespace pq

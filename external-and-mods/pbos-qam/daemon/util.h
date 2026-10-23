// Files, processes and local sockets for pbos-qamd's sections: what the
// Decky plugins did with Python's os, subprocess, urllib and socket.
#pragma once

#include <sys/types.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "json.h"

namespace pq {

// To the journal (stderr), one line.
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

std::string trim(std::string_view s);
std::vector<std::string> split_lines(std::string_view s);
std::string lower(std::string_view s);
// Python's str.isdigit for ASCII: non-empty, digits only.
bool is_digits(std::string_view s);
// Standard base64 (RFC 4648, padded).
std::string base64(std::string_view data);

std::optional<std::string> read_file(const std::string& path);
// The file's text, stripped; def when it cannot be read (Python's rd()).
std::string rd(const std::string& path, std::string_view def = "");
bool exists(const std::string& path);
bool is_dir(const std::string& path);
bool is_file(const std::string& path);
bool is_link(const std::string& path);
bool can_exec(const std::string& path);
bool can_read(const std::string& path);
std::string dirname(const std::string& path);
std::string basename(const std::string& path);
std::string join(const std::string& a, const std::string& b);
std::string realpath(const std::string& path);

// An object, or {} when missing, unreadable or not an object.
pj::Value read_json(const std::string& path);
// path.tmp, then renamed over path; the folder is made first.
bool write_text(const std::string& path, const std::string& text);
bool write_json(const std::string& path, const pj::Value& v, int indent = 2);
// One value written in place, as to a sysfs attribute (no temp file).
bool write_sysfs(const std::string& path, std::string_view value);
bool makedirs(const std::string& path, mode_t mode = 0755);
bool remove_file(const std::string& path);
bool remove_tree(const std::string& path);
// Sorted names in a folder, without . and ..; empty when it cannot be read.
std::vector<std::string> listdir(const std::string& path);
// Sorted matches, as Python's sorted(glob.glob()).
std::vector<std::string> glob(const std::string& pattern);

struct RunOpts {
    std::optional<std::string> input;  // to stdin; else stdin is /dev/null
    // KEY=value; when empty the daemon's own environment is passed on.
    std::vector<std::string> env;
    double timeout = 30;               // seconds; the program is killed after
    bool capture = true;               // stdout and stderr; else /dev/null
};
struct RunResult {
    bool started = false;
    bool timed_out = false;
    int code = -1;                     // the exit status, or -signal
    std::string out, err;
};
RunResult run(const std::vector<std::string>& argv, const RunOpts& opts = {});
// Started and left to run; nothing waits for it.
void spawn(const std::vector<std::string>& argv);
// The environment the Decky plugins called CLEAN_ENV, plus extra.
std::vector<std::string> clean_env(std::vector<std::string> extra = {});

// A program whose output is read line by line as it runs (the stick
// calibration's live values).
class LineChild {
public:
    ~LineChild() { stop(0); }
    bool start(const std::vector<std::string>& argv, std::function<void(const std::string&)> on_line);
    bool running() const;
    // SIGTERM, up to wait seconds for it to end, then SIGKILL; its last
    // lines are read before this returns.
    void stop(double wait);

private:
    pid_t pid_ = -1;
    std::thread reader_;
};

// systemctl is-active: active, activating or deactivating.
bool unit_active(const std::string& unit);

struct HttpResult {
    int status = 0;                    // 0: no answer
    std::string body;
    std::string error;
};
// One request to 127.0.0.1:port (Barry Launcher's shelld and statsd).
HttpResult http(std::string_view method, int port, std::string_view path, const std::string* body,
                double timeout);
// One line to a unix socket and its one-line answer.
std::optional<std::string> unix_request(const std::string& path, const std::string& line, double timeout);

struct User {
    bool ok = false;
    std::string name, home;
    uid_t uid = 0;
    gid_t gid = 0;
};
User user_named(const std::string& name);
// The desktop user (Decky's DECKY_USER): steamos.
const User& desktop_user();

// The device tree's model and compatible strings (NULs as spaces).
std::string dt_model();
std::string dt_compatible();

}  // namespace pq

// The report section: System > Report a Problem (sections/report.js). The
// user picks what went wrong; pbos-report (/usr/bin, run as root here, so it
// can read every log) writes down the version, this boot's logs and the
// state of that part, and the report is saved in ~/Desktop/PB-OS Reports
// for the user to send. Nothing is sent anywhere.
//
// The report goes into the home as the user (setpriv), as the App Store's
// files do: the root daemon never writes there itself.

#include <ctime>
#include <mutex>

#include "section.h"
#include "util.h"

namespace pq {

namespace {

using pj::Value;

const char* const AREAS[] = {"audio", "sleep", "controller", "display", "wifi", "other"};

bool known_area(const std::string& a) {
    for (const char* k : AREAS)
        if (a == k) return true;
    return false;
}

RunResult run_user(std::vector<std::string> argv, std::optional<std::string> input) {
    const User& u = desktop_user();
    std::vector<std::string> full = {"setpriv", "--reuid=" + std::to_string(u.uid), "--regid=" + std::to_string(u.gid),
                                     "--init-groups", "--"};
    full.insert(full.end(), argv.begin(), argv.end());
    RunOpts o;
    o.env = clean_env({"HOME=" + u.home, "USER=" + u.name, "LOGNAME=" + u.name});
    o.input = std::move(input);
    return run(full, o);
}

class Report : public Section {
public:
    std::string name() const override { return "report"; }
    bool here() const override { return can_exec("/usr/bin/pbos-report"); }

    Value call(const std::string& m, const Value& a) override {
        if (m != "make") throw CallError("report has no method " + m);
        std::string area = arg(a, 0, "other").str();
        if (!known_area(area)) throw CallError("Unknown problem area: " + area);
        // One at a time: a second tap waits for the first.
        std::lock_guard<std::mutex> lock(busy_);

        RunOpts o;
        o.env = clean_env();
        o.timeout = 120;
        auto r = run({"/usr/bin/pbos-report", area}, o);
        if (!r.started || r.out.empty()) throw CallError("The report could not be made.");

        char stamp[32];
        time_t now = time(nullptr);
        strftime(stamp, sizeof stamp, "%Y-%m-%d-%H%M%S", localtime(&now));
        std::string dir = desktop_user().home + "/Desktop/PB-OS Reports";
        std::string path = dir + "/pbos-report-" + area + "-" + stamp + ".txt";
        if (run_user({"mkdir", "-p", dir}, std::nullopt).code != 0 ||
            run_user({"sh", "-c", "cat > \"$1\"", "sh", path}, r.out).code != 0)
            throw CallError("The report could not be saved in " + dir + ".");
        // In the journal too: the moment the user saw the problem.
        log("report: %s problem reported, saved as %s", area.c_str(), path.c_str());

        Value out = Value::object();
        out["path"] = path;
        out["bytes"] = static_cast<unsigned long>(r.out.size());
        return out;
    }

private:
    std::mutex busy_;
};

}  // namespace

std::unique_ptr<Section> make_report_section() { return std::make_unique<Report>(); }

}  // namespace pq

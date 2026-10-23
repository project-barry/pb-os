// PB-OS Sound Check: a Steam app (non-Steam shortcut) for the sound
// dropping out (AYN Thor reports, 2026-10). Temporary: it ships until that
// bug is found.
//
// The user runs it when the sound is gone, and again after a restart when
// it works. Each run saves a report (pbos-report audio) and, after asking,
// plays two short quiet tones: one through the default output ("Top
// screen" on the Thor), one straight on the real output under it. Whether
// each was heard is written into the report. The two answers tell PipeWire's
// routing from the driver, DSP and amplifier below it; the two reports, side
// by side, show what changed. Reports go to ~/Desktop/PB-OS Reports and are
// sent nowhere.
//
// SDL2 for the window, the controller (Steam's virtual pad), keys and touch;
// SDL2_ttf with Noto Sans for the text. Barry's head is drawn from its
// pixels (the PB-OS Quick Access icon's, ../pbos-qam/frontend/src/loader.js).

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <mutex>
#include <regex>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

const char* FONT = "/usr/share/fonts/noto/NotoSans-Regular.ttf";
const char* FONT_BOLD = "/usr/share/fonts/noto/NotoSans-Bold.ttf";
const char* TOP = "pbos_top_screen";
const char* TOP_OUT = "pbos_top_screen_out";

// Barry's head, 13 x 11, as runs of pixels: row, x, width.
const int HEAD[][3] = {
    {0, 3, 1},  {0, 9, 1},  {1, 2, 3},  {1, 8, 3},  {2, 2, 9},  {3, 1, 11}, {4, 1, 3},  {4, 5, 3},
    {4, 9, 3},  {5, 0, 6},  {5, 7, 6},  {6, 1, 11}, {7, 0, 13}, {8, 2, 9},  {9, 3, 7},  {10, 4, 5},
};

// ------------------------------------------------------------- helpers ---
std::string sh(const std::string& cmd) {
    std::string out;
    if (FILE* p = popen(cmd.c_str(), "r")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
        pclose(p);
    }
    return out;
}

// Spaces, tabs and line ends at both ends (pactl indents with tabs).
std::string trim(std::string s) {
    auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && blank(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && blank(s[i])) i++;
    return s.substr(i);
}

// Quoted for sh -c.
std::string q(const std::string& s) {
    std::string o = "'";
    for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return o + "'";
}

// An output's volume, 0..1 as pactl shows it (the first channel's %).
double sink_volume(const std::string& name) {
    std::string v = sh("pactl get-sink-volume " + q(name) + " 2>/dev/null");
    size_t pct = v.find('%');
    if (pct == std::string::npos) return 0;
    size_t start = v.find_last_of(" /", pct - 1);
    return std::clamp(std::atof(v.substr(start + 1, pct - start - 1).c_str()) / 100.0, 0.0, 1.0);
}

bool sink_exists(const std::string& name) {
    std::istringstream in(sh("pactl list short sinks 2>/dev/null"));
    for (std::string line; std::getline(in, line);) {
        std::istringstream f(line);
        std::string idx, n;
        f >> idx >> n;
        if (n == name) return true;
    }
    return false;
}

// The output "Top screen"'s loopback plays on, else the default one.
std::string real_output() {
    std::string sink_idx;
    std::istringstream in(sh("pactl list sink-inputs 2>/dev/null"));
    std::string cur;
    for (std::string line; std::getline(in, line);) {
        std::string t = trim(line);
        if (t.rfind("Sink:", 0) == 0) cur = trim(t.substr(5));
        if (t.find(std::string("node.name = \"") + TOP_OUT + "\"") != std::string::npos) sink_idx = cur;
    }
    if (!sink_idx.empty()) {
        std::istringstream s(sh("pactl list short sinks 2>/dev/null"));
        for (std::string line; std::getline(s, line);) {
            std::istringstream f(line);
            std::string idx, n;
            f >> idx >> n;
            if (idx == sink_idx) return n;
        }
    }
    return trim(sh("pactl get-default-sink 2>/dev/null"));
}

// Ear safety: one steady, low beep, never a sweep or a click. 1 s of 440 Hz
// at -26 dBFS (5 % of full scale), faded in and out over 0.15 s, as a 48 kHz
// stereo WAV.
std::string tone_file() {
    const char* rt = getenv("XDG_RUNTIME_DIR");
    std::string path = std::string(rt && *rt ? rt : "/tmp") + "/pbos-sound-check-tone.wav";
    const int rate = 48000, n = rate;
    std::vector<int16_t> pcm(n * 2);
    for (int i = 0; i < n; i++) {
        double env = std::min({1.0, i / 7200.0, (n - i) / 7200.0});
        auto v = static_cast<int16_t>(std::sin(2 * M_PI * 440 * i / rate) * 0.05 * env * 32767);
        pcm[2 * i] = pcm[2 * i + 1] = v;
    }
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    uint32_t data = n * 4;
    f.write("RIFF", 4); u32(36 + data); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(2);
    u32(rate); u32(rate * 4); u16(4); u16(16); f.write("data", 4); u32(data);
    f.write(reinterpret_cast<const char*>(pcm.data()), data);
    return path;
}

std::string model() {
    std::string m = sh("tr -d '\\0' < /proc/device-tree/model 2>/dev/null");
    return trim(m).empty() ? "unknown" : trim(m);
}
std::string version() {
    std::string v = trim(sh("cat /usr/share/pb-os/version 2>/dev/null"));
    return v.empty() ? "unknown" : v;
}

// What the relay accepts in a header: letters, digits, a little punctuation.
std::string header_value(std::string v) {
    for (char& c : v)
        if (!isalnum(static_cast<unsigned char>(c)) && !strchr(" ._:/()+,=-", c)) c = '-';
    return v.substr(0, 120);
}

// The copy that is sent: no Wi-Fi or network lines; MAC, IP addresses and
// the device's host name hidden; at most 400 KB.
std::string for_sending(const std::string& report) {
    static const std::regex network(
        "wlan|wi-?fi|ssid|bssid|AP MAC|NetworkManager|wpa_supplicant|iwd|hostapd|softapmanager|dhcp|cfg80211|"
        "mac80211|ath1[0-2]|tailscale|avahi|resolved|ipv[46]|nmcli",
        std::regex::icase);
    static const std::regex mac("[0-9A-Fa-f]{2}([:_-])([0-9A-Fa-f]{2}\\1){4}[0-9A-Fa-f]{2}");
    static const std::regex ipv4("(^|[^0-9A-Za-z.])([0-9]{1,3}\\.){3}[0-9]{1,3}(?![0-9A-Za-z.])");
    // Link-local and global IPv6 only: SoundWire ids (sdw:2:0:0217:...) stay.
    static const std::regex ipv6("\\b(fe80|[23][0-9A-Fa-f]{3}):[0-9A-Fa-f:]*:[0-9A-Fa-f]{1,4}\\b", std::regex::icase);
    std::string host = trim(sh("cat /etc/hostname 2>/dev/null"));
    std::regex hostname;
    bool mask_host = host.size() >= 3 && std::regex_match(host, std::regex("[A-Za-z0-9-]+"));
    if (mask_host) hostname = std::regex("\\b" + host + "\\b");
    std::string out;
    std::istringstream in(report);
    for (std::string line; std::getline(in, line);) {
        if (std::regex_search(line, network)) continue;
        line = std::regex_replace(line, mac, "xx:xx:xx:xx:xx:xx");
        line = std::regex_replace(line, ipv4, "$1x.x.x.x");
        line = std::regex_replace(line, ipv6, "x:x:x:x");
        if (mask_host) line = std::regex_replace(line, hostname, "<host>");
        out += line + "\n";
        if (out.size() > 400 * 1024) {
            out += "[cut at 400 KB]\n";
            break;
        }
    }
    return out;
}

std::string stamp(const char* fmt) {
    char b[64];
    time_t now = time(nullptr);
    strftime(b, sizeof b, fmt, localtime(&now));
    return b;
}

// ------------------------------------------------------------------ UI ---
struct Button {
    std::string label;
    std::function<void()> act;
    SDL_Rect r{};
};

struct Screen {
    std::string title, text;
    std::vector<Button> buttons;
    bool busy = false;
};

class App {
public:
    bool init() {
        SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
        // Only the pad Steam gives games (evdev), never SDL's HIDAPI drivers:
        // SDL's HIDAPI detached usbhid from the Deck controller's USB/IP
        // interface on the Nova (2026-10-21) and Steam lost the controller.
        SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI, "0");
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0 || TTF_Init() != 0) {
            fprintf(stderr, "pbos-sound-check: %s\n", SDL_GetError());
            return false;
        }
        win_ = SDL_CreateWindow("PB-OS Sound Check", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
        ren_ = win_ ? SDL_CreateRenderer(win_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
        if (!ren_) {
            fprintf(stderr, "pbos-sound-check: %s\n", SDL_GetError());
            return false;
        }
        if (!layout()) {
            fprintf(stderr, "pbos-sound-check: %s\n", TTF_GetError());
            return false;
        }
        for (int i = 0; i < SDL_NumJoysticks(); i++)
            if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
        top_ = sink_exists(TOP);
        welcome();
        return true;
    }

    void run() {
        while (!quit_) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) handle(e);
            {
                std::lock_guard<std::mutex> l(m_);
                if (next_) {
                    next_();
                    next_ = nullptr;
                }
            }
            draw();
        }
        SDL_Quit();
    }

private:
    // The fonts for the window's size, again when it changes.
    bool layout() {
        int w, h;
        SDL_GetRendererOutputSize(ren_, &w, &h);
        if (text_ && w == w_ && h == h_) return true;
        w_ = w;
        h_ = h;
        for (TTF_Font* f : {text_, title_, btn_})
            if (f) TTF_CloseFont(f);
        int unit = std::max(16, std::min(w_, h_) / 26);
        text_ = TTF_OpenFont(FONT, unit);
        title_ = TTF_OpenFont(FONT_BOLD, unit * 3 / 2);
        btn_ = TTF_OpenFont(FONT_BOLD, unit);
        if (!text_ || !title_ || !btn_) return false;
        for (TTF_Font* f : {text_, title_, btn_}) TTF_SetFontWrappedAlign(f, TTF_WRAPPED_ALIGN_CENTER);
        return true;
    }

    // ---- the steps ----
    void show(Screen s) {
        scr_ = std::move(s);
        focus_ = 0;
    }
    // From a worker thread: the next step, on the UI thread.
    void later(std::function<void()> f) {
        std::lock_guard<std::mutex> l(m_);
        next_ = std::move(f);
    }

    void welcome() {
        show({"PB-OS Sound Check",
              "This saves a sound report for the PB-OS team and plays short test tones.\n\n"
              "Is the sound gone right now?",
              {{"Yes, the sound is gone", [this] { start(true); }},
               {"No, the sound works (after a restart)", [this] { start(false); }},
               {"Quit", [this] { quit_ = true; }}}});
    }

    void start(bool broken) {
        broken_ = broken;
        heard_.clear();
        show({"Saving the report…", "This takes a few seconds.", {}, true});
        std::thread([this] {
            std::string r = sh("pbos-report audio 2>&1");
            // The audio path below PipeWire needs root: pbos-report-audio-root.service,
            // which the user may start (50-pbos-report.rules), writes it to /run.
            if (sh("timeout 60 systemctl start pbos-report-audio-root.service >/dev/null 2>&1 && echo ok").find("ok") == 0)
                r += sh("cat /run/pbos-report/audio-root.txt 2>&1");
            else
                r += "\n=== audio path (root only): could not be read\n";
            later([this, r] {
                report_ = r;
                outputs();
            });
        }).detach();
    }

    // The outputs the tones play on: "Top screen" then the real one under
    // it, or just the default output where there is no "Top screen".
    void outputs() {
        tones_.clear();
        std::string real = real_output();
        if (top_) tones_.push_back({TOP, "the Top screen output (what games and Steam play on)"});
        if (!real.empty() && real != TOP)
            tones_.push_back({real, top_ ? "the speakers directly (" + real + ")" : "the default output (" + real + ")"});
        show({"Test tones",
              "Next, PB-OS plays a short, soft beep on each output, one at a time, at your current volume. "
              "If you hear nothing, you can raise the volume a little and play it again.",
              {{"Play the first tone", [this] { tone(0); }}, {"Skip the tones", [this] { save(); }}}});
    }

    void tone(size_t i) {
        if (i >= tones_.size()) return save();
        std::string target = tones_[i].first;
        // The real output under "Top screen" is held at 100 %: played there
        // straight, the beep gets the Top screen's own volume (PipeWire's
        // cubic scale, as the loopback applies it), so it is never louder
        // than the first beep, headphones or speakers.
        std::string vol;
        if (top_ && target != TOP) {
            double v = sink_volume(TOP);
            char b[32];
            snprintf(b, sizeof b, " --volume=%.4f", v * v * v);
            vol = b;
        }
        show({"Playing tone " + std::to_string(i + 1) + " of " + std::to_string(tones_.size()),
              "Listen: a beep on " + tones_[i].second + ".", {}, true});
        std::thread([this, i, target, vol] {
            std::string wav = tone_file();
            std::string err = trim(sh("timeout 5 pw-play --target " + q(target) + vol + " " + q(wav) + " 2>&1"));
            later([this, i, err] { ask(i, err); });
        }).detach();
    }

    void ask(size_t i, const std::string& err) {
        auto answer = [this, i, err](const char* a) {
            heard_.push_back("tone " + std::to_string(i + 1) + " on " + tones_[i].first + ": " + a +
                             (err.empty() ? "" : " (pw-play: " + err + ")"));
            tone(i + 1);
        };
        show({"Did you hear tone " + std::to_string(i + 1) + "?", "The beep on " + tones_[i].second + ".",
              {{"Yes, I heard it", [answer] { answer("heard"); }},
               {"No, nothing", [answer] { answer("NOT heard"); }},
               {"Play it again", [this, i] { tone(i); }}}});
    }

    void save() {
        std::string mode = broken_ ? "sound-gone" : "sound-works";
        std::string dir = std::string(getenv("HOME") ? getenv("HOME") : "/home/steamos") + "/Desktop/PB-OS Reports";
        std::string path = dir + "/pbos-soundcheck-" + mode + "-" + stamp("%Y-%m-%d-%H%M%S") + ".txt";
        std::string head = "=== PB-OS Sound Check: " + mode + ", " + stamp("%F %T %Z") + "\n";
        for (const auto& h : heard_) head += h + "\n";
        if (heard_.empty()) head += "tones skipped\n";
        sh("mkdir -p " + q(dir));
        std::ofstream f(path);
        f << head << "\n" << report_;
        f.close();
        bool ok = f.good();
        sh("logger -t pbos-sound-check " + q(mode + ": report saved as " + path));
        if (!ok) {
            show({"Not saved", "The report couldn't be written to " + dir + ".", {{"Quit", [this] { quit_ = true; }}}});
            return;
        }
        shown_ = "Desktop/PB-OS Reports";
        // PBOS_REPORT_URL_FILE: a test copy's address file (the image's is read-only).
        const char* url_file = getenv("PBOS_REPORT_URL_FILE");
        std::string url = trim(sh("cat " + q(url_file && *url_file ? url_file : "/usr/share/pb-os/report-url") + " 2>/dev/null"));
        if (url.empty()) return done("");
        sent_path_ = path.substr(0, path.size() - 4) + "-sent.txt";
        std::ofstream sent(sent_path_);
        sent << for_sending(head + "\n" + report_);
        sent.close();
        if (!sent.good()) return done("");
        show({"Send it to the PB-OS team?",
              "This sends the sound parts of the report and your answers to the PB-OS team's private Discord "
              "channel. Wi-Fi and network lines are left out and hardware and network addresses are hidden; "
              "the names of what was playing and of Bluetooth headphones can show. Exactly what is sent is "
              "saved next to the report (-sent.txt).",
              {{"Send", [this, url, mode] { send(url, mode); }}, {"Don't send", [this] { done(""); }}}});
    }

    void send(const std::string& url, const std::string& mode) {
        show({"Sending…", "", {}, true});
        std::string path = sent_path_;
        std::thread([this, url, mode, path] {
            std::string r = sh("curl -sS -m 30 -X POST " + q(url + "/sound-report") +
                               " -H 'User-Agent: PB-OS-SoundCheck/1' -H 'Content-Type: text/plain'"
                               " -H " + q("X-PBOS-Device: " + header_value(model())) +
                               " -H " + q("X-PBOS-Version: " + header_value(version())) +
                               " -H " + q("X-PBOS-Mode: " + mode) + " --data-binary @" + q(path) + " 2>&1");
            std::string note;
            if (r.find("\"sent\"") != std::string::npos || r.find("\"duplicate\"") != std::string::npos)
                note = "Sent to the PB-OS team.";
            else if (r.find("\"limit\"") != std::string::npos)
                note = "Not sent: today's limit is reached. The report is still saved.";
            else
                note = "Not sent (no connection?). The report is still saved; you can send the file yourself.";
            sh("logger -t pbos-sound-check " + q("send: " + trim(r)));
            later([this, note] { done(note); });
        }).detach();
    }

    void done(const std::string& note) {
        std::string first = note.empty() ? "" : note + "\n\n";
        if (broken_) {
            show({"Saved. Now restart",
                  first + "Saved in " + shown_ + ".\n\nRestart the device, then open PB-OS Sound Check again and "
                  "pick \"No, the sound works\" once the sound is back.",
                  {{"Restart now", [] { sh("systemctl reboot"); }}, {"Quit", [this] { quit_ = true; }}}});
        } else {
            show({"Saved. Thank you!", first + "Saved in " + shown_ + ".", {{"Quit", [this] { quit_ = true; }}}});
        }
    }

    // ---- input ----
    void press() {
        if (!scr_.busy && focus_ < scr_.buttons.size()) {
            auto act = scr_.buttons[focus_].act;
            act();
        }
    }
    void move(int d) {
        if (scr_.buttons.empty()) return;
        int n = static_cast<int>(scr_.buttons.size());
        focus_ = static_cast<size_t>((static_cast<int>(focus_) + d + n) % n);
    }
    void handle(const SDL_Event& e) {
        switch (e.type) {
        case SDL_QUIT: quit_ = true; break;
        case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(e.cdevice.which); break;
        case SDL_CONTROLLERBUTTONDOWN:
            switch (e.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_A: press(); break;
            case SDL_CONTROLLER_BUTTON_B: if (!scr_.busy) quit_ = true; break;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: case SDL_CONTROLLER_BUTTON_DPAD_LEFT: move(-1); break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: move(1); break;
            }
            break;
        case SDL_CONTROLLERAXISMOTION:
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                int dir = e.caxis.value > 20000 ? 1 : e.caxis.value < -20000 ? -1 : 0;
                if (dir && dir != stick_) move(dir);
                stick_ = dir;
            }
            break;
        case SDL_KEYDOWN:
            switch (e.key.keysym.sym) {
            case SDLK_RETURN: case SDLK_SPACE: press(); break;
            case SDLK_ESCAPE: if (!scr_.busy) quit_ = true; break;
            case SDLK_UP: case SDLK_LEFT: move(-1); break;
            case SDLK_DOWN: case SDLK_RIGHT: case SDLK_TAB: move(1); break;
            }
            break;
        case SDL_MOUSEBUTTONUP: {  // touch arrives as the mouse too
            int x = e.button.x, y = e.button.y;
            int ww, wh;
            SDL_GetWindowSize(win_, &ww, &wh);
            if (ww > 0 && wh > 0) { x = x * w_ / ww; y = y * h_ / wh; }
            for (size_t i = 0; i < scr_.buttons.size(); i++) {
                const SDL_Rect& r = scr_.buttons[i].r;
                if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) {
                    focus_ = i;
                    press();
                    break;
                }
            }
            break;
        }
        }
    }

    // ---- drawing ----
    void text(TTF_Font* f, const std::string& s, int x, int y, int wrap, SDL_Color c, bool center = false, int* hout = nullptr) {
        if (s.empty()) { if (hout) *hout = 0; return; }
        SDL_Surface* surf = TTF_RenderUTF8_Blended_Wrapped(f, s.c_str(), c, static_cast<Uint32>(wrap));
        if (!surf) return;
        SDL_Texture* t = SDL_CreateTextureFromSurface(ren_, surf);
        SDL_Rect r{center ? x + (wrap - surf->w) / 2 : x, y, surf->w, surf->h};
        SDL_RenderCopy(ren_, t, nullptr, &r);
        if (hout) *hout = surf->h;
        SDL_DestroyTexture(t);
        SDL_FreeSurface(surf);
    }

    void head(int x, int y, int px) {
        SDL_SetRenderDrawColor(ren_, 0xf2, 0xf4, 0xf8, 255);
        for (const auto& run : HEAD) {
            SDL_Rect r{x + run[1] * px, y + run[0] * px, run[2] * px, px};
            SDL_RenderFillRect(ren_, &r);
        }
    }

    void draw() {
        if (!layout()) {
            quit_ = true;
            return;
        }
        SDL_SetRenderDrawColor(ren_, 0x14, 0x19, 0x24, 255);
        SDL_RenderClear(ren_);
        int margin = w_ / 14, col = w_ - 2 * margin;
        int px = std::max(3, h_ / 90);
        head((w_ - 13 * px) / 2, h_ / 18, px);
        int y = h_ / 18 + 11 * px + h_ / 30, th = 0;
        text(title_, scr_.title, margin, y, col, {0xff, 0xff, 0xff, 255}, true, &th);
        y += th + h_ / 40;
        text(text_, scr_.text, margin, y, col, {0xc8, 0xd0, 0xdc, 255}, true, &th);
        y += th + h_ / 25;
        if (scr_.busy) {
            int dots = static_cast<int>(SDL_GetTicks() / 400 % 4);
            text(btn_, std::string(static_cast<size_t>(dots), '.') + " ", margin, y, col, {0x1a, 0x9f, 0xff, 255}, true);
        }
        int bh = TTF_FontHeight(btn_) * 2, bw = std::min(col, w_ * 2 / 3), bx = (w_ - bw) / 2;
        for (size_t i = 0; i < scr_.buttons.size(); i++) {
            Button& b = scr_.buttons[i];
            b.r = {bx, y, bw, bh};
            bool on = i == focus_;
            SDL_SetRenderDrawColor(ren_, on ? 0x1a : 0x2a, on ? 0x9f : 0x33, on ? 0xff : 0x44, 255);
            SDL_RenderFillRect(ren_, &b.r);
            int lh = TTF_FontHeight(btn_);
            text(btn_, b.label, bx, y + (bh - lh) / 2, bw, {0xff, 0xff, 0xff, 255}, true);
            y += bh + bh / 4;
        }
        // The hint where the buttons leave room for it.
        if (y + TTF_FontHeight(text_) < h_ - h_ / 12)
            text(text_, "A: select    B: quit    or tap", margin, h_ - h_ / 12, col, {0x80, 0x8a, 0x99, 255}, true);
        SDL_RenderPresent(ren_);
    }

    SDL_Window* win_ = nullptr;
    SDL_Renderer* ren_ = nullptr;
    TTF_Font *text_ = nullptr, *title_ = nullptr, *btn_ = nullptr;
    int w_ = 0, h_ = 0, stick_ = 0;
    Screen scr_;
    size_t focus_ = 0;
    std::atomic<bool> quit_{false};
    std::mutex m_;
    std::function<void()> next_;
    bool top_ = false, broken_ = false;
    std::string report_, shown_, sent_path_;
    std::vector<std::pair<std::string, std::string>> tones_;
    std::vector<std::string> heard_;
};

}  // namespace

int main(int argc, char** argv) {
    // Test: the copy that would be sent, for a saved report.
    if (argc == 3 && std::string(argv[1]) == "--scrub") {
        std::ifstream f(argv[2]);
        std::stringstream b;
        b << f.rdbuf();
        fputs(for_sending(b.str()).c_str(), stdout);
        return 0;
    }
    // Test: the output the second tone plays on.
    if (argc == 2 && std::string(argv[1]) == "--real-output") {
        printf("%s\n", real_output().c_str());
        return 0;
    }
    // Steam starts games with its (x86) overlay preloaded: every command run
    // from here would print ld.so errors into the report.
    unsetenv("LD_PRELOAD");
    App app;
    if (!app.init()) return 1;
    app.run();
    return 0;
}

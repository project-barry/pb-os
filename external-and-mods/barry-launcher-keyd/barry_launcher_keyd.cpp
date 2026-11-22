// barry_launcher_keyd: when Barry Launcher's keyboards come up (AYN Thor,
// Retroid Pocket Duo).
//
// It watches what the user does and where text goes, and tells
// barry_launcher_shelld (which shows the keyboard windows) what to show:
// - text focus and caret moves over AT-SPI (Game Mode, and Barry's own apps
//   in Desktop Mode);
// - Desktop Mode: KWin's input method (barry_launcher_imd), which says when
//   a text field is active; when it is gone for a while, KWin is asked to
//   start it again;
// - the bottom touchscreen: when and where the latest touch came down, and
//   one-finger taps;
// - presses that count as a click on the top screen: a finger on the top
//   touchscreen, the trackpad's Left and Right. Devices are picked up as
//   udev adds them (the Trackpad app makes its trackpad as it opens).
//
// The rules (unchanged from shelld, where they lived before):
// - a field on the bottom screen brings the bottom keyboard up only right
//   after a touch, on that field (or Firefox's address bar), not while an
//   app opens, and not from the touch that put the keyboard away;
// - a field on the top screen brings the Trackpad app's keys up only right
//   after a click on the top screen;
// - focus leaving a field puts the bottom keyboard away (after a moment:
//   apps refocus their own fields).
//
// Talks to shelld over its local HTTP (GET/POST /keyd); answers
// "BEFORE <n>" (the text before the caret, for autocorrect), "AFTER <n>"
// (after it) and "AROUND <b> <a>" (both at once, for the keyboards' word
// taken up again: {"text": before, "after": after}) on
// $XDG_RUNTIME_DIR/barry_launcher_keyd.sock.
#include <atspi/atspi.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <gudev/gudev.h>
#include <json-glib/json-glib.h>
#include <linux/input.h>

#include "bottompanel.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

// Times are seconds on the monotonic clock (Python's time.monotonic() in
// shelld is the same clock).
double now()
{
    return g_get_monotonic_time() / 1e6;
}

void log(const std::string &msg)
{
    std::printf("%s\n", msg.c_str());
    std::fflush(stdout);
}

std::string env(const char *name, const char *fallback)
{
    const char *v = g_getenv(name);
    return v && *v ? v : fallback;
}

const bool DESKTOP = env("BARRY_DESKTOP", "") == "1";
const int PORT = std::atoi(env("BARRY_PORT", "47824").c_str());
// The bottom screen's height (barry_device's BARRY_BOTTOM_H; THOR_BOTTOM_HEIGHT
// as before, for tests).
const int SCREEN_H = std::atoi(env("THOR_BOTTOM_HEIGHT", env("BARRY_BOTTOM_H", "1080").c_str()).c_str());
const bool TIMING = env("BARRY_TIMING", "") == "1";

const char TOUCH_NAME[] = "bottom_touchscreen";
const double TOUCH_S = 1.0;      // text focus shows the keyboard only this soon after a touch
const double TOP_CLICK_S = 1.0;  // a top-screen field: only this soon after a top-screen click
const int TAP_SLOP = 30;         // touchscreen units a tap may wander
const int FIELD_SLOP = 40;       // pixels around a field that a finger on it may miss by
const unsigned FOCUS_OUT_HIDE_MS = 300;  // an app refocusing its field (Signal, after Send): no flicker
const unsigned REVEAL_MS[] = {250, 700};  // after the keyboard pushes the app up: the field in view
const int REVEAL_ROW_MAX = 160;  // the most pixels high a field's bar (its message box, say) is
const unsigned IM_HIDE_DELAY_MS = 200;    // a field handing over to the next one: no flicker
const double IM_KICK_S = 5.0;    // barry_launcher_imd gone this long: KWin is asked to start it again,
const double IM_KICK_EVERY_S = 30.0;  // at most this often
const char IM_ENTRY[] = "/usr/share/applications/org.barry_launcher.keyboard.desktop";

// ---- shelld --------------------------------------------------------------

// What shelld knows that the rules need, asked for each decision.
struct Shell {
    bool ok = false;
    bool visible = false;           // the bottom keyboard shows
    bool barryUp = false;           // Barry's pages are open (Game Mode)
    bool topInputRunning = false;   // the Trackpad app runs (Game Mode)
    bool topKeyboardVisible = false;
    double quietUntil = 0;          // an app opening: no keyboard before this
    double hiddenByHand = 0;        // the keyboard's own hide (its ⌄ key)
    int keyboardHeight = 0;
    std::string browserProfile;     // the Firefox app's profile directory
};

// One HTTP/1.0 request to shelld on 127.0.0.1; the body of a 200 reply.
std::optional<std::string> http(const char *method, const char *path, const std::string &body = "")
{
    GError *err = nullptr;
    GSocketClient *client = g_socket_client_new();
    g_socket_client_set_timeout(client, 3);
    GSocketConnection *conn = g_socket_client_connect_to_host(client, "127.0.0.1", PORT, nullptr, &err);
    g_object_unref(client);
    if (!conn) {
        g_clear_error(&err);
        return std::nullopt;
    }
    // shelld takes only a Host of 127.0.0.1 with its port (a web page in one
    // of its browsers must not reach it).
    std::string req = std::string(method) + " " + path + " HTTP/1.0\r\nHost: 127.0.0.1:" + std::to_string(PORT)
                      + "\r\n";
    if (!body.empty())
        req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    std::string reply;
    if (g_output_stream_write_all(out, req.data(), req.size(), nullptr, nullptr, &err)) {
        char buf[4096];
        gssize n;
        while ((n = g_input_stream_read(in, buf, sizeof buf, nullptr, &err)) > 0)
            reply.append(buf, size_t(n));
    }
    g_clear_error(&err);
    g_object_unref(conn);
    const size_t head = reply.find("\r\n\r\n");
    if (reply.compare(0, 12, "HTTP/1.0 200") != 0 && reply.compare(0, 12, "HTTP/1.1 200") != 0)
        return std::nullopt;
    return head == std::string::npos ? std::string() : reply.substr(head + 4);
}

std::string jsonString(const std::string &s)
{
    JsonNode *node = json_node_init_string(json_node_alloc(), s.c_str());
    gchar *text = json_to_string(node, FALSE);
    std::string r = text;
    g_free(text);
    json_node_unref(node);
    return r;
}

Shell shell()
{
    Shell s;
    const auto body = http("GET", "/keyd");
    if (!body)
        return s;
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_data(p, body->c_str(), -1, nullptr)) {
        JsonNode *root = json_parser_get_root(p);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *o = json_node_get_object(root);
            s.ok = true;
            s.visible = json_object_get_boolean_member_with_default(o, "visible", FALSE);
            s.barryUp = json_object_get_boolean_member_with_default(o, "barryUp", FALSE);
            s.topInputRunning = json_object_get_boolean_member_with_default(o, "topInputRunning", FALSE);
            s.topKeyboardVisible = json_object_get_boolean_member_with_default(o, "topKeyboardVisible", FALSE);
            s.quietUntil = json_object_get_double_member_with_default(o, "quietUntil", 0);
            s.hiddenByHand = json_object_get_double_member_with_default(o, "hiddenByHand", 0);
            s.keyboardHeight = int(json_object_get_int_member_with_default(o, "keyboardHeight", 0));
            s.browserProfile = json_object_get_string_member_with_default(o, "browserProfile", "");
        }
    }
    g_object_unref(p);
    return s;
}

// what: "show" (the bottom keyboard), "hide", "top" (the Trackpad app's
// keys, opening the app if it does not run) or "top-down" (its full
// trackpad again).
void tell(const char *what, const std::string &why)
{
    const std::string body = std::string("{\"do\": \"") + what + "\", \"why\": " + jsonString(why) + "}";
    if (!http("POST", "/keyd", body))
        log(std::string("cannot reach barry_launcher_shelld (") + what + ")");
}

// ---- touches and presses -------------------------------------------------

struct Touch {
    std::optional<double> last;        // a finger on the bottom screen, latest; unset: no touchscreen
    std::optional<double> began;       // the latest touch's first finger down
    std::optional<std::pair<int, int>> at;  // where (screen pixels) that finger came down
    std::optional<double> lastTopClick;     // trackpad click or top-screen touch
} T;

// Where a raw touch is on the bottom screen (bottompanel.h: on the AYN Thor
// the panel is portrait and turned right, its y running left to right on
// screen, its x bottom to top; on the Retroid Pocket Duo its own landscape,
// scaled from the axes' range once the touchscreen is open).
barry::BottomPanel bottomPanel()
{
    barry::BottomPanel p = barry::BottomPanel::fromEnv();
    p.height = SCREEN_H;
    return p;
}
barry::BottomPanel PANEL = bottomPanel();

std::pair<int, int> rawToScreen(int rawX, int rawY)
{
    return {int(PANEL.x(rawX, rawY)), int(PANEL.y(rawX, rawY))};
}

void onTap(int rawX, int rawY);

// The bottom touchscreen, per slot as the kernel keeps them (a new contact
// repeats no coordinate that has not changed).
struct BottomReader {
    int slot = 0;
    std::map<int, std::pair<std::optional<int>, std::optional<int>>> pos;
    std::set<int> down;
    int lead = -1;
    bool multi = false;
    std::optional<std::pair<int, int>> start;

    void event(const input_event &ev)
    {
        if ((ev.type == EV_KEY && ev.code == BTN_TOUCH) || (ev.type == EV_ABS && ev.code == ABS_MT_TRACKING_ID))
            T.last = now();
        if (ev.type == EV_SYN) {
            auto it = pos.find(lead);
            if (down.count(lead) && !start && it != pos.end() && it->second.first && it->second.second) {
                start = std::make_pair(*it->second.first, *it->second.second);
                T.at = rawToScreen(start->first, start->second);
            }
            return;
        }
        if (ev.type != EV_ABS)
            return;
        if (ev.code == ABS_MT_SLOT) {
            slot = ev.value;
        } else if (ev.code == ABS_MT_TRACKING_ID) {
            if (ev.value >= 0) {
                if (down.empty()) {
                    lead = slot;
                    multi = false;
                    start.reset();
                    T.began = now();
                } else {
                    multi = true;
                }
                down.insert(slot);
            } else {
                down.erase(slot);
                if (down.empty()) {
                    auto it = pos.find(lead);
                    if (!multi && start && it != pos.end() && it->second.first && it->second.second) {
                        const int ex = *it->second.first, ey = *it->second.second;
                        if (std::abs(ex - start->first) + std::abs(ey - start->second) < TAP_SLOP)
                            onTap(ex, ey);
                    }
                    start.reset();
                }
            }
        } else if (ev.code == ABS_MT_POSITION_X) {
            pos[slot].first = ev.value;
        } else if (ev.code == ABS_MT_POSITION_Y) {
            pos[slot].second = ev.value;
        }
    }
};

// A watched input device: its fd and what its events do.
struct Watched {
    std::string name;
    std::string node;
    int fd = -1;
    guint source = 0;
    BottomReader bottom;  // the bottom touchscreen only
};
std::map<std::string, Watched *> watching;  // device node -> watcher

bool isTopPress(const std::string &name, const input_event &ev)
{
    if (ev.type != EV_KEY || ev.value != 1)
        return false;
    if (name == "top_touchscreen")
        return ev.code == BTN_TOUCH;
    if (name == "Barry Launcher Trackpad")
        return ev.code == BTN_LEFT || ev.code == BTN_RIGHT;
    return false;
}

gboolean onDeviceReadable(gint fd, GIOCondition cond, gpointer data)
{
    auto *w = static_cast<Watched *>(data);
    if (cond & (G_IO_ERR | G_IO_HUP | G_IO_NVAL)) {
        log("stopped watching " + w->name + " (gone)");
        close(fd);
        watching.erase(w->node);
        delete w;
        return G_SOURCE_REMOVE;
    }
    input_event evs[64];
    const ssize_t got = read(fd, evs, sizeof evs);
    if (got <= 0)
        return G_SOURCE_CONTINUE;
    for (size_t i = 0; i < size_t(got) / sizeof(input_event); i++) {
        const input_event &ev = evs[i];
        if (w->name == TOUCH_NAME) {
            w->bottom.event(ev);
        } else if (isTopPress(w->name, ev)) {
            // Desktop Mode: a press there is a touch too, for the bottom
            // keyboard over Barry's own apps and for KWin's fields.
            T.lastTopClick = now();
            if (DESKTOP)
                T.last = *T.lastTopClick;
            if (TIMING)
                log("timing: " + w->name + " press " + std::to_string(now()));
        }
    }
    return G_SOURCE_CONTINUE;
}

void watchDevice(GUdevDevice *dev)
{
    const char *node = g_udev_device_get_device_file(dev);
    if (!node || !g_str_has_prefix(g_udev_device_get_name(dev), "event"))
        return;
    GUdevDevice *parent = g_udev_device_get_parent(dev);
    const char *name = parent ? g_udev_device_get_sysfs_attr(parent, "name") : nullptr;
    const std::string n = name ? name : "";
    if (parent)
        g_object_unref(parent);
    if (n != TOUCH_NAME && n != "top_touchscreen" && n != "Barry Launcher Trackpad")
        return;
    if (watching.count(node))
        return;
    const int fd = open(node, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        log("cannot read " + n + " (" + node + "): " + std::strerror(errno));
        return;
    }
    auto *w = new Watched;
    w->name = n;
    w->node = node;
    w->fd = fd;
    w->source = g_unix_fd_add(fd, GIOCondition(G_IO_IN | G_IO_ERR | G_IO_HUP), onDeviceReadable, w);
    watching[node] = w;
    if (n == TOUCH_NAME) {
        PANEL.readAxes(fd);
        if (!T.last)
            T.last = 0.0;  // a touchscreen is there: touches count from now
        log(std::string("watching touches on ") + node);
    } else {
        log("watching presses on " + n);
    }
}

void onUevent(GUdevClient *, const gchar *action, GUdevDevice *dev, gpointer)
{
    if (g_strcmp0(action, "add") == 0)
        watchDevice(dev);
}

// ---- AT-SPI --------------------------------------------------------------

AtspiAccessible *textAcc = nullptr;  // the focused editable field, if any

void setTextAcc(AtspiAccessible *acc)
{
    if (acc)
        g_object_ref(acc);
    if (textAcc)
        g_object_unref(textAcc);
    textAcc = acc;
}

std::optional<AtspiRect> extents(AtspiAccessible *acc)
{
    AtspiComponent *c = atspi_accessible_get_component_iface(acc);
    if (!c)
        return std::nullopt;
    GError *err = nullptr;
    AtspiRect *r = atspi_component_get_extents(c, ATSPI_COORD_TYPE_SCREEN, &err);
    g_object_unref(c);
    if (!r) {
        g_clear_error(&err);
        return std::nullopt;
    }
    AtspiRect copy = *r;
    g_free(r);
    return copy;
}

// The field's app is frozen (shelld freezes apps left in the background):
// an AT-SPI call to it would hang until libatspi's timeout, on every tap.
// Its cgroup says so (the bus daemon answers for the pid, not the app).
bool frozen(AtspiAccessible *acc)
{
    GError *err = nullptr;
    const guint pid = atspi_accessible_get_process_id(acc, &err);
    g_clear_error(&err);
    if (!pid)
        return false;
    std::ifstream cg("/proc/" + std::to_string(pid) + "/cgroup");
    std::string line;
    while (std::getline(cg, line)) {
        if (line.rfind("0::", 0) != 0)
            continue;
        std::ifstream ev("/sys/fs/cgroup" + line.substr(3) + "/cgroup.events");
        for (std::string e; std::getline(ev, e);)
            if (e == "frozen 1")
                return true;
        return false;
    }
    return false;
}

bool hasState(AtspiAccessible *acc, AtspiStateType st)
{
    AtspiStateSet *set = atspi_accessible_get_state_set(acc);
    if (!set)
        return false;
    const bool has = atspi_state_set_contains(set, st);
    g_object_unref(set);
    return has;
}

// (editable, in the lower half of the screen); nullopt if it cannot be read.
std::optional<std::pair<bool, bool>> textField(AtspiAccessible *acc)
{
    AtspiStateSet *set = atspi_accessible_get_state_set(acc);
    if (!set)
        return std::nullopt;
    const bool editable = atspi_state_set_contains(set, ATSPI_STATE_EDITABLE)
                          && !atspi_state_set_contains(set, ATSPI_STATE_READ_ONLY);
    g_object_unref(set);
    bool lower = false;
    if (editable) {
        if (const auto e = extents(acc))
            lower = e->y + e->height / 2.0 > SCREEN_H * 0.5;
    }
    return std::make_pair(editable, lower);
}

std::string procFile(guint pid, const char *what)
{
    gchar *data = nullptr;
    gsize len = 0;
    const std::string path = "/proc/" + std::to_string(pid) + "/" + what;
    if (!g_file_get_contents(path.c_str(), &data, &len, nullptr))
        return {};
    std::string s(data, len);
    g_free(data);
    return s;
}

std::optional<guint> pidOf(AtspiAccessible *acc)
{
    GError *err = nullptr;
    const guint pid = atspi_accessible_get_process_id(acc, &err);
    if (err) {
        g_clear_error(&err);
        return std::nullopt;
    }
    return pid;
}

// Game Mode: the accessibility bus is the whole session's, so a field in a
// top-screen app (an emulator's text dialog, say) comes by too, and a finger
// typing on the bottom screen then made it look tapped. Only apps on this
// screen's X display count; one whose display cannot be read (an app's bus
// proxy) does.
bool onThisScreen(AtspiAccessible *acc)
{
    if (DESKTOP)
        return true;
    const auto pid = pidOf(acc);
    if (!pid)
        return true;
    const std::string environ = procFile(*pid, "environ");
    if (environ.empty())
        return true;
    const std::string mine = env("DISPLAY", "");
    size_t i = 0;
    while (i < environ.size()) {
        const size_t end = environ.find('\0', i);
        const std::string var = environ.substr(i, end == std::string::npos ? std::string::npos : end - i);
        if (var.compare(0, 8, "DISPLAY=") == 0)
            return var.substr(8) == mine;
        if (end == std::string::npos)
            break;
        i = end + 1;
    }
    return true;
}

bool touchedLately()
{
    return !T.last || now() - *T.last < TOUCH_S;
}

// Did the latest touch come down on this field? (True when there is no
// telling: no touchscreen, or no position yet.) An app moving focus by
// itself right after a tap elsewhere (WhatsApp, to its message box, after a
// tap on a chat) is no tap on the field.
bool fingerOn(AtspiAccessible *acc)
{
    if (!T.at)
        return true;
    const auto e = extents(acc);
    if (!e || e->width <= 0 || e->height <= 0)
        return true;
    const int x = T.at->first, y = T.at->second;
    return e->x - FIELD_SLOP <= x && x < e->x + e->width + FIELD_SLOP
           && e->y - FIELD_SLOP <= y && y < e->y + e->height + FIELD_SLOP;
}

std::string fingerReport(AtspiAccessible *acc)
{
    const auto e = extents(acc);
    if (!e)
        return {};
    std::string at = T.at ? "(" + std::to_string(T.at->first) + ", " + std::to_string(T.at->second) + ")" : "None";
    return " (touch " + at + ", field " + std::to_string(e->x) + "," + std::to_string(e->y) + " "
           + std::to_string(e->width) + "x" + std::to_string(e->height) + ")";
}

// A field of the Firefox app's own window (its address bar), not of a page.
// Firefox's home page search box is no field: a tap on it hands focus to the
// address bar, and so does the new-tab button.
bool addressBar(AtspiAccessible *acc, const std::string &profile)
{
    const auto pid = pidOf(acc);
    if (!pid || profile.empty())
        return false;
    const std::string cmdline = procFile(*pid, "cmdline");
    GError *err = nullptr;
    AtspiAccessible *node = atspi_accessible_get_parent(acc, &err);
    while (node) {
        const AtspiRole role = atspi_accessible_get_role(node, &err);
        if (err) {
            g_clear_error(&err);
            g_object_unref(node);
            return false;
        }
        if (role == ATSPI_ROLE_DOCUMENT_WEB) {
            g_object_unref(node);
            return false;
        }
        AtspiAccessible *up = atspi_accessible_get_parent(node, &err);
        g_object_unref(node);
        node = up;
    }
    g_clear_error(&err);
    bool kiosk = false, ours = false;
    size_t i = 0;
    while (i < cmdline.size()) {
        const size_t end = cmdline.find('\0', i);
        const std::string arg = cmdline.substr(i, end == std::string::npos ? std::string::npos : end - i);
        kiosk |= arg == "--kiosk";
        ours |= arg == profile;
        if (end == std::string::npos)
            break;
        i = end + 1;
    }
    return !kiosk && ours;
}

// The keyboard pushed the app up, but the focused field can still be under
// it: an app (WhatsApp Web) that will not lay itself out shorter than some
// height is cut off at the bottom instead. Scroll the field's bar (the
// small box around it, with its buttons) into view, just above the keyboard.
gboolean revealField(gpointer)
{
    if (!textAcc || DESKTOP || frozen(textAcc))
        return G_SOURCE_REMOVE;
    const Shell s = shell();
    if (!s.ok || !s.visible)
        return G_SOURCE_REMOVE;
    const int above = SCREEN_H - s.keyboardHeight;
    if (!hasState(textAcc, ATSPI_STATE_FOCUSED))
        return G_SOURCE_REMOVE;
    const auto e = extents(textAcc);
    if (!e || e->height <= 0 || e->y + e->height <= above)
        return G_SOURCE_REMOVE;  // in sight
    GError *err = nullptr;
    AtspiAccessible *bar = static_cast<AtspiAccessible *>(g_object_ref(textAcc));
    AtspiAccessible *parent = atspi_accessible_get_parent(bar, &err);
    while (parent) {
        const auto pe = extents(parent);
        if (!pe || pe->height > REVEAL_ROW_MAX || pe->height <= 0) {
            g_object_unref(parent);
            break;
        }
        g_object_unref(bar);
        bar = parent;
        parent = atspi_accessible_get_parent(bar, &err);
    }
    g_clear_error(&err);
    if (AtspiComponent *c = atspi_accessible_get_component_iface(bar)) {
        if (atspi_component_scroll_to(c, ATSPI_SCROLL_BOTTOM_EDGE, &err))
            log("the focused field was under the keyboard: scrolled into view");
        g_clear_error(&err);
        g_object_unref(c);
    }
    g_object_unref(bar);
    return G_SOURCE_REMOVE;
}

void showForText(bool lower, const std::string &why)
{
    const Shell s = shell();
    if (!s.ok) {
        log(why + ": barry_launcher_shelld does not answer");
        return;
    }
    if (T.began && *T.began < s.hiddenByHand) {
        // The touch that put the keyboard away (its ⌄ key, over a field the
        // keyboard covers, say) is no tap on that field, and the caret moves
        // and focus changes it brings are not either: only a new touch
        // brings the keyboard back.
        log(why + " from the touch that hid the keyboard: no keyboard");
        return;
    }
    if (now() < s.quietUntil) {
        log(why + " while an app opens: no keyboard");
        return;
    }
    if (!touchedLately()) {
        log(why + " without a touch: no keyboard");
        return;
    }
    if (!DESKTOP && s.topInputRunning) {
        // The Trackpad app fills the screen: no field of ours is in sight,
        // and this keyboard would squeeze that app above it.
        log(why + " under the top screen's trackpad: no keyboard");
        return;
    }
    if (!s.visible)
        log(why + (lower ? " (lower half)" : "") + ": keyboard");
    // Always at the bottom: the app moves up above it (shelld's inset).
    tell("show", why);
    for (unsigned ms : REVEAL_MS)
        g_timeout_add(ms, revealField, nullptr);
}

// ---- Desktop Mode: KWin's input method -------------------------------------

struct InputMethod {
    GSocketConnection *conn = nullptr;
    guint source = 0;
    std::string buf;
    bool active = false;
    std::optional<std::string> text;  // around the caret
    long cursor = 0;                  // bytes into text
    double missingSince = 0;
    double kicked = -1e9;
    bool top = false;                 // the active field was clicked on the top screen
    bool leftByClick = false;         // a top-screen field went inactive right after a click
} IM;

bool imDecides()
{
    return IM.conn != nullptr;
}

bool topClickedLately()
{
    // Desktop Mode: the latest press was a trackpad click or a finger on the
    // top screen (not on the bottom one), just now.
    if (!T.lastTopClick || now() - *T.lastTopClick >= TOP_CLICK_S)
        return false;
    return !T.began || *T.lastTopClick > *T.began;
}

gboolean hideIfStillInactive(gpointer)
{
    if (imDecides() && !IM.active) {
        const Shell s = shell();
        if (s.ok && s.visible) {
            log("text field inactive");
            tell("hide", "text field inactive");
        }
        // A click beside a top-screen field (no field active after it): the
        // Trackpad app goes back to the full trackpad, as the bottom keyboard
        // goes away. Only after a click: a field losing focus by itself while
        // the user types (a list of suggestions, say) keeps the keys.
        if (IM.leftByClick && s.ok && s.topKeyboardVisible)
            tell("top-down", "click beside the top-screen field");
    }
    IM.leftByClick = false;
    return G_SOURCE_REMOVE;
}

void onImEvent(JsonObject *ev)
{
    const std::string kind = json_object_get_string_member_with_default(ev, "event", "");
    if (TIMING && (kind == "activate" || kind == "deactivate" || kind == "surrounding"))
        log("timing: field " + kind + " " + std::to_string(now()));
    if (kind == "activate") {
        IM.active = true;
        IM.text.reset();
        IM.cursor = 0;
        IM.top = topClickedLately();
        if (IM.top)
            tell("top", "top-screen text field active");
        else
            showForText(false, "text field active");
    } else if (kind == "deactivate") {
        IM.leftByClick = IM.top && topClickedLately();
        IM.active = false;
        IM.text.reset();
        IM.cursor = 0;
        g_timeout_add(IM_HIDE_DELAY_MS, hideIfStillInactive, nullptr);
    } else if (kind == "surrounding") {
        IM.text = json_object_get_string_member_with_default(ev, "text", "");
        IM.cursor = long(json_object_get_int_member_with_default(ev, "cursor", 0));
        // A tap in the field that is already active moves its caret: show a
        // keyboard hidden by hand again.
        if (!IM.active)
            return;
        if (IM.top) {
            if (topClickedLately())
                tell("top", "caret moved in the top-screen field");
        } else if (touchedLately()) {
            const Shell s = shell();
            if (s.ok && !s.visible)
                showForText(false, "caret moved in the active field");
        }
    }
}

void imDrop()
{
    if (IM.source)
        g_source_remove(IM.source);
    IM.source = 0;
    if (IM.conn)
        g_object_unref(IM.conn);
    IM.conn = nullptr;
    IM.buf.clear();
    IM.active = false;
    IM.text.reset();
    IM.missingSince = now();
}

gboolean onImReadable(gint, GIOCondition cond, gpointer)
{
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(IM.conn));
    char data[4096];
    GError *err = nullptr;
    const gssize n = (cond & G_IO_IN) ? g_pollable_input_stream_read_nonblocking(
                                            G_POLLABLE_INPUT_STREAM(in), data, sizeof data, nullptr, &err)
                                      : 0;
    if (n <= 0) {
        const bool again = err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK);
        g_clear_error(&err);
        if (again)
            return G_SOURCE_CONTINUE;
        log("barry_launcher_imd went away");
        IM.source = 0;  // removed by returning
        imDrop();
        return G_SOURCE_REMOVE;
    }
    IM.buf.append(data, size_t(n));
    size_t nl;
    while ((nl = IM.buf.find('\n')) != std::string::npos) {
        const std::string line = IM.buf.substr(0, nl);
        IM.buf.erase(0, nl + 1);
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, line.c_str(), -1, nullptr)) {
            JsonNode *root = json_parser_get_root(p);
            if (root && JSON_NODE_HOLDS_OBJECT(root))
                onImEvent(json_node_get_object(root));
        }
        g_object_unref(p);
    }
    return G_SOURCE_CONTINUE;
}

// KWin starts its input method again when the setting changes: taken away
// and given back, as Barry's off and on switch does. KWin stopped it when
// Barry was turned off just after Desktop Mode started, and did not start it
// when Barry came back (2026-10-18): without it no field brought the
// keyboard up.
void restartInputMethod()
{
    log("barry_launcher_imd is gone: KWin is asked to start it again");
    const char *del[] = {"kwriteconfig6", "--notify", "--file", "kwinrc", "--group", "Wayland",
                         "--key", "InputMethod", "--delete", nullptr};
    const char *set[] = {"kwriteconfig6", "--notify", "--file", "kwinrc", "--group", "Wayland",
                         "--key", "InputMethod", IM_ENTRY, nullptr};
    for (const char **argv : {del, set}) {
        GError *err = nullptr;
        if (!g_spawn_sync(nullptr, const_cast<gchar **>(argv), nullptr, G_SPAWN_SEARCH_PATH, nullptr, nullptr,
                          nullptr, nullptr, nullptr, &err)) {
            log(std::string("cannot change KWin's input method: ") + err->message);
            g_clear_error(&err);
            return;
        }
        g_usleep(G_USEC_PER_SEC);
    }
}

// Every second while not connected (the socket appears when KWin starts
// barry_launcher_imd).
gboolean imConnect(gpointer)
{
    if (IM.conn)
        return G_SOURCE_CONTINUE;
    const std::string path = env("XDG_RUNTIME_DIR", "/tmp") + "/barry_launcher_im.sock";
    GSocketAddress *addr = g_unix_socket_address_new(path.c_str());
    GSocketClient *client = g_socket_client_new();
    GError *err = nullptr;
    GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), nullptr, &err);
    g_object_unref(client);
    g_object_unref(addr);
    if (!conn) {
        g_clear_error(&err);
        const double t = now();
        if (t - IM.missingSince >= IM_KICK_S && t - IM.kicked >= IM_KICK_EVERY_S) {
            IM.kicked = t;
            restartInputMethod();
        }
        return G_SOURCE_CONTINUE;
    }
    IM.conn = conn;
    GSocket *sock = g_socket_connection_get_socket(conn);
    g_socket_set_blocking(sock, FALSE);
    IM.source = g_unix_fd_add(g_socket_get_fd(sock), GIOCondition(G_IO_IN | G_IO_ERR | G_IO_HUP), onImReadable,
                              nullptr);
    log("connected to barry_launcher_imd");
    return G_SOURCE_CONTINUE;
}

// ---- focus, caret, taps ------------------------------------------------------

gboolean hideAfterFocusOut(gpointer)
{
    // Focus left the field: hide the keyboard, unless a field took focus
    // since. Signal moves focus out of its message box and back after Send.
    if (!textAcc) {
        const Shell s = shell();
        if (s.ok && s.visible) {
            log("text focus out");
            tell("hide", "text focus out");
        }
    }
    return G_SOURCE_REMOVE;
}

// Game Mode, a field of a top-screen app (an emulator's text dialog, say):
// the Trackpad app's keyboard. Only a click: games and apps focus and move
// carets by themselves too. Focus leaving a field does not put it away,
// since typing can move focus (a list of suggestions, say): the ⌄ key does.
void topTextFocus(AtspiAccessible *acc, const std::string &why)
{
    if (DESKTOP)
        return;
    const Shell s = shell();
    if (!s.ok || !s.topInputRunning)
        return;
    if (s.topKeyboardVisible && why.compare(0, 5, "caret") == 0)
        return;  // typing moves the caret all the time
    if (!T.lastTopClick || now() - *T.lastTopClick >= TOP_CLICK_S)
        return;
    const auto f = textField(acc);
    if (f && f->first)
        tell("top", why);
}

void onFocus(AtspiEvent *event, void *)
{
    if (imDecides() || event->detail1 != 1)
        return;  // KWin says, or the next focus gain decides
    AtspiAccessible *acc = event->source;
    if (!onThisScreen(acc)) {
        topTextFocus(acc, "top-screen text focus in");
        return;
    }
    const auto f = textField(acc);
    if (!f)
        return;
    if (f->first) {
        setTextAcc(acc);
        std::string why = "text focus in";
        const Shell s = shell();
        if (s.ok && !s.visible && !fingerOn(acc)) {
            if (!addressBar(acc, s.browserProfile)) {
                log("text focus in, away from the finger" + fingerReport(acc) + ": no keyboard");
                return;
            }
            why = "text focus in the address bar";
        }
        showForText(f->second, why);
        return;
    }
    setTextAcc(nullptr);
    const Shell s = shell();
    if (s.ok && s.visible)
        g_timeout_add(FOCUS_OUT_HIDE_MS, hideAfterFocusOut, nullptr);
}

void onCaret(AtspiEvent *event, void *)
{
    // A tap in a field that already has focus moves only its caret.
    if (imDecides())
        return;
    {
        const Shell s = shell();
        if (!s.ok || s.visible)
            return;
    }
    AtspiAccessible *acc = event->source;
    if (!hasState(acc, ATSPI_STATE_FOCUSED))
        return;
    if (!onThisScreen(acc)) {
        topTextFocus(acc, "caret moved in a top-screen field");
        return;
    }
    const auto f = textField(acc);
    if (!f)
        return;
    if (f->first)
        setTextAcc(acc);
    if (f->first && touchedLately() && fingerOn(acc))  // apps move their own carets all the time
        showForText(f->second, "caret moved in a focused field");
}

// A one-finger tap ended: on the focused field with the keyboard hidden (by
// hand, say), show it again.
void onTap(int rawX, int rawY)
{
    if (imDecides() || !textAcc || frozen(textAcc))
        return;
    if (!hasState(textAcc, ATSPI_STATE_FOCUSED))
        return;
    const auto f = textField(textAcc);
    const auto e = extents(textAcc);
    if (!f || !f->first || !e)
        return;
    const auto [sx, sy] = rawToScreen(rawX, rawY);
    if (!(e->x <= sx && sx < e->x + e->width && e->y <= sy && sy < e->y + e->height))
        return;
    const Shell s = shell();
    if (s.ok && !s.visible)
        showForText(f->second, "tap on the focused field");
}

// ---- the text before the caret, for shelld's autocorrect -------------------

// The object that holds the caret's text. A rich text field (Signal's,
// Discord's: Chromium contenteditable) reports focus on its outer box, whose
// text is one placeholder per paragraph; the caret's text is in the
// paragraph under it, and in that paragraph's text under that.
AtspiAccessible *caretHolder(AtspiAccessible *acc)
{
    AtspiAccessible *cur = static_cast<AtspiAccessible *>(g_object_ref(acc));
    for (int depth = 0; depth < 8; depth++) {
        GError *err = nullptr;
        const gint count = std::min(atspi_accessible_get_child_count(cur, &err), 64);
        g_clear_error(&err);
        AtspiAccessible *next = nullptr;
        for (gint i = 0; i < count && !next; i++) {
            AtspiAccessible *child = atspi_accessible_get_child_at_index(cur, i, &err);
            g_clear_error(&err);
            if (!child)
                continue;
            if (AtspiText *t = atspi_accessible_get_text_iface(child)) {
                if (atspi_text_get_caret_offset(t, &err) >= 0)
                    next = static_cast<AtspiAccessible *>(g_object_ref(child));
                g_clear_error(&err);
                g_object_unref(t);
            }
            g_object_unref(child);
        }
        if (!next)
            return cur;
        g_object_unref(cur);
        cur = next;
    }
    return cur;
}

// The text around the caret in the field, up to before characters before it
// and after characters after it: from KWin's input method in Desktop Mode,
// else over AT-SPI. nullopt if they cannot be read.
struct AroundCaret {
    std::string before, after;
};
std::optional<AroundCaret> aroundCaret(long before, long after)
{
    if (IM.conn && IM.active) {
        if (!IM.text)
            return std::nullopt;
        const size_t cursor = size_t(std::clamp(IM.cursor, 0L, long(IM.text->size())));
        // Characters, not bytes: walk n UTF-8 characters each way.
        const char *start = IM.text->c_str();
        const char *end = start + IM.text->size();
        const char *b = start + cursor;
        for (long i = 0; i < before; i++) {
            const char *prev = g_utf8_find_prev_char(start, b);
            if (!prev)
                break;
            b = prev;
        }
        const char *a = start + cursor;
        for (long i = 0; i < after && a < end; i++) {
            const char *next = g_utf8_find_next_char(a, end);
            a = next ? next : end;
        }
        return AroundCaret{std::string(b, start + cursor), std::string(start + cursor, a)};
    }
    if (!textAcc || frozen(textAcc))
        return std::nullopt;
    AtspiAccessible *holder = caretHolder(textAcc);
    std::optional<AroundCaret> result;
    if (AtspiText *t = atspi_accessible_get_text_iface(holder)) {
        GError *err = nullptr;
        const gint offset = atspi_text_get_caret_offset(t, &err);
        g_clear_error(&err);
        if (offset >= 0) {
            AroundCaret r;
            bool ok = true;
            if (before > 0) {
                gchar *s = atspi_text_get_text(t, std::max<gint>(0, offset - gint(before)), offset, &err);
                ok = s != nullptr;
                if (s)
                    r.before = s;
                g_free(s);
                g_clear_error(&err);
            }
            if (ok && after > 0) {
                const gint count = atspi_text_get_character_count(t, &err);
                g_clear_error(&err);
                const gint to = count >= 0 ? std::min<gint>(count, offset + gint(after)) : offset + gint(after);
                if (to > offset) {
                    gchar *s = atspi_text_get_text(t, offset, to, &err);
                    ok = s != nullptr;
                    if (s)
                        r.after = s;
                    g_free(s);
                    g_clear_error(&err);
                }
            }
            if (ok)
                result = std::move(r);
        }
        g_object_unref(t);
    }
    g_object_unref(holder);
    return result;
}

gboolean onQuery(GSocketService *, GSocketConnection *conn, GObject *, gpointer)
{
    GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(conn));
    GDataInputStream *din = g_data_input_stream_new(in);
    gchar *line = g_data_input_stream_read_line(din, nullptr, nullptr, nullptr);
    std::string reply = "{\"text\": null}";
    auto count = [](const char *p) { return std::clamp(std::atol(p), 0L, 200L); };
    if (line && g_str_has_prefix(line, "BEFORE ")) {
        if (const auto r = aroundCaret(count(line + 7), 0))
            reply = "{\"text\": " + jsonString(r->before) + "}";
    } else if (line && g_str_has_prefix(line, "AFTER ")) {
        if (const auto r = aroundCaret(0, count(line + 6)))
            reply = "{\"text\": " + jsonString(r->after) + "}";
    } else if (line && g_str_has_prefix(line, "AROUND ")) {
        char *rest = nullptr;
        const long b = std::clamp(std::strtol(line + 7, &rest, 10), 0L, 200L);
        if (const auto r = aroundCaret(b, count(rest)))
            reply = "{\"text\": " + jsonString(r->before) + ", \"after\": " + jsonString(r->after) + "}";
    }
    reply += "\n";
    g_output_stream_write_all(out, reply.data(), reply.size(), nullptr, nullptr, nullptr);
    g_free(line);
    g_object_unref(din);
    return TRUE;
}

// ---- start ---------------------------------------------------------------------

// Tell apps an assistive technology is listening (like GNOME's switch).
void enableAccessibility()
{
    GError *err = nullptr;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!bus) {
        log(std::string("cannot enable accessibility: ") + err->message);
        g_clear_error(&err);
        return;
    }
    GVariant *r = g_dbus_connection_call_sync(
        bus, "org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties", "Set",
        g_variant_new("(ssv)", "org.a11y.Status", "IsEnabled", g_variant_new_boolean(TRUE)), nullptr,
        G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &err);
    if (r)
        g_variant_unref(r);
    else {
        log(std::string("cannot enable accessibility: ") + err->message);
        g_clear_error(&err);
    }
    g_object_unref(bus);
}

}  // namespace

int main()
{
    enableAccessibility();
    if (atspi_init() < 0)
        log("AT-SPI already set up");

    // The query socket, before anything asks.
    const std::string sockPath = env("XDG_RUNTIME_DIR", "/tmp") + "/barry_launcher_keyd.sock";
    unlink(sockPath.c_str());
    GSocketService *service = g_socket_service_new();
    GSocketAddress *addr = g_unix_socket_address_new(sockPath.c_str());
    GError *err = nullptr;
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service), addr, G_SOCKET_TYPE_STREAM,
                                       G_SOCKET_PROTOCOL_DEFAULT, nullptr, nullptr, &err)) {
        log(std::string("cannot listen on ") + sockPath + ": " + err->message);
        g_clear_error(&err);
    }
    g_object_unref(addr);
    g_signal_connect(service, "incoming", G_CALLBACK(onQuery), nullptr);
    g_socket_service_start(service);

    AtspiEventListener *focus = atspi_event_listener_new(onFocus, nullptr, nullptr);
    atspi_event_listener_register(focus, "object:state-changed:focused", &err);
    g_clear_error(&err);
    AtspiEventListener *caret = atspi_event_listener_new(onCaret, nullptr, nullptr);
    atspi_event_listener_register(caret, "object:text-caret-moved", &err);
    g_clear_error(&err);

    // The touchscreens and the trackpad: those there now, and as they come.
    const gchar *subsystems[] = {"input", nullptr};
    GUdevClient *udev = g_udev_client_new(subsystems);
    g_signal_connect(udev, "uevent", G_CALLBACK(onUevent), nullptr);
    GList *devices = g_udev_client_query_by_subsystem(udev, "input");
    for (GList *l = devices; l; l = l->next) {
        watchDevice(G_UDEV_DEVICE(l->data));
        g_object_unref(l->data);
    }
    g_list_free(devices);
    bool found = false;
    for (const auto &[node, w] : watching)
        found |= w->name == TOUCH_NAME;
    if (!found)
        log(std::string("no ") + TOUCH_NAME + " input device; the keyboard follows focus alone");

    if (DESKTOP) {
        IM.missingSince = now();
        imConnect(nullptr);
        g_timeout_add_seconds(1, imConnect, nullptr);
    }

    log(std::string("ready") + (DESKTOP ? " (Desktop Mode)" : ""));
    atspi_event_main();
    return 0;
}

const manifest = {"name":"Barry Launcher"};
const API_VERSION = 2;
const internalAPIConnection = window.__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit;
if (!internalAPIConnection) {
    throw new Error('[@decky/api]: Failed to connect to the loader as as the loader API was not initialized. This is likely a bug in Decky Loader.');
}
let api;
try {
    api = internalAPIConnection.connect(API_VERSION, manifest.name);
}
catch {
    api = internalAPIConnection.connect(1, manifest.name);
}
const callable = api.callable;
const definePlugin = (fn) => (...args) => fn(...args);

const DFL = window.DFL;
const SP_REACT = window.SP_REACT;
const SP_JSX = window.SP_JSX;
const { useEffect, useState, useCallback, useRef } = SP_REACT;
const jsx = SP_JSX.jsx;
const jsxs = SP_JSX.jsxs;

// Decky callable(): arguments are passed positionally to the Python method.
const getState = callable("get_state");
const setBottomScreen = callable("set_bottom_screen");
const setDimmers = callable("set_dimmers");
const setRgbDimmer = callable("set_rgb_dimmer");
const barryKeyboardAvailable = callable("barry_keyboard_available");
const getTwoScreen = callable("get_two_screen");
const setTwoScreen = callable("set_two_screen");
const openBarryKeyboard = callable("open_barry_keyboard");
const getBarryApps = callable("get_barry_apps");
const setBarryApps = callable("set_barry_apps");
const barryAppsFolder = callable("barry_apps_folder");
const checkBarryApp = callable("check_barry_app");
const installBarryApp = callable("install_barry_app");
const removeBarryApp = callable("remove_barry_app");
const getGameLinks = callable("get_game_links");
const setGameLinks = callable("set_game_links");
const gameEvent = callable("game_event");
// Decky's file browser (select 0: a file), as @decky/api's openFilePicker.
const ARCHIVE_EXTENSIONS = ["zip", "gz", "tgz", "xz", "txz", "bz2", "tbz2", "tar"];

// Steam's on-screen keyboard never shows on the top screen: whenever Steam
// would open it (a text field tapped or picked with the controller, Steam+X),
// Barry Launcher's Keyboard app opens on the bottom screen instead, and
// typing there goes to the field. Every way Steam opens its keyboard goes
// through its keyboard manager's SetVirtualKeyboardShownInternal(show), which
// this wraps. While the bottom screen is off or Barry Launcher is not
// running, Steam's keyboard works as before; so it does when Barry Launcher's
// keyboard cannot be seen (a dual-screen game keeping the bottom screen).
//
// Steam opens no keyboard for a mouse click on a text field, though, and
// Barry Launcher's trackpad is a mouse: its clicks on a field (Game Mode's
// search box, say) are watched for in Steam's own windows and open the
// keyboard too.
let barryAvailable = false;
let lastOpen = 0;
let unhookKeyboard = null;

function keyboardManager() {
    const inst = window.SteamUIStore && window.SteamUIStore.ActiveWindowInstance;
    return inst ? inst.VirtualKeyboardManager || inst.m_VirtualKeyboardManager : null;
}

function showBarryKeyboard(showSteams) {
    // Steam may ask several times for one tap; bringing the app forward
    // again remaps its window, so once is enough.
    const now = Date.now();
    if (now - lastOpen < 1500) return;
    lastOpen = now;
    openBarryKeyboard()
        .then((shown) => { if (!shown) showSteams(); })
        .catch(() => showSteams());
}

function hookKeyboard() {
    const mgr = keyboardManager();
    if (!mgr) return null;
    const proto = Object.getPrototypeOf(mgr);
    const orig = proto.SetVirtualKeyboardShownInternal;
    if (typeof orig !== "function" || orig.barryWrapped) return null;
    const wrapped = function (show, ...rest) {
        if (show && barryAvailable) {
            showBarryKeyboard(() => orig.call(this, show, ...rest));
            return;
        }
        return orig.call(this, show, ...rest);
    };
    wrapped.barryWrapped = true;
    proto.SetVirtualKeyboardShownInternal = wrapped;
    return () => { proto.SetVirtualKeyboardShownInternal = orig; };
}

// Steam's windows (Game Mode, its menus, Quick Access) as browser windows.
function steamWindows() {
    const wins = new Set();
    const add = (w) => { if (w && w.document) wins.add(w); };
    const store = window.SteamUIStore;
    if (store) {
        add(store.ActiveWindowInstance && store.ActiveWindowInstance.BrowserWindow);
        const all = store.WindowStore && store.WindowStore.SteamUIWindows;
        if (all) for (const inst of all) add(inst && inst.BrowserWindow);
    }
    try { if (typeof DFL.findSP === "function") add(DFL.findSP()); } catch (e) { /* not up yet */ }
    return wins;
}

const TEXT_TYPES = ["", "text", "search", "password", "email", "url", "tel", "number"];

function textField(el) {
    for (let n = el; n && n.nodeType === 1; n = n.parentElement) {
        const tag = n.tagName;
        if (tag === "TEXTAREA") return !n.readOnly && !n.disabled;
        if (tag === "INPUT") return TEXT_TYPES.includes((n.getAttribute("type") || "").toLowerCase()) && !n.readOnly && !n.disabled;
        if (n.isContentEditable) return true;
    }
    return false;
}

function onPointerDown(e) {
    if (e.pointerType !== "mouse" || e.button !== 0 || !barryAvailable) return;
    if (textField(e.target)) showBarryKeyboard(() => {});  // no Steam keyboard for a click anyway
}

const watchedDocs = new Set();

function watchClicks() {
    for (const w of steamWindows()) {
        const doc = w.document;
        if (watchedDocs.has(doc)) continue;
        doc.addEventListener("pointerdown", onPointerDown, true);
        watchedDocs.add(doc);
    }
}

function unwatchClicks() {
    for (const doc of watchedDocs) doc.removeEventListener("pointerdown", onPointerDown, true);
    watchedDocs.clear();
}

// Apps that open with games: Steam's running games (its own and non-Steam
// shortcuts alike, by appid), told to Barry Launcher as they start and
// stop; it opens and closes the linked apps. Games already running when
// the plugin loads are not news.
let gamesSeen = null;

function runningGames() {
    const apps = (DFL.Router && DFL.Router.RunningApps) || [];
    return new Set(apps.map((a) => String(a.appid)));
}

function watchGames() {
    const now = runningGames();
    if (gamesSeen) {
        for (const id of now) if (!gamesSeen.has(id)) gameEvent(id, true).catch(() => {});
        for (const id of gamesSeen) if (!now.has(id)) gameEvent(id, false).catch(() => {});
    }
    gamesSeen = now;
}

// Games to link: those running and the most recently played (Steam's and
// non-Steam), by name, as [{ id, name, running }].
const STEAM_GAME = 1, NON_STEAM_GAME = 1073741824;
function linkableGames() {
    const out = [], seen = new Set();
    const add = (id, name, running) => {
        id = String(id);
        if (!seen.has(id) && name) { seen.add(id); out.push({ id, name, running }); }
    };
    for (const a of (DFL.Router && DFL.Router.RunningApps) || []) add(a.appid, a.display_name, true);
    const all = (window.collectionStore && collectionStore.allAppsCollection && collectionStore.allAppsCollection.allApps) || [];
    all.filter((a) => (a.app_type === STEAM_GAME || a.app_type === NON_STEAM_GAME) && a.rt_last_time_played > 0)
        .sort((a, b) => b.rt_last_time_played - a.rt_last_time_played)
        .slice(0, 40)
        .forEach((a) => add(a.appid, a.display_name, false));
    return out.sort(byName);
}

function byName(a, b) {
    return a.name.localeCompare(b.name, undefined, { sensitivity: "base", numeric: true });
}

function steamKeyboardShowing(mgr) {
    // A subscribable value ({ Value, m_currentValue }) in current Steam builds.
    const v = mgr.IsShowingVirtualKeyboard;
    if (typeof v === "function") return !!v.call(mgr);
    if (v && typeof v === "object") return !!(v.Value !== undefined ? v.Value : v.m_currentValue);
    return !!v;
}

async function watchBarryKeyboard() {
    if (!unhookKeyboard) unhookKeyboard = hookKeyboard();
    watchClicks();  // windows Steam opened since
    const available = await barryKeyboardAvailable().catch(() => false);
    if (available && !barryAvailable) {
        // A Steam keyboard left up from before goes away.
        const mgr = keyboardManager();
        if (mgr && steamKeyboardShowing(mgr)) mgr.SetVirtualKeyboardHidden();
    }
    barryAvailable = available;
}

const row = (child) => jsx(DFL.PanelSectionRow, { children: child });

// Tab icons: outlines in the text colour, 24x24.
const svg = (children) => jsx("svg", {
    width: 22, height: 22, viewBox: "0 0 24 24", fill: "none", stroke: "currentColor",
    strokeWidth: 1.6, strokeLinecap: "round", strokeLinejoin: "round", children,
});
// The Thor, open: the top screen above the hinge, the bottom screen between
// the sticks.
const ThorIcon = () => svg([
    jsx("rect", { x: 4, y: 1.5, width: 16, height: 9.5, rx: 1.5 }, "lid"),
    jsx("rect", { x: 6, y: 3.2, width: 12, height: 6.1, rx: 0.5 }, "top"),
    jsx("path", { d: "M3 12.5h18v6.5a3.5 3.5 0 0 1-3.5 3.5h-11A3.5 3.5 0 0 1 3 19z" }, "body"),
    jsx("rect", { x: 9, y: 14, width: 6, height: 5, rx: 0.5 }, "bottom"),
    jsx("circle", { cx: 6, cy: 16.5, r: 1.3 }, "ls"),
    jsx("circle", { cx: 18, cy: 16.5, r: 1.3 }, "rs"),
]);
// A stick seen from above inside its ring of light, red, green and blue.
const RgbIcon = () => svg([
    jsx("circle", { cx: 12, cy: 12, r: 4.2 }, "stick"),
    jsx("circle", { cx: 12, cy: 12, r: 1.4, fill: "currentColor" }, "cap"),
    jsx("path", { d: "M12 3.5a8.5 8.5 0 0 1 7.36 4.25", stroke: "#ff4d4d", strokeWidth: 2.2 }, "r"),
    jsx("path", { d: "M19.36 16.25A8.5 8.5 0 0 1 12 20.5", stroke: "#4dd96b", strokeWidth: 2.2 }, "g"),
    jsx("path", { d: "M4.64 16.25A8.5 8.5 0 0 1 4.64 7.75", stroke: "#4d8dff", strokeWidth: 2.2 }, "b"),
]);
// A Game Boy: screen, d-pad, A and B.
const HandheldIcon = () => svg([
    jsx("path", { d: "M6.5 1.5h11a1.5 1.5 0 0 1 1.5 1.5v15.5a4 4 0 0 1-4 4H6.5A1.5 1.5 0 0 1 5 21V3a1.5 1.5 0 0 1 1.5-1.5z" }, "body"),
    jsx("rect", { x: 7.5, y: 4, width: 9, height: 7, rx: 0.5 }, "screen"),
    jsx("path", { d: "M8.5 16.5h3M10 15v3" }, "dpad"),
    jsx("circle", { cx: 14, cy: 17.6, r: 1.15, fill: "currentColor", stroke: "none" }, "b"),
    jsx("circle", { cx: 16.6, cy: 15.6, r: 1.15, fill: "currentColor", stroke: "none" }, "a"),
]);

// Barry Launcher's home: a 2x2 grid of app tiles.
// A gamepad: games and the apps that open with them.
const GamesIcon = () => svg([
    jsx("path", { d: "M7 7h10a5 5 0 0 1 4.6 7l-1 2.4a2.5 2.5 0 0 1 -4.3 .5l-1.3 -1.9h-6l-1.3 1.9a2.5 2.5 0 0 1 -4.3 -.5l-1 -2.4a5 5 0 0 1 4.6 -7z" }, "a"),
    jsx("path", { d: "M8 10v3M6.5 11.5h3" }, "b"),
    jsx("path", { d: "M15.5 10.5h.01M17.5 12.5h.01" }, "c"),
]);
const AppsIcon = () => svg([
    jsx("rect", { x: 3, y: 3, width: 7.5, height: 7.5, rx: 2 }, "a"),
    jsx("rect", { x: 13.5, y: 3, width: 7.5, height: 7.5, rx: 2 }, "b"),
    jsx("rect", { x: 3, y: 13.5, width: 7.5, height: 7.5, rx: 2 }, "c"),
    jsx("rect", { x: 13.5, y: 13.5, width: 7.5, height: 7.5, rx: 2 }, "d"),
]);

// LB and RB: the controller's bumpers, in Steam's button numbering.
const GB = DFL.GamepadButton || {};
const BUMPER_LEFT = GB.BUMPER_LEFT != null ? GB.BUMPER_LEFT : 5;
const BUMPER_RIGHT = GB.BUMPER_RIGHT != null ? GB.BUMPER_RIGHT : 6;

// The tab shown, kept while Steam runs.
let lastTab = "screens";

function TabBar({ tabs, active, onPick }) {
    return jsx(DFL.Focusable, {
        "flow-children": "horizontal",
        style: { display: "flex", gap: "6px", padding: "0 16px 8px" },
        children: tabs.map((t) => jsx(DFL.DialogButton, {
            onClick: () => onPick(t.id),
            style: {
                flex: 1, minWidth: 0, height: "40px", padding: 0,
                display: "flex", alignItems: "center", justifyContent: "center",
                ...(t.id === active
                    ? { background: "#1a9fff", color: "#fff" }
                    : { opacity: 0.8 }),
            },
            children: jsx(t.icon, {}),
        }, t.id)),
    });
}
const note = (text) => row(jsx("div", { style: { fontSize: "12px", opacity: 0.75 }, children: text }));

// Sliders send while they move, at most this often, and the last value
// always; the periodic refresh leaves them alone until a while after.
const SEND_MS = 120;
const HOLD_MS = 2000;

function useThrottled(send) {
    const t = useRef({ at: 0, timer: null, value: null });
    return useCallback((value) => {
        const s = t.current;
        s.value = value;
        if (s.timer) return;
        const wait = Math.max(0, s.at + SEND_MS - Date.now());
        s.timer = setTimeout(() => {
            s.timer = null;
            s.at = Date.now();
            send(s.value);
        }, wait);
    }, [send]);
}

// The user's emulators set to put their second screen in a window of its
// own, which gamescope shows on the bottom screen (the backend changes only
// their config files, and puts them back when this goes off).
function TwoScreenSection() {
    const [ts, setTs] = useState(null);
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const load = useCallback(() => { getTwoScreen().then(setTs).catch(() => {}); }, []);
    useEffect(() => {
        load();
        const t = setInterval(load, 5000);
        return () => clearInterval(t);
    }, [load]);
    if (!ts) return null;
    const found = ts.emulators || [];
    const names = [...new Set(found.map((e) => e.name))];
    const set = [...new Set(found.filter((e) => e.twoScreens).map((e) => e.name))];
    const description = !found.length
        ? "No melonDS, Azahar, Lime3DS, Citra or Cemu settings found yet: open the emulator once, then come back."
        : ts.enabled
            ? `Two screens: ${set.join(", ") || "none"}${set.length < names.length ? ` (${names.filter((n) => !set.includes(n)).join(", ")}: once it is closed)` : ""}.`
            : `Found: ${names.join(", ")}.`;
    return jsxs(DFL.PanelSection, { title: "Emulators", children: [
        row(jsx(DFL.ToggleField, {
            label: "Two-screen emulators",
            description,
            checked: !!ts.enabled,
            disabled: busy || !found.length,
            onChange: (v) => {
                setBusy(true);
                setErr("");
                setTwoScreen(v).then((r) => {
                    if (r && !r.ok) setErr(r.error || "Could not change the emulators' settings.");
                }).catch(() => setErr("Could not change the emulators' settings.")).finally(() => {
                    setBusy(false);
                    load();
                });
            },
        })),
        err && note(err),
        ts.dsOnRetroArch > 0 && note(`${ts.dsOnRetroArch} DS game${ts.dsOnRetroArch === 1 ? "" : "s"} in Steam start${ts.dsOnRetroArch === 1 ? "s" : ""} in RetroArch, which shows both screens on the top one. To use the bottom screen, in Desktop Mode open Steam ROM Manager, turn off the RetroArch DS parser, turn on "Nintendo DS - melonDS (Standalone)" and add the games again. Copy your saves over first: they stay with RetroArch.`),
    ] });
}

// Barry Launcher's home screen: each app shown or hidden, and moved up or
// down (barry_launcher_shelld keeps them; the home screen follows within a
// second).
function AppsSection() {
    const [apps, setApps] = useState(null);
    const [err, setErr] = useState("");
    const [msg, setMsg] = useState("");
    const [busy, setBusy] = useState(false);
    const load = useCallback(() => {
        getBarryApps().then((r) => {
            if (r && r.ok) { setApps(r.apps); setErr(""); }
            else setErr((r && r.error) || "Barry Launcher isn't answering.");
        }).catch(() => setErr("Barry Launcher isn't answering."));
    }, []);
    useEffect(load, [load]);
    const save = (next) => {
        setApps(next);
        setBarryApps(next.map((a) => a.id), next.filter((a) => a.hidden).map((a) => a.id))
            .then((r) => { if (r && r.ok) setApps(r.apps); else setErr((r && r.error) || "Could not save."); })
            .catch(() => setErr("Could not save."));
    };
    const move = (i, by) => {
        const j = i + by;
        if (!apps || j < 0 || j >= apps.length) return;
        const next = apps.slice();
        [next[i], next[j]] = [next[j], next[i]];
        save(next);
    };
    // User apps (barry_apps): an archive from the file browser; Barry
    // Launcher checks and installs it, an app already there is updated.
    const install = async () => {
        setErr(""); setMsg("");
        let picked;
        try {
            const start = await barryAppsFolder();
            picked = await api.openFilePicker(0, start, true, true, undefined, ARCHIVE_EXTENSIONS, false, true);
        } catch (e) {
            return;  // closed without a file
        }
        const path = picked && (picked.realpath || picked.path);
        if (!path) return;
        setBusy(true);
        const doInstall = async () => {
            setBusy(true);
            try {
                const r = await installBarryApp(path);
                if (r && r.ok) {
                    setMsg(`${r.app.updated ? "Updated" : "Installed"} ${r.app.name} ${r.app.version}.`);
                    load();
                } else {
                    setErr((r && r.error) || "Could not install.");
                }
            } catch (e) {
                setErr("Could not install.");
            }
            setBusy(false);
        };
        // An app with a service runs a program of its own: asked first.
        let c;
        try {
            c = await checkBarryApp(path);
        } catch (e) {
            c = null;
        }
        setBusy(false);
        if (!c || !c.ok) {
            setErr((c && c.error) || "Could not read the app.");
            return;
        }
        if (!c.app.service) {
            doInstall();
            return;
        }
        DFL.showModal(jsx(DFL.ConfirmModal, {
            strTitle: `Install ${c.app.name}?`,
            strDescription: `${c.app.name} comes with a program that runs on this device while the app is open, `
                + "with the same access to your files and network as you. Install it only if you trust where it came from.",
            strOKButtonText: "Install",
            onOK: doInstall,
        }));
    };
    const remove = (a) => DFL.showModal(jsx(DFL.ConfirmModal, {
        strTitle: `Remove ${a.name}?`,
        strDescription: "The app and everything it saved are deleted.",
        strOKButtonText: "Remove",
        onOK: () => {
            setErr(""); setMsg("");
            removeBarryApp(a.id).then((r) => {
                if (r && r.ok) { setMsg(`Removed ${a.name}.`); load(); }
                else setErr((r && r.error) || "Could not remove.");
            }).catch(() => setErr("Could not remove."));
        },
    }));
    const button = (label, onClick, disabled) => jsx(DFL.DialogButton, {
        onClick, disabled,
        style: { minWidth: "36px", width: "36px", height: "32px", padding: 0, marginLeft: "4px" },
        children: label,
    });
    return jsxs(DFL.PanelSection, { title: "Apps", children: [
        err && note(err),
        ...(apps || []).map((a, i) => row(jsxs(DFL.Focusable, {
            "flow-children": "horizontal",
            style: { display: "flex", alignItems: "center" },
            children: [
                jsx("div", { style: { flex: 1, opacity: a.hidden ? 0.5 : 1 }, children: a.name }),
                a.user && button("✕", () => remove(a)),
                button("▲", () => move(i, -1), i === 0),
                button("▼", () => move(i, 1), i === apps.length - 1),
                // Trackpad and Keyboard always show (Barry Launcher says
                // which): their switch stays on.
                jsx("div", { style: { marginLeft: "10px" }, children: jsx(DFL.Toggle, {
                    value: !a.hidden,
                    disabled: a.hideable === false,
                    onChange: (v) => save(apps.map((b) => (b.id === a.id ? { ...b, hidden: !v } : b))),
                }) }),
            ],
        }), )),
        apps && note("Switch: shown on Barry Launcher's home screen (Trackpad and Keyboard always are). ▲ ▼: its place there. ✕: remove an app you installed."),
        msg && note(msg),
        row(jsx(DFL.ButtonItem, {
            layout: "below", disabled: busy, onClick: install,
            children: busy ? "Installing…" : "Install app…",
        })),
        note("A Barry Launcher app comes as a .zip (or .tar.gz) archive; pick it here. Making one: github.com/project-barry/barry-launcher-apps"),
    ] });
}

// The Games tab's picks. A dropdown's list remounts the panel, which would
// otherwise put the Game back on the last one played.
const gamePicks = { game: null, app: null, close: true };

function GamesSection() {
    const [links, setLinks] = useState(null);
    const [apps, setApps] = useState([]);
    const [games] = useState(linkableGames);
    const [game, setGameState] = useState(() =>
        games.some((g) => g.id === gamePicks.game) ? gamePicks.game
            : ((games.find((g) => g.running) || games[0] || {}).id || null));
    const [app, setAppState] = useState(gamePicks.app);
    const [close, setCloseState] = useState(gamePicks.close);
    const setGame = (v) => { gamePicks.game = v; setGameState(v); };
    const setApp = (v) => setAppState((cur) => {
        const next = typeof v === "function" ? v(cur) : v;
        gamePicks.app = next;
        return next;
    });
    const setClose = (v) => { gamePicks.close = v; setCloseState(v); };
    const [err, setErr] = useState("");
    const load = useCallback(() => {
        getGameLinks().then((r) => {
            if (r && r.ok) { setLinks(r.links); setErr(""); }
            else setErr((r && r.error) || "Barry Launcher isn't answering.");
        }).catch(() => setErr("Barry Launcher isn't answering."));
        getBarryApps().then((r) => {
            if (r && r.ok) {
                // Trackpad and Keyboard are tools for the top screen, not
                // companions to a game.
                const list = r.apps.filter((a) => a.id !== "trackpad" && a.id !== "keyboard");
                setApps(list);
                setApp((cur) => (list.some((a) => a.id === cur) ? cur : (list.length ? list[0].id : null)));
            }
        }).catch(() => {});
    }, []);
    useEffect(load, [load]);
    const save = (next) => setGameLinks(next.map(({ game, gameName, app, close }) => ({ game, gameName, app, close })))
        .then((r) => { if (r && r.ok) setLinks(r.links); else setErr((r && r.error) || "Could not save."); })
        .catch(() => setErr("Could not save."));
    const link = () => {
        const g = games.find((x) => x.id === game);
        if (!g || !app) return;
        save([...(links || []).filter((l) => l.game !== g.id), { game: g.id, gameName: g.name, app, close }]);
    };
    const linked = (links || []).find((l) => l.game === game);
    return jsxs(DFL.PanelSection, { title: "Companion Apps", children: [
        err && note(err),
        games.length === 0 && note("Play a game first: the games you've played are listed here."),
        games.length > 0 && row(jsx(DFL.DropdownItem, {
            label: "Game",
            rgOptions: games.map((g) => ({ data: g.id, label: (g.running ? "▶ " : "") + g.name })),
            selectedOption: game,
            onChange: (o) => setGame(o.data),
        })),
        apps.length > 0 && row(jsx(DFL.DropdownItem, {
            label: "Opens",
            rgOptions: apps.map((a) => ({ data: a.id, label: a.name })),
            selectedOption: app,
            onChange: (o) => setApp(o.data),
        })),
        row(jsx(DFL.ToggleField, {
            label: "Close it when the game closes",
            checked: close,
            onChange: setClose,
        })),
        row(jsx(DFL.ButtonItem, {
            layout: "below", disabled: !game || !app, onClick: link,
            children: linked ? "Change the link" : "Link",
        })),
        ...(links || []).map((l) => ({ ...l, name: l.gameName || l.game })).sort(byName).map((l) => row(jsxs(DFL.Focusable, {
            "flow-children": "horizontal",
            style: { display: "flex", alignItems: "center" },
            children: [
                jsx("div", { style: { flex: 1 }, children: `${l.gameName || l.game} → ${l.appName}${l.close ? "" : " (stays open)"}` }),
                jsx(DFL.DialogButton, {
                    onClick: () => save(links.filter((x) => x.game !== l.game)),
                    style: { minWidth: "36px", width: "36px", height: "32px", padding: 0, marginLeft: "4px" },
                    children: "✕",
                }),
            ],
        }))),
        note("When a linked game starts, its app opens on the bottom screen. ▶: running now."),
    ] });
}

function Content() {
    const [tab, setTab] = useState(lastTab);
    const pick = (id) => { lastTab = id; setTab(id); };
    const [st, setSt] = useState(null);
    // The "both screens" slider: where it was last put, or the common level.
    const [both, setBoth] = useState(null);
    const movedAt = useRef(0);
    const refresh = useCallback(() => {
        if (Date.now() - movedAt.current < HOLD_MS) return;
        getState().then((s) => {
            if (Date.now() - movedAt.current < HOLD_MS) return;
            setSt(s);
            if (s && s.dimmers && s.dimmers.top === s.dimmers.bottom) setBoth(s.dimmers.top);
        }).catch(() => {});
    }, []);
    const sendDimmers = useThrottled(useCallback((d) => { setDimmers(d.top, d.bottom).catch(() => {}); }, []));
    const sendRgb = useThrottled(useCallback((v) => { setRgbDimmer(v).catch(() => {}); }, []));
    const dim = (top, bottom) => {
        movedAt.current = Date.now();
        setSt((s) => ({ ...s, dimmers: { top, bottom } }));
        sendDimmers({ top, bottom });
    };
    useEffect(() => {
        refresh();
        const t = setInterval(refresh, 3000);
        return () => clearInterval(t);
    }, [refresh]);

    if (!st) return jsx(DFL.PanelSection, { children: note("Loading…") });
    if (!st.supported) return jsx(DFL.PanelSection, { children: note("This plugin is for the AYN Thor (two screens).") });

    const d = st.dimmers || { top: 100, bottom: 100 };
    const slider = (label, value, min, onChange, extra) => row(jsx(DFL.SliderField, {
        label: `${label} (${value}%)`, value, min, max: 100, step: 5, onChange, ...extra,
    }));
    const tabs = [
        { id: "screens", icon: ThorIcon },
        { id: "apps", icon: AppsIcon },
        { id: "games", icon: GamesIcon },
        ...(st.rgbDimmer != null ? [{ id: "lights", icon: RgbIcon }] : []),
        { id: "emulators", icon: HandheldIcon },
    ];
    const shown = tabs.some((t) => t.id === tab) ? tab : "screens";
    const step = (by) => {
        const i = tabs.findIndex((t) => t.id === shown);
        pick(tabs[(i + by + tabs.length) % tabs.length].id);
    };
    return jsxs(DFL.Focusable, {
        onButtonDown: (evt) => {
            const b = evt && evt.detail && evt.detail.button;
            if (b === BUMPER_LEFT) step(-1);
            else if (b === BUMPER_RIGHT) step(1);
        },
        children: [jsx(TabBar, { tabs, active: shown, onPick: pick }),
    shown === "screens" && jsxs(DFL.PanelSection, { title: "Dual Screen", children: [
        row(jsx(DFL.ToggleField, {
            label: "Bottom screen",
            description: st.on ? undefined : "Off: dark and ignoring touch until you turn it back on.",
            checked: !!st.on,
            onChange: (v) => {
                setSt((s) => ({ ...s, on: v }));
                setBottomScreen(v).then(refresh).catch(refresh);
            },
        })),
        slider("Top screen dimmer", d.top, 10, (v) => dim(v, d.bottom)),
        slider("Bottom screen dimmer", d.bottom, 10, (v) => dim(d.top, v), { disabled: !st.on }),
        slider("Both screens", both != null ? both : Math.max(d.top, d.bottom), 10, (v) => {
            setBoth(v);
            dim(v, v);
        }),
        note("Steam's brightness slider moves both screens together, each at its own setting here."),
    ] }),
    shown === "lights" && jsxs(DFL.PanelSection, { title: "Joystick RGB Dimmer", children: [
        slider("Lights dimmer", st.rgbDimmer, 20, (v) => {
            movedAt.current = Date.now();
            setSt((s) => ({ ...s, rgbDimmer: v }));
            sendRgb(v);
        }),
        note("This setting adjusts the brightness threshold of the performance dashboard settings"),
    ] }),
    shown === "apps" && jsx(AppsSection, {}),
    shown === "games" && jsx(GamesSection, {}),
    shown === "emulators" && jsx(TwoScreenSection, {}),
    ] });
}

var index = definePlugin(() => {
    const timer = setInterval(watchBarryKeyboard, 2000);
    watchBarryKeyboard();
    const gameTimer = setInterval(watchGames, 1000);
    watchGames();
    return {
        name: "Barry Launcher",
        content: jsx(Content, {}),
        icon: jsx("div", { style: { fontWeight: 800 }, children: "☀" }),
        alwaysRender: false,
        onDismount() {
            clearInterval(timer);
            clearInterval(gameTimer);
            gamesSeen = null;
            if (unhookKeyboard) unhookKeyboard();
            unhookKeyboard = null;
            unwatchClicks();
            barryAvailable = false;
        },
    };
});

export { index as default };

// Barry Launcher in PB-OS's Quick Access tab (was the Barry Launcher Decky
// plugin, dual-screen; AYN Thor only): each former tab is a plugin in the
// PB-OS list, its component unchanged. pbos-qamd's barry section is the
// backend. The loader (src/loader.js) gives P: @decky/ui as DFL, Steam's
// React and JSX, callable(), openFilePicker() and plugin().
window.__pbosQam.section("barry", (P) => {
const { DFL, SP_REACT, SP_JSX, callable } = P;
const api = P;
const { useEffect, useState, useCallback, useRef } = SP_REACT;
const jsx = SP_JSX.jsx;
const jsxs = SP_JSX.jsxs;

// pbos-qamd's barry section (daemon/barry.cpp): arguments are passed
// positionally, as Decky's callable() passed them to the plugin's Python.
const getState = callable("get_state");
const setBottomScreen = callable("set_bottom_screen");
const setRgbDimmer = callable("set_rgb_dimmer");
const setBrightness = callable("set_brightness");
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
const getThemes = callable("get_themes");
const setTheme = callable("set_theme");
const setThemeColor = callable("set_theme_color");
const themesFolder = callable("themes_folder");
const themesDone = callable("themes_done");
const checkTheme = callable("check_theme");
const installTheme = callable("install_theme");
const removeTheme = callable("remove_theme");
const getLanguages = callable("get_languages");
const setLanguage = callable("set_language");
const checkLanguage = callable("check_language");
const installLanguage = callable("install_language");
const removeLanguage = callable("remove_language");
const getWidgets = callable("get_widgets");
const setWidgetShown = callable("set_widget_shown");
const checkWidget = callable("check_widget");
const installWidget = callable("install_widget");
const removeWidget = callable("remove_widget");
const steamGame = callable("steam_game");
const getBottomSettings = callable("get_bottom_settings");
const setBottomSettings = callable("set_bottom_settings");
const getSgdb = callable("get_sgdb");
const setSgdbKey = callable("set_sgdb_key");
// The PB-OS tab's file picker (select 0: a file), called as @decky/api's openFilePicker.
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
        // A game starting is the game selected now (the hero art).
        for (const id of now) if (!gamesSeen.has(id)) selectGame(Number(id));
    }
    gamesSeen = now;
    sendGame();
}

// The game selected on the top screen, for the bottom screen's hero art
// and logo (Barry Launcher; pbos-qamd's heroart.cpp finds the images): the
// game tile or game page Steam's focus is on, a game as it starts, and at
// first the game running, else the one played last. Focus is heard
// (focusin in Steam's windows); a focused element's game is in the React
// props of it or its parents (an app overview with appid and
// display_name), or in the route of a game's page.
let selected = null;   // { appid, name, shortcut }
let gameSent = "";
let selectTimer = 0;
const SELECT_SETTLE_MS = 150;  // moving along a shelf: only where it stops

function appOverview(appid) {
    try { return (window.appStore && appStore.GetAppOverviewByAppID(appid)) || null; } catch (e) { return null; }
}

function isGame(app) {
    return app && (app.app_type === STEAM_GAME || app.app_type === NON_STEAM_GAME);
}

function lastPlayed() {
    let all = [];
    try {
        all = (window.collectionStore && collectionStore.allAppsCollection && collectionStore.allAppsCollection.allApps) || [];
    } catch (e) { /* signed out */ }
    let best = null;
    for (const a of all) if (isGame(a) && a.rt_last_time_played > 0 && (!best || a.rt_last_time_played > best.rt_last_time_played)) best = a;
    return best;
}

function mainRunning() {
    const app = DFL.Router && DFL.Router.MainRunningApp;
    return app && app.appid ? app : null;
}

function gameOf(app) {
    return app ? { appid: String(app.appid), name: app.display_name || "", shortcut: app.app_type === NON_STEAM_GAME } : null;
}

function selectGame(appid) {
    const app = appOverview(appid);
    if (!isGame(app)) return;
    selected = gameOf(app);
    sendGame();
}

function sendGame() {
    if (!selected) selected = gameOf(mainRunning() || lastPlayed());
    if (!selected) return;
    const running = mainRunning();
    const profile = gameOf(running || lastPlayed());
    const game = { ...selected, running: runningGames().has(selected.appid), profile: profile && { appid: profile.appid, name: profile.name } };
    const key = JSON.stringify(game);
    if (key === gameSent) return;
    gameSent = key;
    steamGame(game).catch(() => { gameSent = ""; });
}

function focusedApp(el) {
    for (let n = el; n && n.nodeType === 1; n = n.parentElement) {
        const key = Object.keys(n).find((k) => k.startsWith("__reactFiber$"));
        if (!key) continue;
        // This element's fiber and its owners up the tree.
        for (let f = n[key], i = 0; f && i < 40; f = f.return, i++) {
            const p = f.memoizedProps;
            if (!p || typeof p !== "object") continue;
            for (const app of [p.app, p.overview, p.appOverview]) {
                if (app && typeof app === "object" && app.appid && app.display_name) return isGame(app) ? app : null;
            }
            if (typeof p.appid === "number" && p.appid > 0) {
                const app = appOverview(p.appid);
                if (app) return isGame(app) ? app : null;
            }
        }
        return null;
    }
    return null;
}

function onFocusIn(e) {
    const el = e.target;
    const win = el && el.ownerDocument && el.ownerDocument.defaultView;
    clearTimeout(selectTimer);
    selectTimer = setTimeout(() => {
        let app = focusedApp(el);
        if (!app && win) {
            const m = /\/library\/app\/(\d+)/.exec(win.location.pathname || "");
            if (m) app = appOverview(Number(m[1]));
        }
        if (isGame(app)) selectGame(app.appid);
    }, SELECT_SETTLE_MS);
}

const focusDocs = new Set();

function watchFocus() {
    for (const w of steamWindows()) {
        const doc = w.document;
        if (focusDocs.has(doc)) continue;
        doc.addEventListener("focusin", onFocusIn, true);
        focusDocs.add(doc);
    }
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
    // Steam's collections throw until a user has signed in: no games then.
    let all = [];
    try {
        all = (window.collectionStore && collectionStore.allAppsCollection && collectionStore.allAppsCollection.allApps) || [];
    } catch (e) { /* signed out */ }
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
    watchFocus();
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

// A painter's palette: Barry Launcher's themes.
const ThemesIcon = () => svg([
    jsx("path", { d: "M12 3a9 9 0 1 0 0 18c1.1 0 1.8-.8 1.8-1.7 0-.5-.2-.9-.5-1.2-.3-.3-.5-.7-.5-1.2 0-.9.8-1.7 1.7-1.7H16a5 5 0 0 0 5-5C21 6.4 17 3 12 3z" }, "p"),
    jsx("circle", { cx: 7.5, cy: 11.5, r: 1.2, fill: "currentColor", stroke: "none" }, "a"),
    jsx("circle", { cx: 9.5, cy: 7.5, r: 1.2, fill: "currentColor", stroke: "none" }, "b"),
    jsx("circle", { cx: 14, cy: 6.8, r: 1.2, fill: "currentColor", stroke: "none" }, "c"),
    jsx("circle", { cx: 17.2, cy: 10, r: 1.2, fill: "currentColor", stroke: "none" }, "d"),
]);

// A globe: the keyboards' languages.
const LanguagesIcon = () => svg([
    jsx("circle", { cx: 12, cy: 12, r: 9 }, "o"),
    jsx("path", { d: "M3 12h18" }, "e"),
    jsx("path", { d: "M12 3c2.5 2.6 3.8 5.6 3.8 9s-1.3 6.4-3.8 9c-2.5-2.6-3.8-5.6-3.8-9S9.5 5.6 12 3z" }, "m"),
]);

// Four tiles, one wide: the quick settings page's widgets.
const WidgetsIcon = () => svg([
    jsx("rect", { x: 3, y: 3, width: 8, height: 8, rx: 2 }, "a"),
    jsx("rect", { x: 13, y: 3, width: 8, height: 8, rx: 2 }, "b"),
    jsx("rect", { x: 3, y: 13, width: 18, height: 8, rx: 2 }, "c"),
]);

// LB and RB: the controller's bumpers, in Steam's button numbering.
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

// Adding a theme or a language pack: the file picker opens on a USB drive
// when one is plugged in (mounted read-only for the pick), else Downloads;
// what is picked is checked, then added. One with code in it (QML) is asked
// first, with code(pack) as the reason.
async function addPack({ noun, extensions, check, install, code, setBusy, setErr, setMsg, load }) {
    setErr(""); setMsg("");
    let start;
    try {
        setBusy(true);
        start = await themesFolder();
    } catch (e) {
        start = null;
    }
    setBusy(false);
    if (!start || !start.ok) {
        setErr((start && start.error) || "Could not open the folder.");
        return;
    }
    let picked;
    try {
        picked = await api.openFilePicker(0, start.path, true, true, undefined, extensions, false, true);
    } catch (e) {
        themesDone().catch(() => {});
        return;  // closed without a file
    }
    const path = picked && (picked.realpath || picked.path);
    if (!path) { themesDone().catch(() => {}); return; }
    const doInstall = async () => {
        setBusy(true);
        try {
            const r = await install(path);
            if (r && r.ok) {
                setMsg(`${r[noun].replaces ? "Updated" : "Added"} ${r[noun].title}.`);
                load();
            } else setErr((r && r.error) || `Could not add the ${noun}.`);
        } catch (e) {
            setErr(`Could not add the ${noun}.`);
        }
        setBusy(false);
        themesDone().catch(() => {});
    };
    let c;
    try {
        setBusy(true);
        c = await check(path);
    } catch (e) {
        c = null;
    }
    setBusy(false);
    if (!c || !c.ok) {
        setErr((c && c.error) || `Could not read the ${noun}.`);
        themesDone().catch(() => {});
        return;
    }
    const pack = c[noun];
    if (!pack.code) { doInstall(); return; }
    DFL.showModal(jsx(DFL.ConfirmModal, {
        strTitle: `Add ${pack.title}?`,
        strDescription: code(pack),
        strOKButtonText: "Add",
        onOK: doInstall,
        onCancel: () => themesDone().catch(() => {}),
    }));
}

// A row of a list of themes or languages: its name (picks it; ✓ = in use)
// and, for one you added, ✕ (removes it).
const pickRow = ({ title, on, onPick, onRemove }) => row(jsxs(DFL.Focusable, {
    "flow-children": "horizontal",
    style: { display: "flex", alignItems: "center", gap: "8px", padding: "4px 0" },
    children: [
        jsx(DFL.DialogButton, {
            onClick: onPick,
            style: { flex: 1, minWidth: 0, height: "40px", padding: "0 12px",
                     display: "flex", alignItems: "center", gap: "10px" },
            children: [
                jsx("span", { style: { width: "14px", color: "#1a9fff" }, children: on ? "✓" : "" }, "c"),
                jsx("span", { style: { flex: 1, textAlign: "left", overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }, children: title }, "t"),
            ],
        }),
        onRemove && jsx(DFL.DialogButton, {
            onClick: onRemove,
            style: { minWidth: "40px", width: "40px", height: "40px", padding: 0, flexShrink: 0 },
            children: "✕",
        }),
    ],
}));

// Barry Launcher's themes: the one in use (picked here, it changes at once),
// adding one (from a USB drive when one is plugged in, else Downloads), and
// removing one you added, as with apps (the built-in ones stay).
const THEME_EXTENSIONS = [...ARCHIVE_EXTENSIONS, "json"];

function ThemesSection() {
    const [st, setSt] = useState(null);
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const [msg, setMsg] = useState("");
    const load = useCallback(() => {
        getThemes().then((r) => {
            if (r && r.ok) { setSt(r); setErr(""); }
            else setErr((r && r.error) || "Barry Launcher isn't answering.");
        }).catch(() => setErr("Barry Launcher isn't answering."));
    }, []);
    useEffect(load, [load]);
    const use = (t) => {
        setErr(""); setMsg("");
        setTheme(t.name).then((r) => {
            if (r && r.ok) setSt(r); else setErr((r && r.error) || "Could not change the theme.");
        }).catch(() => setErr("Could not change the theme."));
    };
    // A theme's .zip or .tar.gz, or the skin.json in its folder. One with its
    // own screens (QML) is asked first.
    const add = () => addPack({
        noun: "theme", extensions: THEME_EXTENSIONS, check: checkTheme, install: installTheme,
        code: (t) => `${t.title} draws Barry Launcher's screens with its own code, which runs on this device `
            + "as you. Add it only if you trust where it came from.",
        setBusy, setErr, setMsg, load,
    });
    // A color of the theme in use (its skin.json "variants").
    const recolor = (t, v) => {
        setErr(""); setMsg("");
        setThemeColor(t.name, v.id).then((r) => {
            if (r && r.ok) setSt(r); else setErr((r && r.error) || "Could not change the color.");
        }).catch(() => setErr("Could not change the color."));
    };
    // A variant's swatch, as a small keyboard: its background, a key and the
    // accent key.
    const swatch = (t, v) => {
        const [bg, key, accent] = [0, 1, 2].map((i) => (v.swatch || [])[i] || "#808080");
        const on = t.variant === v.id;
        return jsx(DFL.DialogButton, {
            onClick: () => recolor(t, v),
            "aria-label": v.title,
            style: { minWidth: 0, width: "48px", height: "36px", padding: 0, margin: "3px",
                     borderRadius: "8px", background: bg, position: "relative",
                     boxShadow: on ? "0 0 0 2px #1a9fff" : "0 0 0 1px rgba(255,255,255,0.18)" },
            children: [
                jsx("div", { style: { position: "absolute", left: "9px", bottom: "8px", width: "18px", height: "5px",
                                      borderRadius: "2px", background: key } }, "k"),
                jsx("div", { style: { position: "absolute", right: "7px", bottom: "7px", width: "9px", height: "7px",
                                      borderRadius: "4px", background: accent } }, "a"),
            ],
        }, v.id);
    };
    const remove = (t) => DFL.showModal(jsx(DFL.ConfirmModal, {
        strTitle: `Remove ${t.title}?`,
        strDescription: "The theme is deleted."
            + (st && st.current === t.name ? " Barry Launcher changes to SteamOS." : ""),
        strOKButtonText: "Remove",
        onOK: () => {
            setErr(""); setMsg("");
            removeTheme(t.name).then((r) => {
                if (r && r.ok) { setSt(r); setMsg(`Removed ${t.title}.`); }
                else setErr((r && r.error) || "Could not remove the theme.");
            }).catch(() => setErr("Could not remove the theme."));
        },
    }));
    return jsxs(DFL.PanelSection, { title: "Themes", children: [
        err && note(err),
        // One theme per row, apart from the next: its name (picks it) and,
        // for one you added, ✕ (removes it). The theme in use, if it comes in
        // several colors, has its color picker right under it.
        ...(st ? st.skins : []).flatMap((t) => [
            pickRow({ title: t.title, on: st.current === t.name, onPick: () => use(t),
                      onRemove: !t.builtin && (() => remove(t)) }),
            st.current === t.name && t.variants.length > 1 && row(jsx(DFL.Focusable, {
                "flow-children": "horizontal",
                style: { display: "flex", flexWrap: "wrap", margin: "4px -3px 0" },
                children: t.variants.map((v) => swatch(t, v)),
            })),
            st.current === t.name && t.variants.length > 1 && note(`${t.title} color: `
                + ((t.variants.find((v) => v.id === t.variant) || {}).title || t.variant)),
        ]),
        msg && note(msg),
        row(jsx(DFL.ButtonItem, { layout: "below", disabled: busy, onClick: add,
            children: busy ? "Working…" : "Add a theme" })),
        note("A theme comes as a .zip (or .tar.gz), or as a folder: pick the skin.json in it. "
            + "With a USB drive plugged in, the files open there; otherwise in Downloads."),
    ] });
}

// The keyboards' languages: the one in use (picked here, both of Barry
// Launcher's keyboards change at once), adding a language pack the way a
// theme is added, and removing one you added (the shipped ones stay).
const LANGUAGE_EXTENSIONS = [...ARCHIVE_EXTENSIONS, "json"];

function LanguagesSection() {
    const [st, setSt] = useState(null);
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const [msg, setMsg] = useState("");
    const load = useCallback(() => {
        getLanguages().then((r) => {
            if (r && r.ok) { setSt(r); setErr(""); }
            else setErr((r && r.error) || "Could not read the languages.");
        }).catch(() => setErr("Could not read the languages."));
    }, []);
    useEffect(load, [load]);
    const use = (l) => {
        setErr(""); setMsg("");
        setLanguage(l.name).then((r) => {
            if (r && r.ok) setSt(r); else setErr((r && r.error) || "Could not change the language.");
        }).catch(() => setErr("Could not change the language."));
    };
    // A pack's .zip or .tar.gz, or the keyboard.json in its folder. One with
    // a composer (QML) is asked first.
    const add = () => addPack({
        noun: "language", extensions: LANGUAGE_EXTENSIONS, check: checkLanguage, install: installLanguage,
        code: (l) => `${l.title} has its own code for typing (a composer), which runs on this device `
            + "as you. Add it only if you trust where it came from.",
        setBusy, setErr, setMsg, load,
    });
    // A keyboard PB-OS ships but leaves off (another layout, like
    // Thumb-Key's): added as a pack you added, then put in use.
    const addExtra = (x) => {
        setErr(""); setMsg(""); setBusy(true);
        installLanguage(x.path).then((r) => {
            if (!r || !r.ok) { setErr((r && r.error) || "Could not add the keyboard."); return null; }
            setMsg(`Added ${r.language.title}.`);
            return setLanguage(r.language.name);
        }).then((r) => { if (r) load(); })
          .catch(() => setErr("Could not add the keyboard."))
          .finally(() => setBusy(false));
    };
    const remove = (l) => DFL.showModal(jsx(DFL.ConfirmModal, {
        strTitle: `Remove ${l.title}?`,
        strDescription: "The language pack is deleted."
            + (st && st.current === l.name ? " The keyboards change to English." : ""),
        strOKButtonText: "Remove",
        onOK: () => {
            setErr(""); setMsg("");
            removeLanguage(l.name).then((r) => {
                if (r && r.ok) { setSt(r); setMsg(`Removed ${l.title}.`); }
                else setErr((r && r.error) || "Could not remove the language.");
            }).catch(() => setErr("Could not remove the language."));
        },
    }));
    return jsxs(DFL.PanelSection, { title: "Keyboard Languages", children: [
        err && note(err),
        ...(st ? st.languages : []).map((l) => pickRow({
            title: `${l.title} (${l.label})`, on: st.current === l.name, onPick: () => use(l),
            onRemove: !l.builtin && (() => remove(l)),
        })),
        st && st.languages.length > 1 && note("On the keyboard, the language key next to Fn changes it too."),
        ...(st && st.extras ? st.extras : []).map((x) => row(jsx(DFL.ButtonItem, {
            layout: "below", disabled: busy, onClick: () => addExtra(x), children: `Add ${x.title}`,
        }))),
        st && st.extras && st.extras.some((x) => x.name.indexOf("thumbkey") >= 0)
            && note("Thumb-Key: big keys. Tap for the big letter, swipe toward a small one to type it. "
                + "Your other keyboards stay; ✕ removes it again."),
        msg && note(msg),
        row(jsx(DFL.ButtonItem, { layout: "below", disabled: busy, onClick: add,
            children: busy ? "Working…" : "Add a language" })),
        note("A language pack comes as a .zip (or .tar.gz), or as a folder: pick the keyboard.json in it. "
            + "With a USB drive plugged in, the files open there; otherwise in Downloads."),
    ] });
}

// The quick settings page's widgets (Barry Launcher, swipe left from the
// apps), Barry's own and the packs': each one's row puts it on the page or
// takes it off (✓ = on it), so one taken off there comes back here; ✕
// deletes a pack you added, and one is added the way a theme is. On the
// page itself, a long press moves, resizes and removes them.
const WIDGET_EXTENSIONS = [...ARCHIVE_EXTENSIONS, "json"];

function WidgetsSection() {
    const [st, setSt] = useState(null);
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const [msg, setMsg] = useState("");
    const load = useCallback(() => {
        getWidgets().then((r) => {
            if (r && r.ok) { setSt(r); setErr(""); }
            else setErr((r && r.error) || "Barry Launcher isn't answering.");
        }).catch(() => setErr("Barry Launcher isn't answering."));
    }, []);
    useEffect(load, [load]);
    const toggle = (w) => {
        setErr(""); setMsg("");
        setWidgetShown(w.id, !w.on).then((r) => {
            if (r && r.ok) { setSt(r); setMsg(`${w.title} is ${w.on ? "off" : "on"} the quick settings page.`); }
            else setErr((r && r.error) || "Could not change the page.");
        }).catch(() => setErr("Could not change the page."));
    };
    // A widget's .zip or .tar.gz, or the widget.json in its folder. A widget
    // is always code (QML), so it is always asked first; once added it goes
    // on the page.
    const add = () => addPack({
        noun: "widget", extensions: WIDGET_EXTENSIONS, check: checkWidget,
        install: (path) => installWidget(path).then((r) => {
            if (!r || !r.ok) return r;
            return setWidgetShown(`w-${r.widget.name}`, true).then(() => r).catch(() => r);
        }),
        code: (w) => `${w.title} is a widget with its own code, which runs on this device `
            + "as you. Add it only if you trust where it came from.",
        setBusy, setErr, setMsg, load,
    });
    const remove = (w) => DFL.showModal(jsx(DFL.ConfirmModal, {
        strTitle: `Remove ${w.title}?`,
        strDescription: "The widget is deleted" + (w.on ? " and leaves the quick settings page." : "."),
        strOKButtonText: "Remove",
        onOK: () => {
            setErr(""); setMsg("");
            removeWidget(w.name).then((r) => {
                if (r && r.ok) { setSt(r); setMsg(`Removed ${w.title}.`); }
                else setErr((r && r.error) || "Could not remove the widget.");
            }).catch(() => setErr("Could not remove the widget."));
        },
    }));
    return jsxs(DFL.PanelSection, { title: "Widgets", children: [
        err && note(err),
        ...(st ? st.widgets : []).map((w) => pickRow({
            title: w.author && !w.builtin ? `${w.title} (${w.author})` : w.title,
            on: w.on, onPick: () => toggle(w), onRemove: w.kind === "pack" && !w.builtin && (() => remove(w)),
        })),
        msg && note(msg),
        row(jsx(DFL.ButtonItem, { layout: "below", disabled: busy, onClick: add,
            children: busy ? "Working…" : "Add a widget" })),
        note("✓: on Barry Launcher's quick settings page (swipe left from the apps). There, a long press "
            + "lets you drag widgets around, resize them, and remove them with ✕. A widget pack comes as a .zip (or .tar.gz), or as a folder: pick the widget.json in it. "
            + "With a USB drive plugged in, the files open there; otherwise in Downloads."),
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

// The Dual Screen part and the dimmer (Lighting) part share the plugin's
// old panel state: getState every 3 s while shown, sliders held a while.
function useBarryState() {
    const [st, setSt] = useState(null);
    const movedAt = useRef(0);
    const refresh = useCallback(() => {
        if (Date.now() - movedAt.current < HOLD_MS) return;
        getState().then((s) => {
            if (Date.now() - movedAt.current < HOLD_MS) return;
            setSt(s);
        }).catch(() => {});
    }, []);
    const sendRgb = useThrottled(useCallback((v) => { setRgbDimmer(v).catch(() => {}); }, []));
    // Each screen's brightness, as Android's top and bottom sliders; Steam's
    // slider is Android's "both screens" one.
    const sendTop = useThrottled(useCallback((v) => { setBrightness("top", v).catch(() => {}); }, []));
    const sendBottom = useThrottled(useCallback((v) => { setBrightness("bottom", v).catch(() => {}); }, []));
    const bright = (screen, v) => {
        movedAt.current = Date.now();
        setSt((s) => ({ ...s, brightness: { ...s.brightness, [screen]: v } }));
        (screen === "top" ? sendTop : sendBottom)(v);
    };
    const rgb = (v) => {
        movedAt.current = Date.now();
        setSt((s) => ({ ...s, rgbDimmer: v }));
        sendRgb(v);
    };
    useEffect(() => {
        refresh();
        const t = setInterval(refresh, 3000);
        return () => clearInterval(t);
    }, [refresh]);
    return { st, setSt, refresh, bright, rgb };
}

const slider = (label, value, min, onChange, extra) => row(jsx(DFL.SliderField, {
    label: `${label} (${value}%)`, value, min, max: 100, step: 5, onChange, ...extra,
}));

// Barry Launcher's bottom screen: the game art (on: the art is standby, a
// tap wakes Barry; off: Barry's apps always shown) and the sleep timer.
const SLEEP_CHOICES = [
    { data: 0, label: "Never" },
    { data: 30, label: "30 seconds" },
    { data: 60, label: "1 minute" },
    { data: 120, label: "2 minutes" },
    { data: 300, label: "5 minutes" },
    { data: 600, label: "10 minutes" },
];

function BottomSettings({ on }) {
    const [bs, setBs] = useState(null);
    useEffect(() => { getBottomSettings().then(setBs).catch(() => setBs(false)); }, []);
    if (bs === false) return note("Barry Launcher isn't answering (is the bottom screen's session running?).");
    if (!bs) return null;
    const change = (patch) => {
        setBs({ ...bs, ...patch });
        setBottomSettings(patch).then((r) => { if (r) setBs(r); }).catch(() => {});
    };
    const sleep = SLEEP_CHOICES.some((c) => c.data === bs.bottomSleepSeconds)
        ? bs.bottomSleepSeconds : SLEEP_CHOICES[0].data;
    return [
        row(jsx(DFL.ToggleField, {
            label: "Game art",
            description: bs.heroArt
                ? "The bottom screen shows the selected game's art; a tap or the AYN button opens Barry Launcher over it."
                : "Off: Barry Launcher's apps stay on the bottom screen.",
            checked: !!bs.heroArt,
            onChange: (v) => change({ heroArt: v }),
        })),
        jsx(ArtRows, {}),
        row(jsx(DFL.DropdownItem, {
            label: "Sleep after",
            description: "No touch on the bottom screen this long turns it dark; a touch or the AYN button wakes it.",
            rgOptions: SLEEP_CHOICES,
            selectedOption: sleep,
            disabled: !on,
            onChange: (o) => change({ bottomSleepSeconds: o.data }),
        })),
    ];
}

function ScreensPage() {
    const { st, setSt, refresh, bright } = useBarryState();
    if (!st) return jsx(DFL.PanelSection, { children: note("Loading…") });
    if (!st.supported) return jsx(DFL.PanelSection, { children: note("This is for the AYN Thor (two screens).") });
    return jsxs(DFL.PanelSection, { title: "Dual Screen", children: [
        row(jsx(DFL.ToggleField, {
            label: "Bottom screen",
            description: st.on ? undefined : "Off: dark and ignoring touch until you turn it back on.",
            checked: !!st.on,
            onChange: (v) => {
                setSt((s) => ({ ...s, on: v }));
                setBottomScreen(v).then(refresh).catch(refresh);
            },
        })),
        ...(st.brightness ? [
            slider("Top screen brightness", st.brightness.top, 0, (v) => bright("top", v), { step: 1 }),
            slider("Bottom screen brightness", st.brightness.bottom, 0, (v) => bright("bottom", v), { step: 1, disabled: !st.on }),
        ] : []),
        note("Steam's brightness slider sets both screens."),
        jsx(BottomSettings, { on: !!st.on }),
    ] });
}

// The bottom screen's art: SteamGridDB, for games Steam has no hero or logo
// for, with the user's own API key (steamgriddb.com, Preferences, API).
// Inside Dual Screen, under its own heading (Quick Access has no sections
// inside sections).
function ArtRows() {
    const [st, setSt] = useState(null);
    const [key, setKey] = useState("");
    const [error, setError] = useState("");
    const [busy, setBusy] = useState(false);
    useEffect(() => { getSgdb().then(setSt).catch(() => setSt({})); }, []);
    const save = (k) => {
        setBusy(true);
        setSgdbKey(k).then((r) => {
            setBusy(false);
            if (r && r.ok === false) { setError(r.error || "Not saved."); return; }
            setError(""); setKey(""); setSt(r);
        }).catch(() => { setBusy(false); setError("Not saved."); });
    };
    if (!st) return null;
    const heading = DFL.staticClasses.PanelSectionTitle;
    return [
        row(jsx("div", { className: heading, style: heading ? undefined : { fontWeight: "bold", paddingTop: "8px" },
            children: "Bottom Screen Art" })),
        note("The bottom screen shows the hero art and logo of the game selected on the top screen: your own art for it (as the SteamGridDB plugin sets it), else Steam's."),
        note(st.hasKey
            ? "SteamGridDB: on. Games without art get it from SteamGridDB."
            : "SteamGridDB is optional and off. Games with art (Steam's, or yours) show it without a key. Only for games that have none, mostly non-Steam games, add your own SteamGridDB API key (steamgriddb.com, Preferences, API)."),
        row(jsx(DFL.TextField, {
            label: st.hasKey ? "Replace the API key" : "SteamGridDB API key (optional)", bIsPassword: true, value: key, disabled: busy,
            onChange: (e) => { setKey(e.target.value); setError(""); },
        })),
        ...(error ? [row(jsx("div", { style: { color: "#ff6b6b" }, children: error }))] : []),
        row(jsx(DFL.ButtonItem, { layout: "below", disabled: busy || !key.trim(), onClick: () => save(key), children: "Save key" })),
        ...(st.hasKey ? [row(jsx(DFL.ButtonItem, { layout: "below", disabled: busy, onClick: () => save(""), children: "Remove key" }))] : []),
    ];
}

function DimmerPage() {
    const { st, rgb } = useBarryState();
    if (!st) return jsx(DFL.PanelSection, { children: note("Loading…") });
    if (st.rgbDimmer == null) return jsx(DFL.PanelSection, { children: note("The stick lights' controller isn't answering.") });
    return jsxs(DFL.PanelSection, { title: "Lighting", children: [
        slider("Lights dimmer", st.rgbDimmer, 20, rgb),
        note("This setting adjusts the brightness threshold of the performance dashboard settings"),
    ] });
}

// ------------------------------------------------------------- plugins ---
// Each part goes on its page of the PB-OS list (PAGES in the loader).
P.plugin({ id: "screens", page: "display", icon: ThorIcon, content: ScreensPage, order: 10 });
P.plugin({ id: "emulators", page: "display", icon: HandheldIcon, content: TwoScreenSection, order: 20 });
P.plugin({ id: "themes", page: "personalization", icon: ThemesIcon, content: ThemesSection, order: 20 });
P.plugin({ id: "widgets", page: "personalization", icon: WidgetsIcon, content: WidgetsSection, order: 30 });
P.plugin({ id: "apps", page: "apps", icon: AppsIcon, content: AppsSection, order: 10 });
P.plugin({ id: "games", page: "apps", icon: GamesIcon, content: GamesSection, order: 20 });
P.plugin({ id: "languages", page: "system", icon: LanguagesIcon, content: LanguagesSection, order: 20 });
// The dimmer, first under Personalization, only where the stick lights'
// controller answers.
getState().then((s) => {
    if (s && s.rgbDimmer != null) P.plugin({ id: "lights", page: "personalization", icon: RgbIcon, content: DimmerPage, order: 5 });
}).catch((e) => P.fail("dimmer", e));

// While Steam runs (as the Decky plugin did while loaded): Barry's keyboard
// in place of Steam's, and the companion apps following running games.
setInterval(watchBarryKeyboard, 2000);
watchBarryKeyboard();
setInterval(watchGames, 1000);
watchGames();
});

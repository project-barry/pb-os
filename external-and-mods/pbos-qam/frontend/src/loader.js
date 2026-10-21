// PB-OS Quick Access: the frontend loader. pbos-qamd evaluates this (with
// @decky/ui bundled in, see build.sh) in Steam's Game Mode page, followed by
// each section's script (sections/*.js). It waits for Steam, adds the PB-OS
// tab to Quick Access (Barry's head) and shows the sections' plugins in it:
// one list (what this device has), and each plugin's page with a back button (B goes back too, LB/RB
// step through them).
//
// Steam internals used here are listed in ../STEAM-INTERNALS.md; the check
// (pbos-qamd --check) runs probe() below in the running tab.
//
// The tab hook and the toaster follow Decky Loader's frontend
// (SteamDeckHomebrew/decky-loader, GPL-2.0: tabs-hook.tsx, toaster.tsx,
// components/Toast.tsx); @decky/ui is SteamDeckHomebrew/decky-frontend-lib
// (LGPL-2.1).

const BOOT = window.__pbosQamBoot || {};
const BINDING = "__pbosQamCall";
const QAM_TAB_KEY = 998; // Decky's own tab is 999 (QuickAccessTab.Decky)
const GROUPS = { tools: "PB-OS Tools", barry: "Barry Launcher" };

const status = { dfl: "", tabPatched: false, tabRendered: 0, toaster: "", sections: [], errors: [] };

function note(msg) {
    try {
        window[BINDING] && window[BINDING](JSON.stringify({ id: 0, s: "_", m: "log", a: [String(msg)] }));
    } catch (e) { /* pbos-qamd away */ }
}

function fail(where, e) {
    const msg = `${where}: ${(e && e.stack) || e}`;
    status.errors.push(msg);
    console.error("[PB-OS]", msg);
    note(msg);
}

// ------------------------------------------------------------ pbos-qamd ---
let nextId = 1;
const pending = new Map();
const listeners = new Map();

function send(section, method, args) {
    return new Promise((resolve, reject) => {
        const id = nextId++;
        pending.set(id, { resolve, reject });
        try {
            window[BINDING](JSON.stringify({ id, s: section, m: method, a: args }));
        } catch (e) {
            pending.delete(id);
            reject(new Error("pbos-qamd is not running"));
        }
    });
}

const bridge = {
    _reply(id, ok, value) {
        const p = pending.get(id);
        if (!p) return;
        pending.delete(id);
        if (ok) p.resolve(value);
        else p.reject(new Error(String(value)));
    },
    _emit(section, event, args) {
        for (const fn of listeners.get(`${section}\0${event}`) || []) {
            try { fn(...(args || [])); } catch (e) { fail(`${section} ${event}`, e); }
        }
    },
    _reconnected() {
        for (const [, p] of pending) p.reject(new Error("pbos-qamd restarted"));
        pending.clear();
    },
    status: () => status,
    open: (key) => openPlugin(key),
    plugins: () => entries(plugins).map((e) => e.key),
};

// --------------------------------------------------------------- Steam ---
function steamReady() {
    const app = window.App;
    return !!(app && ((app.BFinishedInitBeforeLogin && app.BFinishedInitBeforeLogin()) ||
        (app.BFinishedInitStageOne && app.BFinishedInitStageOne())));
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Steam's toast renderer, found by its source.
const TOAST_MARK = 'controller:"notification",method:';
function isToastRenderer(e) {
    return !!(e && e.toString && e.toString().includes(TOAST_MARK));
}

// As @decky/ui's findModuleExport, but reading each export inside the try
// too: there, one export whose getter throws ends the whole search.
function findExport(DFL, filter) {
    for (const m of DFL.modules.values()) {
        if (!m) continue;
        for (const mod of [m.default, m]) {
            if (typeof mod !== "object" || !mod || mod == window) continue;
            for (const name in mod) {
                try {
                    const v = mod[name];
                    if (v && filter(v)) return v;
                } catch (e) { /* a throwing getter: next */ }
            }
        }
    }
    return null;
}

async function waitForSteam() {
    while (!steamReady()) await sleep(100);
}

// Steam's React, its JSX runtime and ReactDOM, found as Decky finds them
// (Decky sets the same globals, with the same modules).
function reactGlobals(webpack) {
    if (!window.SP_REACT) {
        window.SP_REACT = webpack.findModule((m) => m.Component && m.PureComponent && m.useLayoutEffect);
        window.SP_REACTDOM = webpack.findModule((m) => m.createPortal && m.createRoot) ||
            webpack.findModule((m) => m.createPortal && m.__DOM_INTERNALS_DO_NOT_USE_OR_WARN_USERS_THEY_CANNOT_UPGRADE);
        const jsxModule = webpack.findModule((m) => (m.jsx && m.jsxs) || (m.jsx && Object.keys(m).length == 1));
        window.SP_JSX = jsxModule && jsxModule.jsxs ? jsxModule
            : { jsx: jsxModule && jsxModule.jsx, jsxs: jsxModule && jsxModule.jsx, Fragment: window.SP_REACT && window.SP_REACT.Fragment };
    }
}

// @decky/ui: Decky's own (window.DFL) whenever Decky is installed. Starting
// our bundled copy in a page where Decky's already runs (its webpack module
// cache alone was enough) made Steam's UI die a second later (React #31,
// then Steam quit): an AYN Thor on 0.5.4.4, Decky loaded first as it is at
// every boot, 2026-10-16. Ours only without Decky, or if Decky's never comes.
const DECKY_WAIT_S = 90;
async function deckyUI() {
    if (BOOT.decky) for (let i = 0; i < DECKY_WAIT_S * 10 && !window.DFL; i++) await sleep(100);
    if (window.DFL) {
        status.dfl = "decky";
        return window.DFL;
    }
    if (BOOT.decky) note(`Decky's @decky/ui never came (${DECKY_WAIT_S} s): using our own`);
    status.dfl = "own";
    reactGlobals(require("@decky/ui/dist/webpack"));
    return require("@decky/ui");
}

// ------------------------------------------------------------ the icon ---
// Barry's head, pixel for pixel as drawn (ears, square eyes and nose), the
// whiskers trimmed to one pixel each side so the head fills the icon (13 x
// 11); one flat shape like Quick Access's own icons, eyes and nose left open.
const BARRY_HEAD = "M3 0h1v1h-1zM9 0h1v1h-1zM2 1h3v1h-3zM8 1h3v1h-3zM2 2h9v1h-9zM1 3h11v1h-11zM1 4h3v1h-3zM5 4h3v1h-3zM9 4h3v1h-3zM0 5h6v1h-6zM7 5h6v1h-6zM1 6h11v1h-11zM0 7h13v1h-13zM2 8h9v1h-9zM3 9h7v1h-7zM4 10h5v1h-5z";

// ------------------------------------------------------------- plugins ---
const plugins = [];
const pluginWatchers = new Set();
let lastOpen = null; // the plugin shown, kept while Steam runs
let lastLeft = null; // the plugin just closed: its entry takes the focus back

const openWatchers = new Set();

// Shows a plugin's page ("section/id"), or the list (null): the next time
// the tab is drawn, or now if it is open. (For tests: pbos-qamd's probe
// drives it through CEF.)
function openPlugin(key) {
    lastOpen = key || null;
    for (const fn of openWatchers) fn(lastOpen);
}

function pluginsChanged() {
    for (const fn of pluginWatchers) fn(plugins.slice());
}

function addPlugin(section, p) {
    const at = plugins.findIndex((x) => x.section === section && x.id === p.id);
    const entry = { section, group: p.group || GROUPS[section] || section, order: 0, ...p };
    if (at >= 0) plugins[at] = entry;
    else plugins.push(entry);
    pluginsChanged();
}

function removePlugin(section, id) {
    const at = plugins.findIndex((x) => x.section === section && x.id === id);
    if (at >= 0) {
        plugins.splice(at, 1);
        pluginsChanged();
    }
}

// The PB-OS list: these pages, in this order (the layout the PB-OS team set
// in its Quick Access editor, 2026-10-20). Sections add parts to a page with
// plugin({ page, order, content }); a page shows its parts in order, and only
// when some section gave it one. Plugins without a page keep an entry of
// their own after the pages.
const PAGES = [
    { id: "display", title: "Display" },
    { id: "personalization", title: "Personalization" },
    { id: "apps", title: "Apps" },
    { id: "performance", title: "Performance" },
    { id: "hardware", title: "Hardware" },
    { id: "power", title: "Power" },
    { id: "system", title: "System" },
];

// The list's entries: { key, title, icon, parts }. Its icon is its first
// part's.
function entries(list) {
    const out = [];
    for (const pg of PAGES) {
        const parts = list.filter((p) => p.page === pg.id).sort((a, b) => a.order - b.order);
        if (parts.length) out.push({ key: `page/${pg.id}`, title: pg.title, icon: parts[0].icon, parts });
    }
    for (const g of ordered(list.filter((p) => !p.page))) {
        for (const p of g.plugins) out.push({ key: `${p.section}/${p.id}`, title: p.title, icon: p.icon, parts: [p] });
    }
    return out;
}

function ordered(list) {
    const groups = [];
    for (const p of list) if (!groups.includes(p.group)) groups.push(p.group);
    // PB-OS Tools first, then Barry Launcher, then anything else.
    const rank = (g) => { const i = Object.values(GROUPS).indexOf(g); return i < 0 ? 99 : i; };
    groups.sort((a, b) => rank(a) - rank(b));
    return groups.map((g) => ({ group: g, plugins: list.filter((p) => p.group === g).sort((a, b) => a.order - b.order) }));
}

// ------------------------------------------------------------- toaster ---
let ownToaster = null;
let deckyWait = null;

function deckyToaster() {
    const loader = window.DeckyPluginLoader;
    return loader && loader.toaster && typeof loader.toaster.toast === "function" ? loader.toaster : null;
}

function toast(DFL, t) {
    // With Decky installed its toaster shows ours too: two patches on
    // Steam's toast renderer would fight. Without Decky, our own.
    const decky = deckyToaster();
    if (decky) { status.toaster = "decky"; return decky.toast(t); }
    if (BOOT.decky) {
        // Decky starts after Steam, maybe after us: give it a little while.
        deckyWait = (deckyWait || Promise.resolve()).then(async () => {
            for (let i = 0; i < 30 && !deckyToaster(); i++) await sleep(1000);
            const d = deckyToaster();
            if (d) { status.toaster = "decky"; d.toast(t); }
            else note(`toast dropped (Decky's toaster never came): ${t.title}`);
        });
        return;
    }
    if (!ownToaster) {
        try { ownToaster = makeToaster(DFL); } catch (e) { fail("toaster", e); }
    }
    status.toaster = ownToaster ? "own" : "none";
    return ownToaster && ownToaster.toast(t);
}

function makeToaster(DFL) {
    const { jsx, jsxs } = window.SP_JSX;
    const React = window.SP_REACT;
    const renderer = findExport(DFL, isToastRenderer);
    if (!renderer || !window.NotificationStore) {
        fail("toaster", "Steam's toast renderer was not found");
        return null;
    }
    const classes = DFL.findClassModule((m) => m.ShortTemplate) || {};
    const join = (...c) => c.filter(Boolean).join(" ");
    const Popup = React.memo(({ toast: t }) => jsxs("div", {
        style: { "--toast-duration": `${t.duration}ms` }, onClick: t.onClick,
        className: join(classes.ShortTemplate, t.className),
        children: [
            t.logo && jsx("div", { className: classes.StandardLogoDimensions, children: t.logo }),
            jsxs("div", { className: join(classes.Content, t.contentClassName), children: [
                jsxs("div", { className: classes.Header, children: [
                    t.icon && jsx("div", { className: classes.Icon, children: t.icon }),
                    jsx("div", { className: classes.Title, children: t.title }),
                ] }),
                jsx("div", { className: classes.Body, children: t.body }),
            ] }),
        ],
    }));
    const InTray = React.memo(({ toast: t, newIndicator }) => jsx(DFL.Focusable, {
        onActivate: () => { t.onClick && t.onClick(); DFL.Navigation.CloseSideMenus(); },
        className: join(classes.StandardTemplateContainer, t.className),
        children: jsxs("div", { className: classes.StandardTemplate, children: [
            t.logo && jsx("div", { className: classes.StandardLogoDimensions, children: t.logo }),
            jsxs("div", { className: join(classes.Content, t.contentClassName), children: [
                jsxs("div", { className: classes.Header, children: [
                    t.icon && jsx("div", { className: classes.Icon, children: t.icon }),
                    t.title && jsx("div", { className: classes.Title, children: t.title }),
                    t.timestamp && jsx("div", { className: classes.Timestamp,
                        children: t.timestamp.toLocaleTimeString(undefined, { timeStyle: "short" }) }),
                ] }),
                t.body && jsx("div", { className: classes.StandardNotificationDescription, children: t.body }),
            ] }),
            newIndicator && jsx("div", { className: classes.NewIndicator, children: jsx("svg", {
                xmlns: "http://www.w3.org/2000/svg", width: "50", height: "50", viewBox: "0 0 50 50", fill: "none",
                children: jsx("circle", { fill: "currentColor", cx: "25", cy: "25", r: "25" }) }) }),
        ] }),
    }));
    const patched = DFL.injectFCTrampoline(renderer);
    DFL.replacePatch(patched, "component", (args) => {
        const a = args && args[0];
        if (a && a.group && (a.group.pbos || (a.group.notifications && a.group.notifications[0] && a.group.notifications[0].pbos))) {
            return a.group.notifications.map((n) => jsx(DFL.ErrorBoundary, {
                children: a.location === 1 ? jsx(Popup, { toast: n.data }) : jsx(InTray, { toast: n.data, newIndicator: n.bNewIndicator }),
            }));
        }
        return DFL.callOriginal;
    });
    return {
        toast(t) {
            if (t.sound === undefined) t.sound = 6;
            if (t.playSound === undefined) t.playSound = true;
            if (t.showToast === undefined) t.showToast = true;
            if (t.timestamp === undefined) t.timestamp = new Date();
            if (t.showNewIndicator === undefined) t.showNewIndicator = true;
            const data = {
                nNotificationID: window.NotificationStore.m_nNextTestNotificationID++,
                bNewIndicator: t.showNewIndicator, rtCreated: Date.now(), eType: t.eType || 31, eSource: 1,
                nToastDurationMS: t.duration || (t.duration = 5e3), data: t, pbos: true,
            };
            let group;
            const info = {
                showToast: t.showToast, sound: t.sound, playSound: t.playSound, eFeature: 0,
                toastDurationMS: data.nToastDurationMS, bCritical: t.critical,
                fnTray: (n, tray) => { group = { eType: n.eType, notifications: [n], pbos: true }; tray.unshift(group); },
            };
            try { window.NotificationStore.ProcessNotification(info, data, 0); } catch (e) { fail("toast", e); }
            return { data: t, dismiss() { try { group && window.NotificationStore.RemoveGroupFromTray(group); } catch (e) { /* gone */ } } };
        },
    };
}

// --------------------------------------------------------- file picker ---
function filePicker(DFL, start, includeFiles, includeFolders, extensions, showHidden) {
    const { jsx, jsxs } = window.SP_JSX;
    const { useState, useEffect } = window.SP_REACT;
    return new Promise((resolve, reject) => {
        let settled = false;
        const done = (fn, v) => { if (!settled) { settled = true; fn(v); } };
        function Picker({ closeModal }) {
            const [path, setPath] = useState(start);
            const [st, setSt] = useState(null);
            useEffect(() => {
                setSt(null);
                send("files", "list", [path, includeFiles !== false, includeFolders !== false, extensions || [], !!showHidden])
                    .then(setSt).catch((e) => setSt({ ok: false, error: String(e.message || e) }));
            }, [path]);
            const close = () => { closeModal(); done(reject, new Error("closed")); };
            const pick = (e) => {
                if (e.dir) { setPath(e.path); return; }
                closeModal();
                done(resolve, { path: e.path, realpath: e.path });
            };
            const rows = !st ? [jsx("div", { style: { opacity: 0.7 }, children: "Loading…" }, "l")]
                : !st.ok ? [jsx("div", { style: { color: "#ff8080" }, children: st.error }, "e")]
                : [
                    ...(st.parent ? [jsx(DFL.DialogButton, { style: { textAlign: "left", marginBottom: "4px" },
                        onClick: () => setPath(st.parent), children: "⬑  Up a folder" }, "up")] : []),
                    ...(st.entries.length ? st.entries.map((e) => jsx(DFL.DialogButton, {
                        style: { textAlign: "left", marginBottom: "4px", display: "flex", justifyContent: "space-between" },
                        onClick: () => pick(e),
                        children: [jsx("span", { children: `${e.dir ? "📁" : "📄"}  ${e.name}` }, "n"),
                            !e.dir && jsx("span", { style: { opacity: 0.6 }, children: e.size >= 1e6 ? `${(e.size / 1e6).toFixed(1)} MB` : `${Math.max(1, Math.round(e.size / 1e3))} KB` }, "s")],
                    }, e.path)) : [jsx("div", { style: { opacity: 0.7 }, children: "Nothing to pick here." }, "none")]),
                ];
            return jsx(DFL.ModalRoot, { onCancel: close, closeModal: close, children: jsxs("div", { children: [
                jsx("div", { style: { fontWeight: "bold", marginBottom: "6px" }, children: "Pick a file" }),
                jsx("div", { style: { fontSize: "12px", opacity: 0.7, marginBottom: "8px", wordBreak: "break-all" }, children: st && st.path ? st.path : path }),
                jsx(DFL.Focusable, { style: { display: "flex", gap: "6px", marginBottom: "8px" }, "flow-children": "horizontal",
                    children: ((st && st.places) || []).map((p) => jsx(DFL.DialogButton, { style: { minWidth: 0, padding: "4px 10px" },
                        onClick: () => setPath(p), children: p.split("/").pop() || p }, p)) }),
                jsx(DFL.Focusable, { style: { maxHeight: "50vh", overflowY: "auto" }, children: rows }),
            ] }) });
        }
        DFL.showModal(jsx(Picker, {}));
    });
}

// --------------------------------------------------------------- panel ---
function makePanel(DFL) {
    const { jsx, jsxs } = window.SP_JSX;
    const React = window.SP_REACT;
    const { useState, useEffect, useRef, createContext, useContext } = React;
    const Visible = createContext(false);
    const GB = DFL.GamepadButton || {};
    const LB = GB.BUMPER_LEFT != null ? GB.BUMPER_LEFT : 5;
    const RB = GB.BUMPER_RIGHT != null ? GB.BUMPER_RIGHT : 6;
    const titleStyle = { display: "flex", alignItems: "center", gap: "8px", paddingRight: "16px", position: "sticky", top: "0px" };
    const smallButton = { height: "28px", width: "40px", minWidth: 0, padding: "10px 12px" };

    function VisibleProvider({ tab, children }) {
        const [visible, setVisible] = useState(tab.initialVisibility);
        tab.qAMVisibilitySetter = (v) => { if (v != visible) setVisible(v); };
        return jsx(Visible.Provider, { value: visible, children });
    }

    function Head() {
        // Square, the head centered in it.
        return jsx("svg", { viewBox: "0 -1 13 13", width: "1em", height: "1em", fill: "currentColor",
            shapeRendering: "crispEdges", children: jsx("path", { d: BARRY_HEAD }) });
    }

    function Panel() {
        const [list, setList] = useState(plugins.slice());
        const [open, setOpen] = useState(lastOpen);
        const visible = useContext(Visible);
        useEffect(() => { pluginWatchers.add(setList); return () => pluginWatchers.delete(setList); }, []);
        useEffect(() => { openWatchers.add(setOpen); return () => openWatchers.delete(setOpen); }, []);
        const show = (key) => {
            if (!key && lastOpen) lastLeft = lastOpen;
            lastOpen = key;
            setOpen(key);
        };
        const listRef = useRef(null);
        // Back on the list: the entry just left has the focus, not the first.
        useEffect(() => {
            if (open || !lastLeft || !listRef.current) return;
            const mark = listRef.current.querySelector(`[data-pbos-entry="${CSS.escape(lastLeft)}"]`);
            const target = mark && mark.closest("[tabindex], button");
            if (target) target.focus();
        }, [open, list]);
        const flat = entries(list);
        const key = (e) => e.key;
        const active = flat.find((e) => key(e) === open);
        if (active) {
            const step = (by) => {
                const i = flat.indexOf(active);
                show(key(flat[(i + by + flat.length) % flat.length]));
            };
            return jsxs(DFL.Focusable, {
                className: "pbos-qam-panel",
                onCancelButton: () => show(null),
                onButtonDown: (evt) => {
                    const b = evt && evt.detail && evt.detail.button;
                    if (b === LB) step(-1);
                    else if (b === RB) step(1);
                },
                children: [
                    jsxs(DFL.Focusable, { className: DFL.staticClasses.Title, style: titleStyle, children: [
                        jsx(DFL.DialogButton, { style: smallButton, onClick: () => show(null),
                            children: jsx("svg", { viewBox: "0 0 24 24", width: "16", height: "16", fill: "currentColor", style: { display: "block", marginTop: "-3px" },
                                children: jsx("path", { d: "M10.6 5 3.6 12l7 7 1.4-1.4L7.4 13H20v-2H7.4L12 6.4z" }) }) }),
                        jsx("div", { style: { flex: 0.9 }, children: active.title }),
                    ] }),
                    jsx("div", { style: { height: "100%", paddingTop: "8px" },
                        // One error boundary per part: a part that failed must not
                        // take the page's other parts, or the next page, with it.
                        children: visible && active.parts.map((p) =>
                            jsx(DFL.ErrorBoundary, { children: jsx(p.content, {}) }, `${key(active)}/${p.section}/${p.id}`)) }),
                ],
            });
        }
        return jsxs("div", { className: "pbos-qam-panel", children: [
            jsxs("div", { className: DFL.staticClasses.Title, style: titleStyle, children: [
                jsx("div", { style: { fontSize: "22px", display: "flex" }, children: jsx(Head, {}) }),
                jsx("div", { style: { flex: 0.9 }, children: "PB-OS" }),
            ] }),
            jsx("div", { ref: listRef, style: { paddingTop: "8px" }, children: list.length === 0
                ? jsx(DFL.PanelSection, { children: jsx(DFL.PanelSectionRow, { children: jsx("div", { style: { fontSize: "12px", opacity: 0.75 }, children: "Loading…" }) }) })
                // One list: the pages this device has (PAGES), without headings.
                : jsx(DFL.PanelSection, { children: flat.map((p) => jsx(DFL.PanelSectionRow, { children: jsx(DFL.ButtonItem, {
                    layout: "below", onClick: () => show(key(p)), autoFocus: key(p) === lastLeft,
                    children: jsxs("div", { "data-pbos-entry": key(p), style: { display: "flex", alignItems: "center", gap: "12px" }, children: [
                        p.icon && jsx("div", { style: { display: "flex", width: "22px", justifyContent: "center" }, children: jsx(p.icon, {}) }),
                        jsx("div", { children: p.title }),
                    ] }),
                }) }, key(p))) }) }),
        ] });
    }

    return { Panel, Head, VisibleProvider };
}

// -------------------------------------------------------------- QAM tab ---
function hookTabs(DFL, view) {
    const { jsx } = window.SP_JSX;
    const qamModule = DFL.findModuleByExport((e) => e && e.type && e.type.toString && e.type.toString().includes("QuickAccessMenuBrowserView"));
    if (!qamModule) throw new Error("Quick Access's module was not found");
    const browserView = Object.values(qamModule).find((e) => e && e.type && e.type.toString && e.type.toString().includes("QuickAccessMenuBrowserView"));
    const embedded = Object.values(qamModule).find((e) => e && e.type && e.type.toString && e.type.toString().includes("QuickAccessMenuEmbedded"));
    const render = (tabs, visible) => {
        status.tabRendered++;
        const ours = tabs.find((t) => t && t.pbos);
        if (ours) {
            if (ours.qAMVisibilitySetter) ours.qAMVisibilitySetter(visible);
            else ours.initialVisibility = visible;
            return;
        }
        const tab = { key: QAM_TAB_KEY, title: null, tab: jsx(view.Head, {}), pbos: true, initialVisibility: visible };
        tab.panel = jsx(DFL.ErrorBoundary, { children: jsx(view.VisibleProvider, { tab, children: jsx(view.Panel, {}) }) });
        tabs.push(tab);
    };
    const handler = DFL.createReactTreePatcher(
        [(tree) => DFL.findInReactTree(tree, (node) => node && node.props && node.props.onFocusNavDeactivated)],
        (args, ret) => {
            const tabs = DFL.findInReactTree(ret, (x) => x && x.props && x.props.tabs);
            if (tabs) render(tabs.props.tabs, args[0].visible);
            return ret;
        },
        "PB-OS tab",
    );
    const patches = [DFL.afterPatch(browserView, "type", handler)];
    if (embedded) patches.push(DFL.afterPatch(embedded, "type", handler));
    // A Quick Access already drawn takes the patched type at its next render.
    const root = DFL.getReactRoot(document.getElementById("root"));
    const node = root && DFL.findInReactTree(root, (n) => n.elementType == browserView || (embedded != null && n.elementType == embedded));
    if (node) {
        node.type = node.elementType.type;
        if (node.alternate) node.alternate.type = node.type;
    }
    status.tabPatched = true;
    if (window.__PBOS_TABS_PATCHES) for (const p of window.__PBOS_TABS_PATCHES) try { p.unpatch(); } catch (e) { /* old */ }
    window.__PBOS_TABS_PATCHES = patches;
}

// ---------------------------------------------------------- the check ---
// What the PB-OS tab and its sections rely on in Steam's frontend, looked
// for without patching or loading anything: pbos-qamd --check, before every
// release.
async function probe(DFL) {
    const items = [];
    // Steam's own getters can throw (collectionStore before sign-in): each
    // value is looked at inside a try.
    const item = (name, ok, detail) => {
        let v = false;
        // ok: a value, or a check to run (a function never stands for itself).
        try { v = typeof ok === "function" ? ok() : ok; } catch (e) { detail = `${detail ? detail + "; " : ""}${e}`; }
        items.push({ name, ok: !!v, detail: detail || "" });
    };
    item("Steam initialized", steamReady());
    item("webpack chunks", !!window.webpackChunksteamui, "window.webpackChunksteamui");
    item("React", !!window.SP_REACT, window.SP_REACTDOM && window.SP_REACTDOM.version);
    item("JSX runtime", !!(window.SP_JSX && window.SP_JSX.jsx));
    item("@decky/ui", !!DFL, status.dfl === "decky" ? "Decky's (window.DFL)" : "our own copy");
    {
        const find = (f) => { try { return f(); } catch (e) { return null; } };
        const qam = find(() => DFL.findModuleByExport((e) => e && e.type && e.type.toString && e.type.toString().includes("QuickAccessMenuBrowserView")));
        item("Quick Access module", qam, "an export whose type mentions QuickAccessMenuBrowserView");
        item("Quick Access embedded view", () => qam && Object.values(qam).find((e) => e && e.type && e.type.toString && e.type.toString().includes("QuickAccessMenuEmbedded")), "optional");
        item("toast renderer", !!find(() => findExport(DFL, isToastRenderer)), `an export containing ${TOAST_MARK}`);
        item("toast classes", find(() => DFL.findClassModule((m) => m.ShortTemplate)), "ShortTemplate");
        item("NotificationStore", () => window.NotificationStore && typeof window.NotificationStore.ProcessNotification === "function",
            "ProcessNotification, RemoveGroupFromTray, m_nNextTestNotificationID");
        for (const name of ["PanelSection", "PanelSectionRow", "ButtonItem", "DialogButton", "Focusable", "Field", "ToggleField",
            "Toggle", "SliderField", "DropdownItem", "TextField", "ConfirmModal", "ModalRoot", "showModal", "ProgressBarWithInfo",
            "ErrorBoundary", "Router", "Navigation", "findSP", "GamepadButton", "staticClasses"]) {
            item(`DFL.${name}`, find(() => DFL[name]) != null);
        }
        item("staticClasses.Title", find(() => DFL.staticClasses.Title));
        item("Router.RunningApps", find(() => Array.isArray(DFL.Router.RunningApps)), "Barry Launcher's companion apps");
        item("React root", find(() => DFL.getReactRoot(document.getElementById("root"))), "#root");
    }
    const SC = window.SteamClient || {};
    const fn = (path) => path.split(".").reduce((o, k) => (o ? o[k] : undefined), SC);
    for (const path of ["Apps.RegisterForAppDetails", "Apps.RegisterForAppOverviewChanges", "Apps.RegisterForGameActionStart",
        "Apps.SetAppLaunchOptions", "Apps.SetShortcutExe", "Apps.RemoveShortcut", "Apps.SpecifyCompatTool", "Settings.SpecifyGlobalCompatTool",
        "GameSessions.RegisterForAppLifetimeNotifications"]) {
        item(`SteamClient.${path}`, typeof fn(path) === "function");
    }
    item("settingsStore.m_Settings", () => window.settingsStore && window.settingsStore.m_Settings, "strCompatTool");
    item("collectionStore", () => window.collectionStore && window.collectionStore.localGamesCollection && window.collectionStore.allAppsCollection,
        "localGamesCollection, allAppsCollection (needs a signed-in user)");
    item("virtual keyboard manager", () => {
        const inst = window.SteamUIStore && window.SteamUIStore.ActiveWindowInstance;
        const vkm = inst && (inst.VirtualKeyboardManager || inst.m_VirtualKeyboardManager);
        return vkm && typeof Object.getPrototypeOf(vkm).SetVirtualKeyboardShownInternal === "function";
    },
        "SteamUIStore.ActiveWindowInstance.VirtualKeyboardManager.SetVirtualKeyboardShownInternal (Barry's keyboard)");
    item("SteamUIStore.WindowStore", () => window.SteamUIStore && window.SteamUIStore.WindowStore && window.SteamUIStore.WindowStore.SteamUIWindows,
        "SteamUIWindows (Barry's trackpad clicks)");
    item("running tab: patched", status.tabPatched);
    item("running tab: drawn", status.tabRendered > 0, `${status.tabRendered} renders (open Quick Access once if 0)`);
    item("running sections", status.sections.length > 0, status.sections.join(", "));
    item("running errors", status.errors.length === 0, status.errors.slice(-3).join(" | "));
    let steam = "";
    try {
        const info = SC.System && SC.System.GetSystemInfo ? await SC.System.GetSystemInfo() : null;
        if (info) steam = Object.entries(info).filter(([k]) => /steam|build|version/i.test(k)).map(([k, v]) => `${k}=${v}`).join(" ");
    } catch (e) { steam = String(e); }
    const optional = new Set(["Quick Access embedded view", "running tab: drawn"]);
    return { ok: items.every((i) => i.ok || optional.has(i.name)), steam, items };
}

// ---------------------------------------------------------------- start ---
async function start() {
    const queued = [];
    window.__pbosQam = { ...bridge, section: (name, fn) => queued.push([name, fn]) };
    await waitForSteam();
    const DFL = await deckyUI();
    // pbos-qamd --check runs this in the running tab, with the @decky/ui in
    // use: loading another copy into Steam's page made Steam quit (seen in
    // the Thor VM, 2026-10-16).
    // A toast as the sections send them (tests: pbos-qamd's probe).
    window.__pbosQam.toast = (t) => toast(DFL, t);
    window.__pbosQam.probe = () => probe(DFL).catch((e) => ({
        ok: false, steam: "", items: [{ name: "probe", ok: false, detail: String((e && e.stack) || e) }] }));
    const view = makePanel(DFL);
    try { hookTabs(DFL, view); } catch (e) { fail("Quick Access tab", e); }
    const api = (name) => ({
        DFL,
        SP_REACT: window.SP_REACT,
        SP_JSX: window.SP_JSX,
        callable: (method) => (...args) => send(name, method, args),
        toaster: { toast: (t) => toast(DFL, t) },
        addEventListener: (event, fn) => {
            const k = `${name}\0${event}`;
            if (!listeners.has(k)) listeners.set(k, new Set());
            listeners.get(k).add(fn);
        },
        removeEventListener: (event, fn) => { const s = listeners.get(`${name}\0${event}`); if (s) s.delete(fn); },
        // As @decky/api's openFilePicker(select, startPath, includeFiles, includeFolders, filter, extensions, showHidden).
        openFilePicker: (_select, startPath, includeFiles, includeFolders, _filter, extensions, showHidden) =>
            filePicker(DFL, startPath, includeFiles, includeFolders, extensions, showHidden),
        plugin: (p) => addPlugin(name, p),
        unplug: (id) => removePlugin(name, id),
        fail: (where, e) => fail(`${name}: ${where}`, e),
    });
    const run = (name, fn) => {
        try {
            fn(api(name));
            status.sections.push(name);
        } catch (e) {
            fail(`section ${name}`, e);
        }
    };
    for (const [name, fn] of queued) run(name, fn);
    window.__pbosQam.section = run;
}

start().catch((e) => fail("start", e));

// The App Store in PB-OS's Quick Access tab: emulators and apps from the
// catalog that ships with PB-OS, installed on request (daemon/store.cpp).
// Follows Armada's Armada Store (armada-os/armada, GPL-2.0-or-later,
// decky/armada-store), with the stock DS and 3DS emulators. The loader
// (src/loader.js) gives P: @decky/ui as DFL, Steam's React and JSX,
// callable(), addEventListener(), openFilePicker() and plugin().
window.__pbosQam.section("store", (P) => {
const { DFL, SP_REACT, SP_JSX, callable } = P;
const { useEffect, useState, useCallback } = SP_REACT;
const jsx = SP_JSX.jsx;
const jsxs = SP_JSX.jsxs;

const getCatalog = callable("get_catalog");
const getStatus = callable("get_status");
const checkUpdates = callable("check_updates");
const install = callable("install");
const uninstall = callable("uninstall");
const cancel = callable("cancel");
const dismiss = callable("dismiss");
const recordShortcut = callable("record_shortcut");
const clearShortcut = callable("clear_shortcut");
const prepareShortcut = callable("prepare_shortcut");
const getArtwork = callable("artwork");
const recordArt = callable("record_art");

const CATEGORIES = [
    { key: "emulators", title: "Emulators" },
    { key: "applications", title: "Apps" },
];
const PHASES = {
    queued: "Waiting", resolving: "Finding the download", downloading: "Downloading", installing: "Installing",
    removing: "Removing", cancelled: "Cancelled", done: "Done",
};
const ACTIVE = ["queued", "resolving", "downloading", "installing", "removing"];
const active = (job) => !!job && ACTIVE.includes(job.phase);
const jobText = (job) => job.phase === "error" ? (job.error || "Failed")
    : `${PHASES[job.phase] || job.phase}${job.percent != null ? ` ${job.percent}%` : "…"}`;

const toast = (title, body) => { try { P.toaster.toast({ title, body }); } catch (e) { /* no toaster */ } };

// ------------------------------------------------------- Steam shortcuts ---
const steamApps = () => window.SteamClient && window.SteamClient.Apps;

async function addToSteam(launch) {
    const apps = steamApps();
    if (!apps || !apps.AddShortcut) throw new Error("Steam can't add shortcuts here.");
    const appid = Number(await apps.AddShortcut(launch.name, launch.exe, launch.startDir, launch.launchOptions || ""));
    if (!appid) throw new Error("Steam didn't make the shortcut.");
    // A new shortcut is named after its program at first.
    try { apps.SetShortcutName(appid, launch.name); } catch (e) { /* kept Steam's */ }
    try { if (launch.launchOptions) apps.SetShortcutLaunchOptions(appid, launch.launchOptions); } catch (e) { /* in AddShortcut */ }
    return appid;
}

function removeFromSteam(appid) {
    const apps = steamApps();
    if (!apps || !apps.RemoveShortcut) throw new Error("Steam can't remove shortcuts here.");
    apps.RemoveShortcut(appid);
}

// The app's own artwork (store/art/ID/, from the backend) on its shortcut:
// capsule, wide, hero and logo through Steam's custom artwork, the icon by
// path. A shortcut the user already gave a capsule keeps all of theirs.
async function applyArt(id, appid) {
    const apps = steamApps();
    if (!apps || !apps.SetCustomArtworkForApp) return;
    const art = await getArtwork(id, appid);
    if (!art.hasOwn) {
        for (const img of art.images) {
            try { await apps.SetCustomArtworkForApp(appid, img.data, img.ext, img.type); } catch (e) { /* the rest still */ }
        }
        try { if (art.icon && apps.SetShortcutIcon) apps.SetShortcutIcon(appid, art.icon); } catch (e) { /* no icon */ }
    }
    await recordArt(id, appid);
}

// A non-Steam shortcut starts by its 64-bit game id: (appid << 32) | 0x02000000.
function launch(appid) {
    const gameid = ((BigInt(appid >>> 0) << 32n) | 0x02000000n).toString();
    const apps = steamApps();
    if (apps && apps.RunGame) apps.RunGame(gameid, "", -1, 100);
    else if (window.SteamClient && window.SteamClient.URL) window.SteamClient.URL.ExecuteSteamURL(`steam://rungameid/${gameid}`);
}

// Whether Steam still has the shortcut (the user may remove it in Steam's
// library); true when Steam's app store can't be asked.
function inSteam(appid) {
    try {
        const store = window.appStore;
        return !store || typeof store.GetAppOverviewByAppID !== "function" || !!store.GetAppOverviewByAppID(appid);
    } catch (e) {
        return true;
    }
}

// What finished installing gets its shortcut here, whether or not the page
// is open (the backend keeps the list until a shortcut is recorded).
let catalog = null;
const tried = new Set();
async function addPending() {
    try {
        if (!catalog) catalog = await getCatalog();
        const st = await getStatus();
        for (const id of st.pending || []) {
            if (tried.has(id)) continue;
            const app = catalog.apps.find((a) => a.id === id);
            if (!app || !app.launch || !(st.installed[id] || {}).installed) continue;
            tried.add(id);  // AddShortcut may have run before a failure: never twice
            try {
                const appid = await addToSteam(app.launch);
                await recordShortcut(id, appid);
                try { await applyArt(id, appid); } catch (e) { P.fail("artwork", e); }
                toast(app.name, "Added to your Steam library");
            } catch (e) {
                toast(app.name, String(e.message || e));
            }
        }
        await artPass(st);
    } catch (e) {
        P.fail("shortcuts", e);
    }
}

// Shortcuts made before their artwork shipped get it once. Steam's library
// fills in after Steam starts: one it doesn't know yet waits for Steam's
// next library change (a removed shortcut just stays waiting).
let artWaiting = [];
let artTimer = null;
async function artPass(st) {
    const waiting = [];
    for (const [id, appid] of Object.entries(st.shortcuts || {})) {
        if ((st.art || {})[id] === appid) continue;
        if (!inSteam(appid)) { waiting.push([id, appid]); continue; }
        try { await applyArt(id, appid); } catch (e) { P.fail("artwork", e); }
    }
    artWaiting = waiting;
}
function onLibraryChange() {
    if (!artWaiting.length || artTimer) return;
    // Changes come in bursts: one look after the burst.
    artTimer = setTimeout(async () => {
        artTimer = null;
        const ready = artWaiting.filter(([, appid]) => inSteam(appid));
        artWaiting = artWaiting.filter(([, appid]) => !inSteam(appid));
        for (const [id, appid] of ready) {
            try { await applyArt(id, appid); } catch (e) { P.fail("artwork", e); }
        }
    }, 1000);
}

// --------------------------------------------------------------- state ---
let jobs = [];
const jobWatchers = new Set();
P.addEventListener("jobs", (list) => {
    const before = new Map(jobs.map((j) => [j.appId, j]));
    jobs = list || [];
    for (const j of jobs) {
        const was = before.get(j.appId);
        if (!was || !active(was) || active(j)) continue;
        const app = catalog && catalog.apps.find((a) => a.id === j.appId);
        const name = app ? app.name : j.appId;
        if (j.phase === "error") toast(name, j.error || "Failed");
        else if (j.phase === "done") toast(name, j.action === "uninstall" ? "Uninstalled" : "Installed");
    }
    if (jobs.some((j) => j.phase === "done" && j.action === "install")) addPending();
    for (const fn of jobWatchers) fn(jobs);
});

function useStore() {
    const [cat, setCat] = useState(catalog);
    const [st, setSt] = useState(null);
    const [ups, setUps] = useState({});
    const [jobList, setJobs] = useState(jobs);
    const refresh = useCallback(() => getStatus().then((s) => { setSt(s); setJobs(s.jobs || []); }).catch(() => {}), []);
    useEffect(() => {
        if (!catalog) getCatalog().then((c) => { catalog = c; setCat(c); }).catch(() => setCat({ apps: [], error: true }));
        refresh();
        checkUpdates(false).then(setUps).catch(() => {});
        const w = (list) => { setJobs(list); refresh(); };
        jobWatchers.add(w);
        return () => jobWatchers.delete(w);
    }, []);
    return { cat, st, ups, jobList, refresh, setUps };
}

// ------------------------------------------------------------------ UI ---
const row = (child, key) => jsx(DFL.PanelSectionRow, { children: child }, key);
const note = (text, key) => row(jsx("div", { style: { fontSize: "12px", opacity: 0.75 }, children: text }), key);
const button = (label, onClick, key, disabled) => row(jsx(DFL.ButtonItem, { layout: "below", onClick, disabled, children: label }), key);

const iconStyle = { width: "32px", height: "32px", borderRadius: "6px", flex: "0 0 auto", objectFit: "contain" };
function AppRow({ app, info, job, update, onClick }) {
    let state = "";
    if (active(job)) state = job.percent != null ? `${job.percent}%` : "…";
    else if (job && job.phase === "error") state = "!";
    else if (info && info.installed && update) state = "Update";
    else if (info && info.installed) state = "✓";
    return row(jsx(DFL.ButtonItem, { layout: "below", onClick, children: jsxs("div", {
        style: { display: "flex", alignItems: "center", gap: "10px", width: "100%" }, children: [
            app.icon ? jsx("img", { src: app.icon, style: iconStyle, loading: "lazy" }, "i")
                : jsx("div", { style: { ...iconStyle, background: "#ffffff22" } }, "i"),
            jsxs("div", { style: { flex: "1 1 auto", textAlign: "left", minWidth: 0 }, children: [
                jsx("div", { children: app.name }, "n"),
                jsx("div", { style: { fontSize: "11px", opacity: 0.7 }, children: app.summary }, "s"),
            ] }, "t"),
            state && jsx("div", { style: { fontSize: "12px", opacity: 0.85 }, children: state }, "st"),
        ] }) }), app.id);
}

function AppPage({ app, store }) {
    const { st, ups, jobList, refresh, setUps } = store;
    const job = jobList.find((j) => j.appId === app.id);
    const info = (st && st.installed[app.id]) || {};
    const shortcut = st && st.shortcuts[app.id];
    const update = info.installed && info.managed && ups[app.id];
    const run = (p, done) => p.then(() => { if (done) done(); refresh(); }).catch((e) => toast(app.name, String(e.message || e)));
    const items = [note(app.summary, "sum")];
    if (app.note) items.push(note(app.note, "note"));
    if (job) items.push(note(jobText(job), "job"));
    if (active(job)) {
        items.push(button("Cancel", () => run(cancel(app.id)), "cancel"));
        return jsx(DFL.PanelSection, { title: app.name, children: items });
    }
    if (job && job.phase === "error") items.push(button("Clear the error", () => run(dismiss(app.id)), "dismiss"));
    if (info.installed && shortcut != null && !app.desktopOnly && inSteam(shortcut)) items.push(button("Play", () => launch(shortcut), "play"));
    if (!info.installed) items.push(button("Install", () => run(install(app.id)), "install"));
    else if (update) items.push(button(`Update${update.latest ? ` to ${update.latest}` : ""}`, () => run(install(app.id), () => setUps((u) => { const n = { ...u }; delete n[app.id]; return n; })), "update"));
    if (info.installed && !info.managed)
        items.push(note("Installed outside the App Store (by EmuDeck, for example): it updates and uninstalls there.", "foreign"));
    if (app.desktopOnly && info.installed) items.push(note("Open it in Desktop Mode.", "desk"));
    if (info.installed && info.managed)
        items.push(button("Uninstall", () => DFL.showModal(jsx(DFL.ConfirmModal, {
            strTitle: `Uninstall ${app.name}?`,
            strDescription: "The program is removed. Its settings and saves stay in your home folder.",
            strOKButtonText: "Uninstall",
            onOK: () => {
                if (shortcut != null) { try { removeFromSteam(shortcut); clearShortcut(app.id, shortcut); } catch (e) { /* kept */ } }
                run(uninstall(app.id));
            },
        })), "uninstall"));
    return jsx(DFL.PanelSection, { title: app.name, children: items });
}

// The page and category shown, kept while Steam runs (Quick Access redraws
// the panel whenever a modal takes the focus).
let view = { category: null, app: null };

function StorePage() {
    const store = useStore();
    const [v, setV] = useState(view);
    const go = (next) => { view = next; setV(next); };
    // The frame's back arrow and B go up one level here: app, list, App Store.
    useEffect(() => {
        if (!P.setBack) return undefined;
        const up = () => {
            if (view.app) { go({ category: view.category, app: null }); return true; }
            if (view.category) { go({ category: null, app: null }); return true; }
            return false;
        };
        P.setBack(up);
        return () => P.setBack(null, up);
    }, []);
    const { cat, st, ups, jobList } = store;
    if (!cat) return jsx(DFL.PanelSection, { children: note("Loading…") });
    if (cat.error) return jsx(DFL.PanelSection, { children: note("The App Store's list can't be read.") });
    const jobOf = (id) => jobList.find((j) => j.appId === id);
    const app = v.app && cat.apps.find((a) => a.id === v.app);
    if (app) return jsx(AppPage, { app, store });
    const category = CATEGORIES.find((c) => c.key === v.category);
    if (category) {
        const apps = cat.apps.filter((a) => a.category === category.key)
            .sort((a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: "base" }));
        return jsx(DFL.PanelSection, { title: category.title, children: [
            ...apps.map((a) => jsx(AppRow, { app: a, info: st && st.installed[a.id], job: jobOf(a.id), update: ups[a.id],
                onClick: () => go({ category: category.key, app: a.id }) }, a.id)),
        ] });
    }
    const addFile = async () => {
        let picked;
        try { picked = await P.openFilePicker(0, cat.home, true, true, undefined, [], false); } catch (e) { return; }
        const path = picked && (picked.realpath || picked.path);
        if (!path) return;
        try {
            const spec = await prepareShortcut(path);
            await addToSteam(spec);
            toast(spec.name, "Added to your Steam library");
        } catch (e) {
            toast("Not added", String(e.message || e));
        }
    };
    return jsx(DFL.PanelSection, { title: "App Store", children: [
        ...CATEGORIES.map((c) => {
            const busy = cat.apps.filter((a) => a.category === c.key).map((a) => jobOf(a.id)).find(active);
            const n = cat.apps.filter((a) => a.category === c.key && st && (st.installed[a.id] || {}).installed && ups[a.id]).length;
            return button(`${c.title}${busy ? ` · ${jobText(busy)}` : n ? ` · ${n} update${n > 1 ? "s" : ""}` : ""}`,
                () => go({ category: c.key, app: null }), c.key);
        }),
        button("Add a Non-Steam Game", addFile, "file"),
        note("PB-OS ships no emulators: these download from Flathub or the project's own releases when you pick them.", "src"),
    ] });
}

// A shopping bag.
const StoreIcon = () => jsx("svg", {
    width: 22, height: 22, viewBox: "0 0 24 24", fill: "none", stroke: "currentColor",
    strokeWidth: 1.6, strokeLinecap: "round", strokeLinejoin: "round", children: [
        jsx("path", { d: "M5 8h14l-1 12.5a1.5 1.5 0 0 1-1.5 1.5h-9A1.5 1.5 0 0 1 6 20.5z" }, "bag"),
        jsx("path", { d: "M9 10V6.5a3 3 0 0 1 6 0V10" }, "handle"),
    ],
});

P.plugin({ id: "store", title: "App Store", icon: StoreIcon, content: StorePage, group: "App Store", order: 10 });
// Installs that ended while Steam was closed get their shortcuts now.
addPending();
try { steamApps().RegisterForAppOverviewChanges(onLibraryChange); } catch (e) { P.fail("artwork", e); }
});

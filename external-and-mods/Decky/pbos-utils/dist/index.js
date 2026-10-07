const manifest = {"name":"PB-OS Utils"};
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
const toaster = api.toaster;
const definePlugin = (fn) => (...args) => fn(...args);

const DFL = window.DFL;
const SP_REACT = window.SP_REACT;
const SP_JSX = window.SP_JSX;
const { useEffect, useState, useCallback, useRef } = SP_REACT;
const jsx = SP_JSX.jsx;
const jsxs = SP_JSX.jsxs;

// Decky callable(): arguments are passed positionally to the Python method.
const getState = callable("get_state");
const check = callable("check");
const start = callable("start");
const startLocal = callable("start_local");
const checkLocal = callable("check_local");
const pause = callable("pause");
const restart = callable("restart");
const getInstall = callable("get_install");
const startMove = callable("start_move");
const powerOff = callable("power_off");
const getLights = callable("get_lights");
const setLights = callable("set_lights");
const setPowerLed = callable("set_power_led");

const row = (child) => jsx(DFL.PanelSectionRow, { children: child });
const note = (text) => row(jsx("div", { style: { fontSize: "12px", opacity: 0.75 }, children: text }));
const warn = (text) => row(jsx("div", { style: { fontSize: "12px", color: "#ffb070" }, children: text }));
const failure = (text) => row(jsx("div", { style: { color: "#ff8080" }, children: text }));
const gb = (n) => (n >= 1e9 ? `${(n / 1e9).toFixed(1)} GB` : `${Math.max(1, Math.round(n / 1e6))} MB`);
const GIB = 1024 ** 3;

// Own bar: DFL.ProgressBarWithInfo lays out as a Field, with the bar in the
// value column, and ran off the panel's right edge.
function bar(text, pct) {
    return row(jsxs("div", { style: { width: "100%" }, children: [
        jsx("div", { style: { fontSize: "12px", opacity: 0.85, marginBottom: "6px" }, children: text }),
        jsx("div", { style: { width: "100%", height: "8px", borderRadius: "4px", background: "rgba(255,255,255,0.15)", overflow: "hidden" },
            children: jsx("div", { style: { width: `${Math.min(100, pct)}%`, height: "100%", background: "#1a9fff" } }) }),
    ] }));
}

// ----------------------------------------------------------------- tabs ---
const svg = (children) => () => jsx("svg", { viewBox: "0 0 24 24", width: "22", height: "22", fill: "currentColor", children });
const UpdateIcon = svg(jsx("path", { d: "M11 3h2v10.2l3.6-3.6 1.4 1.4-6 6-6-6 1.4-1.4 3.6 3.6zM4 19h16v2H4z" }));
// A microSD card with an arrow across it: moving SteamOS.
const InstallIcon = svg([
    jsx("path", { d: "M7 2h8l4 4v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V4a2 2 0 0 1 2-2zm0 2v16h10V6.8L14.2 4H13v3h-1.5V4H10v3H8.5V4z" }, "c"),
    jsx("path", { d: "M8.5 13h4.3l-1.6-1.6 1-1 3.3 3.3-3.3 3.3-1-1 1.6-1.6H8.5z" }, "a"),
]);
const LightsIcon = svg([
    jsx("circle", { cx: 12, cy: 12, r: 4 }, "c"),
    jsx("path", { d: "M12 2a10 10 0 1 1 0 20 10 10 0 0 1 0-20zm0 2a8 8 0 1 0 0 16 8 8 0 0 0 0-16z", opacity: 0.6 }, "r"),
]);

// LB and RB: the controller's bumpers, in Steam's button numbering.
const GB = DFL.GamepadButton || {};
const BUMPER_LEFT = GB.BUMPER_LEFT != null ? GB.BUMPER_LEFT : 5;
const BUMPER_RIGHT = GB.BUMPER_RIGHT != null ? GB.BUMPER_RIGHT : 6;

// The tab shown, kept while Steam runs.
let lastTab = "update";

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

// Sliders send while they move, at most this often, and the last value always.
const SEND_MS = 120;

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

// --------------------------------------------------------------- update ---
function UpdateTab() {
    const [st, setSt] = useState(null);
    const [checking, setChecking] = useState(false);
    const refresh = useCallback(() => {
        getState().then(setSt).catch(() => {});
    }, []);
    const doCheck = useCallback(() => {
        setChecking(true);
        check().catch(() => {}).finally(() => { setChecking(false); refresh(); });
    }, [refresh]);
    useEffect(() => {
        getState().then((s) => {
            setSt(s);
            // Drives are looked at when one is put in; look again when the menu opens.
            if (!s.running && !s.pending) checkLocal().then(refresh).catch(() => {});
            // Look again when the last look is over 10 minutes old.
            if (!s.running && !s.pending && (!s.release || !s.release.time || Date.now() / 1000 - s.release.time > 600)) doCheck();
        }).catch(() => {});
        const t = setInterval(refresh, 1000);
        return () => clearInterval(t);
    }, [refresh, doCheck]);

    if (!st) {
        return jsx(DFL.PanelSection, { children: row("Loading…") });
    }
    const rel = st.release || {};
    const local = st.local || {};
    const localUp = local.update;
    const up = rel.update;
    const job = st.job || {};
    const items = [note(`Installed: ${st.installed}`)];

    if (st.pending) {
        items.push(
            row(jsx("div", { children: "The update is ready. It installs while the device restarts." })),
            row(jsx(DFL.ButtonItem, {
                layout: "below",
                onClick: () => DFL.showModal(jsx(DFL.ConfirmModal, {
                    strTitle: "Restart and install?",
                    strDescription: "Keep the device charging and leave it on. The screen can stay dark for several minutes "
                        + "while the update installs. Games, saves and settings are kept.",
                    strOKButtonText: "Restart",
                    onOK: () => restart(),
                })),
                children: "Restart and install",
            })),
        );
    } else if (st.running) {
        const downloading = (job.step || "").startsWith("Downloading");
        const copying = (job.step || "").startsWith("Copying");
        items.push(row(jsx("div", { children: (job.step || "Starting") + "…" })));
        if (job.total) {
            const pct = Math.min(100, (100 * job.have) / job.total);
            items.push(bar(downloading || copying ? `${gb(job.have)} of ${gb(job.total)} · ${Math.floor(pct)}%` : `${Math.floor(pct)}%`, pct));
        }
        if (downloading) {
            items.push(row(jsx(DFL.ButtonItem, {
                layout: "below",
                onClick: () => pause().then(refresh),
                children: "Pause download",
            })));
            items.push(note("A paused or interrupted download continues where it stopped."));
        } else if (copying) {
            items.push(note("Leave the drive in until the copy is done. You can close this menu."));
        } else {
            if (job.drive_done) items.push(note("Copied. You can take out the drive."));
            items.push(note("Getting the update ready. Games keep running; you can close this menu."));
        }
    } else {
        if (job.error) items.push(failure(job.error));
        if (localUp) {
            items.push(
                row(jsx(DFL.Field, { label: `pb-os ${localUp.version} on the ${localUp.where}`, children: null,
                    description: localUp.kind === "delta"
                        ? `${gb(localUp.size)} (only what changed)`
                        : `${gb(localUp.size)}, about 25 GB free space needed` })),
                row(jsx(DFL.ButtonItem, {
                    layout: "below",
                    onClick: () => startLocal().then(refresh),
                    children: "Install from the drive",
                })),
            );
        }
        for (const problem of local.problems || []) {
            items.push(warn(problem));
        }
        if (checking) {
            items.push(note("Looking for updates…"));
        } else if (rel.ok === false) {
            items.push(note(`Could not look for updates: ${rel.error}`));
        } else if (rel.available && up) {
            const resume = job.dl_total && job.dl_have && job.dl_have < job.dl_total;
            items.push(
                row(jsx(DFL.Field, { label: up.title, children: null, description: up.kind === "delta"
                    ? `${gb(up.size)} download (only what changed)`
                    : `${gb(up.size)} download, about 25 GB free space needed` })),
                row(jsx(DFL.ButtonItem, {
                    layout: "below",
                    onClick: () => start().then(refresh),
                    children: resume ? "Resume download" : "Download and install",
                })),
            );
        } else if (rel.ok && !up) {
            items.push(note("No update for this device in the pb-os releases yet."));
        } else if (rel.ok) {
            items.push(note("pb-os is up to date."));
        }
        items.push(row(jsx(DFL.ButtonItem, {
            layout: "below",
            disabled: checking,
            onClick: () => { checkLocal().then(refresh).catch(() => {}); doCheck(); },
            children: "Check for updates",
        })));
        items.push(note("No internet? Put the update's files, SHA256SUMS and SHA256SUMS.sig from the release "
            + "in the top folder of a microSD card or USB drive and put it in."));
    }
    return jsx(DFL.PanelSection, { title: "pb-os update", children: items });
}

// -------------------------------------------------------------- install ---
const HOME_CHOICES = [
    { data: "all", label: "Everything", desc: "Games, saves, Steam login and settings" },
    { data: "essentials", label: "No games", desc: "Saves, Steam login and settings; games download again" },
    { data: "none", label: "Fresh start", desc: "No Steam login, saves or games" },
];

// The result of the last move, dismissed per run (its start time).
let dismissed = 0;

function MoveProgress({ st, onBack }) {
    const job = st.job || {};
    const args = st.args || {};
    const toSd = args.direction === "sd";
    const title = toSd ? "Copy to microSD" : "Install to internal storage";
    const [, tick] = useState(0);
    useEffect(() => {
        const t = setInterval(() => tick((n) => n + 1), 1000);
        return () => clearInterval(t);
    }, []);
    const secs = Math.max(0, Math.floor(Date.now() / 1000 - (args.started || Date.now() / 1000)));
    const elapsed = `${Math.floor(secs / 60)}:${String(secs % 60).padStart(2, "0")}`;
    const count = job.count || 1;

    if (st.running) {
        const frac = (Math.max(job.step || 1, 1) - 1 + (job.pct || 0) / 100) / count;
        return jsxs(DFL.PanelSection, { title, children: [
            row(jsx("div", { children: `Step ${Math.max(job.step || 1, 1)} of ${count}: ${job.label || "Starting"}` })),
            bar(`${Math.floor(frac * 100)}% · ${elapsed}`, frac * 100),
            (job.lines || []).length ? note(job.lines[job.lines.length - 1]) : null,
            note(toSd
                ? "Keep the device charging and the card in. Sleep is off until it's done. You can close this menu."
                : "Keep the device charging and don't power it off. Sleep is off until it's done. You can close this menu."),
        ] });
    }
    const ok = job.exit === 0;
    const items = [];
    if (ok) {
        items.push(row(jsx("div", { style: { fontWeight: "bold" }, children: toSd ? "SteamOS is on the microSD card." : "SteamOS is on internal storage." })));
        items.push(toSd
            ? note("Power off and keep the card in. Hold Volume Down at power-on for the ABL menu, set Boot source to SD, "
                + "then boot Linux. Boot source Internal still starts the internal install, which is unchanged.")
            : note("Power off and take out the microSD card. Hold Volume Down at power-on for the ABL menu, set Boot source "
                + "to Internal, then boot Linux. Android is in the same menu; it sets itself up again."));
        items.push(row(jsx(DFL.ButtonItem, {
            layout: "below",
            onClick: () => DFL.showModal(jsx(DFL.ConfirmModal, {
                strTitle: "Power off?",
                strDescription: toSd ? "Keep the card in." : "Then take out the microSD card.",
                strOKButtonText: "Power off",
                onOK: () => powerOff(),
            })),
            children: "Power off",
        })));
    } else {
        items.push(row(jsx("div", { style: { fontWeight: "bold" }, children: toSd ? "The copy failed." : "The install failed." })));
        items.push(failure(job.error || (job.exit == null ? "It stopped unexpectedly." : `It ended with code ${job.exit}.`)));
        if (toSd) {
            items.push(note(job.step >= 3
                ? "The card was already erased and is not usable as it is. Internal storage was not changed."
                : "Nothing was written: the card and internal storage are as they were."));
        } else {
            items.push(note(job.step >= 2
                ? "Internal storage was already being changed, so Android may not boot until this is fixed. The ABL "
                    + "menu's UNINSTALL CFW removes the Linux partitions; the old partition table is on the card in /boot/ufs-backup."
                : "The installer stopped before changing internal storage."));
        }
    }
    if (args.saved_log) items.push(note(`The log is saved as ${args.saved_log}.`));
    items.push(row(jsx(DFL.ButtonItem, { layout: "below", onClick: onBack, children: "Back" })));
    return jsx(DFL.PanelSection, { title, children: items });
}

function HomeChoice({ value, onChange, sizes, room }) {
    const need = (id) => (sizes ? sizes[id] : null);
    const fits = (id) => need(id) == null || room == null || need(id) < room * 0.95;
    const choice = HOME_CHOICES.find((c) => c.data === value) || HOME_CHOICES[0];
    return [
        row(jsx(DFL.DropdownItem, {
            label: "Bring along",
            description: choice.desc + (need(value) != null && value !== "none" ? ` (${gb(need(value))})` : ""),
            rgOptions: HOME_CHOICES.map((c) => ({ data: c.data, label: c.label })),
            selectedOption: value,
            onChange: (o) => onChange(o.data),
        })),
        !sizes ? note("Measuring the home folder…") : null,
        !fits(value) ? warn(`That doesn't fit: ${gb(need(value))} to copy, ${gb(room)} of room. Pick "No games".`) : null,
    ];
}

function InstallTab() {
    const [st, setSt] = useState(null);
    const [home, setHome] = useState("all");
    const [android, setAndroid] = useState(null);
    const [ack, setAck] = useState(false);
    const [starting, setStarting] = useState(false);
    const [error, setError] = useState("");
    const [, redraw] = useState(0);
    const refresh = useCallback((force) => {
        getInstall(!!force).then(setSt).catch(() => {});
    }, []);
    useEffect(() => {
        refresh(true);
        const t = setInterval(() => refresh(false), 1500);
        return () => clearInterval(t);
    }, [refresh]);

    if (!st) return jsx(DFL.PanelSection, { children: note("Checking…") });
    const args = st.args || {};
    const job = st.job || {};
    const showResult = st.running || (args.started && args.started !== dismissed && (job.exit != null || job.step > 0));
    if (showResult) {
        return jsx(MoveProgress, { st, onBack: () => { dismissed = args.started; redraw((n) => n + 1); refresh(true); } });
    }
    const again = row(jsx(DFL.ButtonItem, { layout: "below", onClick: () => { setError(""); refresh(true); }, children: "Check again" }));
    const go = (direction, androidGb, expect, text, okText) => DFL.showModal(jsx(DFL.ConfirmModal, {
        strTitle: "Are you sure?",
        strDescription: text,
        strOKButtonText: okText,
        onOK: () => {
            setStarting(true);
            setError("");
            startMove(direction, home, androidGb, expect)
                .then((r) => { if (!r.ok) setError(r.error); setAck(false); })
                .catch((e) => setError(String(e)))
                .finally(() => { setStarting(false); refresh(false); });
        },
    }));
    const items = [];
    if (error) items.push(failure(error));
    if (st.updating) {
        items.push(note("An update is being prepared. Moving SteamOS waits until it's ready."));
        return jsx(DFL.PanelSection, { title: "Move SteamOS", children: items });
    }

    if (st.from === "sd") {
        const p = st.internal || {};
        items.push(note("SteamOS is running from the microSD card."));
        if (p.ERROR) {
            items.push(failure(p.ERROR), again);
        } else if (p.MODE === "installed") {
            items.push(note("SteamOS is already on internal storage. Updates there keep games and saves; reinstalling "
                + "would erase them, so it isn't offered here."));
        } else if (p.MODE === "occupied") {
            items.push(warn(`Other partitions follow Android's (${p.OCCUPIED}). Remove them first with UNINSTALL CFW in the ABL menu.`), again);
        } else if (p.MODE === "toosmall") {
            items.push(failure("Internal storage has too little room after Android for SteamOS."));
        } else if (p.MODE === "fresh") {
            const mn = Number(p.MIN_ANDROID_GIB), mx = Number(p.MAX_ANDROID_GIB);
            const a = Math.min(mx, Math.max(mn, android != null ? android : Number(p.RECOMMENDED_ANDROID_GIB)));
            const total = Number(p.ORIG_ANDROID_GIB), boot = Number(p.BOOT_PART_GIB || 2), root = Number(p.ROOT_PART_GIB || 20);
            const room = (total - a - boot - root) * GIB;
            const needed = st.sizes ? st.sizes[home] : null;
            const fits = needed == null || needed < room * 0.95;
            items.push(
                row(jsx(DFL.Field, { label: "Internal storage", children: null,
                    description: `${p.DISK_TOTAL_GIB} GB · Android has ${total} GB now` })),
                row(jsx(DFL.SliderField, {
                    label: `Android keeps ${a} GB`,
                    description: `SteamOS gets the rest: about ${Math.max(0, total - a)} GB (${root} GB system, ~${Math.max(0, total - a - boot - root)} GB for games and saves)`,
                    value: a, min: mn, max: mx, step: 4,
                    onChange: (v) => setAndroid(v),
                })),
                ...HomeChoice({ value: home, onChange: setHome, sizes: st.sizes, room }),
                row(jsx(DFL.ToggleField, {
                    label: "Erase Android's data",
                    description: "Android stays, but its apps, photos and accounts are erased. Its partition table is saved to this card first.",
                    checked: ack,
                    onChange: setAck,
                })),
                row(jsx(DFL.ButtonItem, {
                    layout: "below",
                    disabled: !ack || !fits || starting,
                    onClick: () => go("internal", a, p.TABLE_FINGERPRINT || "",
                        `Android's data is erased and SteamOS is copied to internal storage (Android keeps ${a} GB). `
                        + "It takes a while; keep the device charging.", "Erase and install"),
                    children: "Install to internal storage",
                })),
            );
        } else {
            items.push(failure(`Unexpected internal storage state: ${p.MODE || "unknown"}`), again);
        }
    } else {
        const c = st.card || {};
        items.push(note("SteamOS is running from internal storage. Copy it to a microSD card to boot from the card, "
            + "as a spare or to give the internal space back later. Internal storage is not changed."));
        if (c.ERROR && !c.CARD) {
            items.push(failure(c.ERROR), again);
        } else {
            const room = Number(c.HOME_PART_BYTES || 0);
            const needed = st.sizes ? st.sizes[home] : null;
            const fits = !c.ERROR && (needed == null || needed < room * 0.95);
            items.push(
                row(jsx(DFL.Field, { label: "microSD card", children: null,
                    description: `${c.CARD_NAME || c.CARD} · ${gb(Number(c.CARD_BYTES || 0))}` })),
                c.CARD_PARTS ? warn(`Erased: ${c.CARD_PARTS.split(",").join(", ")}`) : note("The card is empty."),
                c.CARD_STEAM_LIBRARY === "1" ? warn("The card holds a Steam library. Its games are erased with it.") : null,
                c.ERROR ? failure(c.ERROR) : null,
                ...HomeChoice({ value: home, onChange: setHome, sizes: st.sizes, room }),
                row(jsx(DFL.ToggleField, {
                    label: "Erase the card",
                    description: "Everything on the card is erased.",
                    checked: ack,
                    onChange: setAck,
                })),
                row(jsx(DFL.ButtonItem, {
                    layout: "below",
                    disabled: !ack || !fits || starting,
                    onClick: () => go("sd", 0, c.CARD_CID || "",
                        `Everything on the ${gb(Number(c.CARD_BYTES || 0))} card is erased and SteamOS is copied to it. `
                        + "It takes a while; keep the device charging and the card in.", "Erase and copy"),
                    children: "Copy SteamOS to the card",
                })),
            );
        }
    }
    return jsx(DFL.PanelSection, { title: "Move SteamOS", children: items });
}

// --------------------------------------------------------------- lights ---
const PRESETS = [
    { label: "Ember", mode: "static", color: "ff3c00" },
    { label: "Ice", mode: "static", color: "00b4ff" },
    { label: "Violet", mode: "static", color: "a000ff" },
    { label: "Green", mode: "static", color: "00ff40" },
    { label: "White", mode: "static", color: "ffffff" },
    { label: "Breathe", mode: "breath", color: "ff0040", pbosd: true },
    { label: "Off", mode: "off", color: "000000" },
];

function LightsTab() {
    const [st, setSt] = useState(null);
    const movedAt = useRef(0);
    const refresh = useCallback(() => {
        if (Date.now() - movedAt.current < 2000) return;
        getLights().then((s) => { if (Date.now() - movedAt.current >= 2000) setSt(s); }).catch(() => {});
    }, []);
    const send = useThrottled(useCallback((s) => { setLights(s.mode, s.color, s.brightness).catch(() => {}); }, []));
    useEffect(() => {
        refresh();
        const t = setInterval(refresh, 2000);
        return () => clearInterval(t);
    }, [refresh]);

    if (!st) return jsx(DFL.PanelSection, { children: note("Loading…") });
    if (!st.kind) return jsx(DFL.PanelSection, { children: note("This device has no stick lights pb-os can set.") });
    const presets = PRESETS.filter((p) => !p.pbosd || st.kind === "pbosd");
    const current = Math.max(0, presets.findIndex((p) => p.mode === st.mode && (p.mode === "off" || p.color === st.color)));
    const usable = st.available !== false;
    return jsxs(DFL.PanelSection, { title: "Stick lights", children: [
        row(jsx(DFL.DropdownItem, {
            label: "Lighting",
            disabled: !usable,
            rgOptions: presets.map((p, i) => ({ data: i, label: p.label })),
            selectedOption: current,
            onChange: (o) => {
                const p = presets[o.data];
                setLights(p.mode, p.color, st.brightness).then(setSt).catch(() => {});
            },
        })),
        row(jsx(DFL.SliderField, {
            label: "Brightness",
            value: st.brightness, min: 10, max: 255, step: 5,
            disabled: !usable || st.mode !== "static",
            onChange: (v) => {
                movedAt.current = Date.now();
                const next = { ...st, brightness: v };
                setSt(next);
                send(next);
            },
        })),
        st.kind === "pbosd" ? row(jsx(DFL.ToggleField, {
            label: "Power LED",
            description: "Charging / full / low-battery colours and profile flashes",
            checked: st.power_led !== false,
            onChange: (v) => setPowerLed(v).then(setSt).catch(() => {}),
        })) : null,
        st.kind === "pbosd" && !usable ? note("Stick lighting needs the controller MCU link: PB-OS Control → Hardware.") : null,
        st.kind === "pbosd" && st.daemon === false ? warn("pbosd is not running.") : null,
        st.kind === "pbosd" ? note("The K button can cycle these too (PB-OS Control → Buttons).") : null,
    ] });
}

// ---------------------------------------------------------------- panel ---
function Content() {
    const [tab, setTab] = useState(lastTab);
    const [lights, setLightsKind] = useState(null);
    const pick = (id) => { lastTab = id; setTab(id); };
    useEffect(() => {
        getLights().then((s) => setLightsKind(s.kind || "")).catch(() => setLightsKind(""));
    }, []);
    const tabs = [
        { id: "update", icon: UpdateIcon },
        { id: "install", icon: InstallIcon },
        ...(lights ? [{ id: "lights", icon: LightsIcon }] : []),
    ];
    const shown = tabs.some((t) => t.id === tab) ? tab : "update";
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
        children: [
            jsx(TabBar, { tabs, active: shown, onPick: pick }),
            shown === "update" && jsx(UpdateTab, {}),
            shown === "install" && jsx(InstallTab, {}),
            shown === "lights" && jsx(LightsTab, {}),
        ],
    });
}

function onAvailable(title) {
    toaster.toast({ title: "pb-os update", body: `${title} is available. Open PB-OS Utils in Quick Access.`, duration: 6000 });
}

function onLocal(version, where) {
    toaster.toast({ title: "pb-os update", body: `pb-os ${version} is on the ${where}. Open PB-OS Utils in Quick Access to install it.`, duration: 6000 });
}

function onMoveDone(direction, ok) {
    const what = direction === "sd" ? "Copy to microSD" : "Install to internal storage";
    toaster.toast({ title: "PB-OS Utils", body: ok ? `${what} finished. Open PB-OS Utils for the next steps.` : `${what} failed. Open PB-OS Utils for details.`, duration: 8000 });
}

var index = definePlugin(() => {
    api.addEventListener("pbos_update_available", onAvailable);
    api.addEventListener("pbos_local_available", onLocal);
    api.addEventListener("pbos_move_done", onMoveDone);
    return {
        name: "PB-OS Utils",
        content: jsx(Content, {}),
        // A wrench.
        icon: jsx("svg", { viewBox: "0 0 24 24", width: "1em", height: "1em", fill: "currentColor",
            children: jsx("path", { d: "M21.7 6.3a6 6 0 0 1-7.9 7.2l-7.4 7.4a2.1 2.1 0 0 1-3-3l7.4-7.4a6 6 0 0 1 7.2-7.9l-3.6 3.6.6 2.9 2.9.6z" }) }),
        alwaysRender: false,
        onDismount() {
            api.removeEventListener("pbos_update_available", onAvailable);
            api.removeEventListener("pbos_local_available", onLocal);
            api.removeEventListener("pbos_move_done", onMoveDone);
        },
    };
});

export { index as default };

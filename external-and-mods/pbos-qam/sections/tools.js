// PB-OS Tools in PB-OS's Quick Access tab (was the PB-OS Tools Decky
// plugin, pbos-utils): each former tab is a plugin in the PB-OS list, its
// component unchanged. pbos-qamd's tools section is the backend.
// The loader (src/loader.js) gives P: @decky/ui as DFL, Steam's React and
// JSX, callable(), toaster, events and plugin().
window.__pbosQam.section("tools", (P) => {
const { DFL, SP_REACT, SP_JSX, callable, toaster } = P;
const api = P;
const { useEffect, useState, useCallback, useRef } = SP_REACT;
const jsx = SP_JSX.jsx;
const jsxs = SP_JSX.jsxs;

// pbos-qamd's tools section (daemon/tools.cpp): arguments are passed
// positionally, as Decky's callable() passed them to the plugin's Python.
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
const setAudioPulse = callable("set_audio_pulse");
const setChannel = callable("set_channel");
// Performance and Hardware (KONKR Pocket FIT / AYANEO Pocket S2, via pbosd).
const getControl = callable("get_control");
const setProfile = callable("set_profile");
const setFan = callable("set_fan");
const setButton = callable("set_button");
const setButtonsMode = callable("set_buttons_mode");
const setMcu = callable("set_mcu");
// Face buttons by printed letter or by position (Retroid Pocket 6 / Nova, AYN Thor).
const getFaceButtons = callable("get_face_buttons");
const setFaceButtons = callable("set_face_buttons");
const getMtp = callable("get_mtp");
const setMtp = callable("set_mtp");
// Barry Launcher in Desktop Mode (AYN Thor).
const getBarryDesktop = callable("get_barry_desktop");
const setBarryDesktop = callable("set_barry_desktop");
// Sticks (Retroid Pocket 6 / Nova, AYN Thor; was the Stick Calibration plugin).
const stickState = callable("stick_state");
const stickRest = callable("stick_rest");
const stickEdges = callable("stick_edges");
const stickLive = callable("stick_live_values");
const stickCancel = callable("stick_cancel");
const stickSave = callable("stick_save");
const stickSwitch = callable("stick_switch");
const stickDelete = callable("stick_delete");
const getCharging = callable("get_charging");
const setCharging = callable("set_charging");
const previewCharging = callable("preview_charging");
// Charge limit, bypass and slow charging (Retroid Pocket 6 / Nova, AYN Thor, KONKR Pocket FIT; sm8550-charge-control).
const getChargeControl = callable("get_charge_control");
const setChargeControl = callable("set_charge_control");
// PB-OS twins of Valve's Proton: set per game (pbos-proton builds them).
const getProton = callable("get_proton");
const getProtonGames = callable("get_proton_games");
const setProtonGames = callable("set_proton_games");
// Per-game profiles, applied at launch by /usr/bin/pbos-game-launch.
const getGameProfiles = callable("get_game_profiles");
const setGameProfile = callable("set_game_profile");
// The launch fix ProtonDB's working reports for one game agree on.
const getCommunityHint = callable("get_community_hint");
// Performance modes: the default, and which game runs (its mode wins).
const getPerformance = callable("get_performance");
const setPerformanceDefault = callable("set_performance_default");
const gameRunning = callable("game_running");

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
// A classic 5 mm LED from an electronics kit: domed lens with a highlight,
// the rim at its base, and two legs (the longer one the anode).
const LightsIcon = svg([
    jsx("path", { fillRule: "evenodd", d: "M7.5 9.5a4.5 4.5 0 0 1 9 0V15h-9zM9.4 9.6a2.6 2.6 0 0 1 2.6-2.6v1.3a1.3 1.3 0 0 0-1.3 1.3z" }, "lens"),
    jsx("rect", { x: 6.3, y: 15, width: 11.4, height: 1.9, rx: 0.4 }, "rim"),
    jsx("rect", { x: 9.2, y: 16.9, width: 1.5, height: 6.1, rx: 0.3 }, "anode"),
    jsx("rect", { x: 13.3, y: 16.9, width: 1.5, height: 4.6, rx: 0.3 }, "cathode"),
]);

// A car's tachometer: the dial open at the bottom, its ticks, and the needle
// swung up towards the red line.
const PerformanceIcon = svg([
    jsx("path", { d: "M5.21 18.79A9.6 9.6 0 1 1 18.79 18.79", fill: "none", stroke: "currentColor", strokeWidth: 1.9, strokeLinecap: "round" }, "dial"),
    jsx("path", { d: "M7.83 16.17L6.63 17.37M6.1 12L4.4 12M7.83 7.83L6.63 6.63M12 6.1L12 4.4M16.17 7.83L17.37 6.63M17.9 12L19.6 12M16.17 16.17L17.37 17.37",
        fill: "none", stroke: "currentColor", strokeWidth: 1.5, strokeLinecap: "round" }, "ticks"),
    jsx("path", { d: "M12.74 12.82L17.35 7.18L11.26 11.18Z" }, "needle"),
    jsx("circle", { cx: 12, cy: 12, r: 1.8 }, "hub"),
]);
// A hexagonal nut seen from above: the hex, its chamfer ring and the hole.
const HardwareIcon = svg([
    jsx("path", { fillRule: "evenodd", d: "M22.2 12L17.1 20.83L6.9 20.83L1.8 12L6.9 3.17L17.1 3.17ZM12 7.6a4.4 4.4 0 1 0 0 8.8a4.4 4.4 0 1 0 0-8.8Z" }, "nut"),
    jsx("circle", { cx: 12, cy: 12, r: 6.4, fill: "none", stroke: "#000", strokeOpacity: 0.35, strokeWidth: 0.9 }, "chamfer"),
]);

// A gamepad: per-game profiles. The d-pad and two face buttons are cut out.

// A lightning bolt: power and charging.
const PowerIcon = svg(jsx("path", { d: "M13.5 2 4.5 13.5h6L9.5 22l9-11.5h-6z" }));

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
// Dev updates are for testers: the switch shows only on a device a tester
// unlocked with their passphrase. Tapping the Installed line this many times
// (each within TAP_GAP_MS of the last) asks for it.
const UNLOCK_TAPS = 7;
const TAP_GAP_MS = 3000;

function TesterUnlock({ closeModal, onDone }) {
    const [pass, setPass] = useState("");
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState("");
    const submit = () => {
        if (busy || !pass) return;
        setBusy(true);
        setError("");
        setChannel("dev", pass).then((r) => {
            if (r && r.ok) { onDone(); closeModal(); return; }
            setError((r && r.error === "not a tester passphrase") ? "Not a tester passphrase." : `Could not turn on dev updates: ${(r && r.error) || "unknown error"}`);
            setPass("");
        }).catch(() => setError("Could not turn on dev updates.")).finally(() => setBusy(false));
    };
    return jsx(DFL.ModalRoot, {
        closeModal, onCancel: closeModal,
        children: jsxs("div", { children: [
            jsx("div", { style: { fontSize: "20px", fontWeight: "bold", marginBottom: "12px" }, children: "Tester unlock" }),
            jsx(DFL.TextField, {
                label: "Passphrase", bIsPassword: true, value: pass, disabled: busy,
                onChange: (e) => setPass(e.target.value),
            }),
            error ? jsx("div", { style: { color: "#ff6b6b", marginTop: "8px" }, children: error }) : null,
            busy ? jsx("div", { style: { opacity: 0.75, marginTop: "8px" }, children: "Checking…" }) : null,
            jsxs(DFL.Focusable, {
                "flow-children": "horizontal",
                style: { display: "flex", gap: "8px", marginTop: "16px", justifyContent: "flex-end" },
                children: [
                    jsx(DFL.DialogButton, { style: { width: "auto", minWidth: "120px" }, onClick: closeModal, children: "Cancel" }),
                    jsx(DFL.DialogButton, { style: { width: "auto", minWidth: "120px" }, disabled: busy || !pass, onClick: submit, children: "Unlock" }),
                ],
            }),
        ] }),
    });
}

function UpdateTab() {
    const [st, setSt] = useState(null);
    const [checking, setChecking] = useState(false);
    const taps = useRef({ n: 0, at: 0 });
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
    const dev = rel.channel === "dev";
    const tap = () => {
        if (dev) return;
        const t = taps.current, now = Date.now();
        t.n = now - t.at > TAP_GAP_MS ? 1 : t.n + 1;
        t.at = now;
        if (t.n < UNLOCK_TAPS) return;
        t.n = 0;
        DFL.showModal(jsx(TesterUnlock, { onDone: refresh }));
    };
    const items = [row(jsx(DFL.Focusable, {
        onActivate: tap, onClick: tap,
        style: { fontSize: "12px", opacity: 0.75 },
        children: `Installed: ${st.installed}${dev ? ` · Dev updates on${rel.tester ? ` (${rel.tester})` : ""}` : ""}`,
    }))];

    // Staging writes the pending marker before the boot image, so the update
    // is only ready once the job has finished.
    if (st.pending && !st.running) {
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
        } else if (copying && (job.step || "").endsWith("Downloads folder")) {
            items.push(note("You can close this menu. The files leave the Downloads folder once the update is ready."));
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
                row(jsx(DFL.Field, { label: `pb-os ${localUp.version} ${localUp.place || "on the " + localUp.where}`, children: null,
                    description: localUp.kind === "delta"
                        ? `${gb(localUp.size)} (only what changed)`
                        : `${gb(localUp.size)}, about 25 GB free space needed` })),
                row(jsx(DFL.ButtonItem, {
                    layout: "below",
                    onClick: () => startLocal().then(refresh),
                    children: localUp.drive === "downloads" ? "Install from Downloads" : "Install from the drive",
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
                row(jsx(DFL.Field, { label: up.title, children: null, description: (up.kind === "delta"
                    ? `${gb(up.size)} download (only what changed)`
                    : `${gb(up.size)} download, about 25 GB free space needed`)
                    + (up.repo === "project-barry/pb-os-dev" ? ". Test build (dev updates)." : "") })),
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
        items.push(note("No internet here? Put the update's files, SHA256SUMS and SHA256SUMS.sig from the release "
            + "in the top folder of a microSD card or USB drive and put it in. Or download them in Desktop Mode "
            + "to the Downloads folder; they are deleted from there once the update is ready."));
        // For testers: also offer test builds (pb-os-dev). Shown only once a
        // tester unlocked this device (UNLOCK_TAPS); off hides it again.
        if (dev) items.push(row(jsx(DFL.ToggleField, {
            label: "Dev updates",
            description: "Also offer test builds before they are released. Turning this off hides it until a tester unlocks it again.",
            checked: true,
            disabled: checking,
            onChange: (on) => {
                if (on) return;
                setChecking(true);
                setChannel("prod").catch(() => {}).finally(() => { setChecking(false); refresh(); doCheck(); });
            },
        })));
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
            ? note("Power off and keep the card in. Hold Volume Down at power-on for the OpenABL menu, set Boot source to SD card, "
                + "then Start. Boot source Auto or Internal still starts the internal install, which is unchanged.")
            : note("Power off and take out the microSD card. Hold Volume Down at power-on for the OpenABL menu, set Boot source "
                + "to Internal (or Auto), Boot mode to Linux, then Start. Android: hold Volume Up at power-on; it sets itself up again. "
                + "Never pick Uninstall CFW there: it erases this install and Android's user data."));
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
                ? "Internal storage was already being changed, so Android may not boot until this is fixed. The OpenABL "
                    + "menu's Uninstall CFW removes the Linux partitions (and Android's user data); the old partition table is on the card in /boot/ufs-backup."
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

// The Install tab's picks. A dropdown's list remounts the panel, which put
// "Bring along" back on Everything right after picking No games (and the
// Android size back on the recommended one).
const installPicks = { home: "all", android: null };

function InstallTab() {
    const [st, setSt] = useState(null);
    const [home, setHomeState] = useState(installPicks.home);
    const [android, setAndroidState] = useState(installPicks.android);
    const setHome = (v) => { installPicks.home = v; setHomeState(v); };
    const setAndroid = (v) => { installPicks.android = v; setAndroidState(v); };
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
            items.push(warn(`Other partitions follow Android's (${p.OCCUPIED}). Remove them first with Uninstall CFW in the OpenABL menu (it also erases Android's user data).`), again);
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
const hex2 = (n) => Math.max(0, Math.min(255, Math.round(n))).toString(16).padStart(2, "0");
const toRgb = (color) => [0, 2, 4].map((i) => parseInt(color.slice(i, i + 2), 16) || 0);
const toColor = (rgb) => rgb.map(hex2).join("");
// Full-colour hue (0-359) to RRGGBB, and back (grey has no hue: 0).
function hueColor(h) {
    const f = (n) => { const k = (n + h / 60) % 6; return 255 * (1 - Math.max(0, Math.min(k, 4 - k, 1))); };
    return toColor([f(5), f(3), f(1)]);
}
function colorHue(color) {
    const [r, g, b] = toRgb(color).map((c) => c / 255);
    const max = Math.max(r, g, b), d = max - Math.min(r, g, b);
    if (!d) return 0;
    const h = max === r ? ((g - b) / d) % 6 : max === g ? (b - r) / d + 2 : (r - g) / d + 4;
    return Math.round((h * 60 + 360) % 360);
}
const RAINBOW = "linear-gradient(to right, #f00, #ff0, #0f0, #0ff, #00f, #f0f, #f00)";
// The Advanced and Experimental sections stay as they were left while Steam
// runs; closed at first.
let advancedOpen = false;
let experimentalOpen = false;

function LightsTab() {
    const [st, setSt] = useState(null);
    const [advanced, setAdvanced] = useState(advancedOpen);
    const [experimental, setExperimental] = useState(experimentalOpen);
    const movedAt = useRef(0);
    const refresh = useCallback(() => {
        if (Date.now() - movedAt.current < 2000) return;
        getLights().then((s) => { if (Date.now() - movedAt.current >= 2000) setSt(s); }).catch(() => {});
    }, []);
    const send = useThrottled(useCallback((s) => {
        setLights(s.on, s.effect, s.color, s.brightness, s.speed, !!s.reverse_left, !!s.reverse_right, s.color2 || "").catch(() => {});
    }, []));
    useEffect(() => {
        refresh();
        const t = setInterval(refresh, 2000);
        return () => clearInterval(t);
    }, [refresh]);

    if (!st) return jsx(DFL.PanelSection, { children: note("Loading…") });
    if (!st.kind) return jsx(DFL.PanelSection, { children: note("This device has no stick lights pb-os can set.") });
    // Every change goes out whole (on, effect, colour, brightness, speed).
    const change = (patch) => {
        movedAt.current = Date.now();
        const next = { ...st, ...patch };
        setSt(next);
        send(next);
    };
    const effects = st.effects || [];
    const effect = effects.find((e) => e.id === st.effect) || effects[0] || { id: "static", color: true };
    const usable = st.available !== false;
    const kpf = st.kind === "pbosd";
    const rgb = toRgb(st.color);
    const swatch = jsx("span", { style: { display: "inline-block", width: "14px", height: "14px", borderRadius: "7px",
        marginLeft: "8px", verticalAlign: "middle", border: "1px solid rgba(255,255,255,0.5)", background: `#${st.color}` } });
    const items = [
        row(jsx(DFL.ToggleField, {
            label: "Stick Lights",
            disabled: !usable,
            checked: !!st.on,
            onChange: (v) => change({ on: v }),
        })),
    ];
    if (st.on) {
        items.push(row(jsx(DFL.DropdownItem, {
            label: "Effect",
            disabled: !usable,
            rgOptions: effects.map((e) => ({ data: e.id, label: e.experimental ? `${e.label} (Experimental)` : e.label })),
            selectedOption: effect.id,
            onChange: (o) => change({ effect: o.data }),
        })));
        if (effect.color) {
            items.push(row(jsx(DFL.SliderField, {
                label: jsxs("span", { children: ["Color", swatch] }),
                description: jsx("div", { style: { height: "8px", borderRadius: "4px", background: RAINBOW } }),
                value: colorHue(st.color), min: 0, max: 359, step: 3,
                disabled: !usable,
                onChange: (h) => change({ color: hueColor(h) }),
            })));
        }
        const audioFx = effect.id === "audio" || effect.id === "spectrum";
        if (audioFx) {
            // The audio effects' second color: what a hit turns a zone towards.
            const c2 = st.color2 || "ffffff";
            const swatch2 = jsx("span", { style: { display: "inline-block", width: "14px", height: "14px", borderRadius: "7px",
                marginLeft: "8px", verticalAlign: "middle", border: "1px solid rgba(255,255,255,0.5)", background: `#${c2}` } });
            items.push(row(jsx(DFL.SliderField, {
                label: jsxs("span", { children: ["Intensity Color", swatch2] }),
                description: jsx("div", { style: { height: "8px", borderRadius: "4px", background: RAINBOW } }),
                value: colorHue(c2), min: 0, max: 359, step: 3,
                disabled: !usable,
                onChange: (h) => change({ color2: hueColor(h) }),
            })));
            items.push(note(effect.id === "audio"
                ? "The meter's top LED, and every lit LED on a beat, show the Intensity Color; Brightness is a full meter."
                : "Hits on the beat turn a zone towards the Intensity Color and up to full Brightness; "
                    + "between hits the Color glows dimmer, with room above it."));
        }
        items.push(row(jsx(DFL.SliderField, {
            label: "Brightness",
            value: st.brightness, min: 10, max: 255, step: 5,
            disabled: !usable || (kpf && effect.id === "rainbow"),
            onChange: (v) => change({ brightness: v }),
        })));
        if (effect.speed) {
            items.push(row(jsx(DFL.SliderField, {
                label: "Speed",
                value: st.speed || 5, min: 1, max: 10, step: 1, notchTicksVisible: true,
                disabled: !usable,
                onChange: (v) => change({ speed: v }),
            })));
        }
        if (effect.turns) {
            // One line, each switch on its stick's side.
            const reverse = (label, key) => jsxs("div", { style: { display: "flex", alignItems: "center", gap: "10px" }, children: [
                jsx("div", { children: label }),
                jsx(DFL.Toggle, { value: !!st[key], disabled: !usable, onChange: (v) => change({ [key]: v }) }),
            ] });
            items.push(row(jsxs(DFL.Focusable, {
                "flow-children": "horizontal",
                style: { display: "flex", justifyContent: "space-between", alignItems: "center", padding: "8px 0" },
                children: [reverse("Reverse Left", "reverse_left"), reverse("Reverse Right", "reverse_right")],
            })));
        }
        if (kpf && effect.id === "rainbow") items.push(note("The controller runs the rainbow at its own speed and brightness."));
    }
    if (kpf) {
        items.push(row(jsx(DFL.ToggleField, {
            label: "Power LED",
            description: "Charging / full / low-battery colours and profile flashes",
            checked: st.power_led !== false,
            onChange: (v) => setPowerLed(v).then(setSt).catch(() => {}),
        })));
        if (!usable) items.push(note("Stick lighting needs the controller MCU link: Hardware tab."));
        if (st.daemon === false) items.push(warn("pbosd is not running."));
        items.push(note("The K button can cycle stick lighting too (Hardware tab → Buttons)."));
    }
    if (st.on && usable) {
        items.push(row(jsx(DFL.ButtonItem, {
            layout: "below",
            onClick: () => { advancedOpen = !advanced; setAdvanced(!advanced); },
            children: advanced ? "Advanced ▾" : "Advanced ▸",
        })));
        if (advanced) {
            if (!effect.color) {
                items.push(note(`${effect.label} picks its own colours.`));
            } else {
                ["Red", "Green", "Blue"].forEach((name, i) => items.push(row(jsx(DFL.SliderField, {
                    label: name,
                    value: rgb[i], min: 0, max: 255, step: 1, showValue: true, editableValue: true,
                    onChange: (v) => { const c = rgb.slice(); c[i] = v; change({ color: toColor(c) }); },
                }))));
                items.push(note(`#${st.color.toUpperCase()}. The Color slider picks full colors; white and pastels are set here.`));
                if (effect.id === "audio" || effect.id === "spectrum") {
                    const rgb2 = toRgb(st.color2 || "ffffff");
                    ["Intensity Red", "Intensity Green", "Intensity Blue"].forEach((name, i) => items.push(row(jsx(DFL.SliderField, {
                        label: name,
                        value: rgb2[i], min: 0, max: 255, step: 1, showValue: true, editableValue: true,
                        onChange: (v) => { const c = rgb2.slice(); c[i] = v; change({ color2: toColor(c) }); },
                    }))));
                }
            }
        }
        if (st.kind === "multicolor") {
            items.push(row(jsx(DFL.ButtonItem, {
                layout: "below",
                onClick: () => { experimentalOpen = !experimental; setExperimental(!experimental); },
                children: experimental ? "Experimental ▾" : "Experimental ▸",
            })));
            if (experimental) {
                items.push(row(jsx(DFL.ToggleField, {
                    label: "Audio Effects",
                    description: "Adds effects that follow what the device plays (never the microphone) and switches "
                        + "to Audio Meter: Audio Meter fills each stick's ring with its side's loudness, kicked by the beat; "
                        + "Audio Spectrum shows four bands per stick (bass at the bottom, treble at the top). Other effects stay a pick away.",
                    checked: !!st.experimental_audio,
                    onChange: (v) => {
                        movedAt.current = Date.now();
                        setAudioPulse(v).then((s) => { movedAt.current = 0; setSt(s); }).catch(() => {});
                    },
                })));
                items.push(note("Experimental: may cost a little battery while sound plays."));
            }
        }
    }
    return jsx(DFL.PanelSection, { title: "Lighting", children: items });
}

// ---------------------------------------------- performance, hardware ---
// Was the PB-OS Control plugin: a front for pbosd on the KONKR Pocket FIT.
const PROFILES = [
    { data: "lowpower", label: "Low Power", desc: "About half the power: GPU up to 680 MHz, big cores up to 2 GHz, quiet fan" },
    { data: "balanced", label: "Full Power", desc: "Full clocks on demand, game threads on the big cores" },
];
const BUTTON_ACTIONS = [
    { data: "rgb-next", label: "Cycle stick lighting" },
    { data: "sticks-toggle", label: "Stick lighting on/off" },
    { data: "profile-next", label: "Switch performance profile" },
    { data: "none", label: "Do nothing" },
];
const lines = (...items) => jsx("div", { children: items.map((t, i) => jsx("div", { children: t }, i)) });

// pbosd's settings, read again every 2 s (the buttons and pbosctl change them
// too); not while a slider was just moved.
function useControl() {
    const [st, setSt] = useState(null);
    const movedAt = useRef(0);
    const refresh = useCallback(() => {
        if (Date.now() - movedAt.current < 2000) return;
        getControl().then((s) => { if (Date.now() - movedAt.current >= 2000) setSt(s); }).catch(() => {});
    }, []);
    useEffect(() => {
        refresh();
        const t = setInterval(refresh, 2000);
        return () => clearInterval(t);
    }, [refresh]);
    const apply = useCallback((p) => p.then((s) => { movedAt.current = 0; setSt(s); }).catch(() => {}), []);
    return { st, setSt, apply, movedAt };
}

const MODES = [
    { data: "balanced", label: "Full Power" },
    { data: "lowpower", label: "Low Power" },
];

// pbosd's temperature, fan and GPU, and its fan setting (KONKR Pocket FIT /
// AYANEO Pocket S2): the mode itself is the Default section's.
function PbosdFan() {
    const { st, setSt, apply, movedAt } = useControl();
    const sendFan = useThrottled(useCallback((v) => { setFan("fixed", v).catch(() => {}); }, []));
    if (!st || !st.available) return null;
    const fan = st.fan || { mode: "auto", fixed: 50 };
    const status = [
        st.temp_c != null ? `${st.temp_c} °C` : null,
        st.fan_rpm != null ? `fan ${st.fan_rpm} rpm (${Math.round((st.fan_pwm || 0) / 2.55)}%)` : null,
        st.gpu_mhz != null ? `GPU ${st.gpu_mhz} MHz` : null,
    ].filter(Boolean).join(" · ");
    return jsxs(DFL.PanelSection, { title: "Fan", children: [
        row(jsx(DFL.DropdownItem, {
            label: "Mode",
            description: fan.mode === "fixed"
                ? "Fixed speed (switches back to automatic above 90 °C)"
                : "Automatic, follows the performance mode's temperature curve",
            rgOptions: [{ data: "auto", label: "Automatic" }, { data: "fixed", label: "Fixed speed" }],
            selectedOption: fan.mode,
            onChange: (o) => apply(setFan(o.data, fan.fixed)),
        })),
        fan.mode === "fixed" ? row(jsx(DFL.SliderField, {
            label: "Speed",
            value: fan.fixed, min: 0, max: 100, step: 5, showValue: true, valueSuffix: "%",
            onChange: (v) => {
                movedAt.current = Date.now();
                setSt({ ...st, fan: { ...fan, fixed: v } });
                sendFan(v);
            },
        })) : null,
        st.daemon ? note(status) : warn("pbosd is not running."),
    ] });
}

// The mode while no game with its own mode runs.
function DefaultSection() {
    const [perf, setPerf] = useState(null);
    useEffect(() => { getPerformance().then(setPerf).catch(() => setPerf({})); }, []);
    if (!perf) return jsx(DFL.PanelSection, { title: "Default", children: note("Loading…") });
    const pbosd = perf.kind === "pbosd";
    const descOf = (mode) => pbosd
        ? (PROFILES.find((p) => p.data === mode) || PROFILES[1]).desc
        : (mode === "lowpower" ? perf.lowpower : "No caps: full clocks on demand");
    const mode = perf.default || "balanced";
    const items = [
        row(jsx(DFL.DropdownItem, {
            label: "Mode", description: descOf(mode),
            rgOptions: MODES, selectedOption: mode,
            onChange: (o) => setPerformanceDefault(o.data).then(setPerf).catch(() => {}),
        })),
        note(perf.game && perf.active && perf.active !== mode
            ? `The running game has its own mode: ${(MODES.find((m) => m.data === perf.active) || {}).label || perf.active}.`
            : "In force whenever no running game has a mode of its own (Per Game, below)."),
    ];
    return jsxs(SP_JSX.Fragment, { children: [
        jsx(DFL.PanelSection, { title: "Default", children: items }),
        pbosd ? jsx(PbosdFan, {}) : null,
    ] });
}

function PerformanceTab() {
    return jsxs(SP_JSX.Fragment, { children: [jsx(DefaultSection, {}), jsx(GamesTab, {})] });
}

// Face buttons, for people who physically moved the button caps: which
// InputPlumber map Steam sees A/B/X/Y through. Nintendo positions (A on the
// right) is the default on the Retroid Pocket 6 / Nova and AYN Thor, printed
// that way; Xbox positions (A at the bottom) on the KONKR Pocket FIT.
// sm8550-face-buttons swaps the map and restarts InputPlumber, so the pad
// reconnects once.
const FACE_LAYOUTS = [
    { data: "nintendo", label: "Nintendo Positions" },
    { data: "xbox", label: "XBox Positions" },
];
const FACE_DESC = "Use this option only if you have physically modified the button placement on your device.";

// The "Buttons" section's rows: its own section, or (inline, on the KONKR
// Pocket FIT) the top of pbosd's Buttons section.
function FaceButtons({ inline }) {
    const [st, setSt] = useState(null);
    useEffect(() => { getFaceButtons().then(setSt).catch(() => setSt({ supported: false })); }, []);
    if (!st) return inline ? null : jsx(DFL.PanelSection, { children: note("Loading…") });
    if (!st.supported) return inline ? null : jsx(DFL.PanelSection, { children: note("This device has no hardware settings here.") });
    const layout = FACE_LAYOUTS.find((l) => l.data === st.layout) || FACE_LAYOUTS[0];
    const rows = [
        row(jsx(DFL.DropdownItem, {
            label: "Button layout",
            layout: "below",
            rgOptions: FACE_LAYOUTS.map((l) => ({ data: l.data, label: l.label })),
            selectedOption: layout.data,
            onChange: (o) => {
                if (o.data === layout.data) return;
                setSt({ ...st, layout: o.data });
                setFaceButtons(o.data).then((r) => {
                    setSt(r);
                    toaster.toast({ title: "PB-OS Tools", body: "Face buttons switched, controller reconnects" });
                }).catch(() => getFaceButtons().then(setSt).catch(() => {}));
            },
        })),
        note(FACE_DESC),
    ];
    return inline ? jsx(SP_JSX.Fragment, { children: rows }) : jsx(DFL.PanelSection, { title: "Buttons", children: rows });
}

// ------------------------------------------------------------------- mtp ---
// MTP over the USB-C cable (pbos-mtp.service): the PC sees Home and Steam,
// with nothing to install at either end. Off by default, like SSH. One
// switch: the folders are fixed, so there is nothing here to remove.
function MtpShare() {
    const [st, setSt] = useState(null);
    useEffect(() => { getMtp().then(setSt).catch(() => setSt({ available: false })); }, []);
    if (!st || !st.available) return null;

    const desc = !st.enabled
        ? "Let a PC browse and copy your Home and Steam folders over the USB-C cable"
        : st.connected
            ? "Connected: the PC can see your Home and Steam folders"
            : "On: connect the USB-C cable to a PC";
    return jsxs(DFL.PanelSection, { title: "USB File Transfer", children: [
        row(jsx(DFL.ToggleField, {
            label: "MTP over USB",
            description: desc,
            checked: st.enabled,
            onChange: (v) => {
                setSt({ ...st, enabled: v });
                setMtp(v).then((r) => {
                    setSt(r);
                    toaster.toast({
                        title: "PB-OS Tools",
                        body: v ? "MTP on: the PC can see your Home and Steam folders" : "MTP off",
                    });
                }).catch(() => getMtp().then(setSt).catch(() => {}));
            },
        })),
        note(lines(
            "Closing the lid disconnects it, and a transfer still running is lost.",
            "Changing a game's files while Steam is running can confuse Steam.",
        )),
    ] });
}

// ---------------------------------------------------------------- sticks ---
// Stick calibration (AYN's Android tool, in Game Mode) and fine stick steps
// for the rsinput pad; sm8550-stickcal does the work and keeps both switches.
const STICK_EXPERIMENTAL = "Experimental, for testing.";
// Each direction of each axis has to reach this far (the helper's MIN_HALF)
// before Save is offered.
const STICK_MIN_HALF = 600;
const STICKS = [
    { label: "Left stick", x: "left_x", y: "left_y" },
    { label: "Right stick", x: "right_x", y: "right_y" },
];

function stickReached(live, rest, axis) {
    if (!live || !rest) return [0, 0];
    return [rest[axis] - live.lo[axis], live.hi[axis] - rest[axis]];
}

function stickReady(live, rest) {
    return ["left_x", "left_y", "right_x", "right_y"].every((a) => {
        const [neg, pos] = stickReached(live, rest, a);
        return neg >= STICK_MIN_HALF && pos >= STICK_MIN_HALF;
    });
}

// One stick: the square of the values reached so far, the dot where it is
// now, the centre cross at rest. The scale fits the default ±1408.
function StickView({ label, live, rest, x, y }) {
    const S = 120, H = S / 2, SCALE = 1408;
    const px = (v) => H + Math.max(-1, Math.min(1, v / SCALE)) * (H - 6);
    const r = rest || {};
    const at = (axis, v) => v - (r[axis] || 0);
    const items = [
        jsx("circle", { cx: H, cy: H, r: H - 6, fill: "none", stroke: "rgba(255,255,255,0.25)", strokeWidth: 1 }, "gate"),
        jsx("path", { d: `M${H - 6} ${H}h12M${H} ${H - 6}v12`, stroke: "rgba(255,255,255,0.4)", strokeWidth: 1 }, "c"),
    ];
    if (live) {
        const x0 = px(at(x, live.lo[x])), x1 = px(at(x, live.hi[x]));
        const y0 = px(at(y, live.lo[y])), y1 = px(at(y, live.hi[y]));
        items.push(jsx("rect", { x: x0, y: y0, width: Math.max(0, x1 - x0), height: Math.max(0, y1 - y0),
            fill: "rgba(26,159,255,0.15)", stroke: "#1a9fff", strokeWidth: 1 }, "seen"));
        items.push(jsx("circle", { cx: px(at(x, live.now[x])), cy: px(at(y, live.now[y])), r: 5, fill: "#fff" }, "dot"));
    }
    const nums = (axis) => {
        const [neg, pos] = stickReached(live, rest, axis);
        const ok = neg >= STICK_MIN_HALF && pos >= STICK_MIN_HALF;
        return jsx("div", { style: { color: ok ? "#8fe08f" : undefined }, children:
            `${axis.endsWith("x") ? "X" : "Y"}  −${Math.round(neg)} … +${Math.round(pos)}` });
    };
    return jsxs("div", { style: { display: "flex", flexDirection: "column", alignItems: "center", gap: "4px", fontSize: "12px" }, children: [
        jsx("div", { style: { fontWeight: 600 }, children: label }),
        jsx("svg", { width: S, height: S, viewBox: `0 0 ${S} ${S}`, children: items }),
        nums(x),
        nums(y),
    ] });
}

// The calibration screen. Steps: rest (let go, Start) → edges (round the
// edge until the numbers stop changing, Save) → done or error.
function CalibrateModal({ closeModal, onDone }) {
    const [step, setStep] = useState("rest");
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState("");
    const [rest, setRest] = useState(null);
    const [live, setLive] = useState(null);
    const finished = useRef(false);

    useEffect(() => {
        if (step !== "edges") return undefined;
        const t = setInterval(() => stickLive().then((l) => { if (l) setLive(l); }).catch(() => {}), 100);
        return () => clearInterval(t);
    }, [step]);

    useEffect(() => () => { if (!finished.current) stickCancel().catch(() => {}); }, []);

    const close = () => { closeModal(); };
    const begin = async () => {
        setBusy(true);
        setError("");
        try {
            const r = await stickRest();
            if (!r.ok) { setError(r.error); return; }
            setRest(r.rest);
            setLive(null);
            const s = await stickEdges();
            if (!s.ok) { setError(s.error); return; }
            setStep("edges");
        } catch (e) {
            setError(String(e));
        } finally {
            setBusy(false);
        }
    };
    const finish = async () => {
        setBusy(true);
        try {
            const r = await stickSave();
            if (!r.ok) { setError(r.error); setStep("rest"); return; }
            finished.current = true;
            setStep("done");
            onDone();
        } catch (e) {
            setError(String(e));
        } finally {
            setBusy(false);
        }
    };

    const button = (text, onClick, disabled) => jsx(DFL.DialogButton, {
        style: { flex: 1 }, disabled: busy || !!disabled, onClick, children: text });
    const buttons = (...children) => jsx(DFL.Focusable, {
        style: { display: "flex", gap: "8px", marginTop: "16px" }, children });
    const text = (t) => jsx("div", { style: { marginBottom: "8px" }, children: t });

    let body;
    if (step === "rest") {
        body = [
            text("Let go of both sticks, then press Start."),
            error && jsx("div", { style: { color: "#ff8080", marginBottom: "8px" }, children: error }),
            buttons(button(busy ? "Measuring…" : "Start", begin), button("Cancel", close)),
        ];
    } else if (step === "edges") {
        const ok = stickReady(live, rest);
        body = [
            text("Rotate each stick slowly around the edge 3–4 times, then press Save."),
            jsx("div", { style: { display: "flex", justifyContent: "space-around", margin: "8px 0" },
                children: STICKS.map((s) => jsx(StickView, { ...s, live, rest }, s.label)) }),
            !ok && jsx("div", { style: { fontSize: "12px", opacity: 0.75 },
                children: "Save works once every number is green." }),
            error && jsx("div", { style: { color: "#ff8080" }, children: error }),
            buttons(button(busy ? "Saving…" : "Save", finish, !ok), button("Cancel", close)),
        ];
    } else {
        body = [
            text("Saved and turned on."),
            buttons(button("Close", close)),
        ];
    }
    return jsx(DFL.ModalRoot, { onCancel: close, closeModal, children: jsxs("div", { children: [
        jsx("div", { style: { fontSize: "20px", fontWeight: 600, marginBottom: "12px" }, children: "Calibrate sticks" }),
        ...body.filter(Boolean),
    ] }) });
}

// The "Analog Sticks" section; nothing where sm8550-stickcal isn't (Pocket FIT).
function Sticks() {
    const [st, setSt] = useState(null);
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState("");

    const refresh = useCallback(() => stickState().then(setSt).catch((e) => setError(String(e))), []);
    useEffect(() => { refresh(); }, [refresh]);

    const run = async (fn) => {
        setBusy(true);
        setError("");
        try {
            const r = await fn();
            if (r && r.ok === false) setError(r.error);
            else if (r && r.restart_error) setError(`InputPlumber didn't restart: ${r.restart_error}`);
        } catch (e) {
            setError(String(e));
        } finally {
            setBusy(false);
            refresh();
        }
    };

    if (!st || !st.supported) return null;
    const items = [warn(STICK_EXPERIMENTAL)];
    if (!st.pad) {
        items.push(failure(st.error || "No controller found."));
        return jsx(DFL.PanelSection, { title: "Analog Sticks", children: items });
    }
    items.push(row(jsx(DFL.ToggleField, {
        label: "Use my calibration",
        description: st.has_calibration ? "Centers the sticks and gives them their full reach."
            : "Calibrate the sticks first.",
        checked: st.use_calibration,
        disabled: busy || !st.has_calibration,
        onChange: (on) => run(() => stickSwitch("calibration", on)),
    })));
    if (st.fine_supported) {
        items.push(row(jsx(DFL.ToggleField, {
            label: "Fine stick steps",
            description: "Smaller steps for more precise movement.",
            checked: st.fine,
            disabled: busy,
            onChange: (on) => run(() => stickSwitch("fine", on)),
        })));
    }
    items.push(row(jsx(DFL.ButtonItem, {
        layout: "below",
        disabled: busy,
        onClick: () => DFL.showModal(jsx(CalibrateModal, { onDone: refresh })),
        children: st.has_calibration ? "Calibrate again" : "Calibrate sticks",
    })));
    if (st.has_calibration) {
        items.push(row(jsx(DFL.ButtonItem, {
            layout: "below",
            disabled: busy,
            onClick: () => DFL.showModal(jsx(DFL.ConfirmModal, {
                strTitle: "Delete calibration?",
                strDescription: "Your sticks go back to the default settings.",
                strOKButtonText: "Delete",
                onOK: () => run(stickDelete),
            })),
            children: "Delete calibration",
        })));
    }
    if (error) items.push(failure(error));
    return jsx(DFL.PanelSection, { title: "Analog Sticks", children: items });
}

// The "Desktop Mode" section (AYN Thor): Barry Launcher on the bottom screen
// in Desktop Mode, or a plain desktop screen there; Game Mode keeps Barry.
function BarryDesktop() {
    const [st, setSt] = useState(null);
    const [busy, setBusy] = useState(false);
    useEffect(() => { getBarryDesktop().then(setSt).catch(() => setSt({ supported: false })); }, []);
    if (!st || !st.supported) return null;
    return jsx(DFL.PanelSection, { title: "Desktop Mode", children: [
        row(jsx(DFL.ToggleField, {
            label: "Barry Launcher",
            description: st.enabled
                ? "Barry Launcher on the bottom screen."
                : "The bottom screen is part of the desktop, with the taskbar and Plasma's keyboard.",
            checked: st.enabled,
            disabled: busy,
            onChange: (on) => {
                setBusy(true);
                setSt({ ...st, enabled: on });
                setBarryDesktop(on).then(setSt).catch(() => getBarryDesktop().then(setSt).catch(() => {}))
                    .finally(() => setBusy(false));
            },
        })),
        note("Takes effect the next time Desktop Mode starts. In Desktop Mode, use Barry Launcher On/Off in the applications menu."),
    ] });
}

function HardwareTab() {
    const { st, apply } = useControl();
    if (!st) return jsx(DFL.PanelSection, { children: note("Loading…") });
    if (!st.available) return jsxs(SP_JSX.Fragment, { children: [jsx(FaceButtons, {}), jsx(Sticks, {}), jsx(MtpShare, {}), jsx(BarryDesktop, {})] });
    const buttons = st.buttons || {};
    const steamButtons = (st.buttons_mode || "steam") === "steam";
    return jsxs(SP_JSX.Fragment, { children: [
        jsxs(DFL.PanelSection, { title: "Buttons", children: [
            jsx(FaceButtons, { inline: true }),
            row(jsx(DFL.ToggleField, {
                label: "Steam Remap",
                description: steamButtons
                    ? lines("Custom Function = Left Trackpad Click", "K = Right Trackpad Click", "Bind them in controller settings.")
                    : "Off: buttons map to actions selected below",
                checked: steamButtons,
                onChange: (v) => apply(setButtonsMode(v ? "steam" : "system")).then(() => {
                    toaster.toast({ title: "PB-OS Tools", body: "Buttons switched, controller reconnects" });
                }),
            })),
            row(jsx(DFL.DropdownItem, {
                label: "Custom Function",
                disabled: steamButtons,
                rgOptions: BUTTON_ACTIONS,
                selectedOption: buttons.F14 || "profile-next",
                onChange: (o) => apply(setButton("F14", o.data)),
            })),
            row(jsx(DFL.DropdownItem, {
                label: "K",
                disabled: steamButtons,
                rgOptions: BUTTON_ACTIONS,
                selectedOption: buttons.F13 || "rgb-next",
                onChange: (o) => apply(setButton("F13", o.data)),
            })),
            note("Navigation → Steam · = → Quick Access · Power: tap to sleep, hold for the power menu"),
        ] }),
        jsxs(DFL.PanelSection, { title: "Hardware", children: [
            row(jsx(DFL.ToggleField, {
                label: "Controller MCU link",
                description: "Needed for the KONKR, Performance and Quick Access buttons and stick lighting (Lighting tab)",
                checked: st.mcu_enabled,
                onChange: (v) => apply(setMcu(v)).then(() => {
                    toaster.toast({ title: "PB-OS Tools", body: v ? "MCU link enabled" : "MCU link disabled" });
                }),
            })),
        ] }),
        jsx(MtpShare, {}),
    ] });
}

// ---------------------------------------------------------------- power ---
// The charging indicator: while the device rests on the charger (instead of
// sleeping), the stick lights pulse until it's charged, then stay lit.
// sm8550-charge-rest keeps the settings and their ranges; levels are 0-255
// at a quarter of the LEDs' current, so the lowest are very dim.
const secs = (v) => `${Number(v).toFixed(1).replace(/\.0$/, "")} s`;

// Battery charging: the Retroid Pocket 6 / Nova and AYN Thor firmware can
// stop at 80 % or run the device from USB power without charging (charge
// separation); the KONKR Pocket FIT's firmware enforces charge thresholds
// (it stops at 80 % out of the box). Slow charging caps the charge current
// on every device (sm8550-charge-control).
const CHARGE_MODES = [
    { data: "normal", label: "Normal", desc: "Charges to 100 %." },
    { data: "limit80", label: "Stop at 80 %", desc: "The battery firmware stops charging at 80 %. Easier on the battery when the device lives on the charger." },
    { data: "hold", label: "Hold at a level", desc: "Charges to the level below, then stops and leaves the battery alone.", oem: "Charges to the level below, then runs from USB power and leaves the battery alone." },
    { data: "bypass", label: "Run from USB", desc: "Runs from USB power and doesn't charge (charge separation). Below 15 % it still charges a little.", feature: "bypass" },
];
const CHARGE_SPEEDS = [
    { data: false, label: "Full speed", desc: "Charges as fast as the firmware allows." },
    { data: true, label: "Slow", desc: "Caps the charge current: less heat, easier on the battery, good for playing on the charger." },
];
const amps = (ma) => `${(ma / 1000).toFixed(2).replace(/0$/, "").replace(/\.0$/, "")} A`;

function ChargeControl() {
    const [cc, setCc] = useState(null);
    const movedAt = useRef(0);
    const send = useThrottled(useCallback((changes) => {
        setChargeControl(changes).then((r) => { if (Date.now() - movedAt.current >= 2000) setCc(r); }).catch(() => {});
    }, []));
    useEffect(() => {
        const load = () => { if (Date.now() - movedAt.current >= 2000) getChargeControl().then(setCc).catch(() => {}); };
        load();
        const t = setInterval(load, 5000);
        return () => clearInterval(t);
    }, []);
    if (!cc || !cc.supported) return null;
    const s = cc.settings;
    const f = cc.features || {};
    const change = (patch) => {
        movedAt.current = Date.now();
        const next = { ...s, ...patch };
        setCc({ ...cc, settings: next });
        send(next);
    };
    const modes = CHARGE_MODES.filter((m) => !m.feature || f[m.feature]);
    const mode = modes.find((m) => m.data === s.mode) || modes[0];
    const speed = CHARGE_SPEEDS.find((m) => m.data === !!s.slow) || CHARGE_SPEEDS[0];
    const live = cc.state || {};
    const slowNow = s.slow && live.charge_current_ma ? ` Slow charging, up to ${amps(live.charge_current_ma)}.` : "";
    const status = !live.plugged ? "Not on a charger."
        : live.held ? `On the charger, holding at ${live.capacity} %.`
        : `On the charger, ${String(live.status || "").toLowerCase() || "charging"} at ${live.capacity} %.${slowNow}`;
    return jsx(DFL.PanelSection, { title: "Battery", children: [
        row(jsx(DFL.DropdownItem, {
            label: "Charging",
            description: (cc.backend === "oem" && mode.oem) || mode.desc,
            rgOptions: modes.map((m) => ({ data: m.data, label: m.label })),
            selectedOption: mode.data,
            onChange: (o) => change({ mode: o.data }),
        })),
        s.mode === "hold" ? row(jsx(DFL.SliderField, {
            label: `Hold at ${s.hold_at} %`,
            value: s.hold_at, min: cc.limits.hold_at[0], max: cc.limits.hold_at[1], step: 5,
            onChange: (v) => change({ hold_at: v }),
        })) : null,
        f.slow ? row(jsx(DFL.DropdownItem, {
            label: "Charging speed",
            description: speed.desc,
            rgOptions: CHARGE_SPEEDS.map((m) => ({ data: m.data, label: m.label })),
            selectedOption: !!s.slow,
            onChange: (o) => change({ slow: !!o.data }),
        })) : null,
        f.slow && s.slow ? row(jsx(DFL.SliderField, {
            label: `Slow charging: up to ${amps(s.slow_ma)}`,
            value: s.slow_ma, min: cc.limits.slow_ma[0], max: cc.limits.slow_ma[1], step: 250,
            onChange: (v) => change({ slow_ma: v }),
        })) : null,
        note(status),
    ] });
}

function PowerTab() {
    const [st, setSt] = useState(null);
    const [previewing, setPreviewing] = useState(false);
    const movedAt = useRef(0);
    const refresh = useCallback(() => {
        if (Date.now() - movedAt.current < 2000) return;
        getCharging().then((r) => { if (Date.now() - movedAt.current >= 2000) setSt(r); }).catch(() => {});
    }, []);
    const send = useThrottled(useCallback((changes) => { setCharging(changes).catch(() => {}); }, []));
    useEffect(() => { refresh(); }, [refresh]);

    if (!st) return jsx(DFL.PanelSection, { children: note("Loading…") });
    if (!st.supported) return jsx(ChargeControl, {});
    const s = st.settings;
    const lim = st.limits;
    // Every change goes out whole, so a throttled send never drops one.
    const change = (patch) => {
        movedAt.current = Date.now();
        const next = { ...s, ...patch };
        if (next.pulse_min >= next.pulse_max) next.pulse_min = next.pulse_max - 1;
        setSt({ ...st, settings: next });
        send(next);
    };
    const swatch = (c) => jsx("span", { style: { display: "inline-block", width: "14px", height: "14px", borderRadius: "7px",
        marginLeft: "8px", verticalAlign: "middle", border: "1px solid rgba(255,255,255,0.5)", background: `#${c}` } });
    const colorSlider = (key) => row(jsx(DFL.SliderField, {
        label: jsxs("span", { children: ["Color", swatch(s[key])] }),
        description: jsx("div", { style: { height: "8px", borderRadius: "4px", background: RAINBOW } }),
        value: colorHue(s[key]), min: 0, max: 359, step: 3,
        onChange: (h) => change({ [key]: hueColor(h) }),
    }));
    const level = (label, key, min, max, description) => row(jsx(DFL.SliderField, {
        label, description, value: s[key], min, max, step: 1, showValue: true,
        onChange: (v) => change({ [key]: v }),
    }));
    const time = (label, key, range) => row(jsx(DFL.SliderField, {
        label: `${label}: ${secs(s[key])}`,
        value: s[key], min: range[0], max: range[1], step: 0.5,
        onChange: (v) => change({ [key]: v }),
    }));
    const heading = (text) => row(jsx("div", { style: { fontWeight: "bold", marginTop: "4px" }, children: text }));
    const items = [
        row(jsx(DFL.ToggleField, {
            label: "Charging Indicator",
            description: "While the device sleeps on the charger, the stick lights pulse until it's charged, then stay lit.",
            checked: !!s.enabled,
            onChange: (v) => change({ enabled: v }),
        })),
    ];
    if (s.enabled) {
        items.push(
            heading("Charging"),
            colorSlider("pulse_color"),
            level("Pulse Brightness", "pulse_max", lim.pulse_max[0], lim.pulse_max[1]),
            level("Pulse Minimum", "pulse_min", lim.pulse_min[0], Math.max(lim.pulse_min[0] + 1, s.pulse_max - 1),
                "How far each pulse fades; 0 is off between pulses."),
            time("Pulse Length", "on_s", lim.on_s),
            time("Time Between Pulses", "off_s", lim.off_s),
            heading("Charged"),
            colorSlider("solid_color"),
            level("Brightness", "solid_level", lim.solid_level[0], lim.solid_level[1]),
            row(jsx(DFL.ButtonItem, {
                layout: "below",
                disabled: previewing,
                onClick: () => {
                    setPreviewing(true);
                    previewCharging().catch(() => {});
                    setTimeout(() => setPreviewing(false), (s.on_s + s.off_s + 4) * 1000);
                },
                children: previewing ? "Previewing…" : "Preview",
            })),
            note("Preview shows one pulse, its pause, then the charged light. The lights are very dim on purpose "
                + "(the lowest settings are the defaults); at the dimmest levels a mixed color can show as its strongest part."),
        );
    }
    return jsxs(SP_REACT.Fragment, { children: [jsx(ChargeControl, {}), jsx(DFL.PanelSection, { title: "Power", children: items })] });
}

// ---------------------------------------------------------------- games ---
// Per-game profiles: the x86 emulation (FEX) preset or the user's own FEX
// switches, the CPU cores the game runs on, and environment variables.
// /usr/bin/pbos-game-launch applies them when the game starts; this puts it
// in a game's launch options when the game gets a profile and takes it out
// when the profile is reset, so games without one are never touched.
// Port of Armada's per-game compatibility settings (armada-os/armada,
// decky/armada-control, GPL-2.0-or-later): the FEX presets and switches, the
// core choice with Wine's CPU topology, the environment-variable list and
// editor, and the launch-option wrapping.
const GAME_LAUNCH = "/usr/bin/pbos-game-launch";
// Steam app types: game, application, demo; and a non-Steam game.
const GAME_TYPES = new Set([1, 2, 8]);
const SHORTCUT_TYPE = 1073741824;
const ENV_NAME = /^[A-Za-z_][A-Za-z0-9_]*$/;
const CORE_CHOICES = [
    { data: "all", label: "All Cores" },
    { data: "performance", label: "Big + Prime Cores" },
    { data: "big", label: "Big Cores" },
    { data: "little", label: "Little Cores" },
];
// The game picked, kept while Steam runs.
let lastGame = null;

function runningGame() {
    const app = DFL.Router && DFL.Router.MainRunningApp;
    return app && app.appid ? String(app.appid) : null;
}

// Installed games and non-Steam games: appid -> name.
function installedGames() {
    const cs = window.collectionStore;
    const names = new Map();
    for (const c of [cs && cs.localGamesCollection, cs && cs.deckDesktopApps]) {
        for (const app of (c && c.allApps) || []) {
            if (!GAME_TYPES.has(app.app_type) && app.app_type !== SHORTCUT_TYPE) continue;
            names.set(String(app.appid), app.display_name || `App ${app.appid}`);
        }
    }
    return names;
}

// The launch options with the wrapper in front of %command% (null: already
// there). Options without %command% are arguments, so they go after it.
function withWrapper(opts, appid) {
    if (opts.includes(GAME_LAUNCH)) return null;
    const prefix = `${GAME_LAUNCH} --appid=${appid} %command%`;
    if (opts.includes("%command%")) return opts.replace("%command%", prefix);
    return opts.trim() ? `${prefix} ${opts.trim()}` : prefix;
}

function withoutWrapper(opts) {
    if (!opts.includes(GAME_LAUNCH)) return null;
    const next = opts.replace(/\/usr\/bin\/pbos-game-launch(\s+--appid=\d+)?\s*/g, "").trim();
    return next === "%command%" ? "" : next;
}

async function setWrapper(appid, on) {
    const d = await appDetails(Number(appid));
    if (!d) return false;
    const now = d.strLaunchOptions || "";
    const next = on ? withWrapper(now, appid) : withoutWrapper(now);
    if (next !== null) SteamClient.Apps.SetAppLaunchOptions(Number(appid), next);
    return true;
}

function cpulistError(text, count) {
    const seen = new Set();
    for (const part of text.split(",")) {
        const item = part.trim();
        if (!item) continue;
        const m = /^(\d+)(?:-(\d+))?$/.exec(item);
        if (!m) return `"${item}" is not a core number or a range like 4-7.`;
        const lo = Number(m[1]);
        const hi = m[2] !== undefined ? Number(m[2]) : lo;
        if (hi < lo) return `${item}: the range runs backwards.`;
        for (let c = lo; c <= hi; c++) {
            if (c >= count) return `This device has no core ${c} (it has cores 0-${count - 1}).`;
            if (seen.has(c)) return `Core ${c} is listed twice.`;
            seen.add(c);
        }
    }
    return seen.size ? "" : "List at least one core.";
}

function EnvModal({ closeModal, presets, name: name0, value: value0, fromList, onSave, onDelete }) {
    const [name, setName] = useState(name0 || "");
    const [value, setValue] = useState(value0 || "");
    const [error, setError] = useState("");
    const preset = presets.find((p) => p.name === name);
    const save = () => {
        const n = name.trim();
        if (!ENV_NAME.test(n)) {
            setError("A name has letters, digits and underscores only, and doesn't start with a digit.");
            return;
        }
        onSave(n, value);
        closeModal();
    };
    const button = (text, onClick, disabled) => jsx(DFL.DialogButton, {
        style: { width: "auto", minWidth: "110px" }, disabled, onClick, children: text });
    return jsx(DFL.ModalRoot, {
        closeModal, onCancel: closeModal,
        children: jsxs("div", { children: [
            jsx("div", { style: { fontSize: "20px", fontWeight: "bold", marginBottom: "12px" },
                children: name0 ? "Edit Variable" : "Add Variable" }),
            fromList
                ? jsx(DFL.DropdownItem, {
                    label: "Name", strDefaultLabel: "Choose a variable",
                    rgOptions: presets.map((p) => ({ data: p.name, label: p.name })),
                    selectedOption: name || null,
                    onChange: (o) => { setName(o.data); setError(""); },
                })
                : jsx(DFL.TextField, { label: "Name", value: name, onChange: (e) => { setName(e.target.value); setError(""); } }),
            preset ? jsx("div", { style: { fontSize: "12px", opacity: 0.75, margin: "6px 0" }, children: preset.description }) : null,
            preset && preset.options
                ? jsx(DFL.DropdownItem, {
                    label: "Value", strDefaultLabel: "Choose a value",
                    rgOptions: preset.options.map((o) => ({ data: o.data, label: o.label })),
                    selectedOption: value || null,
                    onChange: (o) => setValue(o.data),
                })
                : jsx(DFL.TextField, {
                    label: "Value", value,
                    description: preset && preset.example ? `For example ${preset.example}` : undefined,
                    onChange: (e) => setValue(e.target.value),
                }),
            error ? jsx("div", { style: { color: "#ff6b6b", marginTop: "8px" }, children: error }) : null,
            jsxs(DFL.Focusable, {
                "flow-children": "horizontal",
                style: { display: "flex", gap: "8px", marginTop: "16px", justifyContent: "flex-end" },
                children: [
                    onDelete ? button("Delete", () => { onDelete(); closeModal(); }) : null,
                    button("Cancel", closeModal),
                    button("Save", save, !name || (fromList && !value)),
                ],
            }),
        ] }),
    });
}

// ProtonDB Community Fix: the launch fix most of the game's working ProtonDB reports agree
// on (tools/protondb-hints.py), applied as profile variables and game
// arguments. Reports are from x86 PCs and Steam Decks.
function CommunitySection({ appid, profile, save }) {
    const [hint, setHint] = useState(null);
    useEffect(() => {
        setHint(null);
        getCommunityHint(appid).then((r) => setHint(r || { available: false })).catch(() => setHint({ available: false }));
    }, [appid]);
    if (!hint || !hint.available) return null;
    const g = hint.game;
    const items = [];
    if (!g || !g.k) {
        items.push(note("ProtonDB has no community fix for this game."));
    } else {
        const env = g.env || {};
        const args = g.args || [];
        const fix = [...Object.entries(env).map(([k, v]) => `${k}=${v}`), ...args].join(" ");
        const pEnv = profile.env || {};
        const pArgs = profile.args || [];
        const applied = Object.entries(env).every(([k, v]) => pEnv[k] === v) && args.every((a) => pArgs.includes(a));
        items.push(row(jsx(DFL.ButtonItem, {
            layout: "below", disabled: applied,
            description: `${fix} (used by ${g.k} of ${g.w} working ProtonDB reports)`,
            onClick: () => {
                const nextEnv = { ...pEnv, ...env };
                const nextArgs = [...pArgs, ...args.filter((a) => !pArgs.includes(a))];
                save({
                    env: Object.keys(nextEnv).length ? nextEnv : undefined,
                    args: nextArgs.length ? nextArgs : undefined,
                });
            },
            children: applied ? "ProtonDB Fix Applied" : "Apply ProtonDB Fix",
        })));
        items.push(note("Source: protondb.com, player reports from x86 PCs and Steam Decks (ODbL). Not tested on PB-OS."));
    }
    if ((profile.args || []).length) {
        items.push(row(jsx(DFL.ButtonItem, {
            label: "Game arguments", description: profile.args.join(" "),
            onClick: () => save({ args: undefined }), children: "Remove",
        })));
    }
    return jsx(DFL.PanelSection, { title: "ProtonDB Community Fix", children: items });
}

function GamesTab() {
    const [info, setInfo] = useState(null);
    const [profiles, setProfiles] = useState({});
    const [pick, setPickState] = useState(lastGame || runningGame());
    const [opts, setOpts] = useState(null);
    const [customCores, setCustomCores] = useState(false);
    const [coresText, setCoresText] = useState(null);
    const [error, setError] = useState("");
    const queue = useRef(Promise.resolve());
    const setPick = (appid) => { lastGame = appid; setPickState(appid); };
    const showOpts = useCallback((appid) => {
        setOpts(null);
        if (appid) appDetails(Number(appid)).then((d) => setOpts(d ? d.strLaunchOptions || "" : null));
    }, []);
    useEffect(() => {
        getGameProfiles().then((r) => { setInfo(r || {}); setProfiles((r && r.games) || {}); }).catch(() => setInfo({}));
    }, []);
    useEffect(() => { showOpts(pick); setCustomCores(false); setCoresText(null); setError(""); }, [pick]);

    if (!info) return jsx(DFL.PanelSection, { children: note("Loading…") });
    if (!info.available) return jsx(DFL.PanelSection, { children: note("This pb-os build has no game profiles (pbos-game-launch is missing).") });

    const names = installedGames();
    const running = runningGame();
    const nameOf = (appid) => names.get(appid) || (profiles[appid] && profiles[appid].name) || `App ${appid}`;
    const ids = [...new Set([...names.keys(), ...Object.keys(profiles)])]
        .filter((id) => id !== running)
        .sort((a, b) => nameOf(a).localeCompare(nameOf(b)));
    const label = (id) => `${nameOf(id)}${profiles[id] ? " (profile)" : ""}`;
    const gameChoices = [
        ...(running ? [{ data: running, label: `Now playing: ${label(running)}` }] : []),
        ...ids.map((id) => ({ data: id, label: label(id) })),
    ];
    const picker = jsx(DFL.PanelSection, { title: "Per Game", children: [
        // The dropdown under the word, the full width: game names are long.
        row(jsx(DFL.DropdownItem, {
            label: "Game", strDefaultLabel: "Choose a game",
            layout: "below", childrenContainerWidth: "max",
            rgOptions: gameChoices, selectedOption: pick,
            onChange: (o) => setPick(o.data),
        })),
        note("Settings for one game. Its mode applies while it runs; the rest the next time it starts."),
    ] });
    if (!pick) return picker;

    const p = profiles[pick] || {};
    // Saves go out one at a time, in order; the reply is what was kept.
    const commit = (appid, next) => {
        queue.current = queue.current.then(() => setGameProfile(appid, next)).then(async (r) => {
            if (!r || !r.ok) { setError(`Not saved: ${(r && r.error) || "unknown error"}`); return; }
            setProfiles((all) => {
                const out = { ...all };
                if (r.profile) out[appid] = r.profile; else delete out[appid];
                return out;
            });
            if (!(await setWrapper(appid, !!r.profile))) setError("Saved, but this game's launch options could not be changed. Try again.");
            else setError("");
            if (lastGame === appid) showOpts(appid);
        }).catch(() => setError("Not saved."));
    };
    const save = (patch) => {
        const next = { ...p, ...patch, name: nameOf(pick) };
        for (const k of Object.keys(patch)) if (patch[k] === undefined) delete next[k];
        setProfiles({ ...profiles, [pick]: next });
        commit(pick, next);
    };

    // Performance mode while this game runs.
    const modeItems = [
        row(jsx(DFL.DropdownItem, {
            layout: "below", childrenContainerWidth: "max",
            rgOptions: [{ data: "", label: "Use Default" }, ...MODES],
            selectedOption: p.mode || "",
            onChange: (o) => save({ mode: o.data || undefined }),
        })),
        note(p.mode ? "In force while this game runs; the default mode comes back when it ends." : "The Default mode, above."),
    ];

    // x86 emulation.
    const doc = info.presets || {};
    const presets = doc.presets || {};
    const options = doc.options || [];
    const fex = p.fex === "custom" || presets[p.fex] ? p.fex : "default";
    const defaults = Object.fromEntries(options.map((o) => [o.key, o.default]));
    const fexNow = fex === "custom" ? { ...defaults, ...(p.fexConfig || {}) } : { ...defaults, ...((presets[fex] || {}).config || {}) };
    const fexText = fex === "custom" ? "Your own switches, below." : (presets[fex] || {}).description;
    const fexItems = [
        row(jsx(DFL.DropdownItem, {
            layout: "below", childrenContainerWidth: "max",
            rgOptions: [...Object.entries(presets).map(([id, x]) => ({ data: id, label: x.label })), { data: "custom", label: "Custom" }],
            selectedOption: fex,
            onChange: (o) => save(o.data === "custom" ? { fex: "custom", fexConfig: fexNow } : { fex: o.data, fexConfig: undefined }),
        })),
        ...(fexText ? [note(fexText)] : []),
    ];
    if (fex === "custom") {
        for (const o of options) {
            fexItems.push(row(jsx(DFL.ToggleField, {
                label: o.label, checked: fexNow[o.key] === "1",
                onChange: (v) => save({ fex: "custom", fexConfig: { ...fexNow, [o.key]: v ? "1" : "0" } }),
            })));
        }
    }
    fexItems.push(note("For Windows and x86 Linux games. Native ARM games ignore it."));

    // CPU cores.
    const groups = info.cores || {};
    const count = info.cpus || 8;
    const cores = p.cores || "";
    const isCustom = customCores || (cores !== "" && !(cores in groups));
    const text = coresText !== null ? coresText : (isCustom ? cores : "");
    const coresError = isCustom && text ? cpulistError(text, count) : "";
    const cpuItems = [
        row(jsx(DFL.DropdownItem, {
            layout: "below", childrenContainerWidth: "max",
            rgOptions: [
                { data: "", label: "Default" },
                ...CORE_CHOICES.filter((c) => groups[c.data]).map((c) => ({ data: c.data, label: `${c.label} (${groups[c.data]})` })),
                { data: "custom", label: "Custom" },
            ],
            selectedOption: isCustom ? "custom" : cores,
            onChange: (o) => {
                if (o.data === "custom") { setCustomCores(true); return; }
                setCustomCores(false);
                setCoresText(null);
                save({ cores: o.data || undefined, wineTopology: o.data ? p.wineTopology : undefined });
            },
        })),
        ...(cores ? [] : [note("Steam's choice: all cores.")]),
    ];
    if (isCustom) {
        cpuItems.push(row(jsx(DFL.TextField, {
            label: `Core list (0-${count - 1}), like 4-7 or 2,3,7`,
            value: text,
            onChange: (e) => {
                const t = e.target.value;
                setCoresText(t);
                // Only a valid list is saved.
                if (!cpulistError(t, count)) save({ cores: t.replace(/\s/g, "") });
            },
        })));
        if (coresError) cpuItems.push(warn(coresError));
    }
    if (cores) {
        cpuItems.push(row(jsx(DFL.ToggleField, {
            label: "Match Windows' Core Count",
            description: "Windows games see only these cores (Wine's CPU topology).",
            checked: p.wineTopology !== false,
            onChange: (v) => save({ wineTopology: v ? undefined : false }),
        })));
    }

    // Environment variables.
    const env = p.env || {};
    const envList = info.env || [];
    const setEnv = (next) => save({ env: Object.keys(next).length ? next : undefined });
    const openEnv = (key, fromList) => DFL.showModal(jsx(EnvModal, {
        presets: envList,
        name: key || "", value: key ? env[key] : "",
        fromList: fromList || (!!key && envList.some((x) => x.name === key)),
        onSave: (name, value) => {
            const next = { ...env };
            if (key && key !== name) delete next[key];
            next[name] = value;
            setEnv(next);
        },
        onDelete: key ? () => { const next = { ...env }; delete next[key]; setEnv(next); } : null,
    }));
    const envItems = Object.keys(env).sort().map((k) => row(jsx(DFL.ButtonItem, {
        label: k, description: env[k] === "" ? "(empty)" : env[k],
        onClick: () => openEnv(k, false), children: "Edit",
    })));
    envItems.push(
        row(jsx(DFL.ButtonItem, { layout: "below", onClick: () => openEnv(null, true), children: "Add Common Variable" })),
        row(jsx(DFL.ButtonItem, { layout: "below", onClick: () => openEnv(null, false), children: "Add Your Own Variable" })),
    );

    // The profile itself.
    const has = !!profiles[pick];
    const profileItems = [];
    if (has && opts !== null && !opts.includes(GAME_LAUNCH)) {
        profileItems.push(warn("pbos-game-launch is not in this game's launch options, so the profile does nothing. Change any setting to put it back."));
    }
    if (has && opts) profileItems.push(note(`Launch options: ${opts}`));
    if (has) {
        profileItems.push(row(jsx(DFL.ButtonItem, {
            layout: "below",
            onClick: () => DFL.showModal(jsx(DFL.ConfirmModal, {
                strTitle: `Reset ${nameOf(pick)}?`,
                strDescription: "Its profile is deleted and pbos-game-launch comes out of its launch options. Your own launch options stay.",
                strOKButtonText: "Reset",
                onOK: () => {
                    setProfiles((all) => { const out = { ...all }; delete out[pick]; return out; });
                    setCustomCores(false);
                    setCoresText(null);
                    commit(pick, null);
                },
            })),
            children: "Reset Game",
        })));
    } else {
        profileItems.push(note("No profile yet: this game starts as usual. Changing a setting makes one."));
    }
    if (error) profileItems.push(failure(error));

    return jsxs(SP_JSX.Fragment, { children: [
        picker,
        jsx(CommunitySection, { appid: pick, profile: p, save }),
        jsx(DFL.PanelSection, { title: "Mode", children: modeItems }),
        jsx(DFL.PanelSection, { title: "FEX Preset", children: fexItems }),
        jsx(DFL.PanelSection, { title: "CPU Cores", children: cpuItems }),
        jsx(DFL.PanelSection, { title: "Environment", children: envItems }),
        jsx(DFL.PanelSection, { title: "Profile", children: profileItems }),
    ] });
}

function onAvailable(title) {
    toaster.toast({ title: "pb-os update", body: `${title} is available. Open PB-OS › Updates in Quick Access.`, duration: 6000 });
}

function onLocal(version, place) {
    toaster.toast({ title: "pb-os update", body: `pb-os ${version} is ${place}. Open PB-OS › Updates in Quick Access to install it.`, duration: 6000 });
}

function onMoveDone(direction, ok) {
    const what = direction === "sd" ? "Copy to microSD" : "Install to internal storage";
    toaster.toast({ title: "PB-OS Tools", body: ok ? `${what} finished. Open PB-OS › Move SteamOS for the next steps.` : `${what} failed. Open PB-OS › Move SteamOS for details.`, duration: 8000 });
}

// Toast whenever the profile changes (Performance button, pbosctl or the
// Performance tab), like Android's on-screen mode switch. Registered at
// plugin load, so it works with Quick Access closed and over games.
const MODE_TOAST = {
    lowpower: { title: "🔋  Low Power", body: "About half the power, GPU and CPU capped" },
    balanced: { title: "⚡  Full Power", body: "Full clocks on demand" },
};
function onMode(profile) {
    const t = MODE_TOAST[profile] || { title: profile, body: "" };
    toaster.toast({ title: t.title, body: t.body, duration: 2000, playSound: false, critical: true });
}

// Proton PB-OS follows Valve's Proton pick, per game:
//   Valve's Proton 11.0 (ARM64)      -> Proton 11.0-2 PB-OS (ARM)
//   Valve's Proton Experimental (ARM64) -> Proton Experimental PB-OS (ARM)
// Valve's pick (priority 85) outranks Steam's global default (75), so the twin
// is set per game like a user's pick (250). Anything else stays: Linux ports
// (Steam Linux Runtime), games the user set themselves, and games whose Valve
// pick is just "the default" (they get the global default, below). A game the
// user changes after PB-OS switched it is left alone from then on.
// The global default is checked once: if it is still Valve's own
// (proton-stable or empty), it becomes Proton 11.0-2 PB-OS (ARM). Either way
// the check is recorded ("global_set" in proton-games.json) and the global
// default is never touched again, so whatever the user picks later stays.
// A device where an earlier version already set the twin just records it.
// Event-driven: at Steam start, when Steam's app overviews change (installs,
// updates), and when a game's Play is pressed.
const USER_PICK = 250;
const VALVE_DEFAULT = ["", "proton-stable"];
function appDetails(appid) {
    return new Promise((resolve) => {
        let reg = null;
        const done = (d) => { clearTimeout(timer); if (reg && reg.unregister) reg.unregister(); resolve(d); };
        const timer = setTimeout(() => done(null), 5000);
        reg = SteamClient.Apps.RegisterForAppDetails(appid, (d) => done(d));
    });
}
async function protonCheck(appids) {
    const twins = (await getProton()).tools || {};
    if (!Object.keys(twins).length) return;
    const settings = window.settingsStore && window.settingsStore.m_Settings;
    const stable = twins["proton_11-arm64"];
    const state = await getProtonGames();
    if (stable && settings && !state.global_set) {
        const current = settings.strCompatTool || "";
        if (VALVE_DEFAULT.includes(current)) {
            SteamClient.Settings.SpecifyGlobalCompatTool(stable);
            state.global_set = stable;
            await setProtonGames(state, `global default -> ${stable} (once)`);
        } else {
            state.global_set = current;
            await setProtonGames(state, `global default is ${current}, left as is from now on`);
        }
    }
    const ours = new Set(Object.values(twins));
    const notes = [];
    let changed = false;
    if (!appids) {
        const local = window.collectionStore && window.collectionStore.localGamesCollection;
        appids = ((local && local.allApps) || []).map((app) => app.appid);
    }
    for (const appid of appids) {
        const key = String(appid);
        if (state.hands_off.includes(key)) continue;
        const d = await appDetails(Number(appid));
        if (!d) continue;
        const tool = d.strCompatToolName || "";
        const mine = state.switched[key];
        if (mine) {
            // Still ours, or changed by the user since: then never again.
            if (tool !== mine.twin) {
                delete state.switched[key];
                state.hands_off.push(key);
                notes.push(`${key}: user picked ${tool || "default"}, hands off`);
                changed = true;
            }
            continue;
        }
        const twin = twins[tool];
        if (!twin || d.nCompatToolPriority >= USER_PICK || ours.has(tool)) continue;
        SteamClient.Apps.SpecifyCompatTool(Number(appid), twin);
        state.switched[key] = { twin, valve: tool };
        notes.push(`${key}: ${tool} -> ${twin}`);
        changed = true;
    }
    if (changed) await setProtonGames(state, notes.join("; "));
}
// One check at a time; a burst of events becomes one more check.
let protonBusy = Promise.resolve();
let protonQueued = null;
function protonSoon(appids) {
    if (protonQueued) {
        protonQueued.appids = protonQueued.appids && appids ? [...new Set([...protonQueued.appids, ...appids])] : null;
        return;
    }
    protonQueued = { appids };
    protonBusy = protonBusy.then(() => new Promise((r) => setTimeout(r, 1500))).then(() => {
        const q = protonQueued;
        protonQueued = null;
        return protonCheck(q.appids);
    }).catch(() => {});
}
let protonEvents = [];

// ------------------------------------------------------------- plugins ---
// Each former tab is a plugin in the PB-OS list. Games first on every
// device; the device's own ones (lighting, performance, hardware, power)
// once the backend has said which it has.
// Performance: the default mode (with pbosd's fan on the KONKR Pocket FIT /
// AYANEO Pocket S2) and per-game settings.
P.plugin({ id: "performance", title: "Performance", icon: PerformanceIcon, content: PerformanceTab, order: 10 });
P.plugin({ id: "update", title: "Updates", icon: UpdateIcon, content: UpdateTab, order: 70 });
P.plugin({ id: "install", title: "Move SteamOS", icon: InstallIcon, content: InstallTab, order: 80 });
(async () => {
    const lights = await getLights().then((s) => s.kind || "").catch(() => "");
    const [f, s] = await Promise.all([getFaceButtons().catch(() => ({})), stickState().catch(() => ({}))]);
    const face = !!f.supported || !!s.supported;
    const [rest, cc] = await Promise.all([getCharging().catch(() => ({})), getChargeControl().catch(() => ({}))]);
    const power = !!rest.supported || !!cc.supported;
    if (lights) P.plugin({ id: "lights", title: "Lighting", icon: LightsIcon, content: LightsTab, order: 20 });
    // pbosd's buttons, or face buttons and sticks: Retroid Pocket 6 / Nova, AYN Thor.
    if (lights === "pbosd" || face) P.plugin({ id: "hardware", title: "Hardware", icon: HardwareIcon, content: HardwareTab, order: 40 });
    if (power) P.plugin({ id: "power", title: "Power", icon: PowerIcon, content: PowerTab, order: 50 });
})().catch((e) => P.fail("plugins", e));

// While Steam runs (as the Decky plugin did while loaded): Proton PB-OS
// follows Valve's picks, and the backend's news shows as toasts.
// The running game, for its performance mode: told now (Steam just loaded)
// and on every start and end. After an end, Steam's running-app list takes a
// moment to drop the game.
function reportGame(appid) { gameRunning(appid != null ? String(appid) : (runningGame() || "")).catch(() => {}); }
reportGame();
protonSoon(null);
protonEvents = [
    SteamClient.GameSessions.RegisterForAppLifetimeNotifications((n) => {
        if (n && n.bRunning) reportGame(n.unAppID);
        else setTimeout(() => reportGame(), 1000);
    }),
    SteamClient.Apps.RegisterForAppOverviewChanges(() => protonSoon(null)),
    SteamClient.Apps.RegisterForGameActionStart((_action, appid) => protonSoon([String(appid)])),
];
api.addEventListener("pbos_update_available", onAvailable);
api.addEventListener("pbos_mode", onMode);
api.addEventListener("pbos_local_available", onLocal);
api.addEventListener("pbos_move_done", onMoveDone);
// A game profile saved in Barry Launcher's quick settings (AYN Thor; the
// backend saved it): the game's launch options are Steam's, so the
// profile's wrapper goes in or comes out here.
api.addEventListener("barry_game_profile", (appid, on) => {
    setWrapper(String(appid), !!on).catch((e) => P.fail("barry game profile", e));
});
});

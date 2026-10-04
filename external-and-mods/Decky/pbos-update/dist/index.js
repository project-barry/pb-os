const manifest = {"name":"PB-OS Update"};
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
const { useEffect, useState, useCallback } = SP_REACT;
const jsx = SP_JSX.jsx;
const jsxs = SP_JSX.jsxs;

// Decky callable(): arguments are passed positionally to the Python method.
const getState = callable("get_state");
const check = callable("check");
const start = callable("start");
const pause = callable("pause");
const restart = callable("restart");

const row = (child) => jsx(DFL.PanelSectionRow, { children: child });
const note = (text) => row(jsx("div", { style: { fontSize: "12px", opacity: 0.75 }, children: text }));
const gb = (n) => (n >= 1e9 ? `${(n / 1e9).toFixed(1)} GB` : `${Math.max(1, Math.round(n / 1e6))} MB`);

function Content() {
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
        items.push(row(jsx("div", { children: (job.step || "Starting") + "…" })));
        if (job.total) {
            const pct = Math.min(100, (100 * job.have) / job.total);
            // Own bar: DFL.ProgressBarWithInfo lays out as a Field, with the
            // bar in the value column, and ran off the panel's right edge.
            items.push(row(jsxs("div", { style: { width: "100%" }, children: [
                jsx("div", { style: { fontSize: "12px", opacity: 0.85, marginBottom: "6px" },
                    children: downloading ? `${gb(job.have)} of ${gb(job.total)} · ${Math.floor(pct)}%` : `${Math.floor(pct)}%` }),
                jsx("div", { style: { width: "100%", height: "8px", borderRadius: "4px", background: "rgba(255,255,255,0.15)", overflow: "hidden" },
                    children: jsx("div", { style: { width: `${pct}%`, height: "100%", background: "#1a9fff" } }) }),
            ] })));
        }
        if (downloading) {
            items.push(row(jsx(DFL.ButtonItem, {
                layout: "below",
                onClick: () => pause().then(refresh),
                children: "Pause download",
            })));
            items.push(note("A paused or interrupted download continues where it stopped."));
        } else {
            items.push(note("Getting the update ready. Games keep running; you can close this menu."));
        }
    } else {
        if (job.error) items.push(row(jsx("div", { style: { color: "#ff8080" }, children: job.error })));
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
            onClick: doCheck,
            children: "Check for updates",
        })));
    }
    return jsx(DFL.PanelSection, { title: "pb-os", children: items });
}

function onAvailable(title) {
    toaster.toast({ title: "pb-os update", body: `${title} is available. Open PB-OS Update in Quick Access.`, duration: 6000 });
}

var index = definePlugin(() => {
    api.addEventListener("pbos_update_available", onAvailable);
    return {
        name: "PB-OS Update",
        content: jsx(Content, {}),
        icon: jsx("svg", { viewBox: "0 0 24 24", width: "1em", height: "1em", fill: "currentColor",
            children: jsx("path", { d: "M11 3h2v10.2l3.6-3.6 1.4 1.4-6 6-6-6 1.4-1.4 3.6 3.6zM4 19h16v2H4z" }) }),
        alwaysRender: false,
        onDismount() {
            api.removeEventListener("pbos_update_available", onAvailable);
        },
    };
});

export { index as default };

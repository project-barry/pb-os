const manifest = {"name":"Stick Calibration"};
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
const measureRest = callable("measure_rest");
const startEdges = callable("start_edges");
const getLive = callable("get_live");
const cancel = callable("cancel");
const save = callable("save");
const setEnabled = callable("set_enabled");
const deleteCal = callable("delete");

const row = (child) => jsx(DFL.PanelSectionRow, { children: child });
const note = (text) => row(jsx("div", { style: { fontSize: "12px", opacity: 0.75 }, children: text }));
const warn = (text) => row(jsx("div", { style: { fontSize: "12px", color: "#ffb070" }, children: text }));
const failure = (text) => row(jsx("div", { style: { color: "#ff8080" }, children: text }));

const EXPERIMENTAL = "Experimental, for testing only. Your calibration lasts until you turn it off "
    + "or restart; after a restart the sticks are back to PB-OS's normal settings. "
    + "If it works well, a later PB-OS release will keep it on for good.";
const RECONNECT = "Turning it on or off restarts InputPlumber: the controller drops out for a second.";

// Each direction of each axis has to reach this far (the helper's MIN_HALF)
// before Save is offered.
const MIN_HALF = 600;
const STICKS = [
    { label: "Left stick", x: "left_x", y: "left_y" },
    { label: "Right stick", x: "right_x", y: "right_y" },
];

function reached(live, rest, axis) {
    if (!live || !rest) return [0, 0];
    return [rest[axis] - live.lo[axis], live.hi[axis] - rest[axis]];
}

function ready(live, rest) {
    return ["left_x", "left_y", "right_x", "right_y"].every((a) => {
        const [neg, pos] = reached(live, rest, a);
        return neg >= MIN_HALF && pos >= MIN_HALF;
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
        const [neg, pos] = reached(live, rest, axis);
        const ok = neg >= MIN_HALF && pos >= MIN_HALF;
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
        const t = setInterval(() => getLive().then((l) => { if (l) setLive(l); }).catch(() => {}), 100);
        return () => clearInterval(t);
    }, [step]);

    useEffect(() => () => { if (!finished.current) cancel().catch(() => {}); }, []);

    const close = () => { closeModal(); };
    const begin = async () => {
        setBusy(true);
        setError("");
        try {
            const r = await measureRest();
            if (!r.ok) { setError(r.error); return; }
            setRest(r.rest);
            setLive(null);
            const s = await startEdges();
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
            const r = await save();
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
        const ok = ready(live, rest);
        body = [
            text("Push each stick to the edge and rotate it slowly a few times. "
                + "When the numbers stop changing, press Save."),
            jsx("div", { style: { display: "flex", justifyContent: "space-around", margin: "8px 0" },
                children: STICKS.map((s) => jsx(StickView, { ...s, live, rest }, s.label)) }),
            !ok && jsx("div", { style: { fontSize: "12px", opacity: 0.75 },
                children: `Each number turns green once that direction has gone far enough (${MIN_HALF}).` }),
            error && jsx("div", { style: { color: "#ff8080" }, children: error }),
            buttons(button(busy ? "Saving…" : "Save", finish, !ok), button("Cancel", close)),
        ];
    } else {
        body = [
            text("Saved and turned on. The controller reconnects in a second."),
            text("Try the sticks in Steam's controller test or a game. Turn the switch off to go back."),
            buttons(button("Close", close)),
        ];
    }
    return jsx(DFL.ModalRoot, { onCancel: close, closeModal, children: jsxs("div", { children: [
        jsx("div", { style: { fontSize: "20px", fontWeight: 600, marginBottom: "8px" }, children: "Stick calibration" }),
        jsx("div", { style: { fontSize: "12px", color: "#ffb070", marginBottom: "12px" }, children: EXPERIMENTAL }),
        ...body.filter(Boolean),
    ] }) });
}

function Content() {
    const [st, setSt] = useState(null);
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState("");

    const refresh = useCallback(() => getState().then(setSt).catch((e) => setError(String(e))), []);
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

    if (!st) return jsx(DFL.PanelSection, { children: note("Loading…") });
    const items = [warn(EXPERIMENTAL)];
    if (!st.thor) {
        items.push(note("This is for the AYN Thor only."));
        return jsx(DFL.PanelSection, { children: items });
    }
    if (!st.pad) items.push(failure("The controller (rsinput) isn't there."));
    items.push(row(jsx(DFL.ToggleField, {
        label: "Use my calibration",
        description: st.saved ? (st.active ? "On until you turn it off or restart." : "Off: PB-OS's normal stick settings.")
            : "Calibrate the sticks first.",
        checked: st.active,
        disabled: busy || !st.saved || !st.pad,
        onChange: (on) => run(() => setEnabled(on)),
    })));
    items.push(row(jsx(DFL.ButtonItem, {
        layout: "below",
        disabled: busy || !st.pad,
        onClick: () => DFL.showModal(jsx(CalibrateModal, { onDone: () => {
            refresh();
            toaster.toast({ title: "Stick Calibration", body: "Calibration saved and on until restart" });
        } })),
        children: st.saved ? "Calibrate again" : "Calibrate sticks",
    })));
    if (st.saved) {
        const a = st.saved.axes;
        const line = (label, x, y) => note(`${label}: rest ${a[x].rest > 0 ? "+" : ""}${a[x].rest} / ${a[y].rest > 0 ? "+" : ""}${a[y].rest}, `
            + `travel ±${a[x].half} / ±${a[y].half}`);
        items.push(line("Left stick", "left_x", "left_y"));
        items.push(line("Right stick", "right_x", "right_y"));
        items.push(row(jsx(DFL.ButtonItem, {
            layout: "below",
            disabled: busy,
            onClick: () => DFL.showModal(jsx(DFL.ConfirmModal, {
                strTitle: "Delete calibration?",
                strDescription: "This turns it off and forgets the measured values.",
                strOKButtonText: "Delete",
                onOK: () => run(deleteCal),
            })),
            children: "Delete calibration",
        })));
    }
    items.push(note(RECONNECT));
    if (error) items.push(failure(error));
    return jsx(DFL.PanelSection, { children: items });
}

// A stick seen from above: the base ring and the cap.
const StickIcon = () => jsxs("svg", { viewBox: "0 0 24 24", width: "1em", height: "1em", fill: "currentColor", children: [
    jsx("circle", { cx: 12, cy: 12, r: 10, fill: "none", stroke: "currentColor", strokeWidth: 2 }, "ring"),
    jsx("circle", { cx: 12, cy: 12, r: 5 }, "cap"),
] });

var index = definePlugin(() => ({
    name: "Stick Calibration",
    titleView: jsx("div", { className: DFL.staticClasses && DFL.staticClasses.Title, children: "Stick Calibration" }),
    content: jsx(Content, {}),
    icon: jsx(StickIcon, {}),
}));

export { index as default };

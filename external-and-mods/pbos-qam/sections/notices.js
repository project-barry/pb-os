// Notices: messages once Steam has started, each shown once
// (daemon/notices.cpp decides).
// The first: an older boot menu PB-OS knows (the PB-OS fork's OpenABL 1.2,
// OpenABL 1.0/1.1, ROCKNIX ABL) instead of the OpenABL release PB-OS ships. An
// optional update, offered here once for each release and, until it is done, on
// the PB-OS tab's System page. The update runs in Game Mode.
// The other: on every device, when there is no boot menu notice to show, a short
// ask to run PB-OS Sound Check if the sound goes.
window.__pbosQam.section("notices", (P) => {
const { DFL, SP_REACT, SP_JSX, callable } = P;
const jsx = SP_JSX.jsx;
const jsxs = SP_JSX.jsxs;

const get = callable("get");
const offer = callable("offer");
const update = callable("update");
const seen = callable("seen");

const line = (text, key) => jsx("div", { style: { marginTop: "8px" }, children: text }, key);
const lines = (items) => jsxs("div", { children: items.map((t, i) => (i === 0 ? jsx("div", { children: t }, "0") : line(t, String(i)))) });

// The shipped release's name, e.g. "OpenABL-PB 1.2".
const shipped = (n) => n.version || "OpenABL";
const message = (title, text) => DFL.showModal(jsx(DFL.ConfirmModal, { strTitle: title, strDescription: text, bAlertDialog: true }));

async function runUpdate(n) {
    const busy = DFL.showModal(jsx(DFL.ConfirmModal, {
        strTitle: "Updating the boot menu",
        strDescription: "Don't turn the device off. This takes a few seconds.",
        bAlertDialog: true, bOKDisabled: true, bHideCloseIcon: true, bDisableBackgroundDismiss: true,
    }));
    try {
        await update();
        busy.Close();
        P.unplug("bootmenu");
        message("Boot menu updated", `${shipped(n)} is on both slots. A backup of the old boot menu is in /home/.pbos-abl-backup.`);
    } catch (e) {
        busy.Close();
        message("Boot menu not updated", String((e && e.message) || e));
    }
}

// A second, deliberate step before anything is written.
const confirmUpdate = (n) => DFL.showModal(jsx(DFL.ConfirmModal, {
    strTitle: "Update the boot menu?",
    strDescription: lines([
        `PB-OS writes ${shipped(n)} to both boot menu slots, one at a time, after backing up both.`,
        "Keep the device on the charger (or above 30% battery) and don't turn it off until it says it's done.",
    ]),
    strOKButtonText: "Update",
    bDestructiveWarning: true,
    onOK: () => runUpdate(n),
}));

function soundCheckHelp(n) {
    seen(n.id).catch((e) => P.fail("notices", e));
    DFL.showModal(jsx(DFL.ConfirmModal, {
        strTitle: "Help! There is an intermittent Audio Bug in PB-OS",
        strDescription: lines([
            "Your mission, if you choose to accept it, is to help us track it down.",
            "If your sound ever cuts unexpectedly, please do us a favor and run the PB-OS Sound Check in Non-Steam Games "
                + "before rebooting or switching to Desktop mode. This will send us logging data that will help us resolve the problem.",
            "xoxo - lavachemist and doctor73n",
        ]),
        strOKButtonText: "Got it",
        bAlertDialog: true,
    }));
}

// What the update is. "Nothing is broken" is only said of the fork's 1.2: the
// older menus lack its fix for the freeze on the logo.
const about = (n) => [
    `${shipped(n)} is PB-OS's own build of the OpenABL boot menu, the same for every device PB-OS runs on.`,
    n.menu === "OpenABL 1.2 (PB-OS fork)"
        ? `This update is optional. Nothing is broken: your ${n.device || "device"} has ${n.menu}, which keeps working.`
        : `This update is optional: your ${n.device || "device"} has ${n.menu}, which keeps working with PB-OS.`,
];

function offerUpdate(n) {
    seen(n.id).catch((e) => P.fail("notices", e));
    DFL.showModal(jsx(DFL.ConfirmModal, {
        strTitle: "Optional boot menu update",
        strDescription: lines([...about(n), "You can do it later too: Quick Access › PB-OS › System."]),
        strOKButtonText: "Update",
        strCancelButtonText: "Not Now",
        onOK: () => confirmUpdate(n),
    }));
}

// An arrow onto a tray.
const BootMenuIcon = () => jsx("svg", { viewBox: "0 0 24 24", width: "22", height: "22", fill: "currentColor",
    children: jsx("path", { d: "M11 3h2v8h3l-4 5-4-5h3zM4 18h16v3H4z" }) });

// System page, only while there is an update.
function BootMenuPart() {
    const [n, setN] = SP_REACT.useState(null);
    SP_REACT.useEffect(() => { offer().then(setN).catch((e) => P.fail("notices", e)); }, []);
    if (!n || !n.canUpdate) return null;
    const row = (child, key) => jsx(DFL.PanelSectionRow, { children: child }, key);
    return jsxs(DFL.PanelSection, { title: "Boot Menu", children: [
        row(jsx("div", { style: { fontSize: "12px", opacity: 0.75 }, children: lines(about(n)) }), "about"),
        row(jsx(DFL.ButtonItem, { layout: "below", onClick: () => confirmUpdate(n), children: `Update to ${shipped(n)}` }), "update"),
    ] });
}

offer().then((n) => {
    if (n && n.canUpdate) P.plugin({ id: "bootmenu", page: "system", icon: BootMenuIcon, content: BootMenuPart, order: 50 });
}).catch((e) => P.fail("notices", e));

get().then((n) => {
    if (n && n.id === "sound-check-help") return soundCheckHelp(n);
    if (n && n.id && n.canUpdate) offerUpdate(n);
}).catch((e) => P.fail("notices", e));
});

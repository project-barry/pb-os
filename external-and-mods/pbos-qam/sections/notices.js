// Notices: a message once Steam has started, for something the user should
// fix (daemon/notices.cpp decides; it has no page in the tab).
// The one notice: an older boot menu PB-OS knows (OpenABL 1.0/1.1, ROCKNIX
// ABL) instead of the OpenABL release PB-OS ships, which fixes the freeze on
// the logo after a restart. The fix runs here, in Game Mode. It shows at every
// boot and Steam restart until the boot menu is updated.
// The other: on every device, only when there is no boot menu notice, a short
// ask to run PB-OS Sound Check if the sound goes. Shown once.
window.__pbosQam.section("notices", (P) => {
const { DFL, SP_JSX, callable } = P;
const jsx = SP_JSX.jsx;
const jsxs = SP_JSX.jsxs;

const get = callable("get");
const update = callable("update");
const seen = callable("seen");

const line = (text, key) => jsx("div", { style: { marginTop: "8px" }, children: text }, key);
const lines = (items) => jsxs("div", { children: items.map((t, i) => (i === 0 ? jsx("div", { children: t }, "0") : line(t, String(i)))) });

const version = (n) => n.version || "1.2";
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
        message("Boot menu updated", `OpenABL ${version(n)} is on both slots. A backup of the old boot menu is in /home/.pbos-abl-backup.`);
    } catch (e) {
        busy.Close();
        message("Boot menu not updated", String((e && e.message) || e));
    }
}

// A second, deliberate step before anything is written.
const confirmUpdate = (n) => DFL.showModal(jsx(DFL.ConfirmModal, {
    strTitle: "Update the boot menu?",
    strDescription: lines([
        `PB-OS writes OpenABL ${version(n)} to both boot menu slots, one at a time, after backing up both.`,
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

get().then((n) => {
    if (n && n.id === "sound-check-help") return soundCheckHelp(n);
    if (!n || !n.id || !n.canUpdate) return;
    DFL.showModal(jsx(DFL.ConfirmModal, {
        strTitle: "Alert: You should update your boot loader!",
        strDescription: lines([
            `Your ${n.device || "device"} is running ${n.menu}.`,
            `Update to OpenABL ${version(n)} to avoid a bug that causes boot instability.`,
        ]),
        strOKButtonText: "Update Now",
        strCancelButtonText: "Close",
        onOK: () => confirmUpdate(n),
    }));
}).catch((e) => P.fail("notices", e));
});

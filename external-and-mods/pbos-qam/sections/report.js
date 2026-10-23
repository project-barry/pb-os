// Report a Problem (System page): the user picks what went wrong and saves a
// report right then, from Game Mode, before restarting. daemon/report.cpp
// runs pbos-report and saves the file in ~/Desktop/PB-OS Reports; nothing
// is sent anywhere.
window.__pbosQam.section("report", (P) => {
const { DFL, SP_REACT, SP_JSX, callable } = P;
const { useState } = SP_REACT;
const jsx = SP_JSX.jsx;

const make = callable("make");

const AREAS = [
    { data: "audio", label: "Sound", desc: "No sound, sound cutting out, the wrong output." },
    { data: "sleep", label: "Sleep and wake", desc: "Won't sleep, won't wake, battery drained while asleep." },
    { data: "controller", label: "Controls", desc: "Buttons, sticks, touch or a controller not working." },
    { data: "display", label: "Screen", desc: "Black screen, flicker, brightness, a TV or monitor." },
    { data: "wifi", label: "Wi-Fi", desc: "Not connecting, dropping, slow." },
    { data: "other", label: "Something else", desc: "Anything not listed." },
];

const row = (child) => jsx(DFL.PanelSectionRow, { children: child });
const note = (text) => row(jsx("div", { style: { fontSize: "12px", opacity: 0.75 }, children: text }));
const failure = (text) => row(jsx("div", { style: { color: "#ff8080" }, children: text }));

// A speech bubble with an exclamation mark.
const ReportIcon = () => jsx("svg", { viewBox: "0 0 24 24", width: "22", height: "22", fill: "currentColor",
    children: jsx("path", { d: "M4 3h16a1 1 0 0 1 1 1v12a1 1 0 0 1-1 1H8l-5 4V4a1 1 0 0 1 1-1zm7 3v6h2V6zm0 8v2h2v-2z" }) });

// The pick outlives the panel remount a dropdown's list causes.
const picks = { area: "audio" };

function ReportPart() {
    const [area, setAreaState] = useState(picks.area);
    const setArea = (v) => { picks.area = v; setAreaState(v); };
    const [busy, setBusy] = useState(false);
    const [saved, setSaved] = useState(null);
    const [error, setError] = useState("");
    const choice = AREAS.find((a) => a.data === area) || AREAS[0];

    const save = () => {
        setBusy(true);
        setSaved(null);
        setError("");
        make(area)
            .then((r) => setSaved(r))
            .catch((e) => setError(String((e && e.message) || e)))
            .finally(() => setBusy(false));
    };

    return jsx(DFL.PanelSection, { title: "Report a Problem", children: [
        note("Save a report right when something goes wrong, before restarting: it keeps what the device was doing."),
        row(jsx(DFL.DropdownItem, {
            label: "What went wrong",
            description: choice.desc,
            rgOptions: AREAS.map((a) => ({ data: a.data, label: a.label })),
            selectedOption: area,
            onChange: (o) => setArea(o.data),
        })),
        row(jsx(DFL.ButtonItem, { layout: "below", disabled: busy, onClick: save,
            children: busy ? "Saving the report…" : "Save Report" })),
        saved ? note(`Saved: ${saved.path.replace(/^\/home\/[^/]+\//, "~/")}. Send this file to the PB-OS team with what happened.`) : null,
        error ? failure(error) : null,
        note("Reports stay on this device. They hold system logs, which can include Wi-Fi network names: look before posting one publicly."),
    ] });
}

P.plugin({ id: "report", page: "system", icon: ReportIcon, content: ReportPart, order: 40 });
});

// shell/AppUpdates.qml (run.sh copies it next to this file): what shows in
// each phase of shelld's updates topic, and which answer each button sends.
import QtQuick
import QtTest

Item {
    id: root
    width: 1240
    height: 1080

    AppUpdates {
        id: card
        anchors.fill: parent
    }
    SignalSpy { id: answers; target: card; signalName: "answer" }

    function apps(status, error) {
        return [{ id: "io.github.project-barry.pipboy", name: "Pip-Boy", installed: "1.0.0", version: "1.1.0",
                  status: status, error: error || "" },
                { id: "io.github.project-barry.dice", name: "Dice", installed: "1.0.0", version: "1.0.1",
                  status: status === "failed" ? "updated" : status, error: "" }]
    }

    TestCase {
        name: "AppUpdates"
        when: windowShown

        function init() { answers.clear() }

        function shown(name) {
            const b = findChild(card, name)
            verify(b !== null, name)
            return b.visible
        }

        function tap(name) {
            waitForRendering(card)  // the buttons' row laid out for this phase
            const b = findChild(card, name)
            verify(b && b.visible, name + " shown")
            mouseClick(b)
        }

        function test_idle_hidden() {
            card.updates = null
            verify(!card.visible)
            card.updates = { serial: 0, phase: "idle", apps: [] }
            verify(!card.visible)
        }

        function test_offer_update() {
            card.updates = { serial: 1, phase: "offer", apps: apps("") }
            verify(card.visible)
            verify(shown("update") && shown("later") && !shown("close"))
            tap("update")
            compare(answers.count, 1)
            compare(answers.signalArguments[0][0], "update")
        }

        function test_offer_not_now() {
            card.updates = { serial: 1, phase: "offer", apps: apps("") }
            tap("later")
            compare(answers.signalArguments[0][0], "later")
        }

        function test_updating_no_buttons() {
            card.updates = { serial: 1, phase: "updating", apps: apps("updating") }
            verify(card.visible)
            verify(!shown("update") && !shown("later") && !shown("close"))
            mouseClick(card)  // the cover takes it: no answer
            compare(answers.count, 0)
        }

        function test_done_close() {
            card.updates = { serial: 1, phase: "done", apps: apps("failed", "no room") }
            verify(card.visible && shown("close") && !shown("update"))
            tap("close")
            compare(answers.signalArguments[0][0], "close")
        }
    }
}

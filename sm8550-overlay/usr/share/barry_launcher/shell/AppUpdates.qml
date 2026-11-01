pragma ComponentBehavior: Bound
// New versions of Project Barry's apps the user has (barry_launcher_shelld's
// updates topic; barry_updates): a card over Barry's pages, the first time
// Barry shows after a PB-OS update found some. It lists the apps with their
// versions, and Update or Not now; updating, how each one goes, then Done.
// Nothing downloads without the Update tap; Not now asks again only after
// the next PB-OS update. Shell.qml shows it and sends the answers (answer:
// "update", "later", "close"). Colors are the skin's; a skin can also
// replace this file (SkinView.qml), keeping these properties and signals.
import QtQuick
import Barry.Input

Item {
    id: card
    property real s: 1
    // The updates topic as shelld says it (phase, apps).
    property var updates: null
    signal answer(string what)

    readonly property string phase: updates && typeof updates.phase === "string" ? updates.phase : "idle"
    readonly property var apps: updates && Array.isArray(updates.apps) ? updates.apps : []
    visible: phase === "offer" || phase === "updating" || phase === "done"

    component Button: Rectangle {
        id: button
        property string label
        property bool primary: false
        signal tapped
        width: Math.max(260 * card.s, label_.implicitWidth + 80 * card.s)
        height: 100 * card.s
        radius: height / 2
        color: tap.pressed ? Skin.colors.pressed : button.primary ? Skin.colors.accent : Skin.colors.raised
        Text {
            id: label_
            anchors.centerIn: parent
            text: button.label
            color: Skin.colors.text
            font { family: Skin.font; pixelSize: 36 * card.s; weight: Font.DemiBold }
        }
        TapHandler {
            id: tap
            gesturePolicy: TapHandler.ReleaseWithinBounds
            onTapped: button.tapped()
        }
    }

    // Under the card, Barry's pages take no touches.
    Rectangle {
        anchors.fill: parent
        color: Skin.colors.cover
        opacity: 0.6
        MouseArea { anchors.fill: parent }
    }

    Rectangle {
        id: box
        anchors.centerIn: parent
        width: Math.min(parent.width - 80 * card.s, 900 * card.s)
        height: column.height + 80 * card.s
        radius: 40 * card.s
        color: Skin.colors.panel
        border.color: Skin.colors.border
        border.width: 3 * card.s

        Column {
            id: column
            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 40 * card.s }
            spacing: 26 * card.s

            Text {
                width: parent.width
                wrapMode: Text.WordWrap
                text: card.phase === "offer" ? (card.apps.length === 1 ? "A new version of your app"
                                                                     : "New versions of your apps")
                      : card.phase === "updating" ? "Updating…" : "Updates"
                color: Skin.colors.text
                font { family: Skin.font; pixelSize: 44 * card.s; weight: Font.DemiBold }
            }

            Repeater {
                model: card.apps
                delegate: Column {
                    id: row
                    required property var modelData
                    width: column.width
                    spacing: 4 * card.s
                    Text {
                        width: parent.width
                        elide: Text.ElideRight
                        text: row.modelData.name + "   " + row.modelData.installed + " → " + row.modelData.version
                        color: Skin.colors.text
                        font { family: Skin.font; pixelSize: 36 * card.s }
                    }
                    Text {
                        width: parent.width
                        visible: text !== ""
                        wrapMode: Text.WordWrap
                        text: row.modelData.status === "updating" ? "Downloading and installing…"
                              : row.modelData.status === "updated" ? "Updated"
                              : row.modelData.status === "failed" ? "Not updated, " + row.modelData.installed
                                                                    + " kept: " + row.modelData.error
                              : ""
                        color: row.modelData.status === "failed" ? Skin.colors.text : Skin.colors.accent
                        opacity: row.modelData.status === "failed" ? 0.75 : 1
                        font { family: Skin.font; pixelSize: 28 * card.s }
                    }
                }
            }

            Row {
                anchors.right: parent.right
                spacing: 30 * card.s
                visible: card.phase !== "updating"

                Button {
                    visible: card.phase === "offer"
                    objectName: "later"
                    label: "Not now"
                    onTapped: card.answer("later")
                }
                Button {
                    visible: card.phase === "offer"
                    primary: true
                    objectName: "update"
                    label: "Update"
                    onTapped: card.answer("update")
                }
                Button {
                    visible: card.phase === "done"
                    primary: true
                    objectName: "close"
                    label: "Done"
                    onTapped: card.answer("close")
                }
            }
        }
    }
}

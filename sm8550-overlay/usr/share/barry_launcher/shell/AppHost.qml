// A user app (barry_apps) in a full-screen window of its own, as Barry
// Launcher's own QML apps are: barry_launcher_shelld starts this with the
// app's folder, main QML file, id, name and data folder after "--", then
// "fullscreen" ("windowed" for barry-app run: a window the bottom screen's
// size), then its service's address and token if it has one. The
// app's main.qml has an Item (or Rectangle) at its root, which fills the
// window. If that root declares `required property var barry`, it gets
// this as it is made (before Component.onCompleted, so bindings can use it):
//
//   barry.appId, barry.appDir, barry.dataDir  ids and folders ("file://" URLs
//                                              for the folders: appDirUrl,
//                                              dataDirUrl)
//   barry.scale                 1 at the AYN Thor's bottom screen (1240 x 1080)
//   barry.close()               quit; the bottom screen shows what it showed before
//   barry.serviceUrl            the app's service ("http://127.0.0.1:PORT/"), or ""
//   barry.serviceToken          for an Image source from it: append "?token=" + this
//   barry.request(method, path, body, done)
//                               call the service: body (an object) goes as
//                               JSON; done(status, reply) gets the reply's
//                               JSON (null if none), status 0 if no answer
//
// Touch is Barry Launcher's: an app scrolls with Barry.Input's TouchScroll
// and pans and zooms with its PanZoomArea (`import Barry.Input`), never with
// touch handling of its own (Barry.Input's GESTURES.md).
//
// A main.qml that fails to load shows why, here, instead of a black screen.
import QtQuick
import QtQuick.Window

Window {
    id: win
    readonly property var args: {
        const a = Qt.application.arguments
        const i = a.indexOf("--")
        return i >= 0 ? a.slice(i + 1) : []
    }
    readonly property string appDir: args[0] || ""
    readonly property string mainFile: args[1] || "main.qml"
    readonly property string appId: args[2] || ""
    readonly property string appName: args[3] || appId
    readonly property string dataDir: args[4] || ""
    readonly property bool windowed: args[5] === "windowed"
    readonly property string serviceUrl: args[6] || ""
    readonly property string serviceToken: args[7] || ""

    // A folder's file:// URL, with a slash at the end; Windows paths too
    // (barry-app run on a PC).
    function folderUrl(path) {
        const p = path.replace(/\\/g, "/")
        return (p.startsWith("/") ? "file://" : "file:///") + p + (p.endsWith("/") ? "" : "/")
    }

    title: "Barry App " + appId
    color: "black"
    width: 1240
    height: 1080
    visibility: windowed ? Window.Windowed : Window.FullScreen
    visible: true

    QtObject {
        id: barry
        readonly property string appId: win.appId
        readonly property string appName: win.appName
        readonly property string appDir: win.appDir
        readonly property string dataDir: win.dataDir
        readonly property url appDirUrl: win.folderUrl(win.appDir)
        readonly property url dataDirUrl: win.folderUrl(win.dataDir)
        readonly property real scale: Math.min(win.width / 1240, win.height / 1080)
        readonly property string serviceUrl: win.serviceUrl
        readonly property string serviceToken: win.serviceToken
        function close() { Qt.quit() }
        function request(method, path, body, done) {
            const xhr = new XMLHttpRequest()
            xhr.onreadystatechange = function() {
                if (xhr.readyState !== XMLHttpRequest.DONE || !done)
                    return
                let reply = null
                try { reply = JSON.parse(xhr.responseText) } catch (e) {}
                done(xhr.status, reply)
            }
            if (!win.serviceUrl) {
                if (done)
                    Qt.callLater(done, 0, null)
                return
            }
            xhr.open(method, win.serviceUrl + path.replace(/^\//, ""))
            xhr.setRequestHeader("X-Barry-Token", win.serviceToken)
            if (body !== undefined && body !== null) {
                xhr.setRequestHeader("Content-Type", "application/json")
                xhr.send(JSON.stringify(body))
            } else {
                xhr.send()
            }
        }
    }

    Loader {
        id: app
        anchors.fill: parent
        focus: true
        Component.onCompleted: setSource(win.folderUrl(win.appDir) + win.mainFile, { barry: barry })
    }

    // Why the app did not load.
    Rectangle {
        anchors.fill: parent
        visible: app.status === Loader.Error
        color: "#101117"
        Column {
            anchors.centerIn: parent
            width: parent.width * 0.85
            spacing: 24 * barry.scale
            Text {
                width: parent.width
                text: win.appName + " could not start"
                color: "#eef0f4"
                font { family: "Noto Sans"; pixelSize: 44 * barry.scale; weight: Font.DemiBold }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "Its main QML file has an error. The details are in ~/.cache/barry_launcher/"
                      + win.appId + ".log."
                color: "#eef0f4"
                opacity: 0.7
                font { family: "Noto Sans"; pixelSize: 28 * barry.scale }
            }
            Rectangle {
                width: 320 * barry.scale
                height: 96 * barry.scale
                radius: 24 * barry.scale
                color: closeTap.pressed ? "#4a4f60" : "#1b1d24"
                border.color: "#3a3e4d"
                border.width: 2 * barry.scale
                Text {
                    anchors.centerIn: parent
                    text: "Close"
                    color: "#eef0f4"
                    font { family: "Noto Sans"; pixelSize: 32 * barry.scale; weight: Font.DemiBold }
                }
                TapHandler {
                    id: closeTap
                    onTapped: Qt.quit()
                }
            }
        }
    }
}

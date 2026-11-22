import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import P2PCall

ApplicationWindow {
    id: win
    width: 960
    height: 640
    visible: true
    title: "P2P Call — " + call.localName + " (port " + call.listenPort + ")"

    readonly property bool idle: call.state === "idle"
    readonly property bool inCall: call.state === "in-call"

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            Label {
                text: call.localName
                font.bold: true
            }
            Label {
                text: "· " + call.state
                opacity: 0.7
            }
            Item { Layout.fillWidth: true }
            Label {
                visible: call.sas !== ""
                text: "Verify code: " + call.sas
                font.bold: true
                font.family: "monospace"
            }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        // ---------------------------------------------------- left: peers
        ColumnLayout {
            Layout.preferredWidth: 300
            Layout.fillHeight: true
            spacing: 8

            Label { text: "Peers on this network"; font.bold: true }
            Frame {
                Layout.fillWidth: true
                Layout.fillHeight: true
                ListView {
                    id: peerList
                    anchors.fill: parent
                    clip: true
                    model: call.peers
                    delegate: ItemDelegate {
                        width: ListView.view.width
                        text: modelData.name + "\n" + modelData.host + ":" + modelData.port
                        enabled: win.idle
                        onClicked: call.callPeer(modelData.id)
                    }
                    Label {
                        anchors.centerIn: parent
                        visible: peerList.count === 0
                        text: "No peers found yet"
                        opacity: 0.6
                    }
                }
            }

            Label { text: "Call by address"; font.bold: true }
            RowLayout {
                Layout.fillWidth: true
                TextField {
                    id: addr
                    Layout.fillWidth: true
                    placeholderText: "127.0.0.1:47101"
                    enabled: win.idle
                    onAccepted: call.callAddress(text)
                }
                Button {
                    text: "Call"
                    enabled: win.idle && addr.text.length > 0
                    onClicked: call.callAddress(addr.text)
                }
            }

            Label { text: "Microphone processing"; font.bold: true }
            CheckBox {
                text: "Echo cancellation"
                checked: call.echoCancel
                onToggled: call.echoCancel = checked
            }
            RowLayout {
                CheckBox {
                    text: "Noise suppression"
                    checked: call.noiseSuppression
                    onToggled: call.noiseSuppression = checked
                }
                ComboBox {
                    enabled: call.noiseSuppression
                    model: ["Low", "Moderate", "High"]
                    currentIndex: call.noiseLevel
                    onActivated: (i) => call.noiseLevel = i
                }
            }
            CheckBox {
                text: "Automatic gain"
                checked: call.autoGain
                onToggled: call.autoGain = checked
            }

            Label { text: "Codecs"; font.bold: true }
            CheckBox {
                text: "Prefer AV1 for camera (software)"
                checked: call.preferAv1Camera
                enabled: win.idle
                onToggled: call.preferAv1Camera = checked
            }
            CheckBox {
                text: "Prefer AV1 for screen share"
                checked: call.preferAv1Screen
                enabled: win.idle
                onToggled: call.preferAv1Screen = checked
            }
        }

        // ---------------------------------------------------- right: call
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 8

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#111"
                radius: 6
                clip: true

                // The peer's shared screen takes the stage while it arrives;
                // otherwise their camera does. Status text shows until either.
                readonly property bool screenOnStage: win.inCall && call.remoteSharingScreen && remoteScreen.hasVideo

                VideoView {
                    id: remoteScreen
                    anchors.fill: parent
                    stream: "remote-screen"
                    visible: parent.screenOnStage
                }

                // Remote camera: full stage, or a tile in the top-right corner
                // while a screen is on the stage.
                Rectangle {
                    id: remoteCameraBox
                    readonly property bool tile: parent.screenOnStage
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: tile ? 10 : 0
                    width: tile ? Math.min(parent.width * 0.22, 280) : parent.width
                    height: tile ? width * 9 / 16 : parent.height
                    color: tile ? "black" : "transparent"
                    border.color: tile ? "#555" : "transparent"
                    visible: win.inCall && remoteVideo.hasVideo
                    z: 1
                    VideoView {
                        id: remoteVideo
                        anchors.fill: parent
                        anchors.margins: remoteCameraBox.tile ? 1 : 0
                        stream: "remote"
                    }
                }

                Column {
                    anchors.centerIn: parent
                    spacing: 6
                    visible: !remoteCameraBox.visible && !remoteScreen.visible
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        color: "white"
                        font.pixelSize: 22
                        text: {
                            switch (call.state) {
                            case "idle": return "Not in a call"
                            case "connecting": return "Connecting…"
                            case "handshaking": return "Securing connection…"
                            case "outgoing": return "Calling " + call.remoteName + "…"
                            case "incoming": return call.remoteName + " is calling"
                            case "in-call": return "In call with " + call.remoteName
                            }
                            return call.state
                        }
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        color: "#aaa"
                        visible: call.sas !== ""
                        text: "Check that the code " + call.sas + " matches on both screens"
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        color: "#aaa"
                        visible: win.inCall
                        text: call.micMuted ? "Microphone muted" : "Audio connected"
                    }
                }

                // Self view (mirrored, like a mirror).
                Rectangle {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.margins: 10
                    width: Math.min(parent.width * 0.25, 320)
                    height: width * 9 / 16
                    color: "black"
                    border.color: "#555"
                    visible: win.inCall && localVideo.hasVideo
                    z: 1
                    VideoView {
                        id: localVideo
                        anchors.fill: parent
                        anchors.margins: 1
                        stream: "local"
                        mirrored: true
                    }
                }

                // Stats overlay
                Rectangle {
                    anchors.fill: statsLabel
                    anchors.margins: -6
                    color: "#a0000000"
                    radius: 4
                    visible: statsLabel.visible
                }
                Label {
                    id: statsLabel
                    anchors.left: parent.left
                    anchors.top: parent.top
                    anchors.margins: 10
                    visible: !win.idle && showStats.checked
                    color: "#8f8"
                    font.family: "monospace"
                    font.pixelSize: 11
                    text: "rtt     " + (call.rttMs < 0 ? "–" : call.rttMs.toFixed(2) + " ms")
                          + "\noffset  " + call.clockOffsetMs.toFixed(2) + " ms"
                          + (call.sendSummary ? "\nsend    " + call.sendSummary : "")
                          + (call.recvSummary ? "\nrecv    " + call.recvSummary : "")
                          + (call.mediaStats ? "\n\n" + call.mediaStats : "")
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Button {
                    text: "Accept"
                    visible: call.state === "incoming"
                    highlighted: true
                    onClicked: call.accept()
                }
                Button {
                    text: "Decline"
                    visible: call.state === "incoming"
                    onClicked: call.reject()
                }
                CheckBox {
                    id: showStats
                    text: "Stats"
                    checked: true
                    visible: !win.idle
                }
                Button {
                    text: call.micMuted ? "Unmute" : "Mute"
                    visible: win.inCall
                    onClicked: call.micMuted = !call.micMuted
                }
                ComboBox {
                    id: screenMode
                    visible: win.inCall && call.canShareScreen && !call.sharingScreen
                    model: ["Text", "Motion"]
                    ToolTip.visible: hovered
                    ToolTip.text: "Text: full resolution, up to 30 fps. Motion: 1080p at 60 fps."
                }
                Button {
                    text: call.sharingScreen ? "Stop sharing (" + call.screenMode + ")" : "Share screen"
                    visible: win.inCall && call.canShareScreen
                    onClicked: call.sharingScreen ? call.stopScreenShare()
                                                  : call.startScreenShare(screenMode.currentIndex === 0 ? "text" : "motion")
                }
                Button {
                    text: "Hang up"
                    visible: !win.idle && call.state !== "incoming"
                    onClicked: call.hangup()
                }
            }

            Label {
                Layout.fillWidth: true
                visible: call.lastError !== ""
                text: call.lastError
                color: "#e55"
                wrapMode: Text.Wrap
            }

            Label { text: "Local codecs: " + call.localCapsSummary; opacity: 0.7; font.pixelSize: 12 }

            Frame {
                Layout.fillWidth: true
                Layout.preferredHeight: 140
                ScrollView {
                    anchors.fill: parent
                    TextArea {
                        readOnly: true
                        font.family: "monospace"
                        font.pixelSize: 11
                        text: call.logLines.join("\n")
                        onTextChanged: cursorPosition = length
                    }
                }
            }
        }
    }
}

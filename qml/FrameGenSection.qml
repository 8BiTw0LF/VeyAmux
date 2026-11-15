import QtQuick

// Frame generation: Off (optionally VRR pacing) / Lossless Scaling (Fixed 2x = default, Fixed 3x).
Column {
    id: root
    property var tr: function (k) { return k }
    spacing: 6

    readonly property string ls: player.lsfgState
    readonly property bool lsUsable: ls === "active" || ls === "ready"

    component Option: Rectangle {
        id: opt
        property int mode
        property string title
        property string detail
        property bool usable: true
        readonly property bool sel: player.frameGenMode === mode
        width: root.width
        height: col.implicitHeight + 14
        radius: 9
        opacity: usable ? 1 : 0.45
        color: sel ? "#33ff7a45" : (ma.containsMouse && usable ? "#22ffffff" : "#1d2028")
        border.color: sel ? "#ff7a45" : "transparent"
        Row {
            x: 10; anchors.verticalCenter: parent.verticalCenter
            spacing: 10
            Rectangle {   // radio dot
                width: 14; height: 14; radius: 7; anchors.verticalCenter: parent.verticalCenter
                color: "transparent"; border.color: opt.sel ? "#ff7a45" : "#6c7182"; border.width: 2
                Rectangle { anchors.centerIn: parent; width: 6; height: 6; radius: 3; color: "#ff7a45"; visible: opt.sel }
            }
            Column {
                id: col
                width: root.width - 44
                spacing: 2
                Text { width: parent.width; text: opt.title; color: opt.sel ? "#ffffff" : "#d6d8e0"; font.pixelSize: 13; font.weight: Font.Medium; wrapMode: Text.Wrap }
                Text { width: parent.width; text: opt.detail; visible: text !== ""; color: "#8b90a0"; font.pixelSize: 11; wrapMode: Text.Wrap }
            }
        }
        MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; enabled: opt.usable; cursorShape: Qt.PointingHandCursor
            onClicked: player.frameGenMode = opt.mode }
    }

    // an indented on/off option belonging to the selected mode
    component SubToggle: Item {
        id: st
        property string title
        property string detail
        property bool checked
        signal toggled(bool value)
        width: root.width
        height: stCol.implicitHeight + 10
        Column {
            id: stCol
            x: 34; y: 5; width: parent.width - 34 - 52; spacing: 2
            Text { width: parent.width; text: st.title; color: "#d6d8e0"; font.pixelSize: 12; font.weight: Font.Medium; wrapMode: Text.Wrap }
            Text { width: parent.width; text: st.detail; visible: text !== ""; color: "#8b90a0"; font.pixelSize: 11; wrapMode: Text.Wrap }
        }
        ToggleSwitch { anchors.right: parent.right; anchors.rightMargin: 4; y: 6; checked: st.checked; onToggled: (v) => st.toggled(v) }
    }

    component LinkButton: Rectangle {
        id: lb
        property string text
        signal clicked()
        width: lbText.implicitWidth + 22; height: 28; radius: 8
        color: lbMa.containsMouse ? "#ff8f63" : "#ff7a45"
        Text { id: lbText; anchors.centerIn: parent; text: lb.text; color: "#14110f"; font.pixelSize: 12; font.weight: Font.DemiBold }
        MouseArea { id: lbMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: lb.clicked() }
    }

    Text { text: tr("fg"); color: "#ffffff"; font.pixelSize: 14; font.weight: Font.DemiBold }

    Option { mode: 0; title: tr("fgOff") }
    SubToggle {
        visible: player.frameGenMode === 0
        title: tr("vrr"); detail: tr("vrrHint")
        checked: player.vrr; onToggled: (v) => player.vrr = v
    }
    Row {
        visible: player.vrrRestartNeeded
        x: 34; spacing: 8
        Text { text: tr("vrrRestart"); color: "#ffb89c"; font.pixelSize: 11; anchors.verticalCenter: parent.verticalCenter; width: root.width - 34 - vrrBtn.width - 12; wrapMode: Text.Wrap }
        LinkButton { id: vrrBtn; text: tr("restartNow"); onClicked: player.restartApp() }
    }

    Text { text: "Lossless Scaling"; color: "#9da2b3"; font.pixelSize: 12; font.weight: Font.Medium; topPadding: 4 }
    Option { mode: 2; usable: root.lsUsable; title: tr("lsFixed"); detail: tr("lsFixedHint") }
    Option {
        mode: 3; usable: root.lsUsable; title: tr("ls3x")
        detail: tr("ls3xHint")
    }

    SubToggle {
        visible: root.lsUsable && player.frameGenMode >= 2
        title: tr("lsPerf"); detail: tr("lsPerfHint").replace("%1", player.lsfgFlowPercent)
        checked: player.lsfgPerformance; onToggled: (v) => player.lsfgPerformance = v
    }

    // Lossless Scaling status / actions
    Rectangle {
        width: root.width
        height: lsCol.implicitHeight + 16
        radius: 9
        color: "#15171d"
        border.color: root.ls === "crashed" || (!root.lsUsable) ? "#44ff7a45" : "#22ffffff"
        Column {
            id: lsCol
            x: 10; y: 8; width: parent.width - 20; spacing: 8
            Text {
                width: parent.width; wrapMode: Text.Wrap; font.pixelSize: 11
                color: root.ls === "active" ? "#9be39b" : (root.lsUsable ? "#c9ccd6" : "#ffb89c")
                text: {
                    switch (root.ls) {
                    case "active": return tr("lsActive").replace("%1", player.lsfgFolder)
                    case "ready": return player.restartNeeded ? tr("lsRestart") : tr("lsReady").replace("%1", player.lsfgFolder)
                    case "noInstall": return tr("lsNoInstall")
                    case "noDll": return tr("lsNoDll")
                    case "noLayer": return tr("lsNoLayer")
                    case "oldLayer": return tr("lsOldLayer")
                    case "noVulkanUi": return tr("lsNoVulkan")
                    case "crashed": return tr("lsCrashed")
                    }
                    return ""
                }
            }
            Text {
                width: parent.width; wrapMode: Text.WrapAnywhere; font.pixelSize: 11
                visible: player.lsfgLog !== ""
                color: player.lsfgLog.indexOf("[ERROR]") >= 0 || player.lsfgLog.indexOf("[WARNING]") >= 0
                       || player.lsfgLog.indexOf("lsfg-vk-cli") >= 0 ? "#ffb89c" : "#9da2b3"
                text: player.lsfgLog
            }
            Row {
                spacing: 8
                LinkButton { visible: player.restartNeeded; text: tr("restartNow"); onClicked: player.restartApp() }
                LinkButton { visible: root.ls === "crashed"; text: tr("tryAgain"); onClicked: player.retryLsfg() }
                LinkButton { visible: root.ls === "noInstall" || root.ls === "noDll"; text: tr("chooseFolder"); onClicked: player.chooseLsfgFolder() }
                LinkButton { visible: root.ls !== "active" && root.ls !== "crashed"; text: tr("checkAgain"); onClicked: player.refreshLsfg() }
            }
        }
    }
}

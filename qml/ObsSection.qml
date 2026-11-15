import QtQuick

// OBS browser source: a local stream of the finished picture + an HTML file to add in OBS as a Browser source.
Column {
    id: root
    property var tr: function (k) { return k }
    spacing: 8

    readonly property var heights: [720, 1080, 1440, 0]

    component LinkButton: Rectangle {
        id: lb
        property string text
        property bool primary: true
        signal clicked()
        width: lbText.implicitWidth + 22; height: 28; radius: 8
        color: primary ? (lbMa.containsMouse ? "#ff8f63" : "#ff7a45") : (lbMa.containsMouse ? "#2b2e38" : "#1d2028")
        Text { id: lbText; anchors.centerIn: parent; text: lb.text; color: lb.primary ? "#14110f" : "#d6d8e0"; font.pixelSize: 12; font.weight: Font.DemiBold }
        MouseArea { id: lbMa; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: lb.clicked() }
    }

    Item {
        width: root.width; height: 24
        Text { text: tr("obs"); color: "#ffffff"; font.pixelSize: 14; font.weight: Font.DemiBold; anchors.verticalCenter: parent.verticalCenter }
        ToggleSwitch { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
            checked: player.obsEnabled; onToggled: (v) => player.obsEnabled = v }
    }
    Text {
        width: root.width; wrapMode: Text.Wrap; font.pixelSize: 11
        color: player.obsError !== "" ? "#ffb89c" : (player.obsViewers > 0 ? "#9be39b" : "#8b90a0")
        text: player.obsError !== "" ? player.obsError
              : !player.obsEnabled ? tr("obsOff")
              : player.obsViewers > 0 ? tr("obsLive").replace("%1", player.obsViewers)
              : tr("obsWaiting").replace("%1", player.obsUrl)
    }
    Text { text: tr("obsRes"); color: "#9da2b3"; font.pixelSize: 12 }
    Segmented {
        width: root.width
        model: ["720p", "1080p", "1440p", tr("native")]
        current: Math.max(0, root.heights.indexOf(player.obsHeight))
        onPicked: (i) => player.obsHeight = root.heights[i]
    }
    Row {
        spacing: 8
        LinkButton { text: tr("obsSave"); onClicked: player.saveObsFile() }
        LinkButton { text: tr("obsCopy"); primary: false; onClicked: player.copyObsUrl() }
    }
    Text {
        width: root.width; wrapMode: Text.Wrap; font.pixelSize: 11; color: "#c9ccd6"
        visible: player.obsFile !== ""
        text: tr("obsSaved").replace("%1", player.obsFile)
    }
    Text { width: root.width; wrapMode: Text.Wrap; font.pixelSize: 11; color: "#8b90a0"; text: tr("obsHelp") }
}

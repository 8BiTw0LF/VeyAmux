import QtQuick
Row {
    id: root
    property var model: []
    property int current: 0
    property bool enabled: true
    signal picked(int index)
    spacing: 4
    opacity: enabled ? 1 : 0.4
    Repeater {
        model: root.model
        Rectangle {
            width: (root.width - (root.model.length - 1) * root.spacing) / root.model.length
            height: 30; radius: 8
            color: index === root.current ? "#ff7a45" : (ma.containsMouse && root.enabled ? "#2b2e38" : "#1d2028")
            Text { anchors.centerIn: parent; text: modelData; font.pixelSize: 12; font.weight: Font.Medium
                color: index === root.current ? "#14110f" : "#c9ccd6" }
            MouseArea { id: ma; anchors.fill: parent; hoverEnabled: true; enabled: root.enabled
                cursorShape: Qt.PointingHandCursor; onClicked: root.picked(index) }
        }
    }
}

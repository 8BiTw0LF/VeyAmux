import QtQuick
Item {
    id: root
    property bool checked: false
    signal toggled(bool value)
    width: 40; height: 22
    Rectangle { anchors.fill: parent; radius: height / 2; color: root.checked ? "#ff7a45" : "#2b2e38"
        Behavior on color { ColorAnimation { duration: 120 } }
        Rectangle { y: 3; x: root.checked ? parent.width - width - 3 : 3; width: 16; height: 16; radius: 8; color: "#ffffff"
            Behavior on x { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } } }
    }
    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.toggled(!root.checked) }
}

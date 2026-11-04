import QtQuick 2.15

Item {
    id: key

    property string label: ""
    property string value: ""
    property bool wide: false
    property bool accent: false
    property color themeAccent: "#4d8bff"
    property color themeText: "#f4f7ff"
    property color themeCard: "#182033"
    property color themeBorder: "#2b3852"
    property real keyHeight: 34
    property real keyWidth: 0
    property bool down: false

    signal pressed()

    width: keyWidth > 0 ? keyWidth : (wide ? keyHeight * 1.8 : keyHeight)
    height: keyHeight

    Rectangle {
        anchors.fill: parent
        anchors.margins: 1
        radius: 7
        color: key.down
               ? Qt.rgba(0.30, 0.55, 1.0, 0.62)
               : (key.accent ? Qt.rgba(0.30, 0.55, 1.0, 0.24) : key.themeCard)
        border.width: key.down ? 2 : 1
        border.color: (key.down || key.accent) ? key.themeAccent : key.themeBorder
        scale: key.down ? 0.96 : 1.0

        Behavior on scale {
            NumberAnimation { duration: 60 }
        }
        Behavior on color {
            ColorAnimation { duration: 60 }
        }

        Text {
            anchors.centerIn: parent
            text: key.label
            color: key.themeText
            font.pixelSize: Math.max(12, key.keyHeight * 0.42)
            font.weight: Font.DemiBold
            elide: Text.ElideRight
            width: parent.width - 6
            horizontalAlignment: Text.AlignHCenter
        }

        MouseArea {
            anchors.fill: parent
            onPressedChanged: key.down = pressed
            onClicked: key.pressed()
        }
    }
}

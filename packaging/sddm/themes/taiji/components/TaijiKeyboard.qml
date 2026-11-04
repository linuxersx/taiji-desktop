import QtQuick 2.15
import "."

Item {
    id: keyboard

    property color accent: "#4d8bff"
    property color textColor: "#f4f7ff"
    property color cardColor: "#182033"
    property color borderColor: "#2b3852"
    property bool shift: false
    property bool symbols: false

    signal key(string text)
    signal backspace()
    signal enterKey()
    signal hideKeyboard()

    readonly property var letterRows: ["qwertyuiop", "asdfghjkl", "zxcvbnm"]
    readonly property var symbolRows: [
        ["!", "@", "#", "$", "%", "^", "&", "*", "(", ")"],
        ["-", "_", "=", "+", "[", "]", "{", "}", "\\", "|"],
        [";", ":", "'", "\"", ",", ".", "<", ">", "/", "?"],
        ["`", "~", "€", "£", "¥", "•", "°"]
    ]

    readonly property real gap: 5
    readonly property real margin: 12
    readonly property real rowHeight: Math.max(28, (height - margin * 2 - gap * 4) / 5)
    readonly property real unit: Math.max(24, (width - margin * 2 - gap * 11) / 11.5)

    Rectangle {
        anchors.fill: parent
        radius: 14
        color: Qt.rgba(0.04, 0.06, 0.10, 0.94)
        border.width: 1
        border.color: keyboard.borderColor
    }

    Column {
        anchors.fill: parent
        anchors.margins: keyboard.margin
        spacing: keyboard.gap

        /* 数字行 + 退格 */
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: keyboard.gap

            Repeater {
                model: 10
                delegate: Key {
                    keyHeight: keyboard.rowHeight
                    keyWidth: keyboard.unit
                    themeAccent: keyboard.accent
                    themeText: keyboard.textColor
                    themeCard: keyboard.cardColor
                    themeBorder: keyboard.borderColor
                    label: keyboard.symbols
                           ? keyboard.symbolRows[0][index]
                           : String((index + 1) % 10)
                    onPressed: keyboard.key(label)
                }
            }
            Key {
                keyHeight: keyboard.rowHeight
                keyWidth: keyboard.unit * 1.5
                themeAccent: keyboard.accent
                themeText: keyboard.textColor
                themeCard: keyboard.cardColor
                themeBorder: keyboard.borderColor
                label: "⌫"
                onPressed: keyboard.backspace()
            }
        }

        /* QWERTY 行 */
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: keyboard.gap

            Repeater {
                model: 10
                delegate: Key {
                    property string ch: keyboard.symbols
                        ? keyboard.symbolRows[1][index]
                        : keyboard.letterRows[0].charAt(index)
                    keyHeight: keyboard.rowHeight
                    keyWidth: keyboard.unit
                    themeAccent: keyboard.accent
                    themeText: keyboard.textColor
                    themeCard: keyboard.cardColor
                    themeBorder: keyboard.borderColor
                    label: keyboard.shift && !keyboard.symbols
                           ? ch.toUpperCase() : ch
                    onPressed: keyboard.key(label)
                }
            }
        }

        /* ASDF 行 + 回车 */
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: keyboard.gap

            Repeater {
                model: keyboard.symbols ? 10 : 9
                delegate: Key {
                    property string ch: keyboard.symbols
                        ? keyboard.symbolRows[2][index]
                        : keyboard.letterRows[1].charAt(index)
                    keyHeight: keyboard.rowHeight
                    keyWidth: keyboard.unit
                    themeAccent: keyboard.accent
                    themeText: keyboard.textColor
                    themeCard: keyboard.cardColor
                    themeBorder: keyboard.borderColor
                    label: keyboard.shift && !keyboard.symbols
                           ? ch.toUpperCase() : ch
                    onPressed: keyboard.key(label)
                }
            }
            Key {
                keyHeight: keyboard.rowHeight
                keyWidth: keyboard.unit * 1.5
                accent: true
                themeAccent: keyboard.accent
                themeText: keyboard.textColor
                themeCard: keyboard.cardColor
                themeBorder: keyboard.borderColor
                label: "→"
                onPressed: keyboard.enterKey()
            }
        }

        /* Shift + ZXCV 行 + Shift */
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: keyboard.gap

            Key {
                keyHeight: keyboard.rowHeight
                keyWidth: keyboard.unit * 1.5
                accent: keyboard.shift && !keyboard.symbols
                themeAccent: keyboard.accent
                themeText: keyboard.textColor
                themeCard: keyboard.cardColor
                themeBorder: keyboard.borderColor
                label: "⇧"
                onPressed: keyboard.shift = !keyboard.shift
            }

            Repeater {
                model: 7
                delegate: Key {
                    property string ch: keyboard.symbols
                        ? keyboard.symbolRows[3][index]
                        : keyboard.letterRows[2].charAt(index)
                    keyHeight: keyboard.rowHeight
                    keyWidth: keyboard.unit
                    themeAccent: keyboard.accent
                    themeText: keyboard.textColor
                    themeCard: keyboard.cardColor
                    themeBorder: keyboard.borderColor
                    label: keyboard.shift && !keyboard.symbols
                           ? ch.toUpperCase() : ch
                    onPressed: keyboard.key(label)
                }
            }

            Key {
                keyHeight: keyboard.rowHeight
                keyWidth: keyboard.unit * 1.5
                accent: keyboard.shift && !keyboard.symbols
                themeAccent: keyboard.accent
                themeText: keyboard.textColor
                themeCard: keyboard.cardColor
                themeBorder: keyboard.borderColor
                label: "⇧"
                onPressed: keyboard.shift = !keyboard.shift
            }
        }

        /* 底部：符号开关 / 逗号 / 空格 / 句号 / 登录 / 收起 */
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: keyboard.gap

            Key {
                keyHeight: keyboard.rowHeight
                keyWidth: keyboard.unit * 1.5
                accent: keyboard.symbols
                themeAccent: keyboard.accent
                themeText: keyboard.textColor
                themeCard: keyboard.cardColor
                themeBorder: keyboard.borderColor
                label: keyboard.symbols ? "ABC" : "?123"
                onPressed: {
                    keyboard.symbols = !keyboard.symbols
                    keyboard.shift = false
                }
            }
            Key {
                keyHeight: keyboard.rowHeight
                keyWidth: keyboard.unit
                themeAccent: keyboard.accent
                themeText: keyboard.textColor
                themeCard: keyboard.cardColor
                themeBorder: keyboard.borderColor
                label: ","
                onPressed: keyboard.key(",")
            }
            Key {
                keyHeight: keyboard.rowHeight
                keyWidth: keyboard.unit * 5
                themeAccent: keyboard.accent
                themeText: keyboard.textColor
                themeCard: keyboard.cardColor
                themeBorder: keyboard.borderColor
                label: "空格"
                onPressed: keyboard.key(" ")
            }
            Key {
                keyHeight: keyboard.rowHeight
                keyWidth: keyboard.unit
                themeAccent: keyboard.accent
                themeText: keyboard.textColor
                themeCard: keyboard.cardColor
                themeBorder: keyboard.borderColor
                label: "."
                onPressed: keyboard.key(".")
            }
            Key {
                keyHeight: keyboard.rowHeight
                keyWidth: keyboard.unit * 1.5
                accent: true
                themeAccent: keyboard.accent
                themeText: keyboard.textColor
                themeCard: keyboard.cardColor
                themeBorder: keyboard.borderColor
                label: "→"
                onPressed: keyboard.enterKey()
            }
            Key {
                keyHeight: keyboard.rowHeight
                keyWidth: keyboard.unit
                themeAccent: keyboard.accent
                themeText: keyboard.textColor
                themeCard: keyboard.cardColor
                themeBorder: keyboard.borderColor
                label: "⌄"
                onPressed: keyboard.hideKeyboard()
            }
        }
    }
}

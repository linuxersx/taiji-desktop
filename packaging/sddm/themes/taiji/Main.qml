import QtQuick 2.15
import QtQuick.Window 2.15
import "."
import "components"

Rectangle {
    id: root

    width: Screen.width > 0 ? Screen.width : 1280
    height: Screen.height > 0 ? Screen.height : 720
    color: Theme.bg

    property int sessionIndex: 0
    property string userName: ""
    property bool keyboardVisible: false
    property bool sessionOpen: false
    property bool sessionFound: false
    property string message: ""
    property real uiScale: 1.15
    property real ui: Math.min(2.2, Math.max(0.62, height / 720) * uiScale)
    property bool compact: height < 420
    property real avatarSize: root.compact ? 78 : Math.max(112, 150 * root.ui)

    FontLoader {
        id: taijiFont
        source: "assets/TaijiCJK.ttf"
    }

    Image {
        anchors.fill: parent
        source: "assets/splash.jpg"
        fillMode: Image.PreserveAspectCrop
        opacity: 0.42
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.bg
        opacity: 0.72
    }

    Column {
        id: header
        x: Theme.pad * root.ui
        y: Math.max(8, 18 * root.ui)
        spacing: 2
        Text {
            text: "太极"
            color: Theme.text
            font.family: taijiFont.name
            font.pixelSize: Math.max(26, 46 * root.ui)
            font.weight: Font.DemiBold
        }
        Text {
            text: "TAIJI  /  NOVARETRO DESKTOP"
            color: Theme.text3
            font.family: taijiFont.name
            font.pixelSize: Math.max(8, 12 * root.ui)
            font.letterSpacing: 1
        }
    }

    Text {
        anchors.right: parent.right
        anchors.rightMargin: Theme.pad * root.ui
        anchors.top: parent.top
        anchors.topMargin: Math.max(12, 20 * root.ui)
        color: Theme.text2
        font.family: taijiFont.name
        font.pixelSize: Math.max(10, 15 * root.ui)
        text: Qt.formatTime(new Date(), "HH:mm")
    }

    /* Windows 登录风格：整组居中，键盘出现时整体上移 */
    Column {
        id: loginStack
        width: root.width
        anchors.horizontalCenter: parent.horizontalCenter
        y: root.keyboardVisible
           ? (header.y + header.height + Math.max(6, 12 * root.ui))
           : (root.compact ? 34 : Math.max(48, 70 * root.ui))
        spacing: root.compact ? 7 : 14 * root.ui

        /* 大头像 */
        Row {
            id: avatarRow
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 16 * root.ui
            /* 键盘弹出后隐藏头像，给密码框和键盘留出空间 */
            visible: !root.keyboardVisible

            Repeater {
                model: userModel
                delegate: Item {
                    width: root.avatarSize
                    height: root.avatarSize

                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: Theme.elevated
                        border.width: root.userName === model.name ? 3 : 1
                        border.color: root.userName === model.name ? Theme.accent : Theme.border

                        Text {
                            anchors.centerIn: parent
                            text: model.name ? String(model.name).charAt(0).toUpperCase() : "?"
                            color: Theme.text
                            font.family: taijiFont.name
                            font.pixelSize: parent.height * 0.42
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            root.userName = model.name
                            passwordInput.text = ""
                            root.message = ""
                        }
                    }

                    Component.onCompleted: {
                        if (index === 0 && root.userName === "")
                            root.userName = model.name
                    }
                }
            }
        }

        Text {
            id: userNameText
            anchors.horizontalCenter: parent.horizontalCenter
            visible: !root.keyboardVisible
            text: root.userName
            color: Theme.text
            font.family: taijiFont.name
            font.pixelSize: root.compact ? 16 : Math.max(18, 24 * root.ui)
            font.weight: Font.DemiBold
        }

        /* 会话下拉菜单（原生 SDDM 风格） */
        Item {
            id: sessionBox
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.max(260, 360 * root.ui)
            height: root.compact ? 32 : Math.max(42, 52 * root.ui)
            z: 20

            Rectangle {
                id: sessionSelector
                anchors.fill: parent
                radius: height / 2
                color: Theme.surface
                border.width: 1
                border.color: root.sessionOpen ? Theme.accent : Theme.border

                Repeater {
                    model: sessionModel
                    delegate: Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        text: model.name
                        visible: index === root.sessionIndex
                        color: Theme.text
                        font.family: taijiFont.name
                        font.pixelSize: root.compact ? 13 : Math.max(14, 17 * root.ui)
                    }
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "▾"
                    color: Theme.text2
                    font.pixelSize: root.compact ? 14 : Math.max(14, 17 * root.ui)
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: root.sessionOpen = !root.sessionOpen
                }
            }

            Column {
                id: sessionMenu
                visible: root.sessionOpen
                anchors.top: sessionSelector.bottom
                anchors.topMargin: 4 * root.ui
                anchors.horizontalCenter: sessionSelector.horizontalCenter
                width: sessionSelector.width

                Repeater {
                    model: sessionModel
                    delegate: Rectangle {
                        width: sessionMenu.width
                        height: root.compact ? 30 : Math.max(38, 44 * root.ui)
                        color: index === root.sessionIndex
                               ? Qt.rgba(0.30, 0.55, 1.0, 0.24) : Theme.card
                        border.width: 1
                        border.color: Theme.border

                        Text {
                            anchors.left: parent.left
                            anchors.leftMargin: 16
                            anchors.verticalCenter: parent.verticalCenter
                            text: model.name
                            color: index === root.sessionIndex ? Theme.text : Theme.text2
                            font.family: taijiFont.name
                            font.pixelSize: root.compact ? 13 : Math.max(14, 17 * root.ui)
                        }
                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                root.sessionIndex = index
                                root.sessionOpen = false
                            }
                        }

                        Component.onCompleted: {
                            // 中文环境下名字是“太极（Wayland）”，必须同时匹配 model.file
                            var n = model.name ? String(model.name).toLowerCase() : ""
                            var f = model.file ? String(model.file).toLowerCase() : ""
                            if (!root.sessionFound && (
                                n.indexOf("taiji") !== -1 ||
                                n.indexOf("太极") !== -1 ||
                                f.indexOf("taiji") !== -1)) {
                                root.sessionIndex = index
                                root.sessionFound = true
                            }
                        }
                    }
                }
            }
        }

        /* 密码框，右侧内置登录箭头 */
        Rectangle {
            id: passwordBox
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.max(240, Math.min(
                       root.width - Theme.pad * root.ui * 2,
                       Math.max(340, 500 * root.ui)))
            height: root.compact ? 40 : Math.max(48, 58 * root.ui)
            radius: height / 2
            color: Theme.surface
            border.width: 1
            border.color: passwordInput.activeFocus ? Theme.accent : Theme.border

            TextInput {
                id: passwordInput
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 54
                verticalAlignment: TextInput.AlignVCenter
                color: Theme.text
                font.family: taijiFont.name
                font.pixelSize: root.compact ? 15 : Math.max(16, 20 * root.ui)
                echoMode: TextInput.Password
                passwordCharacter: "●"
                selectByMouse: false
                activeFocusOnPress: true
                activeFocusOnTab: true
                inputMethodHints: Qt.ImhHiddenText | Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
                onAccepted: root.doLogin()
                onActiveFocusChanged: if (activeFocus) root.keyboardVisible = true
            }

            MouseArea {
                anchors.fill: parent
                z: 2
                cursorShape: Qt.IBeamCursor
                onClicked: {
                    root.sessionOpen = false
                    passwordInput.forceActiveFocus()
                    root.keyboardVisible = true
                }
            }

            Text {
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                text: "输入密码"
                color: Theme.text3
                font.family: taijiFont.name
                font.pixelSize: root.compact ? 14 : Math.max(15, 18 * root.ui)
                visible: passwordInput.text.length === 0
            }

            Rectangle {
                anchors.right: parent.right
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                width: parent.height - 12
                height: width
                radius: width / 2
                color: Theme.accent
                Text {
                    anchors.centerIn: parent
                    text: "→"
                    color: "#ffffff"
                    font.pixelSize: parent.height * 0.55
                    font.weight: Font.DemiBold
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: root.doLogin()
                }
            }
        }

        Text {
            id: errorText
            anchors.horizontalCenter: parent.horizontalCenter
            width: root.width - Theme.pad * root.ui * 2
            horizontalAlignment: Text.AlignHCenter
            text: root.message
            color: Theme.danger
            font.family: taijiFont.name
            font.pixelSize: root.compact ? 10 : Math.max(10, 13 * root.ui)
            elide: Text.ElideRight
        }
    }

    /* 虚拟键盘：默认隐藏，点击密码框后出现 */
    TaijiKeyboard {
        id: keyboard
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Math.max(8, 14 * root.ui)
        height: root.compact ? root.height * 0.48 : Math.min(root.height * 0.55, Math.max(170, 360 * root.ui))
        visible: root.keyboardVisible
        accent: Theme.accent
        textColor: Theme.text
        cardColor: Theme.card
        borderColor: Theme.border

        onKey: root.insertText(text)
        onBackspace: root.backspace()
        onEnterKey: root.doLogin()
        onHideKeyboard: root.keyboardVisible = false
    }

    function insertText(text) {
        var pos = passwordInput.cursorPosition
        var value = passwordInput.text
        passwordInput.text = value.slice(0, pos) + text + value.slice(pos)
        passwordInput.cursorPosition = pos + text.length
        passwordInput.forceActiveFocus()
    }

    function backspace() {
        var pos = passwordInput.cursorPosition
        if (pos <= 0) return
        var value = passwordInput.text
        passwordInput.text = value.slice(0, pos - 1) + value.slice(pos)
        passwordInput.cursorPosition = pos - 1
        passwordInput.forceActiveFocus()
    }

    function doLogin() {
        if (!root.userName) {
            root.message = "请选择用户"
            return
        }
        root.message = ""
        sddm.login(root.userName, passwordInput.text, root.sessionIndex)
    }

    Connections {
        target: sddm
        onLoginFailed: {
            root.message = "密码错误，请重试"
            passwordInput.text = ""
            passwordInput.forceActiveFocus()
            root.keyboardVisible = true
        }
        onInformationMessage: root.message = message
    }

    Component.onCompleted: {
        root.keyboardVisible = false
        root.sessionOpen = false
    }
}

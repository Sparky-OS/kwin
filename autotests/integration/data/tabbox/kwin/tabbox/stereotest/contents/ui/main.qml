/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

import QtQuick
import QtQuick.Window

import org.kde.kwin as KWin

// A switcher that fakes depth and is drawn once for each eye: a square popped, one sunk, a card
// turned about its vertical axis, and a square inside a tilted view, each at the depth it fakes;
// a strip at the screen.
KWin.TabBoxSwitcher {
    id: tabBox
    stereo: true

    Window {
        id: window
        x: tabBox.screenGeometry.x
        y: tabBox.screenGeometry.y
        width: tabBox.screenGeometry.width * eyes.count
        height: tabBox.screenGeometry.height
        flags: Qt.Popup | Qt.BypassWindowManagerHint | Qt.FramelessWindowHint
        visible: true
        color: "black"

        Repeater {
            id: eyes
            model: tabBox.stereoEyes ? [KWin.StereoRotation.LeftEye, KWin.StereoRotation.RightEye] : [KWin.StereoRotation.NoEye]

            Item {
                id: eyeView
                required property int index
                required property int modelData
                readonly property real side: modelData === KWin.StereoRotation.LeftEye ? 0.5 : (modelData === KWin.StereoRotation.RightEye ? -0.5 : 0)

                x: index * width
                width: tabBox.screenGeometry.width
                height: tabBox.screenGeometry.height
                clip: tabBox.stereoEyes

                Rectangle {
                    objectName: "popped"
                    x: 20 + eyeView.side * tabBox.stereoPopped
                    y: 20
                    width: 40
                    height: 40
                    color: "red"
                }
                Rectangle {
                    x: 80 - eyeView.side * tabBox.stereoSunk
                    y: 20
                    width: 40
                    height: 40
                    color: "lime"
                }
                Rectangle {
                    x: 20
                    y: 200
                    width: 280
                    height: 20
                    color: "white"
                }
                Rectangle {
                    x: 160
                    y: 20
                    width: 120
                    height: 60
                    color: "blue"
                    transform: KWin.StereoRotation {
                        origin.x: 60
                        origin.y: 30
                        axis { x: 0; y: 1; z: 0 }
                        angle: 50
                        eye: eyeView.modelData
                        parallax: -tabBox.stereoSunk / 2
                        parallaxPerDepth: 0.1
                    }
                }
                Item {
                    x: 20
                    y: 100
                    width: 280
                    height: 80
                    transform: KWin.StereoRotation {
                        id: tilt
                        origin.x: 140
                        origin.y: 40
                        axis { x: 0; y: 1; z: -0.15 }
                        angle: 10
                        eye: eyeView.modelData
                        parallaxPerDepth: 0.1
                    }
                    Rectangle {
                        id: inner
                        readonly property point offset: tilt.eyeOffset(Qt.point(x + width / 2, y + height / 2), eyeView.side * tabBox.stereoPopped)
                        x: 180
                        y: 20
                        width: 60
                        height: 40
                        color: "yellow"
                        transform: Translate {
                            x: inner.offset.x
                            y: inner.offset.y
                        }
                    }
                }
            }
        }
    }

    onVisibleChanged: window.visible = visible
}

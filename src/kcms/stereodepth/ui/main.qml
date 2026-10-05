/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami
import org.kde.kcmutils as KCM
import org.kde.kwin.kwinstereodepthsettings

KCM.SimpleKCM {
    id: root

    implicitWidth: Kirigami.Units.gridUnit * 40
    implicitHeight: Kirigami.Units.gridUnit * 24

    Kirigami.FormLayout {
        QQC2.Label {
            Kirigami.FormData.isSection: true
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 30
            wrapMode: Text.Wrap
            text: i18n("How far the desktop's windows and menus reach behind and in front of the screen on a display in one of its 3D modes. The values are the distance between what the two eyes see, in pixels on a screen 1920 pixels wide, and grow and shrink with the screen. Zero switches that side off.")
        }

        RowLayout {
            Kirigami.FormData.label: i18nc("@label:slider", "Background depth:")
            QQC2.Slider {
                id: sunkSlider
                from: 0
                to: 30
                stepSize: 1
                snapMode: QQC2.Slider.SnapAlways
                value: kcm.settings.sunkLimit
                onMoved: kcm.settings.sunkLimit = value
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "SunkLimit"
                }
            }
            QQC2.Label {
                Layout.minimumWidth: Kirigami.Units.gridUnit * 4
                text: sunkSlider.value === 0 ? i18nc("@label no depth", "Off") : i18ncp("@label pixels", "%1 pixel", "%1 pixels", sunkSlider.value)
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 30
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
            text: i18n("Inactive windows and the wallpaper sink behind the screen by up to this much. More depth: the background drifts further apart.")
        }

        RowLayout {
            Kirigami.FormData.label: i18nc("@label:slider", "Pop-out:")
            QQC2.Slider {
                id: poppedSlider
                from: 0
                to: 12
                stepSize: 1
                snapMode: QQC2.Slider.SnapAlways
                value: kcm.settings.poppedLimit
                onMoved: kcm.settings.poppedLimit = value
                KCM.SettingStateBinding {
                    configObject: kcm.settings
                    settingName: "PoppedLimit"
                }
            }
            QQC2.Label {
                Layout.minimumWidth: Kirigami.Units.gridUnit * 4
                text: poppedSlider.value === 0 ? i18nc("@label no pop-out", "Off") : i18ncp("@label pixels", "%1 pixel", "%1 pixels", poppedSlider.value)
            }
        }
        QQC2.Label {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 30
            wrapMode: Text.Wrap
            font: Kirigami.Theme.smallFont
            text: i18n("Menus, dialogs and tooltips of the active window come forward by up to this much. More pop-out: things near the screen's edges and long reading get harder.")
        }
    }
}

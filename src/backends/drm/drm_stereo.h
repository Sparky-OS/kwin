/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

namespace KWin
{

/**
 * HDMI 1.4 stereoscopic 3D output: the 3D structure a mode is sent in
 * (DRM_MODE_FLAG_3D_*, from the display's EDID), with the desktop drawn into both eyes.
 */
enum class StereoLayout {
    None,
    SideBySideHalf,
    TopAndBottom,
};

}

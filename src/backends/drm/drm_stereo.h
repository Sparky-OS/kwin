/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

namespace KWin
{

/**
 * Stereoscopic 3D output. SideBySideHalf and TopAndBottom are HDMI 1.4 structures a
 * mode is sent in (DRM_MODE_FLAG_3D_*, from the display's EDID), with the desktop drawn
 * into both eyes; the anaglyph layouts need no 3D mode and work on any screen, the two
 * eyes mixed into one picture for red/cyan glasses (CRT or modern-screen matrices).
 */
enum class StereoLayout {
    None,
    SideBySideHalf,
    TopAndBottom,
    AnaglyphCrt,
    AnaglyphModern,
};

inline bool isAnaglyph(StereoLayout layout)
{
    return layout == StereoLayout::AnaglyphCrt || layout == StereoLayout::AnaglyphModern;
}

}

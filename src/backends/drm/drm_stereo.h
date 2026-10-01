/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/output.h"

#include <QSize>

#include <xf86drmMode.h>

namespace KWin
{

/**
 * Stereoscopic 3D output. SideBySideHalf, TopAndBottom and FramePacking are HDMI 1.4
 * structures a mode is sent in (DRM_MODE_FLAG_3D_*, from the display's EDID): choosing such a mode is
 * turning 3D on, and the desktop is drawn into both eyes. The anaglyph layouts need no 3D
 * mode and work on any screen, the two eyes mixed into one picture for red/cyan glasses
 * (CRT or modern-screen matrices).
 */
enum class StereoLayout {
    None,
    SideBySideHalf,
    TopAndBottom,
    FramePacking,
    AnaglyphCrt,
    AnaglyphModern,
};

inline bool isAnaglyph(StereoLayout layout)
{
    return layout == StereoLayout::AnaglyphCrt || layout == StereoLayout::AnaglyphModern;
}

/**
 * The mode flag for a DRM mode's 3D structure; empty for 2D modes and for the 3D structures
 * the desktop can't be drawn into yet (frame packing and the rest), which aren't listed.
 */
inline OutputModeline::Flags stereoFlagsForDrmMode(uint32_t drmFlags)
{
    switch (drmFlags & DRM_MODE_FLAG_3D_MASK) {
    case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF:
        return OutputModeline::Flag::Stereo3DSideBySideHalf;
    case DRM_MODE_FLAG_3D_TOP_AND_BOTTOM:
        return OutputModeline::Flag::Stereo3DTopAndBottom;
    case DRM_MODE_FLAG_3D_FRAME_PACKING:
        return OutputModeline::Flag::Stereo3DFramePacking;
    default:
        return {};
    }
}

inline StereoLayout stereoLayoutForMode(OutputModeline::Flags flags)
{
    if (flags & OutputModeline::Flag::Stereo3DSideBySideHalf) {
        return StereoLayout::SideBySideHalf;
    }
    if (flags & OutputModeline::Flag::Stereo3DTopAndBottom) {
        return StereoLayout::TopAndBottom;
    }
    if (flags & OutputModeline::Flag::Stereo3DFramePacking) {
        return StereoLayout::FramePacking;
    }
    return StereoLayout::None;
}

/**
 * Frame packing sends both eyes in full in one taller frame: the left eye, the mode's
 * vertical blanking, then the right eye from line vtotal on (HDMI 1.4). The
 * scanout buffer is that whole frame; the mode's size is one eye.
 */
inline QSize framePackedSize(const drmModeModeInfo &mode)
{
    return QSize(mode.hdisplay, mode.vtotal + mode.vdisplay);
}

}

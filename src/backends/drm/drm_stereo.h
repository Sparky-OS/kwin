/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/output.h"
#include "core/rect.h"

#include <QPoint>
#include <QSize>

#include <xf86drmMode.h>

namespace KWin
{

/**
 * Stereoscopic 3D output. SideBySideHalf, TopAndBottom, FramePacking and SideBySideFull
 * are HDMI 1.4 structures a mode is sent in (DRM_MODE_FLAG_3D_*, from the display's EDID):
 * choosing such a mode is turning 3D on, and the desktop is drawn into both eyes.
 *
 * Without a 3D mode, an output can still be packed side by side (half) or top and bottom
 * on its 2D mode, for displays that don't detect a 3D signal and have their 3D format set
 * by hand (projectors and the like), or mixed as anaglyph for red/cyan glasses on any
 * screen (CRT or modern-screen matrices).
 */
enum class StereoLayout {
    None,
    SideBySideHalf,
    TopAndBottom,
    FramePacking,
    SideBySideFull,
    AnaglyphCrt,
    AnaglyphModern,
    RowsLeftFirst,
    ColumnsLeftFirst,
    CheckerboardLeftFirst,
    SequentialLeftFirst,
};

inline bool isSpatialStereo(StereoLayout layout)
{
    return layout == StereoLayout::RowsLeftFirst || layout == StereoLayout::ColumnsLeftFirst
        || layout == StereoLayout::CheckerboardLeftFirst;
}

inline bool isAnaglyph(StereoLayout layout)
{
    return layout == StereoLayout::AnaglyphCrt || layout == StereoLayout::AnaglyphModern;
}

// each refresh shows one eye, alternately; the eye is tied to the vblank sequence, see drm_frame_sequential.h
inline bool isFrameSequential(StereoLayout layout)
{
    return layout == StereoLayout::SequentialLeftFirst;
}

/**
 * The mode flag for a DRM mode's 3D structure; empty for 2D modes and for the 3D structures
 * the desktop can't be drawn into (field and line alternative, L + depth), which aren't
 * listed.
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
    case DRM_MODE_FLAG_3D_SIDE_BY_SIDE_FULL:
        return OutputModeline::Flag::Stereo3DSideBySideFull;
    default:
        return {};
    }
}

inline StereoLayout stereoLayoutForMode(OutputModeline::Flags flags)
{
    if (flags & OutputModeline::Flag::Stereo3DAnaglyphModern) {
        return StereoLayout::AnaglyphModern;
    }
    if (flags & OutputModeline::Flag::Stereo3DAnaglyphCrt) {
        return StereoLayout::AnaglyphCrt;
    }
    if (flags & OutputModeline::Flag::Stereo3DRowsLeftFirst) {
        return StereoLayout::RowsLeftFirst;
    }
    if (flags & OutputModeline::Flag::Stereo3DColumnsLeftFirst) {
        return StereoLayout::ColumnsLeftFirst;
    }
    if (flags & OutputModeline::Flag::Stereo3DCheckerboardLeftFirst) {
        return StereoLayout::CheckerboardLeftFirst;
    }

    if (flags & OutputModeline::Flag::Stereo3DSequentialLeftFirst) {
        return StereoLayout::SequentialLeftFirst;
    }
    if (flags & OutputModeline::Flag::Stereo3DSideBySideHalf) {
        return StereoLayout::SideBySideHalf;
    }
    if (flags & OutputModeline::Flag::Stereo3DTopAndBottom) {
        return StereoLayout::TopAndBottom;
    }
    if (flags & OutputModeline::Flag::Stereo3DFramePacking) {
        return StereoLayout::FramePacking;
    }
    if (flags & OutputModeline::Flag::Stereo3DSideBySideFull) {
        return StereoLayout::SideBySideFull;
    }
    return StereoLayout::None;
}

/**
 * The layouts that send both eyes in full in one frame bigger than the mode, which is one
 * eye: frame packing (the left eye, the mode's vertical blanking, then the right eye from
 * line vtotal on), side by side full (the right eye from column hdisplay on), and frame
 * sequential (the two eyes from column hdisplay on, selected one refresh at a time). The
 * scanout buffer is that whole frame.
 */
inline bool isFullFrameStereo(StereoLayout layout)
{
    return layout == StereoLayout::FramePacking || layout == StereoLayout::SideBySideFull
        || layout == StereoLayout::SequentialLeftFirst;
}

inline QSize stereoFrameSize(const drmModeModeInfo &mode, StereoLayout layout)
{
    switch (layout) {
    case StereoLayout::FramePacking:
        return QSize(mode.hdisplay, mode.vtotal + mode.vdisplay);
    case StereoLayout::SideBySideFull:
    case StereoLayout::SequentialLeftFirst:
        return QSize(2 * mode.hdisplay, mode.vdisplay);
    default:
        return QSize(mode.hdisplay, mode.vdisplay);
    }
}

// where the right eye starts in a full-frame layout
inline QPoint stereoRightEyeOffset(const drmModeModeInfo &mode, StereoLayout layout)
{
    return layout == StereoLayout::FramePacking ? QPoint(0, mode.vtotal) : QPoint(mode.hdisplay, 0);
}

inline Rect frameSequentialSourceRect(const drmModeModeInfo &mode, StereoEye eye)
{
    return Rect(eye == StereoEye::Left ? QPoint(0, 0) : QPoint(mode.hdisplay, 0), QSize(mode.hdisplay, mode.vdisplay));
}

}

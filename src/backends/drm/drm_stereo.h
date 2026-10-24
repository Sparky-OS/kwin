/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/output.h"
#include "core/rect.h"
#include "core/stereodepthcoding.h"

#include <QPoint>
#include <QSize>

#include <xf86drmMode.h>

#include <algorithm>

namespace KWin
{

/**
 * Stereoscopic 3D output. SideBySideHalf, TopAndBottom, FramePacking, SideBySideFull,
 * FieldAlternative, LineAlternative, LDepth and LDepthGraphics are the HDMI 1.4a structures
 * a mode is sent in (Appendix H, Table H-2; DRM_MODE_FLAG_3D_*, from the display's EDID):
 * choosing such a mode is turning 3D on, and the desktop is drawn into both eyes, or for
 * the picture and depth structures into the left eye's picture and its depth.
 *
 * Without a 3D mode, an output can still be packed side by side (half) or top and bottom
 * on its 2D mode, for displays that don't detect a 3D signal and have their 3D format set
 * by hand (projectors and the like), or mixed as anaglyph for red/cyan glasses on any
 * screen (CRT or modern-screen matrices). A 2D mode two eyes wide (3840x1080) also gets
 * a side by side (full) twin, for USB-C 3D glasses that show its left half to the left eye
 * and its right half to the right eye: the mode is the whole frame and each eye is one half,
 * where the HDMI structure is the mode per eye and the frame twice as wide.
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
    RowsRightFirst,
    ColumnsLeftFirst,
    ColumnsRightFirst,
    CheckerboardLeftFirst,
    CheckerboardRightFirst,
    SequentialLeftFirst,
    SequentialRightFirst,
    FieldAlternative,
    LineAlternative,
    LDepth,
    LDepthGraphics,
};

// the side by side (full) twin of a 2D mode two eyes wide: the mode is the frame, not an eye
inline bool isVirtualSideBySideFull(OutputModeline::Flags flags)
{
    return flags.testFlag(OutputModeline::Flag::VirtualStereo) && flags.testFlag(OutputModeline::Flag::Stereo3DSideBySideFull);
}

inline bool isSpatialStereo(StereoLayout layout)
{
    return layout >= StereoLayout::RowsLeftFirst && layout <= StereoLayout::CheckerboardRightFirst;
}

inline bool isAnaglyph(StereoLayout layout)
{
    return layout == StereoLayout::AnaglyphCrt || layout == StereoLayout::AnaglyphModern;
}

// each refresh shows one eye, alternately; the eye is tied to the vblank sequence, see drm_frame_sequential.h
inline bool isFrameSequential(StereoLayout layout)
{
    return layout == StereoLayout::SequentialLeftFirst || layout == StereoLayout::SequentialRightFirst;
}

// the picture and depth structures: the left eye's picture with its depth (Figure H-5), and
// also with graphics and the graphics' depth (Figure H-6)
inline bool isPictureAndDepth(StereoLayout layout)
{
    return layout == StereoLayout::LDepth || layout == StereoLayout::LDepthGraphics;
}

/**
 * The mode flag for a DRM mode's 3D structure, empty for 2D modes: every structure of HDMI
 * 1.4a, Appendix H, Table H-2.
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
    case DRM_MODE_FLAG_3D_FIELD_ALTERNATIVE:
        return OutputModeline::Flag::Stereo3DFieldAlternative;
    case DRM_MODE_FLAG_3D_LINE_ALTERNATIVE:
        return OutputModeline::Flag::Stereo3DLineAlternative;
    case DRM_MODE_FLAG_3D_L_DEPTH:
        return OutputModeline::Flag::Stereo3DLDepth;
    case DRM_MODE_FLAG_3D_L_DEPTH_GFX_GFX_DEPTH:
        return OutputModeline::Flag::Stereo3DLDepthGraphics;
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
    if (flags & OutputModeline::Flag::Stereo3DRowsRightFirst) {
        return StereoLayout::RowsRightFirst;
    }
    if (flags & OutputModeline::Flag::Stereo3DColumnsLeftFirst) {
        return StereoLayout::ColumnsLeftFirst;
    }
    if (flags & OutputModeline::Flag::Stereo3DColumnsRightFirst) {
        return StereoLayout::ColumnsRightFirst;
    }
    if (flags & OutputModeline::Flag::Stereo3DCheckerboardLeftFirst) {
        return StereoLayout::CheckerboardLeftFirst;
    }
    if (flags & OutputModeline::Flag::Stereo3DCheckerboardRightFirst) {
        return StereoLayout::CheckerboardRightFirst;
    }

    if (flags & OutputModeline::Flag::Stereo3DSequentialLeftFirst) {
        return StereoLayout::SequentialLeftFirst;
    }
    if (flags & OutputModeline::Flag::Stereo3DSequentialRightFirst) {
        return StereoLayout::SequentialRightFirst;
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
    if (flags & OutputModeline::Flag::Stereo3DFieldAlternative) {
        return StereoLayout::FieldAlternative;
    }
    if (flags & OutputModeline::Flag::Stereo3DLineAlternative) {
        return StereoLayout::LineAlternative;
    }
    if (flags & OutputModeline::Flag::Stereo3DLDepth) {
        return StereoLayout::LDepth;
    }
    if (flags & OutputModeline::Flag::Stereo3DLDepthGraphics) {
        return StereoLayout::LDepthGraphics;
    }
    return StereoLayout::None;
}

/**
 * The layouts that send the eyes, or the picture and its depth, in full in one frame bigger
 * than the mode, which is one eye: frame packing and field alternative (the left eye, the
 * mode's vertical blanking, then the right eye from line vtotal on; HDMI 1.4a, Figures 8-3
 * and H-2), L + depth (the depth from line vtotal on, Figure H-5), L + depth + graphics +
 * graphics-depth (four pictures one vtotal apart, Figure H-6), line alternative (the eyes
 * line by line, left first, Figure H-3), side by side full (the right eye from column hdisplay
 * on, Figure H-4), and frame sequential (the two eyes from column hdisplay on, selected one
 * refresh at a time). The scanout buffer is that whole frame, as drm_mode_get_hv_timing()
 * sizes it.
 */
inline bool isFullFrameStereo(StereoLayout layout)
{
    switch (layout) {
    case StereoLayout::FramePacking:
    case StereoLayout::SideBySideFull:
    case StereoLayout::SequentialLeftFirst:
    case StereoLayout::SequentialRightFirst:
    case StereoLayout::FieldAlternative:
    case StereoLayout::LineAlternative:
    case StereoLayout::LDepth:
    case StereoLayout::LDepthGraphics:
        return true;
    default:
        return false;
    }
}

inline QSize stereoFrameSize(const drmModeModeInfo &mode, StereoLayout layout, OutputModeline::Flags flags = {})
{
    if (layout == StereoLayout::SideBySideFull && isVirtualSideBySideFull(flags)) {
        return QSize(mode.hdisplay, mode.vdisplay);
    }
    switch (layout) {
    case StereoLayout::FramePacking:
    case StereoLayout::FieldAlternative:
    case StereoLayout::LDepth:
        return QSize(mode.hdisplay, mode.vtotal + mode.vdisplay);
    case StereoLayout::LDepthGraphics:
        return QSize(mode.hdisplay, 3 * mode.vtotal + mode.vdisplay);
    case StereoLayout::LineAlternative:
        return QSize(mode.hdisplay, 2 * mode.vdisplay);
    case StereoLayout::SideBySideFull:
    case StereoLayout::SequentialLeftFirst:
    case StereoLayout::SequentialRightFirst:
        return QSize(2 * mode.hdisplay, mode.vdisplay);
    default:
        return QSize(mode.hdisplay, mode.vdisplay);
    }
}

// one eye of a full-frame layout: the mode, or half of it for the twin of a mode two eyes wide
inline QSize stereoEyeSize(const drmModeModeInfo &mode, OutputModeline::Flags flags)
{
    if (isVirtualSideBySideFull(flags)) {
        return QSize(mode.hdisplay / 2, mode.vdisplay);
    }
    return QSize(mode.hdisplay, mode.vdisplay);
}

// where the right eye starts in a full-frame layout; for L + depth where the depth starts
inline QPoint stereoRightEyeOffset(const drmModeModeInfo &mode, StereoLayout layout, OutputModeline::Flags flags = {})
{
    if (layout == StereoLayout::SideBySideFull && isVirtualSideBySideFull(flags)) {
        return QPoint(mode.hdisplay / 2, 0);
    }
    switch (layout) {
    case StereoLayout::FramePacking:
    case StereoLayout::FieldAlternative:
    case StereoLayout::LDepth:
    case StereoLayout::LDepthGraphics:
        return QPoint(0, mode.vtotal);
    case StereoLayout::LineAlternative:
        return QPoint(0, 1);
    default:
        return QPoint(mode.hdisplay, 0);
    }
}

// where each picture of L + depth and L + depth + graphics + graphics-depth starts
inline QPoint stereoPlaneOffset(const drmModeModeInfo &mode, StereoPlane plane)
{
    return QPoint(0, int(plane) * mode.vtotal);
}

/**
 * The value of the "HDMI 3D metadata" connector property: struct drm_hdmi_3d_metadata of the
 * edition's kernel (include/uapi/drm/drm_mode.h), which libdrm doesn't have yet. type is
 * 3D_Metadata_type (HDMI 1.4a, Appendix H, Table H-4), length 3D_Metadata_Length, data
 * 3D_Metadata_1 to 3D_Metadata_N as they are sent.
 */
struct DrmHdmi3dMetadata
{
    uint8_t type;
    uint8_t length;
    uint8_t data[30];
};
static_assert(sizeof(DrmHdmi3dMetadata) == 32);

// the parallax information of ISO/IEC 23002-3 that the depth of a picture and depth mode is coded with
inline DrmHdmi3dMetadata hdmi3dMetadata(const StereoDepthCoding &coding)
{
    DrmHdmi3dMetadata ret{};
    const auto bytes = coding.metadata();
    ret.type = 0;
    ret.length = bytes.size();
    std::copy(bytes.begin(), bytes.end(), ret.data);
    return ret;
}

inline Rect frameSequentialSourceRect(const drmModeModeInfo &mode, StereoEye eye)
{
    return Rect(eye == StereoEye::Left ? QPoint(0, 0) : QPoint(mode.hdisplay, 0), QSize(mode.hdisplay, mode.vdisplay));
}

}

/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/colorspace.h"
#include "core/output.h"
#include "kwin_export.h"

#include <QSize>

#include <memory>

namespace KWin
{

class EglSwapchainSlot;
class SyncReleasePoint;

/**
 * The two eyes of the scene as an output with stereo eyes composed them, one picture each, in
 * that output's own pixels (STEREO3D.md: the scene is composed once, outputs are filters). An
 * output that replicates it copies the pair into its own eye buffers, scaled once, and applies
 * only its own filter (frame packing, anaglyph, half side by side, ...) on the way out; it does
 * not paint the scene again.
 *
 * The pair holds the buffers: while any reference to it is alive, the composing output does not
 * render over them. A reader that has issued its copy adds its fence to
 * the release points through addReaderFence().
 */
class KWIN_EXPORT StereoEyePair
{
public:
    /** left eye */
    std::shared_ptr<EglSwapchainSlot> left;
    /** right eye; null when the scene was composed once, as one picture for both eyes */
    std::shared_ptr<EglSwapchainSlot> right;
    /** the size of one eye's buffer */
    QSize size;
    /** the orientation of the eyes' buffers, as the composing layer made them */
    OutputTransform transform = OutputTransform::Kind::Normal;
    /** the colour space the eyes are composed in */
    std::shared_ptr<ColorDescription> blendingColor;
    /** the device (the GL context) the buffers belong to */
    const void *owner = nullptr;
    /** counts the pairs a layer has composed */
    quint64 generation = 0;

    /** keeps the buffers out of the swapchain's reuse while the pair is alive */
    std::shared_ptr<SyncReleasePoint> leftHold;
    std::shared_ptr<SyncReleasePoint> rightHold;
};

}

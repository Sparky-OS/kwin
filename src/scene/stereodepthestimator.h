/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/stereodepthcoding.h"

#include <QSize>
#include <QVector2D>

#include <kwin_export.h>

#include <memory>

namespace KWin
{

class GLFramebuffer;
class GLShader;
class GLTexture;

/**
 * The depth picture of a picture and depth output, on the GPU.
 *
 * estimate() finds the parallax between the picture and the other eye's view by matching
 * blocks along the eyes' axis, at a quarter of the size: for 3D content, whose depth only its
 * two views tell, and notes where they matched at one parallax only. compose() then writes the
 * depth picture: the desktop's own samples where the desktop knows them, the estimate's
 * elsewhere where it matched, and the desktop's there otherwise (a plain area of a window keeps
 * its window's level).
 */
class KWIN_EXPORT StereoDepthEstimator
{
public:
    StereoDepthEstimator();
    ~StereoDepthEstimator();

    // the pixels of the pictures each texel of the estimate stands for, along each axis
    static constexpr int s_scale = 4;

    /**
     * Estimates the parallax of every texel of @a picture against @a other, the position in
     * @a other minus the one in @a picture, in pixels along @a direction (a pixel to the right
     * on the output, in pixels of the textures), up to @a range either way. Both textures have
     * the same size. Returns false when the GPU can't.
     */
    bool estimate(GLTexture *picture, GLTexture *other, const QVector2D &direction, int range);

    /**
     * Writes the depth picture into the framebuffer bound now, @a size pixels: where @a desktop
     * has alpha one its colour; where it has alpha zero the sample of the last estimate where
     * the views matched at one parallax, and its colour where they didn't or when @a useEstimate
     * is false. Returns false when the GPU can't.
     */
    bool compose(const QSize &size, GLTexture *desktop, bool useEstimate, const StereoDepthCoding &coding);

private:
    std::unique_ptr<GLShader> m_estimateShader;
    std::unique_ptr<GLShader> m_composeShader;
    std::unique_ptr<GLTexture> m_estimate;
    std::unique_ptr<GLFramebuffer> m_estimateFramebuffer;
    bool m_failed = false;
};

}

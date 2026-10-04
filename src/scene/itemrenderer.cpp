/*
    SPDX-FileCopyrightText: 2022 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/itemrenderer.h"

namespace KWin
{

ItemRenderer::ItemRenderer()
{
}

ItemRenderer::~ItemRenderer()
{
}

QPainter *ItemRenderer::painter() const
{
    return nullptr;
}

void ItemRenderer::beginFrame(const RenderTarget &renderTarget, const RenderViewport &viewport)
{
}

void ItemRenderer::endFrame()
{
}

void ItemRenderer::setLayerDebugging(bool enable)
{
}

StereoEye ItemRenderer::stereoEye() const
{
    return m_stereoEye;
}

void ItemRenderer::setStereoEye(StereoEye eye)
{
    m_stereoEye = eye;
}

bool ItemRenderer::stereoCapture() const
{
    return m_stereoCapture;
}

void ItemRenderer::setStereoCapture(bool enabled)
{
    m_stereoCapture = enabled;
}

} // namespace KWin

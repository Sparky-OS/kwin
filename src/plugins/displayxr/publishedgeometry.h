/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "core/output.h"
#include "window.h"

#include <QPoint>

#include <cmath>

namespace KWin
{

/**
 * The whole logical value that the runtime's conversion, round(value * scale), maps onto
 * @p devicePixel; the nearest one when only a fractional position reaches that pixel.
 */
inline int logicalForDevicePixel(int devicePixel, qreal scale)
{
    const int nearest = std::round(devicePixel / scale);
    for (const int candidate : {nearest, nearest - 1, nearest + 1}) {
        if (std::round(candidate * scale) == devicePixel) {
            return candidate;
        }
    }
    return nearest;
}

/**
 * The device pixel, relative to the window's output, at which KWin paints the window's frame when
 * the frame is at @p framePosition.
 */
inline QPoint paintedFramePosition(const Window *window, const QPointF &framePosition)
{
    const LogicalOutput *output = window->output();
    const qreal scale = output ? output->scale() : 1.0;
    const QPoint outputOrigin = output ? output->geometry().topLeft() : QPoint();
    return QPoint(std::round(framePosition.x() * scale) - std::round(outputOrigin.x() * scale),
                  std::round(framePosition.y() * scale) - std::round(outputOrigin.y() * scale));
}

/**
 * The device pixel, relative to the window's output, at which KWin paints the window's main
 * surface when the frame is at @p framePosition. Each scene item is snapped to the pixel grid
 * relative to its parent (ItemRendererOpenGL::createRenderNode), so the surface's offset in the
 * frame is rounded on its own.
 */
inline QPoint paintedSurfacePosition(const Window *window, const QPointF &framePosition)
{
    const qreal scale = window->output() ? window->output()->scale() : 1.0;
    const QPointF offset = window->bufferGeometry().topLeft() - window->frameGeometry().topLeft();
    return paintedFramePosition(window, framePosition) + QPoint(std::round(offset.x() * scale), std::round(offset.y() * scale));
}

/**
 * The whole logical position that the runtime converts onto @p devicePosition on the window's
 * output.
 */
inline QPoint publishedPosition(const Window *window, const QPoint &devicePosition)
{
    const LogicalOutput *output = window->output();
    const qreal scale = output ? output->scale() : 1.0;
    const QPoint outputOrigin = output ? output->geometry().topLeft() : QPoint();
    return outputOrigin + QPoint(logicalForDevicePixel(devicePosition.x(), scale), logicalForDevicePixel(devicePosition.y(), scale));
}

/**
 * The frame position at which KWin paints the window's main surface on the device pixel that the
 * runtime converts the published surface position @p surface onto: @p preferred when it does,
 * else the logical position of the device pixel the frame then needs.
 */
inline QPointF framePositionForSurface(const Window *window, const QPoint &surface, const QPointF &preferred)
{
    const LogicalOutput *output = window->output();
    const qreal scale = output ? output->scale() : 1.0;
    const QPoint outputOrigin = output ? output->geometry().topLeft() : QPoint();
    const QPoint surfaceShift = paintedSurfacePosition(window, QPointF());
    const auto axis = [scale](int surface, qreal preferred, int shift) -> qreal {
        const qreal frame = std::round(surface * scale) - shift;
        return std::round(preferred * scale) == frame ? preferred : frame / scale;
    };
    const QPoint relative = surface - outputOrigin;
    return QPointF(axis(relative.x(), preferred.x(), surfaceShift.x()), axis(relative.y(), preferred.y(), surfaceShift.y()));
}

/**
 * The top-left of the window's frame as it is published.
 */
inline QPoint publishedFramePosition(const Window *window)
{
    return publishedPosition(window, paintedFramePosition(window, window->pos()));
}

} // namespace KWin

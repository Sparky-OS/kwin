/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QPointF>
#include <QRectF>
#include <QSize>
#include <algorithm>
#include <cmath>

namespace KWin
{

struct HeadsetView
{
    enum class Mode {
        Fit,
        FullHeight,
        Zoom,
    };
    Mode mode = Mode::Fit;
    double zoom = 1.0;
    QPointF center = QPointF(0.5, 0.5);
    bool followPointer = true;
    bool centerActiveWindow = false;

    bool operator==(const HeadsetView &) const = default;

    bool isValid() const
    {
        return mode >= Mode::Fit && mode <= Mode::Zoom
            && std::isfinite(zoom) && zoom >= 1.0 && zoom <= 8.0
            && std::isfinite(center.x()) && std::isfinite(center.y())
            && center.x() >= 0 && center.x() <= 1 && center.y() >= 0 && center.y() <= 1;
    }

    QRectF viewport(const QRectF &source, const QSize &eye, const QPointF &focus) const
    {
        if (source.isEmpty() || eye.isEmpty() || mode == Mode::Fit) {
            return source;
        }
        const double height = source.height() / (mode == Mode::Zoom ? zoom : 1.0);
        const QSizeF size(height * eye.width() / eye.height(), height);
        const auto origin = [](double value, double start, double extent, double view) {
            return view >= extent ? start + (extent - view) / 2
                                  : std::clamp(value - view / 2, start, start + extent - view);
        };
        return QRectF(QPointF(origin(focus.x(), source.x(), source.width(), size.width()),
                              origin(focus.y(), source.y(), source.height(), size.height())), size);
    }
};

}

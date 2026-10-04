/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/output.h"
#include "core/rect.h"

#include <QVector3D>

#include <xf86drmMode.h>

namespace KWin
{

inline Rect stereoPairSourceRect(const drmModeModeInfo &mode, StereoPairRole role)
{
    return Rect(role == StereoPairRole::Right || role == StereoPairRole::Front ? QPoint(mode.hdisplay, 0) : QPoint(0, 0),
                QSize(mode.hdisplay, mode.vdisplay));
}

inline bool stereoPairRolesMatch(StereoPairMode mode, StereoPairRole first, StereoPairRole second)
{
    if (mode == StereoPairMode::Ized3d) {
        return (first == StereoPairRole::Back && second == StereoPairRole::Front)
            || (first == StereoPairRole::Front && second == StereoPairRole::Back);
    }
    return (first == StereoPairRole::Left && second == StereoPairRole::Right)
        || (first == StereoPairRole::Right && second == StereoPairRole::Left);
}

inline OutputTransform stereoPairOutputTransform(StereoPairReflection reflection)
{
    switch (reflection) {
    case StereoPairReflection::None:
        return OutputTransform::Normal;
    case StereoPairReflection::Horizontal:
        return OutputTransform::FlipX;
    case StereoPairReflection::Vertical:
        return OutputTransform::FlipY;
    }
    Q_UNREACHABLE();
}

struct Ized3dPixel {
    QVector3D back;
    QVector3D front;
};

// iZ3D's ordinary analytic mode, using the encoded source values as the
// original shader does. The epsilon branch is part of the device contract.
inline Ized3dPixel ized3dPixel(const QVector3D &left, const QVector3D &right)
{
    const QVector3D sum = left + right;
    QVector3D front(0.5f, 0.5f, 0.5f);
    for (int channel = 0; channel < 3; ++channel) {
        if (sum[channel] >= 0.000001f) {
            front[channel] = right[channel] / sum[channel];
        }
    }
    return {
        .back = sum * 0.5f,
        .front = front,
    };
}

}

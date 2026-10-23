/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QObject>

namespace KWin
{

class Window;

/**
 * Tells the programs that follow a surface's desktop depth (kde_stereo_depth_v1) whether the
 * elements they draw inside it may be shown in depth now and which of its edges lie on its output's
 * edge, from StereoDepth's own rules, whenever that may have changed: a window activated, raised,
 * moved, pinned or put on another output, an output switched into or out of a 3D mode.
 */
class StereoDepthStates : public QObject
{
    Q_OBJECT

public:
    explicit StereoDepthStates(QObject *parent = nullptr);

private:
    void watch(Window *window);
    void scheduleUpdate();
    void update();

    bool m_scheduled = false;
};

}

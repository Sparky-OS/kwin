/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/stereodepthstates.h"

#include "core/backendoutput.h"
#include "core/output.h"
#include "scene/stereodepth.h"
#include "wayland/surface.h"
#include "window.h"
#include "workspace.h"

namespace KWin
{

StereoDepthStates::StereoDepthStates(QObject *parent)
    : QObject(parent)
{
    Workspace *ws = workspace();
    connect(ws, &Workspace::windowAdded, this, [this](Window *window) {
        watch(window);
        scheduleUpdate();
    });
    connect(ws, &Workspace::windowRemoved, this, &StereoDepthStates::scheduleUpdate);
    connect(ws, &Workspace::windowActivated, this, &StereoDepthStates::scheduleUpdate);
    connect(ws, &Workspace::stackingOrderChanged, this, &StereoDepthStates::scheduleUpdate);
    connect(ws, &Workspace::currentDesktopChanged, this, &StereoDepthStates::scheduleUpdate);
    connect(ws, &Workspace::currentActivityChanged, this, &StereoDepthStates::scheduleUpdate);
    // an output switched into or out of a 3D mode
    auto watchOutput = [this](LogicalOutput *output) {
        connect(output->backendOutput(), &BackendOutput::changed, this, &StereoDepthStates::scheduleUpdate);
    };
    connect(ws, &Workspace::outputAdded, this, watchOutput);
    connect(ws, &Workspace::outputsChanged, this, &StereoDepthStates::scheduleUpdate);
    const auto outputs = ws->outputs();
    for (LogicalOutput *output : outputs) {
        watchOutput(output);
    }
    const auto windows = ws->windows();
    for (Window *window : windows) {
        watch(window);
    }
    scheduleUpdate();
}

void StereoDepthStates::watch(Window *window)
{
    connect(window, &Window::frameGeometryChanged, this, &StereoDepthStates::scheduleUpdate);
    connect(window, &Window::outputChanged, this, &StereoDepthStates::scheduleUpdate);
    connect(window, &Window::fullScreenChanged, this, &StereoDepthStates::scheduleUpdate);
    connect(window, &Window::keepAboveChanged, this, &StereoDepthStates::scheduleUpdate);
    connect(window, &Window::keepBelowChanged, this, &StereoDepthStates::scheduleUpdate);
    connect(window, &Window::minimizedChanged, this, &StereoDepthStates::scheduleUpdate);
    connect(window, &Window::hiddenChanged, this, &StereoDepthStates::scheduleUpdate);
    connect(window, &Window::desktopsChanged, this, &StereoDepthStates::scheduleUpdate);
    if (SurfaceInterface *surface = window->surface()) {
        // a surface may be followed before its window is shown, or again later
        connect(surface, &SurfaceInterface::stereoDepthFollowed, this, &StereoDepthStates::scheduleUpdate);
    }
}

void StereoDepthStates::scheduleUpdate()
{
    if (m_scheduled) {
        return;
    }
    m_scheduled = true;
    QMetaObject::invokeMethod(this, &StereoDepthStates::update, Qt::QueuedConnection);
}

void StereoDepthStates::update()
{
    m_scheduled = false;
    const auto windows = workspace()->windows();
    for (Window *window : windows) {
        SurfaceInterface *surface = window->surface();
        if (!surface || window->isDeleted()) {
            continue;
        }
        bool followed = false;
        surface->traverseTree([&followed](SurfaceInterface *child) {
            followed = followed || child->followsStereoDepth();
        });
        if (!followed) {
            continue;
        }
        const bool awake = StereoDepth::isAwake(window);
        const Qt::Edges edges = StereoDepth::outputEdges(window);
        surface->traverseTree([awake, edges](SurfaceInterface *child) {
            if (child->followsStereoDepth()) {
                child->setStereoDepthState(awake, edges);
            }
        });
    }
}

}

#include "moc_stereodepthstates.cpp"

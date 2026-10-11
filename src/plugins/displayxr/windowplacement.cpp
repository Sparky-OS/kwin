/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "windowplacement.h"
#include "window.h"
#include "windowplacement1adaptor.h"
#include "workspace.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>

namespace KWin
{

/**
 * The window of @p pid a runtime means: the active one, else the largest normal window, the
 * choice the runtime makes among its own windows.
 */
static Window *windowOfPid(pid_t pid)
{
    Window *best = nullptr;
    const auto windows = workspace()->windows();
    for (Window *window : windows) {
        if (window->pid() != pid || !window->isClient() || window->isDeleted() || !window->isNormalWindow()) {
            continue;
        }
        if (!best || window->isActive()) {
            best = window;
        } else if (!best->isActive() && window->width() * window->height() > best->width() * best->height()) {
            best = window;
        }
    }
    return best;
}

WindowPlacement::WindowPlacement()
{
    new WindowPlacement1Adaptor(this);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/displayxr/WindowPlacement"), this);
}

bool WindowPlacement::MoveWindow(uint pid, int x, int y)
{
    // A client moves only its own window: the process is the bus connection's, the argument must
    // name it or be 0.
    const QDBusReply<uint> caller = connection().interface()->servicePid(message().service());
    if (!caller.isValid() || (pid != 0 && pid != caller.value())) {
        return false;
    }
    Window *window = windowOfPid(caller.value());
    // Never against the user: while they drag or resize the window, its position is theirs.
    if (!window || window->isInteractiveMove() || window->isInteractiveResize() || window->isFullScreen() || !window->isMovable()) {
        return false;
    }
    window->move(QPointF(x, y));
    return true;
}

uint WindowPlacement::GetPlacementCapabilities() const
{
    return 0;
}

bool WindowPlacement::SetDragLattice(uint, bool, int, int, int, int, int, const QList<int> &, const QList<int> &, int &startX, int &startY)
{
    startX = 0;
    startY = 0;
    return false;
}

bool WindowPlacement::SetDragLatticeAt(uint, int, int, bool, int, int, int, int, int, const QList<int> &, const QList<int> &, int &acceptedStartX, int &acceptedStartY)
{
    acceptedStartX = 0;
    acceptedStartY = 0;
    return false;
}

void WindowPlacement::ClearDragLattice(uint)
{
}

bool WindowPlacement::EnableMoveSync(uint)
{
    return false;
}

bool WindowPlacement::BeginPointerDrag(uint, uint)
{
    return false;
}

void WindowPlacement::EndPointerDrag(uint)
{
}

} // namespace KWin

#include "moc_windowplacement.cpp"

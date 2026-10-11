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

// GetPlacementCapabilities() bits.
static constexpr uint s_dragLattice = 1 << 0;
static constexpr uint s_dragLatticeAt = 1 << 1;

WindowPlacement::WindowPlacement()
{
    connect(&m_lattice, &DragLattice::needed, this, &WindowPlacement::DragLatticeNeeded);
    connect(&m_lattice, &DragLattice::done, this, &WindowPlacement::DragLatticeDone);
    new WindowPlacement1Adaptor(this);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/displayxr/WindowPlacement"), this);
}

bool WindowPlacement::latticeDrop(const Window *window) const
{
    return m_lattice.landedOnTable(window);
}

std::optional<pid_t> WindowPlacement::callerPid(uint pid) const
{
    // A client acts only on its own windows: the process is the bus connection's, the argument
    // must name it or be 0.
    const QDBusReply<uint> caller = connection().interface()->servicePid(message().service());
    if (!caller.isValid() || (pid != 0 && pid != caller.value())) {
        return std::nullopt;
    }
    return caller.value();
}

bool WindowPlacement::MoveWindow(uint pid, int x, int y)
{
    const std::optional<pid_t> caller = callerPid(pid);
    if (!caller) {
        return false;
    }
    Window *window = windowOfPid(*caller);
    // Never against the user: while they drag or resize the window, its position is theirs.
    if (!window || window->isInteractiveMove() || window->isInteractiveResize() || window->isFullScreen() || !window->isMovable()) {
        return false;
    }
    window->move(QPointF(x, y));
    return true;
}

uint WindowPlacement::GetPlacementCapabilities() const
{
    return s_dragLattice | s_dragLatticeAt;
}

bool WindowPlacement::SetDragLattice(uint pid, bool extend, int cell, int minDx, int minDy, int maxDx, int maxDy, const QList<int> &dx, const QList<int> &dy, int &startX, int &startY)
{
    return setDragLattice(pid, std::nullopt, extend, cell, QRect(QPoint(minDx, minDy), QPoint(maxDx, maxDy)), dx, dy, startX, startY);
}

bool WindowPlacement::SetDragLatticeAt(uint pid, int startX, int startY, bool extend, int cell, int minDx, int minDy, int maxDx, int maxDy, const QList<int> &dx, const QList<int> &dy, int &acceptedStartX, int &acceptedStartY)
{
    return setDragLattice(pid, QPoint(startX, startY), extend, cell, QRect(QPoint(minDx, minDy), QPoint(maxDx, maxDy)), dx, dy, acceptedStartX, acceptedStartY);
}

bool WindowPlacement::setDragLattice(uint pid, std::optional<QPoint> start, bool extend, int cell, const QRect &bounds, const QList<int> &dx, const QList<int> &dy, int &startX, int &startY)
{
    startX = 0;
    startY = 0;
    const std::optional<pid_t> caller = callerPid(pid);
    if (!caller) {
        return false;
    }
    Window *window = windowOfPid(*caller);
    if (!window || !m_lattice.set(window, *caller, extend, cell, bounds, dx, dy, start)) {
        return false;
    }
    // The start the table was recorded for, so the client can check it against its own.
    const QPoint recorded = m_lattice.startOf(window).value();
    startX = recorded.x();
    startY = recorded.y();
    return true;
}

void WindowPlacement::ClearDragLattice(uint pid)
{
    if (const std::optional<pid_t> caller = callerPid(pid)) {
        if (const Window *window = windowOfPid(*caller)) {
            m_lattice.clear(window);
        }
    }
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

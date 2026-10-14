/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "draglattice.h"
#include "movesync.h"

#include <QDBusContext>
#include <QObject>

namespace KWin
{

class Window;

/**
 * org.displayxr.WindowPlacement1: moves the caller's own window, keeps it on the caller's drag
 * table while the user drags it (DragLattice), or paints it where each frame was woven for
 * (MoveSync). The pointer drag is not implemented; GetPlacementCapabilities() says so and its
 * methods refuse.
 */
class WindowPlacement : public QObject, protected QDBusContext
{
    Q_OBJECT

public:
    explicit WindowPlacement();

    bool latticeDrop(const Window *window) const;
    bool moveSynced(const Window *window) const;
    bool moveSyncHolding(Window *window) const;

public Q_SLOTS:
    bool MoveWindow(uint pid, int x, int y);
    uint GetPlacementCapabilities() const;
    bool SetDragLattice(uint pid, bool extend, int cell, int minDx, int minDy, int maxDx, int maxDy, const QList<int> &dx, const QList<int> &dy, int &startX, int &startY);
    bool SetDragLatticeAt(uint pid, int startX, int startY, bool extend, int cell, int minDx, int minDy, int maxDx, int maxDy, const QList<int> &dx, const QList<int> &dy, int &acceptedStartX, int &acceptedStartY);
    void ClearDragLattice(uint pid);
    bool EnableMoveSync(uint pid);
    bool BeginPointerDrag(uint pid, uint button);
    void EndPointerDrag(uint pid);

Q_SIGNALS:
    void DragLatticeNeeded(uint pid, int dx, int dy);
    void DragLatticeDone(uint pid, uint moves, uint corrected, uint misses, uint maxCorrection, uint tables, bool landedOnTable);
    /**
     * A process enabled move sync, or its registration ended with its bus connection.
     */
    void moveSyncChanged();
    /**
     * Move sync began or ended holding a window where its frames were woven for, so the pixels the
     * snapshot names for it changed.
     */
    void moveSyncHoldChanged();

private:
    std::optional<pid_t> callerPid(uint pid) const;
    bool setDragLattice(uint pid, std::optional<QPoint> start, bool extend, int cell, const QRect &bounds, const QList<int> &dx, const QList<int> &dy, int &startX, int &startY);

    DragLattice m_lattice;
    MoveSync m_moveSync;
};

} // namespace KWin

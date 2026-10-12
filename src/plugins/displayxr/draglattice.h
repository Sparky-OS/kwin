/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSet>

#include <chrono>
#include <optional>

namespace KWin
{

class LogicalOutput;
class Window;

/**
 * The drag table of a runtime that weaves for a panel: the positions, as whole logical
 * displacements from the drag start, where the client's display processor finds the window's
 * phase correct. While the user drags the window, every position the move proposes is replaced by
 * a table entry before the window is moved there, so no frame shows it anywhere else.
 *
 * The client builds its table for its main surface at the published start plus an entry, painted
 * at round(position * scale), one rounding as Mutter paints it. KWin rounds the frame's position
 * and the surface's offset in the frame separately, so each entry's frame position is chosen to
 * put the surface on the client's pixel.
 */
class DragLattice : public QObject
{
    Q_OBJECT

public:
    explicit DragLattice();
    ~DragLattice() override;

    bool set(Window *window, pid_t pid, bool extend, int cell, const QRect &bounds, const QList<int> &dx, const QList<int> &dy, std::optional<QPoint> start);
    void clear(const Window *window);
    std::optional<QPoint> startOf(const Window *window) const;
    /**
     * Whether the window's last drag ended on its table and the window has not moved since.
     */
    bool landedOnTable(const Window *window) const;

Q_SIGNALS:
    void needed(uint pid, int dx, int dy);
    void done(uint pid, uint moves, uint corrected, uint misses, uint maxCorrection, uint tables, bool landedOnTable);

private:
    /**
     * Which entry a move lands on, as the GNOME extension's LatticeChoice picks it: every entry is
     * equally phase correct, so the error goes where it shows least. A sideways error costs three
     * times an error along the drag, and the lead or lag along it is capped. Device pixels.
     */
    struct Choice
    {
        void observe(const QPointF &displacement, qreal scale);
        std::optional<QPoint> choose(const QList<QPoint> &entries, const QPointF &displacement, qreal scale, bool nearest) const;

        double axisX = 0;
        double axisY = 0;
        QPointF last;
    };

    struct Statistics
    {
        uint moves = 0;
        uint corrected = 0;
        uint misses = 0;
        double maxCorrection = 0;
        uint tables = 0;
        uint paintedOn = 0;
        uint paintedOff = 0;
        uint paintedOutside = 0;
    };

    struct Table
    {
        pid_t pid = 0;
        QPoint start;
        QPoint surface;
        int cell = 1;
        QRect bounds;
        QRect piece;
        QHash<QPoint, QList<QPoint>> buckets;
        QSet<QPoint> members;
        bool grabbing = false;
        bool asked = false;
        std::chrono::steady_clock::time_point expires;
        Choice choice;
        std::optional<std::chrono::steady_clock::time_point> lastMove;
        QPointF lastDisplacement;
    };

    struct Published
    {
        QPoint frame;
        QPoint surface;
    };

    QPointF constrain(const Window *window, const QPointF &position);
    QPoint surfaceAt(const Window *window, const QPoint &start) const;
    static QPointF positionOf(const Table &table, const Window *window, const QPoint &entry);
    std::optional<QPoint> entryAt(const Table &table, const Window *window, const QPointF &position) const;
    QList<QPoint> candidates(const Table &table, const QPointF &displacement) const;
    bool covers(const Table &table, const QPointF &displacement) const;
    void askAhead(Table &table, const QPointF &displacement);
    void ask(const Table &table, const QPointF &displacement);
    void track(Window *window);
    void grabBegan(Window *window);
    void grabEnded(Window *window);
    void forget(const Window *window);
    void audit(const LogicalOutput *output);

    QHash<const Window *, Table> m_tables;
    QHash<const Window *, Statistics> m_statistics;
    QHash<const Window *, QPointF> m_landed;
    QHash<const Window *, Published> m_pressed;
    const bool m_nearest;
    const bool m_debug;
};

} // namespace KWin

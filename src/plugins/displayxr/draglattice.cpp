/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "draglattice.h"
#include "core/backendoutput.h"
#include "core/output.h"
#include "core/renderloop.h"
#include "displayxrlogging.h"
#include "publishedgeometry.h"
#include "scene/windowitem.h"
#include "window.h"
#include "workspace.h"

#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace std::chrono_literals;

namespace KWin
{

// A table no drag follows stops applying after this long.
static constexpr auto s_tableLifetime = 2s;

// The choice's constants, as the GNOME extension has them (LatticeChoice in its lib.js).
static constexpr double s_acrossWeight = 9;
static constexpr double s_maxAlong = 6;
static constexpr double s_directionMemory = 8;
static constexpr double s_minTravel = 4;

void DragLattice::Choice::observe(const QPointF &displacement, qreal scale)
{
    const double moveX = (displacement.x() - last.x()) * scale;
    const double moveY = (displacement.y() - last.y()) * scale;
    const double length = std::hypot(moveX, moveY);
    if (length > 0) {
        // The direction is an axis: a drag that reverses along the same line keeps it.
        const double sign = moveX * axisX + moveY * axisY < 0 ? -1 : 1;
        const double decay = std::exp(-length / s_directionMemory);
        axisX = axisX * decay + sign * moveX;
        axisY = axisY * decay + sign * moveY;
    }
    last = displacement;
}

std::optional<QPoint> DragLattice::Choice::choose(const QList<QPoint> &entries, const QPointF &displacement, qreal scale, bool nearest) const
{
    const double length = std::hypot(axisX, axisY);
    const bool directed = !nearest && length >= s_minTravel;
    const double unitX = directed ? axisX / length : 1;
    const double unitY = directed ? axisY / length : 0;
    std::optional<QPoint> best;
    std::optional<QPoint> closest;
    double bestCost = std::numeric_limits<double>::infinity();
    double closestDistance = std::numeric_limits<double>::infinity();
    for (const QPoint &entry : entries) {
        const double errorX = (entry.x() - displacement.x()) * scale;
        const double errorY = (entry.y() - displacement.y()) * scale;
        const double distance = errorX * errorX + errorY * errorY;
        if (distance < closestDistance) {
            closestDistance = distance;
            closest = entry;
        }
        if (!directed) {
            continue;
        }
        const double along = errorX * unitX + errorY * unitY;
        const double across = errorY * unitX - errorX * unitY;
        if (std::abs(along) > s_maxAlong) {
            continue;
        }
        const double cost = along * along + s_acrossWeight * across * across;
        if (cost < bestCost) {
            bestCost = cost;
            best = entry;
        }
    }
    return best ? best : closest;
}

static QPoint bucketOf(const QPointF &displacement, int cell)
{
    return QPoint(std::floor(displacement.x() / cell), std::floor(displacement.y() / cell));
}

DragLattice::DragLattice()
    : m_nearest(qEnvironmentVariableIntValue("DISPLAYXR_LATTICE_NEAREST") == 1)
    , m_debug(qEnvironmentVariableIntValue("DISPLAYXR_DEBUG") == 1)
{
    workspace()->setInteractiveMoveConstraint([this](const Window *window, const QPointF &position) {
        return constrain(window, position);
    });
    const auto windows = workspace()->windows();
    for (Window *window : windows) {
        track(window);
    }
    connect(workspace(), &Workspace::windowAdded, this, &DragLattice::track);
    connect(workspace(), &Workspace::windowRemoved, this, &DragLattice::forget);

    // DISPLAYXR_DEBUG=1: check every frame that a window dragged with a table is painted on it.
    if (m_debug) {
        const auto watch = [this](LogicalOutput *output) {
            connect(output->backendOutput()->renderLoop(), &RenderLoop::frameRequested, this, [this, output]() {
                audit(output);
            });
        };
        const auto outputs = workspace()->outputs();
        for (LogicalOutput *output : outputs) {
            watch(output);
        }
        connect(workspace(), &Workspace::outputAdded, this, watch);
    }
}

DragLattice::~DragLattice()
{
    if (workspace()) {
        workspace()->setInteractiveMoveConstraint({});
    }
}

bool DragLattice::set(Window *window, pid_t pid, bool extend, int cell, const QRect &bounds, const QList<int> &dx, const QList<int> &dy, std::optional<QPoint> start)
{
    if (dx.size() != dy.size() || cell < 1) {
        return false;
    }
    const auto previous = m_tables.constFind(window);
    const bool havePrevious = previous != m_tables.constEnd();
    // Every table of one drag is relative to the same start. A fresh one takes the window's position
    // now, as it is published, before the drag moves the window away.
    if (!start) {
        start = extend && havePrevious ? previous->start : publishedFramePosition(window);
    }
    // Pieces of one drag add up: all of them are phase correct for the same start.
    const bool merge = extend && havePrevious && previous->start == *start;

    Table table = merge ? *previous : Table();
    table.pid = pid;
    table.start = *start;
    table.surface = merge ? previous->surface : surfaceAt(window, *start);
    table.cell = cell;
    table.piece = bounds;
    table.bounds = merge ? previous->bounds.united(bounds) : bounds;
    for (qsizetype i = 0; i < dx.size(); ++i) {
        const QPoint entry(dx[i], dy[i]);
        table.buckets[bucketOf(entry, cell)].append(entry);
        table.members.insert(entry);
    }
    table.asked = false;
    // A table can arrive in the middle of a drag, when the window reaches the client's panel.
    table.grabbing = (havePrevious && previous->grabbing) || window->isInteractiveMove();
    table.expires = std::chrono::steady_clock::now() + s_tableLifetime;
    table.lastMove.reset();
    m_tables[window] = table;

    // A fresh table outside a drag starts a new drag's statistics.
    if (!extend && !window->isInteractiveMove()) {
        m_statistics[window] = Statistics();
    }
    m_statistics[window].tables++;
    m_landed.remove(window);

    qCDebug(KWIN_DISPLAYXR) << "drag table: pid" << pid << "extend" << extend << "entries" << dx.size() << "cell" << cell
                            << "bounds" << table.bounds << "start" << table.start;

    // A table that arrives during the drag takes the window onto it now, not at the next move.
    if (table.grabbing) {
        const QPointF next = constrain(window, window->pos());
        if (next != window->pos()) {
            window->move(next);
        }
    }
    return true;
}

void DragLattice::clear(const Window *window)
{
    m_tables.remove(window);
}

std::optional<QPoint> DragLattice::startOf(const Window *window) const
{
    const auto table = m_tables.constFind(window);
    if (table == m_tables.constEnd()) {
        return std::nullopt;
    }
    return table->start;
}

bool DragLattice::landedOnTable(const Window *window) const
{
    const auto landed = m_landed.constFind(window);
    return landed != m_landed.constEnd() && window->pos() == *landed;
}

QPointF DragLattice::constrain(const Window *window, const QPointF &position)
{
    const auto it = m_tables.find(window);
    if (it == m_tables.end() || !it->grabbing) {
        return position;
    }
    Table &table = *it;
    Statistics &statistics = m_statistics[window];
    statistics.moves++;

    const QPointF displacement = position - table.start;
    const qreal scale = window->output() ? window->output()->scale() : 1.0;
    table.choice.observe(displacement, scale);
    const std::optional<QPoint> entry = covers(table, displacement) ? table.choice.choose(candidates(table, displacement), displacement, scale, m_nearest) : std::nullopt;
    if (!entry) {
        // Off the table the drag goes on unsnapped, and the client is asked for more, once.
        statistics.misses++;
        if (!table.asked) {
            table.asked = true;
            ask(table, displacement);
        }
        return position;
    }
    askAhead(table, displacement);

    const QPointF next = positionOf(table, window, *entry);
    if (next != position) {
        statistics.corrected++;
        statistics.maxCorrection = std::max({statistics.maxCorrection, std::abs(next.x() - position.x()), std::abs(next.y() - position.y())});
    }
    if (m_debug) {
        // One line per move, for comparing drags: where the move proposed the window and where it lands.
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch());
        const QPoint surface = paintedSurfacePosition(window, next);
        qCDebug(KWIN_DISPLAYXR, "drag table move t=%lld proposed=%.3f,%.3f landed=%d,%d scale=%.6f surface=%d,%d", qlonglong(now.count()),
                displacement.x(), displacement.y(), entry->x(), entry->y(), scale, surface.x(), surface.y());
    }
    return next;
}

QPoint DragLattice::surfaceAt(const Window *window, const QPoint &start) const
{
    // Where the snapshot had the main surface when it had the frame at the start: now, or at the
    // press, when the client read it; otherwise where it has it with the frame exactly there.
    if (publishedFramePosition(window) == start) {
        return publishedPosition(window, paintedSurfacePosition(window, window->pos()));
    }
    const auto pressed = m_pressed.constFind(window);
    if (pressed != m_pressed.constEnd() && pressed->frame == start) {
        return pressed->surface;
    }
    return publishedPosition(window, paintedSurfacePosition(window, start));
}

QPointF DragLattice::positionOf(const Table &table, const Window *window, const QPoint &entry) const
{
    // The client's pixel for the entry is its table's surface position plus the entry, converted as
    // the runtime converts it. The frame goes where KWin paints the main surface on that pixel: at
    // the whole position the entry names when that does it, else on the pixel the frame needs.
    const LogicalOutput *output = window->output();
    const qreal scale = output ? output->scale() : 1.0;
    const QPoint outputOrigin = output ? output->geometry().topLeft() : QPoint();
    const QPoint surfaceShift = paintedSurfacePosition(window, QPointF());
    const auto axis = [scale](int surface, int whole, int shift) -> qreal {
        const qreal frame = std::round(surface * scale) - shift;
        return std::round(whole * scale) == frame ? whole : frame / scale;
    };
    const QPoint surface = table.surface + entry - outputOrigin;
    const QPoint whole = table.start + entry;
    return QPointF(axis(surface.x(), whole.x(), surfaceShift.x()), axis(surface.y(), whole.y(), surfaceShift.y()));
}

std::optional<QPoint> DragLattice::entryAt(const Table &table, const Window *window, const QPointF &position) const
{
    // An entry's frame position is less than two logical pixels from the whole position it names.
    const QPoint nearest = (position - table.start).toPoint();
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            const QPoint entry = nearest + QPoint(i, j);
            if (table.members.contains(entry) && positionOf(table, window, entry) == position) {
                return entry;
            }
        }
    }
    return std::nullopt;
}

QList<QPoint> DragLattice::candidates(const Table &table, const QPointF &displacement) const
{
    // The entries in the 5 x 5 cells around the proposed position.
    QList<QPoint> entries;
    const QPoint bucket = bucketOf(displacement, table.cell);
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            entries.append(table.buckets.value(bucket + QPoint(i, j)));
        }
    }
    return entries;
}

bool DragLattice::covers(const Table &table, const QPointF &displacement) const
{
    return displacement.x() >= table.bounds.left() && displacement.x() <= table.bounds.right() && displacement.y() >= table.bounds.top()
        && displacement.y() <= table.bounds.bottom() && !candidates(table, displacement).isEmpty();
}

void DragLattice::askAhead(Table &table, const QPointF &displacement)
{
    // Ask for the next piece once the drag is half-way from the newest piece's centre to its edge,
    // for where the window will be about 150 ms later, so the client probes it before it is needed.
    const auto now = std::chrono::steady_clock::now();
    QPointF velocity;
    if (table.lastMove && now > *table.lastMove) {
        velocity = (displacement - table.lastDisplacement) / std::chrono::duration<double>(now - *table.lastMove).count();
    }
    table.lastMove = now;
    table.lastDisplacement = displacement;
    if (table.asked) {
        return;
    }
    const QPointF centre((table.piece.left() + table.piece.right()) / 2.0, (table.piece.top() + table.piece.bottom()) / 2.0);
    const double half = std::min(table.piece.right() - table.piece.left(), table.piece.bottom() - table.piece.top()) / 2.0;
    if (std::max(std::abs(displacement.x() - centre.x()), std::abs(displacement.y() - centre.y())) < half / 2) {
        return;
    }
    const auto lead = [limit = half / 2](double speed) {
        return std::clamp(speed * 0.15, -limit, limit);
    };
    table.asked = true;
    ask(table, displacement + QPointF(lead(velocity.x()), lead(velocity.y())));
}

void DragLattice::ask(const Table &table, const QPointF &displacement)
{
    // Never from inside the move: the signal goes out on the next event loop turn.
    const QPoint at = displacement.toPoint();
    QTimer::singleShot(0, this, [this, pid = table.pid, at]() {
        Q_EMIT needed(pid, at.x(), at.y());
    });
}

void DragLattice::track(Window *window)
{
    connect(window, &Window::interactiveMoveResizeStarted, this, [this, window]() {
        grabBegan(window);
    });
    connect(window, &Window::interactiveMoveResizeFinished, this, [this, window]() {
        grabEnded(window);
    });
}

void DragLattice::grabBegan(Window *window)
{
    m_landed.remove(window);
    // What the snapshot says at the press, which a table that arrives later was built for.
    m_pressed[window] = Published{
        .frame = publishedFramePosition(window),
        .surface = publishedPosition(window, paintedSurfacePosition(window, window->pos())),
    };
    const auto table = m_tables.find(window);
    if (table == m_tables.end()) {
        return;
    }
    if (!table->grabbing && std::chrono::steady_clock::now() > table->expires) {
        m_tables.erase(table);
        return;
    }
    table->grabbing = true;
}

void DragLattice::grabEnded(Window *window)
{
    m_pressed.remove(window);
    if (!m_tables.contains(window)) {
        return;
    }
    const Table table = m_tables.take(window);
    const Statistics statistics = m_statistics.take(window);
    const bool onTable = entryAt(table, window, window->pos()).has_value();
    if (onTable) {
        m_landed[window] = window->pos();
    }
    qCDebug(KWIN_DISPLAYXR) << "drag table done:" << statistics.moves << "moves," << statistics.corrected << "corrected (largest"
                            << statistics.maxCorrection << "logical px)," << statistics.misses << "off the table; painted on it"
                            << statistics.paintedOn << "frames, off it" << statistics.paintedOff << ", outside it" << statistics.paintedOutside
                            << "; landed on it" << onTable;
    Q_EMIT done(table.pid, statistics.moves, statistics.corrected, statistics.misses, std::round(statistics.maxCorrection), statistics.tables, onTable);
}

void DragLattice::forget(const Window *window)
{
    m_tables.remove(window);
    m_statistics.remove(window);
    m_landed.remove(window);
    m_pressed.remove(window);
}

void DragLattice::audit(const LogicalOutput *output)
{
    for (auto it = m_tables.cbegin(); it != m_tables.cend(); ++it) {
        const Window *window = it.key();
        if (!it->grabbing || window->output() != output) {
            continue;
        }
        // What this frame paints: the window item, at the frame's position.
        const QPointF painted = window->windowItem() ? window->windowItem()->position() : window->pos();
        const QPointF displacement = painted - it->start;
        Statistics &statistics = m_statistics[window];
        if (entryAt(*it, window, painted)) {
            statistics.paintedOn++;
        } else if (!covers(*it, displacement)) {
            // Beyond the table the drag goes on unsnapped until the client sends more.
            statistics.paintedOutside++;
        } else {
            statistics.paintedOff++;
            if (statistics.paintedOff <= 5) {
                qCDebug(KWIN_DISPLAYXR) << "drag table: painted off the table at" << displacement;
            }
        }
    }
}

} // namespace KWin

#include "moc_draglattice.cpp"

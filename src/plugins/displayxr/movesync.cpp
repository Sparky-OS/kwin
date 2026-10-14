/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "movesync.h"
#include "core/backendoutput.h"
#include "core/output.h"
#include "core/renderloop.h"
#include "displayxrlogging.h"
#include "publishedgeometry.h"
#include "scene/windowitem.h"
#include "wayland/subcompositor.h"
#include "wayland/surface.h"
#include "window.h"
#include "workspace.h"

#include <QDBusConnection>

#include <cmath>

using namespace std::chrono_literals;

namespace KWin
{

// The tag is the woven position modulo this, in logical px; the runtime's
// COMP_VK_NATIVE_WL_MOVE_SYNC_TAG_MOD.
static constexpr int s_tagModulus = 256;
// The rules' constants, as the GNOME extension has them (MoveSyncChoice in its lib.js).
static constexpr auto s_historyAge = 1s;
static constexpr qsizetype s_historySize = 256;
static constexpr auto s_timeout = 100ms;
static constexpr uint s_timeoutFrames = 6;
static constexpr auto s_resync = 60ms;
static constexpr auto s_settle = 300ms;

static int tagCoordinate(int value)
{
    return ((value % s_tagModulus) + s_tagModulus) % s_tagModulus;
}

// The clock of the debug lines, CLOCK_MONOTONIC in microseconds.
static qlonglong microseconds(std::chrono::steady_clock::time_point time)
{
    return std::chrono::duration_cast<std::chrono::microseconds>(time.time_since_epoch()).count();
}

MoveSync::MoveSync()
    : m_debug(qEnvironmentVariableIntValue("DISPLAYXR_DEBUG") == 1)
{
    m_watcher.setConnection(QDBusConnection::sessionBus());
    m_watcher.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(&m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, &MoveSync::dropClient);

    const auto windows = workspace()->windows();
    for (Window *window : windows) {
        track(window);
    }
    connect(workspace(), &Workspace::windowAdded, this, &MoveSync::track);
    connect(workspace(), &Workspace::windowRemoved, this, [this](Window *window) {
        if (m_holds.contains(window)) {
            release(window, "closed");
        }
    });

    // After each frame: the timeout, the tick at a stop and the settling after the move.
    const auto watch = [this](LogicalOutput *output) {
        connect(output->backendOutput()->renderLoop(), &RenderLoop::frameRequested, this, [this, output]() {
            frame(output);
        });
    };
    const auto outputs = workspace()->outputs();
    for (LogicalOutput *output : outputs) {
        watch(output);
    }
    connect(workspace(), &Workspace::outputAdded, this, watch);
}

MoveSync::~MoveSync()
{
    const auto windows = m_holds.keys();
    for (Window *window : windows) {
        release(window, "disabled");
    }
}

void MoveSync::enable(const QString &service, pid_t pid)
{
    if (!m_clients.contains(service)) {
        m_watcher.addWatchedService(service);
    }
    m_clients[service].insert(pid);
    qCDebug(KWIN_DISPLAYXR) << "move sync enabled for pid" << pid;
    Q_EMIT clientsChanged();
}

bool MoveSync::isHeld(Window *window) const
{
    const auto it = m_holds.constFind(window);
    return it != m_holds.constEnd() && it->held;
}

bool MoveSync::isEnabled(pid_t pid) const
{
    for (const QSet<pid_t> &pids : m_clients) {
        if (pids.contains(pid)) {
            return true;
        }
    }
    return false;
}

void MoveSync::dropClient(const QString &service)
{
    m_watcher.removeWatchedService(service);
    m_clients.remove(service);
    Q_EMIT clientsChanged();
    const auto windows = m_holds.keys();
    for (Window *window : windows) {
        if (!isEnabled(window->pid())) {
            release(window, "client gone");
        }
    }
}

void MoveSync::track(Window *window)
{
    connect(window, &Window::interactiveMoveResizeStarted, this, [this, window]() {
        begin(window);
    });
    connect(window, &Window::interactiveMoveResizeFinished, this, [this, window]() {
        end(window);
    });
}

void MoveSync::begin(Window *window)
{
    if (!window->isInteractiveMove() || window->isFullScreen() || !window->surface() || !window->windowItem() || !isEnabled(window->pid())) {
        return;
    }
    if (const auto it = m_holds.find(window); it != m_holds.end()) {
        // Grabbed again while settling: the same hold goes on.
        it->grabbing = true;
        return;
    }
    const auto now = Clock::now();
    Hold hold;
    hold.history.append(Position{publishedSurfacePosition(window), window->pos(), now});
    hold.shown = hold.history.last();
    hold.progress = now;
    hold.size = window->frameGeometry().size();
    // The window is held only while a tag is mapped; until one is, it moves as any window.
    hold.held = tagOf(window).has_value();
    hold.statistics.holds = hold.held ? 1 : 0;
    hold.connections.append(connect(window, &Window::frameGeometryChanged, this, [this, window]() {
        moved(window);
    }));
    hold.connections.append(connect(window->surface(), &SurfaceInterface::committed, this, [this, window]() {
        committed(window);
    }));
    // The window item follows the window's position; while the window is held, it goes back.
    hold.connections.append(connect(window->windowItem(), &Item::positionChanged, this, [this, window]() {
        if (const auto it = m_holds.constFind(window); it != m_holds.constEnd()) {
            show(window, *it);
        }
    }));
    m_holds.insert(window, hold);
    qCDebug(KWIN_DISPLAYXR, "move sync begin t=%lld pid=%d %s", microseconds(now), window->pid(), hold.held ? "held" : "plain");
}

void MoveSync::end(Window *window)
{
    const auto it = m_holds.find(window);
    if (it == m_holds.end()) {
        return;
    }
    it->grabbing = false;
    it->settleUntil = Clock::now() + s_settle;
    if (window->maximizeMode() != MaximizeRestore || window->isFullScreen()) {
        release(window, "maximized");
    } else if (!it->held) {
        release(window, "plain move");
    } else {
        // Give the frame woven for the last position the time to land.
        window->windowItem()->scheduleFrame();
    }
}

void MoveSync::moved(Window *window)
{
    const auto it = m_holds.find(window);
    if (it == m_holds.end()) {
        return;
    }
    Hold &hold = *it;
    if (window->frameGeometry().size() != hold.size) {
        // A resize (tiling, maximizing) is not a move any more.
        release(window, "resized");
        return;
    }
    const auto now = Clock::now();
    const Position current{publishedSurfacePosition(window), window->pos(), now};
    // The window starts moving away from a caught-up hold: the timeout counts from here.
    if (hold.shown.published == hold.history.last().published) {
        hold.progress = now;
        hold.progressFrames = 0;
    }
    hold.history.append(current);
    while (hold.history.size() > 2 && (now - hold.history.first().time > s_historyAge || hold.history.size() > s_historySize)) {
        hold.history.removeFirst();
    }
    if (hold.followingSince) {
        hold.shown = current;
    }
    show(window, hold);
}

void MoveSync::committed(Window *window)
{
    const auto it = m_holds.find(window);
    if (it == m_holds.end()) {
        return;
    }
    Hold &hold = *it;
    const std::optional<QPoint> tag = tagOf(window);
    const auto now = Clock::now();
    const Position current = hold.history.last();
    if (!hold.held) {
        if (!tag) {
            hold.statistics.untagged++;
            return;
        }
        // A tag appeared: the hold starts where the window is, and the kept history resolves it.
        hold.held = true;
        hold.shown = current;
        hold.progress = now;
        hold.progressFrames = 0;
        hold.followingSince.reset();
        hold.lastHistorySize = hold.history.size();
        hold.statistics.holds++;
        qCDebug(KWIN_DISPLAYXR, "move sync tag appeared t=%lld held", microseconds(now));
        Q_EMIT heldChanged();
    } else if (!tag) {
        // The tag went away: what is painted now is woven for no position, so the window item goes
        // to the window at once.
        hold.held = false;
        hold.followingSince.reset();
        hold.shown = current;
        hold.statistics.tagLost++;
        show(window, hold);
        qCDebug(KWIN_DISPLAYXR, "move sync tag gone t=%lld plain", microseconds(now));
        if (!hold.grabbing) {
            release(window, "tag unmapped");
        } else {
            Q_EMIT heldChanged();
        }
        return;
    }

    // The newest position the window had that the tag names.
    std::optional<Position> woven;
    for (auto position = hold.history.crbegin(); position != hold.history.crend(); ++position) {
        if (tagCoordinate(position->published.x()) == tag->x() && tagCoordinate(position->published.y()) == tag->y()) {
            woven = *position;
            break;
        }
    }
    if (hold.followingSince) {
        // Following the window after a stall: back to the woven frames with the first one woven for
        // a position the window had since, and recently.
        if (woven && woven->time >= *hold.followingSince && woven->time >= current.time - s_resync) {
            qCDebug(KWIN_DISPLAYXR, "move sync resync t=%lld on a frame woven for %d,%d", microseconds(now), woven->published.x(), woven->published.y());
            hold.followingSince.reset();
            if (woven->time < hold.shown.time) {
                hold.statistics.backHops++;
            }
            hold.shown = *woven;
            hold.progress = now;
            hold.progressFrames = 0;
            hold.statistics.advanced++;
        } else {
            hold.shown = current;
        }
    } else if (woven && woven->published != hold.shown.published) {
        // By position, the newest match: at a reversal either match is the same pixel.
        if (woven->time < hold.shown.time) {
            hold.statistics.backHops++;
        }
        hold.shown = *woven;
        hold.progress = now;
        hold.progressFrames = 0;
        hold.statistics.advanced++;
    }
    show(window, hold);
}

void MoveSync::frame(const LogicalOutput *output)
{
    const auto now = Clock::now();
    const auto windows = m_holds.keys();
    for (Window *window : windows) {
        const auto it = m_holds.find(window);
        if (it == m_holds.end() || window->output() != output) {
            continue;
        }
        Hold &hold = *it;
        Statistics &statistics = hold.statistics;
        if (!hold.held) {
            // A plain move: the window item is where the window is.
            statistics.plainFrames++;
            if (window->windowItem()->position() != window->pos()) {
                statistics.plainOff++;
            }
            continue;
        }
        statistics.frames++;
        const Position &current = hold.history.last();
        const bool caughtUp = hold.shown.published == current.published;
        // While behind a window that has stopped, keep frames coming: nothing else asks for one, and
        // the frame woven for the last position would wait.
        const bool tick = !caughtUp && hold.history.size() == hold.lastHistorySize;
        hold.lastHistorySize = hold.history.size();
        if (caughtUp) {
            hold.progress = now;
            hold.progressFrames = 0;
        } else if (!hold.followingSince) {
            const QPoint behind = current.published - hold.shown.published;
            statistics.maxBehind = std::max({statistics.maxBehind, std::abs(behind.x()), std::abs(behind.y())});
            hold.progressFrames++;
            if (now - hold.progress > s_timeout && hold.progressFrames >= s_timeoutFrames) {
                // No frame woven for a newer position in time: never leave the window behind.
                hold.followingSince = now;
                hold.shown = current;
                statistics.timeouts++;
                show(window, hold);
                qCDebug(KWIN_DISPLAYXR, "move sync timeout t=%lld: no frame woven for %d,%d in 100 ms, following the window", microseconds(now),
                        current.published.x(), current.published.y());
            }
        }
        if (m_debug) {
            // What this frame painted against the position its buffer was woven for.
            const QPoint painted = paintedSurfacePosition(window, window->windowItem()->position());
            const QPoint shown = convertedPosition(window, hold.shown.published);
            const std::optional<QPoint> tag = tagOf(window);
            (painted == shown && !hold.followingSince ? statistics.paintedWoven : statistics.paintedStale)++;
            qCDebug(KWIN_DISPLAYXR, "move sync frame t=%lld tag=%d,%d shown=%d,%d cur=%d,%d painted=%d,%d woven=%d,%d", microseconds(now),
                    tag ? tag->x() : -1, tag ? tag->y() : -1, hold.shown.published.x(), hold.shown.published.y(),
                    current.published.x(), current.published.y(), painted.x(), painted.y(), shown.x(), shown.y());
        }
        if (tick) {
            window->windowItem()->scheduleFrame();
        }
        if (!hold.grabbing && (caughtUp || now > hold.settleUntil)) {
            release(window, caughtUp ? "settled" : "settle timeout");
        }
    }
}

void MoveSync::release(Window *window, const char *why)
{
    const auto it = m_holds.find(window);
    if (it == m_holds.end()) {
        return;
    }
    const Hold hold = *it;
    m_holds.erase(it);
    for (const QMetaObject::Connection &connection : hold.connections) {
        disconnect(connection);
    }
    if (WindowItem *item = window->windowItem()) {
        item->setPosition(window->pos());
    }
    const Statistics &s = hold.statistics;
    qCDebug(KWIN_DISPLAYXR, "move sync done t=%lld (%s): %u frames, %u advances on a woven frame, %u timeouts, %u back hops, %u untagged, max behind %d logical px, %u holds, %u tag-lost releases; painted where woven %u, stale %u; plain frames %u, off the window %u",
            microseconds(Clock::now()), why, s.frames, s.advanced, s.timeouts, s.backHops, s.untagged, s.maxBehind, s.holds, s.tagLost, s.paintedWoven, s.paintedStale, s.plainFrames, s.plainOff);
    if (hold.held) {
        Q_EMIT heldChanged();
    }
}

void MoveSync::show(Window *window, const Hold &hold)
{
    WindowItem *item = window->windowItem();
    if (!item) {
        return;
    }
    // Held, the main surface goes on the device pixel the runtime wove the shown frame for.
    const QPointF position = hold.held ? framePositionForSurface(window, hold.shown.published, hold.shown.frame) : window->pos();
    if (item->position() != position) {
        item->setPosition(position);
    }
}

std::optional<QPoint> MoveSync::tagOf(const Window *window) const
{
    const SurfaceInterface *surface = window->surface();
    if (!surface) {
        return std::nullopt;
    }
    for (const QList<SubSurfaceInterface *> &children : {surface->below(), surface->above()}) {
        for (const SubSurfaceInterface *child : children) {
            const SurfaceInterface *tag = child->surface();
            if (tag && tag->isMapped() && tag->size() == QSizeF(1, 1)) {
                const QPointF position = child->position();
                return QPoint(tagCoordinate(std::round(position.x())), tagCoordinate(std::round(position.y())));
            }
        }
    }
    return std::nullopt;
}

} // namespace KWin

#include "moc_movesync.cpp"

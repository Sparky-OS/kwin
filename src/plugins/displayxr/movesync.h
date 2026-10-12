/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QDBusServiceWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QSet>
#include <QSizeF>

#include <chrono>
#include <optional>

namespace KWin
{

class LogicalOutput;
class Window;

/**
 * Move-synchronised re-weave: while the user moves a window of a process that enabled it, KWin
 * moves the window as usual, so its geometry is published where it really is, but paints it only
 * where the frame on screen was woven for. The runtime tags each commit with a 1x1 synchronised
 * subsurface of the window's main surface, placed at (x mod 256, y mod 256) of the published
 * content position the frame was woven for. The tag is resolved against the positions the window
 * had in the last second, newest first, and the window item is held where KWin paints the main
 * surface on the device pixel the runtime converted that position to.
 *
 * The rules are the GNOME extension's (MoveSyncChoice): a window is held only while a tag is
 * mapped; after 100 ms and 6 frames behind with no newer woven frame it follows the window until a
 * frame woven for a position it had in the last 60 ms arrives; after the move it settles for up to
 * 300 ms.
 */
class MoveSync : public QObject
{
    Q_OBJECT

public:
    explicit MoveSync();
    ~MoveSync() override;

    void enable(const QString &service, pid_t pid);
    bool isEnabled(pid_t pid) const;

Q_SIGNALS:
    void clientsChanged();

private:
    using Clock = std::chrono::steady_clock;

    struct Position
    {
        QPoint published;
        QPointF frame;
        Clock::time_point time;
    };

    struct Statistics
    {
        uint frames = 0;
        uint advanced = 0;
        uint timeouts = 0;
        uint backHops = 0;
        uint untagged = 0;
        uint holds = 0;
        uint tagLost = 0;
        int maxBehind = 0;
        uint paintedWoven = 0;
        uint paintedStale = 0;
        uint plainFrames = 0;
        uint plainOff = 0;
    };

    struct Hold
    {
        bool held = false;
        bool grabbing = true;
        QList<Position> history;
        Position shown;
        Clock::time_point progress;
        uint progressFrames = 0;
        std::optional<Clock::time_point> followingSince;
        qsizetype lastHistorySize = 0;
        Clock::time_point settleUntil;
        QSizeF size;
        Statistics statistics;
        QList<QMetaObject::Connection> connections;
    };

    void track(Window *window);
    void begin(Window *window);
    void end(Window *window);
    void moved(Window *window);
    void committed(Window *window);
    void frame(const LogicalOutput *output);
    void release(Window *window, const char *why);
    void show(Window *window, const Hold &hold);
    std::optional<QPoint> tagOf(const Window *window) const;
    void dropClient(const QString &service);

    QHash<QString, QSet<pid_t>> m_clients;
    QDBusServiceWatcher m_watcher;
    QHash<Window *, Hold> m_holds;
    const bool m_debug;
};

} // namespace KWin

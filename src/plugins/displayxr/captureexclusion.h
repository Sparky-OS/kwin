/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include <QDBusContext>
#include <QDBusServiceWatcher>
#include <QHash>
#include <QObject>
#include <QSet>

namespace KWin
{

class Window;

/**
 * org.displayxr.CaptureExclusion1: keeps a process's windows out of screenshots and screen casts
 * while they stay on the screen (Window::setExcludeFromCapture()). A client may exclude only its
 * own process, and the exclusion ends with its bus connection.
 */
class CaptureExclusion : public QObject, protected QDBusContext
{
    Q_OBJECT

public:
    explicit CaptureExclusion();
    ~CaptureExclusion() override;

public Q_SLOTS:
    uint Exclude(uint pid);
    void Release(uint pid);
    QString GetState() const;

private:
    void apply();
    void dropClient(const QString &service);

    QHash<QString, QSet<pid_t>> m_clients;
    QHash<Window *, bool> m_previous;
    QDBusServiceWatcher m_watcher;
};

} // namespace KWin

/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "captureexclusion.h"
#include "captureexclusion1adaptor.h"
#include "window.h"
#include "workspace.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace KWin
{

CaptureExclusion::CaptureExclusion()
{
    m_watcher.setConnection(QDBusConnection::sessionBus());
    m_watcher.setWatchMode(QDBusServiceWatcher::WatchForUnregistration);
    connect(&m_watcher, &QDBusServiceWatcher::serviceUnregistered, this, &CaptureExclusion::dropClient);
    connect(workspace(), &Workspace::windowAdded, this, &CaptureExclusion::apply);
    connect(workspace(), &Workspace::windowRemoved, this, [this](Window *window) {
        m_previous.remove(window);
    });

    new CaptureExclusion1Adaptor(this);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/displayxr/CaptureExclusion"), this);
}

CaptureExclusion::~CaptureExclusion()
{
    // No window stays out of captures once the service is gone.
    m_clients.clear();
    apply();
}

uint CaptureExclusion::Exclude(uint pid)
{
    const QDBusReply<uint> caller = connection().interface()->servicePid(message().service());
    if (!caller.isValid()) {
        sendErrorReply(QDBusError::Failed, QStringLiteral("could not resolve the caller PID"));
        return 0;
    }
    if (pid != 0 && pid != caller.value()) {
        sendErrorReply(QDBusError::AccessDenied,
                       QStringLiteral("a client may only exclude its own windows (caller pid %1, asked %2)").arg(caller.value()).arg(pid));
        return 0;
    }
    const QString service = message().service();
    if (!m_clients.contains(service)) {
        m_watcher.addWatchedService(service);
    }
    m_clients[service].insert(caller.value());
    apply();

    const auto windows = workspace()->windows();
    return std::count_if(windows.cbegin(), windows.cend(), [&caller](const Window *window) {
        return window->pid() == pid_t(caller.value()) && window->excludeFromCapture();
    });
}

void CaptureExclusion::Release(uint pid)
{
    const QString service = message().service();
    const auto it = m_clients.find(service);
    if (it == m_clients.end()) {
        return;
    }
    if (pid == 0) {
        pid = connection().interface()->servicePid(service).value();
    }
    it->remove(pid);
    if (it->isEmpty()) {
        dropClient(service);
    } else {
        apply();
    }
}

QString CaptureExclusion::GetState() const
{
    QJsonArray clients;
    for (const auto &[service, pids] : m_clients.asKeyValueRange()) {
        QJsonArray pidList;
        for (const pid_t pid : pids) {
            pidList.append(pid);
        }
        clients.append(QJsonObject{{QStringLiteral("sender"), service}, {QStringLiteral("pids"), pidList}});
    }
    QJsonArray windows;
    for (const Window *window : m_previous.keys()) {
        windows.append(QJsonObject{{QStringLiteral("pid"), window->pid()}, {QStringLiteral("title"), window->caption()}});
    }
    const QJsonObject state{
        {QStringLiteral("version"), 1},
        {QStringLiteral("clients"), clients},
        {QStringLiteral("windows"), windows},
        // KWin's screenshot and screen cast paths leave excluded windows out without counting paints.
        {QStringLiteral("paints"), QJsonObject{{QStringLiteral("onscreen"), 0}, {QStringLiteral("skipped"), 0}}},
    };
    return QString::fromUtf8(QJsonDocument(state).toJson(QJsonDocument::Compact));
}

void CaptureExclusion::apply()
{
    if (!workspace()) {
        return;
    }
    QSet<pid_t> wanted;
    for (const QSet<pid_t> &pids : std::as_const(m_clients)) {
        wanted.unite(pids);
    }
    const auto windows = workspace()->windows();
    for (Window *window : windows) {
        const bool want = wanted.contains(window->pid());
        if (want && !m_previous.contains(window)) {
            m_previous.insert(window, window->excludeFromCapture());
            window->setExcludeFromCapture(true);
        } else if (!want && m_previous.contains(window)) {
            window->setExcludeFromCapture(m_previous.take(window));
        }
    }
}

void CaptureExclusion::dropClient(const QString &service)
{
    m_watcher.removeWatchedService(service);
    m_clients.remove(service);
    apply();
}

} // namespace KWin

#include "moc_captureexclusion.cpp"

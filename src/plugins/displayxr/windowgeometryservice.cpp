/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "windowgeometryservice.h"
#include "core/output.h"
#include "wayland_server.h"
#include "window.h"
#include "windowgeometry1adaptor.h"
#include "windowplacement.h"
#include "workspace.h"
#if KWIN_BUILD_X11
#include "x11window.h"
#endif

#include <QDBusConnection>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace KWin
{

static const QString s_serviceName = QStringLiteral("org.displayxr.WindowGeometry");

/**
 * The whole logical value that the runtime's conversion, round(value * scale), maps onto
 * @p devicePixel; the nearest one when only a fractional position reaches that pixel.
 */
static int logicalForDevicePixel(int devicePixel, qreal scale)
{
    const int nearest = std::round(devicePixel / scale);
    for (const int candidate : {nearest, nearest - 1, nearest + 1}) {
        if (std::round(candidate * scale) == devicePixel) {
            return candidate;
        }
    }
    return nearest;
}

static QJsonArray publishedRect(const QPoint &deviceOrigin, const QSizeF &logicalSize, const QPoint &outputOrigin, qreal scale)
{
    return QJsonArray{
        outputOrigin.x() + logicalForDevicePixel(deviceOrigin.x(), scale),
        outputOrigin.y() + logicalForDevicePixel(deviceOrigin.y(), scale),
        logicalForDevicePixel(std::round(logicalSize.width() * scale), scale),
        logicalForDevicePixel(std::round(logicalSize.height() * scale), scale),
    };
}

static QJsonObject windowEntry(Window *window)
{
    QJsonObject entry{
        {QStringLiteral("pid"), window->pid()},
        {QStringLiteral("app_id"), window->resourceClass()},
        {QStringLiteral("title"), window->caption()},
        {QStringLiteral("focus"), window->isActive()},
#if KWIN_BUILD_X11
        {QStringLiteral("xwayland"), qobject_cast<X11Window *>(window) != nullptr},
#else
        {QStringLiteral("xwayland"), false},
#endif
        {QStringLiteral("capture_excluded"), window->excludeFromCapture()},
        {QStringLiteral("lattice_drop"), false},
        {QStringLiteral("move_sync"), false},
        {QStringLiteral("moving"), window->isInteractiveMove() || window->isInteractiveResize()},
    };

    // KWin paints the window item at the frame's position and the main surface at its offset in
    // the frame, each snapped to the output's pixel grid (WindowItem, Item::mapToView()). The
    // rectangles are published so that the runtime's conversion lands on those device pixels.
    const LogicalOutput *output = window->output();
    const qreal scale = output ? output->scale() : 1.0;
    const QPoint outputOrigin = output ? output->geometry().topLeft() : QPoint();
    const RectF frame = window->frameGeometry();
    const RectF buffer = window->bufferGeometry();
    const QPoint frameDevice(std::round(frame.x() * scale) - std::round(outputOrigin.x() * scale),
                             std::round(frame.y() * scale) - std::round(outputOrigin.y() * scale));
    const QPoint bufferDevice = frameDevice + QPoint(std::round((buffer.x() - frame.x()) * scale), std::round((buffer.y() - frame.y()) * scale));
    entry[QStringLiteral("frame")] = publishedRect(frameDevice, frame.size(), outputOrigin, scale);
    entry[QStringLiteral("buffer")] = publishedRect(bufferDevice, buffer.size(), outputOrigin, scale);

    if (output) {
        const Rect geometry = output->geometry();
        entry[QStringLiteral("monitor")] = QJsonObject{
            {QStringLiteral("x"), geometry.x()},
            {QStringLiteral("y"), geometry.y()},
            {QStringLiteral("w"), geometry.width()},
            {QStringLiteral("h"), geometry.height()},
            {QStringLiteral("scale"), scale},
            // KWin's layout is logical everywhere, so the output's scale is the device factor.
            {QStringLiteral("device_scale"), scale},
        };
    } else {
        entry[QStringLiteral("monitor")] = QJsonValue::Null;
    }
    return entry;
}

WindowGeometryService::WindowGeometryService()
{
    new WindowGeometry1Adaptor(this);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/org/displayxr/WindowGeometry"), this);
    m_placement = std::make_unique<WindowPlacement>();

    const auto windows = workspace()->windows();
    for (Window *window : windows) {
        trackWindow(window);
    }
    connect(workspace(), &Workspace::windowAdded, this, [this](Window *window) {
        trackWindow(window);
        scheduleChanged();
    });
    connect(workspace(), &Workspace::windowRemoved, this, &WindowGeometryService::scheduleChanged);
    connect(workspace(), &Workspace::windowActivated, this, &WindowGeometryService::scheduleChanged);

    const auto outputs = workspace()->outputs();
    for (LogicalOutput *output : outputs) {
        trackOutput(output);
    }
    connect(workspace(), &Workspace::outputAdded, this, &WindowGeometryService::trackOutput);
    connect(workspace(), &Workspace::outputsChanged, this, &WindowGeometryService::scheduleChanged);

    // The lock screen hides every window, so the published list is empty while it is up.
    connect(waylandServer(), &WaylandServer::lockStateChanged, this, &WindowGeometryService::scheduleChanged);

    // Both objects are on the bus before the name, so a client that sees the name can call them.
    QDBusConnection::sessionBus().registerService(s_serviceName);
}

WindowGeometryService::~WindowGeometryService()
{
    QDBusConnection::sessionBus().unregisterService(s_serviceName);
}

void WindowGeometryService::trackWindow(Window *window)
{
    connect(window, &Window::frameGeometryChanged, this, &WindowGeometryService::scheduleChanged);
    connect(window, &Window::bufferGeometryChanged, this, &WindowGeometryService::scheduleChanged);
    connect(window, &Window::outputChanged, this, &WindowGeometryService::scheduleChanged);
    connect(window, &Window::captionChanged, this, &WindowGeometryService::scheduleChanged);
    connect(window, &Window::windowClassChanged, this, &WindowGeometryService::scheduleChanged);
    connect(window, &Window::excludeFromCaptureChanged, this, &WindowGeometryService::scheduleChanged);
    connect(window, &Window::interactiveMoveResizeStarted, this, &WindowGeometryService::scheduleChanged);
    connect(window, &Window::interactiveMoveResizeFinished, this, &WindowGeometryService::scheduleChanged);
    // A runtime weaves each frame for the position it read last, so a move step goes out at once.
    connect(window, &Window::interactiveMoveResizeStepped, this, &WindowGeometryService::emitChanged);
}

void WindowGeometryService::trackOutput(LogicalOutput *output)
{
    connect(output, &LogicalOutput::changed, this, &WindowGeometryService::scheduleChanged);
}

void WindowGeometryService::scheduleChanged()
{
    // One signal per event loop turn, unless a move step has published the change already.
    if (!m_changeScheduled) {
        m_changeScheduled = true;
        QTimer::singleShot(0, this, [this]() {
            if (m_changeScheduled) {
                emitChanged();
            }
        });
    }
}

void WindowGeometryService::emitChanged()
{
    m_changeScheduled = false;
    Q_EMIT WindowsChanged(GetWindows());
}

QString WindowGeometryService::GetWindows() const
{
    QJsonArray windows;
    if (!waylandServer()->isScreenLocked()) {
        const auto stackingOrder = workspace()->stackingOrder();
        for (Window *window : stackingOrder) {
            if (window->isClient() && !window->isDeleted() && window->isNormalWindow()) {
                windows.append(windowEntry(window));
            }
        }
    }
    const QJsonObject snapshot{
        {QStringLiteral("version"), 1},
        {QStringLiteral("layout_mode"), QStringLiteral("logical")},
        {QStringLiteral("windows"), windows},
    };
    return QString::fromUtf8(QJsonDocument(snapshot).toJson(QJsonDocument::Compact));
}

} // namespace KWin

#include "moc_windowgeometryservice.cpp"

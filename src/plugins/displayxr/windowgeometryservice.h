/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "plugin.h"

namespace KWin
{

class CaptureExclusion;
class LogicalOutput;
class Window;
class WindowPlacement;

/**
 * Serves DisplayXR's window-geometry service, org.displayxr.WindowGeometry, to spatial display
 * runtimes: they weave a lenticular or other phase-sensitive picture by where a window's pixels
 * land on the output. Rectangles are logical, as KWin's layout is; each window's output and its
 * fractional scale let the runtime convert them to device pixels.
 */
class WindowGeometryService : public Plugin
{
    Q_OBJECT

public:
    explicit WindowGeometryService();
    ~WindowGeometryService() override;

public Q_SLOTS:
    QString GetWindows() const;

Q_SIGNALS:
    void WindowsChanged(const QString &json);

private:
    void trackWindow(Window *window);
    void trackOutput(LogicalOutput *output);
    void scheduleChanged();
    void emitChanged();

    std::unique_ptr<WindowPlacement> m_placement;
    std::unique_ptr<CaptureExclusion> m_captureExclusion;
    bool m_changeScheduled = false;
};

} // namespace KWin

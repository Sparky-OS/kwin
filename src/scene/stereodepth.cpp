/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/stereodepth.h"

#include "core/output.h"
#include "options.h"
#include "window.h"
#include "workspace.h"

#include <algorithm>
#include <cmath>

namespace KWin
{

// the width of the view the limits are given for
static constexpr qreal s_referenceWidth = 1920;

enum class Role {
    Fixed, // at the screen's level whatever happens
    Desktop,
    Active,
    Popped,
    Sunk,
};

static const Window *mainWindow(const Window *window)
{
    const Window *main = window;
    for (int i = 0; i < 16; ++i) {
        const QList<Window *> mains = main->mainWindows();
        if (mains.isEmpty()) {
            break;
        }
        main = mains.first();
    }
    return main;
}

// what pops up over a window rather than being a window of its own
static bool isPopup(const Window *window)
{
    return window->isMenu() || window->isDropdownMenu() || window->isPopupMenu() || window->isComboBox()
        || window->isTooltip() || window->isNotification() || window->isCriticalNotification()
        || window->isOnScreenDisplay() || window->isAppletPopup() || window->isPopupWindow();
}

static Role roleOf(const Window *window)
{
    if (window->isDeleted() || window->isFullScreen() || window->isLockScreen() || window->isInternal()
        || window->isOutline() || window->isInputMethod() || window->isDNDIcon() || window->isPictureInPicture()) {
        return Role::Fixed;
    }
    if (window->isDesktop()) {
        return Role::Desktop;
    }
    if (window->isDock()) {
        return Role::Fixed;
    }
    const Window *main = mainWindow(window);
    if (main != window) {
        // what is transient for a window follows it when it is sunk, and pops over everything else
        return roleOf(main) == Role::Sunk ? Role::Sunk : Role::Popped;
    }
    if (isPopup(window)) {
        return Role::Popped;
    }
    const Window *active = workspace()->activeWindow();
    return active && mainWindow(active) == window ? Role::Active : Role::Sunk;
}

static bool isOnStage(const Window *window, const LogicalOutput *output)
{
    return window->output() == output && window->isShown() && window->isOnCurrentDesktop() && window->isOnCurrentActivity();
}

static bool touchesEdge(const Window *window)
{
    const RectF frame = window->frameGeometry();
    const RectF screen = window->output()->geometryF();
    return frame.x() <= screen.x() || frame.y() <= screen.y()
        || frame.x() + frame.width() >= screen.x() + screen.width()
        || frame.y() + frame.height() >= screen.y() + screen.height();
}

bool StereoDepth::isEnabled()
{
    return options->stereoSunkLimit() > 0 || options->stereoPoppedLimit() > 0;
}

qreal StereoDepth::level(const Window *window)
{
    const LogicalOutput *output = window->output();
    if (!output) {
        return 0;
    }
    const QList<Window *> &stack = workspace()->stackingOrder();
    switch (roleOf(window)) {
    case Role::Fixed:
    case Role::Active:
        return 0;
    case Role::Desktop:
        return -1;
    case Role::Popped: {
        if (touchesEdge(window)) {
            return 0;
        }
        // the popped limit is shared by what is popped over each other, the topmost at the limit
        int count = 0;
        int below = 0;
        bool found = false;
        for (const Window *other : stack) {
            if (isOnStage(other, output) && roleOf(other) == Role::Popped && !touchesEdge(other)) {
                count++;
                found |= other == window;
                below += !found;
            }
        }
        return found ? qreal(below + 1) / count : 1;
    }
    case Role::Sunk: {
        // the sunk range is shared by the windows beneath the active one, the lowest at the limit
        const Window *main = mainWindow(window);
        int count = 0;
        int above = 0;
        bool found = false;
        for (const Window *other : stack) {
            if (isOnStage(other, output) && other == mainWindow(other) && roleOf(other) == Role::Sunk) {
                count++;
                found |= other == main;
                above += found && other != main;
            }
        }
        return found ? -qreal(above + 1) / count : 0;
    }
    }
    return 0;
}

int StereoDepth::parallax(const Window *window, int viewWidth)
{
    const qreal level = StereoDepth::level(window);
    const qreal limit = level > 0 ? options->stereoPoppedLimit() : options->stereoSunkLimit();
    return std::lround(level * limit * viewWidth / s_referenceWidth);
}

int StereoDepth::pointerParallax(const QPointF &pos, int viewWidth)
{
    const Window *window = workspace()->moveResizeWindow();
    if (!window) {
        const QList<Window *> &stack = workspace()->stackingOrder();
        for (auto it = stack.crbegin(); it != stack.crend(); ++it) {
            const Window *candidate = *it;
            if (candidate->isShown() && candidate->isOnCurrentDesktop() && candidate->isOnCurrentActivity()
                && candidate->frameGeometry().contains(pos) && candidate->hitTest(pos)) {
                window = candidate;
                break;
            }
        }
    }
    return window ? parallax(window, viewWidth) : 0;
}

int StereoDepth::eyeShift(int parallax, StereoEye eye)
{
    switch (eye) {
    case StereoEye::Left:
        return parallax - parallax / 2;
    case StereoEye::Right:
        return -(parallax / 2);
    default:
        return 0;
    }
}

int StereoDepth::maxEyeShift(int viewWidth)
{
    const qreal limit = std::max(options->stereoSunkLimit(), options->stereoPoppedLimit());
    return (std::lround(limit * viewWidth / s_referenceWidth) + 1) / 2;
}

} // namespace KWin

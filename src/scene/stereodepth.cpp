/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "scene/stereodepth.h"

#include "core/output.h"
#include "effect/effecthandler.h"
#include "options.h"
#include "wayland/surface.h"
#include "window.h"
#include "workspace.h"

#include <QEasingCurve>

#include <algorithm>
#include <cmath>
#include <limits>

namespace KWin
{

// the width of the view the limits are given for
static constexpr qreal s_referenceWidth = 1920;

// how long a change of level lasts at the default animation speed: towards the viewer quickly, so
// that nothing is drawn behind what it is over for more than an instant, and away from it gently
static constexpr std::chrono::milliseconds s_riseDuration(80);
static constexpr std::chrono::milliseconds s_sinkDuration(150);

enum class Role {
    Fixed, // at the screen's level whatever happens
    Desktop,
    Active,
    Popped,
    Sunk,
    Above, // pinned above the others by the user or a rule: at the popped limit
    Below, // pinned below the others by the user or a rule: at the sunk limit
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

static Role roleOf(const Window *window);

static bool isOnStage(const Window *window, const LogicalOutput *output)
{
    return window->output() == output && window->isShown() && window->isOnCurrentDesktop() && window->isOnCurrentActivity();
}

// whether a window of the pile lies over @a window: the active window waits there until it is raised
static bool isCovered(const Window *window)
{
    const QList<Window *> &stack = workspace()->stackingOrder();
    const RectF frame = window->frameGeometry();
    for (qsizetype i = stack.indexOf(const_cast<Window *>(window)) + 1; i > 0 && i < stack.size(); ++i) {
        const Window *other = stack[i];
        if (other == mainWindow(other) && isOnStage(other, window->output()) && other->frameGeometry().intersects(frame)
            && roleOf(other) == Role::Sunk) {
            return true;
        }
    }
    return false;
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
        const Role mainRole = roleOf(main);
        return mainRole == Role::Sunk || mainRole == Role::Below ? mainRole : Role::Popped;
    }
    if (isPopup(window)) {
        return Role::Popped;
    }
    // a window pinned above or below stays at its limit whatever is active
    if (window->keepAbove() != window->keepBelow()) {
        return window->keepAbove() ? Role::Above : Role::Below;
    }
    const Window *active = workspace()->activeWindow();
    return active && mainWindow(active) == window && !isCovered(window) ? Role::Active : Role::Sunk;
}

// where the program declared a popup sits (kde_stereo_depth_v1.set_place), from -1 to 1
static std::optional<qreal> declaredPlace(const Window *window)
{
    const SurfaceInterface *surface = window->surface();
    return surface ? surface->stereoPlace() : std::nullopt;
}

static bool touchesEdge(const Window *window)
{
    const RectF frame = window->frameGeometry();
    const RectF screen = window->output()->geometryF();
    return frame.x() <= screen.x() || frame.y() <= screen.y()
        || frame.x() + frame.width() >= screen.x() + screen.width()
        || frame.y() + frame.height() >= screen.y() + screen.height();
}

bool StereoDepth::isDesktopChrome(const Window *window)
{
    return window->isDock() || isPopup(window);
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
    case Role::Above:
        return touchesEdge(window) ? 0 : 1;
    case Role::Below:
        return -1;
    case Role::Popped: {
        if (touchesEdge(window)) {
            return 0;
        }
        qreal level = 1;
        if (const auto place = declaredPlace(window)) {
            // where the program declared it, under the rules below
            level = std::clamp(*place, -1.0, 1.0);
        } else {
            // the popped limit is shared by what is popped over each other, the topmost at the
            // limit; a popup with a declared place takes no share
            int count = 0;
            int below = 0;
            bool found = false;
            for (const Window *other : stack) {
                if (isOnStage(other, output) && roleOf(other) == Role::Popped && !touchesEdge(other) && !declaredPlace(other)) {
                    count++;
                    found |= other == window;
                    below += !found;
                }
            }
            level = found ? qreal(below + 1) / count : 1;
        }
        // nor behind what its own window popped up before it, which it was opened from: a sub-menu
        // that ends up under its menu shares the menu's level
        const Window *main = mainWindow(window);
        const QList<Window *> &shown = workspace()->windows();
        for (qsizetype i = 0, end = shown.indexOf(const_cast<Window *>(window)); i < end; ++i) {
            const Window *other = shown[i];
            if (mainWindow(other) == main && isOnStage(other, output) && roleOf(other) == Role::Popped && !touchesEdge(other)) {
                level = std::max(level, StereoDepth::level(other));
            }
        }
        return level;
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

bool StereoDepth::isAwake(const Window *window)
{
    const LogicalOutput *output = window->output();
    if (!output || !output->hasStereoEyes() || !isOnStage(window, output)) {
        return false;
    }
    switch (roleOf(window)) {
    case Role::Active:
    case Role::Popped:
    case Role::Above:
    case Role::Desktop:
        return true;
    case Role::Fixed: {
        // a panel is never in the pile; a fullscreen window is awake while it is the active one
        const Window *active = workspace()->activeWindow();
        return window->isDock() || window->isLockScreen() || window->isInternal()
            || (window->isFullScreen() && active && mainWindow(active) == mainWindow(window));
    }
    case Role::Sunk:
    case Role::Below:
        return false;
    }
    return false;
}

Qt::Edges StereoDepth::outputEdges(const Window *window)
{
    const LogicalOutput *output = window->output();
    if (!output) {
        return Qt::Edges();
    }
    const RectF frame = window->frameGeometry();
    const RectF screen = output->geometryF();
    Qt::Edges edges;
    edges.setFlag(Qt::LeftEdge, frame.x() <= screen.x());
    edges.setFlag(Qt::TopEdge, frame.y() <= screen.y());
    edges.setFlag(Qt::RightEdge, frame.x() + frame.width() >= screen.x() + screen.width());
    edges.setFlag(Qt::BottomEdge, frame.y() + frame.height() >= screen.y() + screen.height());
    return edges;
}

int StereoDepth::parallax(const Window *window, int viewWidth)
{
    return parallax(levelDepth(window), viewWidth);
}

int StereoDepth::parallax(qreal depth, int viewWidth)
{
    return std::lround(depth * viewWidth / s_referenceWidth);
}

bool StereoDepth::effectsEnabled()
{
    return options->stereoEffectSunkLimit() > 0 || options->stereoEffectPoppedLimit() > 0;
}

qreal StereoDepth::effectPoppedLimit()
{
    return options->stereoEffectPoppedLimit();
}

qreal StereoDepth::effectSunkLimit()
{
    return options->stereoEffectSunkLimit();
}

qreal StereoDepth::levelDepth(const Window *window)
{
    const qreal level = StereoDepth::level(window);
    return level * (level > 0 ? options->stereoPoppedLimit() : options->stereoSunkLimit());
}

qreal StereoDepth::coveredDepth(const RectF &area, const Window *window)
{
    const LogicalOutput *output = window->output();
    const QList<Window *> &stack = workspace()->stackingOrder();
    qsizetype end = stack.indexOf(const_cast<Window *>(window));
    if (end < 0) {
        end = stack.size();
    }
    qreal depth = std::numeric_limits<qreal>::lowest();
    for (qsizetype i = 0; i < end; ++i) {
        const Window *other = stack[i];
        if (!isOnStage(other, output)) {
            continue;
        }
        if (other->isDesktop() || other->frameGeometry().intersects(area)) {
            depth = std::max(depth, levelDepth(other));
        }
    }
    return depth;
}

qreal StereoDepth::poppedLimit(int viewWidth)
{
    return options->stereoPoppedLimit() * viewWidth / s_referenceWidth;
}

qreal StereoDepth::sunkLimit(int viewWidth)
{
    return options->stereoSunkLimit() * viewWidth / s_referenceWidth;
}

int StereoDepth::pointerParallax(const QPointF &pos, int viewWidth)
{
    return parallax(pointerDepth(pos), viewWidth);
}

qreal StereoDepth::pointerDepth(const QPointF &pos)
{
    if (effects && effects->activeFullScreenEffect()) {
        return 0;
    }
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
    return window ? levelDepth(window) : 0;
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

void StereoDepth::Motion::setTarget(qreal depth, std::chrono::nanoseconds time)
{
    if (!m_started) {
        m_from = m_to = depth;
        m_started = true;
        return;
    }
    if (depth == m_to) {
        return;
    }
    m_from = this->depth(time);
    m_to = depth;
    m_start = time;
    m_duration = m_to > m_from ? riseDuration() : sinkDuration();
}

qreal StereoDepth::Motion::depth(std::chrono::nanoseconds time) const
{
    if (m_duration <= std::chrono::nanoseconds::zero() || time >= m_start + m_duration) {
        return m_to;
    }
    if (time <= m_start) {
        return m_from;
    }
    const qreal progress = qreal((time - m_start).count()) / m_duration.count();
    return m_from + (m_to - m_from) * QEasingCurve(QEasingCurve::OutCubic).valueForProgress(progress);
}

bool StereoDepth::Motion::isMoving(std::chrono::nanoseconds time) const
{
    return m_duration > std::chrono::nanoseconds::zero() && time < m_start + m_duration;
}

std::chrono::nanoseconds StereoDepth::Motion::riseDuration()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(s_riseDuration * options->animationTimeFactor());
}

std::chrono::nanoseconds StereoDepth::Motion::sinkDuration()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(s_sinkDuration * options->animationTimeFactor());
}

} // namespace KWin

/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "core/rect.h"
#include "effect/globals.h"

#include <QPointF>

#include <chrono>

#include <kwin_export.h>

namespace KWin
{

class Window;

/**
 * The depth levels of the desktop's own elements on a stereo output, and who sits at which one.
 *
 * A level runs from -1 (the sunk limit, behind the screen) through 0 (the screen) to +1 (the
 * popped limit, in front of it). Anything between is valid, nothing goes beyond. The active
 * window is at the screen's level and what pops up over it (menus, dialogs, tooltips) is popped,
 * the inactive windows share the sunk range evenly in their stacking order, the wallpaper is at
 * the sunk limit, panels are at the screen's level, and a popped thing that touches the edge of
 * its output stays at the screen's level. A window the user or a rule pinned above the others
 * (Keep Above) stays at the popped limit and one pinned below (Keep Below) at the sunk limit,
 * whatever is active. Fullscreen windows and windows that are not part of
 * the desktop's own elements stay at the screen's level.
 *
 * Nothing is behind what it covers: the active window comes to the screen's level once it is on
 * top of the windows it overlaps, so with a delayed raise it stays in their pile until it is
 * raised.
 */
class KWIN_EXPORT StereoDepth
{
public:
    /**
     * A depth that follows its level in a short eased motion. Towards the viewer it is quick, so
     * that nothing is drawn behind what it is over for more than an instant, and away from the
     * viewer it is gentle. Both ease out, follow the animation speed, and are instant when
     * animations are off. Depths are in pixels of a view 1920 wide, positive in front of the screen.
     */
    class KWIN_EXPORT Motion
    {
    public:
        /**
         * Moves towards @a depth from where the motion is at @a time. The first depth is taken at once.
         */
        void setTarget(qreal depth, std::chrono::nanoseconds time);
        qreal depth(std::chrono::nanoseconds time) const;
        bool isMoving(std::chrono::nanoseconds time) const;

        /**
         * How long a motion towards the viewer and away from it lasts at the animation speed.
         */
        static std::chrono::nanoseconds riseDuration();
        static std::chrono::nanoseconds sinkDuration();

    private:
        qreal m_from = 0;
        qreal m_to = 0;
        std::chrono::nanoseconds m_start = std::chrono::nanoseconds::zero();
        std::chrono::nanoseconds m_duration = std::chrono::nanoseconds::zero();
        bool m_started = false;
    };

    /**
     * Whether any level is switched on. With both limits at zero the desktop is drawn as before.
     */
    static bool isEnabled();

    static qreal level(const Window *window);

    /**
     * Whether effects may use depth: either of the effects' limits is above zero. Effects
     * describe their motion in depth with a z in pixels of a view 1920 wide, the unit of the
     * limits, and the renderer turns it into the parallax between the eyes (RenderViewport).
     */
    static bool effectsEnabled();

    /**
     * How far effects may reach in front of and behind the screen, in pixels of a view 1920 wide.
     * They are the user's own settings and are not bound by the desktop's limits above: an
     * effect's motion may be bolder than where the windows sit.
     */
    static qreal effectPoppedLimit();
    static qreal effectSunkLimit();

    /**
     * The depth of @a window's level in pixels of a view 1920 wide, positive in front of the screen.
     */
    static qreal levelDepth(const Window *window);

    /**
     * The depth of the nearest thing beneath @a window in the stack that lies under @a area (in
     * logical coordinates), in pixels of a view 1920 wide, or the lowest qreal when nothing does.
     * A piece of @a window that is drawn over @a area must not go behind it: nothing is behind
     * what it covers.
     */
    static qreal coveredDepth(const RectF &area, const Window *window);

    static qreal poppedLimit(int viewWidth);
    static qreal sunkLimit(int viewWidth);

    /**
     * The parallax between the eyes' copies of the window in pixels of a view that is
     * @a viewWidth pixels wide, positive when the window is nearer than the screen.
     */
    static int parallax(const Window *window, int viewWidth);

    /**
     * The parallax of @a depth, in pixels of a view 1920 wide, on a view @a viewWidth pixels wide.
     */
    static int parallax(qreal depth, int viewWidth);

    /**
     * The parallax of the pointer at @a pos in the 2D geometry: that of the window under its hot
     * spot, or of the window being moved or resized, and none over nothing.
     */
    static int pointerParallax(const QPointF &pos, int viewWidth);

    /**
     * The same as a depth, in pixels of a view 1920 wide.
     */
    static qreal pointerDepth(const QPointF &pos);

    /**
     * How far @a eye's copy moves for @a parallax, to the right for the left eye when the window
     * is nearer, so that the two copies end up @a parallax pixels apart.
     */
    static int eyeShift(int parallax, StereoEye eye);

    /**
     * The farthest any window's copy moves in one eye on a view that is @a viewWidth pixels wide.
     */
    static int maxEyeShift(int viewWidth);
};

} // namespace KWin

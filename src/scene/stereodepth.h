/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include "effect/globals.h"

#include <QPointF>

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
 * its output stays at the screen's level. Fullscreen windows and windows that are not part of
 * the desktop's own elements stay at the screen's level.
 */
class StereoDepth
{
public:
    /**
     * Whether any level is switched on. With both limits at zero the desktop is drawn as before.
     */
    static bool isEnabled();

    static qreal level(const Window *window);

    static qreal poppedLimit(int viewWidth);
    static qreal sunkLimit(int viewWidth);

    /**
     * The parallax between the eyes' copies of the window in pixels of a view that is
     * @a viewWidth pixels wide, positive when the window is nearer than the screen.
     */
    static int parallax(const Window *window, int viewWidth);

    /**
     * The parallax of the pointer at @a pos in the 2D geometry: that of the window under its hot
     * spot, or of the window being moved or resized, and none over nothing.
     */
    static int pointerParallax(const QPointF &pos, int viewWidth);

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

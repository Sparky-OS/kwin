/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include "core/rect.h"
#include "core/region.h"
#include "effect/globals.h"
#include "kwin_export.h"

#include <QList>
#include <QObject>
#include <QPointer>

#include <memory>
#include <optional>
#include <vector>

#include <xcb/xcb.h>

namespace KWin
{

class SurfaceInterface;
class X11Window;
class XwaylandSurfaceV1Interface;

/**
 * A child window of an X11 window that holds stereo content, such as the OpenGL area of a
 * program whose interface is drawn in 2D around it.
 */
struct KWIN_EXPORT X11StereoChild
{
    xcb_window_t window = XCB_WINDOW_NONE;
    // the windows between the child and the X11 window, whose moves move the child
    QList<xcb_window_t> ancestors;
    std::optional<quint64> surfaceSerial;
    QPointer<SurfaceInterface> surface;
    // the child's place in the X11 window
    RectF geometry;
    // Bounding shape in native X pixels, independent of the displayed place.
    RegionF shape;
    QSize nativeSize;
    // the child and the windows between it and the X11 window are mapped, so the child is
    // shown with the X11 window
    bool mapped = false;
    // the child's own _KDE_NET_WM_STEREO_CONTENT_CHILD_PLACE: the rectangle in the X11 window it
    // is shown at, however large its native raster is, and whether it is shown below the X11
    // window's own surface (so the ordinary window paints its controls above the child)
    std::optional<RectF> declaredPlace;
    bool belowParent = false;
    StereoContent content = StereoContentNone;
    // the child's own _KDE_NET_WM_STEREO_CONTENT_ONE_VIEW_SIZE: the size one view is shown at,
    // empty when the child declares none
    QSizeF oneViewSize;

    // The child's place in the X11 window as the scene shows it, in this order: its declared
    // place (_KDE_NET_WM_STEREO_CONTENT_CHILD_PLACE); else, when it holds stereo content and
    // declares the size of one view, its position at that size, however large its X11 window is
    // (as for an X11 window, the views are kept at the program's own raster); else its X11
    // window's rectangle.
    RectF place() const
    {
        if (declaredPlace) {
            return *declaredPlace;
        }
        if (content != StereoContentNone && !oneViewSize.isEmpty()) {
            return RectF(geometry.topLeft(), oneViewSize);
        }
        return geometry;
    }
};

/**
 * The stereo child windows an X11 window declares in _KDE_NET_WM_STEREO_CONTENT_CHILDREN.
 *
 * A declared child is redirected, so Xwayland gives it a surface of its own; the scene shows that
 * surface at the child's place, one view for each eye as the child's own
 * _KDE_NET_WM_STEREO_CONTENT declares, and the rest of the window the same in both eyes.
 */
class KWIN_EXPORT X11StereoChildren : public QObject
{
    Q_OBJECT

public:
    explicit X11StereoChildren(X11Window *window);
    ~X11StereoChildren() override;

    /**
     * Reads the X11 window's _KDE_NET_WM_STEREO_CONTENT_CHILDREN.
     */
    void update();
    /**
     * Gives the children back to the X server: they are no longer redirected.
     */
    void release();

    /**
     * Whether @p window is a declared child or one of the windows between a child and the
     * X11 window.
     */
    bool owns(xcb_window_t window) const;
    bool hasSurfaceSerial(quint64 serial) const;
    void associate(XwaylandSurfaceV1Interface *surface);
    /**
     * Handles an event of a declared child or of a window between a child and the X11 window.
     */
    void handleEvent(xcb_generic_event_t *event);

    const std::vector<std::unique_ptr<X11StereoChild>> &children() const;
    /**
     * Whether a child holds stereo content and is shown.
     */
    bool holdsStereoContent() const;

Q_SIGNALS:
    void changed();

private:
    std::unique_ptr<X11StereoChild> watch(xcb_window_t window);
    void unwatch(X11StereoChild *child);
    void updateGeometry(X11StereoChild *child);
    void updateContent(X11StereoChild *child);
    void updateShape(X11StereoChild *child);
    void setSurface(X11StereoChild *child, SurfaceInterface *surface);
    X11StereoChild *findChild(xcb_window_t window) const;

    X11Window *m_window;
    std::vector<std::unique_ptr<X11StereoChild>> m_children;
};

} // namespace KWin

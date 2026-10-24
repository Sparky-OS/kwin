/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "x11stereochildren.h"
#include "atoms.h"
#include "main.h"
#include "utils/c_ptr.h"
#include "utils/xcbutils.h"
#include "wayland/surface.h"
#include "wayland/xwaylandshell_v1.h"
#include "wayland_server.h"
#include "x11window.h"

#include <xcb/composite.h>

namespace KWin
{

X11StereoChildren::X11StereoChildren(X11Window *window)
    : m_window(window)
{
}

X11StereoChildren::~X11StereoChildren() = default;

const std::vector<std::unique_ptr<X11StereoChild>> &X11StereoChildren::children() const
{
    return m_children;
}

X11StereoChild *X11StereoChildren::findChild(xcb_window_t window) const
{
    for (const auto &child : m_children) {
        if (child->window == window) {
            return child.get();
        }
    }
    return nullptr;
}

bool X11StereoChildren::owns(xcb_window_t window) const
{
    for (const auto &child : m_children) {
        if (child->window == window || child->ancestors.contains(window)) {
            return true;
        }
    }
    return false;
}

bool X11StereoChildren::hasSurfaceSerial(quint64 serial) const
{
    for (const auto &child : m_children) {
        if (child->surfaceSerial == serial) {
            return true;
        }
    }
    return false;
}

bool X11StereoChildren::holdsStereoContent() const
{
    for (const auto &child : m_children) {
        if (child->content != StereoContentNone && child->mapped && child->surface) {
            return true;
        }
    }
    return false;
}

// _KDE_NET_WM_STEREO_CONTENT_CHILDREN: WINDOW/32 on the X11 window, the child windows that hold
// stereo content, each one declaring its content with its own _KDE_NET_WM_STEREO_CONTENT.
void X11StereoChildren::update()
{
    Xcb::Property property(false, m_window->window(), atoms->kde_net_wm_stereo_content_children, XCB_ATOM_WINDOW, 0, 1024);
    QList<xcb_window_t> declared;
    if (const auto windows = property.array<xcb_window_t>()) {
        for (const xcb_window_t window : *windows) {
            if (window != XCB_WINDOW_NONE && window != m_window->window() && !declared.contains(window)) {
                declared.append(window);
            }
        }
    }

    bool changed = false;
    for (auto it = m_children.begin(); it != m_children.end();) {
        if (!declared.contains((*it)->window)) {
            std::unique_ptr<X11StereoChild> child = std::move(*it);
            it = m_children.erase(it);
            unwatch(child.get());
            changed = true;
        } else {
            ++it;
        }
    }
    for (const xcb_window_t window : std::as_const(declared)) {
        if (findChild(window)) {
            continue;
        }
        if (auto child = watch(window)) {
            m_children.push_back(std::move(child));
            changed = true;
        }
    }
    if (changed) {
        Q_EMIT this->changed();
    }
}

std::unique_ptr<X11StereoChild> X11StereoChildren::watch(xcb_window_t window)
{
    auto child = std::make_unique<X11StereoChild>();
    child->window = window;

    // only a window inside the X11 window is a child of it
    xcb_window_t current = window;
    while (true) {
        Xcb::Tree tree(current);
        if (tree.isNull()) {
            return nullptr;
        }
        const xcb_window_t parent = tree.parent();
        if (parent == m_window->window()) {
            break;
        }
        if (parent == XCB_WINDOW_NONE || parent == tree->root) {
            return nullptr;
        }
        child->ancestors.append(parent);
        current = parent;
    }

    xcb_connection_t *connection = kwinApp()->x11Connection();
    const uint32_t childEvents = XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(connection, window, XCB_CW_EVENT_MASK, &childEvents);
    const uint32_t ancestorEvents = XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    for (const xcb_window_t ancestor : std::as_const(child->ancestors)) {
        xcb_change_window_attributes(connection, ancestor, XCB_CW_EVENT_MASK, &ancestorEvents);
    }
    // a redirected child gets a surface of its own from Xwayland, which announces it with
    // WL_SURFACE_SERIAL as it does for a top-level window
    xcb_composite_redirect_window(connection, window, XCB_COMPOSITE_REDIRECT_MANUAL);

    updateGeometry(child.get());
    updateContent(child.get());
    return child;
}

void X11StereoChildren::unwatch(X11StereoChild *child)
{
    xcb_connection_t *connection = kwinApp()->x11Connection();
    // the child may be gone already, with its X11 window destroyed: that is no error to report
    xcb_discard_reply(connection, xcb_composite_unredirect_window_checked(connection, child->window, XCB_COMPOSITE_REDIRECT_MANUAL).sequence);
    const uint32_t noEvents = XCB_EVENT_MASK_NO_EVENT;
    xcb_change_window_attributes(connection, child->window, XCB_CW_EVENT_MASK, &noEvents);
    for (const xcb_window_t ancestor : std::as_const(child->ancestors)) {
        if (!owns(ancestor)) {
            xcb_change_window_attributes(connection, ancestor, XCB_CW_EVENT_MASK, &noEvents);
        }
    }
    if (child->surface) {
        disconnect(child->surface, nullptr, this, nullptr);
    }
}

void X11StereoChildren::release()
{
    std::vector<std::unique_ptr<X11StereoChild>> children = std::move(m_children);
    m_children.clear();
    for (const auto &child : children) {
        unwatch(child.get());
    }
}

void X11StereoChildren::updateGeometry(X11StereoChild *child)
{
    xcb_connection_t *connection = kwinApp()->x11Connection();
    Xcb::WindowGeometry geometry(child->window);
    const xcb_translate_coordinates_cookie_t cookie = xcb_translate_coordinates(connection, child->window, m_window->window(), 0, 0);
    UniqueCPtr<xcb_translate_coordinates_reply_t> position(xcb_translate_coordinates_reply(connection, cookie, nullptr));
    if (geometry.isNull() || !position) {
        return;
    }
    child->geometry = Xcb::fromXNative(Rect(position->dst_x, position->dst_y, geometry->width, geometry->height));

    // mapped up to the X11 window: the X11 window's own state is the window's, and a child that
    // was mapped while the X11 window was not gets no MapNotify when the X11 window is mapped
    child->mapped = true;
    QList<xcb_window_t> windows = child->ancestors;
    windows.prepend(child->window);
    for (const xcb_window_t window : std::as_const(windows)) {
        Xcb::WindowAttributes attributes(window);
        if (attributes.isNull() || attributes->map_state == XCB_MAP_STATE_UNMAPPED) {
            child->mapped = false;
            break;
        }
    }
}

void X11StereoChildren::updateContent(X11StereoChild *child)
{
    // the child's own _KDE_NET_WM_STEREO_CONTENT, read as for an X11 window: absent, malformed
    // or out of range means none, and the child is shown the same in both eyes
    Xcb::Property property(false, child->window, atoms->kde_net_wm_stereo_content, XCB_ATOM_CARDINAL, 0, 1);
    const uint32_t value = property.value<uint32_t>().value_or(StereoContentNone);
    child->content = value == StereoContentSideBySideFull ? StereoContentSideBySideFull : StereoContentNone;
}

void X11StereoChildren::setSurface(X11StereoChild *child, SurfaceInterface *surface)
{
    if (child->surface == surface) {
        return;
    }
    if (child->surface) {
        disconnect(child->surface, nullptr, this, nullptr);
    }
    child->surface = surface;
    if (surface) {
        const xcb_window_t window = child->window;
        connect(surface, &SurfaceInterface::aboutToBeDestroyed, this, [this, window]() {
            if (X11StereoChild *child = findChild(window)) {
                child->surface = nullptr;
                Q_EMIT changed();
            }
        });
    }
    Q_EMIT changed();
}

void X11StereoChildren::associate(XwaylandSurfaceV1Interface *surface)
{
    for (const auto &child : m_children) {
        if (child->surfaceSerial && child->surfaceSerial == surface->serial()) {
            setSurface(child.get(), surface->surface());
            return;
        }
    }
}

void X11StereoChildren::handleEvent(xcb_generic_event_t *event)
{
    const uint8_t eventType = event->response_type & ~0x80;
    switch (eventType) {
    case XCB_CONFIGURE_NOTIFY:
    case XCB_MAP_NOTIFY:
    case XCB_UNMAP_NOTIFY: {
        // a child or a window between it and the X11 window moved, was resized, shown or hidden
        for (const auto &child : m_children) {
            updateGeometry(child.get());
        }
        Q_EMIT changed();
        break;
    }
    case XCB_REPARENT_NOTIFY: {
        // a child, or a window between it and the X11 window, moved to another parent: the
        // child is watched again, or dropped when it left the X11 window
        const auto *reparent = reinterpret_cast<xcb_reparent_notify_event_t *>(event);
        QList<xcb_window_t> moved;
        for (auto it = m_children.begin(); it != m_children.end();) {
            if ((*it)->window == reparent->window || (*it)->ancestors.contains(reparent->window)) {
                std::unique_ptr<X11StereoChild> child = std::move(*it);
                it = m_children.erase(it);
                unwatch(child.get());
                moved.append(child->window);
            } else {
                ++it;
            }
        }
        for (const xcb_window_t window : std::as_const(moved)) {
            if (auto watched = watch(window)) {
                m_children.push_back(std::move(watched));
            }
        }
        if (!moved.isEmpty()) {
            Q_EMIT changed();
        }
        break;
    }
    case XCB_DESTROY_NOTIFY: {
        const auto *destroy = reinterpret_cast<xcb_destroy_notify_event_t *>(event);
        if (std::erase_if(m_children, [destroy](const std::unique_ptr<X11StereoChild> &child) {
                return child->window == destroy->window;
            })) {
            Q_EMIT changed();
        }
        break;
    }
    case XCB_PROPERTY_NOTIFY: {
        const auto *property = reinterpret_cast<xcb_property_notify_event_t *>(event);
        if (property->atom == atoms->kde_net_wm_stereo_content) {
            if (X11StereoChild *child = findChild(property->window)) {
                updateContent(child);
                Q_EMIT changed();
            }
        }
        break;
    }
    case XCB_CLIENT_MESSAGE: {
        const auto *message = reinterpret_cast<xcb_client_message_event_t *>(event);
        if (message->type == atoms->wl_surface_serial) {
            if (X11StereoChild *child = findChild(message->window)) {
                child->surfaceSerial = (quint64(message->data.data32[1]) << 32) | message->data.data32[0];
                if (XwaylandSurfaceV1Interface *surface = waylandServer()->xwaylandShell()->findSurface(*child->surfaceSerial)) {
                    setSurface(child, surface->surface());
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

} // namespace KWin

#include "moc_x11stereochildren.cpp"

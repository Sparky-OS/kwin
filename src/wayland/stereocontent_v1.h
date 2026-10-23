/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#pragma once

#include "kwin_export.h"
#include "wayland/qwayland-server-kde-stereo-content-v1.h"

#include <QObject>
#include <QPointer>

#include <optional>

namespace KWin
{

class Display;
class SurfaceInterface;

class KWIN_EXPORT StereoContentManagerV1 : public QObject, private QtWaylandServer::kde_stereo_content_manager_v1
{
public:
    explicit StereoContentManagerV1(Display *display, QObject *parent);

private:
    void kde_stereo_content_manager_v1_destroy(Resource *resource) override;
    void kde_stereo_content_manager_v1_create(Resource *resource, uint32_t id, ::wl_resource *surface) override;
    void kde_stereo_content_manager_v1_get_depth(Resource *resource, uint32_t id, ::wl_resource *surface) override;
};

class StereoContentSurfaceV1 : private QtWaylandServer::kde_stereo_content_v1
{
public:
    explicit StereoContentSurfaceV1(wl_client *client, uint32_t id, uint32_t version, SurfaceInterface *surface);
    ~StereoContentSurfaceV1() override;

private:
    void kde_stereo_content_v1_destroy_resource(Resource *resource) override;
    void kde_stereo_content_v1_destroy(Resource *resource) override;
    void kde_stereo_content_v1_set_content(Resource *resource, uint32_t content) override;

    const QPointer<SurfaceInterface> m_surface;
};

/**
 * One kde_stereo_depth_v1 object: follows a surface's desktop depth, and may mark the surface's pair
 * as the desktop's own depth or declare where a popup surface sits. A surface may have many.
 */
class StereoDepthSurfaceV1 : private QtWaylandServer::kde_stereo_depth_v1
{
public:
    explicit StereoDepthSurfaceV1(wl_client *client, uint32_t id, uint32_t version, SurfaceInterface *surface);
    ~StereoDepthSurfaceV1() override;

    bool desktopDepth() const;
    std::optional<qreal> place() const;
    // orders the places set by the objects of one surface: the latest wins
    quint64 placeSerial() const;

    // sends the state unless it is the one this object last received
    void sendState(bool awake, uint32_t outputEdges);

private:
    void kde_stereo_depth_v1_destroy_resource(Resource *resource) override;
    void kde_stereo_depth_v1_destroy(Resource *resource) override;
    void kde_stereo_depth_v1_set_desktop_depth(Resource *resource, uint32_t desktopDepth) override;
    void kde_stereo_depth_v1_set_place(Resource *resource, wl_fixed_t depth) override;
    void kde_stereo_depth_v1_unset_place(Resource *resource) override;

    const QPointer<SurfaceInterface> m_surface;
    bool m_desktopDepth = false;
    std::optional<qreal> m_place;
    quint64 m_placeSerial = 0;
    std::optional<std::pair<bool, uint32_t>> m_sent;
};

}

/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#pragma once

#include "kwin_export.h"
#include "wayland/qwayland-server-kde-stereo-content-v1.h"

#include <QObject>
#include <QPointer>

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
    void kde_stereo_content_v1_set_content_class(Resource *resource, uint32_t contentClass, uint32_t subclass) override;

    const QPointer<SurfaceInterface> m_surface;
};

}

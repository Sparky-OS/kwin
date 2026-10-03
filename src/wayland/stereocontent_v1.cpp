/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "stereocontent_v1.h"

#include "display.h"
#include "effect/globals.h"
#include "surface.h"
#include "surface_p.h"

namespace KWin
{

static constexpr uint32_t s_version = 1;

static constexpr StereoContent toStereoContent(uint32_t content)
{
    return content <= StereoContentTopAndBottomFullRightFirst ? StereoContent(content) : StereoContentNone;
}

StereoContentManagerV1::StereoContentManagerV1(Display *display, QObject *parent)
    : QObject(parent)
    , QtWaylandServer::kde_stereo_content_manager_v1(*display, s_version)
{
}

void StereoContentManagerV1::kde_stereo_content_manager_v1_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

void StereoContentManagerV1::kde_stereo_content_manager_v1_create(Resource *resource, uint32_t id, ::wl_resource *surface)
{
    SurfaceInterface *surf = SurfaceInterface::get(surface);
    SurfaceInterfacePrivate *priv = SurfaceInterfacePrivate::get(surf);
    if (priv->stereoContent) {
        wl_resource_post_error(resource->handle, error_already_declared, "wl_surface already has a stereo content declaration");
        return;
    }
    priv->stereoContent = new StereoContentSurfaceV1(resource->client(), id, resource->version(), surf);
}

StereoContentSurfaceV1::StereoContentSurfaceV1(wl_client *client, uint32_t id, uint32_t version, SurfaceInterface *surface)
    : QtWaylandServer::kde_stereo_content_v1(client, id, version)
    , m_surface(surface)
{
}

StereoContentSurfaceV1::~StereoContentSurfaceV1()
{
    if (m_surface) {
        const auto priv = SurfaceInterfacePrivate::get(m_surface);
        priv->stereoContent = nullptr;
        priv->pending->stereoContent = StereoContentNone;
        priv->pending->stereoContentClass = 0;
        priv->pending->stereoContentSubclass = 0;
        priv->pending->committed |= SurfaceState::Field::StereoContent;
    }
}

void StereoContentSurfaceV1::kde_stereo_content_v1_destroy_resource(Resource *resource)
{
    delete this;
}

void StereoContentSurfaceV1::kde_stereo_content_v1_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

void StereoContentSurfaceV1::kde_stereo_content_v1_set_content(Resource *resource, uint32_t content)
{
    if (!m_surface) {
        wl_resource_post_error(resource->handle, error_no_surface, "wl_surface was destroyed before a set_content request");
        return;
    }
    const auto priv = SurfaceInterfacePrivate::get(m_surface);
    priv->pending->stereoContent = toStereoContent(content);
    priv->pending->committed |= SurfaceState::Field::StereoContent;
}

void StereoContentSurfaceV1::kde_stereo_content_v1_set_content_class(Resource *resource, uint32_t contentClass, uint32_t subclass)
{
    if (!m_surface) {
        wl_resource_post_error(resource->handle, error_no_surface, "wl_surface was destroyed before a set_content_class request");
        return;
    }
    if (contentClass > std::numeric_limits<uint8_t>::max() || subclass > std::numeric_limits<uint8_t>::max()) {
        wl_resource_post_error(resource->handle, error_invalid_class, "class and sub-class are 8-bit values");
        return;
    }
    const auto priv = SurfaceInterfacePrivate::get(m_surface);
    priv->pending->stereoContentClass = contentClass;
    priv->pending->stereoContentSubclass = subclass;
    priv->pending->committed |= SurfaceState::Field::StereoContent;
}

}

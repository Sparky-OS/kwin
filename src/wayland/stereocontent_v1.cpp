/*
    SPDX-FileCopyrightText: 2026 Daniel Campos Ramos <Capitain_Jack@yahoo.com>

    SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/
#include "stereocontent_v1.h"

#include "display.h"
#include "effect/globals.h"
#include "surface.h"
#include "surface_p.h"

#include <algorithm>

namespace KWin
{

static constexpr uint32_t s_version = 3;

static constexpr StereoContent toStereoContent(uint32_t content)
{
    return content == StereoContentSideBySideFull ? StereoContentSideBySideFull : StereoContentNone;
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

void StereoContentManagerV1::kde_stereo_content_manager_v1_get_depth(Resource *resource, uint32_t id, ::wl_resource *surface)
{
    SurfaceInterface *surf = SurfaceInterface::get(surface);
    auto depth = new StereoDepthSurfaceV1(resource->client(), id, resource->version(), surf);
    SurfaceInterfacePrivate::get(surf)->stereoDepths.append(depth);
    Q_EMIT surf->stereoDepthFollowed();
    // a surface whose state is known tells a new follower at once
    if (const auto state = SurfaceInterfacePrivate::get(surf)->stereoDepthState) {
        depth->sendState(state->first, state->second);
    }
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
        priv->pending->stereoContentOneEyeWide = false;
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
    priv->pending->stereoContentOneEyeWide = resource->version() >= 2;
    priv->pending->committed |= SurfaceState::Field::StereoContent;
}

// orders the places set by several objects of one surface
static quint64 s_placeSerial = 0;

StereoDepthSurfaceV1::StereoDepthSurfaceV1(wl_client *client, uint32_t id, uint32_t version, SurfaceInterface *surface)
    : QtWaylandServer::kde_stereo_depth_v1(client, id, version)
    , m_surface(surface)
{
}

StereoDepthSurfaceV1::~StereoDepthSurfaceV1()
{
    if (m_surface) {
        const auto priv = SurfaceInterfacePrivate::get(m_surface);
        priv->stereoDepths.removeOne(this);
        // what this object marked or declared is withdrawn on the next commit
        if (m_desktopDepth || m_place) {
            m_desktopDepth = false;
            m_place.reset();
            priv->updatePendingStereoDepth();
        }
    }
}

bool StereoDepthSurfaceV1::desktopDepth() const
{
    return m_desktopDepth;
}

std::optional<qreal> StereoDepthSurfaceV1::place() const
{
    return m_place;
}

quint64 StereoDepthSurfaceV1::placeSerial() const
{
    return m_placeSerial;
}

void StereoDepthSurfaceV1::sendState(bool awake, uint32_t outputEdges)
{
    const auto state = std::make_pair(awake, outputEdges);
    if (!m_surface || m_sent == state) {
        return;
    }
    m_sent = state;
    send_depth_state(awake ? 1 : 0, outputEdges);
}

void StereoDepthSurfaceV1::kde_stereo_depth_v1_destroy_resource(Resource *resource)
{
    delete this;
}

void StereoDepthSurfaceV1::kde_stereo_depth_v1_destroy(Resource *resource)
{
    wl_resource_destroy(resource->handle);
}

void StereoDepthSurfaceV1::kde_stereo_depth_v1_set_desktop_depth(Resource *resource, uint32_t desktopDepth)
{
    // inert once the surface is gone
    if (!m_surface) {
        return;
    }
    m_desktopDepth = desktopDepth != 0;
    SurfaceInterfacePrivate::get(m_surface)->updatePendingStereoDepth();
}

void StereoDepthSurfaceV1::kde_stereo_depth_v1_set_place(Resource *resource, wl_fixed_t depth)
{
    if (!m_surface) {
        return;
    }
    m_place = std::clamp(wl_fixed_to_double(depth), -1.0, 1.0);
    m_placeSerial = ++s_placeSerial;
    SurfaceInterfacePrivate::get(m_surface)->updatePendingStereoDepth();
}

void StereoDepthSurfaceV1::kde_stereo_depth_v1_unset_place(Resource *resource)
{
    if (!m_surface) {
        return;
    }
    m_place.reset();
    SurfaceInterfacePrivate::get(m_surface)->updatePendingStereoDepth();
}

}

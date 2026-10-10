// SPDX-License-Identifier: GPL-2.0-or-later
#include "wayland-xdg-shell-client-protocol.h"
#include "wayland-viewporter-client-protocol.h"
#include "wayland-kde-stereo-content-v1-client-protocol.h"

#include <QCoreApplication>
#include <QImage>
#include <QTemporaryFile>
#include <wayland-client.h>
#include <sys/mman.h>
#include <cstring>

struct Client
{
    wl_compositor *compositor = nullptr;
    wl_shm *shm = nullptr;
    xdg_wm_base *shell = nullptr;
    wp_viewporter *viewporter = nullptr;
    kde_stereo_content_manager_v1 *stereo = nullptr;
    wl_surface *surface = nullptr;
    wl_buffer *buffer = nullptr;
    bool closed = false;
};

static void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    auto client = static_cast<Client *>(data);
    if (!std::strcmp(interface, wl_compositor_interface.name)) {
        client->compositor = static_cast<wl_compositor *>(wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (!std::strcmp(interface, wl_shm_interface.name)) {
        client->shm = static_cast<wl_shm *>(wl_registry_bind(registry, name, &wl_shm_interface, 1));
    } else if (!std::strcmp(interface, xdg_wm_base_interface.name)) {
        client->shell = static_cast<xdg_wm_base *>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
    } else if (!std::strcmp(interface, wp_viewporter_interface.name)) {
        client->viewporter = static_cast<wp_viewporter *>(wl_registry_bind(registry, name, &wp_viewporter_interface, 1));
    } else if (!std::strcmp(interface, kde_stereo_content_manager_v1_interface.name)) {
        client->stereo = static_cast<kde_stereo_content_manager_v1 *>(wl_registry_bind(registry, name, &kde_stereo_content_manager_v1_interface, 1));
    }
}

static void removed(void *, wl_registry *, uint32_t) {}
static void ping(void *, xdg_wm_base *shell, uint32_t serial) { xdg_wm_base_pong(shell, serial); }
static void configure(void *, xdg_toplevel *, int32_t, int32_t, wl_array *) {}
static void close(void *data, xdg_toplevel *) { static_cast<Client *>(data)->closed = true; }

static void surfaceConfigure(void *data, xdg_surface *surface, uint32_t serial)
{
    auto client = static_cast<Client *>(data);
    xdg_surface_ack_configure(surface, serial);
    wl_surface_attach(client->surface, client->buffer, 0, 0);
    wl_surface_damage_buffer(client->surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(client->surface);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 5) {
        return 2;
    }
    const QImage image = QImage(QString::fromLocal8Bit(argv[1])).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const QSize viewportSize(QString::fromLocal8Bit(argv[2]).toInt(), QString::fromLocal8Bit(argv[3]).toInt());
    const bool stereo = QString::fromLocal8Bit(argv[4]) == QLatin1String("stereo");
    if (image.isNull() || viewportSize.isEmpty()) {
        return 3;
    }
    wl_display *display = wl_display_connect(nullptr);
    if (!display) {
        return 4;
    }
    Client client;
    wl_registry *registry = wl_display_get_registry(display);
    const wl_registry_listener registryListener{global, removed};
    wl_registry_add_listener(registry, &registryListener, &client);
    if (wl_display_roundtrip(display) < 0 || !client.compositor || !client.shm || !client.shell || !client.viewporter || (stereo && !client.stereo)) {
        return 5;
    }
    QTemporaryFile storage;
    if (!storage.open() || !storage.resize(image.sizeInBytes())) {
        return 6;
    }
    void *pixels = mmap(nullptr, image.sizeInBytes(), PROT_READ | PROT_WRITE, MAP_SHARED, storage.handle(), 0);
    if (pixels == MAP_FAILED) {
        return 7;
    }
    std::memcpy(pixels, image.constBits(), image.sizeInBytes());
    auto pool = wl_shm_create_pool(client.shm, storage.handle(), image.sizeInBytes());
    client.buffer = wl_shm_pool_create_buffer(pool, 0, image.width(), image.height(), image.bytesPerLine(), WL_SHM_FORMAT_ARGB8888);
    client.surface = wl_compositor_create_surface(client.compositor);
    auto viewport = wp_viewporter_get_viewport(client.viewporter, client.surface);
    wp_viewport_set_destination(viewport, viewportSize.width(), viewportSize.height());
    if (stereo) {
        auto declaration = kde_stereo_content_manager_v1_create(client.stereo, client.surface);
        kde_stereo_content_v1_set_content(declaration, 3);
    }
    const xdg_wm_base_listener shellListener{ping};
    xdg_wm_base_add_listener(client.shell, &shellListener, &client);
    auto surface = xdg_wm_base_get_xdg_surface(client.shell, client.surface);
    const xdg_surface_listener surfaceListener{surfaceConfigure};
    xdg_surface_add_listener(surface, &surfaceListener, &client);
    auto toplevel = xdg_surface_get_toplevel(surface);
    const xdg_toplevel_listener toplevelListener{configure, close};
    xdg_toplevel_add_listener(toplevel, &toplevelListener, &client);
    xdg_toplevel_set_app_id(toplevel, "org.kde.kwin.capturefixture");
    if (QString::fromLocal8Bit(argv[4]) == QLatin1String("fullscreen")) {
        xdg_toplevel_set_fullscreen(toplevel, nullptr);
    }
    wl_surface_commit(client.surface);
    while (!client.closed && wl_display_dispatch(display) >= 0) {
    }
    wl_display_disconnect(display);
    munmap(pixels, image.sizeInBytes());
    return 0;
}

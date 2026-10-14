// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Fullscreen, declared full-SBS fixture. Adapted from Sparky Stereo OS's
 * vkms-stereo-bench fullscreen-2d client. Each eye encodes its identity and
 * pixel coordinates. The independent Python reference defines the same input.
 */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "stereo-client.h"
#include "viewporter-client.h"

struct state {
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct xdg_wm_base *wm_base;
	struct kde_stereo_content_manager_v1 *stereo;
	struct wp_viewporter *viewporter;
	int width, height;
	bool configured;
};

static void wm_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
	xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial)
{
	struct state *state = data;

	xdg_surface_ack_configure(surface, serial);
	state->configured = true;
}

static const struct xdg_surface_listener surface_listener = { .configure = surface_configure };

static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t w, int32_t h,
			       struct wl_array *states)
{
	struct state *state = data;

	if (w > 0 && h > 0) {
		state->width = w;
		state->height = h;
	}
}

static void toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = toplevel_configure,
	.close = toplevel_close,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
			    const char *interface, uint32_t version)
{
	struct state *state = data;

	if (!strcmp(interface, wl_compositor_interface.name))
		state->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
	else if (!strcmp(interface, wl_shm_interface.name))
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	else if (!strcmp(interface, xdg_wm_base_interface.name))
		state->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
	else if (!strcmp(interface, kde_stereo_content_manager_v1_interface.name))
		state->stereo = wl_registry_bind(registry, name, &kde_stereo_content_manager_v1_interface, 1);
	else if (!strcmp(interface, wp_viewporter_interface.name))
		state->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *registry, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
	bool *done = data;
	*done = true;
	wl_callback_destroy(callback);
}

static const struct wl_callback_listener frame_listener = { .done = frame_done };

int main(void)
{
	struct state state = { .width = 640, .height = 480 };
	struct wl_display *display = wl_display_connect(NULL);
	const char *hold = getenv("HOLD_SECONDS");
	struct wl_shm_pool *pool;
	struct wl_buffer *buffer;
	uint32_t *pixels;
	int fd, x, y, stride, size;
	time_t end;

	if (!display)
		return 1;
	wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, &state);
	wl_display_roundtrip(display);
	if (!state.compositor || !state.shm || !state.wm_base || !state.stereo || !state.viewporter)
		return 1;
	xdg_wm_base_add_listener(state.wm_base, &wm_listener, &state);

	struct wl_surface *surface = wl_compositor_create_surface(state.compositor);
	struct kde_stereo_content_v1 *declaration = kde_stereo_content_manager_v1_create(state.stereo, surface);
	kde_stereo_content_v1_set_content(declaration, KDE_STEREO_CONTENT_V1_CONTENT_SIDE_BY_SIDE_FULL);
	struct xdg_surface *xdg_surface = xdg_wm_base_get_xdg_surface(state.wm_base, surface);
	struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg_surface);

	xdg_surface_add_listener(xdg_surface, &surface_listener, &state);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, &state);
	xdg_toplevel_set_title(toplevel, "wiz3D audit LEFT red RIGHT blue");
	xdg_toplevel_set_fullscreen(toplevel, NULL);
	wl_surface_commit(surface);
	while (!state.configured)
		if (wl_display_dispatch(display) < 0)
			return 1;

	int eye_width = state.width;
	struct wp_viewport *viewport = wp_viewporter_get_viewport(state.viewporter, surface);
	wp_viewport_set_destination(viewport, state.width, state.height);
	xdg_surface_set_window_geometry(xdg_surface, 0, 0, state.width, state.height);
	state.width *= 2;
	stride = state.width * 4;
	size = stride * state.height;
	fd = memfd_create("gradient", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size))
		return 1;
	pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pixels == MAP_FAILED)
		return 1;
	for (y = 0; y < state.height; y++) {
		for (x = 0; x < state.width; x++) {
			int u = x % eye_width;
			bool right = x >= eye_width;
			if (getenv("SWAP_EYES")) right = !right;
			int r = right ? 16 + u % 64 : 128 + u % 64;
			int g = 32 + (y % 32) * 4;
			int b = right ? 128 + (u + y) % 64 : 16 + (u + y) % 64;
			if (getenv("PALETTE_FIXTURE")) {
				r = (right ? 16 : 160) + 16 * (u % 2);
				g = (right ? 224 : 32) + 16 * (y % 2);
				b = (right ? 128 : 16) + 16 * ((u + y) % 2);
			}
			pixels[y * state.width + x] = 0xff000000 | r << 16 | g << 8 | b;
		}
	}
	pool = wl_shm_create_pool(state.shm, fd, size);
	buffer = wl_shm_pool_create_buffer(pool, 0, state.width, state.height, stride,
					   WL_SHM_FORMAT_XRGB8888);
	wl_surface_attach(surface, buffer, 0, 0);
	wl_surface_damage(surface, 0, 0, state.width, state.height);
	for (int frame = 0; frame < 3; frame++) {
		bool done = false;
		struct wl_callback *callback = wl_surface_frame(surface);
		wl_callback_add_listener(callback, &frame_listener, &done);
		wl_surface_damage(surface, 0, 0, eye_width, state.height);
		wl_surface_commit(surface);
		while (!done)
			if (wl_display_dispatch(display) < 0)
				return 1;
	}
	printf("fullscreen %dx%d frames=3\n", eye_width * 2, state.height);
	fflush(stdout);

	end = time(NULL) + (hold ? atoi(hold) : 60);
	while (time(NULL) < end) {
		wl_display_dispatch_pending(display);
		wl_display_flush(display);
		usleep(100000);
	}
	return 0;
}

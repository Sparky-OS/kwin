// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * drm-flipwatch: once per vblank of the CRTC that drives CONNECTOR, prints the vblank sequence
 * and the primary plane's SRC_X and SRC_W (in pixels), read half a refresh after the vblank.
 * A frame-sequential output scans one eye per refresh out of a frame twice as wide as the mode:
 * SRC_X alternates between 0 (left half) and the mode's width (right half).
 *
 *   drm-flipwatch CARD CONNECTOR COUNT [INTERVAL_US]
 *
 * With INTERVAL_US it reads SRC_X COUNT times that many microseconds apart instead, without
 * waiting for vblanks, so that a plane that changes less often than once per refresh is seen too.
 *
 * Not the DRM master: needs no authentication for these requests (root in the test guest).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm.h>
#include <drm_mode.h>

static int fd;

static int xioctl(unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r ? -errno : 0;
}

/* The CRTC driving the connector called NAME ("HDMI-A-1"), 0 if none. */
static uint32_t crtc_of(const uint32_t *connectors, unsigned int count, const char *name)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		struct drm_mode_get_connector c = { .connector_id = connectors[i] };
		struct drm_mode_get_encoder e = { 0 };
		char buf[32];

		if (xioctl(DRM_IOCTL_MODE_GETCONNECTOR, &c) || c.connector_type != DRM_MODE_CONNECTOR_HDMIA)
			continue;
		snprintf(buf, sizeof(buf), "HDMI-A-%u", c.connector_type_id);
		if (strcmp(buf, name))
			continue;
		e.encoder_id = c.encoder_id;
		if (!c.encoder_id || xioctl(DRM_IOCTL_MODE_GETENCODER, &e))
			return 0;
		return e.crtc_id;
	}
	return 0;
}

/* The value of the object's property called NAME, -1 if it has none. */
static int64_t property(uint32_t object, uint32_t type, const char *name)
{
	struct drm_mode_obj_get_properties props = { .obj_id = object, .obj_type = type };
	uint32_t ids[64];
	uint64_t values[64];
	unsigned int i;

	if (xioctl(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) || props.count_props > 64)
		return -1;
	props.props_ptr = (uintptr_t)ids;
	props.prop_values_ptr = (uintptr_t)values;
	if (xioctl(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props))
		return -1;
	for (i = 0; i < props.count_props; i++) {
		struct drm_mode_get_property p = { .prop_id = ids[i] };

		if (!xioctl(DRM_IOCTL_MODE_GETPROPERTY, &p) && !strcmp(p.name, name))
			return values[i];
	}
	return -1;
}

int main(int argc, char **argv)
{
	struct drm_mode_card_res res = { 0 };
	struct drm_mode_get_plane_res pres = { 0 };
	struct drm_mode_crtc crtc = { 0 };
	uint32_t crtcs[8], connectors[8], planes[32], plane = 0;
	unsigned int i, pipe = 0, count;
	long period_us;

	if (argc != 4 && argc != 5) {
		fprintf(stderr, "usage: %s CARD CONNECTOR COUNT [INTERVAL_US]\n", argv[0]);
		return 2;
	}
	count = atoi(argv[3]);
	fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	xioctl(DRM_IOCTL_SET_CLIENT_CAP, &(struct drm_set_client_cap){ DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 });
	xioctl(DRM_IOCTL_SET_CLIENT_CAP, &(struct drm_set_client_cap){ DRM_CLIENT_CAP_ATOMIC, 1 });
	if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res) || res.count_crtcs > 8 || res.count_connectors > 8)
		return 1;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.connector_id_ptr = (uintptr_t)connectors;
	res.count_encoders = res.count_fbs = 0;
	if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res))
		return 1;
	crtc.crtc_id = crtc_of(connectors, res.count_connectors, argv[2]);
	if (!crtc.crtc_id || xioctl(DRM_IOCTL_MODE_GETCRTC, &crtc) || !crtc.mode_valid) {
		fprintf(stderr, "%s: no active CRTC\n", argv[2]);
		return 1;
	}
	for (i = 0; i < res.count_crtcs; i++)
		if (crtcs[i] == crtc.crtc_id)
			pipe = i;
	if (xioctl(DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) || pres.count_planes > 32)
		return 1;
	pres.plane_id_ptr = (uintptr_t)planes;
	if (xioctl(DRM_IOCTL_MODE_GETPLANERESOURCES, &pres))
		return 1;
	for (i = 0; i < pres.count_planes && !plane; i++) {
		struct drm_mode_get_plane p = { .plane_id = planes[i] };

		if (!xioctl(DRM_IOCTL_MODE_GETPLANE, &p) && p.crtc_id == crtc.crtc_id
		    && property(planes[i], DRM_MODE_OBJECT_PLANE, "type") == 1 /* Primary */)
			plane = planes[i];
	}
	if (!plane) {
		fprintf(stderr, "no primary plane on CRTC %u\n", crtc.crtc_id);
		return 1;
	}
	period_us = 1000000L * crtc.mode.htotal * crtc.mode.vtotal / (crtc.mode.clock * 1000L);
	printf("mode %ux%u@%u crtc %u pipe %u plane %u period %ld us\n", crtc.mode.hdisplay, crtc.mode.vdisplay,
	       crtc.mode.vrefresh, crtc.crtc_id, pipe, plane, period_us);
	for (i = 0; argc == 5 && i < count; i++) {
		usleep(atoi(argv[4]));
		printf("poll %u src_x %lld\n", i, (long long)(property(plane, DRM_MODE_OBJECT_PLANE, "SRC_X") >> 16));
	}
	for (i = 0; argc == 4 && i < count; i++) {
		union drm_wait_vblank vbl = { .request = {
			.type = _DRM_VBLANK_RELATIVE | ((pipe << _DRM_VBLANK_HIGH_CRTC_SHIFT) & _DRM_VBLANK_HIGH_CRTC_MASK),
			.sequence = 1 } };

		if (xioctl(DRM_IOCTL_WAIT_VBLANK, &vbl)) {
			fprintf(stderr, "WAIT_VBLANK: %s\n", strerror(errno));
			return 1;
		}
		usleep(period_us / 2);
		printf("seq %u src_x %lld src_w %lld\n", vbl.reply.sequence,
		       (long long)(property(plane, DRM_MODE_OBJECT_PLANE, "SRC_X") >> 16),
		       (long long)(property(plane, DRM_MODE_OBJECT_PLANE, "SRC_W") >> 16));
	}
	return 0;
}

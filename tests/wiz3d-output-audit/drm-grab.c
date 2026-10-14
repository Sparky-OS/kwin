// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * drm-grab: saves the frame a CRTC scans out, as a binary PPM, without being
 * the DRM master (needs CAP_SYS_ADMIN for the buffer handle, as root in the
 * test guest). Prints the CRTC's mode, its stereo layout and the frame size.
 *
 *   drm-grab CARD OUT.ppm
 *
 * The primary plane of the first active CRTC is read through
 * DRM_IOCTL_MODE_GETFB2 and DRM_IOCTL_MODE_MAP_DUMB. RGB formats of 8, 10 and
 * 16 bits per channel; the PPM keeps the high 8 bits. The last line printed is
 * the number of pixels that are not black.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <drm_mode.h>

static int fd;

/* Bits per pixel of the formats this tool reads, 0 for the others. */
static int bits_of(uint32_t format)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		return 32;
	case DRM_FORMAT_XRGB16161616:
	case DRM_FORMAT_ARGB16161616:
	case DRM_FORMAT_XBGR16161616:
	case DRM_FORMAT_ABGR16161616:
		return 64;
	}
	return 0;
}

/* The 8-bit red, green and blue of pixel x of a row (the high bits of each channel). */
static void to_rgb(uint32_t format, const uint8_t *row, unsigned int x, uint8_t rgb[3])
{
	uint64_t p = bits_of(format) == 64 ? ((const uint64_t *)row)[x] : ((const uint32_t *)row)[x];

	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		rgb[0] = p >> 16; rgb[1] = p >> 8; rgb[2] = p;
		break;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		rgb[0] = p; rgb[1] = p >> 8; rgb[2] = p >> 16;
		break;
	case DRM_FORMAT_XRGB2101010:
	case DRM_FORMAT_ARGB2101010:
		rgb[0] = p >> 22; rgb[1] = p >> 12; rgb[2] = p >> 2;
		break;
	case DRM_FORMAT_XBGR2101010:
	case DRM_FORMAT_ABGR2101010:
		rgb[0] = p >> 2; rgb[1] = p >> 12; rgb[2] = p >> 22;
		break;
	case DRM_FORMAT_XRGB16161616:
	case DRM_FORMAT_ARGB16161616:
		rgb[0] = p >> 40; rgb[1] = p >> 24; rgb[2] = p >> 8;
		break;
	default:	/* XBGR16161616, ABGR16161616 */
		rgb[0] = p >> 8; rgb[1] = p >> 24; rgb[2] = p >> 40;
		break;
	}
}

static int xioctl(unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && (errno == EINTR || errno == EAGAIN));
	return r ? -errno : 0;
}

static void print_plane_properties(uint32_t plane)
{
	struct drm_mode_obj_get_properties props = {.obj_id = plane, .obj_type = DRM_MODE_OBJECT_PLANE};
	uint32_t ids[64];
	uint64_t values[64];
	if (xioctl(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props) || props.count_props > 64)
		exit(1);
	props.props_ptr = (uintptr_t)ids;
	props.prop_values_ptr = (uintptr_t)values;
	if (xioctl(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &props))
		exit(1);
	for (unsigned int i = 0; i < props.count_props; ++i) {
		struct drm_mode_get_property prop = {.prop_id = ids[i]};
		if (xioctl(DRM_IOCTL_MODE_GETPROPERTY, &prop))
			exit(1);
		printf("property %s %llu\n", prop.name, (unsigned long long)values[i]);
	}
}

int main(int argc, char **argv)
{
	struct drm_mode_get_plane_res pres = { 0 };
	struct drm_mode_card_res res = { 0 };
	struct drm_mode_crtc crtc = { 0 };
	uint32_t crtcs[8], plane_ids[32];
	struct drm_mode_fb_cmd2 fb = { 0 };
	struct drm_mode_map_dumb map = { 0 };
	unsigned int i, x, y, plane_id = 0, tries;
	unsigned long nonblack = 0;
	uint8_t *pixels, *copy;
	size_t size;
	FILE *out;

	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: %s CARD OUT.ppm [ACTIVE_CRTC_INDEX]\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	xioctl(DRM_IOCTL_SET_CLIENT_CAP, &(struct drm_set_client_cap){ DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 });
	xioctl(DRM_IOCTL_SET_CLIENT_CAP, &(struct drm_set_client_cap){ DRM_CLIENT_CAP_ATOMIC, 1 });
	xioctl(DRM_IOCTL_SET_CLIENT_CAP, &(struct drm_set_client_cap){ DRM_CLIENT_CAP_STEREO_3D, 1 });

	if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res) || res.count_crtcs > 8)
		return 1;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.count_connectors = res.count_encoders = res.count_fbs = 0;
	if (xioctl(DRM_IOCTL_MODE_GETRESOURCES, &res))
		return 1;
	int active_index = argc == 4 ? atoi(argv[3]) : 0;
	for (i = 0; i < res.count_crtcs; i++) {
		crtc.crtc_id = crtcs[i];
		if (!xioctl(DRM_IOCTL_MODE_GETCRTC, &crtc) && crtc.mode_valid && active_index-- == 0)
			break;
	}
	if (i == res.count_crtcs) {
		fprintf(stderr, "no active CRTC\n");
		return 1;
	}

	if (xioctl(DRM_IOCTL_MODE_GETPLANERESOURCES, &pres) || pres.count_planes > 32)
		return 1;
	pres.plane_id_ptr = (uintptr_t)plane_ids;
	if (xioctl(DRM_IOCTL_MODE_GETPLANERESOURCES, &pres))
		return 1;
	for (i = 0; i < pres.count_planes; i++) {
		struct drm_mode_get_plane p = { .plane_id = plane_ids[i] };

		if (xioctl(DRM_IOCTL_MODE_GETPLANE, &p))
			continue;
		printf("plane %u crtc %u fb %u\n", p.plane_id, p.crtc_id, p.fb_id);
		if (p.crtc_id == crtc.crtc_id && p.fb_id && !plane_id) {
			plane_id = p.plane_id;	/* the primary plane comes first */
			fb.fb_id = p.fb_id;
		}
	}
	printf("mode %s %ux%u total %ux%u clock %u flags 0x%x layout %u\n", crtc.mode.name,
	       crtc.mode.hdisplay, crtc.mode.vdisplay, crtc.mode.htotal, crtc.mode.vtotal,
	       crtc.mode.clock, crtc.mode.flags, (crtc.mode.flags & DRM_MODE_FLAG_3D_MASK) >> 14);
	if (!fb.fb_id || xioctl(DRM_IOCTL_MODE_GETFB2, &fb)) {
		fprintf(stderr, "GETFB2: %s\n", strerror(errno));
		return 1;
	}
	printf("fb %u %ux%u format %.4s pitch %u handle %u\n", fb.fb_id, fb.width, fb.height,
	       (char *)&fb.pixel_format, fb.pitches[0], fb.handles[0]);
	print_plane_properties(plane_id);
	if (!fb.handles[0] || !bits_of(fb.pixel_format)) {
		fprintf(stderr, "no handle or an unknown format\n");
		return 1;
	}
	map.handle = fb.handles[0];
	if (xioctl(DRM_IOCTL_MODE_MAP_DUMB, &map)) {
		fprintf(stderr, "MAP_DUMB: %s\n", strerror(errno));
		return 1;
	}
	size = (size_t)fb.pitches[0] * fb.height + fb.offsets[0];
	pixels = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, map.offset);
	copy = malloc(size);
	if (pixels == MAP_FAILED || !copy) {
		perror("mmap");
		return 1;
	}
	/*
	 * The compositor renders into its other buffers meanwhile: copy the
	 * scanned-out one at once, and again if the plane flipped away from it
	 * while copying.
	 */
	for (tries = 0; tries < 50; tries++) {
		struct drm_mode_get_plane p = { .plane_id = plane_id };

		memcpy(copy, pixels, size);
		if (!xioctl(DRM_IOCTL_MODE_GETPLANE, &p) && p.fb_id == fb.fb_id)
			break;
		usleep(5000);
	}
	printf("copied after %u retries\n", tries);
	pixels = copy;
	out = fopen(argv[2], "wb");
	if (!out) {
		perror(argv[2]);
		return 1;
	}
	fprintf(out, "P6\n%u %u\n255\n", fb.width, fb.height);
	for (y = 0; y < fb.height; y++) {
		const uint8_t *row = pixels + fb.offsets[0] + (size_t)y * fb.pitches[0];

		for (x = 0; x < fb.width; x++) {
			uint8_t rgb[3];

			to_rgb(fb.pixel_format, row, x, rgb);
			fwrite(rgb, 1, 3, out);
			nonblack += rgb[0] || rgb[1] || rgb[2];
		}
	}
	fclose(out);
	printf("saved %s\nnonblack %lu\n", argv[2], nonblack);
	return 0;
}

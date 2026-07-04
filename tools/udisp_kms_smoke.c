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
#include <xf86drm.h>
#include <xf86drmMode.h>

static void fill_pattern(uint32_t *pix, uint32_t width, uint32_t height,
			 uint32_t pitch_bytes)
{
	uint32_t x, y;
	uint32_t stride = pitch_bytes / 4;

	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			uint8_t r = 24, g = 24, b = 24;

			if (x < 70 && y < 70) {
				r = 255; g = 0; b = 0;
			} else if (x >= width - 70 && y < 70) {
				r = 0; g = 255; b = 0;
			} else if (x < 70 && y >= height - 70) {
				r = 0; g = 0; b = 255;
			} else if (x >= width - 70 && y >= height - 70) {
				r = 255; g = 255; b = 0;
			} else if ((x % 50) < 3 || (y % 100) < 4) {
				r = 255; g = 255; b = 255;
			}

			pix[y * stride + x] = (r << 16) | (g << 8) | b;
		}
	}
}

int main(int argc, char **argv)
{
	const char *node = argc > 1 ? argv[1] : "/dev/dri/card0";
	int fd = -1, ret = 1;
	drmModeRes *res = NULL;
	drmModeConnector *conn = NULL;
	drmModeEncoder *enc = NULL;
	drmModeCrtc *old_crtc = NULL;
	drmModeModeInfo mode;
	uint32_t conn_id = 0, crtc_id = 0, fb_id = 0;
	struct drm_mode_create_dumb create = {0};
	struct drm_mode_map_dumb map = {0};
	struct drm_mode_destroy_dumb destroy = {0};
	void *ptr = MAP_FAILED;
	int i, j;

	fd = open(node, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	res = drmModeGetResources(fd);
	if (!res) {
		perror("drmModeGetResources");
		goto out;
	}

	for (i = 0; i < res->count_connectors; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn)
			continue;
		if (conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
			break;
		drmModeFreeConnector(conn);
		conn = NULL;
	}
	if (!conn) {
		fprintf(stderr, "no connected connector with modes\n");
		goto out;
	}

	conn_id = conn->connector_id;
	mode = conn->modes[0];

	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	if (enc)
		crtc_id = enc->crtc_id;

	if (!crtc_id) {
		for (i = 0; !crtc_id && i < conn->count_encoders; i++) {
			drmModeEncoder *candidate = drmModeGetEncoder(fd, conn->encoders[i]);

			if (!candidate)
				continue;
			for (j = 0; j < res->count_crtcs; j++) {
				if (candidate->possible_crtcs & (1 << j)) {
					crtc_id = res->crtcs[j];
					break;
				}
			}
			drmModeFreeEncoder(candidate);
		}
	}
	if (!crtc_id) {
		fprintf(stderr, "no usable crtc\n");
		goto out;
	}

	create.width = mode.hdisplay;
	create.height = mode.vdisplay;
	create.bpp = 32;
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		perror("CREATE_DUMB");
		goto out;
	}

	if (drmModeAddFB(fd, create.width, create.height, 24, 32,
			 create.pitch, create.handle, &fb_id)) {
		perror("drmModeAddFB");
		goto out;
	}

	map.handle = create.handle;
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0) {
		perror("MAP_DUMB");
		goto out;
	}

	ptr = mmap(NULL, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		   map.offset);
	if (ptr == MAP_FAILED) {
		perror("mmap");
		goto out;
	}

	fill_pattern(ptr, create.width, create.height, create.pitch);

	old_crtc = drmModeGetCrtc(fd, crtc_id);
	if (drmModeSetCrtc(fd, crtc_id, fb_id, 0, 0, &conn_id, 1, &mode)) {
		perror("drmModeSetCrtc");
		goto out;
	}

	printf("set %s connector=%u crtc=%u mode=%ux%u@%.3f, sleeping 8s\n",
	       node, conn_id, crtc_id, mode.hdisplay, mode.vdisplay,
	       mode.clock * 1000.0 / mode.htotal / mode.vtotal);
	sleep(8);
	ret = 0;

out:
	if (old_crtc) {
		drmModeSetCrtc(fd, old_crtc->crtc_id, old_crtc->buffer_id,
			       old_crtc->x, old_crtc->y, &conn_id, 1,
			       old_crtc->mode_valid ? &old_crtc->mode : NULL);
		drmModeFreeCrtc(old_crtc);
	}
	if (ptr != MAP_FAILED)
		munmap(ptr, create.size);
	if (fb_id)
		drmModeRmFB(fd, fb_id);
	if (create.handle) {
		destroy.handle = create.handle;
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	}
	if (enc)
		drmModeFreeEncoder(enc);
	if (conn)
		drmModeFreeConnector(conn);
	if (res)
		drmModeFreeResources(res);
	if (fd >= 0)
		close(fd);
	return ret;
}

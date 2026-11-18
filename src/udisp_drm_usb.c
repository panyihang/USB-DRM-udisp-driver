// SPDX-License-Identifier: GPL-2.0

#include <linux/module.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/usb.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/vmalloc.h>
#include <linux/ktime.h>
#include <asm/unaligned.h>
#include "inc/tiny_jpeg.h"

#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_drv.h>
#include <drm/drm_edid.h>
#include <drm/drm_fbdev_generic.h>
#include <drm/drm_cache.h>
#include <drm/drm_file.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_managed.h>
#include <drm/drm_modes.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>

#define UDISP_USB_VID 0x303a
#define UDISP_USB_PID 0x2986
#define UDISP_USB_PID_DISPLAY 0x2987
#define UDISP_VENDOR_INTERFACE 0
#define UDISP_TYPE_RGB565 0
#define UDISP_TYPE_JPG 3
#define UDISP_TYPE_END 0xff

#define UDISP_DEFAULT_WIDTH 452
#define UDISP_DEFAULT_HEIGHT 1280
#define UDISP_DEFAULT_FPS 45
#define UDISP_DEFAULT_LIMIT 300000
#define UDISP_DEFAULT_KEEPALIVE_MS 10000
#define UDISP_MAX_PAYLOAD 0x003fffffU
#define UDISP_MAX_TRANSFER (UDISP_MAX_PAYLOAD + 16)
#define UDISP_JPEG_ALIGN 16
#define UDISP_RING_SLOTS 3
#define UDISP_ROTATE_TILE_ROWS 32

/* Keep the KMS device stable across cable reconnects.  In particular, older
 * compositors cannot reliably remove/re-add a secondary GPU at the same minor.
 */
static bool persistent_drm = true;
module_param(persistent_drm, bool, 0444);
MODULE_PARM_DESC(persistent_drm, "retain the DRM device across USB disconnect/reconnect (default true)");

static LIST_HEAD(udisp_devices);
static DEFINE_MUTEX(udisp_devices_lock);

static int rotate = 90;
module_param(rotate, int, 0644);
MODULE_PARM_DESC(rotate, "host-to-device rotation: 0, 90, 180, 270. Default 90.");

static bool host_native;
module_param(host_native, bool, 0644);
MODULE_PARM_DESC(host_native, "expose device-native portrait mode to host; default 0 exposes landscape mode and rotates into the device");

static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "enable verbose frame send logging");

static char *connector_type = "usb";
module_param(connector_type, charp, 0644);
MODULE_PARM_DESC(connector_type, "DRM connector type: usb, hdmi, dvi, virtual, dp");

static int jpeg_quality;
module_param(jpeg_quality, int, 0644);
MODULE_PARM_DESC(jpeg_quality, "override device-reported JPEG quality when set to 1..10");

static int jpeg_subsampling = 420;
module_param(jpeg_subsampling, int, 0644);
MODULE_PARM_DESC(jpeg_subsampling, "JPEG sampling: 420 (Windows-compatible default) or 444");

static bool test_pattern;
module_param(test_pattern, bool, 0644);
MODULE_PARM_DESC(test_pattern, "send an internally generated RGB test pattern instead of reading the framebuffer");

static int keepalive_ms = UDISP_DEFAULT_KEEPALIVE_MS;
module_param(keepalive_ms, int, 0644);
MODULE_PARM_DESC(keepalive_ms, "resend the current JPEG frame after this many static milliseconds; 0 disables keepalive");

struct udisp_frame_header {
	__le16 crc16;
	u8 type;
	u8 cmd;
	__le16 x;
	__le16 y;
	__le16 width;
	__le16 height;
	__le32 frame_payload;
} __packed;

struct udisp_frame_slot {
	u8 *tx_buf;
	u8 *jpg_src;
	size_t payload_len;
	u8 quality;
	bool busy;
	bool ready;
	bool keepalive;
	u64 seq;
	u64 t0;
	u64 t_convert;
	u64 t_encode;
	u32 fb_width;
	u32 fb_height;
	u32 fb_pitch;
	u32 fb_cpp;
	u32 fb_format;
	u32 auto_rotate;
	bool iomem;
};

struct udisp_device {
	struct kref refcount;
	struct list_head node;
	struct platform_device *platform;
	struct usb_device *udev;
	struct usb_interface *intf;
	u8 out_ep;
	u16 out_ep_size;

	struct drm_device *drm;
	struct drm_connector connector;
	struct drm_simple_display_pipe pipe;

	struct delayed_work flush_work;
	struct delayed_work hotplug_work;
	struct work_struct send_work;
	struct workqueue_struct *wq;
	struct workqueue_struct *tx_wq;
	struct mutex lock;
	bool alive;
	bool drm_ready;
	bool enabled;
	bool flush_pending;

	u16 dev_width;
	u16 dev_height;
	u16 host_width;
	u16 host_height;
	u16 jpg_width;
	u16 jpg_height;
	int effective_rotate;
	u16 fps;
	u8 jpg_quality;
	u32 frame_limit;
	u16 frame_id;
	unsigned long last_frame_jiffies;
	u64 last_send_start_ns;
	struct udisp_frame_header *end_header;
	u64 frames_sent;
	u64 frames_dropped;
	u64 encode_failures;
	u64 usb_failures;
	u64 last_stat_ns;
	u64 last_stat_frames;
	u64 next_slot_seq;

	struct drm_framebuffer *pending_fb;
	struct drm_framebuffer *current_fb;
	size_t tx_buf_size;
	size_t jpg_src_size;
	struct udisp_frame_slot slots[UDISP_RING_SLOTS];
	u8 *last_tx_buf;
	size_t last_payload_len;
	u8 last_quality;
	bool has_last_frame;
	u8 *line_buf;
	size_t line_buf_size;
	u8 *rotate_buf;
	size_t rotate_buf_size;
	u8 edid[EDID_LENGTH];

	char manufacturer[128];
	char product[128];
	char serial[128];
	char interface_name[128];
	char usb_path[128];
};

/* USB probe and DRM open files have independent lifetimes. */
static void udisp_device_release(struct kref *ref)
{
	struct udisp_device *ud = container_of(ref, struct udisp_device, refcount);

	usb_put_intf(ud->intf);
	usb_put_dev(ud->udev);
	if (ud->platform)
		platform_device_unregister(ud->platform);
	kfree(ud);
}

static inline struct udisp_device *connector_to_udisp(struct drm_connector *connector)
{
	return container_of(connector, struct udisp_device, connector);
}

static inline struct udisp_device *pipe_to_udisp(struct drm_simple_display_pipe *pipe)
{
	return container_of(pipe, struct udisp_device, pipe);
}

static void udisp_queue_flush(struct udisp_device *ud, unsigned long delay)
{
	ud->flush_pending = true;
	mod_delayed_work(ud->wq ? ud->wq : system_wq, &ud->flush_work, delay);
}

static void udisp_replace_fb_locked(struct drm_framebuffer **slot,
				    struct drm_framebuffer *fb)
{
	if (fb)
		drm_framebuffer_get(fb);
	if (*slot)
		drm_framebuffer_put(*slot);
	*slot = fb;
}

static unsigned long udisp_keepalive_delay(struct udisp_device *ud)
{
	unsigned long interval;
	unsigned long next;

	if (keepalive_ms <= 0)
		return 0;

	interval = msecs_to_jiffies(keepalive_ms);
	if (!interval)
		interval = 1;
	if (!ud->last_frame_jiffies)
		return 0;

	next = ud->last_frame_jiffies + interval;
	if (time_after_eq(jiffies, next))
		return 0;
	return next - jiffies;
}

static bool udisp_queue_keepalive_locked(struct udisp_device *ud)
{
	int i;

	if (!ud->alive || !ud->enabled || !ud->current_fb || keepalive_ms <= 0)
		return false;

	/* A queued JPEG will refresh last_frame_jiffies when USB completes.
	 * Requeueing an overdue keepalive before then spins and fills the ring.
	 * The sender rearms the timer after the last in-flight slot is freed.
	 */
	for (i = 0; i < UDISP_RING_SLOTS; i++)
		if (ud->slots[i].busy)
			return false;

	udisp_queue_flush(ud, max_t(unsigned long, msecs_to_jiffies(20),
				   udisp_keepalive_delay(ud)));
	return true;
}

static struct udisp_frame_slot *udisp_get_free_slot_locked(struct udisp_device *ud)
{
	int i;

	for (i = 0; i < UDISP_RING_SLOTS; i++) {
		if (!ud->slots[i].busy) {
			ud->slots[i].busy = true;
			ud->slots[i].ready = false;
			ud->slots[i].seq = ud->next_slot_seq++;
			return &ud->slots[i];
		}
	}

	return NULL;
}

static struct udisp_frame_slot *udisp_get_ready_slot_locked(struct udisp_device *ud)
{
	struct udisp_frame_slot *best = NULL;
	int i;

	for (i = 0; i < UDISP_RING_SLOTS; i++) {
		if (!ud->slots[i].ready)
			continue;
		if (!best || ud->slots[i].seq < best->seq)
			best = &ud->slots[i];
	}
	if (best)
		best->ready = false;
	return best;
}

static __le32 udisp_pack_frame_payload(u16 frame_id, u32 payload_total)
{
	return cpu_to_le32(((u32)(frame_id & 0x03ffu)) |
			   ((payload_total & 0x003fffffu) << 10));
}

static u16 udisp_align_u16(u16 value, u16 align)
{
	return (value + align - 1) & ~(align - 1);
}

static void udisp_edid_checksum(u8 *edid)
{
	u8 sum = 0;
	int i;

	for (i = 0; i < EDID_LENGTH - 1; i++)
		sum += edid[i];
	edid[EDID_LENGTH - 1] = (u8)(0 - sum);
}

static void udisp_generate_edid(struct udisp_device *ud)
{
	u16 width = ud->host_width;
	u16 height = ud->host_height;
	u16 fps = ud->fps ? ud->fps : UDISP_DEFAULT_FPS;
	u32 hblank = width < 640 ? 80 : 160;
	u32 vblank = 32;
	u32 htotal = width + hblank;
	u32 vtotal = height + vblank;
	u32 pixel_clock_10khz = DIV_ROUND_CLOSEST(htotal * vtotal * fps, 10000);
	u16 width_mm = (ud->effective_rotate == 90 || ud->effective_rotate == 270) ? 148 : 52;
	u16 height_mm = (ud->effective_rotate == 90 || ud->effective_rotate == 270) ? 52 : 148;
	u8 *edid = ud->edid;
	u8 *dtd;

	memset(edid, 0, EDID_LENGTH);
	edid[0] = 0x00;
	edid[1] = 0xff;
	edid[2] = 0xff;
	edid[3] = 0xff;
	edid[4] = 0xff;
	edid[5] = 0xff;
	edid[6] = 0xff;
	edid[7] = 0x00;
	edid[8] = 0x60;  /* XFY */
	edid[9] = 0xd9;
	edid[10] = 0x86;
	edid[11] = 0x19;
	edid[12] = 0x01;
	edid[13] = 0x00;
	edid[14] = 0x00;
	edid[15] = 0x00;
	edid[16] = 0x01;
	edid[17] = 0x24;
	edid[18] = 0x01;
	edid[19] = 0x04;
	edid[20] = 0xa5;
	edid[21] = DIV_ROUND_CLOSEST(width_mm, 10);
	edid[22] = DIV_ROUND_CLOSEST(height_mm, 10);
	edid[23] = 0x78;
	edid[24] = 0x0a;
	edid[25] = 0xcf;
	edid[26] = 0x74;
	edid[27] = 0xa3;
	edid[28] = 0x57;
	edid[29] = 0x4c;
	edid[30] = 0xb0;
	edid[31] = 0x23;
	edid[32] = 0x09;
	edid[33] = 0x48;
	edid[34] = 0x4c;

	memset(&edid[35], 0x01, 19);

	dtd = &edid[54];
	dtd[0] = pixel_clock_10khz & 0xff;
	dtd[1] = (pixel_clock_10khz >> 8) & 0xff;
	dtd[2] = width & 0xff;
	dtd[3] = hblank & 0xff;
	dtd[4] = (((width >> 8) & 0x0f) << 4) | ((hblank >> 8) & 0x0f);
	dtd[5] = height & 0xff;
	dtd[6] = vblank & 0xff;
	dtd[7] = (((height >> 8) & 0x0f) << 4) | ((vblank >> 8) & 0x0f);
	dtd[8] = 16;
	dtd[9] = 32;
	dtd[10] = (4 << 4) | 8;
	dtd[11] = 0;
	dtd[12] = width_mm & 0xff;
	dtd[13] = height_mm & 0xff;
	dtd[14] = (((width_mm >> 8) & 0x0f) << 4) | ((height_mm >> 8) & 0x0f);
	dtd[17] = 0x1a;

	edid[72] = 0x00;
	edid[73] = 0x00;
	edid[74] = 0x00;
	edid[75] = 0xfc;
	edid[76] = 0x00;
	memcpy(&edid[77], "USB-UDISP\n   ", 13);

	edid[90] = 0x00;
	edid[91] = 0x00;
	edid[92] = 0x00;
	edid[93] = 0xfd;
	edid[94] = 0x00;
	edid[95] = fps > 5 ? fps - 5 : fps;
	edid[96] = fps + 5;
	edid[97] = 0x1e;
	edid[98] = 0x50;
	edid[99] = 0x08;
	edid[100] = 0x00;
	edid[101] = 0x0a;
	memset(&edid[102], 0x20, 6);

	udisp_edid_checksum(edid);
}

static void udisp_parse_token(struct udisp_device *ud, const char *token)
{
	int a;
	int b;

	if (sscanf(token, "R%dx%d", &a, &b) == 2 && a > 0 && a <= 8192 && b > 0 && b <= 8192) {
		ud->dev_width = a;
		ud->dev_height = b;
		return;
	}
	if (sscanf(token, "Fps%d", &a) == 1 && a > 0 && a <= 240) {
		ud->fps = a;
		return;
	}
	if (sscanf(token, "Ejpg%d", &a) == 1 && a >= 1 && a <= 10) {
		ud->jpg_quality = a;
		return;
	}
	if (sscanf(token, "Bl%d", &a) == 1 && a > 0) {
		/* Windows protocol expresses Bl in KiB, not bytes. */
		ud->frame_limit = min_t(u64, (u64)a * 1024, UDISP_MAX_PAYLOAD);
		return;
	}
}

static void udisp_parse_info_string(struct udisp_device *ud, const char *value)
{
	char tmp[256];
	char *p;
	char *token;

	if (!value || !value[0])
		return;

	strscpy(tmp, value, sizeof(tmp));
	p = tmp;
	while ((token = strsep(&p, "_")) != NULL)
		udisp_parse_token(ud, token);
}

static void udisp_parse_usb_strings(struct udisp_device *ud)
{
	struct usb_host_interface *alt = ud->intf->cur_altsetting;

	ud->dev_width = UDISP_DEFAULT_WIDTH;
	ud->dev_height = UDISP_DEFAULT_HEIGHT;
	ud->fps = UDISP_DEFAULT_FPS;
	ud->jpg_quality = 6;
	ud->frame_limit = UDISP_DEFAULT_LIMIT;

	usb_string(ud->udev, ud->udev->descriptor.iManufacturer,
		   ud->manufacturer, sizeof(ud->manufacturer));
	usb_string(ud->udev, ud->udev->descriptor.iProduct,
		   ud->product, sizeof(ud->product));
	usb_string(ud->udev, ud->udev->descriptor.iSerialNumber,
		   ud->serial, sizeof(ud->serial));
	if (alt && alt->desc.iInterface)
		usb_string(ud->udev, alt->desc.iInterface,
			   ud->interface_name, sizeof(ud->interface_name));

	udisp_parse_info_string(ud, ud->product);
	udisp_parse_info_string(ud, ud->interface_name);
	if (jpeg_quality >= 1 && jpeg_quality <= 10)
		ud->jpg_quality = jpeg_quality;

	if (host_native) {
		ud->host_width = ud->dev_width;
		ud->host_height = ud->dev_height;
		ud->effective_rotate = 0;
	} else if (rotate == 90 || rotate == 270) {
		ud->host_width = ud->dev_height;
		ud->host_height = ud->dev_width;
		ud->effective_rotate = rotate;
	} else {
		ud->host_width = ud->dev_width;
		ud->host_height = ud->dev_height;
		ud->effective_rotate = rotate;
	}
	ud->jpg_width = udisp_align_u16(ud->dev_width, UDISP_JPEG_ALIGN);
	ud->jpg_height = udisp_align_u16(ud->dev_height, UDISP_JPEG_ALIGN);
	udisp_generate_edid(ud);

	dev_info(&ud->intf->dev,
		 "display info: product='%s' interface='%s' dev=%ux%u host=%ux%u jpeg=%ux%u fps=%u jpg=%u limit=%u rotate=%d effective_rotate=%d host_native=%d\n",
		 ud->product, ud->interface_name, ud->dev_width, ud->dev_height,
		 ud->host_width, ud->host_height, ud->jpg_width, ud->jpg_height,
		 ud->fps, ud->jpg_quality,
		 ud->frame_limit, rotate, ud->effective_rotate, host_native);
}

static int udisp_connector_type(void)
{
	if (!connector_type)
		return DRM_MODE_CONNECTOR_USB;
	if (!strcmp(connector_type, "usb"))
		return DRM_MODE_CONNECTOR_USB;
	if (!strcmp(connector_type, "hdmi") || !strcmp(connector_type, "hdmi-a"))
		return DRM_MODE_CONNECTOR_HDMIA;
	if (!strcmp(connector_type, "dvi") || !strcmp(connector_type, "dvi-i"))
		return DRM_MODE_CONNECTOR_DVII;
	if (!strcmp(connector_type, "virtual"))
		return DRM_MODE_CONNECTOR_VIRTUAL;
	if (!strcmp(connector_type, "dp") || !strcmp(connector_type, "displayport"))
		return DRM_MODE_CONNECTOR_DisplayPort;

	pr_warn("udisp_drm: unknown connector_type='%s', using usb\n", connector_type);
	return DRM_MODE_CONNECTOR_USB;
}

static enum drm_connector_status udisp_connector_detect(struct drm_connector *connector,
							bool force)
{
	struct udisp_device *ud = connector_to_udisp(connector);

	return READ_ONCE(ud->alive) && READ_ONCE(ud->drm_ready) ?
		connector_status_connected : connector_status_disconnected;
}

static void udisp_mode_fill(struct drm_display_mode *mode,
			    u16 width, u16 height, u16 fps)
{
	u32 hblank = width < 640 ? 80 : 160;
	u32 vblank = 32;
	u32 htotal = width + hblank;
	u32 vtotal = height + vblank;

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	mode->clock = (htotal * vtotal * fps) / 1000;
	mode->hdisplay = width;
	mode->hsync_start = width + 16;
	mode->hsync_end = width + 48;
	mode->htotal = htotal;
	mode->vdisplay = height;
	mode->vsync_start = height + 4;
	mode->vsync_end = height + 12;
	mode->vtotal = vtotal;
	mode->width_mm = 148;
	mode->height_mm = 52;
	drm_mode_set_name(mode);
}

static int udisp_connector_get_modes(struct drm_connector *connector)
{
	struct udisp_device *ud = connector_to_udisp(connector);
	struct drm_display_mode *mode;
	int count;

	drm_connector_update_edid_property(connector, (const struct edid *)ud->edid);
	count = drm_add_edid_modes(connector, (struct edid *)ud->edid);
	if (count > 0) {
		connector->display_info.width_mm =
			(ud->effective_rotate == 90 || ud->effective_rotate == 270) ? 148 : 52;
		connector->display_info.height_mm =
			(ud->effective_rotate == 90 || ud->effective_rotate == 270) ? 52 : 148;
		return count;
	}

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return 0;

	udisp_mode_fill(mode, ud->host_width, ud->host_height, ud->fps);
	drm_mode_probed_add(connector, mode);
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	return 1;
}

static void udisp_connector_destroy(struct drm_connector *connector)
{
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs udisp_connector_funcs = {
	.detect = udisp_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,
	.destroy = udisp_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs udisp_connector_helper_funcs = {
	.get_modes = udisp_connector_get_modes,
};

static enum drm_mode_status udisp_pipe_mode_valid(struct drm_simple_display_pipe *pipe,
						 const struct drm_display_mode *mode)
{
	struct udisp_device *ud = pipe_to_udisp(pipe);

	if (mode->hdisplay != ud->host_width || mode->vdisplay != ud->host_height)
		return MODE_ONE_SIZE;

	return MODE_OK;
}

static int udisp_pipe_check(struct drm_simple_display_pipe *pipe,
			    struct drm_plane_state *plane_state,
			    struct drm_crtc_state *crtc_state)
{
	crtc_state->no_vblank = true;
	return 0;
}

static void udisp_pipe_enable(struct drm_simple_display_pipe *pipe,
			      struct drm_crtc_state *crtc_state,
			      struct drm_plane_state *plane_state)
{
	struct udisp_device *ud = pipe_to_udisp(pipe);
	struct drm_framebuffer *fb = plane_state ? plane_state->fb : NULL;
	bool queued = false;

	mutex_lock(&ud->lock);
	ud->enabled = true;
	if (ud->alive && fb) {
		udisp_replace_fb_locked(&ud->current_fb, fb);
		udisp_replace_fb_locked(&ud->pending_fb, fb);
		udisp_queue_flush(ud, 0);
		queued = true;
	}
	mutex_unlock(&ud->lock);

	/* no_vblank reports completion as soon as this hook returns.  Keep
	 * the compositor's scanout buffer owned until CPU capture is finished.
	 * A framebuffer reference alone does not prevent producer reuse.
	 */
	if (queued)
		flush_delayed_work(&ud->flush_work);
}

static void udisp_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct udisp_device *ud = pipe_to_udisp(pipe);

	mutex_lock(&ud->lock);
	ud->enabled = false;
	udisp_replace_fb_locked(&ud->pending_fb, NULL);
	udisp_replace_fb_locked(&ud->current_fb, NULL);
	mutex_unlock(&ud->lock);
}

static void udisp_pipe_update(struct drm_simple_display_pipe *pipe,
			      struct drm_plane_state *old_plane_state)
{
	struct udisp_device *ud = pipe_to_udisp(pipe);
	struct drm_framebuffer *fb = pipe->plane.state ? pipe->plane.state->fb : NULL;
	bool queued = false;

	if (!fb)
		return;

	mutex_lock(&ud->lock);
	if (ud->alive && ud->enabled) {
		udisp_replace_fb_locked(&ud->current_fb, fb);
		udisp_replace_fb_locked(&ud->pending_fb, fb);
		udisp_queue_flush(ud, 0);
		queued = true;
	}
	mutex_unlock(&ud->lock);

	/* Must complete the snapshot BEFORE DRM sends its fake page-flip
	 * event and allows the compositor to reuse the previous buffers.
	 * JPEG/USB data remains private to the ring after this barrier.
	 */
	if (queued)
		flush_delayed_work(&ud->flush_work);
}

static const struct drm_simple_display_pipe_funcs udisp_pipe_funcs = {
	.mode_valid = udisp_pipe_mode_valid,
	.enable = udisp_pipe_enable,
	.disable = udisp_pipe_disable,
	.check = udisp_pipe_check,
	.update = udisp_pipe_update,
};

static const char *udisp_fourcc_name(u32 format, char name[5])
{
	name[0] = format & 0xff;
	name[1] = (format >> 8) & 0xff;
	name[2] = (format >> 16) & 0xff;
	name[3] = (format >> 24) & 0xff;
	name[4] = '\0';
	return name;
}

static void udisp_read_rgb(const u8 *p, u32 format, u8 *r, u8 *g, u8 *b)
{
	switch (format) {
	case DRM_FORMAT_XRGB8888:
	case DRM_FORMAT_ARGB8888:
		*r = p[2];
		*g = p[1];
		*b = p[0];
		break;
	case DRM_FORMAT_RGBX8888:
	case DRM_FORMAT_RGBA8888:
		*r = p[3];
		*g = p[2];
		*b = p[1];
		break;
	case DRM_FORMAT_XBGR8888:
	case DRM_FORMAT_ABGR8888:
		*r = p[0];
		*g = p[1];
		*b = p[2];
		break;
	case DRM_FORMAT_BGRX8888:
	case DRM_FORMAT_BGRA8888:
		*r = p[1];
		*g = p[2];
		*b = p[3];
		break;
	case DRM_FORMAT_RGB565: {
		u16 v = get_unaligned_le16(p);

		*r = ((v >> 11) & 0x1f) << 3;
		*g = ((v >> 5) & 0x3f) << 2;
		*b = (v & 0x1f) << 3;
		break;
	}
	default:
		*r = p[2];
		*g = p[1];
		*b = p[0];
		break;
	}
}

static int udisp_auto_rotate(struct udisp_device *ud, struct drm_framebuffer *fb)
{
	if (ud->effective_rotate)
		return ud->effective_rotate;
	if (fb->width == ud->dev_height && fb->height == ud->dev_width &&
	    (rotate == 90 || rotate == 270))
		return rotate;
	return 0;
}

static void udisp_fill_test_pattern(struct udisp_device *ud, u8 *dst)
{
	u32 x;
	u32 y;

	for (y = 0; y < ud->jpg_height; y++) {
		for (x = 0; x < ud->jpg_width; x++) {
			u8 r = 20;
			u8 g = 20;
			u8 b = 20;
			u8 *out = dst + (y * ud->jpg_width + x) * 3;

			if (x >= ud->dev_width || y >= ud->dev_height) {
				r = 0; g = 0; b = 0;
			} else if (x < 70 && y < 70) {
				r = 255; g = 0; b = 0;
			} else if (x >= ud->dev_width - 70 && y < 70) {
				r = 0; g = 255; b = 0;
			} else if (x < 70 && y >= ud->dev_height - 70) {
				r = 0; g = 0; b = 255;
			} else if (x >= ud->dev_width - 70 && y >= ud->dev_height - 70) {
				r = 255; g = 255; b = 0;
			} else if ((x % 50) < 3 || (y % 100) < 4) {
				r = 255; g = 255; b = 255;
			}

			out[0] = r;
			out[1] = g;
			out[2] = b;
		}
	}
}

static void udisp_copy_from_fb(struct iosys_map *map, void *dst_buf,
			       size_t offset, size_t len)
{
	struct iosys_map dst;
	struct iosys_map src;

	iosys_map_set_vaddr(&dst, dst_buf);
	src = IOSYS_MAP_INIT_OFFSET(map, offset);
	drm_memcpy_from_wc(&dst, &src, len);
}

static void udisp_copy_line_from_fb(struct udisp_device *ud, struct iosys_map *map,
				    size_t offset, size_t len)
{
	udisp_copy_from_fb(map, ud->line_buf, offset, len);
}

static bool udisp_write_rotated_fb_to_rgb_fast(struct udisp_device *ud,
					       struct drm_framebuffer *fb,
					       u8 *dst, struct iosys_map *map,
					       int eff_rotate)
{
	u32 src_width = fb->width;
	u32 src_height = fb->height;
	u32 format = fb->format->format;
	u32 cpp = fb->format->cpp[0];
	u32 pitch = fb->pitches[0];
	u32 sx;
	u32 sy;
	u32 copy_width;
	u32 copy_len;
	u32 tile_rows;

	if (cpp > 4 || src_width * cpp > ud->line_buf_size)
		return false;
	if (src_width < ud->dev_height || src_height < ud->dev_width)
		return false;
	if (eff_rotate != 90 && eff_rotate != 270)
		return false;

	copy_width = ud->dev_height;
	copy_len = copy_width * cpp;
	if (!ud->rotate_buf || ud->rotate_buf_size < copy_len)
		return false;

	if (ud->jpg_width > ud->dev_width) {
		for (sy = 0; sy < ud->jpg_height; sy++)
			memset(dst + (sy * ud->jpg_width + ud->dev_width) * 3, 0,
			       (ud->jpg_width - ud->dev_width) * 3);
	}
	if (ud->jpg_height > ud->dev_height)
		memset(dst + ud->dev_height * ud->jpg_width * 3, 0,
		       (ud->jpg_height - ud->dev_height) * ud->jpg_width * 3);

	tile_rows = min_t(u32, UDISP_ROTATE_TILE_ROWS,
			  ud->rotate_buf_size / copy_len);
	if (!tile_rows)
		return false;

	for (sy = 0; sy < ud->dev_width; sy += tile_rows) {
		u32 rows = min_t(u32, tile_rows, ud->dev_width - sy);
		u32 ty;

		for (ty = 0; ty < rows; ty++)
			udisp_copy_from_fb(map, ud->rotate_buf + ty * copy_len,
					   (sy + ty) * pitch, copy_len);

		for (sx = 0; sx < copy_width; sx++) {
			u8 *out;

			if (eff_rotate == 90)
				out = dst + (sx * ud->jpg_width + ud->dev_width - 1 - sy) * 3;
			else
				out = dst + ((ud->dev_height - 1 - sx) * ud->jpg_width + sy) * 3;

			for (ty = 0; ty < rows; ty++) {
				const u8 *p = ud->rotate_buf + ty * copy_len + sx * cpp;

				if (format == DRM_FORMAT_XRGB8888 ||
				    format == DRM_FORMAT_ARGB8888) {
					out[0] = p[2];
					out[1] = p[1];
					out[2] = p[0];
				} else {
					udisp_read_rgb(p, format, &out[0], &out[1], &out[2]);
				}
				out += (eff_rotate == 90) ? -3 : 3;
			}
		}
	}

	return true;
}

static void udisp_write_fb_to_rgb(struct udisp_device *ud, struct drm_framebuffer *fb,
				  u8 *dst, struct iosys_map *map)
{
	u32 x;
	u32 y;
	u32 format = fb->format->format;
	u32 cpp = fb->format->cpp[0];
	u32 pitch = fb->pitches[0];
	int eff_rotate = udisp_auto_rotate(ud, fb);
	u32 src_width = fb->width;
	u32 src_height = fb->height;

	if (test_pattern) {
		udisp_fill_test_pattern(ud, dst);
		return;
	}

	if (!eff_rotate && src_width >= ud->dev_width && src_height >= ud->dev_height &&
	    cpp <= 4 && ud->dev_width * cpp <= ud->line_buf_size) {
		for (y = 0; y < ud->dev_height; y++) {
			const u8 *line = ud->line_buf;
			u8 *out = dst + y * ud->jpg_width * 3;

			udisp_copy_line_from_fb(ud, map, y * pitch, ud->dev_width * cpp);
			if (format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_ARGB8888) {
				for (x = 0; x < ud->dev_width; x++) {
					const u8 *p = line + x * 4;

					out[0] = p[2];
					out[1] = p[1];
					out[2] = p[0];
					out += 3;
				}
				if (ud->jpg_width > ud->dev_width)
					memset(out, 0, (ud->jpg_width - ud->dev_width) * 3);
				continue;
			}
			for (x = 0; x < ud->dev_width; x++) {
				udisp_read_rgb(line + x * cpp, format, &out[0], &out[1], &out[2]);
				out += 3;
			}
			if (ud->jpg_width > ud->dev_width)
				memset(out, 0, (ud->jpg_width - ud->dev_width) * 3);
		}
		for (; y < ud->jpg_height; y++)
			memset(dst + y * ud->jpg_width * 3, 0, ud->jpg_width * 3);
		return;
	}

	if (udisp_write_rotated_fb_to_rgb_fast(ud, fb, dst, map, eff_rotate))
		return;

	for (y = 0; y < ud->jpg_height; y++) {
		for (x = 0; x < ud->jpg_width; x++) {
			u32 hx;
			u32 hy;
			const u8 *p;
			u8 *out = dst + (y * ud->jpg_width + x) * 3;

			if (x >= ud->dev_width || y >= ud->dev_height) {
				out[0] = 0;
				out[1] = 0;
				out[2] = 0;
				continue;
			}

			switch (((eff_rotate % 360) + 360) % 360) {
			case 90:
				hx = y;
				hy = src_height - 1 - x;
				break;
			case 270:
				hx = src_width - 1 - y;
				hy = x;
				break;
			case 180:
				hx = src_width - 1 - x;
				hy = src_height - 1 - y;
				break;
			case 0:
			default:
				hx = x;
				hy = y;
				break;
			}

			if (hx >= src_width || hy >= src_height) {
				out[0] = 0;
				out[1] = 0;
				out[2] = 0;
				continue;
			}

			p = map->vaddr + hy * pitch + hx * cpp;
			udisp_read_rgb(p, format, &out[0], &out[1], &out[2]);
		}
	}
}

static int udisp_usb_send_jpeg(struct udisp_device *ud, struct udisp_frame_slot *slot)
{
    struct udisp_frame_header *hdr = (struct udisp_frame_header *)slot->tx_buf;
    int actual = 0;
    int ret;
    size_t total = sizeof(*hdr) + slot->payload_len;

    if (total > ud->tx_buf_size)
        return -ENOMEM;
    if (slot->payload_len > 0x003fffff)
        return -EINVAL;
    if (ud->frame_limit && slot->payload_len > ud->frame_limit)
        return -EMSGSIZE;

    memset(hdr, 0, sizeof(*hdr));
    hdr->type = UDISP_TYPE_JPG;
    hdr->x = cpu_to_le16(0);
    hdr->y = cpu_to_le16(0);
    hdr->width = cpu_to_le16(ud->dev_width);
    hdr->height = cpu_to_le16(ud->dev_height);
    hdr->frame_payload = udisp_pack_frame_payload(ud->frame_id++, slot->payload_len);

    /* Rate-limit the actual USB sender, not a deferred framebuffer read.
     * Otherwise ready slots can drain back-to-back after a scheduling stall.
     */
    if (ud->fps && ud->last_send_start_ns) {
        u64 deadline = ud->last_send_start_ns + DIV_ROUND_UP_ULL(NSEC_PER_SEC, ud->fps);
        u64 now = ktime_get_ns();

        if (deadline > now) {
            unsigned long wait_us = DIV_ROUND_UP_ULL(deadline - now, NSEC_PER_USEC);

            usleep_range(wait_us, wait_us + 100);
        }
    }
    ud->last_send_start_ns = ktime_get_ns();
    ret = usb_bulk_msg(ud->udev, usb_sndbulkpipe(ud->udev, ud->out_ep),
                       slot->tx_buf, total, &actual, 1000);
    if (ret)
        return ret;
    if (actual != total)
        return -EIO;

    /* Match Windows: an exact max-packet boundary needs a short END
     * transfer, so TinyUSB does not wait for the next frame to flush it.
     * end_header is kmalloc-backed; USB DMA must never use stack memory.
     */
    if (ud->out_ep_size && !(total % ud->out_ep_size)) {
        ret = usb_bulk_msg(ud->udev, usb_sndbulkpipe(ud->udev, ud->out_ep),
                           ud->end_header, sizeof(*ud->end_header), &actual, 1000);
        if (ret)
            return ret;
        if (actual != sizeof(*ud->end_header))
            return -EIO;
    }
    ud->last_frame_jiffies = jiffies;
    ud->frames_sent++;
    if (debug)
        dev_info_ratelimited(&ud->intf->dev,
                             "sent jpg frame=%llu payload=%zu total=%zu q=%u\n",
                             ud->frames_sent, slot->payload_len, total,
                             slot->quality);
    return 0;
}

static void udisp_send_work(struct work_struct *work)
{
	struct udisp_device *ud = container_of(work, struct udisp_device, send_work);
	struct udisp_frame_slot *slot;

	for (;;) {
		u64 t_send_start;
		u64 t_done;
		u64 t_send;
		int ret;

		mutex_lock(&ud->lock);
		if (!ud->alive) {
			mutex_unlock(&ud->lock);
			return;
		}
		slot = udisp_get_ready_slot_locked(ud);
		mutex_unlock(&ud->lock);

		if (!slot)
			return;

		t_send_start = ktime_get_ns();
		ret = udisp_usb_send_jpeg(ud, slot);
		t_done = ktime_get_ns();
		t_send = t_done - t_send_start;

		if (ret) {
			ud->usb_failures++;
			ud->frames_dropped++;
			dev_warn_ratelimited(&ud->intf->dev,
					     "usb send failed: ret=%d payload=%zu failures=%llu\n",
					     ret, slot->payload_len, ud->usb_failures);
		} else if (ud->last_tx_buf) {
			mutex_lock(&ud->lock);
			memcpy(ud->last_tx_buf + sizeof(struct udisp_frame_header),
			       slot->tx_buf + sizeof(struct udisp_frame_header),
			       slot->payload_len);
			ud->last_payload_len = slot->payload_len;
			ud->last_quality = slot->quality;
			ud->has_last_frame = true;
			mutex_unlock(&ud->lock);
		}

		if (debug && (ud->frames_sent <= 5 ||
			      t_done - ud->last_stat_ns > 5ULL * NSEC_PER_SEC)) {
			char fmt[5];
			u64 stat_ns = ud->last_stat_ns ? t_done - ud->last_stat_ns : 0;
			u64 stat_frames = ud->frames_sent - ud->last_stat_frames;
			u64 fps_x100 = stat_ns ?
				div64_u64(stat_frames * 100ULL * NSEC_PER_SEC, stat_ns) : 0;

			ud->last_stat_ns = t_done;
			ud->last_stat_frames = ud->frames_sent;
			dev_info(&ud->intf->dev,
				 "frame stats: fb=%ux%u pitch=%u cpp=%u fmt=%s out=%ux%u auto_rotate=%u iomem=%d test=%d keepalive=%d ring=%d convert=%llums encode=%llums usb=%llums total=%llums fps=%llu.%02llu sent=%llu dropped=%llu\n",
				 slot->fb_width, slot->fb_height, slot->fb_pitch, slot->fb_cpp,
				 udisp_fourcc_name(slot->fb_format, fmt),
				 ud->jpg_width, ud->jpg_height, slot->auto_rotate,
				 slot->iomem, test_pattern, slot->keepalive, UDISP_RING_SLOTS,
				 div_u64(slot->t_convert, NSEC_PER_MSEC),
				 div_u64(slot->t_encode, NSEC_PER_MSEC),
				 div_u64(t_send, NSEC_PER_MSEC),
				 div_u64(t_done - slot->t0, NSEC_PER_MSEC),
				 div_u64(fps_x100, 100), div_u64(fps_x100, 1) % 100,
				 ud->frames_sent, ud->frames_dropped);
		}

		mutex_lock(&ud->lock);
		slot->busy = false;
		slot->ready = false;
		if (!ud->pending_fb && !delayed_work_pending(&ud->flush_work))
			udisp_queue_keepalive_locked(ud);
		mutex_unlock(&ud->lock);
	}
}

static void udisp_flush_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct udisp_device *ud = container_of(dwork, struct udisp_device, flush_work);
	struct drm_framebuffer *fb;
	struct udisp_frame_slot *slot = NULL;
	struct iosys_map map, data;
	int ret;
	u8 *payload;
	u64 t0 = ktime_get_ns();
	u64 t_map;
	bool keepalive = false;
	bool encoded = false;

	mutex_lock(&ud->lock);
	fb = ud->pending_fb;
	ud->pending_fb = NULL;
	if (!fb && ud->alive && ud->enabled && ud->current_fb &&
	    keepalive_ms > 0 && !udisp_keepalive_delay(ud)) {
		fb = ud->current_fb;
		drm_framebuffer_get(fb);
		keepalive = true;
	}
	if (!fb) {
		if (!udisp_queue_keepalive_locked(ud))
			ud->flush_pending = false;
	}
	mutex_unlock(&ud->lock);

	if (!fb)
		return;

	mutex_lock(&ud->lock);
	slot = udisp_get_free_slot_locked(ud);
	if (!slot) {
		ud->frames_dropped++;
		mutex_unlock(&ud->lock);
		dev_warn_ratelimited(&ud->intf->dev,
				     "frame ring full: slots=%d dropped=%llu\n",
				     UDISP_RING_SLOTS, ud->frames_dropped);
		goto out_put_fb;
	}
	mutex_unlock(&ud->lock);

	slot->t0 = t0;
	slot->keepalive = keepalive;
	slot->payload_len = 0;
	slot->quality = 0;
	slot->fb_width = fb->width;
	slot->fb_height = fb->height;
	slot->fb_pitch = fb->pitches[0];
	slot->fb_cpp = fb->format->cpp[0];
	slot->fb_format = fb->format->format;
	slot->auto_rotate = udisp_auto_rotate(ud, fb);
	slot->t_convert = 0;
	slot->t_encode = 0;

	if (keepalive) {
		mutex_lock(&ud->lock);
		if (ud->has_last_frame && ud->last_payload_len <=
		    ud->tx_buf_size - sizeof(struct udisp_frame_header)) {
			memcpy(slot->tx_buf + sizeof(struct udisp_frame_header),
			       ud->last_tx_buf + sizeof(struct udisp_frame_header),
			       ud->last_payload_len);
			slot->payload_len = ud->last_payload_len;
			slot->quality = ud->last_quality;
			encoded = true;
		}
		mutex_unlock(&ud->lock);
		if (encoded)
			goto out;
	}

	ret = drm_gem_fb_begin_cpu_access(fb, DMA_FROM_DEVICE);
	if (ret)
		goto out;

	ret = drm_gem_fb_vmap(fb, &map, &data);
	if (!ret) {
		stream_mgr_t mgr;
		u8 quality;
		u64 t1;
		u64 t2;

		slot->iomem = data.is_iomem;
		payload = slot->tx_buf + sizeof(struct udisp_frame_header);
		t_map = ktime_get_ns();
		udisp_write_fb_to_rgb(ud, fb, slot->jpg_src, &data);
		t1 = ktime_get_ns();
		slot->t_convert = t1 - t_map;

		for (quality = ud->jpg_quality; quality >= 1; quality--) {
			t1 = ktime_get_ns();
			mgr.data = payload;
			mgr.max = ud->tx_buf_size - sizeof(struct udisp_frame_header);
			mgr.dp = 0;
			mgr.subsampling = jpeg_subsampling == 444 ? 0 : 2;
			if (!tje_encode_to_ctx(&mgr, ud->jpg_width, ud->jpg_height, 3,
					       slot->jpg_src, quality))
				continue;
			t2 = ktime_get_ns();
			slot->t_encode = t2 - t1;

			if (ud->frame_limit && mgr.dp > ud->frame_limit)
				continue;

			encoded = true;
			slot->payload_len = mgr.dp;
			slot->quality = quality;
			break;
		}

		if (!encoded) {
			ud->encode_failures++;
			ud->frames_dropped++;
			dev_warn_ratelimited(&ud->intf->dev,
					     "jpeg encode failed/oversize: quality=%u limit=%u failures=%llu\n",
					     ud->jpg_quality, ud->frame_limit,
					     ud->encode_failures);
		}

		drm_gem_fb_vunmap(fb, &map);
	}
	drm_gem_fb_end_cpu_access(fb, DMA_FROM_DEVICE);

out:
	if (slot) {
		mutex_lock(&ud->lock);
		if (encoded && ud->alive) {
			slot->ready = true;
			queue_work(ud->tx_wq ? ud->tx_wq : system_wq, &ud->send_work);
		} else {
			slot->busy = false;
			slot->ready = false;
		}
		mutex_unlock(&ud->lock);
	}

out_put_fb:
	drm_framebuffer_put(fb);

	mutex_lock(&ud->lock);
	if (ud->alive && ud->enabled && ud->pending_fb) {
		unsigned long delay = 0;
		unsigned long min_interval = ud->fps ?
			msecs_to_jiffies(DIV_ROUND_UP(1000, ud->fps)) : 0;

		if (min_interval && ud->last_frame_jiffies &&
		    time_before(jiffies, ud->last_frame_jiffies + min_interval))
			delay = (ud->last_frame_jiffies + min_interval) - jiffies;
		udisp_queue_flush(ud, delay);
	} else {
		if (!udisp_queue_keepalive_locked(ud))
			ud->flush_pending = false;
	}
	mutex_unlock(&ud->lock);
}

static const u32 udisp_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_RGBA8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_ABGR8888,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_BGRA8888,
	DRM_FORMAT_RGB565,
};

static const struct drm_mode_config_funcs udisp_mode_config_funcs = {
	.fb_create = drm_gem_fb_create_with_dirty,
	.atomic_check = drm_atomic_helper_check,
	.atomic_commit = drm_atomic_helper_commit,
};

DEFINE_DRM_GEM_FOPS(udisp_drm_fops);

static const struct drm_driver udisp_drm_driver = {
	.driver_features = DRIVER_MODESET | DRIVER_GEM | DRIVER_ATOMIC,
	DRM_GEM_SHMEM_DRIVER_OPS,
	.fops = &udisp_drm_fops,
	.name = "udisp_drm",
	.desc = "USB DRM udisp display",
	.date = "20260630",
	.major = 0,
	.minor = 1,
};

static void udisp_drm_release(struct drm_device *drm, void *ptr)
{
	struct udisp_device *ud = ptr;
	int i;

	cancel_delayed_work_sync(&ud->hotplug_work);
	cancel_delayed_work_sync(&ud->flush_work);
	cancel_work_sync(&ud->send_work);
	if (ud->wq) {
		destroy_workqueue(ud->wq);
		ud->wq = NULL;
	}
	if (ud->tx_wq) {
		destroy_workqueue(ud->tx_wq);
		ud->tx_wq = NULL;
	}
	if (ud->pending_fb) {
		drm_framebuffer_put(ud->pending_fb);
		ud->pending_fb = NULL;
	}
	if (ud->current_fb) {
		drm_framebuffer_put(ud->current_fb);
		ud->current_fb = NULL;
	}
	for (i = 0; i < UDISP_RING_SLOTS; i++) {
		vfree(ud->slots[i].jpg_src);
		kfree(ud->slots[i].tx_buf);
		ud->slots[i].jpg_src = NULL;
		ud->slots[i].tx_buf = NULL;
	}
	kfree(ud->end_header);
	ud->end_header = NULL;
	kfree(ud->last_tx_buf);
	ud->last_tx_buf = NULL;
	kfree(ud->rotate_buf);
	ud->rotate_buf = NULL;
	kfree(ud->line_buf);
	kref_put(&ud->refcount, udisp_device_release);
}

static int udisp_drm_init(struct udisp_device *ud)
{
	struct drm_device *drm;
	int ret;
	int i;
	size_t transfer_len;

	drm = drm_dev_alloc(&udisp_drm_driver, ud->platform ?
			    &ud->platform->dev : &ud->intf->dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	ud->drm = drm;
	drm->dev_private = ud;

	kref_get(&ud->refcount);
	ret = drmm_add_action_or_reset(drm, udisp_drm_release, ud);
	if (ret)
		goto err_put;

	ret = drmm_mode_config_init(drm);
	if (ret)
		goto err_put;
	drm->mode_config.funcs = &udisp_mode_config_funcs;
	drm->mode_config.min_width = 1;
	drm->mode_config.max_width = 8192;
	drm->mode_config.min_height = 1;
	drm->mode_config.max_height = 8192;
	drm->mode_config.preferred_depth = 24;

	ret = drm_connector_init(drm, &ud->connector, &udisp_connector_funcs,
				 udisp_connector_type());
	if (ret)
		goto err_put;
	drm_connector_helper_add(&ud->connector, &udisp_connector_helper_funcs);
	drm_connector_attach_edid_property(&ud->connector);
	ud->connector.polled = DRM_CONNECTOR_POLL_CONNECT | DRM_CONNECTOR_POLL_DISCONNECT;

	ret = drm_simple_display_pipe_init(drm, &ud->pipe, &udisp_pipe_funcs,
					   udisp_formats, ARRAY_SIZE(udisp_formats),
					   NULL, &ud->connector);
	if (ret)
		goto err_put;

	drm_mode_config_reset(drm);

	transfer_len = sizeof(struct udisp_frame_header) + min_t(size_t, ud->frame_limit, UDISP_MAX_TRANSFER - sizeof(struct udisp_frame_header));
	ud->tx_buf_size = min_t(size_t, transfer_len, UDISP_MAX_TRANSFER);
	ud->jpg_src_size = ud->jpg_width * ud->jpg_height * 3;
	for (i = 0; i < UDISP_RING_SLOTS; i++) {
		ud->slots[i].tx_buf = kmalloc(ud->tx_buf_size, GFP_KERNEL);
		if (!ud->slots[i].tx_buf) {
			ret = -ENOMEM;
			goto err_put;
		}
		ud->slots[i].jpg_src = vmalloc(ud->jpg_src_size);
		if (!ud->slots[i].jpg_src) {
			ret = -ENOMEM;
			goto err_put;
		}
	}
	ud->end_header = kzalloc(sizeof(*ud->end_header), GFP_KERNEL);
	if (!ud->end_header) {
		ret = -ENOMEM;
		goto err_put;
	}
	ud->end_header->type = UDISP_TYPE_END;
	ud->last_tx_buf = kmalloc(ud->tx_buf_size, GFP_KERNEL);
	if (!ud->last_tx_buf) {
		ret = -ENOMEM;
		goto err_put;
	}
	ud->line_buf_size = max_t(size_t, ud->host_width, ud->host_height) * 8 + 4096;
	ud->line_buf = kmalloc(ud->line_buf_size, GFP_KERNEL);
	if (!ud->line_buf) {
		ret = -ENOMEM;
		goto err_put;
	}
	ud->rotate_buf_size = UDISP_ROTATE_TILE_ROWS *
		max_t(size_t, ud->host_width, ud->host_height) * 4;
	ud->rotate_buf = kmalloc(ud->rotate_buf_size, GFP_KERNEL);
	if (!ud->rotate_buf) {
		ret = -ENOMEM;
		goto err_put;
	}

	ret = drm_dev_register(drm, 0);
	if (ret)
		goto err_put;

	drm_fbdev_generic_setup(drm, 32);
	drm_kms_helper_hotplug_event(drm);
	return 0;

err_put:
	drm_dev_put(drm);
	ud->drm = NULL;
	return ret;
}

/* Update the cached connector state before emitting HOTPLUG.  Re-emitting a
 * uevent with an unchanged state is insufficient for some compositors.
 */
static void udisp_notify_hotplug(struct udisp_device *ud)
{
	mutex_lock(&ud->drm->mode_config.mutex);
	ud->connector.funcs->fill_modes(&ud->connector,
		ud->drm->mode_config.max_width, ud->drm->mode_config.max_height);
	mutex_unlock(&ud->drm->mode_config.mutex);
	drm_kms_helper_hotplug_event(ud->drm);
}

/* Let userspace register a newly added secondary GPU while it is disconnected,
 * then publish the connector transition.  Otherwise an early hotplug event can
 * be consumed before the compositor has a KMS resource object for this GPU.
 */
static void udisp_initial_hotplug_work(struct work_struct *work)
{
	struct udisp_device *ud = container_of(to_delayed_work(work),
					      struct udisp_device, hotplug_work);

	mutex_lock(&ud->lock);
	if (!ud->alive) {
		mutex_unlock(&ud->lock);
		return;
	}
	ud->drm_ready = true;
	mutex_unlock(&ud->lock);
	udisp_notify_hotplug(ud);
}

/* The pool owns one kref and one drm_dev reference until module teardown.
 * A USB binding owns a separate kref and may disappear without unplugging KMS.
 */
static struct udisp_device *udisp_find_disconnected(struct udisp_device *info)
{
	struct udisp_device *ud;

	list_for_each_entry(ud, &udisp_devices, node) {
		if (ud->alive || !ud->drm)
			continue;
		if (!strcmp(ud->usb_path, info->usb_path) &&
		    !strcmp(ud->product, info->product) &&
		    !strcmp(ud->serial, info->serial) &&
		    ud->host_width == info->host_width &&
		    ud->host_height == info->host_height &&
		    ud->frame_limit == info->frame_limit)
			return ud;
	}
	return NULL;
}

static void udisp_reattach(struct udisp_device *ud, struct udisp_device *info)
{
	struct usb_device *old_udev = ud->udev;
	struct usb_interface *old_intf = ud->intf;
	int i;

	/* Previous disconnect finished both workers under udisp_devices_lock. */
	mutex_lock(&ud->lock);
	ud->udev = usb_get_dev(info->udev);
	ud->intf = usb_get_intf(info->intf);
	ud->out_ep = info->out_ep;
	ud->out_ep_size = info->out_ep_size;
	ud->fps = info->fps;
	ud->jpg_quality = info->jpg_quality;
	ud->enabled = false;
	ud->has_last_frame = false;
	ud->last_payload_len = 0;
	ud->last_frame_jiffies = 0;
	ud->last_send_start_ns = 0;
	ud->frame_id = 0;
	ud->last_stat_ns = 0;
	ud->last_stat_frames = ud->frames_sent;
	ud->flush_pending = false;
	for (i = 0; i < UDISP_RING_SLOTS; i++) {
		ud->slots[i].busy = false;
		ud->slots[i].ready = false;
	}
	ud->alive = true;
	ud->drm_ready = true;
	mutex_unlock(&ud->lock);

	usb_put_intf(old_intf);
	usb_put_dev(old_udev);
	kref_get(&ud->refcount); /* new USB binding */
	usb_set_intfdata(info->intf, ud);
	udisp_notify_hotplug(ud);
	dev_info(&ud->intf->dev,
		 "USB reconnected: reused existing DRM device and connector\n");
}

static int udisp_usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct usb_host_interface *alt = intf->cur_altsetting;
	struct udisp_device *ud, *existing;
	int i;
	int ret;

	if (alt->desc.bInterfaceNumber != UDISP_VENDOR_INTERFACE)
		return -ENODEV;

	ud = kzalloc(sizeof(*ud), GFP_KERNEL);
	if (!ud)
		return -ENOMEM;
	kref_init(&ud->refcount);
	INIT_LIST_HEAD(&ud->node);
	ud->udev = usb_get_dev(interface_to_usbdev(intf));
	ud->intf = usb_get_intf(intf);
	mutex_init(&ud->lock);
	INIT_DELAYED_WORK(&ud->flush_work, udisp_flush_work);
	INIT_DELAYED_WORK(&ud->hotplug_work, udisp_initial_hotplug_work);
	INIT_WORK(&ud->send_work, udisp_send_work);
	ud->alive = true;
	ud->drm_ready = !persistent_drm;

	for (i = 0; i < alt->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep = &alt->endpoint[i].desc;
		if (usb_endpoint_is_bulk_out(ep)) {
			ud->out_ep = ep->bEndpointAddress;
			ud->out_ep_size = usb_endpoint_maxp(ep);
			break;
		}
	}
	if (!ud->out_ep) {
		ret = -ENODEV;
		goto err_free;
	}
	udisp_parse_usb_strings(ud);
	if (usb_make_path(ud->udev, ud->usb_path, sizeof(ud->usb_path)) < 0)
		strscpy(ud->usb_path, dev_name(&intf->dev), sizeof(ud->usb_path));

	mutex_lock(&udisp_devices_lock);
	if (persistent_drm) {
		existing = udisp_find_disconnected(ud);
		if (existing) {
			udisp_reattach(existing, ud);
			mutex_unlock(&udisp_devices_lock);
			kref_put(&ud->refcount, udisp_device_release);
			return 0;
		}
		/* The parent must outlive the USB interface, otherwise removing USB
		 * would also remove the KMS node from sysfs regardless of our refs.
		 */
		ud->platform = platform_device_register_simple("udisp-display",
					 PLATFORM_DEVID_AUTO, NULL, 0);
		if (IS_ERR(ud->platform)) {
			ret = PTR_ERR(ud->platform);
			ud->platform = NULL;
			goto err_unlock;
		}
		ret = dma_coerce_mask_and_coherent(&ud->platform->dev, DMA_BIT_MASK(64));
		if (ret)
			goto err_unlock;
	}

	ud->wq = alloc_ordered_workqueue("udisp_drm_wq", WQ_UNBOUND | WQ_MEM_RECLAIM);
	if (!ud->wq) {
		ret = -ENOMEM;
		goto err_unlock;
	}
	ud->tx_wq = alloc_ordered_workqueue("udisp_drm_tx_wq", WQ_UNBOUND | WQ_MEM_RECLAIM);
	if (!ud->tx_wq) {
		ret = -ENOMEM;
		goto err_unlock;
	}
	usb_set_intfdata(intf, ud);
	ret = udisp_drm_init(ud);
	if (ret)
		goto err_unlock;
	if (persistent_drm) {
		kref_get(&ud->refcount); /* reconnect pool */
		list_add_tail(&ud->node, &udisp_devices);
		mod_delayed_work(system_wq, &ud->hotplug_work, msecs_to_jiffies(1000));
	}
	mutex_unlock(&udisp_devices_lock);
	dev_info(&intf->dev, "USB DRM udisp connected on ep 0x%02x (persistent=%d)\n",
		 ud->out_ep, persistent_drm);
	return 0;

err_unlock:
	mutex_unlock(&udisp_devices_lock);
err_free:
	usb_set_intfdata(intf, NULL);
	if (ud->wq)
		destroy_workqueue(ud->wq);
	if (ud->tx_wq)
		destroy_workqueue(ud->tx_wq);
	kref_put(&ud->refcount, udisp_device_release);
	return ret;
}

static void udisp_usb_disconnect(struct usb_interface *intf)
{
	struct udisp_device *ud = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (!ud)
		return;
	mutex_lock(&udisp_devices_lock);
	mutex_lock(&ud->lock);
	ud->alive = false;
	ud->drm_ready = false;
	ud->enabled = false;
	mutex_unlock(&ud->lock);

	cancel_delayed_work_sync(&ud->hotplug_work);
	cancel_delayed_work_sync(&ud->flush_work);
	cancel_work_sync(&ud->send_work);
	mutex_lock(&ud->lock);
	udisp_replace_fb_locked(&ud->pending_fb, NULL);
	udisp_replace_fb_locked(&ud->current_fb, NULL);
	ud->has_last_frame = false;
	mutex_unlock(&ud->lock);

	if (ud->drm) {
		if (persistent_drm) {
			drm_atomic_helper_shutdown(ud->drm);
			udisp_notify_hotplug(ud);
			dev_info(&intf->dev, "USB disconnected: DRM node retained for reconnect\n");
		} else {
			drm_dev_unplug(ud->drm);
			drm_atomic_helper_shutdown(ud->drm);
			drm_dev_put(ud->drm);
		}
	}
	mutex_unlock(&udisp_devices_lock);
	kref_put(&ud->refcount, udisp_device_release); /* old USB binding */
}

static const struct usb_device_id udisp_usb_ids[] = {
	{ USB_DEVICE_INTERFACE_NUMBER(UDISP_USB_VID, UDISP_USB_PID, UDISP_VENDOR_INTERFACE) },
	{ USB_DEVICE_INTERFACE_NUMBER(UDISP_USB_VID, UDISP_USB_PID_DISPLAY, UDISP_VENDOR_INTERFACE) },
	{ }
};
MODULE_DEVICE_TABLE(usb, udisp_usb_ids);

static struct usb_driver udisp_usb_driver = {
	.name = KBUILD_MODNAME,
	.probe = udisp_usb_probe,
	.disconnect = udisp_usb_disconnect,
	.id_table = udisp_usb_ids,
};

static int __init udisp_init(void)
{
	return usb_register(&udisp_usb_driver);
}

static void __exit udisp_exit(void)
{
	struct udisp_device *ud, *next;

	usb_deregister(&udisp_usb_driver);
	mutex_lock(&udisp_devices_lock);
	list_for_each_entry_safe(ud, next, &udisp_devices, node) {
		list_del_init(&ud->node);
		drm_dev_unplug(ud->drm);
		drm_atomic_helper_shutdown(ud->drm);
		drm_dev_put(ud->drm); /* pool's DRM device reference */
		kref_put(&ud->refcount, udisp_device_release); /* pool's kref */
	}
	mutex_unlock(&udisp_devices_lock);
}
module_init(udisp_init);
module_exit(udisp_exit);

MODULE_AUTHOR("xfz1986/Codex");
MODULE_DESCRIPTION("USB DRM/KMS driver for xfz1986 udisp devices");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1.5");

#ifndef UDISP_DRM_LOG_H
#define UDISP_DRM_LOG_H

#include <linux/printk.h>

#define LOG(fmt, ...) pr_debug("udisp_drm: " fmt, ##__VA_ARGS__)
#define LOGD(fmt, ...) pr_debug("udisp_drm: " fmt, ##__VA_ARGS__)
#define LOGI(fmt, ...) pr_info("udisp_drm: " fmt, ##__VA_ARGS__)
#define LOGW(fmt, ...) pr_warn("udisp_drm: " fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) pr_err("udisp_drm: " fmt, ##__VA_ARGS__)

#endif

#ifndef SCREEN_DRIVER_H
#define SCREEN_DRIVER_H

#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

/*
 * 4.2" 300x400 六色墨水屏（E Ink Spectra 6 类显示膜，驱动 IC：SSD2693 系列）
 *
 * 逻辑显示方向 = 竖屏 300(宽) x 400(高)，与网页画布 / 传图格式一致。
 * 面板原生方向   = 400 source x 300 gate（横屏），屏幕驱动内部负责旋转，
 *                  见 SCREEN_PANEL_ROTATION。
 *
 * 传图格式：4bpp 六色，每字节 2 个横向像素（高半字节 = 左侧 / 偶数 x 像素），
 *          行优先，整帧 SCREEN_WIDTH * SCREEN_HEIGHT / 2 = 60000 字节。
 */
#define SCREEN_WIDTH        300
#define SCREEN_HEIGHT       400
#define SCREEN_FRAME_BYTES  (SCREEN_WIDTH * SCREEN_HEIGHT / 2) /* 60000 */

/* 面板原生分辨率（SSD2693：400 Source x 300 Gate，见 docs/ 调研） */
#define PANEL_NATIVE_WIDTH  400
#define PANEL_NATIVE_HEIGHT 300

/*
 * 面板在设备中的安装方向：把逻辑竖屏画面旋转多少度后，落到面板原生横屏坐标。
 * 实测图像方向不对时只改这里：90 -> 270 是上下颠倒，0/180 仅在横屏安装时使用。
 */
#define SCREEN_PANEL_ROTATION 90

/* 若面板排线方向导致画面左右/上下镜像，打开对应的镜像开关（只影响显示映射） */
#define SCREEN_PANEL_MIRROR_X 0
#define SCREEN_PANEL_MIRROR_Y 0

/* 6 色 E6 颜色码（E Ink Spectra 6 / Waveshare 1.54" E6 官方定义一致） */
typedef enum {
    SCREEN_COLOR_BLACK  = 0x0,
    SCREEN_COLOR_WHITE  = 0x1,
    SCREEN_COLOR_YELLOW = 0x2,
    SCREEN_COLOR_RED    = 0x3,
    SCREEN_COLOR_BLUE   = 0x5,
    SCREEN_COLOR_GREEN  = 0x6,
} screen_color_t;

esp_err_t screen_driver_init(void);
esp_err_t screen_driver_fill(screen_color_t color);
/* 推送逻辑显示方向的帧：行优先，每字节 2 个横向像素（高半字节=偶 x） */
esp_err_t screen_driver_show_frame(const uint8_t *frame, size_t len);
esp_err_t screen_driver_deep_sleep(void);

#endif /* SCREEN_DRIVER_H */

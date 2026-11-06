#include "screen_driver.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "pin_map.h"

static const char *TAG = "screen";

/*
 * 4.2" 300x400 六色墨水屏驱动（驱动 IC：SSD2693 系列）
 *
 * 移植来源：
 *  - 传图/显示流程沿用本工程 1.54" 六色 E6 驱动（Waveshare 1.54" E-Panel (E) 官方 demo 同款）：
 *        Reset -> vendor 波形选择 0xE9/0x01 -> 0x10 DTM 写整帧 -> 0x04 PON -> 0x12/0x00 DRF
 *        -> 0x02/0x00 POF -> 0x07/0xA5 DSLP
 *    这些命令/时序与 SSD2683 规格书第 9 章的上电/刷新/关屏序列一致（2683 为 2bpp 4 色，
 *    仅 DTM 数据量和分辨率不同）。
 *  - 4bpp 数据格式：每字节 2 像素、高半字节 = 左侧/偶数 x 像素，颜色码 0/1/2/3/5/6
 *    （E Ink Spectra 6 / Waveshare E6 官方 GUI_Paint.cpp 的打包方式）。
 *  - 分辨率/方向：SSD2693 原生 400 Source x 300 Gate（横屏），产品为 300x400 竖屏，
 *    故驱动内部按 SCREEN_PANEL_ROTATION 旋转后再发送。
 *
 * SSD2693 完整数据手册暂未拿到（docs 内是 SSD2683 参考手册），以下三处是最可能需要按
 * 实际模组调整的地方，均已集中在这份文件里，便于后续替换：
 *  1) SCREEN_INIT_PROFILE / K_PANEL_INIT：面板专有初始化命令（默认仅 E9 01，与 1.54" E6 一致）；
 *  2) SCREEN_PANEL_ROTATION / MIRROR：画面方向；
 *  3) EINK_BUSY_READY_LEVEL：BUSY 极性（当前按手册/官方 demo 的低=忙）。
 */

/* SPI 与面板时序参数 */
#define EINK_SPI_HOST         SPI2_HOST
#define EINK_SPI_CLOCK_HZ     (4 * 1000 * 1000) /* SSD2683 手册允许 20MHz；4MHz 留足余量 */
#define EINK_CHUNK_BYTES      4096
#define EINK_BUSY_TIMEOUT_MS  120000 /* 六色全屏刷新约 20-40s，留足超时 */

/* BUSY_N：低 = 忙，高 = 空闲（SSD2683 手册 Table 5-2 / Waveshare E6 demo 一致）。
 * 若实际模组极性相反，把 ready 电平改成 0。 */
#define EINK_BUSY_READY_LEVEL 1

/* 刷新结束是否进入深睡（1uA）。每次刷新前都会硬复位+重新 init，因此可以放心睡。 */
#define EINK_SLEEP_AFTER_REFRESH 1

/*
 * 面板初始化命令表。
 * SSD2683 参考手册里厂商示例在复位后固定发 0xE9/0x01（未公开 vendor 寄存器，推测为
 * OTP/LUT 波形选择），旧 1.54" 六色 E6 驱动同样如此；SSD2693 手册暂未拿到，因此提供
 * 三套 Profile，上机点不亮/颜色不对时改 SCREEN_INIT_PROFILE 逐个试，找到可用序列后删掉
 * 其余 Profile 即可。拿到 SSD2693 手册/模组厂 demo 后，把正确命令按 {cmd,len,data,delay}
 * 追加进 k_panel_init 即可，不需要动其它代码。
 *
 *  0 = 最小 init：仅 vendor 波形选择 0xE9/0x01（Waveshare 1.54" E6 官方 demo / 2683 demo 同款，默认）
 *  1 = 旧 1.54" 六色模组专有 init：FF A5 / AE 10 / 31 1C / FF E3 / E9 01（本工程换屏前的序列）
 *  2 = SSD2683 家族 POR 默认寄存器表（400x300，仅当确认 2693 寄存器兼容时使用）
 */
#ifndef SCREEN_INIT_PROFILE
#define SCREEN_INIT_PROFILE 0
#endif

typedef struct {
    uint8_t cmd;
    uint8_t len; /* 参数字节数，0 = 纯命令 */
    uint8_t data[8];
    uint16_t delay_ms;
} eink_init_cmd_t;

#if (SCREEN_INIT_PROFILE == 0)
static const eink_init_cmd_t k_init_minimal[] = {
    {0xE9, 1, {0x01}, 0}, /* vendor: 选择 OTP/LUT 波形 */
};
#define K_PANEL_INIT k_init_minimal
#elif (SCREEN_INIT_PROFILE == 1)
static const eink_init_cmd_t k_init_old_1in54_e6[] = {
    {0xFF, 1, {0xA5}, 0},
    {0xAE, 1, {0x10}, 0},
    {0x31, 1, {0x1C}, 0},
    {0xFF, 1, {0xE3}, 0},
    {0xE9, 1, {0x01}, 0},
};
#define K_PANEL_INIT k_init_old_1in54_e6
#elif (SCREEN_INIT_PROFILE == 2)
static const eink_init_cmd_t k_init_family_2683[] = {
    {0xE9, 1, {0x01}, 0},                                /* vendor 波形选择 */
    {0x00, 2, {0x2F, 0x09}, 0},                          /* PSR: 400x300, UD=1, SHL=1 */
    {0x01, 6, {0x07, 0xF0, 0x00, 0x00, 0x00, 0x00}, 0},  /* PWR: 内部 DCDC 默认电压 */
    {0x06, 4, {0x0F, 0x8B, 0x9C, 0x96}, 0},              /* BTST: booster soft start */
    {0x03, 1, {0x00}, 0},                                /* POFS: 默认掉电序列 */
    {0x50, 1, {0x97}, 0},                                /* CDI: VCOM/data interval 默认 */
    {0x30, 1, {0x02}, 0},                                /* PLL: 默认帧率 */
    {0x61, 4, {0x01, 0x90, 0x01, 0x2C}, 0},              /* TRES: 400x300 */
    {0x65, 4, {0x00, 0x00, 0x00, 0x00}, 0},              /* GSST: S/G start = 0 */
};
#define K_PANEL_INIT k_init_family_2683
#else
#error "SCREEN_INIT_PROFILE 只能是 0 / 1 / 2"
#endif

typedef struct {
    spi_device_handle_t spi;
    SemaphoreHandle_t lock;
    bool inited;
} screen_ctx_t;

static screen_ctx_t s_ctx;

/* 面板原生行缓冲：400 像素 / 2 = 200 字节；静态 DRAM + 4 字节对齐，可被 SPI DMA 直接发送 */
static uint8_t s_line_buf[PANEL_NATIVE_WIDTH / 2] __attribute__((aligned(4)));

static void delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static esp_err_t wait_busy(void)
{
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(EINK_BUSY_TIMEOUT_MS);
    while (gpio_get_level(eink_busy) != EINK_BUSY_READY_LEVEL) {
        if (xTaskGetTickCount() > deadline) {
            ESP_LOGE(TAG, "BUSY timeout");
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    delay_ms(10);
    return ESP_OK;
}

static void lcd_spi_pre_transfer_callback(spi_transaction_t *t)
{
    int dc = (int)t->user;
    gpio_set_level(eink_dc, dc);
}

static esp_err_t write_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length = 8,
        .tx_buffer = &cmd,
        .user = (void *)0,
    };
    return spi_device_polling_transmit(s_ctx.spi, &t);
}

static esp_err_t write_data(const uint8_t *data, size_t len)
{
    while (len > 0) {
        size_t chunk = len > EINK_CHUNK_BYTES ? EINK_CHUNK_BYTES : len;
        spi_transaction_t t = {
            .length = chunk * 8,
            .tx_buffer = data,
            .user = (void *)1,
        };
        esp_err_t err = spi_device_polling_transmit(s_ctx.spi, &t);
        if (err != ESP_OK) {
            return err;
        }
        data += chunk;
        len -= chunk;
    }
    return ESP_OK;
}

static esp_err_t write_data_byte(uint8_t data)
{
    return write_data(&data, 1);
}

static esp_err_t hw_reset(void)
{
    /* 与 Waveshare E6 demo / SSD2683 手册 §9.1 一致：低 20ms -> 高 20ms -> 等 BUSY 就绪 */
    gpio_set_level(eink_rst, 0);
    delay_ms(20);
    gpio_set_level(eink_rst, 1);
    delay_ms(20);
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "reset busy");
    delay_ms(10);
    return ESP_OK;
}

static esp_err_t init_panel(void)
{
    for (size_t i = 0; i < sizeof(K_PANEL_INIT) / sizeof(K_PANEL_INIT[0]); i++) {
        const eink_init_cmd_t *c = &K_PANEL_INIT[i];
        ESP_RETURN_ON_ERROR(write_cmd(c->cmd), TAG, "init cmd 0x%02X", c->cmd);
        if (c->len > 0) {
            ESP_RETURN_ON_ERROR(write_data(c->data, c->len), TAG, "init data 0x%02X", c->cmd);
        }
        if (c->delay_ms > 0) {
            delay_ms(c->delay_ms);
        }
    }
    return ESP_OK;
}

/* ---------- 逻辑竖屏帧 -> 面板原生横屏行的旋转映射 ---------- */

#if (SCREEN_PANEL_ROTATION != 0) && (SCREEN_PANEL_ROTATION != 90) && \
    (SCREEN_PANEL_ROTATION != 180) && (SCREEN_PANEL_ROTATION != 270)
#error "SCREEN_PANEL_ROTATION 只能是 0 / 90 / 180 / 270"
#endif

#if (SCREEN_PANEL_ROTATION == 90) || (SCREEN_PANEL_ROTATION == 270)
#if (PANEL_NATIVE_WIDTH != SCREEN_HEIGHT) || (PANEL_NATIVE_HEIGHT != SCREEN_WIDTH)
#error "90/270 旋转要求 面板宽高 = 逻辑高宽（400x300 <-> 300x400）"
#endif
#else
#if (PANEL_NATIVE_WIDTH != SCREEN_WIDTH) || (PANEL_NATIVE_HEIGHT != SCREEN_HEIGHT)
#error "0/180 旋转要求 面板分辨率 = 逻辑分辨率"
#endif
#endif

/* 读逻辑帧中的一个像素（4bpp，高半字节 = 偶数 x） */
static inline uint8_t frame_pixel(const uint8_t *frame, int x, int y)
{
    const uint8_t b = frame[(size_t)y * (SCREEN_WIDTH / 2) + (x >> 1)];
    return (x & 1) ? (uint8_t)(b & 0x0F) : (uint8_t)(b >> 4);
}

/* 面板原生坐标 (px,py) 对应的逻辑画面坐标 (lx,ly) */
static inline void panel_to_frame_xy(int px, int py, int *lx, int *ly)
{
#if (SCREEN_PANEL_ROTATION == 90)
    /* 逻辑画面顺时针转 90° = 面板原生画面 */
    *lx = py;
    *ly = SCREEN_HEIGHT - 1 - px;
#elif (SCREEN_PANEL_ROTATION == 180)
    *lx = PANEL_NATIVE_WIDTH - 1 - px;
    *ly = PANEL_NATIVE_HEIGHT - 1 - py;
#elif (SCREEN_PANEL_ROTATION == 270)
    /* 逻辑画面逆时针转 90° = 面板原生画面 */
    *lx = SCREEN_WIDTH - 1 - py;
    *ly = px;
#else
    *lx = px;
    *ly = py;
#endif
#if SCREEN_PANEL_MIRROR_X
    *lx = SCREEN_WIDTH - 1 - *lx;
#endif
#if SCREEN_PANEL_MIRROR_Y
    *ly = SCREEN_HEIGHT - 1 - *ly;
#endif
}

/* 生成面板第 py 行（各像素映射到逻辑帧后按 4bpp 打包） */
static void build_panel_line(const uint8_t *frame, int py, uint8_t *line)
{
    for (int px = 0; px < PANEL_NATIVE_WIDTH; px += 2) {
        int lx0, ly0, lx1, ly1;
        panel_to_frame_xy(px, py, &lx0, &ly0);
        panel_to_frame_xy(px + 1, py, &lx1, &ly1);
        const uint8_t hi = frame_pixel(frame, lx0, ly0);
        const uint8_t lo = frame_pixel(frame, lx1, ly1);
        line[px / 2] = (uint8_t)((hi << 4) | lo);
    }
}

/* ---------- 刷新一帧：复位 -> init -> DTM -> PON -> DRF -> POF -> (DSLP) ---------- */

static esp_err_t panel_refresh_buffer(const uint8_t *frame)
{
    ESP_RETURN_ON_ERROR(hw_reset(), TAG, "reset");
    ESP_RETURN_ON_ERROR(init_panel(), TAG, "init");

    ESP_RETURN_ON_ERROR(write_cmd(0x10), TAG, "DTM");
    for (int py = 0; py < PANEL_NATIVE_HEIGHT; py++) {
        build_panel_line(frame, py, s_line_buf);
        ESP_RETURN_ON_ERROR(write_data(s_line_buf, sizeof(s_line_buf)), TAG, "line %d", py);
    }

    ESP_RETURN_ON_ERROR(write_cmd(0x04), TAG, "PON"); /* 打开面板电源 */
    delay_ms(10);
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "PON busy");
    delay_ms(10);

    ESP_RETURN_ON_ERROR(write_cmd(0x12), TAG, "DRF"); /* 全屏刷新（10-40s） */
    ESP_RETURN_ON_ERROR(write_data_byte(0x00), TAG, "DRF data");
    delay_ms(10);
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "DRF busy");

    ESP_RETURN_ON_ERROR(write_cmd(0x02), TAG, "POF"); /* 关闭面板电源 */
    ESP_RETURN_ON_ERROR(write_data_byte(0x00), TAG, "POF data");
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "POF busy");
    delay_ms(20);

#if EINK_SLEEP_AFTER_REFRESH
    ESP_RETURN_ON_ERROR(write_cmd(0x07), TAG, "DSLP");
    ESP_RETURN_ON_ERROR(write_data_byte(0xA5), TAG, "DSLP data");
#endif
    return ESP_OK;
}

esp_err_t screen_driver_init(void)
{
    if (s_ctx.inited) {
        return ESP_OK;
    }

    s_ctx.lock = xSemaphoreCreateMutex();
    if (s_ctx.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io_out = {
        .pin_bit_mask = (1ULL << eink_dc) | (1ULL << eink_rst),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_out), TAG, "gpio out");

    gpio_config_t io_in = {
        .pin_bit_mask = (1ULL << eink_busy),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_in), TAG, "gpio busy");

    gpio_set_level(eink_rst, 1);
    gpio_set_level(eink_dc, 1);

    spi_bus_config_t buscfg = {
        .mosi_io_num = eink_sda,
        .miso_io_num = -1,
        .sclk_io_num = eink_clk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = EINK_CHUNK_BYTES,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(EINK_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "spi bus");

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = EINK_SPI_CLOCK_HZ,
        .mode = 0, /* CPOL=0, CPHA=0：SCL 空闲低，上升沿采样 */
        .spics_io_num = eink_cs,
        .queue_size = 4,
        .pre_cb = lcd_spi_pre_transfer_callback,
        .flags = SPI_DEVICE_NO_DUMMY,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(EINK_SPI_HOST, &devcfg, &s_ctx.spi),
                        TAG, "spi device");

    ESP_RETURN_ON_ERROR(hw_reset(), TAG, "reset");
    ESP_RETURN_ON_ERROR(init_panel(), TAG, "init");

    s_ctx.inited = true;
    ESP_LOGI(TAG, "SSD2693 ready: logical %dx%d, panel %dx%d, rotate %d, frame %u bytes",
             SCREEN_WIDTH, SCREEN_HEIGHT, PANEL_NATIVE_WIDTH, PANEL_NATIVE_HEIGHT,
             SCREEN_PANEL_ROTATION, (unsigned)SCREEN_FRAME_BYTES);
    return ESP_OK;
}

esp_err_t screen_driver_fill(screen_color_t color)
{
    if (!s_ctx.inited) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t *frame = heap_caps_malloc(SCREEN_FRAME_BYTES, MALLOC_CAP_8BIT);
    if (frame == NULL) {
        frame = malloc(SCREEN_FRAME_BYTES);
    }
    if (frame == NULL) {
        return ESP_ERR_NO_MEM;
    }

    const uint8_t pixel_byte = (uint8_t)((color << 4) | color);
    memset(frame, pixel_byte, SCREEN_FRAME_BYTES);

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    esp_err_t err = panel_refresh_buffer(frame);
    xSemaphoreGive(s_ctx.lock);

    free(frame);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "fill color=0x%X done", (unsigned)color);
    }
    return err;
}

esp_err_t screen_driver_show_frame(const uint8_t *frame, size_t len)
{
    if (!s_ctx.inited || frame == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (len != SCREEN_FRAME_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }

    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    esp_err_t err = panel_refresh_buffer(frame);
    xSemaphoreGive(s_ctx.lock);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "frame flushed");
    }
    return err;
}

esp_err_t screen_driver_deep_sleep(void)
{
    if (!s_ctx.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctx.lock, portMAX_DELAY);
    esp_err_t err = write_cmd(0x07);
    if (err == ESP_OK) {
        err = write_data_byte(0xA5);
    }
    xSemaphoreGive(s_ctx.lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "deep sleep");
    }
    return err;
}

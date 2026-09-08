/*
 * AXS15231B QSPI panel + capacitive touch + LVGL v8, for the digital
 * Morse Code Trainer.
 *
 * The board is the **V2** hardware revision of the Waveshare
 * ESP32-S3-Touch-LCD-3.49. Everything from the top of this file down to
 * build_scene() -- the IO-expander/reset/backlight sequence, the vendor
 * init command list, the hand-rolled chunked LVGL flush, the touch-axis
 * mapping -- is copied from the sibling morse_code_listener project's
 * components/lcd_display/lcd_display.c, where it was verified against real
 * V2 hardware across the whole "everything reports success, screen stays
 * dark" saga. Do not "fix" any of it here without checking there first.
 *
 * What differs from that file: the scene (a trainer UI instead of a
 * scrolling text log) and the public API.
 */

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_axs15231b.h"
#include "esp_lvgl_port.h"
#include "lcd_ui.h"

static const char *TAG = "lcd_ui";

/* --- Waveshare ESP32-S3-Touch-LCD-3.49 V2 pinout (from Waveshare's own
 * ESP-IDF demo firmware; the product docs have no pinout table). --- */
#define LCD_HOST       SPI3_HOST
#define LCD_PIN_CS     9
#define LCD_PIN_PCLK   10
#define LCD_PIN_DATA0  11
#define LCD_PIN_DATA1  12
#define LCD_PIN_DATA2  13
#define LCD_PIN_DATA3  14

/* Shared "ESP I2C" bus -- carries the TCA9554 expander (LCD reset +
 * backlight enable live behind it on V2). */
#define SHARED_I2C_PORT I2C_NUM_0
#define SHARED_I2C_SDA  47
#define SHARED_I2C_SCL  48

#define TCA9554_ADDR      ESP_IO_EXPANDER_I2C_TCA9554_ADDRESS_000
#define EXIO_PIN_TOUCH_INT (1ULL << 0)
#define EXIO_PIN_BL_EN     (1ULL << 1)
#define EXIO_PIN_LCD_RST   (1ULL << 5)
/* Bit 7 is the onboard speaker-amplifier enable (board_cfg.txt lists the
 * codec's own "pa" pin as -1). Waveshare's 08_Audio_Test drives EXIO7 high
 * in tca9554_init() before bringing the ES8311 up; without it the DAC
 * clocks out silently into a disabled amp. Enabled here because this file
 * is the sole owner of the TCA9554 -- the sidetone component can't safely
 * open a second handle to the same chip. Harmless when audio is unused. */
#define EXIO_PIN_AUDIO_PA  (1ULL << 7)

/* The AXS15231B touch interface is on its OWN I2C bus, not the shared one. */
#define TOUCH_I2C_PORT I2C_NUM_1
#define TOUCH_I2C_SDA  17
#define TOUCH_I2C_SCL  18

/* Native panel geometry -- portrait. See the long comment in the sibling
 * project's lcd_display.c for why this is 172x640 and not swapped. */
#define LCD_H_RES 172
#define LCD_V_RES 640
#define LCD_DRAW_BUF_LINES 80

/* --- Scene layout (portrait 172 x 640) --- */
#define MODEBAR_Y     0
#define MODEBAR_H     30
#define SYMBOL_Y      36
#define SYMBOL_H      52
#define LETTER_Y      92
#define LETTER_H      74
#define OUTPUT_Y      170
#define OUTPUT_H      42
#define DIVIDER_Y     216
#define CAND_Y        222
#define CAND_H        348
#define KEYZONE_H     64
#define KEYZONE_Y     (LCD_V_RES - KEYZONE_H)

/* Practice widgets share the vertical band the candidate list uses in
 * Training mode. */
#define LENROW_Y      222
#define LENROW_H      36
#define PCAP_Y        264
#define PCAP_H        20
#define PTARGET_Y     288
#define PTARGET_H     150
#define PSTATUS_Y     446
#define PSTATUS_H     60

/* Drawn dot/dash shapes in the symbol strip. A dash is DASH_LEN long and
 * SYMBOL_THK thick; a dot is a SYMBOL_THK circle. Sized so up to 4 symbols
 * sit on one row at 172 px wide; longer sequences wrap to a second row. */
#define SYMBOL_THK    20
#define DASH_LEN      34
#define SYMBOL_GAP    8

#define OUTPUT_MAX_CHARS 512
#define OUTPUT_TRIM_CHARS 128
#define LETTER_FLASH_MS  260

/* --- HAKISTA brand theme --------------------------------------------------
 * Straight off the branding sheet:
 *   cyan    #07DBF9   primary accent   (hoodie / frame / outline)
 *   white   #FFFFFF   primary text
 *   grey    #BBB6C2   secondary / muted text
 *   red     #FF4122   alert / wrong
 *   navy    #0F1E35 -> black #000000    background gradient
 *   violet  #5511C7 -> magenta #E531D6  secondary accent gradient
 * The brand face is Russo One; only Montserrat is compiled into this
 * panel build, so the type is unchanged for now (see README). */
#define BRAND_CYAN      lv_color_hex(0x07DBF9)
#define BRAND_WHITE     lv_color_hex(0xFFFFFF)
#define BRAND_GREY      lv_color_hex(0xBBB6C2)
#define BRAND_RED       lv_color_hex(0xFF4122)
#define BRAND_NAVY      lv_color_hex(0x0F1E35)
#define BRAND_BLACK     lv_color_hex(0x000000)
#define BRAND_VIOLET    lv_color_hex(0x5511C7)
#define BRAND_MAGENTA   lv_color_hex(0xE531D6)

/* Derived shades -- brand hues nudged in lightness for fills / pressed. */
#define BRAND_NAVY_HI   lv_color_hex(0x1B3357)  /* lifted navy -- pressed panel */
#define BRAND_TEAL_LO   lv_color_hex(0x0E3A44)  /* deep teal -- key zone at rest */
#define BRAND_TEAL_MID  lv_color_hex(0x0B7C8C)  /* mid teal  -- key zone pressed */
#define BRAND_DIVIDER   lv_color_hex(0x123A47)  /* faint cyan rule */

/* Role mapping. */
#define COLOR_SYMBOL             BRAND_CYAN
#define COLOR_LETTER             BRAND_WHITE
#define COLOR_FLASH              BRAND_MAGENTA
#define COLOR_OUTPUT             BRAND_GREY
#define COLOR_OUTPUT_BG          BRAND_NAVY
#define COLOR_OUTPUT_BG_PRESSED  BRAND_NAVY_HI
#define COLOR_CAND               BRAND_CYAN
#define COLOR_KEYZONE            BRAND_TEAL_LO
#define COLOR_KEYZONE_PRESSED    BRAND_TEAL_MID
#define COLOR_MODEBAR            BRAND_VIOLET
#define COLOR_MODEBAR_PRESSED    BRAND_MAGENTA
#define COLOR_LENBTN             BRAND_NAVY
#define COLOR_LENBTN_SEL         BRAND_CYAN
#define COLOR_DIVIDER            BRAND_DIVIDER
#define COLOR_SCREEN_TOP         BRAND_NAVY
#define COLOR_SCREEN_BOTTOM      BRAND_BLACK

/* Practice per-glyph recolour tags (LVGL "#rrggbb x#" markup). */
#define PMARK_PENDING "BBB6C2"
#define PMARK_OK      "07DBF9"
#define PMARK_WRONG   "FF4122"

static lv_disp_t *s_disp;
static lv_obj_t *s_symbol_row;   /* flex container; children are the drawn dot/dash shapes */
static lv_obj_t *s_letter_label;
static lv_obj_t *s_output_label;
static lv_obj_t *s_cand_label;
static lv_obj_t *s_key_zone;
static lv_obj_t *s_divider;
static lv_obj_t *s_modebar;       /* full-width MODE button at the top */
static lv_obj_t *s_modebar_label;
static lv_obj_t *s_len_row;       /* Practice: flex row of the six length buttons */
static lv_obj_t *s_len_btn[6];
static lv_obj_t *s_pcap_label;    /* Practice: "TARGET" caption */
static lv_obj_t *s_ptarget_label; /* Practice: the word, per-glyph recoloured */
static lv_obj_t *s_pstatus_label; /* Practice: status / score line */
static size_t s_output_len;
static lv_timer_t *s_flash_timer;
static volatile bool s_key_down;
static lcd_ui_cfg_t s_cfg;

/* --- Deferred UI state -------------------------------------------------
 * The keyer runs on its own task. If it took LVGL's lock directly to
 * paint every symbol, it would race the touch handler for that lock and,
 * mid-tap (when LVGL is busy repainting the pressed key zone), time out
 * and silently drop the update -- which is exactly why on-screen keying
 * used to leave the dot/dash strip stale while the BOOT button did not.
 *
 * Instead the setters below just stash the wanted state under a tiny
 * mutex (held only for a memcpy) and s_ui_timer, a repeating lv_timer,
 * applies it from the LVGL task itself. No cross-task LVGL lock, nothing
 * to drop. */
static SemaphoreHandle_t s_pending_mux;
static lv_timer_t *s_ui_timer;
static struct {
    char symbols[24];
    char cands[544];
    char output[64];   /* chars still to be appended */
    char letter;        /* 0 = blank */
    bool flash;         /* flash the letter green on apply */
    bool symbols_dirty;
    bool cands_dirty;
    bool letter_dirty;

    /* Mode / Practice state (all lower frequency than the keyer strip). */
    int  mode;             /* lcd_ui_mode_t */
    bool mode_dirty;
    int  length;           /* selected length 1..6, 0 = none */
    bool length_dirty;
    char ptarget[8];       /* the target word */
    char pmark[8];         /* per glyph: ' ' pending, 'o' correct, 'x' wrong */
    bool practice_dirty;   /* ptarget and/or pmark changed -- repaint the word */
    char pstatus[32];
    bool pstatus_dirty;
} s_pending;

static esp_lcd_touch_handle_t s_touch_handle; /* NULL if init_touch() didn't fully succeed */
static esp_io_expander_handle_t s_io_expander;

/* Hand-rolled LVGL display driver state (see init_lvgl()). */
static lv_disp_draw_buf_t s_draw_buf_dsc;
static lv_disp_drv_t s_disp_drv;
static uint16_t *s_lvgl_render_buf;   /* PSRAM, CPU-only */
static uint16_t *s_lvgl_staging_buf;  /* small internal-RAM DMA buffer */
static SemaphoreHandle_t s_flush_sem;

/* ---------------------------------------------------------------------- */
/* Panel bring-up -- copied from morse_code_listener/.../lcd_display.c     */
/* ---------------------------------------------------------------------- */

static esp_err_t init_io_expander(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = SHARED_I2C_PORT,
        .scl_io_num = SHARED_I2C_SCL,
        .sda_io_num = SHARED_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_io_expander_new_i2c_tca9554(bus_handle, TCA9554_ADDR, &s_io_expander);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_io_expander_new_i2c_tca9554 failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_ERROR_CHECK(esp_io_expander_set_dir(s_io_expander, EXIO_PIN_TOUCH_INT, IO_EXPANDER_INPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_dir(s_io_expander, EXIO_PIN_BL_EN | EXIO_PIN_LCD_RST | EXIO_PIN_AUDIO_PA, IO_EXPANDER_OUTPUT));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, EXIO_PIN_BL_EN, 0));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, EXIO_PIN_LCD_RST, 1));
    ESP_ERROR_CHECK(esp_io_expander_set_level(s_io_expander, EXIO_PIN_AUDIO_PA, 1)); /* enable the speaker amp */
    return ESP_OK;
}

static void expander_lcd_reset(void)
{
    esp_io_expander_set_level(s_io_expander, EXIO_PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    esp_io_expander_set_level(s_io_expander, EXIO_PIN_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_io_expander_set_level(s_io_expander, EXIO_PIN_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
}

static void expander_backlight_set(bool enable)
{
    esp_io_expander_set_level(s_io_expander, EXIO_PIN_BL_EN, enable ? 1 : 0);
}

/* esp_lcd_axs15231b's default init sequence ends by blanking the panel;
 * this alternate sequence (from the component's own test app) is the same
 * vendor register writes minus that trailing blank-out. Opaque values --
 * copied verbatim, not hand-derived. */
static const axs15231b_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0xBB, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5A, 0xA5}, 8, 0},
    {0xA0, (uint8_t []){0xC0, 0x10, 0x00, 0x02, 0x00, 0x00, 0x04, 0x3F, 0x20, 0x05, 0x3F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00}, 17, 0},
    {0xA2, (uint8_t []){0x30, 0x3C, 0x24, 0x14, 0xD0, 0x20, 0xFF, 0xE0, 0x40, 0x19, 0x80, 0x80, 0x80, 0x20, 0xf9, 0x10, 0x02, 0xff, 0xff, 0xF0, 0x90, 0x01, 0x32, 0xA0, 0x91, 0xE0, 0x20, 0x7F, 0xFF, 0x00, 0x5A}, 31, 0},
    {0xD0, (uint8_t []){0xE0, 0x40, 0x51, 0x24, 0x08, 0x05, 0x10, 0x01, 0x20, 0x15, 0x42, 0xC2, 0x22, 0x22, 0xAA, 0x03, 0x10, 0x12, 0x60, 0x14, 0x1E, 0x51, 0x15, 0x00, 0x8A, 0x20, 0x00, 0x03, 0x3A, 0x12}, 30, 0},
    {0xA3, (uint8_t []){0xA0, 0x06, 0xAa, 0x00, 0x08, 0x02, 0x0A, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x55, 0x55}, 22, 0},
    {0xC1, (uint8_t []){0x31, 0x04, 0x02, 0x02, 0x71, 0x05, 0x24, 0x55, 0x02, 0x00, 0x41, 0x00, 0x53, 0xFF, 0xFF, 0xFF, 0x4F, 0x52, 0x00, 0x4F, 0x52, 0x00, 0x45, 0x3B, 0x0B, 0x02, 0x0d, 0x00, 0xFF, 0x40}, 30, 0},
    {0xC3, (uint8_t []){0x00, 0x00, 0x00, 0x50, 0x03, 0x00, 0x00, 0x00, 0x01, 0x80, 0x01}, 11, 0},
    {0xC4, (uint8_t []){0x00, 0x24, 0x33, 0x80, 0x00, 0xea, 0x64, 0x32, 0xC8, 0x64, 0xC8, 0x32, 0x90, 0x90, 0x11, 0x06, 0xDC, 0xFA, 0x00, 0x00, 0x80, 0xFE, 0x10, 0x10, 0x00, 0x0A, 0x0A, 0x44, 0x50}, 29, 0},
    {0xC5, (uint8_t []){0x18, 0x00, 0x00, 0x03, 0xFE, 0x3A, 0x4A, 0x20, 0x30, 0x10, 0x88, 0xDE, 0x0D, 0x08, 0x0F, 0x0F, 0x01, 0x3A, 0x4A, 0x20, 0x10, 0x10, 0x00}, 23, 0},
    {0xC6, (uint8_t []){0x05, 0x0A, 0x05, 0x0A, 0x00, 0xE0, 0x2E, 0x0B, 0x12, 0x22, 0x12, 0x22, 0x01, 0x03, 0x00, 0x3F, 0x6A, 0x18, 0xC8, 0x22}, 20, 0},
    {0xC7, (uint8_t []){0x50, 0x32, 0x28, 0x00, 0xa2, 0x80, 0x8f, 0x00, 0x80, 0xff, 0x07, 0x11, 0x9c, 0x67, 0xff, 0x24, 0x0c, 0x0d, 0x0e, 0x0f}, 20, 0},
    {0xC9, (uint8_t []){0x33, 0x44, 0x44, 0x01}, 4, 0},
    {0xCF, (uint8_t []){0x2C, 0x1E, 0x88, 0x58, 0x13, 0x18, 0x56, 0x18, 0x1E, 0x68, 0x88, 0x00, 0x65, 0x09, 0x22, 0xC4, 0x0C, 0x77, 0x22, 0x44, 0xAA, 0x55, 0x08, 0x08, 0x12, 0xA0, 0x08}, 27, 0},
    {0xD5, (uint8_t []){0x40, 0x8E, 0x8D, 0x01, 0x35, 0x04, 0x92, 0x74, 0x04, 0x92, 0x74, 0x04, 0x08, 0x6A, 0x04, 0x46, 0x03, 0x03, 0x03, 0x03, 0x82, 0x01, 0x03, 0x00, 0xE0, 0x51, 0xA1, 0x00, 0x00, 0x00}, 30, 0},
    {0xD6, (uint8_t []){0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x93, 0x00, 0x01, 0x83, 0x07, 0x07, 0x00, 0x07, 0x07, 0x00, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x00, 0x84, 0x00, 0x20, 0x01, 0x00}, 30, 0},
    {0xD7, (uint8_t []){0x03, 0x01, 0x0b, 0x09, 0x0f, 0x0d, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19, 0x40, 0x8E, 0x04, 0x00, 0x20, 0xA0, 0x1F}, 19, 0},
    {0xD8, (uint8_t []){0x02, 0x00, 0x0a, 0x08, 0x0e, 0x0c, 0x1E, 0x1F, 0x18, 0x1d, 0x1f, 0x19}, 12, 0},
    {0xD9, (uint8_t []){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDD, (uint8_t []){0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F, 0x1F}, 12, 0},
    {0xDF, (uint8_t []){0x44, 0x73, 0x4B, 0x69, 0x00, 0x0A, 0x02, 0x90}, 8,  0},
    {0xE0, (uint8_t []){0x3B, 0x28, 0x10, 0x16, 0x0c, 0x06, 0x11, 0x28, 0x5c, 0x21, 0x0D, 0x35, 0x13, 0x2C, 0x33, 0x28, 0x0D}, 17, 0},
    {0xE1, (uint8_t []){0x37, 0x28, 0x10, 0x16, 0x0b, 0x06, 0x11, 0x28, 0x5C, 0x21, 0x0D, 0x35, 0x14, 0x2C, 0x33, 0x28, 0x0F}, 17, 0},
    {0xE2, (uint8_t []){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE3, (uint8_t []){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x35, 0x44, 0x32, 0x0C, 0x14, 0x14, 0x36, 0x32, 0x2F, 0x0F}, 17, 0},
    {0xE4, (uint8_t []){0x3B, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0D}, 17, 0},
    {0xE5, (uint8_t []){0x37, 0x07, 0x12, 0x18, 0x0E, 0x0D, 0x17, 0x39, 0x44, 0x2E, 0x0C, 0x14, 0x14, 0x36, 0x3A, 0x2F, 0x0F}, 17, 0},
    {0xA4, (uint8_t []){0x85, 0x85, 0x95, 0x82, 0xAF, 0xAA, 0xAA, 0x80, 0x10, 0x30, 0x40, 0x40, 0x20, 0xFF, 0x60, 0x30}, 16, 0},
    {0xA4, (uint8_t []){0x85, 0x85, 0x95, 0x85}, 4, 0},
    {0xBB, (uint8_t []){0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, 8, 0},
    {0x13, (uint8_t []){0x00}, 0, 0},
    {0x11, (uint8_t []){0x00}, 0, 120},
    {0x2C, (uint8_t []){0x00, 0x00, 0x00, 0x00}, 4, 0},
};

static esp_err_t init_panel(esp_lcd_panel_io_handle_t *out_io, esp_lcd_panel_handle_t *out_panel)
{
    esp_err_t err = init_io_expander();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Initializing QSPI bus");
    const spi_bus_config_t buscfg = AXS15231B_PANEL_BUS_QSPI_CONFIG(
        LCD_PIN_PCLK, LCD_PIN_DATA0, LCD_PIN_DATA1, LCD_PIN_DATA2, LCD_PIN_DATA3,
        LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t));
    err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Installing panel IO");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config =
        AXS15231B_PANEL_IO_QSPI_CONFIG(LCD_PIN_CS, NULL, NULL);
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Installing AXS15231B panel driver");
    esp_lcd_panel_handle_t panel_handle = NULL;
    const axs15231b_vendor_config_t vendor_config = {
        .init_cmds = s_lcd_init_cmds,
        .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = -1, /* V2: reset is the expander's LCD_RST pin */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = (void *)&vendor_config,
    };
    err = esp_lcd_new_panel_axs15231b(io_handle, &panel_config, &panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_axs15231b failed: %s", esp_err_to_name(err));
        return err;
    }

    expander_lcd_reset();
    esp_lcd_panel_init(panel_handle);
    /* Deliberately NOT calling esp_lcd_panel_disp_on_off() -- Waveshare's
     * working V2 demo doesn't either, and adding it left the panel solid
     * black. The vendor init sequence already does Sleep-Out + Display-On. */
    expander_backlight_set(true);

    *out_io = io_handle;
    *out_panel = panel_handle;
    return ESP_OK;
}

static bool lcd_flush_done_cb(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_sem, &woken);
    return woken == pdTRUE;
}

/* Hand-rolled LVGL flush: stages every wire transfer through a small
 * internal-RAM buffer in LCD_DRAW_BUF_LINES-row chunks, and (with
 * full_refresh forced on) always sweeps the whole panel top-to-bottom.
 * That exact combination is what esp_lcd_axs15231b's QSPI draw_bitmap()
 * needs -- see the sibling project's lcd_display.c for the full story. */
static void lcd_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
    const int width = area->x2 - area->x1 + 1;
    int y = area->y1;
    int rows_left = area->y2 - area->y1 + 1;
    const uint16_t *src = (const uint16_t *)color_map;

    xSemaphoreGive(s_flush_sem); /* prime -- nothing in flight for the first chunk */
    while (rows_left > 0) {
        int chunk_rows = (rows_left > LCD_DRAW_BUF_LINES) ? LCD_DRAW_BUF_LINES : rows_left;
        xSemaphoreTake(s_flush_sem, portMAX_DELAY);
        memcpy(s_lvgl_staging_buf, src, (size_t)width * chunk_rows * sizeof(uint16_t));
        esp_lcd_panel_draw_bitmap(panel_handle, area->x1, y, area->x1 + width, y + chunk_rows, s_lvgl_staging_buf);
        src += (size_t)width * chunk_rows;
        y += chunk_rows;
        rows_left -= chunk_rows;
    }
    xSemaphoreTake(s_flush_sem, portMAX_DELAY);
    lv_disp_flush_ready(drv);
}

static esp_err_t init_lvgl(esp_lcd_panel_io_handle_t io_handle, esp_lcd_panel_handle_t panel_handle)
{
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,
        .task_stack = 6144,
        .task_affinity = 1,
        .task_max_sleep_ms = 500,
        .task_stack_caps = 0,
        .timer_period_ms = 5,
    };
    esp_err_t err = lvgl_port_init(&lvgl_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    s_flush_sem = xSemaphoreCreateBinary();
    if (!s_flush_sem) {
        ESP_LOGE(TAG, "Failed to create LVGL flush semaphore");
        return ESP_ERR_NO_MEM;
    }

    /* Full-canvas render buffer (full_refresh mode has LVGL rasterize the
     * whole screen at absolute coords with no chunking -- a smaller buffer
     * would be silent heap corruption). PSRAM, CPU-touched only; nothing
     * DMAs from it -- the wire transfers go out through the staged copies. */
    const size_t render_px = (size_t)LCD_H_RES * LCD_V_RES;
    s_lvgl_render_buf = heap_caps_malloc(render_px * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_lvgl_render_buf) {
        ESP_LOGE(TAG, "Failed to allocate LVGL render buffer (%u px, PSRAM)", (unsigned)render_px);
        return ESP_ERR_NO_MEM;
    }
    s_lvgl_staging_buf = heap_caps_malloc((size_t)LCD_H_RES * LCD_DRAW_BUF_LINES * sizeof(uint16_t),
                                          MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_lvgl_staging_buf) {
        ESP_LOGE(TAG, "Failed to allocate QSPI staging buffer");
        return ESP_ERR_NO_MEM;
    }

    lv_disp_draw_buf_init(&s_draw_buf_dsc, s_lvgl_render_buf, NULL, render_px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_H_RES;
    s_disp_drv.ver_res = LCD_V_RES;
    s_disp_drv.flush_cb = lcd_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf_dsc;
    s_disp_drv.full_refresh = 1;
    s_disp_drv.user_data = panel_handle;

    const esp_lcd_panel_io_callbacks_t io_cbs = {
        .on_color_trans_done = lcd_flush_done_cb,
    };
    esp_lcd_panel_io_register_event_callbacks(io_handle, &io_cbs, NULL);

    s_disp = lv_disp_drv_register(&s_disp_drv);
    if (!s_disp) {
        ESP_LOGE(TAG, "lv_disp_drv_register failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* Touch on its own I2C bus. Axis mapping (swap_xy + mirror_x) confirmed
 * against real V2 hardware in the sibling project. Failures are logged and
 * swallowed -- the BOOT button is a full fallback key. */
static void init_touch(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = TOUCH_I2C_PORT,
        .scl_io_num = TOUCH_I2C_SCL,
        .sda_io_num = TOUCH_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t touch_bus = NULL;
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &touch_bus);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch: i2c_new_master_bus failed: %s -- tap-key zone won't respond, use BOOT button", esp_err_to_name(err));
        return;
    }

    esp_err_t probe_err = i2c_master_probe(touch_bus, ESP_LCD_TOUCH_IO_I2C_AXS15231B_ADDRESS, 200);
    ESP_LOGI(TAG, "touch: i2c_master_probe(0x%02x) -> %s", ESP_LCD_TOUCH_IO_I2C_AXS15231B_ADDRESS, esp_err_to_name(probe_err));

    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_AXS15231B_CONFIG();
    io_config.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t io_handle = NULL;
    err = esp_lcd_new_panel_io_i2c(touch_bus, &io_config, &io_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch: esp_lcd_new_panel_io_i2c failed: %s -- use BOOT button", esp_err_to_name(err));
        return;
    }

    const esp_lcd_touch_config_t touch_config = {
        .x_max = LCD_V_RES,
        .y_max = LCD_H_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .flags = {
            .swap_xy = 1,
            .mirror_x = 1,
        },
    };
    esp_lcd_touch_handle_t touch_handle = NULL;
    err = esp_lcd_touch_new_i2c_axs15231b(io_handle, &touch_config, &touch_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch: esp_lcd_touch_new_i2c_axs15231b failed: %s -- use BOOT button", esp_err_to_name(err));
        return;
    }

    const lvgl_port_touch_cfg_t touch_add_cfg = {
        .disp = s_disp,
        .handle = touch_handle,
    };
    if (!lvgl_port_add_touch(&touch_add_cfg)) {
        ESP_LOGW(TAG, "touch: lvgl_port_add_touch failed -- use BOOT button");
        return;
    }

    s_touch_handle = touch_handle;
    ESP_LOGI(TAG, "Touch input ready");
}

/* ---------------------------------------------------------------------- */
/* Scene                                                                  */
/* ---------------------------------------------------------------------- */

static void key_zone_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        s_key_down = true;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_key_down = false;
    }
}

/* MODE bar tapped -- hand back to main, which asks morse_key to toggle. */
static void modebar_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_cfg.on_mode_btn) {
        s_cfg.on_mode_btn(s_cfg.ctx);
    }
}

/* A length button 1..6 tapped -- the length is stashed as the event user
 * data. */
static void length_event_cb(lv_event_t *e)
{
    int len = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_cfg.on_length_btn) {
        s_cfg.on_length_btn(len, s_cfg.ctx);
    }
}

/* Tapping the running-output strip clears it. Runs on the LVGL task. */
static void output_clear_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_output_label) {
        lv_label_set_text(s_output_label, "");
        s_output_len = 0;
    }
    if (s_pending_mux) { /* drop any not-yet-applied appends too */
        xSemaphoreTake(s_pending_mux, portMAX_DELAY);
        s_pending.output[0] = '\0';
        xSemaphoreGive(s_pending_mux);
    }
}

static void flash_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_letter_label) {
        lv_obj_set_style_text_color(s_letter_label, COLOR_LETTER, 0);
    }
    s_flash_timer = NULL; /* one-shot; LVGL deletes it after this returns */
}

static lv_obj_t *make_label(lv_obj_t *scr, int y, int h, const lv_font_t *font,
                            lv_color_t color, lv_text_align_t align)
{
    lv_obj_t *lbl = lv_label_create(scr);
    lv_obj_set_size(lbl, LCD_H_RES, h);
    lv_obj_set_pos(lbl, 0, y);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    lv_obj_set_style_text_align(lbl, align, 0);
    /* Transparent by default so the screen's navy->black gradient shows
     * through; widgets that want their own fill set it after creation. */
    lv_obj_set_style_bg_opa(lbl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(lbl, 2, 0);
    return lbl;
}

static void ui_apply_cb(lv_timer_t *t); /* defined below, in the public-API section */

/* Show the widget set for `mode` and hide the other, and refresh the MODE
 * bar caption. LVGL context (build_scene or ui_apply_cb). */
static void set_widget_mode(lcd_ui_mode_t mode)
{
    bool practice = (mode == LCD_UI_MODE_PRACTICE);

    lv_obj_t *training_only[] = { s_cand_label, s_output_label, s_divider };
    lv_obj_t *practice_only[] = { s_len_row, s_pcap_label, s_ptarget_label, s_pstatus_label };
    for (size_t i = 0; i < sizeof(training_only) / sizeof(training_only[0]); i++) {
        if (training_only[i]) {
            if (practice) {
                lv_obj_add_flag(training_only[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_clear_flag(training_only[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    for (size_t i = 0; i < sizeof(practice_only) / sizeof(practice_only[0]); i++) {
        if (practice_only[i]) {
            if (practice) {
                lv_obj_clear_flag(practice_only[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(practice_only[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    if (s_modebar_label) {
        lv_label_set_text(s_modebar_label, practice ? "MODE: PRACTICE" : "MODE: TRAINING");
    }
}

static void build_scene(void)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock while building the scene");
        return;
    }

    lv_obj_t *scr = lv_scr_act();
    /* Brand background: navy at the top fading to black at the key zone. */
    lv_obj_set_style_bg_color(scr, COLOR_SCREEN_TOP, 0);
    lv_obj_set_style_bg_grad_color(scr, COLOR_SCREEN_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* Full-width MODE bar -- tap to toggle Training / Practice. */
    s_modebar = lv_btn_create(scr);
    lv_obj_set_size(s_modebar, LCD_H_RES, MODEBAR_H);
    lv_obj_set_pos(s_modebar, 0, MODEBAR_Y);
    lv_obj_set_style_radius(s_modebar, 0, 0);
    lv_obj_set_style_bg_color(s_modebar, COLOR_MODEBAR, 0);
    lv_obj_set_style_bg_color(s_modebar, COLOR_MODEBAR_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_modebar, modebar_event_cb, LV_EVENT_CLICKED, NULL);
    s_modebar_label = lv_label_create(s_modebar);
    lv_label_set_text(s_modebar_label, "MODE: TRAINING");
    lv_obj_set_style_text_color(s_modebar_label, COLOR_LETTER, 0);
    lv_obj_center(s_modebar_label);

    /* Drawn dot/dash shapes live in a centred, wrapping flex row. */
    s_symbol_row = lv_obj_create(scr);
    lv_obj_remove_style_all(s_symbol_row);
    lv_obj_set_size(s_symbol_row, LCD_H_RES, SYMBOL_H);
    lv_obj_set_pos(s_symbol_row, 0, SYMBOL_Y);
    lv_obj_set_style_bg_opa(s_symbol_row, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_symbol_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_symbol_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_symbol_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_symbol_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_symbol_row, 6, 0);
    lv_obj_set_style_pad_column(s_symbol_row, SYMBOL_GAP, 0);

    s_letter_label = make_label(scr, LETTER_Y, LETTER_H, &lv_font_montserrat_48,
                                COLOR_LETTER, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_letter_label, "");

    s_output_label = make_label(scr, OUTPUT_Y, OUTPUT_H, LV_FONT_DEFAULT,
                                COLOR_OUTPUT, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_output_label, "");
    /* The output strip is a tap target: touching it clears the text. */
    lv_obj_add_flag(s_output_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_output_label, output_clear_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_radius(s_output_label, 4, 0);
    lv_obj_set_style_bg_opa(s_output_label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_output_label, COLOR_OUTPUT_BG, 0);
    lv_obj_set_style_bg_color(s_output_label, COLOR_OUTPUT_BG_PRESSED, LV_STATE_PRESSED);

    s_divider = lv_obj_create(scr);
    lv_obj_set_size(s_divider, LCD_H_RES, 2);
    lv_obj_set_pos(s_divider, 0, DIVIDER_Y);
    lv_obj_set_style_bg_color(s_divider, COLOR_DIVIDER, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);

    s_cand_label = make_label(scr, CAND_Y, CAND_H, &lv_font_montserrat_20,
                              COLOR_CAND, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_cand_label, "");

    /* --- Practice widgets (hidden until Practice mode) --- */
    s_len_row = lv_obj_create(scr);
    lv_obj_remove_style_all(s_len_row);
    lv_obj_set_size(s_len_row, LCD_H_RES, LENROW_H);
    lv_obj_set_pos(s_len_row, 0, LENROW_Y);
    lv_obj_set_style_bg_opa(s_len_row, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollbar_mode(s_len_row, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(s_len_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_len_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_len_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_len_row, 4, 0);
    for (int i = 0; i < 6; i++) {
        lv_obj_t *b = lv_btn_create(s_len_row);
        lv_obj_set_size(b, 24, 32);
        lv_obj_set_style_radius(b, 4, 0);
        lv_obj_set_style_bg_color(b, COLOR_LENBTN, 0);
        lv_obj_set_style_bg_color(b, COLOR_LENBTN_SEL, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, length_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
        lv_obj_t *bl = lv_label_create(b);
        lv_label_set_text_fmt(bl, "%d", i + 1);
        lv_obj_set_style_text_color(bl, COLOR_LETTER, 0);
        lv_obj_center(bl);
        s_len_btn[i] = b;
    }

    s_pcap_label = make_label(scr, PCAP_Y, PCAP_H, &lv_font_montserrat_20,
                              COLOR_OUTPUT, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_pcap_label, "TARGET");

    s_ptarget_label = make_label(scr, PTARGET_Y, PTARGET_H, &lv_font_montserrat_48,
                                 COLOR_LETTER, LV_TEXT_ALIGN_CENTER);
    lv_label_set_recolor(s_ptarget_label, true);
    lv_label_set_text(s_ptarget_label, "");

    s_pstatus_label = make_label(scr, PSTATUS_Y, PSTATUS_H, &lv_font_montserrat_20,
                                 COLOR_CAND, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_pstatus_label, "");

    s_key_zone = lv_btn_create(scr);
    lv_obj_set_size(s_key_zone, LCD_H_RES, KEYZONE_H);
    lv_obj_set_pos(s_key_zone, 0, KEYZONE_Y);
    lv_obj_set_style_radius(s_key_zone, 0, 0);
    lv_obj_set_style_bg_color(s_key_zone, COLOR_KEYZONE, 0);
    lv_obj_set_style_bg_color(s_key_zone, COLOR_KEYZONE_PRESSED, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_key_zone, key_zone_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *kl = lv_label_create(s_key_zone);
    lv_label_set_text(kl, "TAP KEY\nshort = dot   long = dash");
    lv_obj_set_style_text_color(kl, COLOR_LETTER, 0);
    lv_obj_set_style_text_align(kl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(kl);

    /* Start in Training mode -- hides the Practice widgets just built. */
    set_widget_mode(LCD_UI_MODE_TRAINING);

    /* Paints whatever the keyer stashed; see the s_pending comment. */
    s_ui_timer = lv_timer_create(ui_apply_cb, 25, NULL);

    lvgl_port_unlock();
}

/* ---------------------------------------------------------------------- */
/* Public API                                                            */
/* ---------------------------------------------------------------------- */

esp_err_t lcd_ui_init(const lcd_ui_cfg_t *cfg)
{
    if (cfg) {
        s_cfg = *cfg;
    }

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_err_t err = init_panel(&io_handle, &panel_handle);
    if (err != ESP_OK) {
        return err;
    }
    err = init_lvgl(io_handle, panel_handle);
    if (err != ESP_OK) {
        return err;
    }
    s_pending_mux = xSemaphoreCreateMutex();
    if (!s_pending_mux) {
        ESP_LOGE(TAG, "Failed to create the pending-UI mutex");
        return ESP_ERR_NO_MEM;
    }

    init_touch();
    build_scene();
    ESP_LOGI(TAG, "UI ready");
    return ESP_OK;
}

/* --- Apply-from-the-LVGL-task side ---------------------------------- */

/* Rebuild the symbol strip: one drawn shape per entered symbol, a filled
 * circle for a dot and a filled rounded bar for a dash. LVGL context. */
static void rebuild_symbol_shapes(const char *ascii_dotdash)
{
    lv_obj_clean(s_symbol_row);
    for (const char *c = ascii_dotdash ? ascii_dotdash : ""; *c; c++) {
        bool dash = (*c == '-');
        lv_obj_t *shape = lv_obj_create(s_symbol_row);
        lv_obj_remove_style_all(shape);
        lv_obj_set_size(shape, dash ? DASH_LEN : SYMBOL_THK, SYMBOL_THK);
        lv_obj_set_style_radius(shape, dash ? (SYMBOL_THK / 2) : LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(shape, COLOR_SYMBOL, 0);
        lv_obj_set_style_bg_opa(shape, LV_OPA_COVER, 0);
    }
}

/* Append `chars` to the running output, trimming from the front once it
 * grows past OUTPUT_MAX_CHARS. LVGL context. */
static void apply_output(const char *chars)
{
    if (!chars || !chars[0]) {
        return;
    }
    lv_label_ins_text(s_output_label, LV_LABEL_POS_LAST, chars);
    s_output_len += strlen(chars);
    if (s_output_len > OUTPUT_MAX_CHARS) {
        const char *full = lv_label_get_text(s_output_label);
        size_t full_len = strlen(full);
        size_t drop = full_len > OUTPUT_TRIM_CHARS ? OUTPUT_TRIM_CHARS : full_len;
        lv_label_set_text(s_output_label, full + drop);
        s_output_len = full_len - drop;
    }
}

/* Paint the Practice target word from `word` + per-glyph `mark`
 * (' '/'o'/'x'), recolouring each glyph. Also picks a font that fits the
 * word width. LVGL context. */
static void rebuild_practice_word(const char *word, const char *mark)
{
    size_t len = strlen(word);

    char buf[8 * 12 + 1]; /* up to 8 glyphs, "#rrggbb x#" each */
    size_t used = 0;
    for (size_t i = 0; i < len && used < sizeof(buf) - 12; i++) {
        const char *col = PMARK_PENDING;
        if (mark[i] == 'o') {
            col = PMARK_OK;
        } else if (mark[i] == 'x') {
            col = PMARK_WRONG;
        }
        used += (size_t)snprintf(buf + used, sizeof(buf) - used, "#%s %c#", col, word[i]);
    }
    buf[used] = '\0';

    lv_obj_set_style_text_font(s_ptarget_label,
                               len <= 4 ? &lv_font_montserrat_48 : &lv_font_montserrat_20, 0);
    lv_label_set_text(s_ptarget_label, buf);
}

/* Runs on the LVGL task (via lv_timer_handler, which holds LVGL's lock).
 * Pulls whatever the keyer stashed and paints it. */
static void ui_apply_cb(lv_timer_t *t)
{
    (void)t;

    /* static: this timer only ever runs on the one LVGL task, and these
     * spare the LVGL task stack a ~600-byte hit inside lv_timer_handler. */
    static char symbols[sizeof(s_pending.symbols)];
    static char cands[sizeof(s_pending.cands)];
    static char output[sizeof(s_pending.output)];
    static char ptarget[sizeof(s_pending.ptarget)];
    static char pmark[sizeof(s_pending.pmark)];
    static char pstatus[sizeof(s_pending.pstatus)];
    char letter;
    int mode, length;
    bool flash, do_symbols, do_cands, do_letter, do_mode, do_length, do_practice, do_pstatus;

    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    do_symbols = s_pending.symbols_dirty;
    do_cands = s_pending.cands_dirty;
    do_letter = s_pending.letter_dirty;
    do_mode = s_pending.mode_dirty;
    do_length = s_pending.length_dirty;
    do_practice = s_pending.practice_dirty;
    do_pstatus = s_pending.pstatus_dirty;
    flash = s_pending.flash;
    letter = s_pending.letter;
    mode = s_pending.mode;
    length = s_pending.length;
    memcpy(symbols, s_pending.symbols, sizeof(symbols));
    memcpy(cands, s_pending.cands, sizeof(cands));
    memcpy(output, s_pending.output, sizeof(output));
    memcpy(ptarget, s_pending.ptarget, sizeof(ptarget));
    memcpy(pmark, s_pending.pmark, sizeof(pmark));
    memcpy(pstatus, s_pending.pstatus, sizeof(pstatus));
    s_pending.symbols_dirty = s_pending.cands_dirty = s_pending.letter_dirty = false;
    s_pending.mode_dirty = s_pending.length_dirty = false;
    s_pending.practice_dirty = s_pending.pstatus_dirty = false;
    s_pending.flash = false;
    s_pending.output[0] = '\0';
    xSemaphoreGive(s_pending_mux);

    if (do_mode) {
        set_widget_mode((lcd_ui_mode_t)mode);
    }
    if (do_length) {
        for (int i = 0; i < 6; i++) {
            bool sel = (i + 1 == length);
            lv_obj_set_style_bg_color(s_len_btn[i], sel ? COLOR_LENBTN_SEL : COLOR_LENBTN, 0);
            lv_obj_t *bl = lv_obj_get_child(s_len_btn[i], 0);
            if (bl) {
                /* dark digit on the bright cyan chip, white on the navy ones */
                lv_obj_set_style_text_color(bl, sel ? COLOR_SCREEN_TOP : COLOR_LETTER, 0);
            }
        }
    }
    if (do_practice) {
        rebuild_practice_word(ptarget, pmark);
    }
    if (do_pstatus) {
        lv_label_set_text(s_pstatus_label, pstatus);
    }

    if (do_symbols) {
        rebuild_symbol_shapes(symbols);
    }
    if (do_cands) {
        lv_label_set_text(s_cand_label, cands);
    }
    if (do_letter) {
        char buf[2] = { letter ? letter : '?', '\0' };
        lv_label_set_text(s_letter_label, letter ? buf : "");
        if (flash) {
            lv_obj_set_style_text_color(s_letter_label, COLOR_FLASH, 0);
            if (s_flash_timer) {
                lv_timer_reset(s_flash_timer);
            } else {
                s_flash_timer = lv_timer_create(flash_timer_cb, LETTER_FLASH_MS, NULL);
                lv_timer_set_repeat_count(s_flash_timer, 1);
            }
        } else {
            lv_obj_set_style_text_color(s_letter_label, COLOR_LETTER, 0);
        }
    }
    apply_output(output);
}

/* --- Public API: stash state, never touch LVGL directly ------------- */

void lcd_ui_set_symbols(const char *ascii_dotdash)
{
    if (!s_pending_mux) {
        return;
    }
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    strlcpy(s_pending.symbols, ascii_dotdash ? ascii_dotdash : "", sizeof(s_pending.symbols));
    s_pending.symbols_dirty = true;
    xSemaphoreGive(s_pending_mux);
}

void lcd_ui_set_big_letter(char c)
{
    if (!s_pending_mux) {
        return;
    }
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    s_pending.letter = c;
    s_pending.flash = false;
    s_pending.letter_dirty = true;
    xSemaphoreGive(s_pending_mux);
}

void lcd_ui_flash_letter(char c)
{
    if (!s_pending_mux) {
        return;
    }
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    s_pending.letter = c ? c : '?';
    s_pending.flash = true;
    s_pending.letter_dirty = true;
    xSemaphoreGive(s_pending_mux);
}

void lcd_ui_append_output(char c)
{
    if (!s_pending_mux) {
        return;
    }
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    size_t len = strlen(s_pending.output);
    if (len + 1 < sizeof(s_pending.output)) {
        s_pending.output[len] = c;
        s_pending.output[len + 1] = '\0';
    }
    xSemaphoreGive(s_pending_mux);
}

void lcd_ui_set_candidates(const char *multiline)
{
    if (!s_pending_mux) {
        return;
    }
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    strlcpy(s_pending.cands, multiline ? multiline : "", sizeof(s_pending.cands));
    s_pending.cands_dirty = true;
    xSemaphoreGive(s_pending_mux);
}

void lcd_ui_show(const char *symbols, char letter, const char *candidates)
{
    if (!s_pending_mux) {
        return;
    }
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    strlcpy(s_pending.symbols, symbols ? symbols : "", sizeof(s_pending.symbols));
    strlcpy(s_pending.cands, candidates ? candidates : "", sizeof(s_pending.cands));
    s_pending.letter = letter;
    s_pending.flash = false;
    s_pending.symbols_dirty = s_pending.cands_dirty = s_pending.letter_dirty = true;
    xSemaphoreGive(s_pending_mux);
}

void lcd_ui_set_mode(lcd_ui_mode_t mode)
{
    if (!s_pending_mux) {
        return;
    }
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    s_pending.mode = (int)mode;
    s_pending.mode_dirty = true;
    xSemaphoreGive(s_pending_mux);
}

void lcd_ui_set_length(int len)
{
    if (!s_pending_mux) {
        return;
    }
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    s_pending.length = len;
    s_pending.length_dirty = true;
    xSemaphoreGive(s_pending_mux);
}

void lcd_ui_practice_set_target(const char *word)
{
    if (!s_pending_mux) {
        return;
    }
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    strlcpy(s_pending.ptarget, word ? word : "", sizeof(s_pending.ptarget));
    memset(s_pending.pmark, ' ', sizeof(s_pending.pmark));
    s_pending.practice_dirty = true;
    xSemaphoreGive(s_pending_mux);
}

void lcd_ui_practice_mark(int index, bool correct)
{
    if (!s_pending_mux) {
        return;
    }
    if (index < 0 || index >= (int)sizeof(s_pending.pmark)) {
        return;
    }
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    s_pending.pmark[index] = correct ? 'o' : 'x';
    s_pending.practice_dirty = true;
    xSemaphoreGive(s_pending_mux);
}

void lcd_ui_practice_set_status(const char *text)
{
    if (!s_pending_mux) {
        return;
    }
    xSemaphoreTake(s_pending_mux, portMAX_DELAY);
    strlcpy(s_pending.pstatus, text ? text : "", sizeof(s_pending.pstatus));
    s_pending.pstatus_dirty = true;
    xSemaphoreGive(s_pending_mux);
}

bool lcd_ui_key_is_down(void)
{
    return s_key_down;
}

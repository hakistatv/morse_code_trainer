/*
 * Digital Morse Code Trainer for the Waveshare ESP32-S3-Touch-LCD-3.49.
 *
 * Two modes share one screen (tap the MODE bar, or hold BOOT ~3 s):
 *
 *   Training -- a re-creation of the physical "Morse Code Trainer Card":
 *     key dots and dashes (BOOT button or the on-screen tap zone -- short
 *     = dot, long = dash) and watch the pointer walk the dichotomic Morse
 *     tree until it lands on a letter, which lights up and is appended to
 *     the running output line, with an autocomplete list of every letter
 *     still reachable from the current prefix.
 *
 *   Practice -- the screen shows a real word (length 1-6, pick with the
 *     on-screen buttons); key the whole word in Morse. Each committed
 *     letter is scored against the expected position -- green if right,
 *     red if wrong, no retry. When the word is finished the status line
 *     shows SUCCESS (all letters right) or FAIL, with a matching
 *     rising/falling tone. The next dot/dash starts a fresh word.
 *
 *   app_main -> morse_tree_init -> lcd_ui_init -> morse_key_init
 *
 * See ~/.claude/plans/i-want-to-create-snug-spindle.md for the original
 * design.
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_random.h"

#include "morse_tree.h"
#include "lcd_ui.h"
#include "morse_key.h"
#include "sidetone.h"
#include "word_list.h"

static const char *TAG = "morse_code_trainer";

/* --- Mode + Practice state -------------------------------------------------
 * s_mode is flipped from the morse_key task (on_mode_switch). The Practice
 * fields are touched both from the morse_key task (on_letter) and the LVGL
 * task (on_length_btn -> load_new_word), so a short mutex guards them. --- */
static volatile lcd_ui_mode_t s_mode = LCD_UI_MODE_TRAINING;

static SemaphoreHandle_t s_practice_mux;
static int s_len = 3;              /* target word length 1..6 */
static char s_target[8];
static int s_pos;                 /* index of the next expected letter */
static int s_correct;             /* correct letters so far this attempt */
static bool s_attempt_done;       /* whole word keyed; next symbol starts a new one */

/* --- Training-mode view ------------------------------------------------- */

static void refresh_view(void)
{
    char candidates[512];
    morse_tree_render_candidates(candidates, sizeof(candidates));
    /* 0 letter -> blank until an exact code is keyed. */
    lcd_ui_show(morse_tree_current_code(), morse_tree_current_letter(), candidates);
}

/* --- Practice-mode helpers ------------------------------------------------ */

/* Pick a fresh target of the current length and repaint. Caller must hold
 * s_practice_mux. */
static void load_new_word_locked(void)
{
    const char *w = word_list_pick(s_len, esp_random());
    strlcpy(s_target, w ? w : "", sizeof(s_target));
    s_pos = 0;
    s_correct = 0;
    s_attempt_done = false;

    if (s_target[0]) {
        lcd_ui_practice_set_target(s_target);
        lcd_ui_practice_set_status("key it");
    } else {
        lcd_ui_practice_set_target("");
        lcd_ui_practice_set_status("no words");
    }
}

static void load_new_word(void)
{
    xSemaphoreTake(s_practice_mux, portMAX_DELAY);
    load_new_word_locked();
    xSemaphoreGive(s_practice_mux);
}

/* --- Keyer callbacks (morse_key task) --------------------------------- */

static void on_symbol(bool is_dash, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", is_dash ? "dash" : "dot");
    if (s_mode == LCD_UI_MODE_TRAINING) {
        refresh_view();
    } else {
        /* Just the drawn dot/dash strip -- no tree autocomplete in Practice. */
        lcd_ui_set_symbols(morse_tree_current_code());
    }
}

static void on_letter(char letter, void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "[decoded] %c", letter);

    if (s_mode == LCD_UI_MODE_TRAINING) {
        lcd_ui_flash_letter(letter);
        lcd_ui_append_output(letter);
        /* Clear the working strip / candidate list now that the letter is committed. */
        lcd_ui_set_symbols("");
        lcd_ui_set_candidates("");
        return;
    }

    /* Practice: score this letter against the expected position. */
    xSemaphoreTake(s_practice_mux, portMAX_DELAY);
    if (s_attempt_done) {
        load_new_word_locked(); /* this letter is the first of the next word */
    }
    if (s_target[0] && s_pos < (int)strlen(s_target)) {
        bool ok = (letter == s_target[s_pos]);
        if (ok) {
            s_correct++;
        }
        lcd_ui_practice_mark(s_pos, ok);
        s_pos++;
        if (s_pos >= (int)strlen(s_target)) {
            s_attempt_done = true;
            bool win = (s_correct == s_pos); /* every letter matched */
            lcd_ui_practice_set_status(win ? "SUCCESS" : "FAIL");
            sidetone_cue(win ? SIDETONE_CUE_SUCCESS : SIDETONE_CUE_FAIL);
        }
    }
    xSemaphoreGive(s_practice_mux);

    lcd_ui_flash_letter(letter); /* "you keyed X" feedback, same as Training */
    lcd_ui_set_symbols("");
}

static void on_space(void *ctx)
{
    (void)ctx;
    if (s_mode == LCD_UI_MODE_TRAINING) {
        ESP_LOGI(TAG, "[decoded] <space>");
        lcd_ui_append_output(' ');
    }
    /* Practice: word gaps are meaningless -- ignore. */
}

static void on_gate(bool key_down, void *ctx)
{
    (void)ctx;
    sidetone_gate(key_down); /* practice-oscillator tone, sounds while the key is held */
}

/* Training <-> Practice, from the MODE bar or a 3 s BOOT hold. morse_key
 * has already reset the tree + its timing; we just flip our own view. */
static void on_mode_switch(void *ctx)
{
    (void)ctx;
    s_mode = (s_mode == LCD_UI_MODE_TRAINING) ? LCD_UI_MODE_PRACTICE : LCD_UI_MODE_TRAINING;
    ESP_LOGI(TAG, "mode -> %s", s_mode == LCD_UI_MODE_PRACTICE ? "PRACTICE" : "TRAINING");
    lcd_ui_set_mode(s_mode);
    lcd_ui_set_symbols("");
    lcd_ui_set_big_letter(0);

    if (s_mode == LCD_UI_MODE_PRACTICE) {
        lcd_ui_set_length(s_len);
        load_new_word();
    } else {
        refresh_view();
    }
}

static void on_hold_hint(morse_key_hold_evt_t evt, void *ctx)
{
    (void)ctx;
    if (evt == MORSE_KEY_HOLD_ARMED) {
        if (s_mode == LCD_UI_MODE_PRACTICE) {
            lcd_ui_practice_set_status("hold to switch mode\xE2\x80\xA6");
        } else {
            lcd_ui_set_symbols("");
            lcd_ui_set_candidates("hold to switch mode\xE2\x80\xA6\nrelease to stay in Training");
        }
    } else { /* ABORTED */
        if (s_mode == LCD_UI_MODE_PRACTICE) {
            lcd_ui_practice_set_status(s_attempt_done ? "done" : "key it");
        } else {
            refresh_view();
        }
    }
}

/* --- On-screen control callbacks (LVGL task) ------------------------- */

static void on_mode_btn(void *ctx)
{
    (void)ctx;
    morse_key_request_mode_toggle(); /* the key task performs the switch */
}

static void on_length_btn(int len, void *ctx)
{
    (void)ctx;
    if (len < WORD_LIST_MIN_LEN || len > WORD_LIST_MAX_LEN) {
        return;
    }
    s_len = len;
    lcd_ui_set_length(len);
    if (s_mode == LCD_UI_MODE_PRACTICE) {
        load_new_word();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "morse_code_trainer starting up");

    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "This is %s chip with %d CPU core(s), silicon revision v%d.%d",
             CONFIG_IDF_TARGET, chip_info.cores,
             chip_info.revision / 100, chip_info.revision % 100);
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "%" PRIu32 "MB flash, minimum free heap: %" PRIu32 " bytes",
                 flash_size / (uint32_t)(1024 * 1024), esp_get_minimum_free_heap_size());
    }

    s_practice_mux = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_practice_mux ? ESP_OK : ESP_ERR_NO_MEM);

    morse_tree_init();

    const lcd_ui_cfg_t ui_cfg = {
        .on_mode_btn = on_mode_btn,
        .on_length_btn = on_length_btn,
    };
    esp_err_t err = lcd_ui_init(&ui_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lcd_ui_init failed (%s) -- continuing headless (serial log only)",
                 esp_err_to_name(err));
    }

    err = sidetone_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sidetone_init failed (%s) -- continuing without audio", esp_err_to_name(err));
    }

    const morse_key_cfg_t key_cfg = {
        .on_symbol = on_symbol,
        .on_letter = on_letter,
        .on_space = on_space,
        .on_gate = on_gate,
        .on_mode_switch = on_mode_switch,
        .on_hold_hint = on_hold_hint,
    };
    ESP_ERROR_CHECK(morse_key_init(&key_cfg));

    ESP_LOGI(TAG, "ready -- Training mode; tap MODE or hold BOOT 3 s for Practice");
}

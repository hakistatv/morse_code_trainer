/*
 * Short/long-press Morse keyer. One poll loop (MORSE_KEY_POLL_MS) reads
 * the BOOT button OR'd with the on-screen tap zone, debounces the
 * combined signal, and classifies each press:
 *   - released before MORSE_KEY_LONG_MS   -> dot
 *   - still held at MORSE_KEY_LONG_MS     -> dash (fired then; the
 *                                            eventual release is ignored)
 * Between presses it runs the letter/word timing off the same loop's
 * clock -- no separate esp_timer, so everything that touches morse_tree
 * stays on this one task.
 */

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "morse_tree.h"
#include "lcd_ui.h"
#include "morse_key.h"

static const char *TAG = "morse_key";

#define BOOT_GPIO        GPIO_NUM_0
#define MORSE_KEY_POLL_MS 10
#define DEBOUNCE_MS       25

static morse_key_cfg_t s_cfg;

/* Set by morse_key_request_mode_toggle() from any task; consumed at the
 * top of key_task's poll so morse_tree stays single-writer. */
static volatile bool s_toggle_pending;

void morse_key_request_mode_toggle(void)
{
    s_toggle_pending = true;
}

static void emit_symbol(bool is_dash, int64_t now_us)
{
    if (is_dash) {
        morse_tree_dash();
    } else {
        morse_tree_dot();
    }
    if (s_cfg.on_symbol) {
        s_cfg.on_symbol(is_dash, s_cfg.ctx);
    }
    (void)now_us;
}

static void commit_letter(void)
{
    char c = morse_tree_current_letter();
    if (c == 0) {
        c = '?';
    }
    if (s_cfg.on_letter) {
        s_cfg.on_letter(c, s_cfg.ctx);
    }
    morse_tree_reset();
}

static void key_task(void *arg)
{
    (void)arg;

    bool stable_pressed = false;
    bool raw_pressed = false;
    int64_t last_raw_change_us = 0;
    int64_t press_start_us = 0;
    int64_t last_symbol_us = 0;
    bool long_fired = false;
    bool mid_letter = false;       /* at least one symbol keyed, not yet committed */
    bool awaiting_word_gap = false; /* a letter was committed, watching for a word space */

    /* BOOT-only mode-switch hold (separate from the dot/dash key, which is
     * BOOT OR the tap zone). */
    bool boot_was_down = false;
    bool hold_armed = false;    /* held past the hint threshold */
    bool hold_consumed = false; /* switch fired; ignore the rest of this hold */
    int64_t boot_down_since_us = 0;

    for (;;) {
        int64_t now = esp_timer_get_time();

        /* A requested mode switch (from the 3 s hold below or from
         * morse_key_request_mode_toggle()) lands here, on this task, so
         * morse_tree keeps exactly one writer. */
        if (s_toggle_pending) {
            s_toggle_pending = false;
            morse_tree_reset();
            mid_letter = false;
            awaiting_word_gap = false;
            long_fired = true; /* swallow any press in progress */
            if (s_cfg.on_mode_switch) {
                s_cfg.on_mode_switch(s_cfg.ctx);
            }
        }

        bool raw = (gpio_get_level(BOOT_GPIO) == 0) || lcd_ui_key_is_down();

        /* --- BOOT-only hold -> mode switch --- */
        bool boot_now = (gpio_get_level(BOOT_GPIO) == 0);
        if (boot_now && !boot_was_down) {
            boot_down_since_us = now;
            hold_armed = false;
        }
        if (!boot_now && boot_was_down) {
            if (hold_armed && s_cfg.on_hold_hint) {
                s_cfg.on_hold_hint(MORSE_KEY_HOLD_ABORTED, s_cfg.ctx);
            }
            hold_armed = false;
            hold_consumed = false;
        }
        boot_was_down = boot_now;

        if (boot_now && !hold_armed && !hold_consumed &&
            (now - boot_down_since_us) >= MORSE_KEY_MODE_HINT_MS * 1000) {
            hold_armed = true;
            /* Abandon the press: erase the dash keyed at MORSE_KEY_LONG_MS
             * and stop this press emitting anything on release. */
            morse_tree_reset();
            mid_letter = false;
            awaiting_word_gap = false;
            long_fired = true;
            if (s_cfg.on_hold_hint) {
                s_cfg.on_hold_hint(MORSE_KEY_HOLD_ARMED, s_cfg.ctx);
            }
        }
        if (boot_now && hold_armed && !hold_consumed &&
            (now - boot_down_since_us) >= MORSE_KEY_MODE_HOLD_MS * 1000) {
            hold_armed = false;
            hold_consumed = true;
            s_toggle_pending = true; /* handled at the top of the next poll */
        }

        if (raw != raw_pressed) {
            raw_pressed = raw;
            last_raw_change_us = now;
        }
        if (raw_pressed != stable_pressed && (now - last_raw_change_us) >= DEBOUNCE_MS * 1000) {
            stable_pressed = raw_pressed;
            if (s_cfg.on_gate) {
                s_cfg.on_gate(stable_pressed, s_cfg.ctx); /* sidetone follows the key, dot/dash classification below is unchanged */
            }
            if (stable_pressed) {
                press_start_us = now;
                long_fired = false;
            } else {
                if (!long_fired) { /* clean short release -> dot */
                    emit_symbol(false, now);
                    last_symbol_us = now;
                    mid_letter = true;
                    awaiting_word_gap = false;
                }
            }
        }

        /* Held long enough -> dash, fired once while still down. */
        if (stable_pressed && !long_fired && (now - press_start_us) >= MORSE_KEY_LONG_MS * 1000) {
            long_fired = true;
            emit_symbol(true, now);
            last_symbol_us = now;
            mid_letter = true;
            awaiting_word_gap = false;
        }

        /* Letter / word timing -- only while the key is up. */
        if (!stable_pressed) {
            if (mid_letter && (now - last_symbol_us) >= MORSE_KEY_INTER_LETTER_MS * 1000) {
                commit_letter();
                mid_letter = false;
                awaiting_word_gap = true;
            } else if (awaiting_word_gap && (now - last_symbol_us) >= MORSE_KEY_WORD_MS * 1000) {
                awaiting_word_gap = false;
                if (s_cfg.on_space) {
                    s_cfg.on_space(s_cfg.ctx);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(MORSE_KEY_POLL_MS));
    }
}

esp_err_t morse_key_init(const morse_key_cfg_t *cfg)
{
    if (cfg) {
        s_cfg = *cfg;
    }

    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    if (xTaskCreate(key_task, "morse_key", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(morse_key) failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Keyer up: BOOT (GPIO%d) or tap zone; short=dot, long(>=%dms)=dash",
             BOOT_GPIO, MORSE_KEY_LONG_MS);
    return ESP_OK;
}

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The Morse "key": one short/long-press classifier fed by two sources in
 * parallel -- the onboard BOOT button (GPIO0) and the on-screen tap zone
 * (polled via lcd_ui_key_is_down()). A quick press is a dot, a held press
 * (>= MORSE_KEY_LONG_MS) is a dash.
 *
 * morse_key owns the traversal of morse_tree: each symbol advances the
 * tree pointer; after MORSE_KEY_INTER_LETTER_MS of no input the current
 * node's character is committed and the pointer resets; a further gap of
 * MORSE_KEY_WORD_MS emits a space. All of this runs in one dedicated
 * FreeRTOS task, so morse_tree has exactly one writer and needs no lock.
 *
 * The callbacks fire from that task. Keep them non-blocking-ish -- the
 * lcd_ui setters they typically call already marshal onto LVGL's lock
 * with a bounded wait, which is fine.
 */

/* Milliseconds. Generous defaults for a learner keying by hand; tune
 * against real use. */
#define MORSE_KEY_LONG_MS          250
#define MORSE_KEY_INTER_LETTER_MS  900
#define MORSE_KEY_WORD_MS          2100

/* Hold the BOOT button (alone, not the tap zone) this long to switch
 * between Training and Practice mode. MODE_HINT_MS is when the "keep
 * holding..." hint fires and the in-progress press is abandoned. */
#define MORSE_KEY_MODE_HINT_MS     800
#define MORSE_KEY_MODE_HOLD_MS     3000

typedef void (*morse_key_symbol_cb_t)(bool is_dash, void *ctx);
typedef void (*morse_key_letter_cb_t)(char letter, void *ctx); /* '?' if the sequence isn't a real code */
typedef void (*morse_key_space_cb_t)(void *ctx);
typedef void (*morse_key_gate_cb_t)(bool key_down, void *ctx); /* debounced press/release edges, for a sidetone */

/* Progress of a mode-switch BOOT hold. FIRED is not reported here -- it
 * arrives as on_mode_switch instead. */
typedef enum {
    MORSE_KEY_HOLD_ARMED,   /* held past MORSE_KEY_MODE_HINT_MS: show "keep holding..." */
    MORSE_KEY_HOLD_ABORTED, /* released after ARMED but before the switch: restore the view */
} morse_key_hold_evt_t;

typedef void (*morse_key_mode_cb_t)(void *ctx);                        /* a mode switch just happened */
typedef void (*morse_key_hold_cb_t)(morse_key_hold_evt_t evt, void *ctx);

typedef struct {
    morse_key_symbol_cb_t on_symbol;
    morse_key_letter_cb_t on_letter;
    morse_key_space_cb_t  on_space;
    morse_key_gate_cb_t   on_gate; /* fires on every debounced key-down/up edge; classification is unaffected */
    /* Fired from the key task once it has reset morse_tree and its own
     * letter/word timing -- the caller just flips its own mode + repaints.
     * Triggered by a full MORSE_KEY_MODE_HOLD_MS BOOT hold or by
     * morse_key_request_mode_toggle(). */
    morse_key_mode_cb_t   on_mode_switch;
    morse_key_hold_cb_t   on_hold_hint; /* ARMED / ABORTED progress of a BOOT mode-hold */
    void *ctx;
} morse_key_cfg_t;

/* Configures GPIO0, starts the key task. Call once, after
 * morse_tree_init() and lcd_ui_init(). Any callback may be NULL. */
esp_err_t morse_key_init(const morse_key_cfg_t *cfg);

/* Ask for a Training/Practice mode switch. Safe to call from any task
 * (e.g. the LVGL button handler) -- it just sets a flag that the key task
 * acts on at the top of its next poll, so morse_tree keeps its single
 * writer. Fires on_mode_switch when it lands. */
void morse_key_request_mode_toggle(void);

#ifdef __cplusplus
}
#endif

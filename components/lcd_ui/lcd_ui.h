#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The trainer's on-screen UI on the Waveshare ESP32-S3-Touch-LCD-3.49
 * (V2 hardware) -- AXS15231B QSPI panel + capacitive touch + LVGL v8.
 *
 * The panel/touch/LVGL bring-up in lcd_ui.c is copied almost verbatim
 * from the sibling project's components/lcd_display/lcd_display.c, which
 * is where all the "screen stays dark" / byte-swap / QSPI-chunking /
 * touch-axis-mapping quirks for this exact board were worked out. Only
 * the scene (build_scene) and the public API differ.
 *
 * Portrait 172 x 640, top to bottom:
 *   - big drawn dot/dash shapes for the sequence being keyed
 *   - big decoded-letter glyph (flashes green on commit)
 *   - running committed output (wraps, trims from the front)
 *   - a divider
 *   - autocomplete list: which letters/digits are still reachable
 *   - a full-width tap-key zone (short press = dot, long = dash)
 *
 * Every setter is safe to call from any task and never blocks on LVGL:
 * it stashes the wanted state under a short mutex and a repeating LVGL
 * timer paints it from the LVGL task a frame later. (An earlier version
 * took LVGL's lock directly and lost the race against the touch handler
 * mid-tap, leaving the dot/dash strip stale when keying on-screen.)
 */

/* Two behaviours share one screen:
 *   TRAINING -- the original free-form keyer: drawn dot/dash strip, the
 *               decoded letter, the running output line, the autocomplete
 *               list.
 *   PRACTICE -- a target word (length 1..6) the user keys in full; each
 *               committed letter is scored green/right or red/wrong in
 *               place, with a running score. A length picker sits above
 *               the word.
 * The symbol strip, the big decoded-letter glyph and the tap-key zone are
 * shown in both. */
typedef enum {
    LCD_UI_MODE_TRAINING,
    LCD_UI_MODE_PRACTICE,
} lcd_ui_mode_t;

/* On-screen controls the UI can't act on by itself -- it hands them back
 * to main. Both fire from the LVGL task; keep the handlers short (they
 * typically just poke morse_key / other lcd_ui setters, which is fine). */
typedef struct {
    void (*on_mode_btn)(void *ctx);            /* the MODE bar was tapped */
    void (*on_length_btn)(int len, void *ctx); /* a length button 1..6 was tapped */
    void *ctx;
} lcd_ui_cfg_t;

/* `cfg` (and any field in it) may be NULL. */
esp_err_t lcd_ui_init(const lcd_ui_cfg_t *cfg);

/* Switch which widget set is on screen and update the MODE bar caption. */
void lcd_ui_set_mode(lcd_ui_mode_t mode);

/* Highlight length button `len` (1..6) in the Practice picker. */
void lcd_ui_set_length(int len);

/* Practice: show `word` (uppercase) as the target, all glyphs "pending",
 * score cleared. */
void lcd_ui_practice_set_target(const char *word);

/* Practice: mark the glyph at `index` (0-based) correct (green) or wrong
 * (red). */
void lcd_ui_practice_mark(int index, bool correct);

/* Practice: set the status / score line under the target word. */
void lcd_ui_practice_set_status(const char *text);

/* Show the dot/dash sequence entered so far for the current letter, as an
 * ASCII string of '.' and '-'. Rendered as big drawn shapes -- a filled
 * circle per dot, a filled bar per dash. Pass "" to clear it. */
void lcd_ui_set_symbols(const char *ascii_dotdash);

/* Set the big centre glyph. 0 blanks it. No flash. */
void lcd_ui_set_big_letter(char c);

/* Update the three "current sequence" widgets together in one shot: the
 * drawn dot/dash strip, the big letter (0 = blank, no flash), and the
 * autocomplete list. Cheaper and glitch-free versus three separate
 * setters -- use this for the per-symbol refresh. */
void lcd_ui_show(const char *symbols, char letter, const char *candidates);

/* Set the big centre glyph and flash it green briefly (the physical
 * card's "letter lights up"). 0 is treated as '?'. */
void lcd_ui_flash_letter(char c);

/* Append one character to the running output line. */
void lcd_ui_append_output(char c);

/* Replace the autocomplete list (multi-line; see
 * morse_tree_render_candidates). Pass "" to clear it. */
void lcd_ui_set_candidates(const char *multiline);

/* Current state of the on-screen tap-key zone: true while it is held.
 * Polled by morse_key alongside the BOOT button so both feed one
 * short/long-press classifier. Returns false if touch never came up. */
bool lcd_ui_key_is_down(void);

#ifdef __cplusplus
}
#endif

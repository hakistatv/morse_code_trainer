#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Practice-oscillator sidetone for the Morse trainer.
 *
 * Routes a keyed sine through the board's onboard ES8311 DAC -> amplifier
 * -> the "MX1.25 2PIN Speaker Header" on the BACK of the Waveshare
 * ESP32-S3-Touch-LCD-3.49. There is no speaker soldered on the board --
 * plug a small 8ohm speaker into that header to hear anything.
 *
 * The tone is GATED, not blipped: it sounds for exactly as long as the
 * key is held (sidetone_gate(true) on the debounced key-down edge,
 * sidetone_gate(false) on key-up), the way a real straight-key practice
 * oscillator does, so a learner can hear their own dot/dash rhythm. Each
 * edge gets a short raised-cosine ramp so it doesn't click.
 *
 * The ES8311's control bus is the shared "ESP I2C" (SDA47/SCL48) that
 * lcd_ui already brings up for the TCA9554 expander; sidetone_init()
 * reuses that bus if it exists (call it after lcd_ui_init()) and creates
 * it otherwise. If codec bring-up fails the module disables itself
 * (logged, not fatal) and every call below becomes a no-op.
 */
esp_err_t sidetone_init(void);

/* Key the tone on (true) / off (false). Safe to call from any task; the
 * ramp happens on the sidetone task within a few ms. No-op while the
 * module is disabled or muted. */
void sidetone_gate(bool on);

/* Mute/unmute without tearing down the codec: muted == every gate(true)
 * is ignored and any sounding tone is ramped out. Default: enabled. */
void sidetone_set_enabled(bool enabled);
bool sidetone_is_enabled(void);

/* ES8311 master volume, 0..100 (clamped). Applied on the next tone.
 * Default 80. */
void sidetone_set_volume(int pct);

/* Tone pitch in Hz, clamped to 100..4000. Default 620. */
void sidetone_set_freq(int hz);

/* Short non-blocking feedback jingle through the same DAC path: a rising
 * three-note arpeggio for SUCCESS, a falling one for FAIL. Queued on the
 * sidetone task and played when the key is not currently held; ignored
 * while the module is disabled/muted. Safe to call from any task. Does
 * not disturb sidetone_set_freq() -- the keyed tone keeps its pitch. */
typedef enum {
    SIDETONE_CUE_SUCCESS,
    SIDETONE_CUE_FAIL,
} sidetone_cue_t;

void sidetone_cue(sidetone_cue_t kind);

#ifdef __cplusplus
}
#endif

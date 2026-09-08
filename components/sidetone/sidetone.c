/*
 * Gated sidetone through the onboard ES8311 DAC on the Waveshare
 * ESP32-S3-Touch-LCD-3.49.
 *
 * esp_codec_dev + raw i2s_std, output (DAC) direction only. The codec
 * bring-up mirrors the sibling project's morse_code/components/morse_player
 * (same esp_codec_dev 1.6.x, same ESP_CODEC_DEV_WORK_MODE_DAC path, proven
 * on ES8311) -- what changes here is the board pins and that this plays a
 * continuously-gated tone instead of synthesizing a whole Morse message.
 *
 * Board wiring, from Waveshare's own ESP-IDF demo for this exact board
 * (github.com/waveshareteam/ESP32-S3-Touch-LCD-3.49,
 * Examples/ESP-IDF/08_Audio_Test/components/codec_board/board_cfg.txt,
 * board id "S3_LCD_3_49"):
 *
 *   i2c:  {sda: 47, scl: 48}
 *   i2s:  {mclk: 7, bclk: 15, ws: 46, din: 6, dout: 45}
 *   out:  {codec: ES8311, pa: -1, pa_gain: 6, use_mclk: 1}
 *
 * "pa: -1" -> this board exposes no software speaker-amp enable pin (the
 * amp on the MX1.25 header is hard-enabled), and unlike the ePaper boards
 * it has no separate codec power-rail GPIO either. So there is nothing to
 * toggle around open()/close(); the codec is opened once and left open.
 *
 * din (GPIO6, the ES7210 mic) is untouched -- the trainer only plays.
 */

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es8311_codec.h"
#include "sidetone.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "sidetone";

/* --- Board wiring (see file header) --- */
#define AUDIO_I2C_PORT      I2C_NUM_0   /* shared with the TCA9554 that lcd_ui owns */
#define AUDIO_I2C_SDA_PIN   47
#define AUDIO_I2C_SCL_PIN   48
#define AUDIO_I2S_MCLK_PIN  7
#define AUDIO_I2S_BCLK_PIN  15
#define AUDIO_I2S_WS_PIN    46
#define AUDIO_I2S_DOUT_PIN  45
#define AUDIO_PA_GAIN_DB    6.0f        /* board_cfg.txt "pa_gain: 6" */

/* --- Playback format --- */
/* 16 kHz is ample for a ~600 Hz sine; 16-bit. Stereo: esp_codec_dev's
 * ES8311 path opens with channel = 2 (a mono value written to both L/R) --
 * matching morse_player and the IDF i2s_es8311 example. */
#define SAMPLE_RATE_HZ      16000
#define BITS_PER_SAMPLE     16
#define CHANNELS            2

/* The tone is rendered and written in CHUNK_MS slices; a gate-off is
 * noticed at the next slice boundary, so this also bounds key-up latency. */
#define CHUNK_MS            10
#define CHUNK_FRAMES        (SAMPLE_RATE_HZ * CHUNK_MS / 1000)   /* 160 */
/* Raised-cosine fade on each gate edge -- kills the click a hard on/off of
 * a sine makes. 4 ms is inaudible as a slope but well above the ~1.6 ms
 * period of a 620 Hz tone. */
#define EDGE_RAMP_MS        4
#define RAMP_FRAMES         (SAMPLE_RATE_HZ * EDGE_RAMP_MS / 1000) /* 64 */

#define VOLUME_DEFAULT      80
#define FREQ_DEFAULT_HZ     620
#define FREQ_MIN_HZ         100
#define FREQ_MAX_HZ         4000
#define TONE_AMPLITUDE      0.9f  /* fraction of full scale; headroom for the ramp */

static esp_codec_dev_handle_t s_dev;          /* NULL => module disabled */
static TaskHandle_t           s_task;
static volatile bool          s_gate;         /* key currently held */
static volatile bool          s_enabled = true;
static volatile int           s_freq_hz  = FREQ_DEFAULT_HZ;
static volatile int           s_req_vol  = VOLUME_DEFAULT;
static int                    s_cur_vol  = -1; /* last value pushed to the codec */
static volatile int           s_cue_req  = -1; /* sidetone_cue_t pending, or -1 */

enum ramp { RAMP_NONE, RAMP_IN, RAMP_OUT };

/* Fills `dst` with `frames` stereo frames of a sine at `freq_hz`, advancing
 * *phase (radians, wrapped) so slices join without a discontinuity. */
static void render_chunk(int16_t *dst, int frames, double *phase, enum ramp r, int freq_hz)
{
    const double w = 2.0 * M_PI * (double)freq_hz / (double)SAMPLE_RATE_HZ;
    const float peak = TONE_AMPLITUDE * 32767.0f;

    for (int n = 0; n < frames; n++) {
        float env = 1.0f;
        if (r == RAMP_IN && n < RAMP_FRAMES) {
            env = 0.5f * (1.0f - cosf((float)M_PI * (float)n / (float)RAMP_FRAMES));
        } else if (r == RAMP_OUT && n >= frames - RAMP_FRAMES) {
            env = 0.5f * (1.0f - cosf((float)M_PI * (float)(frames - 1 - n) / (float)RAMP_FRAMES));
        }
        int16_t v = (int16_t)lrintf(sinf((float)*phase) * env * peak);
        dst[2 * n] = v;
        dst[2 * n + 1] = v;
        *phase += w;
        if (*phase >= 2.0 * M_PI) {
            *phase -= 2.0 * M_PI;
        }
    }
}

static void apply_pending_volume(void)
{
    int want = s_req_vol;
    if (want != s_cur_vol) {
        esp_codec_dev_set_out_vol(s_dev, want);
        s_cur_vol = want;
    }
}

/* A short fixed arpeggio played straight to the codec (not gated). Runs on
 * the sidetone task, so it never overlaps a keyed tone. */
static void play_cue(int16_t *buf, sidetone_cue_t kind)
{
    static const int RISE[3] = { 620, 830, 1240 };  /* success: up */
    static const int FALL[3] = { 990, 660, 440 };   /* fail: down */
    const int *notes  = (kind == SIDETONE_CUE_FAIL) ? FALL : RISE;
    const int note_ms = (kind == SIDETONE_CUE_FAIL) ? 150 : 100;
    const int chunks  = note_ms / CHUNK_MS;
    const int bytes   = CHUNK_FRAMES * CHANNELS * (int)sizeof(int16_t);

    apply_pending_volume();
    for (int i = 0; i < 3; i++) {
        double phase = 0.0;
        for (int c = 0; c < chunks; c++) {
            enum ramp r = (c == 0) ? RAMP_IN : (c == chunks - 1) ? RAMP_OUT : RAMP_NONE;
            render_chunk(buf, CHUNK_FRAMES, &phase, r, notes[i]);
            if (esp_codec_dev_write(s_dev, buf, bytes) != ESP_CODEC_DEV_OK) {
                return;
            }
        }
        if (kind == SIDETONE_CUE_FAIL) {
            memset(buf, 0, (size_t)bytes); /* a beat of silence between the falling notes */
            esp_codec_dev_write(s_dev, buf, bytes);
        }
    }
}

/* One tone per gate-on: ramp in, sustain while the key stays down and the
 * module stays enabled, then ramp out. Blocks between tones. Also plays a
 * queued success/fail cue when the key is idle. */
static void sidetone_task(void *arg)
{
    (void)arg;
    int16_t *buf = malloc((size_t)CHUNK_FRAMES * CHANNELS * sizeof(int16_t));
    if (!buf) {
        ESP_LOGE(TAG, "no memory for the chunk buffer");
        vTaskDelete(NULL);
        return;
    }

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int cue = s_cue_req;
        if (cue >= 0 && !s_gate && s_enabled && s_dev) {
            s_cue_req = -1;
            play_cue(buf, (sidetone_cue_t)cue);
            continue;
        }
        if (!s_gate || !s_enabled) {
            continue;
        }

        apply_pending_volume();
        double phase = 0.0;
        bool first = true;
        while (s_gate && s_enabled) {
            render_chunk(buf, CHUNK_FRAMES, &phase, first ? RAMP_IN : RAMP_NONE, s_freq_hz);
            first = false;
            if (esp_codec_dev_write(s_dev, buf, CHUNK_FRAMES * CHANNELS * (int)sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "codec write failed -- dropping this tone");
                break;
            }
            apply_pending_volume();
        }
        /* Fade the tail out even if we never fully faded in (very short tap). */
        render_chunk(buf, CHUNK_FRAMES, &phase, RAMP_OUT, s_freq_hz);
        esp_codec_dev_write(s_dev, buf, CHUNK_FRAMES * CHANNELS * (int)sizeof(int16_t));
    }
}

static esp_err_t get_shared_i2c_bus(i2c_master_bus_handle_t *out)
{
    if (i2c_master_get_bus_handle(AUDIO_I2C_PORT, out) == ESP_OK && *out) {
        ESP_LOGI(TAG, "reusing the shared I2C bus on port %d", AUDIO_I2C_PORT);
        return ESP_OK;
    }
    const i2c_master_bus_config_t cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = AUDIO_I2C_PORT,
        .scl_io_num = AUDIO_I2C_SCL_PIN,
        .sda_io_num = AUDIO_I2C_SDA_PIN,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t sidetone_init(void)
{
    i2c_master_bus_handle_t i2c_bus = NULL;
    esp_err_t err = get_shared_i2c_bus(&i2c_bus);
    if (err != ESP_OK) {
        return err;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true; /* feed zeros (silence) whenever we're not writing */
    i2s_chan_handle_t tx_handle = NULL;
    err = i2s_new_channel(&chan_cfg, &tx_handle, NULL); /* TX only */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCLK_PIN,
            .bclk = AUDIO_I2S_BCLK_PIN,
            .ws = AUDIO_I2S_WS_PIN,
            .dout = AUDIO_I2S_DOUT_PIN,
            .din = I2S_GPIO_UNUSED,
        },
    };
    err = i2s_channel_init_std_mode(tx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    /* esp_codec_dev's I2S data interface owns enable/disable of the channel
     * around open()/close() -- don't call i2s_channel_enable() here. */
    audio_codec_i2s_cfg_t data_cfg = { .port = I2S_NUM_0, .tx_handle = tx_handle };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&data_cfg);

    audio_codec_i2c_cfg_t ctrl_cfg = {
        .port = AUDIO_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&ctrl_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = -1,          /* board_cfg.txt "pa: -1" -- no amp-enable GPIO */
        .pa_reverted = false,
        .use_mclk = true,
        .master_mode = false,  /* the ESP32 drives BCLK/WS/MCLK */
        .hw_gain.pa_gain = AUDIO_PA_GAIN_DB,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!data_if || !ctrl_if || !gpio_if || !codec_if) {
        ESP_LOGE(TAG, "failed to build one or more ES8311 interfaces");
        return ESP_FAIL;
    }

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SAMPLE_RATE_HZ,
        .channel = CHANNELS,
        .channel_mask = 0x03,
        .bits_per_sample = BITS_PER_SAMPLE,
    };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed");
        s_dev = NULL;
        return ESP_FAIL;
    }
    esp_codec_dev_set_out_vol(s_dev, s_req_vol);
    s_cur_vol = s_req_vol;

    if (xTaskCreate(sidetone_task, "sidetone", 4096, NULL, 6, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(sidetone) failed");
        esp_codec_dev_close(s_dev);
        s_dev = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ready: ES8311 @ %d Hz, tone %d Hz, vol %d%% (plug a speaker into the back MX1.25 header)",
             SAMPLE_RATE_HZ, s_freq_hz, s_req_vol);
    return ESP_OK;
}

void sidetone_gate(bool on)
{
    if (!s_dev) {
        return;
    }
    if (on && !s_enabled) {
        return;
    }
    s_gate = on;
    if (on) {
        xTaskNotifyGive(s_task); /* wake the task to start the tone */
    }
}

void sidetone_set_enabled(bool enabled)
{
    s_enabled = enabled;
    if (!enabled) {
        s_gate = false; /* task ramps the current tone out at the next slice */
    }
    ESP_LOGI(TAG, "%s", enabled ? "unmuted" : "muted");
}

bool sidetone_is_enabled(void)
{
    return s_enabled;
}

void sidetone_set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_req_vol = pct; /* picked up by the task on the next tone */
    ESP_LOGI(TAG, "volume -> %d%% (applies on the next tone)", pct);
}

void sidetone_set_freq(int hz)
{
    if (hz < FREQ_MIN_HZ) hz = FREQ_MIN_HZ;
    if (hz > FREQ_MAX_HZ) hz = FREQ_MAX_HZ;
    s_freq_hz = hz;
}

void sidetone_cue(sidetone_cue_t kind)
{
    if (!s_dev || !s_enabled) {
        return;
    }
    s_cue_req = (int)kind;
    xTaskNotifyGive(s_task); /* played by the task once the key is idle */
}

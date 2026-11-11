/* Continuous 16 kHz capture with energy VAD and utterance segmentation.
 *
 * The Whistle model is not a streaming model: it transcribes one clip of up to
 * 30 s. Continuous listening therefore means "find the speech, cut it into
 * utterances, transcribe each one", which is what this file does. The mic task
 * never waits for inference: it fills one of three PSRAM slots and pushes it
 * to the worker queue, then goes back to listening.
 *
 * VAD is a 20 ms RMS gate with an adaptive noise floor, 60 ms onset, 500 ms
 * hangover, 300 ms pre-roll and 200 ms tail. It is deliberately simple; the
 * ESP32-P4 board's ES8311 has no hardware VAD, and the model's own silence
 * check is a post-hoc gate, not a real-time detector.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "board_p4.h"
#include "audio_p4.h"
#include "es8311_p4.h"

static const char *TAG = "audio_p4";

#define FRAME_MS       20
#define FRAME_SAMPLES  (P4_SAMPLE_RATE / 1000 * FRAME_MS)          /* 320 */
#define PRE_SAMPLES    (P4_SAMPLE_RATE / 1000 * P4_VAD_PREROLL_MS) /* 4800 */
#define START_FRAMES   (P4_VAD_START_MS / FRAME_MS)                /* 3 */
#define END_FRAMES     (P4_VAD_END_MS / FRAME_MS)                  /* 25 */
#define TAIL_SAMPLES   (P4_SAMPLE_RATE / 1000 * P4_VAD_TAIL_MS)    /* 3200 */
#define MIN_SAMPLES    (P4_SAMPLE_RATE / 1000 * P4_VAD_MIN_MS)     /* 4800 */
#define FLOOR_MIN_DB   (-80.0f)
#define FLOOR_MAX_DB   (-30.0f)
#define FLOOR_FRAMES   50          /* 1 s of 20 ms frames */

static i2s_chan_handle_t s_rx;
static QueueHandle_t     s_out_q;
static QueueHandle_t     s_free_q;
static audio_utt_t       s_pool[P4_POOL_SLOTS];
static uint32_t          s_slot_samples;
static volatile uint32_t s_dropped;

/* pre-roll ring, 300 ms */
static int16_t  s_pre[PRE_SAMPLES];
static uint32_t s_pre_w;

/* VAD state */
static float   s_floor_db = -60.0f;
static float   s_floor_hist[FLOOR_FRAMES];
static int     s_seen;
static int     s_above, s_below, s_speech;
static bool    s_active;
static bool    s_skip_pre;
static audio_utt_t *s_cur;
static uint32_t     s_last_voice_sample;

static void pre_push(const int16_t *x, int n)
{
    for (int i = 0; i < n; i++) {
        s_pre[s_pre_w] = x[i];
        if (++s_pre_w == PRE_SAMPLES)
            s_pre_w = 0;
    }
}

/* Copy the ring oldest -> newest, tracking the peak across the pre-roll. The
 * pre-roll can contain the loudest part of a short word; if its peak were not
 * counted, the quiet-utterance gate could reject real speech. */
static void pre_copy(int16_t *dst, uint32_t *peak)
{
    uint32_t r = s_pre_w;
    for (uint32_t i = 0; i < PRE_SAMPLES; i++) {
        int16_t v = s_pre[r];
        dst[i] = v;
        int a = v < 0 ? -v : v;
        if ((uint32_t)a > *peak)
            *peak = (uint32_t)a;
        if (++r == PRE_SAMPLES)
            r = 0;
    }
}

static void slot_put(audio_utt_t *u)
{
    if (u)
        xQueueSend(s_free_q, &u, 0);
}

/* A slot, if necessary by discarding the oldest queued utterance. */
static audio_utt_t *slot_get(void)
{
    audio_utt_t *u = NULL;
    if (xQueueReceive(s_free_q, &u, 0) == pdTRUE)
        return u;

    audio_utt_t *old = NULL;
    if (xQueueReceive(s_out_q, &old, 0) == pdTRUE) {
        s_dropped++;
        printf("EVT drop pool_full\n");
        slot_put(old);
        if (xQueueReceive(s_free_q, &u, 0) == pdTRUE)
            return u;
    }
    return NULL;
}

static void enqueue(audio_utt_t *u)
{
    if (xQueueSend(s_out_q, &u, 0) == pdTRUE)
        return;

    /* Queue full: drop the oldest, then retry. */
    audio_utt_t *old = NULL;
    if (xQueueReceive(s_out_q, &old, 0) == pdTRUE) {
        s_dropped++;
        printf("EVT drop queue_full\n");
        slot_put(old);
    }
    if (xQueueSend(s_out_q, &u, 0) != pdTRUE) {
        s_dropped++;
        slot_put(u);
    }
}

static void end_utterance(int64_t now, bool forced)
{
    audio_utt_t *u = s_cur;
    s_cur = NULL;
    s_active = false;
    s_above = s_below = 0;
    if (!u)
        return;

    if (!forced) {
        uint32_t keep = s_last_voice_sample + TAIL_SAMPLES;
        if (u->n > keep)
            u->n = keep;
    }

    if (u->n < MIN_SAMPLES) {
        slot_put(u);
        s_speech = 0;
        return;
    }

    u->t_end_us = now;
    float peak_db = u->peak ? 20.0f * log10f((float)u->peak / 32768.0f) : -120.0f;

    /* A quiet room puts the adaptive floor near -77 dBFS, and any small
     * transient (a cup, a chair) then clears floor+9 dB. Real speech in the
     * same room peaks well above -45 dBFS; noise clips do not. Rejecting on
     * the utterance peak, not the onset RMS, keeps the model from turning
     * those transients into "Thank you." and "you". */
    if (peak_db < P4_VAD_MIN_PEAK_DB) {
        printf("EVT reject quiet ms=%u peak_dbfs=%.1f\n",
               (unsigned)(u->n / (P4_SAMPLE_RATE / 1000)), peak_db);
        slot_put(u);
        s_speech = 0;
        return;
    }

    printf("EVT speech_end ms=%u peak_dbfs=%.1f clipped=%u\n",
           (unsigned)(u->n / (P4_SAMPLE_RATE / 1000)), peak_db, (unsigned)u->clipped);
    enqueue(u);
    s_speech = 0;
}

static void process_frame(const int16_t *x, int n, int64_t now)
{
    double sq = 0.0;
    int    peak = 0, clip = 0;
    for (int i = 0; i < n; i++) {
        int v = x[i];
        sq += (double)v * (double)v;
        int a = v < 0 ? -v : v;
        if (a > peak)
            peak = a;
        if (a >= 32767)
            clip++;
    }
    float rms_db = 10.0f * log10f((float)(sq / n) / (32768.0f * 32768.0f) + 1e-12f);

    pre_push(x, n);

    /* Noise floor: the 20th percentile of the first second, then a slow
     * tracker while no speech is active. A minimum would latch onto one quiet
     * gap and sit at FLOOR_MIN_DB forever, which makes room noise look like
     * speech. */
    if (s_seen < FLOOR_FRAMES) {
        s_floor_hist[s_seen] = rms_db;
        if (s_seen == FLOOR_FRAMES - 1) {
            float tmp[FLOOR_FRAMES];
            memcpy(tmp, s_floor_hist, sizeof(tmp));
            for (int i = 1; i < FLOOR_FRAMES; i++) {
                float v = tmp[i];
                int   j = i;
                for (; j > 0 && tmp[j - 1] > v; j--)
                    tmp[j] = tmp[j - 1];
                tmp[j] = v;
            }
            s_floor_db = tmp[FLOOR_FRAMES / 5];       /* 20th percentile */
        }
        s_seen++;
        return;                    /* 1 s warm-up: do not gate speech yet */
    }
    if (!s_active) {
        float a = (rms_db < s_floor_db) ? 0.10f : 0.03f;
        s_floor_db += a * (rms_db - s_floor_db);
    }
    if (s_floor_db < FLOOR_MIN_DB) s_floor_db = FLOOR_MIN_DB;
    if (s_floor_db > FLOOR_MAX_DB) s_floor_db = FLOOR_MAX_DB;
    s_seen++;

    if (!s_active) {
        if (rms_db > s_floor_db + P4_VAD_START_DB && rms_db > P4_VAD_MIN_RMS_DB)
            s_above++;
        else
            s_above = 0;

        if (s_above >= START_FRAMES) {
            audio_utt_t *u = slot_get();
            if (!u) {
                s_dropped++;
                s_above = 0;
                return;
            }
            u->n = 0;
            u->peak = (uint32_t)peak;
            u->clipped = (uint32_t)clip;
            u->t_start_us = now - (int64_t)PRE_SAMPLES * 1000000 / P4_SAMPLE_RATE;
            u->t_end_us = 0;

            if (!s_skip_pre && u->cap >= PRE_SAMPLES) {
                pre_copy(u->pcm, &u->peak);
                u->n = PRE_SAMPLES;
            }
            s_skip_pre = false;

            s_cur = u;
            s_active = true;
            s_below = 0;
            s_speech = START_FRAMES;
            s_last_voice_sample = u->n;
            s_above = 0;
            printf("EVT speech_start floor_dbfs=%.1f rms_dbfs=%.1f\n", s_floor_db, rms_db);
            return;                 /* this frame is already in the pre-roll */
        }
        return;
    }

    if (s_cur->n + (uint32_t)n > s_cur->cap) {
        /* 30 s reached: close it and start the next utterance on this frame. */
        end_utterance(now, true);
        s_skip_pre = true;
        audio_utt_t *u = slot_get();
        if (!u) {
            s_dropped++;
            return;
        }
        u->n = (uint32_t)n;
        u->peak = (uint32_t)peak;
        u->clipped = (uint32_t)clip;
        u->t_start_us = now;
        u->t_end_us = 0;
        memcpy(u->pcm, x, (size_t)n * sizeof(int16_t));
        s_cur = u;
        s_active = true;
        s_below = 0;
        s_speech = 1;
        s_last_voice_sample = u->n;
        s_skip_pre = false;
        return;
    }

    memcpy(s_cur->pcm + s_cur->n, x, (size_t)n * sizeof(int16_t));
    s_cur->n += (uint32_t)n;
    if ((uint32_t)peak > s_cur->peak)
        s_cur->peak = (uint32_t)peak;
    s_cur->clipped += (uint32_t)clip;

    if (rms_db > s_floor_db + P4_VAD_END_DB) {
        s_below = 0;
        s_speech++;
        s_last_voice_sample = s_cur->n;
    } else {
        s_below++;
    }

    if (s_below >= END_FRAMES)
        end_utterance(now, false);
}

static void audio_task(void *arg)
{
    int16_t raw[FRAME_SAMPLES * 2];      /* stereo interleaved */
    int16_t mono[FRAME_SAMPLES];
    (void)arg;

    for (;;) {
        size_t got = 0;
        esp_err_t e = i2s_channel_read(s_rx, raw, sizeof(raw), &got, portMAX_DELAY);
        if (e != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int frames = (int)(got / (2 * sizeof(int16_t)));
        if (frames <= 0)
            continue;
        if (frames > FRAME_SAMPLES)
            frames = FRAME_SAMPLES;
        /* ES8311 register 0x44 = 0x08 puts the ADC on both slots; left is fine. */
        for (int i = 0; i < frames; i++)
            mono[i] = raw[2 * i];
        process_frame(mono, frames, esp_timer_get_time());
    }
}

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 480;       /* 30 ms at 16 kHz */
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx), TAG, "i2s channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(P4_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)P4_I2S_MCLK,
            .bclk = (gpio_num_t)P4_I2S_BCLK,
            .ws   = (gpio_num_t)P4_I2S_WS,
            .dout = (gpio_num_t)P4_I2S_DOUT,
            .din  = (gpio_num_t)P4_I2S_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "i2s std mode");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "i2s enable");
    return ESP_OK;
}

esp_err_t audio_p4_init(QueueHandle_t out_q)
{
    s_out_q = out_q;
    s_free_q = xQueueCreate(P4_POOL_SLOTS, sizeof(audio_utt_t *));
    if (!s_free_q)
        return ESP_ERR_NO_MEM;

    uint32_t sec = P4_MAX_UTT_SEC;
    for (;;) {
        bool ok = true;
        for (int i = 0; i < P4_POOL_SLOTS; i++) {
            s_pool[i].pcm = (int16_t *)heap_caps_malloc(
                (size_t)sec * P4_SAMPLE_RATE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
            if (!s_pool[i].pcm) {
                ok = false;
                ESP_LOGW(TAG, "no PSRAM for %u s slot %d", (unsigned)sec, i);
            }
        }
        if (ok)
            break;
        for (int i = 0; i < P4_POOL_SLOTS; i++) {
            if (s_pool[i].pcm) {
                heap_caps_free(s_pool[i].pcm);
                s_pool[i].pcm = NULL;
            }
        }
        if (sec <= P4_FALLBACK_SEC) {
            ESP_LOGE(TAG, "no PSRAM for the audio pool");
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGW(TAG, "falling back to %u s utterances", (unsigned)P4_FALLBACK_SEC);
        sec = P4_FALLBACK_SEC;
    }

    s_slot_samples = sec * P4_SAMPLE_RATE;
    for (int i = 0; i < P4_POOL_SLOTS; i++) {
        s_pool[i].cap = s_slot_samples;
        s_pool[i].n = 0;
        audio_utt_t *p = &s_pool[i];
        xQueueSend(s_free_q, &p, 0);
    }

    gpio_config_t pa = {
        .pin_bit_mask = 1ULL << P4_PA_CTRL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&pa), TAG, "PA gpio");
    gpio_set_level((gpio_num_t)P4_PA_CTRL, 0);      /* speaker amp off */

    /* I2S first: the codec needs MCLK while it is being configured. */
    ESP_RETURN_ON_ERROR(i2s_init(), TAG, "i2s");
    ESP_RETURN_ON_ERROR(es8311_p4_init(), TAG, "es8311");
    ESP_RETURN_ON_ERROR(es8311_p4_set_adc_gain(P4_MIC_ADC_GAIN), TAG, "mic gain");

    printf("EVT audio max_sec=%u slots=%d preroll_ms=%d vad_start_db=%.1f vad_end_db=%.1f\n",
           (unsigned)sec, P4_POOL_SLOTS, P4_VAD_PREROLL_MS, P4_VAD_START_DB, P4_VAD_END_DB);
    return ESP_OK;
}

void audio_p4_start(void)
{
    xTaskCreatePinnedToCore(audio_task, "audio_p4", 8192, NULL, 7, NULL, 0);
}

void audio_p4_release(audio_utt_t *u)
{
    slot_put(u);
}

uint32_t audio_p4_dropped(void)
{
    return s_dropped;
}

uint32_t audio_p4_max_sec(void)
{
    return s_slot_samples / P4_SAMPLE_RATE;
}

/* Whistle continuous transcription on the Waveshare ESP32-P4-WIFI6.
 *
 * No button: the microphone is always on, an energy VAD cuts speech into
 * utterances, and each utterance is written to the SD card as a WAV and
 * transcribed with Whistle (depth 4 by default). The S3 demo in
 * esp32/whistle_demo is untouched; this app shares only the portable engine.
 *
 *   audio task (core 0, prio 7) --utterance--> queue --> worker (core 0, prio 5)
 *        I2S/ES8311                3 slots         WAV + Whistle + JSONL
 *
 * The engine's row matmuls use the second core through nd_parallel_rows, so
 * the worker owns a helper task on core 1.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "nd_quant.h"
#include "nd_whistle.h"

#include "audio_p4.h"
#include "board_p4.h"
#include "sd_log.h"

static const char *const LANGS[] = { "en", "de", "fr", "es", "it", "nl", "pl" };

static nd_whistle    s_w;
static QueueHandle_t s_q;

static void *psram_alloc(size_t n)
{
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

/* The only P4-specific arithmetic in the engine. Compare the assembly kernel
 * with the same integer sum in C before trusting a transcript to it; the
 * vectors are 16-byte aligned because esp.vld.128.ip requires it. */
extern int32_t nd_p4_dot128_s8(const int8_t *a, const int8_t *b);

static bool dot_selftest(void)
{
    static int8_t a[128] __attribute__((aligned(16)));
    static int8_t b[128] __attribute__((aligned(16)));
    int32_t ref = 0;

    for (int i = 0; i < 128; i++) {
        a[i] = (int8_t)(i * 7 - 64);
        b[i] = (int8_t)(i * 13 + 5);
        ref += (int32_t)a[i] * (int32_t)b[i];
    }
    int32_t got = nd_p4_dot128_s8(a, b);
    if (got != ref) {
        printf("ERR pie_dot ref=%ld got=%ld\n", (long)ref, (long)got);
        return false;
    }
    printf("EVT pie_dot ok sum=%ld\n", (long)ref);
    return true;
}

/* The engine splits every matvec across two cores. */
static SemaphoreHandle_t s_go, s_done;
static nd_row_fn         s_fn;
static void             *s_ctx;
static uint32_t          s_r0, s_r1;

static void worker_rows(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        s_fn(s_ctx, s_r0, s_r1);
        xSemaphoreGive(s_done);
    }
}

static void rows_dual_core(nd_row_fn fn, void *ctx, uint32_t nrows)
{
    uint32_t half = nrows / 2;
    if (half < 2 || !s_go) {
        fn(ctx, 0, nrows);
        return;
    }
    s_fn = fn;
    s_ctx = ctx;
    s_r0 = half;
    s_r1 = nrows;
    xSemaphoreGive(s_go);
    fn(ctx, 0, half);
    xSemaphoreTake(s_done, portMAX_DELAY);
}

static void rows_start(void)
{
    s_go = xSemaphoreCreateBinary();
    s_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(worker_rows, "nd_rows", 16384, NULL,
                            configMAX_PRIORITIES - 2, NULL, 1);
    nd_parallel_rows = rows_dual_core;
}

static bool model_load(void)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "model");
    if (!part) {
        printf("ERR no_model_partition\n");
        return false;
    }

    esp_partition_mmap_handle_t handle;
    const void *blob = NULL;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &blob, &handle) != ESP_OK) {
        printf("ERR mmap_failed size=%u\n", (unsigned)part->size);
        return false;
    }

    int rc = nd_whistle_open(&s_w, blob, part->size);
    if (rc != 0) {
        printf("ERR whistle_open rc=%d (flash whistle.cact to 0x%06x)\n",
               rc, (unsigned)part->address);
        return false;
    }
    if (nd_whistle_set_depth(&s_w, P4_DECODER_DEPTH) != 0) {
        printf("ERR set_depth %d\n", P4_DECODER_DEPTH);
        return false;
    }

    int64_t t = esp_timer_get_time();
    size_t  head = nd_whistle_cache_head(&s_w, psram_alloc);
    printf("EVT head_in_psram bytes=%u ms=%.0f\n", (unsigned)head,
           (esp_timer_get_time() - t) / 1000.0);

    printf("EVT ready model=whistle depth=%u enc_layers=%u d_model=%u vocab=%u "
           "psram_free=%u internal_free=%u\n",
           (unsigned)s_w.depth, (unsigned)s_w.enc_layers, (unsigned)s_w.d,
           (unsigned)s_w.vocab,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

static void handle_utterance(audio_utt_t *u, float *x)
{
    char     wav_name[32] = { 0 };
    uint32_t seq = 0;

    /* Save the audio before inference: if the model or power fails, the WAV
     * is already on the card. */
    if (sd_log_ready()) {
        uint64_t freeb = sd_log_free_bytes();
        if (freeb == 0 || freeb > 50ULL * 1024 * 1024) {
            seq = sd_log_next_seq();
            if (sd_log_write_wav(u, seq, wav_name, sizeof(wav_name)) != ESP_OK)
                wav_name[0] = '\0';
        }
    }

    for (uint32_t i = 0; i < u->n; i++)
        x[i] = (float)u->pcm[i] / 32768.0f;

    uint32_t T = nd_whistle_frames(&s_w, u->n);
    float   *enc = (float *)psram_alloc((size_t)T * s_w.d * sizeof(float));
    if (!enc) {
        printf("ERR enc_alloc frames=%u\n", (unsigned)T);
        return;
    }

    int64_t t0 = esp_timer_get_time();
    if (nd_whistle_encode(&s_w, x, u->n, enc) != 0) {
        printf("ERR encode\n");
        heap_caps_free(enc);
        return;
    }
    int64_t t1 = esp_timer_get_time();

    uint32_t ids[320];
    char     text[2048];
    int      nt = nd_whistle_decode(&s_w, enc, T, -1, ids, 320);
    int64_t  t2 = esp_timer_get_time();
    heap_caps_free(enc);

    if (nt < 0) {
        printf("ERR decode\n");
        return;
    }
    nd_whistle_detok(&s_w, ids, (uint32_t)nt, text, sizeof(text));

    const char *lang = (s_w.lang >= 0 && s_w.lang < ND_WH_LANGS) ? LANGS[s_w.lang] : "??";
    printf("EVT lang %s\n", lang);
    printf("TXT %s\n", text);
    printf("EVT time audio_ms=%u encode_ms=%.0f decode_ms=%.0f tokens=%d peak_dbfs=%.1f\n",
           (unsigned)(u->n / (P4_SAMPLE_RATE / 1000)),
           (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, nt,
           u->peak ? 20.0f * log10f((float)u->peak / 32768.0f) : -120.0f);
    fflush(stdout);

    if (wav_name[0] && sd_log_ready()) {
        sd_log_append_json(u, wav_name, lang, text,
                           (uint32_t)((t1 - t0) / 1000),
                           (uint32_t)((t2 - t1) / 1000),
                           nt, P4_DECODER_DEPTH);
        printf("LOG %s\n", wav_name);
        fflush(stdout);
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    float *x = (float *)psram_alloc((size_t)P4_MAX_UTT_SEC * P4_SAMPLE_RATE * sizeof(float));
    if (!x) {
        printf("ERR clip_alloc\n");
        vTaskDelete(NULL);
    }

    for (;;) {
        audio_utt_t *u = NULL;
        if (xQueueReceive(s_q, &u, portMAX_DELAY) == pdTRUE) {
            handle_utterance(u, x);
            audio_p4_release(u);
        }
    }
}

void app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    s_q = xQueueCreate(P4_POOL_SLOTS, sizeof(audio_utt_t *));
    if (!s_q) {
        printf("ERR queue_alloc\n");
        return;
    }

    if (sd_log_init() != ESP_OK)
        printf("EVT sd unavailable - transcriptions will be UART only\n");
    else
        printf("EVT sd ready\n");

    if (audio_p4_init(s_q) != ESP_OK) {
        printf("ERR audio_init\n");
        return;
    }

    if (!dot_selftest()) {
        /* The assembly kernel is the only P4-specific arithmetic. If it is
         * wrong, the portable float kernels still produce correct (much
         * slower) transcripts; do not brick the logger on a bad instruction
         * encoding. */
        printf("ERR pie_dot failed - using the float kernels\n");
        nd_cq_int8 = 0;
    }

    rows_start();
    if (!model_load())
        return;

    xTaskCreatePinnedToCore(worker_task, "whistle", 64 * 1024, NULL, 5, NULL, 0);

    printf("\nEVT ==================================================\n");
    printf("EVT  LISTENING - speak; each utterance is transcribed\n");
    printf("EVT  max=%u s  depth=%d  VAD start=%.0f dB above floor\n",
           (unsigned)audio_p4_max_sec(), P4_DECODER_DEPTH, P4_VAD_START_DB);
    printf("EVT ==================================================\n\n");
    fflush(stdout);

    audio_p4_start();
}

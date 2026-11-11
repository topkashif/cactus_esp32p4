/* SD card audio + transcript log for the ESP32-P4-WIFI6.
 *
 * The card is the board's TF slot on SDMMC slot 0, 4-bit, with the IO power
 * domain from the P4's on-chip LDO channel 4. If 4-bit or the LDO fails the
 * mount is retried 1-bit and/or without the LDO, because a logger with no card
 * is still useful over UART.
 *
 * Files:
 *   /sdcard/rec_00001.wav        16 kHz mono PCM16, playable anywhere
 *   /sdcard/transcripts.jsonl    one object per utterance, appended
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include "board_p4.h"
#include "sd_log.h"

static const char *TAG = "sd_log";
#define MOUNT_POINT "/sdcard"

static sdmmc_card_t      *s_card;
static sd_pwr_ctrl_handle_t s_pwr;
static bool               s_mounted;
static nvs_handle_t       s_nvs;
static bool               s_nvs_ok;
static uint32_t           s_fallback_seq;

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static esp_err_t mount_try(int width, int freq_khz)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = freq_khz;
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = width;
    slot.clk = (gpio_num_t)P4_SD_CLK;
    slot.cmd = (gpio_num_t)P4_SD_CMD;
    slot.d0  = (gpio_num_t)P4_SD_D0;
    slot.d1  = (gpio_num_t)P4_SD_D1;
    slot.d2  = (gpio_num_t)P4_SD_D2;
    slot.d3  = (gpio_num_t)P4_SD_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,      /* never touch the user's card */
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    return esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mc, &s_card);
}

esp_err_t sd_log_init(void)
{
    if (s_nvs_ok == false) {
        if (nvs_open("whistle", NVS_READWRITE, &s_nvs) == ESP_OK)
            s_nvs_ok = true;
    }

    sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = P4_SD_LDO_CH };
    if (sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr) != ESP_OK) {
        ESP_LOGW(TAG, "on-chip LDO %d unavailable; trying the card without it", P4_SD_LDO_CH);
        s_pwr = NULL;
    }

    /* 1-bit at 10 MHz first: this board's slot is 4-bit capable, but the
     * extra data lines at 20 MHz were returning timeouts after the first
     * write on a 30 GB card. The logger only needs 32 KB/s, so 1-bit/10 MHz
     * is plenty and much more forgiving of the card and the wiring. */
    esp_err_t e = mount_try(1, 10000);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "1-bit/10 MHz mount failed (%s); retrying 4-bit/10 MHz",
                 esp_err_to_name(e));
        e = mount_try(4, 10000);
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "no SD card: %s (audio will only go to UART)", esp_err_to_name(e));
        if (s_pwr) {
            sd_pwr_ctrl_del_on_chip_ldo(s_pwr);
            s_pwr = NULL;
        }
        return e;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total, &freeb) == ESP_OK)
        printf("EVT sd mounted total_mb=%llu free_mb=%llu\n",
               (unsigned long long)(total / (1024 * 1024)),
               (unsigned long long)(freeb / (1024 * 1024)));
    return ESP_OK;
}

bool sd_log_ready(void)
{
    return s_mounted;
}

uint32_t sd_log_next_seq(void)
{
    uint32_t v = 0;
    if (s_nvs_ok) {
        if (nvs_get_u32(s_nvs, "seq", &v) != ESP_OK)
            v = 0;
        v++;
        if (nvs_set_u32(s_nvs, "seq", v) == ESP_OK)
            nvs_commit(s_nvs);
        return v;
    }
    return ++s_fallback_seq;
}

esp_err_t sd_log_write_wav(const audio_utt_t *u, uint32_t seq,
                           char *wav_name, size_t name_cap)
{
    if (!s_mounted)
        return ESP_ERR_INVALID_STATE;

    snprintf(wav_name, name_cap, "rec_%05u.wav", (unsigned)seq);
    char path[96];
    snprintf(path, sizeof(path), MOUNT_POINT "/%s", wav_name);

    uint32_t data_bytes = u->n * 2;
    uint32_t riff_bytes = 36 + data_bytes;
    uint8_t  hdr[44] = { 0 };

    memcpy(hdr + 0, "RIFF", 4);
    put32(hdr + 4, riff_bytes);
    memcpy(hdr + 8, "WAVE", 4);
    memcpy(hdr + 12, "fmt ", 4);
    put32(hdr + 16, 16);                    /* PCM header size */
    put16(hdr + 20, 1);                     /* PCM */
    put16(hdr + 22, 1);                     /* mono */
    put32(hdr + 24, P4_SAMPLE_RATE);
    put32(hdr + 28, P4_SAMPLE_RATE * 2);    /* byte rate */
    put16(hdr + 32, 2);                     /* block align */
    put16(hdr + 34, 16);                    /* bits */
    memcpy(hdr + 36, "data", 4);
    put32(hdr + 40, data_bytes);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open %s failed", path);
        return ESP_FAIL;
    }
    size_t w = fwrite(hdr, 1, sizeof(hdr), f);
    w += fwrite(u->pcm, sizeof(int16_t), u->n, f);
    if (fclose(f) != 0 || w != sizeof(hdr) + (size_t)u->n)
        return ESP_FAIL;
    return ESP_OK;
}

static void json_escape(const char *in, char *out, size_t cap)
{
    size_t j = 0;
    for (; *in && j + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c == '\n') {
            out[j++] = '\\'; out[j++] = 'n';
        } else if (c == '\r') {
            out[j++] = '\\'; out[j++] = 'r';
        } else if (c == '\t') {
            out[j++] = '\\'; out[j++] = 't';
        } else if (c < 0x20) {
            snprintf(out + j, cap - j, "\\u%04x", c);
            j += 6;
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
}

esp_err_t sd_log_append_json(const audio_utt_t *u, const char *wav_name,
                             const char *lang, const char *text,
                             uint32_t enc_ms, uint32_t dec_ms, int tokens,
                             int depth)
{
    if (!s_mounted)
        return ESP_ERR_INVALID_STATE;

    char esc[2200];
    json_escape(text, esc, sizeof(esc));

    char path[96];
    snprintf(path, sizeof(path), MOUNT_POINT "/transcripts.jsonl");
    FILE *f = fopen(path, "a");
    if (!f)
        return ESP_FAIL;

    float peak_db = u->peak ? 20.0f * log10f((float)u->peak / 32768.0f) : -120.0f;
    int n = fprintf(f,
        "{\"schema\":1,\"file\":\"%s\",\"uptime_ms\":%lld,\"lang\":\"%s\","
        "\"text\":\"%s\",\"audio_ms\":%u,\"peak_dbfs\":%.1f,\"clipped\":%u,"
        "\"encode_ms\":%lu,\"decode_ms\":%lu,\"tokens\":%d,\"depth\":%d}\n",
        wav_name, (long long)(u->t_start_us / 1000), lang, esc,
        (unsigned)(u->n / (P4_SAMPLE_RATE / 1000)), peak_db, (unsigned)u->clipped,
        (unsigned long)enc_ms, (unsigned long)dec_ms, tokens, depth);
    fflush(f);
    fclose(f);
    return n > 0 ? ESP_OK : ESP_FAIL;
}

uint64_t sd_log_free_bytes(void)
{
    uint64_t total = 0, freeb = 0;
    if (!s_mounted || esp_vfs_fat_info(MOUNT_POINT, &total, &freeb) != ESP_OK)
        return 0;
    return freeb;
}

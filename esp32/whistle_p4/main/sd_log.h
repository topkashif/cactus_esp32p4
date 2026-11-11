#ifndef SD_LOG_H
#define SD_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "audio_p4.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mount the TF card at /sdcard (SDMMC 4-bit, LDO channel 4). Failure is not
 * fatal: the app keeps transcribing to the serial console. */
esp_err_t sd_log_init(void);
bool      sd_log_ready(void);

/* Monotonic file number kept in NVS, starting at 1. */
uint32_t  sd_log_next_seq(void);

/* Write one 16 kHz mono PCM16 WAV. `wav_name` receives the basename. */
esp_err_t sd_log_write_wav(const audio_utt_t *u, uint32_t seq,
                           char *wav_name, size_t name_cap);

/* Append one JSON object to /sdcard/transcripts.jsonl. */
esp_err_t sd_log_append_json(const audio_utt_t *u, const char *wav_name,
                             const char *lang, const char *text,
                             uint32_t enc_ms, uint32_t dec_ms, int tokens,
                             int depth);

uint64_t  sd_log_free_bytes(void);

#ifdef __cplusplus
}
#endif
#endif /* SD_LOG_H */

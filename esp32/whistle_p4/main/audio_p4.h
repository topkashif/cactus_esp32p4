#ifndef AUDIO_P4_H
#define AUDIO_P4_H

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One VAD-segmented utterance. The PCM is 16 kHz mono int16 in PSRAM. */
typedef struct {
    int16_t *pcm;
    uint32_t n;             /* samples */
    uint32_t cap;           /* samples the slot can hold */
    int64_t  t_start_us;    /* esp_timer time of the first sample */
    int64_t  t_end_us;      /* esp_timer time of the end decision */
    uint32_t peak;          /* largest |sample| seen */
    uint32_t clipped;       /* samples at +-32767 */
} audio_utt_t;

/* Allocate the utterance pool (30 s, falling back to 15 s), configure the
 * ES8311 and I2S RX, but do not start the capture task yet. `out_q` receives
 * audio_utt_t* pointers; the consumer must call audio_p4_release(). */
esp_err_t audio_p4_init(QueueHandle_t out_q);

/* Start the continuous capture/VAD task (core 0, priority 7). */
void audio_p4_start(void);

/* Return a consumed utterance slot to the pool. */
void audio_p4_release(audio_utt_t *u);

uint32_t audio_p4_dropped(void);
uint32_t audio_p4_max_sec(void);

#ifdef __cplusplus
}
#endif
#endif /* AUDIO_P4_H */

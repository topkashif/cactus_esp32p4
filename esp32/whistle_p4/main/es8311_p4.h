#ifndef ES8311_P4_H
#define ES8311_P4_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the onboard ES8311 for 16 kHz, 16-bit, I2S slave, analog mic,
 * ADC data on both slots. Returns ESP_OK when every register write and the
 * read-back check pass. */
esp_err_t es8311_p4_init(void);

/* ADC digital gain step: 0 = 0 dB, 1 = 6 dB, ... 7 = 42 dB. The analog PGA is
 * already at +30 dB in the init sequence. */
esp_err_t es8311_p4_set_adc_gain(int step);

bool es8311_p4_ready(void);

#ifdef __cplusplus
}
#endif
#endif /* ES8311_P4_H */

/* Board facts for the Waveshare ESP32-P4-WIFI6 (basic kit).
 *
 * Everything here was checked against the board schematic and Waveshare's
 * ESP32-P4-Platform examples:
 *   I2C  SDA=7  SCL=8          ES8311 codec, address 0x18
 *   I2S  MCLK=13 BCLK=12 WS=10 DIN=11 DOUT=9   (codec is I2S slave)
 *   PA   GPIO53                NS4150B amplifier enable, active high
 *   SD   CLK=43 CMD=44 D0..D3=39,40,41,42      SDMMC 4-bit, LDO ch 4
 *   BOOT GPIO0                 strapping pin, readable at run time
 *   UART0 TX=37 RX=38          CH343 bridge on the Type-C connector
 */
#ifndef BOARD_P4_H
#define BOARD_P4_H

#define P4_I2C_SDA        7
#define P4_I2C_SCL        8
#define P4_I2S_MCLK       13
#define P4_I2S_BCLK       12
#define P4_I2S_WS         10
#define P4_I2S_DIN        11
#define P4_I2S_DOUT       9
#define P4_PA_CTRL        53

#define P4_SD_CLK         43
#define P4_SD_CMD         44
#define P4_SD_D0          39
#define P4_SD_D1          40
#define P4_SD_D2          41
#define P4_SD_D3          42
#define P4_SD_LDO_CH      4

#define P4_SAMPLE_RATE    16000
#define P4_MAX_UTT_SEC    30      /* try 30 s first; the app falls back to 15 */
#define P4_FALLBACK_SEC   15
#define P4_POOL_SLOTS     6       /* utterances waiting for / in inference */
#define P4_DECODER_DEPTH  4       /* Whistle ladder depth; 8 is the full model */
#define P4_MIC_ADC_GAIN   2       /* ES8311 reg 0x16: 0=0 dB, 2=12 dB, ... 7=42 dB */
#define P4_VAD_START_DB   9.0f
#define P4_VAD_END_DB     5.0f
#define P4_VAD_START_MS   60
#define P4_VAD_END_MS     500
#define P4_VAD_PREROLL_MS 300
#define P4_VAD_TAIL_MS    200
#define P4_VAD_MIN_MS     300
/* Absolute gates. The adaptive floor sits near -77 dBFS in a quiet room, so
 * floor+9 dB is still quieter than a chair creak. A frame must clear
 * MIN_RMS_DB to start, and a finished utterance must have peaked above
 * MIN_PEAK_DB, or it is discarded without reaching Whistle. */
#define P4_VAD_MIN_RMS_DB  (-65.0f)
#define P4_VAD_MIN_PEAK_DB (-45.0f)

#endif /* BOARD_P4_H */

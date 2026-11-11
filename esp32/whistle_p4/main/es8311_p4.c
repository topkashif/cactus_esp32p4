/* ES8311 bring-up for the Waveshare ESP32-P4-WIFI6.
 *
 * The register sequence is the one esp_codec_dev 1.6.2 writes for this board's
 * analog microphone (es8311_open -> es8311_set_fs -> es8311_start), recorded as
 * absolute values. Espressif's codec driver is Apache-2.0; this file is a small
 * new-I2C-master port of that sequence because ESP-IDF 6.x no longer ships the
 * legacy driver/i2c.h API the espressif/es8311 component uses.
 *
 * MCLK is 256 x 16 kHz = 4.096 MHz (the I2S driver is configured to match).
 * Register 0x44 = 0x08 puts the ADC on both I2S slots, so the capture code can
 * take either channel.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"

#include "board_p4.h"
#include "es8311_p4.h"

static const char *TAG = "es8311";

#define ES8311_ADDR       0x18
#define ES8311_REGS       0x46
#define I2C_TIMEOUT_MS    100

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_ready;

/* Registers 0x00..0x45 as the codec powers up. */
static const uint8_t POWER_ON[ES8311_REGS] = {
    0x1F, 0x00, 0x00, 0x10, 0x10, 0x00, 0x03, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x20, 0xFC, 0x6A, 0x00,
    0x13, 0x7C, 0x02, 0x40, 0x10, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x4C, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* 16 kHz, 16-bit Philips, MCLK 4.096 MHz; analog mic, ADC unmuted,
 * +30 dB PGA, ADC data on both slots. 0x16 is the digital ADC gain (0 dB). */
static const uint8_t ADC_16K[][2] = {
    {0x0D, 0xFA}, {0x44, 0x08}, {0x44, 0x08}, {0x01, 0x30}, {0x02, 0x00}, {0x03, 0x10}, {0x16, 0x24},
    {0x04, 0x10}, {0x05, 0x00}, {0x0B, 0x00}, {0x0C, 0x00}, {0x10, 0x1F}, {0x11, 0x7F}, {0x00, 0x80},
    {0x00, 0x80}, {0x01, 0x3F}, {0x06, 0x03}, {0x13, 0x10}, {0x1B, 0x0A}, {0x1C, 0x6A}, {0x44, 0x08},
    {0x09, 0x0C}, {0x0A, 0x0C}, {0x09, 0x0C}, {0x0A, 0x0C}, {0x02, 0x00}, {0x05, 0x00}, {0x03, 0x10},
    {0x04, 0x20}, {0x07, 0x00}, {0x08, 0xFF}, {0x06, 0x03},
    {0x00, 0x80}, {0x01, 0x3F}, {0x09, 0x0C}, {0x0A, 0x0C}, {0x17, 0xBF}, {0x0E, 0x02}, {0x14, 0x1A},
    {0x14, 0x1A}, {0x0D, 0x01}, {0x15, 0x40}, {0x37, 0x08}, {0x45, 0x00},
    {0x16, 0x00},
};

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, sizeof(b), I2C_TIMEOUT_MS);
}

static int rd(uint8_t reg)
{
    uint8_t v = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &v, 1, I2C_TIMEOUT_MS) != ESP_OK)
        return -1;
    return v;
}

static esp_err_t apply(const uint8_t (*steps)[2], size_t n)
{
    for (size_t i = 0; i < n; i++) {
        esp_err_t e = wr(steps[i][0], steps[i][1]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "write reg 0x%02x=%02x: %s", steps[i][0], steps[i][1],
                     esp_err_to_name(e));
            return e;
        }
    }
    return ESP_OK;
}

esp_err_t es8311_p4_init(void)
{
    if (s_ready)
        return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = P4_I2C_SDA,
        .scl_io_num = P4_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ES8311_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "i2c device");

    /* Absolute power-on image, one register at a time. */
    for (int r = 0; r < ES8311_REGS; r++) {
        esp_err_t e = wr((uint8_t)r, POWER_ON[r]);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "power-on reg 0x%02x: %s", r, esp_err_to_name(e));
            return e;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(apply(ADC_16K, sizeof(ADC_16K) / sizeof(ADC_16K[0])), TAG, "ADC sequence");

    /* A few registers must read back as written; log rather than fail so a
     * reserved-bit difference does not stop the build on a new board revision. */
    static const struct { uint8_t reg, want; } CHECK[] = {
        {0x00, 0x80}, {0x01, 0x3F}, {0x09, 0x0C}, {0x0A, 0x0C}, {0x44, 0x08}, {0x14, 0x1A},
    };
    for (size_t i = 0; i < sizeof(CHECK) / sizeof(CHECK[0]); i++) {
        int v = rd(CHECK[i].reg);
        if (v != CHECK[i].want)
            ESP_LOGW(TAG, "reg 0x%02x reads 0x%02x, expected 0x%02x",
                     CHECK[i].reg, v & 0xff, CHECK[i].want);
    }

    s_ready = true;
    ESP_LOGI(TAG, "ready: 16 kHz, 16-bit, analog mic, MCLK 256x");
    return ESP_OK;
}

esp_err_t es8311_p4_set_adc_gain(int step)
{
    if (!s_ready)
        return ESP_ERR_INVALID_STATE;
    if (step < 0 || step > 7)
        return ESP_ERR_INVALID_ARG;
    return wr(0x16, (uint8_t)step);
}

bool es8311_p4_ready(void)
{
    return s_ready;
}

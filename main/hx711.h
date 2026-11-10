#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

#define HX711_MAX_SAMPLES 32

typedef struct {
    gpio_num_t dout_pin;
    gpio_num_t sck_pin;
    uint8_t gain_pulses;
    size_t window_size;
    size_t sample_count;
    size_t next_sample;
    int32_t samples[HX711_MAX_SAMPLES];
    int64_t tare_offset;
    float scale_factor;
} hx711_t;

/**
 * Initialize an HX711. Supported gains are 32, 64, and 128.
 * The rolling-average window must be between 1 and HX711_MAX_SAMPLES.
 */
esp_err_t hx711_init(hx711_t *device, gpio_num_t dout_pin, gpio_num_t sck_pin,
                     uint8_t gain, size_t window_size);

/** Return true when the HX711 has completed a conversion. */
bool hx711_is_ready(const hx711_t *device);

/**
 * Read one conversion, waiting up to timeout_ms for readiness.
 * A timeout_ms of 0 performs a non-blocking readiness check.
 */
esp_err_t hx711_read_raw(hx711_t *device, uint32_t timeout_ms, int32_t *raw);

/** Read and add a ready conversion to the rolling average without waiting. */
esp_err_t hx711_sample(hx711_t *device);

/**
 * Set the tare offset to the average of sample_count conversions.
 * Each conversion may wait up to timeout_ms.
 */
esp_err_t hx711_tare(hx711_t *device, size_t sample_count,
                     uint32_t timeout_ms);

/** Set raw-units per displayed unit; displayed = (raw - tare_offset) / scale. */
esp_err_t hx711_set_scale(hx711_t *device, float scale_factor);

/** Get the filtered, scaled measurement. Returns ESP_ERR_INVALID_STATE if empty. */
esp_err_t hx711_get_units(const hx711_t *device, float *units);

int64_t hx711_get_tare_offset(const hx711_t *device);
void hx711_set_tare_offset(hx711_t *device, int64_t offset);

/** Put the HX711 in its low-power state or wake it up. */
esp_err_t hx711_power_down(hx711_t *device);
esp_err_t hx711_power_up(hx711_t *device);

#include "hx711.h"

#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

static portMUX_TYPE s_hx711_mux = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t read_conversion(hx711_t *device, int32_t *raw)
{
    uint32_t value = 0;

    portENTER_CRITICAL(&s_hx711_mux);
    for (unsigned int bit = 0; bit < 24; ++bit) {
        gpio_set_level(device->sck_pin, 1);
        value = (value << 1) | (uint32_t)gpio_get_level(device->dout_pin);
        gpio_set_level(device->sck_pin, 0);
    }

    for (uint8_t pulse = 0; pulse < device->gain_pulses; ++pulse) {
        gpio_set_level(device->sck_pin, 1);
        gpio_set_level(device->sck_pin, 0);
    }
    portEXIT_CRITICAL(&s_hx711_mux);

    if ((value & 0x800000U) != 0) {
        value |= 0xFF000000U;
    }
    *raw = (int32_t)value;
    return ESP_OK;
}

esp_err_t hx711_init(hx711_t *device, gpio_num_t dout_pin, gpio_num_t sck_pin,
                     uint8_t gain, size_t window_size)
{
    if (device == NULL || dout_pin < 0 || sck_pin < 0 || dout_pin == sck_pin ||
        window_size == 0 || window_size > HX711_MAX_SAMPLES) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (gain) {
    case 128:
        device->gain_pulses = 1;
        break;
    case 64:
        device->gain_pulses = 3;
        break;
    case 32:
        device->gain_pulses = 2;
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }

    gpio_config_t dout_config = {
        .pin_bit_mask = 1ULL << dout_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&dout_config);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t sck_config = {
        .pin_bit_mask = 1ULL << sck_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    err = gpio_config(&sck_config);
    if (err != ESP_OK) {
        return err;
    }

    err = gpio_set_level(sck_pin, 0);
    if (err != ESP_OK) {
        return err;
    }

    device->dout_pin = dout_pin;
    device->sck_pin = sck_pin;
    device->window_size = window_size;
    device->sample_count = 0;
    device->next_sample = 0;
    device->tare_offset = 0;
    device->scale_factor = 1.0f;
    return ESP_OK;
}

bool hx711_is_ready(const hx711_t *device)
{
    return device != NULL && gpio_get_level(device->dout_pin) == 0;
}

esp_err_t hx711_read_raw(hx711_t *device, uint32_t timeout_ms, int32_t *raw)
{
    if (device == NULL || raw == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const int64_t start_us = esp_timer_get_time();
    while (!hx711_is_ready(device)) {
        if (timeout_ms == 0 ||
            esp_timer_get_time() - start_us >= (int64_t)timeout_ms * 1000) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    return read_conversion(device, raw);
}

esp_err_t hx711_sample(hx711_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int32_t raw;
    esp_err_t err = hx711_read_raw(device, 0, &raw);
    if (err != ESP_OK) {
        return err;
    }

    device->samples[device->next_sample] = raw;
    device->next_sample = (device->next_sample + 1) % device->window_size;
    if (device->sample_count < device->window_size) {
        ++device->sample_count;
    }
    return ESP_OK;
}

esp_err_t hx711_tare(hx711_t *device, size_t sample_count,
                     uint32_t timeout_ms)
{
    if (device == NULL || sample_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    int64_t total = 0;
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t raw;
        esp_err_t err = hx711_read_raw(device, timeout_ms, &raw);
        if (err != ESP_OK) {
            return err;
        }
        total += raw;
    }

    device->tare_offset = total / (int64_t)sample_count;
    device->sample_count = 0;
    device->next_sample = 0;
    return ESP_OK;
}

esp_err_t hx711_set_scale(hx711_t *device, float scale_factor)
{
    if (device == NULL || !isfinite(scale_factor) || scale_factor == 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    device->scale_factor = scale_factor;
    return ESP_OK;
}

esp_err_t hx711_get_units(const hx711_t *device, float *units)
{
    if (device == NULL || units == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (device->sample_count == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    int64_t total = 0;
    int32_t lowest = device->samples[0];
    int32_t highest = device->samples[0];
    for (size_t i = 0; i < device->sample_count; ++i) {
        const int32_t value = device->samples[i];
        total += value;
        if (value < lowest) {
            lowest = value;
        }
        if (value > highest) {
            highest = value;
        }
    }

    size_t divisor = device->sample_count;
    if (device->sample_count >= 3) {
        total -= (int64_t)lowest + highest;
        divisor -= 2;
    }

    const float average = (float)total / (float)divisor;
    *units = (average - (float)device->tare_offset) / device->scale_factor;
    return ESP_OK;
}

int64_t hx711_get_tare_offset(const hx711_t *device)
{
    return device == NULL ? 0 : device->tare_offset;
}

void hx711_set_tare_offset(hx711_t *device, int64_t offset)
{
    if (device != NULL) {
        device->tare_offset = offset;
        device->sample_count = 0;
        device->next_sample = 0;
    }
}

esp_err_t hx711_power_down(hx711_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = gpio_set_level(device->sck_pin, 0);
    if (err != ESP_OK) {
        return err;
    }
    err = gpio_set_level(device->sck_pin, 1);
    if (err != ESP_OK) {
        return err;
    }
    esp_rom_delay_us(80);
    return ESP_OK;
}

esp_err_t hx711_power_up(hx711_t *device)
{
    if (device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = gpio_set_level(device->sck_pin, 0);
    if (err == ESP_OK) {
        vTaskDelay(1);
    }
    return err;
}

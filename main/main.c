#include <inttypes.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hx711.h"

#define HX711_DOUT_GPIO GPIO_NUM_4
#define HX711_SCK_GPIO  GPIO_NUM_5
#define HX711_GAIN      128
#define HX711_WINDOW    16
#define HX711_SCALE     704.44f
#define TARE_SAMPLES    10
#define READ_TIMEOUT_MS 2000

typedef enum {
    COMMAND_TARE,
} scale_command_t;

static const char *TAG = "espresso_sensors";
static QueueHandle_t command_queue;

static void serial_command_task(void *arg)
{
    (void)arg;
    char line[16];
    size_t length = 0;
    bool overflow = false;

    while (true) {
        const int ch = getchar();
        if (ch == EOF) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (length == 0 && !overflow) {
                continue;
            }
            line[length] = '\0';
            if (overflow) {
                printf("@ERROR,COMMAND_TOO_LONG\n");
            } else {
                for (size_t i = 0; i < length; ++i) {
                    line[i] = (char)toupper((unsigned char)line[i]);
                }
                if (strcmp(line, "TARE") == 0) {
                    const scale_command_t command = COMMAND_TARE;
                    if (xQueueSend(command_queue, &command, 0) != pdTRUE) {
                        printf("@ERROR,COMMAND_QUEUE_FULL\n");
                    }
                } else {
                    printf("@ERROR,UNKNOWN_COMMAND\n");
                }
            }
            fflush(stdout);
            length = 0;
            overflow = false;
        } else if (length < sizeof(line) - 1) {
            line[length++] = (char)ch;
        } else {
            overflow = true;
        }
    }
}

void app_main(void)
{
    hx711_t scale;
    esp_err_t err = hx711_init(&scale, HX711_DOUT_GPIO, HX711_SCK_GPIO,
                               HX711_GAIN, HX711_WINDOW);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HX711 initialization failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_ERROR_CHECK(hx711_set_scale(&scale, HX711_SCALE));

    ESP_LOGI(TAG, "Warming up HX711 before tare");
    vTaskDelay(pdMS_TO_TICKS(2000));

    err = hx711_tare(&scale, TARE_SAMPLES, READ_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HX711 tare failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Tare offset: %" PRId64, hx711_get_tare_offset(&scale));

    command_queue = xQueueCreate(2, sizeof(scale_command_t));
    if (command_queue == NULL) {
        ESP_LOGE(TAG, "Unable to create UART command queue");
        return;
    }

    if (xTaskCreate(serial_command_task, "serial_commands", 3072, NULL, 5,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Unable to start UART command task");
        vQueueDelete(command_queue);
        return;
    }

    printf("@READY\n");
    fflush(stdout);

    int64_t last_log_us = 0;
    while (true) {
        scale_command_t command;
        if (xQueueReceive(command_queue, &command, 0) == pdTRUE &&
            command == COMMAND_TARE) {
            ESP_LOGI(TAG, "Tare requested over UART");
            err = hx711_tare(&scale, TARE_SAMPLES, READ_TIMEOUT_MS);
            if (err == ESP_OK) {
                printf("@TARE,OK,%" PRId64 "\n",
                       hx711_get_tare_offset(&scale));
                ESP_LOGI(TAG, "Tare offset: %" PRId64,
                         hx711_get_tare_offset(&scale));
            } else {
                printf("@TARE,ERROR,%s\n", esp_err_to_name(err));
                ESP_LOGE(TAG, "UART tare failed: %s", esp_err_to_name(err));
            }
            fflush(stdout);
            last_log_us = esp_timer_get_time();
            continue;
        }

        err = hx711_sample(&scale);
        if (err == ESP_OK) {
            const int64_t now_us = esp_timer_get_time();
            if (now_us - last_log_us >= 500000) {
                float measurement;
                err = hx711_get_units(&scale, &measurement);
                if (err == ESP_OK) {
                    printf("@DATA,%.2f\n", measurement);
                    fflush(stdout);
                } else {
                    ESP_LOGE(TAG, "Unable to get measurement: %s",
                             esp_err_to_name(err));
                }
                last_log_us = now_us;
            }
        } else if (err != ESP_ERR_TIMEOUT) {
            ESP_LOGE(TAG, "HX711 read failed: %s", esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

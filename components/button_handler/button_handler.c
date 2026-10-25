#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "button_handler.h"

static const char *TAG = "BUTTON_HANDLER";

#define BUTTON_GPIO_NUM     GPIO_NUM_9
#define LONG_PRESS_MS       3000
#define SHORT_PRESS_MAX_MS  800

typedef struct {
    gpio_num_t  gpio;
    void      (*long_callback)(void);
    void      (*short_callback)(void);
    TaskHandle_t task_handle;
    bool         initialized;
} button_context_t;

static button_context_t s_btn = {0};

static void button_task(void *arg)
{
    button_context_t *ctx = (button_context_t *)arg;
    bool last_state  = true;   /* active-low: true = released */
    uint32_t press_start = 0;
    bool long_fired  = false;

    while (1) {
        bool cur = gpio_get_level(ctx->gpio);

        if (last_state && !cur) {
            /* falling edge — button pressed */
            press_start = xTaskGetTickCount() * portTICK_PERIOD_MS;
            long_fired  = false;
        } else if (!last_state && cur) {
            /* rising edge — button released */
            uint32_t held = xTaskGetTickCount() * portTICK_PERIOD_MS - press_start;
            if (!long_fired && held < SHORT_PRESS_MAX_MS && ctx->short_callback) {
                ESP_LOGI(TAG, "Short press %"PRIu32" ms", held);
                ctx->short_callback();
            }
        }

        /* Fire long-press while still held */
        if (!cur && !long_fired) {
            uint32_t elapsed = xTaskGetTickCount() * portTICK_PERIOD_MS - press_start;
            if (elapsed >= LONG_PRESS_MS && ctx->long_callback) {
                ESP_LOGI(TAG, "Long press %"PRIu32" ms", elapsed);
                long_fired = true;
                ctx->long_callback();
            }
        }

        last_state = cur;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void button_handler_init(void)
{
    s_btn.gpio = BUTTON_GPIO_NUM;

    gpio_config_t io_conf = {
        .pin_bit_mask  = (1ULL << s_btn.gpio),
        .mode          = GPIO_MODE_INPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    xTaskCreate(button_task, "button_task", 2048, &s_btn, 5, &s_btn.task_handle);
    s_btn.initialized = true;
    ESP_LOGI(TAG, "Button handler initialized on GPIO%d", BUTTON_GPIO_NUM);
}

void button_handler_set_callback(uint8_t button_id, void (*callback)(void))
{
    (void)button_id;
    if (s_btn.initialized)
        s_btn.long_callback = callback;
}

void button_handler_set_short_callback(void (*callback)(void))
{
    if (s_btn.initialized)
        s_btn.short_callback = callback;
}

void button_handler_deinit(void)
{
    if (s_btn.initialized && s_btn.task_handle) {
        vTaskDelete(s_btn.task_handle);
        s_btn.task_handle = NULL;
        s_btn.initialized = false;
    }
}

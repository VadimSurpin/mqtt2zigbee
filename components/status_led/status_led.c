#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_encoder.h"
#include "esp_log.h"
#include "status_led.h"

static const char *TAG = "STATUS_LED";

/* WS2812 timing at 10 MHz RMT clock (1 tick = 100 ns).
   Spec: T0H=400ns, T0L=850ns, T1H=800ns, T1L=450ns. */
#define WS2812_T0H  4   /* 400 ns */
#define WS2812_T0L  8   /* 800 ns */
#define WS2812_T1H  8   /* 800 ns */
#define WS2812_T1L  4   /* 400 ns */

/* Pixel layout: RGB byte order, MSB first (board uses RGB not GRB variant). */
typedef struct { uint8_t r, g, b; } ws2812_pixel_t;

/* Blink pattern: sequence of on/off durations in ms, 0-terminated.
   Index 0 = on phase, 1 = off, 2 = on, … alternating. */
static const uint16_t PAT_SLOW[]   = {500, 500, 0};              /* 1 Hz */
static const uint16_t PAT_FAST[]   = {100, 100, 0};              /* 5 Hz */
static const uint16_t PAT_DOUBLE[] = {150, 150, 150, 1550, 0};   /* double-pulse / 2 s */
static const uint16_t PAT_SOLID[]  = {0};                        /* solid on (special) */
static const uint16_t PAT_TRIPLE[] = {100, 100, 100, 100, 100, 1500, 0}; /* triple / 2 s */

typedef struct {
    uint8_t r, g, b;
    const uint16_t *pattern;
} led_mode_t;

static const led_mode_t s_modes[] = {
    [LED_STATE_NOT_CONFIGURED] = {   0,   0, 200, PAT_SLOW   },  /* blue   */
    [LED_STATE_WIFI_SEARCHING] = { 200, 180,   0, PAT_FAST   },  /* yellow */
    [LED_STATE_WIFI_CONNECTED] = {   0, 160, 160, PAT_DOUBLE },  /* cyan   */
    [LED_STATE_MQTT_CONNECTED] = {   0, 160,   0, PAT_SOLID  },  /* green  */
    [LED_STATE_CONN_ERROR]     = { 200,   0,   0, PAT_TRIPLE },  /* red    */
};

/* ---- RMT state ---- */
static rmt_channel_handle_t s_rmt_chan   = NULL;
static rmt_encoder_handle_t s_bytes_enc = NULL;

/* ---- Task state ---- */
static volatile led_state_t s_state = LED_STATE_NOT_CONFIGURED;
static TaskHandle_t         s_task  = NULL;

static void ws2812_set(uint8_t r, uint8_t g, uint8_t b)
{
    ws2812_pixel_t px = { .r = r, .g = g, .b = b };

    rmt_transmit_config_t tx_cfg = {
        .loop_count = 0,
        .flags.eot_level = 0,   /* output stays low after frame = reset pulse */
    };

    rmt_transmit(s_rmt_chan, s_bytes_enc, &px, sizeof(px), &tx_cfg);
    rmt_tx_wait_all_done(s_rmt_chan, 10);   /* wait ≤ 10 ms */
    vTaskDelay(pdMS_TO_TICKS(1));           /* ≥ 50 µs reset, rounded to 1 RTOS tick */
}

static void ws2812_off(void)
{
    ws2812_set(0, 0, 0);
}

static void led_task(void *arg)
{
    while (1) {
        led_state_t state     = s_state;
        const led_mode_t *mode = &s_modes[state];

        if (mode->pattern == PAT_SOLID || mode->pattern[0] == 0) {
            ws2812_set(mode->r, mode->g, mode->b);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bool on = true;
        for (int i = 0; mode->pattern[i] != 0; i++, on = !on) {
            if (on) {
                ws2812_set(mode->r, mode->g, mode->b);
            } else {
                ws2812_off();
            }

            uint32_t remaining = mode->pattern[i];
            while (remaining > 0) {
                uint32_t slice = remaining > 50 ? 50 : remaining;
                vTaskDelay(pdMS_TO_TICKS(slice));
                remaining -= slice;
                if (s_state != state) goto next_cycle;
            }
        }
        next_cycle:;
    }
}

void status_led_init(int gpio_num)
{
    /* RMT TX channel */
    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num          = gpio_num,
        .clk_src           = RMT_CLK_SRC_DEFAULT,
        .resolution_hz     = 10 * 1000 * 1000,  /* 10 MHz → 100 ns/tick */
        .mem_block_symbols = 64,
        .trans_queue_depth = 4,
        .flags.invert_out  = false,
        .flags.with_dma    = false,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &s_rmt_chan));

    /* Bytes encoder: WS2812 bit timings */
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = {
            .level0 = 1, .duration0 = WS2812_T0H,
            .level1 = 0, .duration1 = WS2812_T0L,
        },
        .bit1 = {
            .level0 = 1, .duration0 = WS2812_T1H,
            .level1 = 0, .duration1 = WS2812_T1L,
        },
        .flags.msb_first = 1,
    };
    ESP_ERROR_CHECK(rmt_new_bytes_encoder(&enc_cfg, &s_bytes_enc));

    ESP_ERROR_CHECK(rmt_enable(s_rmt_chan));

    /* Start dark */
    ws2812_off();

    xTaskCreate(led_task, "led_task", 2048, NULL, 3, &s_task);
    ESP_LOGI(TAG, "RGB LED (WS2812) initialised on GPIO%d", gpio_num);
}

void status_led_set(led_state_t state)
{
    if (s_rmt_chan == NULL) return;
    if (s_state == state) return;
    s_state = state;
    ESP_LOGI(TAG, "LED state → %d", (int)state);
}

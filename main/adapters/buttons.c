#include "buttons.h"
#include "platform/board_config.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include <stdint.h>

enum {
    BUTTON_COUNT = 4,
    REPEATABLE_BUTTON_COUNT = 2, /* UP and DOWN only. */
};

/* The task samples at 100 Hz. Debounce requires 30 ms; key repeat starts
 * after 2 s and then emits one event every 100 ms. */
#define BUTTON_POLL_PERIOD_MS 10
#define BUTTON_DEBOUNCE_SAMPLES 3
#define BUTTON_REPEAT_DELAY_TICKS (2000 / BUTTON_POLL_PERIOD_MS)
#define BUTTON_REPEAT_PERIOD_TICKS (100 / BUTTON_POLL_PERIOD_MS)

static const char *TAG = "buttons";
static uint32_t s_dropped_events;
static QueueHandle_t s_queue;

static void post_event(app_event_t event)
{
    if (xQueueSend(s_queue, &event, 0) != pdTRUE) {
        ++s_dropped_events;
        if ((s_dropped_events & 0x0fU) == 1U) {
            ESP_LOGW(TAG, "event queue full; dropped %lu event(s)",
                     (unsigned long)s_dropped_events);
        }
    }
}

static const gpio_num_t s_pins[] = {
    BOARD_BUTTON_UP_GPIO, BOARD_BUTTON_DOWN_GPIO,
    BOARD_BUTTON_SELECT_GPIO, BOARD_BUTTON_BACK_GPIO,
};
static const app_event_t s_events[] = {
    APP_EVENT_BUTTON_UP, APP_EVENT_BUTTON_DOWN,
    APP_EVENT_BUTTON_SELECT, APP_EVENT_BUTTON_BACK,
};
static const app_event_t s_repeat_events[] = {
    APP_EVENT_BUTTON_UP_REPEAT, APP_EVENT_BUTTON_DOWN_REPEAT,
};

static void button_task(void *arg)
{
    bool stable[BUTTON_COUNT];
    for (unsigned i = 0; i < BUTTON_COUNT; ++i) {
        stable[i] = gpio_get_level(s_pins[i]) != 0;
    }
    uint8_t same_count[BUTTON_COUNT] = {0};
    uint16_t held_ticks[REPEATABLE_BUTTON_COUNT] = {0};
    for (;;) {
        for (unsigned i = 0; i < BUTTON_COUNT; ++i) {
            bool raw = gpio_get_level(s_pins[i]) != 0;
            if (raw != stable[i]) {
                if (++same_count[i] >= BUTTON_DEBOUNCE_SAMPLES) {
                    stable[i] = raw;
                    same_count[i] = 0;
                    if (!raw) {
                        post_event(s_events[i]);
                        if (i < REPEATABLE_BUTTON_COUNT) held_ticks[i] = 0;
                    } else if (i < REPEATABLE_BUTTON_COUNT) {
                        held_ticks[i] = 0;
                    }
                }
            } else {
                same_count[i] = 0;
                if (!raw && i < REPEATABLE_BUTTON_COUNT) {
                    /* Repeat only UP/DOWN after a long hold. The app ignores
                     * these events outside setpoint editing mode. */
                    ++held_ticks[i];
                    if (held_ticks[i] >= BUTTON_REPEAT_DELAY_TICKS &&
                        (held_ticks[i] - BUTTON_REPEAT_DELAY_TICKS) % BUTTON_REPEAT_PERIOD_TICKS == 0) {
                        post_event(s_repeat_events[i]);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_PERIOD_MS));
    }
}

esp_err_t buttons_start(QueueHandle_t queue, bool *back_held_at_boot)
{
    if (!queue || !back_held_at_boot) return ESP_ERR_INVALID_ARG;
    s_queue = queue;
    uint64_t mask = 0;
    for (unsigned i = 0; i < BUTTON_COUNT; ++i) mask |= 1ULL << s_pins[i];
    const gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), "buttons", "GPIO setup");
    vTaskDelay(pdMS_TO_TICKS(30));
    *back_held_at_boot = gpio_get_level(BOARD_BUTTON_BACK_GPIO) == 0;
    if (xTaskCreate(button_task, "buttons", 2048, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

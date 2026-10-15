#include "application/load_app.h"
#include "domain/load_limits.h"
#include "adapters/buttons.h"
#include "adapters/display.h"
#include "adapters/ina219.h"
#include "adapters/pwm_output.h"
#include "adapters/settings_storage.h"
#include "adapters/web_server.h"
#include "platform/board_config.h"

#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DISPLAY_REFRESH_PERIOD_MS 100

typedef struct {
    load_app_t *app;
    SemaphoreHandle_t mutex;
} app_snapshot_context_t;

static bool get_app_snapshot(void *context, app_view_t *view)
{
    app_snapshot_context_t *snapshot = context;
    if (!snapshot || !view ||
        xSemaphoreTake(snapshot->mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
        return false;
    }
    load_app_get_view(snapshot->app, view);
    xSemaphoreGive(snapshot->mutex);
    return true;
}

static bool get_wifi_diagnostic_snapshot(void *context, app_view_t *view)
{
    (void)context;
    if (!view) return false;
    *view = (app_view_t){
        .mode = LOAD_MODE_CC,
        .status = APP_STATUS_NO_INPUT,
    };
    return true;
}

static void init_nvs_storage(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
#if CONFIG_LOAD_WIFI_DIAGNOSTIC_MODE
    /* Isolate Wi-Fi startup from all load hardware during radio diagnosis. */
    init_nvs_storage();
    ESP_ERROR_CHECK(web_server_start(get_wifi_diagnostic_snapshot, NULL));
    ESP_LOGI("app_main", "Wi-Fi diagnostic mode: load hardware is disabled");
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
#else
    const i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = BOARD_I2C_SDA_GPIO,
        .scl_io_num = BOARD_I2C_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    QueueHandle_t events = xQueueCreate(16, sizeof(app_event_t));
    ESP_ERROR_CHECK(events ? ESP_OK : ESP_ERR_NO_MEM);
    init_nvs_storage();
    bool back_held_at_boot = false;
    ESP_ERROR_CHECK(buttons_start(events, &back_held_at_boot));

    /* Compose adapters and application. PWM is configured to 0% before sensing. */
    ESP_ERROR_CHECK(pwm_output_init());
    ina219_bind_bus(bus);
    const load_app_ports_t ports = {
        .init_sensor = ina219_reinitialize,
        .read_measurement = ina219_read,
        .set_pwm_duty = pwm_output_set_duty,
        .load_settings = settings_storage_load,
        .save_settings = settings_storage_save,
        .set_shunt_resistance = ina219_set_shunt_resistance,
        .set_ina219_shunt_range = ina219_set_shunt_range,
        .set_pwm_frequency = pwm_output_set_frequency,
    };
    load_app_t app;
    load_app_result_t app_result = load_app_init(&app, &ports);
    ESP_ERROR_CHECK(app_result == LOAD_APP_OK ? ESP_OK : ESP_ERR_INVALID_ARG);
    if (back_held_at_boot) load_app_handle_event(&app, APP_EVENT_BOOT_SETTINGS);
    ESP_ERROR_CHECK(display_init(bus));

    SemaphoreHandle_t app_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(app_mutex ? ESP_OK : ESP_ERR_NO_MEM);
    app_snapshot_context_t snapshot = {.app = &app, .mutex = app_mutex};
    ESP_ERROR_CHECK(web_server_start(get_app_snapshot, &snapshot));

    app_view_t view;
    load_app_get_view(&app, &view);
    display_show(&view);
    int64_t last_display_us = esp_timer_get_time();
    const int64_t period_us = (int64_t)LOAD_CONTROL_PERIOD_MS * 1000;
    int64_t last_tick_us = esp_timer_get_time();
    int64_t next_tick_us = last_tick_us + period_us;
    for (;;) {
        app_event_t event;
        int64_t now_us = esp_timer_get_time();
        int64_t remaining_us = next_tick_us - now_us;
        TickType_t wait_ticks = 0;
        if (remaining_us > 0) {
            uint64_t ticks = ((uint64_t)remaining_us * configTICK_RATE_HZ +
                              999999ULL) / 1000000ULL;
            wait_ticks = ticks > 0 ? (TickType_t)ticks : 1;
        }
        bool event_received = xQueueReceive(events, &event, wait_ticks) == pdTRUE;
        if (event_received) {
            xSemaphoreTake(app_mutex, portMAX_DELAY);
            load_app_handle_event(&app, event);
            xSemaphoreGive(app_mutex);
        }
        now_us = esp_timer_get_time();
        if (now_us >= next_tick_us) {
            float elapsed_seconds = (float)(now_us - last_tick_us) / 1000000.0f;
            last_tick_us = now_us;
            int64_t missed_periods = (now_us - next_tick_us) / period_us + 1;
            next_tick_us += missed_periods * period_us;
            xSemaphoreTake(app_mutex, portMAX_DELAY);
            load_app_tick(&app, elapsed_seconds, elapsed_seconds);
            xSemaphoreGive(app_mutex);
        }
        if (event_received || now_us - last_display_us >=
                              (int64_t)DISPLAY_REFRESH_PERIOD_MS * 1000) {
            xSemaphoreTake(app_mutex, portMAX_DELAY);
            load_app_get_view(&app, &view);
            xSemaphoreGive(app_mutex);
            display_show(&view);
            last_display_us = esp_timer_get_time();
        }
    }
#endif
}

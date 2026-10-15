#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "domain/load_controller.h"

typedef enum {
    APP_EVENT_BUTTON_UP,
    APP_EVENT_BUTTON_DOWN,
    APP_EVENT_BUTTON_SELECT,
    APP_EVENT_BUTTON_BACK,
    APP_EVENT_BUTTON_UP_REPEAT,
    APP_EVENT_BUTTON_DOWN_REPEAT,
    APP_EVENT_BOOT_SETTINGS,
} app_event_t;

typedef enum {
    APP_MENU_MODE = 0,
    APP_MENU_SETPOINT,
    APP_MENU_LOAD,
    APP_MENU_CLEAR,
    APP_MENU_FAULT_RESET,
    APP_MENU_ITEM_COUNT,
    APP_MENU_SETTINGS,
} app_menu_item_t;

typedef struct {
    uint32_t shunt_microohms;
    uint32_t pwm_frequency_hz;
    uint32_t ina219_shunt_range_uv;
    uint32_t current_kp_milli;
    uint32_t current_ki_milli;
    uint32_t voltage_kp_milli;
    uint32_t voltage_ki_milli;
} load_app_settings_t;

typedef enum {
    APP_STATUS_NO_INPUT = 0,
    APP_STATUS_OFF,
    APP_STATUS_EDIT,
    APP_STATUS_RUN,
    APP_STATUS_WAITING_INPUT,
    APP_STATUS_SENSOR_ERROR,
    APP_STATUS_LIMIT,
    APP_STATUS_OUTPUT_ERROR,
    APP_STATUS_SETTINGS,
    APP_STATUS_SETTINGS_ERROR,
    APP_STATUS_COUNT,
} app_status_t;

typedef struct {
    app_menu_item_t menu_item;
    load_mode_t mode;
    app_status_t status;
    float setpoint;
    float voltage_v;
    float current_a;
    float power_w;
    double energy_wh;
    double runtime_seconds;
    uint32_t measurement_sequence;
    float duty;
    bool enabled;
    bool runtime_counting;
    bool editing;
    bool settings_mode;
    bool settings_editing;
    uint32_t shunt_microohms;
    uint32_t pwm_frequency_hz;
    uint32_t ina219_shunt_range_uv;
    uint32_t current_kp_milli;
    uint32_t current_ki_milli;
    uint32_t voltage_kp_milli;
    uint32_t voltage_ki_milli;
    unsigned settings_index;
    unsigned settings_digit_index;
} app_view_t;

typedef struct {
    /* Hardware boundary: application code uses these callbacks instead of
     * depending on ESP-IDF drivers or concrete adapter implementations. */
    bool (*init_sensor)(void);
    bool (*read_measurement)(load_measurement_t *measurement);
    bool (*set_pwm_duty)(float duty_0_to_1);
    bool (*load_settings)(load_app_settings_t *settings);
    bool (*save_settings)(const load_app_settings_t *settings);
    bool (*set_shunt_resistance)(float shunt_ohms);
    bool (*set_ina219_shunt_range)(uint32_t full_scale_uv);
    bool (*set_pwm_frequency)(uint32_t frequency_hz);
} load_app_ports_t;

typedef enum {
    LOAD_APP_OK = 0,
    LOAD_APP_INVALID_ARGUMENT,
} load_app_result_t;

typedef struct {
    /* Runtime state belongs to this application instance. Mutate it only from
     * the app task while holding the mutex shared with the web snapshot. */
    load_state_t state;
    unsigned menu_index;
    uint32_t measurement_sequence;
    bool editing;
    bool settings_mode;
    bool settings_editing;
    bool settings_error;
    unsigned settings_index;
    unsigned settings_digit_index;
    load_app_settings_t settings;
    load_app_settings_t settings_draft;
    load_app_ports_t ports;
} load_app_t;

load_app_result_t load_app_init(load_app_t *app, const load_app_ports_t *ports);
/* Apply one queued input event or one scheduled measurement/control step. */
void load_app_handle_event(load_app_t *app, app_event_t event);
void load_app_tick(load_app_t *app, float control_dt_seconds, float elapsed_seconds);
/* Copy the UI/API-facing projection; callers should not expose load_app_t. */
void load_app_get_view(const load_app_t *app, app_view_t *view);

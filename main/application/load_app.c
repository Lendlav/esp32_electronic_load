#include "load_app.h"
#include "domain/load_limits.h"

#include <string.h>

enum {
    SETTING_SHUNT = 0,
    SETTING_PWM,
    SETTING_INA219_RANGE,
    SETTING_CURRENT_KP,
    SETTING_CURRENT_KI,
    SETTING_VOLTAGE_KP,
    SETTING_VOLTAGE_KI,
    SETTING_COUNT,
    SHUNT_EDITABLE_DIGITS = 5,
    PWM_EDITABLE_DIGITS = 2,
    INA219_RANGE_EDITABLE_DIGITS = 1,
    PI_GAIN_EDITABLE_DIGITS = 4,
};

static float mode_step(load_mode_t mode)
{
    switch (mode) {
    case LOAD_MODE_CC:
    case LOAD_MODE_CP:
    case LOAD_MODE_CR:
        return 0.01f;
    case LOAD_MODE_CV:
    default:
        return 0.1f;
    }
}

/* Settings are edited as decimal digits; values use integer base units so
 * each digit can be changed without floating-point rounding or carry. */
#define DEFAULT_SHUNT_MICROOHMS  100000U
#define MIN_SHUNT_MICROOHMS      100U
#define MAX_SHUNT_MICROOHMS      200000U
#define MIN_PWM_FREQUENCY_HZ     1000U
#define MAX_PWM_FREQUENCY_HZ     40000U
#define PWM_FREQUENCY_STEP_HZ    1000U
#define INA219_RANGE_OPTION_COUNT 4U
#define MAX_PI_GAIN_MILLI 9999U

static const uint32_t s_ina219_shunt_ranges_uv[INA219_RANGE_OPTION_COUNT] = {
    40000U, 80000U, 160000U, 320000U,
};

static bool settings_valid(const load_app_settings_t *settings)
{
    return settings && settings->shunt_microohms >= MIN_SHUNT_MICROOHMS &&
           settings->shunt_microohms <= MAX_SHUNT_MICROOHMS &&
           settings->pwm_frequency_hz >= MIN_PWM_FREQUENCY_HZ &&
           settings->pwm_frequency_hz <= MAX_PWM_FREQUENCY_HZ &&
           settings->pwm_frequency_hz % PWM_FREQUENCY_STEP_HZ == 0 &&
           (settings->ina219_shunt_range_uv == 40000U ||
            settings->ina219_shunt_range_uv == 80000U ||
            settings->ina219_shunt_range_uv == 160000U ||
            settings->ina219_shunt_range_uv == 320000U) &&
           settings->current_kp_milli <= MAX_PI_GAIN_MILLI &&
           settings->current_ki_milli <= MAX_PI_GAIN_MILLI &&
           settings->voltage_kp_milli <= MAX_PI_GAIN_MILLI &&
           settings->voltage_ki_milli <= MAX_PI_GAIN_MILLI;
}

static bool apply_settings(load_app_t *app,
                           const load_app_settings_t *settings)
{
    return app->ports.set_shunt_resistance((float)settings->shunt_microohms / 1000000.0f) &&
           app->ports.set_ina219_shunt_range(settings->ina219_shunt_range_uv) &&
           app->ports.set_pwm_frequency(settings->pwm_frequency_hz) &&
           load_controller_set_pi_gains(&app->state,
               (float)settings->current_kp_milli / 1000.0f,
               (float)settings->current_ki_milli / 1000.0f,
               (float)settings->voltage_kp_milli / 1000.0f,
               (float)settings->voltage_ki_milli / 1000.0f);
}

static void move_setting_digit(load_app_t *app, bool toward_more_significant)
{
    /* SHUNT has five displayed decimals; PWM exposes the 10 kHz and 1 kHz
     * digits. Selection wraps so the controls never get stuck at an edge. */
    unsigned digit_count;
    if (app->settings_index == SETTING_SHUNT) digit_count = SHUNT_EDITABLE_DIGITS;
    else if (app->settings_index == SETTING_PWM) digit_count = PWM_EDITABLE_DIGITS;
    else if (app->settings_index == SETTING_INA219_RANGE)
        digit_count = INA219_RANGE_EDITABLE_DIGITS;
    else digit_count = PI_GAIN_EDITABLE_DIGITS;
    if (toward_more_significant) {
        app->settings_digit_index = (app->settings_digit_index + digit_count - 1U) % digit_count;
    } else {
        app->settings_digit_index = (app->settings_digit_index + 1U) % digit_count;
    }
}

static void increment_setting_digit(load_app_t *app)
{
    /* Each table entry is the integer weight of one decimal digit. Shunt is
     * stored in micro-ohms and displayed with five decimals; PWM is in Hz. */
    static const uint32_t shunt_places[] = {100000U, 10000U, 1000U, 100U, 10U};
    static const uint32_t pwm_places[] = {10000U, 1000U};
    if (app->settings_index == SETTING_INA219_RANGE) {
        unsigned current = 0;
        while (current + 1U < INA219_RANGE_OPTION_COUNT &&
               s_ina219_shunt_ranges_uv[current] != app->settings_draft.ina219_shunt_range_uv) {
            ++current;
        }
        current = (current + 1U) % INA219_RANGE_OPTION_COUNT;
        app->settings_draft.ina219_shunt_range_uv = s_ina219_shunt_ranges_uv[current];
        return;
    }
    uint32_t *gain = NULL;
    switch (app->settings_index) {
    case SETTING_CURRENT_KP: gain = &app->settings_draft.current_kp_milli; break;
    case SETTING_CURRENT_KI: gain = &app->settings_draft.current_ki_milli; break;
    case SETTING_VOLTAGE_KP: gain = &app->settings_draft.voltage_kp_milli; break;
    case SETTING_VOLTAGE_KI: gain = &app->settings_draft.voltage_ki_milli; break;
    default: break;
    }
    if (gain) {
        static const uint32_t gain_places[] = {1000U, 100U, 10U, 1U};
        uint32_t place = gain_places[app->settings_digit_index];
        uint32_t digit = (*gain / place) % 10U;
        if (digit == 9U) *gain -= 9U * place;
        else *gain += place;
        return;
    }
    uint32_t *value = app->settings_index == SETTING_SHUNT
        ? &app->settings_draft.shunt_microohms
        : &app->settings_draft.pwm_frequency_hz;
    uint32_t place = app->settings_index == SETTING_SHUNT
        ? shunt_places[app->settings_digit_index]
        : pwm_places[app->settings_digit_index];

    uint32_t digit = (*value / place) % 10U;
    /* Wrap this digit from 9 to 0 without carrying into the next digit. */
    if (digit == 9U) *value -= 9U * place;
    else *value += place;
}

static bool save_settings(load_app_t *app)
{
    if (!settings_valid(&app->settings_draft) ||
        !apply_settings(app, &app->settings_draft) ||
        !app->ports.save_settings(&app->settings_draft)) {
        (void)apply_settings(app, &app->settings);
        app->settings_error = true;
        return false;
    }
    app->settings = app->settings_draft;
    app->settings_error = false;
    app->settings_mode = false;
    app->settings_editing = false;
    return true;
}

static void move_menu_selection(load_app_t *app, bool toward_previous)
{
    if (toward_previous) {
        app->menu_index = (app->menu_index + APP_MENU_ITEM_COUNT - 1U) %
                          APP_MENU_ITEM_COUNT;
    } else {
        app->menu_index = (app->menu_index + 1U) % APP_MENU_ITEM_COUNT;
    }
}

static void clear_runtime_counters_if_disabled(load_app_t *app)
{
    if (app->state.enabled) return;
    app->state.energy_wh = 0.0;
    app->state.runtime_seconds = 0.0;
}

static void handle_settings_event(load_app_t *app, app_event_t event)
{
    switch (event) {
    case APP_EVENT_BUTTON_UP:
    case APP_EVENT_BUTTON_DOWN:
        if (app->settings_editing) {
            move_setting_digit(app, event == APP_EVENT_BUTTON_UP);
        } else {
            int direction = event == APP_EVENT_BUTTON_UP ? -1 : 1;
            app->settings_index = (app->settings_index + SETTING_COUNT + direction) %
                                  SETTING_COUNT;
        }
        break;
    case APP_EVENT_BUTTON_SELECT:
        if (app->settings_editing) {
            increment_setting_digit(app);
        } else {
            app->settings_editing = true;
            app->settings_digit_index = app->settings_index == SETTING_SHUNT
                ? SHUNT_EDITABLE_DIGITS - 1U
                : (app->settings_index == SETTING_PWM
                    ? PWM_EDITABLE_DIGITS - 1U
                    : (app->settings_index == SETTING_INA219_RANGE
                        ? INA219_RANGE_EDITABLE_DIGITS - 1U
                        : PI_GAIN_EDITABLE_DIGITS - 1U));
        }
        break;
    case APP_EVENT_BUTTON_BACK:
        if (app->settings_editing) app->settings_editing = false;
        else (void)save_settings(app);
        break;
    case APP_EVENT_BUTTON_UP_REPEAT:
        if (app->settings_editing) move_setting_digit(app, true);
        break;
    case APP_EVENT_BUTTON_DOWN_REPEAT:
        if (app->settings_editing) move_setting_digit(app, false);
        break;
    default:
        break;
    }
}

static void handle_fault_event(load_app_t *app, app_event_t event)
{
    switch (event) {
    case APP_EVENT_BUTTON_UP:
        move_menu_selection(app, true);
        break;
    case APP_EVENT_BUTTON_DOWN:
        move_menu_selection(app, false);
        break;
    case APP_EVENT_BUTTON_SELECT:
        if (app->menu_index == APP_MENU_CLEAR) {
            clear_runtime_counters_if_disabled(app);
        } else if (app->menu_index == APP_MENU_FAULT_RESET) {
            load_controller_set_enabled(&app->state, false);
            load_controller_reset_control(&app->state);
            if (app->ports.init_sensor()) app->state.fault = LOAD_FAULT_NONE;
        }
        break;
    default:
        break;
    }
}

static void handle_normal_event(load_app_t *app, app_event_t event)
{
    /* BACK is an unconditional output stop during normal operation. */
    if (event == APP_EVENT_BUTTON_BACK) {
        load_controller_set_enabled(&app->state, false);
        app->editing = false;
        app->state.fault = LOAD_FAULT_NONE;
        return;
    }

    if (app->editing) {
        float increment = mode_step(app->state.mode);
        if (event == APP_EVENT_BUTTON_UP) {
            load_controller_adjust_setpoint(&app->state, increment);
        } else if (event == APP_EVENT_BUTTON_DOWN) {
            load_controller_adjust_setpoint(&app->state, -increment);
        } else if (event == APP_EVENT_BUTTON_SELECT) {
            app->editing = false;
        }
        return;
    }

    switch (event) {
    case APP_EVENT_BUTTON_UP:
        move_menu_selection(app, true);
        break;
    case APP_EVENT_BUTTON_DOWN:
        move_menu_selection(app, false);
        break;
    case APP_EVENT_BUTTON_SELECT:
        switch (app->menu_index) {
        case APP_MENU_MODE:
            load_controller_set_mode(&app->state,
                (app->state.mode + 1U) % LOAD_MODE_COUNT);
            break;
        case APP_MENU_SETPOINT:
            app->editing = true;
            break;
        case APP_MENU_LOAD:
            load_controller_set_enabled(&app->state, !app->state.enabled);
            break;
        case APP_MENU_CLEAR:
            clear_runtime_counters_if_disabled(app);
            break;
        case APP_MENU_FAULT_RESET:
            app->state.fault = LOAD_FAULT_NONE;
            load_controller_set_enabled(&app->state, false);
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }
}

static app_status_t current_status(const load_app_t *app)
{
    if (app->settings_mode) {
        return app->settings_error ? APP_STATUS_SETTINGS_ERROR
                                   : APP_STATUS_SETTINGS;
    }

    switch (app->state.fault) {
    case LOAD_FAULT_SENSOR:
        return APP_STATUS_SENSOR_ERROR;
    case LOAD_FAULT_LIMIT:
        return APP_STATUS_LIMIT;
    case LOAD_FAULT_OUTPUT:
        return APP_STATUS_OUTPUT_ERROR;
    case LOAD_FAULT_NONE:
    default:
        break;
    }

    if (app->editing) return APP_STATUS_EDIT;
    if (app->state.enabled) {
        return app->state.measured_voltage_v < 0.5f
            ? APP_STATUS_WAITING_INPUT : APP_STATUS_RUN;
    }
    return app->state.measured_voltage_v < 0.5f
        ? APP_STATUS_NO_INPUT : APP_STATUS_OFF;
}

static app_menu_item_t current_menu_item(const load_app_t *app)
{
    if (app->settings_mode) return APP_MENU_SETTINGS;
    if (app->menu_index >= APP_MENU_ITEM_COUNT) return APP_MENU_MODE;
    return (app_menu_item_t)app->menu_index;
}

load_app_result_t load_app_init(load_app_t *app, const load_app_ports_t *ports)
{
    if (!app || !ports || !ports->init_sensor || !ports->read_measurement ||
        !ports->set_pwm_duty || !ports->load_settings || !ports->save_settings ||
        !ports->set_shunt_resistance || !ports->set_ina219_shunt_range ||
        !ports->set_pwm_frequency)
        return LOAD_APP_INVALID_ARGUMENT;
    memset(app, 0, sizeof(*app));
    app->ports = *ports;
    load_controller_init(&app->state);
    app->settings = (load_app_settings_t){
        .shunt_microohms = DEFAULT_SHUNT_MICROOHMS,
        .pwm_frequency_hz = LOAD_PWM_FREQUENCY_HZ,
        .ina219_shunt_range_uv = 320000U,
        .current_kp_milli = 300U,
        .current_ki_milli = 200U,
        .voltage_kp_milli = 300U,
        .voltage_ki_milli = 200U,
    };
    (void)app->ports.load_settings(&app->settings);
    if (!settings_valid(&app->settings)) {
        app->settings = (load_app_settings_t){
            .shunt_microohms = DEFAULT_SHUNT_MICROOHMS,
            .pwm_frequency_hz = LOAD_PWM_FREQUENCY_HZ,
            .ina219_shunt_range_uv = 320000U,
            .current_kp_milli = 300U,
            .current_ki_milli = 200U,
            .voltage_kp_milli = 300U,
            .voltage_ki_milli = 200U,
        };
    }
    app->settings_draft = app->settings;
    if (!apply_settings(app, &app->settings)) app->state.fault = LOAD_FAULT_OUTPUT;
    if (!app->ports.init_sensor()) app->state.fault = LOAD_FAULT_SENSOR;
    return LOAD_APP_OK;
}

void load_app_handle_event(load_app_t *app, app_event_t e)
{
    if (!app) return;
    if (e == APP_EVENT_BOOT_SETTINGS) {
        load_controller_set_enabled(&app->state, false);
        app->settings_mode = true;
        app->settings_editing = false;
        app->settings_error = false;
        app->settings_index = 0;
        app->settings_digit_index = 4;
        app->settings_draft = app->settings;
        return;
    }
    if (e == APP_EVENT_BUTTON_UP_REPEAT || e == APP_EVENT_BUTTON_DOWN_REPEAT) {
        if (app->settings_mode && app->settings_editing) {
            move_setting_digit(app, e == APP_EVENT_BUTTON_UP_REPEAT);
        } else if (app->editing && app->state.fault == LOAD_FAULT_NONE) {
            float increment = mode_step(app->state.mode);
            float direction = e == APP_EVENT_BUTTON_UP_REPEAT ? 1.0f : -1.0f;
            load_controller_adjust_setpoint(&app->state, direction * increment);
        }
        return;
    }
    if (app->settings_mode) {
        handle_settings_event(app, e);
        return;
    }
    if (app->state.fault != LOAD_FAULT_NONE) {
        if (e == APP_EVENT_BUTTON_BACK) {
            handle_normal_event(app, e);
        } else {
            handle_fault_event(app, e);
        }
        return;
    }
    handle_normal_event(app, e);
}

void load_app_tick(load_app_t *app, float control_dt_seconds, float elapsed_seconds)
{
    if (!app) return;
    /* One scheduled step: sample, update domain state, then apply its safe
     * output command. A failed PWM write is converted into a latched fault. */
    load_measurement_t measurement = {0};
    bool ok = app->ports.read_measurement(&measurement);
    ++app->measurement_sequence;
    measurement.valid = ok;
    load_controller_update(&app->state, &measurement, control_dt_seconds,
                           elapsed_seconds);
    if (app->state.fault != LOAD_FAULT_NONE) app->editing = false;
    float duty = app->state.enabled && app->state.fault == LOAD_FAULT_NONE
        ? app->state.duty : 0.0f;
    if (!app->ports.set_pwm_duty(duty)) {
        (void)app->ports.set_pwm_duty(0.0f);
        load_controller_latch_fault(&app->state, LOAD_FAULT_OUTPUT);
        app->editing = false;
    }
}

void load_app_get_view(const load_app_t *app, app_view_t *v)
{
    if (!app || !v) return;
    /* Keep presentation status and settings drafts in this read-only view. */
    memset(v, 0, sizeof(*v));
    v->menu_item = current_menu_item(app);
    v->mode = app->state.mode;
    v->status = current_status(app);
    v->setpoint = app->state.setpoint;
    v->voltage_v = app->state.measured_voltage_v;
    v->current_a = app->state.measured_current_a;
    v->power_w = app->state.measured_power_w;
    v->energy_wh = app->state.energy_wh;
    v->runtime_seconds = app->state.runtime_seconds;
    v->measurement_sequence = app->measurement_sequence;
    v->duty = app->state.duty;
    v->enabled = app->state.enabled;
    v->runtime_counting = app->state.runtime_counting;
    v->editing = app->editing;
    v->settings_mode = app->settings_mode;
    v->settings_editing = app->settings_editing;
    v->shunt_microohms = app->settings_mode ? app->settings_draft.shunt_microohms
                                            : app->settings.shunt_microohms;
    v->pwm_frequency_hz = app->settings_mode
        ? app->settings_draft.pwm_frequency_hz : app->settings.pwm_frequency_hz;
    v->ina219_shunt_range_uv = app->settings_mode
        ? app->settings_draft.ina219_shunt_range_uv : app->settings.ina219_shunt_range_uv;
    v->current_kp_milli = app->settings_mode
        ? app->settings_draft.current_kp_milli : app->settings.current_kp_milli;
    v->current_ki_milli = app->settings_mode
        ? app->settings_draft.current_ki_milli : app->settings.current_ki_milli;
    v->voltage_kp_milli = app->settings_mode
        ? app->settings_draft.voltage_kp_milli : app->settings.voltage_kp_milli;
    v->voltage_ki_milli = app->settings_mode
        ? app->settings_draft.voltage_ki_milli : app->settings.voltage_ki_milli;
    v->settings_index = app->settings_index;
    v->settings_digit_index = app->settings_digit_index;
}

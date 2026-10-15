#include "load_controller.h"

#include <math.h>
#include <string.h>

#include "load_limits.h"

/* Load policy owns tuning; the PI algorithm itself is unit agnostic. */
#define CURRENT_KP 0.3f
#define CURRENT_KI 0.2f
#define VOLTAGE_KP 0.3f
#define VOLTAGE_KI 0.2f

static float clampf(float value, float minimum, float maximum)
{
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

void load_controller_reset_control(load_state_t *s)
{
    if (!s) return;
    s->duty = 0.0f;
    s->runtime_counting = false;
    pi_controller_reset(&s->current_pi);
    pi_controller_reset(&s->voltage_pi);
}

static void stop_with_fault(load_state_t *s, load_fault_t fault)
{
    s->fault = fault;
    s->enabled = false;
    load_controller_reset_control(s);
}

void load_controller_latch_fault(load_state_t *s, load_fault_t fault)
{
    if (!s || fault == LOAD_FAULT_NONE) return;
    stop_with_fault(s, fault);
}

void load_controller_init(load_state_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->mode = LOAD_MODE_CC;
    s->setpoint = 0.100f;
    s->voltage_limit_v = LOAD_MAX_VOLTAGE_V;
    s->current_limit_a = LOAD_MAX_CURRENT_A;
    s->power_limit_w = LOAD_MAX_POWER_W;

    const pi_controller_config_t current_config = {
        .kp = CURRENT_KP,
        .ki = CURRENT_KI,
        .integral_min = 0.0f,
        .integral_max = LOAD_MAX_PWM_DUTY,
        .output_min = 0.0f,
        .output_max = LOAD_MAX_PWM_DUTY,
    };
    const pi_controller_config_t voltage_config = {
        .kp = VOLTAGE_KP,
        .ki = VOLTAGE_KI,
        .integral_min = 0.0f,
        .integral_max = s->current_limit_a,
        .output_min = 0.0f,
        .output_max = s->current_limit_a,
    };
    pi_controller_init(&s->current_pi, &current_config);
    pi_controller_init(&s->voltage_pi, &voltage_config);
}

bool load_controller_set_pi_gains(load_state_t *s, float current_kp,
                                  float current_ki, float voltage_kp,
                                  float voltage_ki)
{
    if (!s || !isfinite(current_kp) || !isfinite(current_ki) ||
        !isfinite(voltage_kp) || !isfinite(voltage_ki) ||
        current_kp < 0.0f || current_ki < 0.0f ||
        voltage_kp < 0.0f || voltage_ki < 0.0f) return false;

    s->current_pi.config.kp = current_kp;
    s->current_pi.config.ki = current_ki;
    s->voltage_pi.config.kp = voltage_kp;
    s->voltage_pi.config.ki = voltage_ki;
    load_controller_reset_control(s);
    return true;
}

void load_controller_set_enabled(load_state_t *s, bool enabled)
{
    if (!s) return;
    if (s->fault != LOAD_FAULT_NONE || (enabled && s->measured_voltage_v < 0.5f)) {
        enabled = false;
    }
    s->enabled = enabled;
    load_controller_reset_control(s);
}

void load_controller_set_mode(load_state_t *s, load_mode_t mode)
{
    if (!s || mode < LOAD_MODE_CC || mode >= LOAD_MODE_COUNT) return;
    s->mode = mode;
    switch (mode) {
    case LOAD_MODE_CC: s->setpoint = 0.100f; break;
    case LOAD_MODE_CV: s->setpoint = 5.000f; break;
    case LOAD_MODE_CP: s->setpoint = 1.000f; break;
    case LOAD_MODE_CR: s->setpoint = 10.00f; break;
    default: break;
    }
    load_controller_reset_control(s);
}

void load_controller_adjust_setpoint(load_state_t *s, float delta)
{
    if (!s || !isfinite(delta)) return;

    float minimum;
    float maximum;
    switch (s->mode) {
    case LOAD_MODE_CC:
        minimum = 0.01f;
        maximum = s->current_limit_a;
        break;
    case LOAD_MODE_CV:
        minimum = 0.1f;
        maximum = s->voltage_limit_v;
        break;
    case LOAD_MODE_CP:
        minimum = 0.01f;
        maximum = s->power_limit_w;
        break;
    case LOAD_MODE_CR:
        minimum = 0.01f;
        maximum = 1000.0f;
        break;
    default:
        return;
    }
    s->setpoint = clampf(s->setpoint + delta, minimum, maximum);
}

void load_controller_update(load_state_t *s, const load_measurement_t *m,
                            float control_dt, float elapsed_seconds)
{
    if (!s) return;
    s->runtime_counting = false;

    bool invalid_measurement = !m || !m->valid || !isfinite(m->voltage_v) ||
        !isfinite(m->current_a) || !isfinite(m->power_w) || m->voltage_v < 0.0f ||
        m->power_w < 0.0f;
    bool invalid_limits = !isfinite(s->voltage_limit_v) || s->voltage_limit_v <= 0.0f ||
        !isfinite(s->current_limit_a) || s->current_limit_a <= 0.0f ||
        !isfinite(s->power_limit_w) || s->power_limit_w <= 0.0f;
    bool exceeded_limit = !invalid_measurement && !invalid_limits &&
        (m->voltage_v > s->voltage_limit_v || m->current_a > s->current_limit_a ||
         m->power_w > s->power_limit_w);

    if (invalid_limits) {
        stop_with_fault(s, LOAD_FAULT_LIMIT);
        return;
    }
    if (invalid_measurement || exceeded_limit) {
        stop_with_fault(s, invalid_measurement ? LOAD_FAULT_SENSOR : LOAD_FAULT_LIMIT);
        return;
    }

    s->measured_voltage_v = m->voltage_v;
    s->measured_current_a = m->current_a < 0.0f ? 0.0f : m->current_a;
    s->measured_power_w = m->power_w;
    if (!s->enabled || s->fault != LOAD_FAULT_NONE) {
        load_controller_reset_control(s);
        return;
    }

    /* Keep the load output off when the source voltage is absent. */
    if (m->voltage_v < 0.5f) {
        load_controller_reset_control(s);
        return;
    }

    if (!isfinite(control_dt) || control_dt <= 0.0f) control_dt = 0.05f;
    control_dt = clampf(control_dt, 0.001f, 0.25f);
    if (!isfinite(elapsed_seconds) || elapsed_seconds <= 0.0f)
        elapsed_seconds = control_dt;

    if (!isfinite(s->setpoint) || s->setpoint <= 0.0f ||
        s->mode < LOAD_MODE_CC || s->mode >= LOAD_MODE_COUNT) {
        stop_with_fault(s, LOAD_FAULT_LIMIT);
        return;
    }

    if (!isfinite(s->energy_wh) || s->energy_wh < 0.0) s->energy_wh = 0.0;
    if (!isfinite(s->runtime_seconds) || s->runtime_seconds < 0.0) {
        s->runtime_seconds = 0.0;
    }
    s->runtime_counting = true;
    s->energy_wh += (double)m->power_w * (double)elapsed_seconds / 3600.0;
    s->runtime_seconds += (double)elapsed_seconds;

    /* Convert the selected load mode into a target current. */
    float target_current_a;
    switch (s->mode) {
    case LOAD_MODE_CC:
        target_current_a = s->setpoint;
        break;
    case LOAD_MODE_CV: {
        float voltage_error_v = m->voltage_v - s->setpoint;
        target_current_a = pi_controller_update(&s->voltage_pi,
                                                 voltage_error_v, control_dt);
        break;
    }
    case LOAD_MODE_CP:
        target_current_a = m->voltage_v > 0.05f
            ? s->setpoint / m->voltage_v : 0.0f;
        break;
    case LOAD_MODE_CR:
        target_current_a = m->voltage_v / s->setpoint;
        break;
    default:
        stop_with_fault(s, LOAD_FAULT_LIMIT);
        return;
    }

    target_current_a = clampf(target_current_a, 0.0f, s->current_limit_a);
    float current_error_a = target_current_a - s->measured_current_a;
    s->duty = pi_controller_update(&s->current_pi, current_error_a, control_dt);
}

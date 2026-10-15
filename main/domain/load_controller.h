#pragma once

#include <stdbool.h>
#include "pi_controller.h"

typedef enum {
    LOAD_MODE_CC = 0, /* constant current */
    LOAD_MODE_CV,     /* constant voltage */
    LOAD_MODE_CP,     /* constant power */
    LOAD_MODE_CR,     /* constant resistance */
    LOAD_MODE_COUNT
} load_mode_t;

typedef struct {
    float voltage_v;
    float current_a;
    float power_w;
    bool valid;
} load_measurement_t;

typedef enum {
    LOAD_FAULT_NONE = 0,
    LOAD_FAULT_SENSOR,
    LOAD_FAULT_LIMIT,
    LOAD_FAULT_OUTPUT,
} load_fault_t;

typedef struct {
    load_mode_t mode;
    float setpoint;
    float voltage_limit_v;
    float current_limit_a;
    float power_limit_w;
    float measured_current_a;
    float measured_voltage_v;
    float measured_power_w;
    double energy_wh;
    double runtime_seconds;
    float duty;
    pi_controller_t current_pi;
    pi_controller_t voltage_pi;
    bool enabled;
    bool runtime_counting;
    load_fault_t fault;
} load_state_t;

void load_controller_init(load_state_t *state);
bool load_controller_set_pi_gains(load_state_t *state, float current_kp,
                                  float current_ki, float voltage_kp,
                                  float voltage_ki);
void load_controller_set_enabled(load_state_t *state, bool enabled);
void load_controller_set_mode(load_state_t *state, load_mode_t mode);
void load_controller_adjust_setpoint(load_state_t *state, float delta);
void load_controller_reset_control(load_state_t *state);
void load_controller_latch_fault(load_state_t *state, load_fault_t fault);
void load_controller_update(load_state_t *state, const load_measurement_t *measurement,
                            float control_dt_seconds, float elapsed_seconds);

#pragma once

typedef struct {
    float kp;
    float ki;
    float integral_min;
    float integral_max;
    float output_min;
    float output_max;
} pi_controller_config_t;

typedef struct {
    pi_controller_config_t config;
    float integral;
    float output;
} pi_controller_t;

void pi_controller_init(pi_controller_t *controller,
                        const pi_controller_config_t *config);
void pi_controller_reset(pi_controller_t *controller);

/* Generic scalar PI: error and output units are defined by its caller. */
float pi_controller_update(pi_controller_t *controller, float error,
                           float dt_seconds);

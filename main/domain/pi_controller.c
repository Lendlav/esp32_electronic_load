#include "pi_controller.h"

#include <stdbool.h>
#include <math.h>
#include <string.h>

static float clampf(float value, float minimum, float maximum)
{
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

void pi_controller_init(pi_controller_t *controller,
                        const pi_controller_config_t *config)
{
    if (!controller) return;
    memset(controller, 0, sizeof(*controller));

    if (!config || !isfinite(config->kp) || !isfinite(config->ki) ||
        !isfinite(config->integral_min) || !isfinite(config->integral_max) ||
        !isfinite(config->output_min) || !isfinite(config->output_max) ||
        config->ki < 0.0f || config->integral_min > config->integral_max ||
        config->output_min > config->output_max) {
        return;
    }

    controller->config = *config;
    controller->integral = clampf(0.0f, config->integral_min,
                                  config->integral_max);
    controller->output = clampf(0.0f, config->output_min,
                                config->output_max);
}

void pi_controller_reset(pi_controller_t *controller)
{
    if (!controller) return;
    controller->integral = clampf(0.0f, controller->config.integral_min,
                                  controller->config.integral_max);
    controller->output = clampf(0.0f, controller->config.output_min,
                                controller->config.output_max);
}

float pi_controller_update(pi_controller_t *controller, float error,
                           float dt_seconds)
{
    if (!controller) return 0.0f;
    if (!isfinite(error) || !isfinite(dt_seconds) || dt_seconds <= 0.0f) {
        return controller->output;
    }

    const pi_controller_config_t *cfg = &controller->config;
    if (!isfinite(controller->integral)) {
        controller->integral = clampf(0.0f, cfg->integral_min,
                                      cfg->integral_max);
    }
    controller->integral = clampf(controller->integral, cfg->integral_min,
                                   cfg->integral_max);

    float proportional = cfg->kp * error;
    float candidate_integral = clampf(controller->integral +
                                          cfg->ki * error * dt_seconds,
                                      cfg->integral_min, cfg->integral_max);
    float candidate_output = proportional + candidate_integral;

    bool pushes_high = candidate_output > cfg->output_max && error > 0.0f;
    bool pushes_low = candidate_output < cfg->output_min && error < 0.0f &&
                      candidate_integral >= controller->integral;
    if (!pushes_high && !pushes_low) {
        controller->integral = candidate_integral;
    }

    controller->output = clampf(proportional + controller->integral,
                                cfg->output_min, cfg->output_max);
    return controller->output;
}

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t pwm_output_init(void);
bool pwm_output_set_duty(float duty_0_to_1);
bool pwm_output_set_frequency(uint32_t frequency_hz);

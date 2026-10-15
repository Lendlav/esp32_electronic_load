#pragma once

/* Conservative operating policy. Tune for the weakest part of the real circuit. */
#define LOAD_MAX_CURRENT_A       120.000f
#define LOAD_MAX_VOLTAGE_V       30.000f
#define LOAD_MAX_POWER_W         2000.000f
#define LOAD_MAX_PWM_DUTY        0.850f
#define LOAD_CONTROL_PERIOD_MS   50
#define LOAD_PWM_FREQUENCY_HZ    20000U

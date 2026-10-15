#include "pwm_output.h"
#include "platform/board_config.h"
#include "domain/load_limits.h"
#include "driver/ledc.h"
#include "esp_check.h"

#include <math.h>

/* Keep this value in sync with LEDC_TIMER_10_BIT below. */
#define PWM_TIMER_MAX_TICKS 1023U

bool pwm_output_set_frequency(uint32_t frequency_hz)
{
    if (frequency_hz < 1000 || frequency_hz > 40000) return false;
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = frequency_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    return ledc_timer_config(&timer) == ESP_OK;
}

esp_err_t pwm_output_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = LOAD_PWM_FREQUENCY_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), "load_pwm", "timer setup");
    const ledc_channel_config_t channel = {
        .gpio_num = BOARD_LOAD_PWM_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    return ledc_channel_config(&channel);
}

bool pwm_output_set_duty(float duty)
{
    if (!isfinite(duty) || duty < 0.0f) duty = 0.0f;
    if (duty > LOAD_MAX_PWM_DUTY) duty = LOAD_MAX_PWM_DUTY;
    /* LEDC duty is expressed in timer ticks; the domain gives a 0.0..1.0
     * fraction and the load policy already caps that fraction. */
    uint32_t ticks = (uint32_t)(duty * (float)PWM_TIMER_MAX_TICKS);
    if (ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, ticks) != ESP_OK)
        return false;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0) == ESP_OK;
}

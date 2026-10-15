#include "display.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_sh1107.h"
#include "esp_lvgl_port.h"
#include "font/lv_font.h"
#include "lv_conf_internal.h"
#include "lvgl.h"
#include "platform/board_config.h"

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define POWER_CHART_WIDTH 40
#define POWER_CHART_HEIGHT 40
#define POWER_CHART_POINTS 16
#define POWER_CHART_SCALE 100
#define POWER_CHART_MIN_RANGE_W 0.05f
#define DISPLAY_INIT_LOCK_TIMEOUT_MS 1000
#define DISPLAY_UPDATE_LOCK_TIMEOUT_MS 20

static const char *TAG = "display";

/* LVGL objects and timers are private to this adapter. Every access below is
 * made while holding the LVGL port lock. */
static uint32_t s_lock_timeouts;
static lv_obj_t *s_header;
static lv_obj_t *s_voltage;
static lv_obj_t *s_current;
static lv_obj_t *s_power;
static lv_obj_t *s_energy;
static lv_obj_t *s_runtime;
static lv_obj_t *s_power_chart;
static lv_chart_series_t *s_power_series;
static double s_runtime_seconds;
static float s_latest_power_w;
static double s_power_window_sum_w;
static uint32_t s_power_window_samples;
static uint32_t s_last_measurement_sequence;
static float s_power_samples[POWER_CHART_POINTS];
static lv_coord_t s_power_chart_values[POWER_CHART_POINTS];
static uint16_t s_power_sample_count;
static uint16_t s_power_sample_next;

/* Animation state is kept here rather than in application/domain state: it
 * changes only how the current view is drawn, never what the load does. */
static bool s_runtime_separator_visible = true;
static bool s_runtime_counting;
static lv_timer_t *s_runtime_separator_timer;
static lv_timer_t *s_power_chart_timer;
static lv_obj_t *s_menu;
static char s_last_menu_text[32];
static bool s_menu_text_initialized;
static char s_menu_base_text[32];
static unsigned s_menu_blink_char_index;
static bool s_menu_blink_active;
static bool s_menu_blink_visible = true;
static lv_timer_t *s_menu_blink_timer;

static void render_menu_blink_state(void)
{
    char text[sizeof(s_menu_base_text)];
    snprintf(text, sizeof(text), "%s", s_menu_base_text);
    size_t length = strlen(text);
    if (s_menu_blink_active && !s_menu_blink_visible &&
        s_menu_blink_char_index < length) {
        text[s_menu_blink_char_index] = ' ';
    }
    lv_label_set_text(s_menu, text);
}

static void menu_blink_timer_callback(lv_timer_t *timer)
{
    (void)timer;
    if (!s_menu_blink_active) return;
    s_menu_blink_visible = !s_menu_blink_visible;
    render_menu_blink_state();
}

static void refresh_power_chart(void)
{
    /* Keep recent samples in a ring buffer; expose them oldest-to-newest to
     * LVGL and scale the vertical axis to the largest visible sample. */
    float max_power_w = POWER_CHART_MIN_RANGE_W;
    for (uint16_t i = 0; i < s_power_sample_count; ++i) {
        if (s_power_samples[i] > max_power_w) max_power_w = s_power_samples[i];
    }

    lv_coord_t range_max = (lv_coord_t)ceilf(max_power_w * 1.2f * POWER_CHART_SCALE);
    if (range_max < 1) range_max = 1;
    lv_chart_set_range(s_power_chart, LV_CHART_AXIS_PRIMARY_Y, 0, range_max);

    for (uint16_t i = 0; i < POWER_CHART_POINTS; ++i) {
        s_power_chart_values[i] = LV_CHART_POINT_NONE;
    }
    uint16_t visible_count = s_power_sample_count;
    uint16_t first = s_power_sample_count == POWER_CHART_POINTS
        ? s_power_sample_next : 0;
    for (uint16_t i = 0; i < visible_count; ++i) {
        uint16_t sample_index = (first + i) % POWER_CHART_POINTS;
        float scaled = s_power_samples[sample_index] * POWER_CHART_SCALE;
        if (scaled > range_max) scaled = (float)range_max;
        s_power_chart_values[POWER_CHART_POINTS - visible_count + i] =
            (lv_coord_t)lroundf(scaled);
    }
    lv_chart_refresh(s_power_chart);
}

static void power_chart_timer_callback(lv_timer_t *timer)
{
    (void)timer;
    if (!s_runtime_counting || s_power_window_samples == 0) return;

    float power_w = (float)(s_power_window_sum_w / s_power_window_samples);
    s_power_samples[s_power_sample_next] = power_w;
    s_power_sample_next = (s_power_sample_next + 1) % POWER_CHART_POINTS;
    if (s_power_sample_count < POWER_CHART_POINTS) ++s_power_sample_count;
    s_power_window_sum_w = 0.0;
    s_power_window_samples = 0;
    refresh_power_chart();
}

static void reset_power_chart(void)
{
    memset(s_power_samples, 0, sizeof(s_power_samples));
    s_power_sample_count = 0;
    s_power_sample_next = 0;
    s_power_window_sum_w = 0.0;
    s_power_window_samples = 0;
    refresh_power_chart();
}

typedef struct {
    const char *name;
    const char *unit;
    int precision;
} mode_presentation_t;

typedef struct {
    char header[32];
    char voltage[32];
    char current[32];
    char power[32];
    char energy[32];
    char runtime[32];
    char menu[32];
} display_frame_t;

typedef void (*menu_formatter_t)(char *buffer, size_t size,
                                 const app_view_t *view);

static const mode_presentation_t s_mode_presentations[LOAD_MODE_COUNT] = {
    [LOAD_MODE_CC] = {"CC", "A",   2},
    [LOAD_MODE_CV] = {"CV", "V",   1},
    [LOAD_MODE_CP] = {"CP", "W",   2},
    [LOAD_MODE_CR] = {"CR", "Ohm", 2},
};

static const char *const s_status_text[APP_STATUS_COUNT] = {
    [APP_STATUS_NO_INPUT] = "NO PWR",
    [APP_STATUS_OFF] = "OFF",
    [APP_STATUS_EDIT] = "EDIT",
    [APP_STATUS_RUN] = "RUN",
    [APP_STATUS_WAITING_INPUT] = "WAIT",
    [APP_STATUS_SENSOR_ERROR] = "INA_ERR",
    [APP_STATUS_LIMIT] = "LIMIT",
    [APP_STATUS_OUTPUT_ERROR] = "PWM_ERR",
    [APP_STATUS_SETTINGS] = "SETUP",
    [APP_STATUS_SETTINGS_ERROR] = "CFG_ERR",
};

static const char *mode_name(load_mode_t mode)
{
    unsigned index = (unsigned)mode;
    return index < LOAD_MODE_COUNT
        ? s_mode_presentations[index].name
        : s_mode_presentations[LOAD_MODE_CC].name;
}

static const mode_presentation_t *mode_presentation(load_mode_t mode)
{
    unsigned index = (unsigned)mode;
    return index < LOAD_MODE_COUNT
        ? &s_mode_presentations[index]
        : &s_mode_presentations[LOAD_MODE_CC];
}

static const char *status_text(app_status_t status)
{
    unsigned index = (unsigned)status;
    return index < APP_STATUS_COUNT
        ? s_status_text[index]
        : s_status_text[APP_STATUS_OFF];
}

static void format_mode_menu(char *buffer, size_t size, const app_view_t *view)
{
    snprintf(buffer, size, ">MODE %s", mode_name(view->mode));
}

static void format_setpoint_menu(char *buffer, size_t size,
                                 const app_view_t *view)
{
    const mode_presentation_t *presentation = mode_presentation(view->mode);
    snprintf(buffer, size, ">%.*f%s%s", presentation->precision,
             (double)view->setpoint, presentation->unit,
             view->editing ? "*" : "");
}

static void format_load_menu(char *buffer, size_t size, const app_view_t *view)
{
    snprintf(buffer, size, ">LOAD %s", view->enabled ? "1" : "0");
}

static void format_fault_menu(char *buffer, size_t size,
                              const app_view_t *view)
{
    (void)view;
    snprintf(buffer, size, ">FAULT RST");
}

static void format_clear_menu(char *buffer, size_t size,
                              const app_view_t *view)
{
    snprintf(buffer, size, view->enabled ? ">CLEAR OFF" : ">CLEAR E/T");
}

static void format_settings_menu(char *buffer, size_t size,
                                 const app_view_t *view)
{
    switch (view->settings_index) {
    case 0:
        if (view->settings_editing) {
            char value[16];
            snprintf(value, sizeof(value), "%.5f",
                     (double)view->shunt_microohms / 1000000.0);
            snprintf(buffer, size, ">SH %s", value);
        } else {
            snprintf(buffer, size, ">SHUNT %.5f",
                     (double)view->shunt_microohms / 1000000.0);
        }
        break;
    case 1:
        if (view->settings_editing) {
            char value[16];
            snprintf(value, sizeof(value), "%05lu",
                     (unsigned long)view->pwm_frequency_hz);
            snprintf(buffer, size, ">PWM %s", value);
        } else {
            snprintf(buffer, size, ">PWM %luHz",
                     (unsigned long)view->pwm_frequency_hz);
        }
        break;
    case 2: {
        unsigned range_index = 0;
        switch (view->ina219_shunt_range_uv) {
        case 80000U: range_index = 1; break;
        case 160000U: range_index = 2; break;
        case 320000U: range_index = 3; break;
        default: break;
        }
        unsigned range_mv = view->ina219_shunt_range_uv / 1000U;
        if (view->settings_editing) {
            snprintf(buffer, size, ">PGA %u %umV", range_index, range_mv);
        } else {
            snprintf(buffer, size, ">PGA +/- %umV", range_mv);
        }
        break;
    }
    case 3:
        snprintf(buffer, size, ">IKP %lu.%03lu",
                 (unsigned long)(view->current_kp_milli / 1000U),
                 (unsigned long)(view->current_kp_milli % 1000U));
        break;
    case 4:
        snprintf(buffer, size, ">IKI %lu.%03lu",
                 (unsigned long)(view->current_ki_milli / 1000U),
                 (unsigned long)(view->current_ki_milli % 1000U));
        break;
    case 5:
        snprintf(buffer, size, ">VKP %lu.%03lu",
                 (unsigned long)(view->voltage_kp_milli / 1000U),
                 (unsigned long)(view->voltage_kp_milli % 1000U));
        break;
    case 6:
        snprintf(buffer, size, ">VKI %lu.%03lu",
                 (unsigned long)(view->voltage_ki_milli / 1000U),
                 (unsigned long)(view->voltage_ki_milli % 1000U));
        break;
    default:
        snprintf(buffer, size, ">SETTINGS");
        break;
    }
}

static const menu_formatter_t s_menu_formatters[APP_MENU_SETTINGS + 1] = {
    [APP_MENU_MODE] = format_mode_menu,
    [APP_MENU_SETPOINT] = format_setpoint_menu,
    [APP_MENU_LOAD] = format_load_menu,
    [APP_MENU_CLEAR] = format_clear_menu,
    [APP_MENU_FAULT_RESET] = format_fault_menu,
    [APP_MENU_SETTINGS] = format_settings_menu,
};

static void format_menu(char *buffer, size_t size, const app_view_t *view)
{
    unsigned index = (unsigned)view->menu_item;
    menu_formatter_t formatter = index <= (unsigned)APP_MENU_SETTINGS
        ? s_menu_formatters[index]
        : format_fault_menu;
    formatter(buffer, size, view);
}

static void format_runtime(char *buffer, size_t size, double runtime_seconds,
                           bool separator_visible)
{
    uint64_t total_seconds = !isfinite(runtime_seconds) || runtime_seconds <= 0.0
        ? 0
        : runtime_seconds >= (double)UINT64_MAX
            ? UINT64_MAX
            : (uint64_t)runtime_seconds;
    unsigned long long hours = (unsigned long long)(total_seconds / 3600ULL);
    unsigned minutes = (unsigned)((total_seconds / 60ULL) % 60ULL);
    unsigned seconds = (unsigned)(total_seconds % 60ULL);

    char separator = separator_visible ? ':' : ' ';
    snprintf(buffer, size, "T:%02llu%c%02u%c%02u",
             hours, separator, minutes, separator, seconds);
}

static void runtime_separator_timer_callback(lv_timer_t *timer)
{
    (void)timer;
    if (!s_runtime_counting) return;
    s_runtime_separator_visible = !s_runtime_separator_visible;
    char runtime_text[32];
    format_runtime(runtime_text, sizeof(runtime_text), s_runtime_seconds,
                   s_runtime_separator_visible);
    lv_label_set_text(s_runtime, runtime_text);
}

static void format_frame(display_frame_t *frame, const app_view_t *view)
{
    snprintf(frame->header, sizeof(frame->header), "MOD:%s #%s",
             mode_name(view->mode), status_text(view->status));
    snprintf(frame->voltage, sizeof(frame->voltage), "V:%.1fV", view->voltage_v);
    snprintf(frame->current, sizeof(frame->current), "I:%.1fA", view->current_a);
    snprintf(frame->power, sizeof(frame->power), "P:%.1fW", view->power_w);
    snprintf(frame->energy, sizeof(frame->energy), "E:%.1fWh", view->energy_wh);
    format_menu(frame->menu, sizeof(frame->menu), view);
}

esp_err_t display_init(i2c_master_bus_handle_t bus)
{
    /* Hardware setup first, then the LVGL scene and its paused timers. */
    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = BOARD_OLED_I2C_ADDRESS,
        .scl_speed_hz = 500000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags.disable_control_phase = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io), "display", "panel IO");

    esp_lcd_panel_sh1107_config_t sh_cfg = {.contrast = 128, .offset = 0};
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &sh_cfg,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_sh1107(io, &panel_cfg, &panel), "display", "SH1107 create");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), "display", "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), "display", "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, 2, 0), "display", "gap");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), "display", "on");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(panel, false), "display", "normal colors");

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), "display", "LVGL init");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = SCREEN_WIDTH * SCREEN_HEIGHT,
        .double_buffer = false,
        .hres = SCREEN_WIDTH,
        .vres = SCREEN_HEIGHT,
        .monochrome = true,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
    };
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) return ESP_FAIL;
    lv_disp_set_rotation(disp, LV_DISP_ROT_180);
    if (!lvgl_port_lock(DISPLAY_INIT_LOCK_TIMEOUT_MS)) return ESP_ERR_TIMEOUT;
    lv_obj_t *screen = lv_scr_act();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);

    s_header = lv_label_create(screen);
    lv_obj_set_style_text_font(s_header, &lv_font_unscii_8, 0);
    lv_obj_align(s_header, LV_ALIGN_TOP_LEFT, 3, 0);

    s_voltage = lv_label_create(screen);
    lv_obj_set_style_text_font(s_voltage, &lv_font_unscii_8, 0);
    lv_obj_align(s_voltage, LV_ALIGN_TOP_LEFT, 3, 10);

    s_current = lv_label_create(screen);
    lv_obj_set_style_text_font(s_current, &lv_font_unscii_8, 0);
    lv_obj_align(s_current, LV_ALIGN_TOP_LEFT, 3, 19);

    s_power = lv_label_create(screen);
    lv_obj_set_style_text_font(s_power, &lv_font_unscii_8, 0);
    lv_obj_align(s_power, LV_ALIGN_TOP_LEFT, 3, 28);

    s_energy = lv_label_create(screen);
    lv_obj_set_style_text_font(s_energy, &lv_font_unscii_8, 0);
    lv_obj_align(s_energy, LV_ALIGN_TOP_LEFT, 3, 37);

    s_runtime = lv_label_create(screen);
    lv_obj_set_style_text_font(s_runtime, &lv_font_unscii_8, 0);
    lv_obj_align(s_runtime, LV_ALIGN_TOP_LEFT, 3, 46);

    s_power_chart = lv_chart_create(screen);
    lv_obj_set_size(s_power_chart, POWER_CHART_WIDTH, POWER_CHART_HEIGHT);
    lv_obj_align(s_power_chart, LV_ALIGN_TOP_RIGHT, -2, 12);
    lv_obj_set_style_bg_opa(s_power_chart, LV_OPA_TRANSP, LV_PART_MAIN);
    /* Keep the origin sharp; the chart theme can otherwise round this corner. */
    lv_obj_set_style_radius(s_power_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_power_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(s_power_chart, LV_BORDER_SIDE_LEFT,
                                 LV_PART_MAIN);
    lv_obj_set_style_border_color(s_power_chart, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_power_chart, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_power_chart, 0, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_power_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_color(s_power_chart, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_line_opa(s_power_chart, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_line_dash_width(s_power_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_line_dash_gap(s_power_chart, 2, LV_PART_MAIN);
    lv_obj_set_style_line_width(s_power_chart, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(s_power_chart, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_width(s_power_chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_power_chart, 0, LV_PART_INDICATOR);
    lv_chart_set_type(s_power_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_power_chart, POWER_CHART_POINTS);
    lv_chart_set_update_mode(s_power_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(s_power_chart, 6, 5);
    lv_chart_set_axis_tick(s_power_chart, LV_CHART_AXIS_PRIMARY_X,
                           2, 0, 5, 0, false, 2);
    lv_chart_set_axis_tick(s_power_chart, LV_CHART_AXIS_PRIMARY_Y,
                           2, 0, 6, 0, false, 2);
    s_power_series = lv_chart_add_series(s_power_chart, lv_color_white(),
                                         LV_CHART_AXIS_PRIMARY_Y);
    if (!s_power_series) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    for (uint16_t i = 0; i < POWER_CHART_POINTS; ++i) {
        s_power_chart_values[i] = LV_CHART_POINT_NONE;
    }
    lv_chart_set_ext_y_array(s_power_chart, s_power_series, s_power_chart_values);
    lv_chart_set_range(s_power_chart, LV_CHART_AXIS_PRIMARY_Y, 0,
                       (lv_coord_t)(POWER_CHART_MIN_RANGE_W * POWER_CHART_SCALE));
    s_power_chart_timer = lv_timer_create(power_chart_timer_callback, 500, NULL);
    if (!s_power_chart_timer) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_timer_pause(s_power_chart_timer);

    s_runtime_separator_timer = lv_timer_create(runtime_separator_timer_callback,
                                                500, NULL);
    if (!s_runtime_separator_timer) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_timer_pause(s_runtime_separator_timer);

    s_menu_blink_timer = lv_timer_create(menu_blink_timer_callback, 500, NULL);
    if (!s_menu_blink_timer) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_timer_pause(s_menu_blink_timer);

    s_menu = lv_label_create(screen);
    lv_obj_set_style_text_font(s_menu, &lv_font_unscii_8, 0);
    lv_obj_align(s_menu, LV_ALIGN_BOTTOM_LEFT, 3, 0);

    lvgl_port_unlock();
    return ESP_OK;
}

void display_show(const app_view_t *v)
{
    if (!v) return;
    display_frame_t frame;
    format_frame(&frame, v);
    if (!lvgl_port_lock(DISPLAY_UPDATE_LOCK_TIMEOUT_MS)) {
        if ((++s_lock_timeouts & 0x0fU) == 1U) {
            ESP_LOGW(TAG, "LVGL lock timeout; skipped %lu display update(s)",
                     (unsigned long)s_lock_timeouts);
        }
        return;
    }

    /* The application supplies a snapshot. UI-only averaging and blinking
     * happen here, protected by the same lock as all LVGL operations. */
    s_runtime_seconds = v->runtime_seconds;
    s_latest_power_w = v->power_w;
    bool was_counting = s_runtime_counting;
    s_runtime_counting = v->runtime_counting;
    if (s_runtime_counting != was_counting) {
        s_runtime_separator_visible = true;
        if (s_runtime_counting) {
            reset_power_chart();
            /* Include this first fresh measurement in the new averaging window. */
            s_last_measurement_sequence = v->measurement_sequence - 1U;
            lv_timer_reset(s_runtime_separator_timer);
            lv_timer_resume(s_runtime_separator_timer);
            lv_timer_reset(s_power_chart_timer);
            lv_timer_resume(s_power_chart_timer);
        } else {
            lv_timer_pause(s_runtime_separator_timer);
            lv_timer_pause(s_power_chart_timer);
        }
    }
    if (s_runtime_counting) {
        if (v->measurement_sequence != s_last_measurement_sequence) {
            if (isfinite(s_latest_power_w) && s_latest_power_w > 0.0f) {
                s_power_window_sum_w += s_latest_power_w;
            }
            ++s_power_window_samples;
            s_last_measurement_sequence = v->measurement_sequence;
        }
    } else {
        s_last_measurement_sequence = v->measurement_sequence;
    }
    format_runtime(frame.runtime, sizeof(frame.runtime), s_runtime_seconds,
                   s_runtime_separator_visible);
    lv_label_set_text(s_header, frame.header);
    lv_label_set_text(s_voltage, frame.voltage);
    lv_label_set_text(s_current, frame.current);
    lv_label_set_text(s_power, frame.power);
    lv_label_set_text(s_energy, frame.energy);
    lv_label_set_text(s_runtime, frame.runtime);
    bool setpoint_changed = v->status == APP_STATUS_EDIT &&
                            s_menu_text_initialized &&
                            strcmp(s_last_menu_text, frame.menu) != 0;
    bool blink_digit = v->menu_item == APP_MENU_SETTINGS &&
                       v->settings_editing;
    if (blink_digit) {
        snprintf(s_menu_base_text, sizeof(s_menu_base_text), "%s", frame.menu);
        if (v->settings_index == 0U) {
            /* SHUNT is formatted as 0.00000; the five digits follow the dot. */
            s_menu_blink_char_index = 6U + v->settings_digit_index;
        } else if (v->settings_index == 2U) {
            /* The selected PGA option number is the editable digit. */
            s_menu_blink_char_index = 5U;
        } else if (v->settings_index == 1U) {
            /* PWM is zero-padded: index 0 selects 10 kHz, index 1 selects 1 kHz. */
            s_menu_blink_char_index = 5U + v->settings_digit_index;
        } else {
            static const unsigned gain_digit_positions[] = {5U, 7U, 8U, 9U};
            unsigned digit = v->settings_digit_index;
            if (digit >= sizeof(gain_digit_positions) / sizeof(gain_digit_positions[0]))
                digit = 0U;
            s_menu_blink_char_index = gain_digit_positions[digit];
        }
        if (!s_menu_blink_active) {
            s_menu_blink_visible = true;
            lv_timer_reset(s_menu_blink_timer);
            lv_timer_resume(s_menu_blink_timer);
        }
        s_menu_blink_active = true;
        render_menu_blink_state();
    } else {
        if (s_menu_blink_active) lv_timer_pause(s_menu_blink_timer);
        s_menu_blink_active = false;
        snprintf(s_menu_base_text, sizeof(s_menu_base_text), "%s", frame.menu);
        lv_label_set_text(s_menu, frame.menu);
    }
    snprintf(s_last_menu_text, sizeof(s_last_menu_text), "%s", frame.menu);
    s_menu_text_initialized = true;
    /* Flush each changed setpoint synchronously so repeated key events do not
     * coalesce into a later displayed value. */
    if (setpoint_changed) lv_refr_now(lv_disp_get_default());
    lvgl_port_unlock();
}

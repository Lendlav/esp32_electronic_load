#include "settings_storage.h"

#include "nvs.h"

#define SETTINGS_NAMESPACE "load_cfg"
#define KEY_SHUNT_MICROOHMS "shunt_uohm"
#define KEY_SHUNT_LEGACY_MILLIOHMS "shunt_mohm"
#define KEY_PWM_FREQUENCY "pwm_hz"
#define KEY_INA219_SHUNT_RANGE "ina219_shunt_uv"
#define KEY_CURRENT_KP_MILLI "cur_kp_milli"
#define KEY_CURRENT_KI_MILLI "cur_ki_milli"
#define KEY_VOLTAGE_KP_MILLI "volt_kp_milli"
#define KEY_VOLTAGE_KI_MILLI "volt_ki_milli"

bool settings_storage_load(load_app_settings_t *settings)
{
    if (!settings) return false;
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return true;

    uint32_t value = 0;
    esp_err_t err = nvs_get_u32(handle, KEY_SHUNT_MICROOHMS, &value);
    if (err == ESP_OK) settings->shunt_microohms = value;
    else if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = nvs_get_u32(handle, KEY_SHUNT_LEGACY_MILLIOHMS, &value);
        if (err == ESP_OK) {
            settings->shunt_microohms = value <= UINT32_MAX / 1000U
                ? value * 1000U : UINT32_MAX;
        }
        else if (err != ESP_ERR_NVS_NOT_FOUND) {
            nvs_close(handle);
            return false;
        }
    } else {
        nvs_close(handle);
        return false;
    }

    err = nvs_get_u32(handle, KEY_PWM_FREQUENCY, &value);
    if (err == ESP_OK) settings->pwm_frequency_hz = value;
    else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return false;
    }
    err = nvs_get_u32(handle, KEY_INA219_SHUNT_RANGE, &value);
    if (err == ESP_OK) settings->ina219_shunt_range_uv = value;
    else if (err != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return false;
    }
#define LOAD_OPTIONAL_GAIN(key, field) \
    do { \
        err = nvs_get_u32(handle, key, &value); \
        if (err == ESP_OK) settings->field = value; \
        else if (err != ESP_ERR_NVS_NOT_FOUND) { \
            nvs_close(handle); \
            return false; \
        } \
    } while (0)
    LOAD_OPTIONAL_GAIN(KEY_CURRENT_KP_MILLI, current_kp_milli);
    LOAD_OPTIONAL_GAIN(KEY_CURRENT_KI_MILLI, current_ki_milli);
    LOAD_OPTIONAL_GAIN(KEY_VOLTAGE_KP_MILLI, voltage_kp_milli);
    LOAD_OPTIONAL_GAIN(KEY_VOLTAGE_KI_MILLI, voltage_ki_milli);
#undef LOAD_OPTIONAL_GAIN
    nvs_close(handle);
    return true;
}

bool settings_storage_save(const load_app_settings_t *settings)
{
    if (!settings) return false;
    nvs_handle_t handle;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return false;

    esp_err_t err = nvs_set_u32(handle, KEY_SHUNT_MICROOHMS,
                                settings->shunt_microohms);
    if (err == ESP_OK) err = nvs_set_u32(handle, KEY_PWM_FREQUENCY,
                                          settings->pwm_frequency_hz);
    if (err == ESP_OK) err = nvs_set_u32(handle, KEY_INA219_SHUNT_RANGE,
                                          settings->ina219_shunt_range_uv);
    if (err == ESP_OK) err = nvs_set_u32(handle, KEY_CURRENT_KP_MILLI,
                                          settings->current_kp_milli);
    if (err == ESP_OK) err = nvs_set_u32(handle, KEY_CURRENT_KI_MILLI,
                                          settings->current_ki_milli);
    if (err == ESP_OK) err = nvs_set_u32(handle, KEY_VOLTAGE_KP_MILLI,
                                          settings->voltage_kp_milli);
    if (err == ESP_OK) err = nvs_set_u32(handle, KEY_VOLTAGE_KI_MILLI,
                                          settings->voltage_ki_milli);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err == ESP_OK;
}

#include "ina219.h"
#include "platform/board_config.h"

#include <math.h>
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* INA219 register map and the conversion selected by CONFIG_VALUE. */
enum {
    INA219_REG_CONFIGURATION = 0x00,
    INA219_REG_SHUNT_VOLTAGE = 0x01,
    INA219_REG_BUS_VOLTAGE = 0x02,
    INA219_CONFIG_CONTINUOUS_32V_12BIT = 0x219F,
    INA219_CONFIG_PGA_MASK = 0x1800,
    INA219_BUS_OVERFLOW_MASK = 1U << 0,
    INA219_BUS_CONVERSION_READY_MASK = 1U << 1,
};

#define INA219_BUS_VOLTAGE_LSB_V  0.004f
#define INA219_SHUNT_VOLTAGE_LSB_V 0.00001f
#define INA219_ZERO_CALIBRATION_SAMPLES 16U
#define INA219_ZERO_DEADBAND_COUNTS 0.5f

static const char *TAG = "ina219";
static i2c_master_dev_handle_t s_dev;
static i2c_master_bus_handle_t s_bus;
static float s_shunt_ohms = INA219_SHUNT_OHMS;
static uint32_t s_shunt_range_uv = 320000U;
static float s_shunt_zero_offset_counts;
static bool s_shunt_zero_calibrated;

bool ina219_set_shunt_resistance(float resistance_ohms)
{
    if (!isfinite(resistance_ohms) || resistance_ohms < 0.0001f ||
        resistance_ohms > 0.2f) return false;
    s_shunt_ohms = resistance_ohms;
    return true;
}

static bool shunt_range_bits(uint32_t full_scale_uv, uint16_t *bits)
{
    if (!bits) return false;
    switch (full_scale_uv) {
    case 40000U:  *bits = 0x0000; return true;
    case 80000U:  *bits = 0x0800; return true;
    case 160000U: *bits = 0x1000; return true;
    case 320000U: *bits = 0x1800; return true;
    default: return false;
    }
}

static uint16_t configuration_value(void)
{
    uint16_t pga_bits = 0;
    (void)shunt_range_bits(s_shunt_range_uv, &pga_bits);
    return (uint16_t)((INA219_CONFIG_CONTINUOUS_32V_12BIT &
                       (uint16_t)~INA219_CONFIG_PGA_MASK) | pga_bits);
}

void ina219_bind_bus(i2c_master_bus_handle_t bus)
{
    s_bus = bus;
}

static esp_err_t write_reg(uint8_t reg, uint16_t value)
{
    uint8_t data[3] = {reg, (uint8_t)(value >> 8), (uint8_t)value};
    return i2c_master_transmit(s_dev, data, sizeof(data), 100);
}

static esp_err_t read_reg(uint8_t reg, uint16_t *value)
{
    uint8_t bytes[2];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, bytes, sizeof(bytes), 100);
    if (err == ESP_OK) *value = ((uint16_t)bytes[0] << 8) | bytes[1];
    return err;
}

/* Wait for a completed conversion, then take its bus and shunt registers. */
static bool read_raw_sample(uint16_t *bus_raw, int16_t *shunt_counts)
{
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        esp_err_t err = read_reg(INA219_REG_BUS_VOLTAGE, bus_raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "bus-voltage register read failed: %s", esp_err_to_name(err));
            return false;
        }
        if (*bus_raw & INA219_BUS_OVERFLOW_MASK) {
            ESP_LOGW(TAG, "INA219 reports measurement overflow (bus register 0x%04x)",
                     (unsigned)*bus_raw);
            return false;
        }
        if (*bus_raw & INA219_BUS_CONVERSION_READY_MASK) break;
        if (attempt == 2) {
            ESP_LOGW(TAG, "conversion not ready after retries (bus register 0x%04x)",
                     (unsigned)*bus_raw);
            return false;
        }
        vTaskDelay(1);
    }

    uint16_t shunt_raw;
    esp_err_t err = read_reg(INA219_REG_SHUNT_VOLTAGE, &shunt_raw);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "shunt-voltage register read failed: %s", esp_err_to_name(err));
        return false;
    }
    *shunt_counts = (int16_t)shunt_raw;
    return true;
}

/* Called during sensor initialization, when the load output is kept disabled. */
static esp_err_t calibrate_shunt_zero(void)
{
    int32_t sum = 0;
    for (unsigned i = 0; i < INA219_ZERO_CALIBRATION_SAMPLES; ++i) {
        uint16_t bus_raw;
        int16_t shunt_counts;
        if (!read_raw_sample(&bus_raw, &shunt_counts)) {
            s_shunt_zero_calibrated = false;
            return ESP_FAIL;
        }
        sum += shunt_counts;
        /* Allow the continuous ADC to produce a new sample between reads. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_shunt_zero_offset_counts = (float)sum / INA219_ZERO_CALIBRATION_SAMPLES;
    s_shunt_zero_calibrated = true;
    ESP_LOGI(TAG, "zero-current shunt offset: %.2f counts (Rshunt=%.6f ohm)",
             (double)s_shunt_zero_offset_counts, (double)s_shunt_ohms);
    return ESP_OK;
}

bool ina219_set_shunt_range(uint32_t full_scale_uv)
{
    uint16_t bits;
    if (!shunt_range_bits(full_scale_uv, &bits)) return false;
    (void)bits;
    s_shunt_range_uv = full_scale_uv;
    if (!s_dev) return true;

    if (write_reg(INA219_REG_CONFIGURATION, configuration_value()) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
    return calibrate_shunt_zero() == ESP_OK;
}

esp_err_t ina219_init(i2c_master_bus_handle_t bus)
{
    if (!s_bus) s_bus = bus;
    if (s_dev) {
        ESP_RETURN_ON_ERROR(write_reg(INA219_REG_CONFIGURATION,
                                      configuration_value()),
                            "ina219", "configure sensor");
        vTaskDelay(pdMS_TO_TICKS(100));
        return calibrate_shunt_zero();
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_INA219_I2C_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &cfg, &s_dev), "ina219", "add device");
    /* 32 V bus range, selected shunt range, 12-bit, continuous conversion. */
    ESP_RETURN_ON_ERROR(write_reg(INA219_REG_CONFIGURATION,
                                  configuration_value()),
                        "ina219", "configure sensor");
    vTaskDelay(pdMS_TO_TICKS(100)); /* allow the first continuous conversion to complete */
    return calibrate_shunt_zero();
}

bool ina219_reinitialize(void)
{
    return s_bus != NULL && ina219_init(s_bus) == ESP_OK;
}

bool ina219_read(load_measurement_t *m)
{
    if (!m || !s_dev || !s_shunt_zero_calibrated) return false;
    uint16_t bus_raw;
    int16_t shunt_signed;
    if (!read_raw_sample(&bus_raw, &shunt_signed)) return false;
    /* Bus voltage is stored in bits 15:3 at 4 mV/bit; shunt voltage is
     * signed at 10 uV/bit. Current is derived from Vshunt / Rshunt. */
    m->voltage_v = (float)(bus_raw >> 3) * INA219_BUS_VOLTAGE_LSB_V;
    float corrected_counts = (float)shunt_signed - s_shunt_zero_offset_counts;
    /* Ignore less than half an ADC count of residual zero-current noise. */
    if (fabsf(corrected_counts) < INA219_ZERO_DEADBAND_COUNTS) corrected_counts = 0.0f;
    m->current_a = (corrected_counts * INA219_SHUNT_VOLTAGE_LSB_V) /
                   s_shunt_ohms;
    m->power_w = m->voltage_v * (m->current_a > 0.0f ? m->current_a : 0.0f);
    return isfinite(m->voltage_v) && isfinite(m->current_a) && isfinite(m->power_w);
}

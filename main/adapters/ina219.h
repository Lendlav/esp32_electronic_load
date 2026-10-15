#pragma once
#include <stdint.h>
#include "driver/i2c_master.h"
#include "domain/load_controller.h"
#include "esp_err.h"

esp_err_t ina219_init(i2c_master_bus_handle_t bus);
void ina219_bind_bus(i2c_master_bus_handle_t bus);
bool ina219_reinitialize(void);
bool ina219_read(load_measurement_t *measurement);
bool ina219_set_shunt_resistance(float resistance_ohms);
bool ina219_set_shunt_range(uint32_t full_scale_uv);

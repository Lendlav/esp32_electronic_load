#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "application/load_app.h"
esp_err_t display_init(i2c_master_bus_handle_t bus);
void display_show(const app_view_t *view);

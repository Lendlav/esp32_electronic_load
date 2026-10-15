#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_err.h"
#include "application/load_app.h"
esp_err_t buttons_start(QueueHandle_t event_queue, bool *back_held_at_boot);

#pragma once

#include <stdbool.h>
#include "application/load_app.h"
#include "esp_err.h"

typedef bool (*web_view_provider_t)(void *context, app_view_t *view);

/* Starts the board access point and a read-only HTTP status interface. */
esp_err_t web_server_start(web_view_provider_t get_view, void *context);

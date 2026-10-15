#pragma once

#include "application/load_app.h"

bool settings_storage_load(load_app_settings_t *settings);
bool settings_storage_save(const load_app_settings_t *settings);

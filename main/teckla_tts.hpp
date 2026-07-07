#pragma once

#include "esp_err.h"

esp_err_t teckla_tts_init();
esp_err_t teckla_tts_say(const char *text);
bool teckla_tts_is_busy();

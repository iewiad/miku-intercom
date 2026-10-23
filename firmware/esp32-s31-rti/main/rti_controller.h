#pragma once

#include "esp_err.h"
#include "board_audio.h"

esp_err_t rti_controller_init(const board_audio_t *audio);

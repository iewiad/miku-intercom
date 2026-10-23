#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_codec_dev.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_codec_dev_handle_t microphone;
    esp_codec_dev_handle_t speaker;
    uint8_t output_volume;
} board_audio_t;

/** Initialize the Korvo-1 ES8389 and its 16 kHz full-duplex I2S bus. */
esp_err_t board_audio_init(board_audio_t *audio);

/** Apply a bounded 0..100 speaker volume and optionally persist it in NVS. */
esp_err_t board_audio_set_output_volume(board_audio_t *audio, uint8_t volume, bool persist);

/** Return the currently applied 0..100 speaker volume. */
uint8_t board_audio_get_output_volume(const board_audio_t *audio);

#ifdef __cplusplus
}
#endif

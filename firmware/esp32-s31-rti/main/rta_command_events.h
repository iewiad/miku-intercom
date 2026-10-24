#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define WEBRTC_COMMAND_TEXT_MAX 512

typedef enum {
    WEBRTC_COMMAND_READY,
    WEBRTC_COMMAND_FINAL,
    WEBRTC_COMMAND_ERROR,
} webrtc_command_event_type_t;

typedef struct {
    webrtc_command_event_type_t type;
    uint32_t generation;
    esp_err_t error;
    char text[WEBRTC_COMMAND_TEXT_MAX + 1];
} webrtc_command_event_t;

/** Runs on a Peer worker. Copy any needed data before returning; never stop the
 * Peer or perform blocking work here. A callback already admitted before cancel
 * may finish afterward, so the owner must also check generation/cancel intent.
 */
typedef void (*webrtc_command_event_cb_t)(const webrtc_command_event_t *event, void *context);

typedef struct {
    uint32_t generation;
    bool active;
    bool audio_available;
    bool update_acknowledged;
    bool ready;
    bool terminal;
} rta_command_state_t;

typedef enum {
    RTA_COMMAND_INPUT_IGNORE,
    RTA_COMMAND_INPUT_UPDATED,
    RTA_COMMAND_INPUT_FINAL,
    RTA_COMMAND_INPUT_ERROR,
} rta_command_input_type_t;

typedef struct {
    rta_command_input_type_t type;
    esp_err_t error;
    /** Diagnostics are fixed static literals, never provider-controlled text. */
    const char *event_type;
    const char *parse_reason;
    char text[WEBRTC_COMMAND_TEXT_MAX + 1];
} rta_command_input_t;

/** No logs, callbacks or retained transcript. Malformed/oversized final text is
 * terminal ERROR instead of a truncated command. Parsing may allocate cJSON.
 */
void rta_command_parse_event(const uint8_t *data, size_t size, rta_command_input_t *input);

/** These state-only functions require the caller's short critical section. */
void rta_command_begin(rta_command_state_t *state, uint32_t generation);
void rta_command_cancel(rta_command_state_t *state);
bool rta_command_media_allowed(const rta_command_state_t *state);
bool rta_command_audio_available(rta_command_state_t *state, webrtc_command_event_t *event);
bool rta_command_accept(rta_command_state_t *state, uint32_t generation,
                        const rta_command_input_t *input, webrtc_command_event_t *event);

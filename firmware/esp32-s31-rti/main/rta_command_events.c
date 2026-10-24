#include "rta_command_events.h"

#include <string.h>
#include <stdlib.h>

#include "cJSON.h"

#define COMMAND_JSON_MAX (12 * 1024)

static bool valid_utf8_text(const unsigned char *text, size_t size)
{
    bool nonblank = false;
    for (size_t i = 0; i < size;) {
        const unsigned char c = text[i];
        if (c < 0x80) {
            if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') return false;
            if (c == 0x7f) return false;
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') nonblank = true;
            ++i;
            continue;
        }
        size_t count;
        uint32_t code;
        if (c >= 0xc2 && c <= 0xdf) { count = 2; code = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { count = 3; code = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 4; code = c & 0x07; }
        else return false;
        if (count > size - i) return false;
        for (size_t j = 1; j < count; ++j) {
            if ((text[i + j] & 0xc0) != 0x80) return false;
            code = (code << 6) | (text[i + j] & 0x3f);
        }
        if ((count == 3 && code < 0x800) || (count == 4 && code < 0x10000) ||
            (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff) return false;
        nonblank = true;
        i += count;
    }
    return nonblank;
}

static const char *command_event_type(const char *type)
{
    static const char *const known[] = {
        "session.created", "session.updated", "error", "rtid.error", "session.closed",
        "conversation.item.input_audio_transcription.delta",
        "conversation.item.input_audio_transcription.completed",
        "conversation.item.input_audio_transcription.failed",
        "input_audio_buffer.speech_started", "input_audio_buffer.speech_stopped",
        "response.created", "response.done", "response.audio_transcript.delta",
        "response.output_audio_transcript.delta", "response.text.delta",
    };
    for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
        if (strcmp(type, known[i]) == 0) return known[i];
    }
    return "other";
}

static const char *command_error_reason(const cJSON *root)
{
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
    if (!cJSON_IsString(code)) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        code = cJSON_IsObject(error) ? cJSON_GetObjectItemCaseSensitive(error, "code") : NULL;
    }
    static const char *const known[] = {
        "invalid_session_update", "invalid_datachannel_event", "locked_voice_enforcement_failed",
        "rti_capability_unsupported", "rti_event_invalid", "rti_event_type_unsupported",
        "rti_profile_violation", "rti_provider_protocol_error", "rti_provider_unavailable",
        "rti_response_incomplete", "invalid_request_error", "invalid_value", "invalid_request",
    };
    if (cJSON_IsString(code)) {
        for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); ++i) {
            if (strcmp(code->valuestring, known[i]) == 0) return known[i];
        }
    }
    return "provider_error";
}

void rta_command_parse_event(const uint8_t *data, size_t size, rta_command_input_t *input)
{
    if (input == NULL) return;
    memset(input, 0, sizeof(*input));
    input->event_type = "unavailable";
    input->parse_reason = "invalid_size";
    if (data == NULL || size == 0 || size > COMMAND_JSON_MAX) return;
    input->parse_reason = "embedded_nul";
    if (memchr(data, 0, size)) return;
    // cJSON represents strings as C strings. Reject escaped NUL anywhere so a
    // final command cannot be silently shortened before authorization matching.
    for (size_t i = 0; i + 5 < size; ++i) {
        if (memcmp(data + i, "\\u0000", 6) == 0) {
            input->type = RTA_COMMAND_INPUT_ERROR;
            input->error = ESP_ERR_INVALID_RESPONSE;
            input->parse_reason = "escaped_nul";
            return;
        }
    }
    char *json = malloc(size + 1);
    if (json == NULL) {
        input->type = RTA_COMMAND_INPUT_ERROR;
        input->error = ESP_ERR_NO_MEM;
        input->parse_reason = "allocation_failed";
        return;
    }
    memcpy(json, data, size);
    json[size] = 0;
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithOpts(json, &end, true);
    if (root == NULL) { input->parse_reason = "malformed_json"; free(json); return; }
    input->parse_reason = "missing_type";
    const cJSON *kind = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(kind)) {
        input->event_type = command_event_type(kind->valuestring);
        input->parse_reason = "ignored";
        if (strcmp(kind->valuestring, "session.updated") == 0) {
            input->type = RTA_COMMAND_INPUT_UPDATED;
            input->parse_reason = "accepted";
        } else if (strcmp(kind->valuestring, "error") == 0 ||
                   strcmp(kind->valuestring, "rtid.error") == 0 ||
                   strcmp(kind->valuestring, "session.closed") == 0 ||
                   strcmp(kind->valuestring, "conversation.item.input_audio_transcription.failed") == 0) {
            input->type = RTA_COMMAND_INPUT_ERROR;
            input->error = ESP_FAIL;
            input->parse_reason = command_error_reason(root);
        } else if (strcmp(kind->valuestring, "conversation.item.input_audio_transcription.completed") == 0) {
            const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "transcript");
            const size_t length = cJSON_IsString(text) ? strlen(text->valuestring) : 0;
            if (length > 0 && length <= WEBRTC_COMMAND_TEXT_MAX &&
                valid_utf8_text((const unsigned char *)text->valuestring, length)) {
                input->type = RTA_COMMAND_INPUT_FINAL;
                input->parse_reason = "accepted";
                memcpy(input->text, text->valuestring, length + 1);
            } else {
                input->type = RTA_COMMAND_INPUT_ERROR;
                input->error = ESP_ERR_INVALID_RESPONSE;
                input->parse_reason = "invalid_final_text";
            }
        }
    }
    cJSON_Delete(root);
    // The transient input/output contain the sole retained final text. The
    // owner copies once into its short-lived command slot and clears it later.
    memset(json, 0, size);
    free(json);
}

void rta_command_begin(rta_command_state_t *state, uint32_t generation)
{
    memset(state, 0, sizeof(*state));
    state->generation = generation;
    state->active = true;
}

void rta_command_cancel(rta_command_state_t *state)
{
    state->active = false;
    state->ready = false;
    state->terminal = true;
}

bool rta_command_media_allowed(const rta_command_state_t *state)
{
    return state->active && state->ready && !state->terminal;
}

static bool publish_ready(rta_command_state_t *state, webrtc_command_event_t *event)
{
    if (!state->active || state->terminal || state->ready ||
        !state->audio_available || !state->update_acknowledged) return false;
    state->ready = true;
    memset(event, 0, sizeof(*event));
    event->type = WEBRTC_COMMAND_READY;
    event->generation = state->generation;
    return true;
}

bool rta_command_audio_available(rta_command_state_t *state, webrtc_command_event_t *event)
{
    state->audio_available = true;
    return publish_ready(state, event);
}

bool rta_command_accept(rta_command_state_t *state, uint32_t generation,
                        const rta_command_input_t *input, webrtc_command_event_t *event)
{
    if (!state->active || state->terminal || generation != state->generation || input == NULL) return false;
    if (input->type == RTA_COMMAND_INPUT_UPDATED) {
        state->update_acknowledged = true;
        return publish_ready(state, event);
    }
    if (input->type != RTA_COMMAND_INPUT_ERROR &&
        (input->type != RTA_COMMAND_INPUT_FINAL || !state->ready)) return false;
    state->terminal = true;
    state->ready = false;
    memset(event, 0, sizeof(*event));
    event->generation = state->generation;
    event->type = input->type == RTA_COMMAND_INPUT_FINAL ? WEBRTC_COMMAND_FINAL : WEBRTC_COMMAND_ERROR;
    event->error = input->error;
    if (event->type == WEBRTC_COMMAND_FINAL) memcpy(event->text, input->text, sizeof(event->text));
    return true;
}

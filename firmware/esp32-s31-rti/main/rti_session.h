#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char session_id[96];
    char trace_id[96];
    char rtc_url[384];
    char access_token[2048];
    int64_t expires_at_ms;
} rti_session_t;

#define RTI_VOICE_PROFILE_LIMIT 20
#define RTI_VOICE_PROFILE_NAME_MAX 32
#define RTI_VOICE_PROFILE_TITLE_MAX 64

typedef struct {
    char names[RTI_VOICE_PROFILE_LIMIT][RTI_VOICE_PROFILE_NAME_MAX];
    char titles[RTI_VOICE_PROFILE_LIMIT][RTI_VOICE_PROFILE_TITLE_MAX];
    size_t count;
} rti_voice_profile_list_t;

const char *rti_session_get_voice_profile(void);
const char *rti_session_get_voice_title(void);
esp_err_t rti_session_set_voice_profile(const char *voice_profile, const char *voice_title);
esp_err_t rti_session_list_voice_profiles(rti_voice_profile_list_t *profiles);
esp_err_t rti_session_create(rti_session_t *session);
esp_err_t rti_session_exchange_sdp(const rti_session_t *session, const char *offer_sdp,
                                   char **answer_sdp);
esp_err_t rti_session_stop(const rti_session_t *session, const char *reason);

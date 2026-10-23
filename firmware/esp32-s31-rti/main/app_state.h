#pragma once

typedef enum {
    RTI_APP_STATE_BOOT = 0,
    RTI_APP_STATE_WIFI_CONNECTING,
    RTI_APP_STATE_READY,
    RTI_APP_STATE_SESSION_CREATING,
    RTI_APP_STATE_RTC_CONNECTING,
    RTI_APP_STATE_LISTENING,
    RTI_APP_STATE_USER_SPEAKING,
    RTI_APP_STATE_AGENT_SPEAKING,
    RTI_APP_STATE_RECONNECTING,
    RTI_APP_STATE_STOPPING,
    RTI_APP_STATE_ERROR,
} rti_app_state_t;

const char *rti_app_state_name(rti_app_state_t state);
void rti_app_set_state(rti_app_state_t state);
rti_app_state_t rti_app_get_state(void);

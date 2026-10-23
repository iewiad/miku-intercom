#include "app_state.h"

#include "esp_log.h"

static const char *TAG = "rti_state";
static rti_app_state_t current_state = RTI_APP_STATE_BOOT;

const char *rti_app_state_name(rti_app_state_t state)
{
    switch (state) {
    case RTI_APP_STATE_BOOT:
        return "boot";
    case RTI_APP_STATE_WIFI_CONNECTING:
        return "wifi_connecting";
    case RTI_APP_STATE_READY:
        return "ready";
    case RTI_APP_STATE_SESSION_CREATING:
        return "session_creating";
    case RTI_APP_STATE_RTC_CONNECTING:
        return "rtc_connecting";
    case RTI_APP_STATE_LISTENING:
        return "listening";
    case RTI_APP_STATE_USER_SPEAKING:
        return "user_speaking";
    case RTI_APP_STATE_AGENT_SPEAKING:
        return "agent_speaking";
    case RTI_APP_STATE_RECONNECTING:
        return "reconnecting";
    case RTI_APP_STATE_STOPPING:
        return "stopping";
    case RTI_APP_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

void rti_app_set_state(rti_app_state_t state)
{
    if (state == current_state) {
        return;
    }
    ESP_LOGI(TAG, "%s -> %s", rti_app_state_name(current_state), rti_app_state_name(state));
    current_state = state;
}

rti_app_state_t rti_app_get_state(void)
{
    return current_state;
}

#include "rti_controller.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_state.h"
#include "board_display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"
#include "rti_session.h"
#include "webrtc_peer.h"

#define RTI_CONTROLLER_TASK_STACK_SIZE (16 * 1024)
#define RTI_CONTROLLER_TASK_PRIORITY 10
#define RTI_CONTROLLER_ACTION_START 1U
#define RTI_CONTROLLER_ACTION_STOP 2U

static const char *TAG = "rti_controller";
static SemaphoreHandle_t controller_lock;
static rti_session_t active_session;
static bool session_active;
static bool operation_in_progress;
static board_audio_t board_audio;
static TaskHandle_t controller_task_handle;

static void finish_operation(bool active)
{
    xSemaphoreTake(controller_lock, portMAX_DELAY);
    session_active = active;
    operation_in_progress = false;
    xSemaphoreGive(controller_lock);
}

static void start_session(void)
{
    rti_session_t created = {0};
    board_display_set_conversation_state(BOARD_CONVERSATION_STARTING,
                                         "Creating a secure voice session", false);
    const esp_err_t err = rti_session_create(&created);
    if (err == ESP_OK) {
        board_display_set_conversation_state(BOARD_CONVERSATION_STARTING,
                                             "Connecting realtime channel", false);
        const esp_err_t rtc_err = webrtc_peer_start(&created, &board_audio);
        if (rtc_err != ESP_OK) {
            ESP_LOGW(TAG, "RTC start failed: %s", esp_err_to_name(rtc_err));
            (void)rti_session_stop(&created, "rtc_connect_failed");
            finish_operation(false);
            rti_app_set_state(RTI_APP_STATE_ERROR);
            board_display_set_conversation_state(BOARD_CONVERSATION_ERROR,
                                                 board_display_is_chinese()
                                                     ? "RTC 连接失败，请重试"
                                                     : "Could not connect RTC. Tap to retry.", false);
            mbedtls_platform_zeroize(&created, sizeof(created));
            return;
        }
        xSemaphoreTake(controller_lock, portMAX_DELAY);
        active_session = created;
        xSemaphoreGive(controller_lock);
        finish_operation(true);
        board_display_set_conversation_state(BOARD_CONVERSATION_ACTIVE,
                                             "Listening", true);
        board_display_reset_transcript();
        board_display_set_transcript(BOARD_TRANSCRIPT_USER,
                                     board_display_is_chinese()
                                         ? "正在等待你的语音输入"
                                         : "Waiting for your voice",
                                     false);
        board_display_set_transcript(BOARD_TRANSCRIPT_ASSISTANT,
                                     board_display_is_chinese()
                                         ? "正在等待实时回复"
                                         : "Waiting for a realtime reply",
                                     false);
        ESP_LOGI(TAG, "session lifecycle is active");
    } else {
        finish_operation(false);
        board_display_set_conversation_state(BOARD_CONVERSATION_ERROR,
                                             board_display_is_chinese()
                                                 ? "无法创建会话，请重试"
                                                 : "Could not create session. Tap to retry.", false);
        ESP_LOGW(TAG, "session start failed: %s", esp_err_to_name(err));
    }
    mbedtls_platform_zeroize(&created, sizeof(created));
}

static void stop_session(void)
{
    rti_session_t stopping = {0};
    xSemaphoreTake(controller_lock, portMAX_DELAY);
    stopping = active_session;
    xSemaphoreGive(controller_lock);

    rti_app_set_state(RTI_APP_STATE_STOPPING);
    board_display_set_conversation_state(BOARD_CONVERSATION_STOPPING,
                                         "Ending the voice session", true);
    webrtc_peer_stop();
    const esp_err_t err = rti_session_stop(&stopping, "device_hangup");
    if (err == ESP_OK) {
        xSemaphoreTake(controller_lock, portMAX_DELAY);
        mbedtls_platform_zeroize(&active_session, sizeof(active_session));
        xSemaphoreGive(controller_lock);
        finish_operation(false);
        rti_app_set_state(RTI_APP_STATE_READY);
        board_display_set_conversation_state(BOARD_CONVERSATION_IDLE,
                                             "Connected and ready", false);
        board_display_reset_transcript();
        board_display_set_transcript(BOARD_TRANSCRIPT_USER,
                                     board_display_is_chinese()
                                         ? "你的语音文字会显示在这里"
                                         : "Your speech transcript appears here",
                                     false);
        board_display_set_transcript(BOARD_TRANSCRIPT_ASSISTANT,
                                     board_display_is_chinese()
                                         ? "实时回复会显示在这里"
                                         : "Realtime replies appear here",
                                     false);
    } else {
        finish_operation(true);
        rti_app_set_state(RTI_APP_STATE_ERROR);
        board_display_set_conversation_state(BOARD_CONVERSATION_ERROR,
                                             board_display_is_chinese()
                                                 ? "无法结束会话，请重试"
                                                 : "Could not end session. Tap to retry.", true);
        ESP_LOGW(TAG, "session stop failed: %s", esp_err_to_name(err));
    }
    mbedtls_platform_zeroize(&stopping, sizeof(stopping));
}

static void controller_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "persistent lifecycle worker ready");
    while (true) {
        uint32_t action = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &action, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ESP_LOGI(TAG, "lifecycle worker received action=%" PRIu32, action);
        if (action == RTI_CONTROLLER_ACTION_STOP) {
            stop_session();
        } else if (action == RTI_CONTROLLER_ACTION_START) {
            start_session();
        } else {
            ESP_LOGW(TAG, "lifecycle worker ignored unknown action=%" PRIu32, action);
        }
    }
}

static void conversation_action(bool stop, void *context)
{
    (void)context;
    if (controller_lock == NULL || xSemaphoreTake(controller_lock, 0) != pdTRUE) {
        ESP_LOGW(TAG, "conversation action ignored: controller lock unavailable");
        return;
    }
    ESP_LOGI(TAG, "conversation action requested: stop=%d active=%d busy=%d",
             stop, session_active, operation_in_progress);
    if (operation_in_progress || stop != session_active) {
        ESP_LOGW(TAG, "conversation action ignored: stop=%d active=%d busy=%d",
                 stop, session_active, operation_in_progress);
        xSemaphoreGive(controller_lock);
        return;
    }
    operation_in_progress = true;
    xSemaphoreGive(controller_lock);

    const uint32_t action = stop ? RTI_CONTROLLER_ACTION_STOP : RTI_CONTROLLER_ACTION_START;
    if (controller_task_handle == NULL ||
        xTaskNotify(controller_task_handle, action, eSetValueWithoutOverwrite) != pdPASS) {
        ESP_LOGE(TAG, "could not notify lifecycle worker: action=%" PRIu32, action);
        finish_operation(stop);
        board_display_set_conversation_state(BOARD_CONVERSATION_ERROR,
                                             "Could not schedule session action", stop);
    }
}

static void volume_action(uint8_t volume, bool persist, void *context)
{
    (void)context;
    const esp_err_t err = board_audio_set_output_volume(&board_audio, volume, persist);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not set speaker volume: %s", esp_err_to_name(err));
    }
}

esp_err_t rti_controller_init(const board_audio_t *audio)
{
    if (audio == NULL || audio->microphone == NULL || audio->speaker == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    board_audio = *audio;
    controller_lock = xSemaphoreCreateMutex();
    if (controller_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(controller_task, "rti_lifecycle",
                    RTI_CONTROLLER_TASK_STACK_SIZE, NULL,
                    RTI_CONTROLLER_TASK_PRIORITY, &controller_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "could not create persistent lifecycle worker");
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(board_display_show_conversation(conversation_action, volume_action,
                                                        board_audio_get_output_volume(&board_audio),
                                                        NULL),
                        TAG, "could not show conversation screen");
    rti_app_set_state(RTI_APP_STATE_READY);
    return ESP_OK;
}

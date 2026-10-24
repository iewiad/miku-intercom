#include "board_display.h"
#include "app_brand.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "rti_session.h"
#include "wifi_manager.h"

#define WIFI_SCAN_LIMIT 12
#define WIFI_TASK_STACK_SIZE 6144
#define TRANSCRIPT_TEXT_MAX 512
#define TRANSCRIPT_HISTORY_TURNS 10
#define TRANSCRIPT_HISTORY_USER_MAX 128
#define TRANSCRIPT_HISTORY_ASSISTANT_MAX 192
#define TRANSCRIPT_TIMELINE_MAX 6144
#define UI_SETTINGS_NAMESPACE "rti_settings"
#define UI_LANGUAGE_KEY "ui_lang"

static const char *TAG = "board_display";
static wifi_ap_record_t scan_results[WIFI_SCAN_LIMIT];
static uint16_t scan_result_count;
static char selected_ssid[33];
static char selected_password[65];
static lv_obj_t *conversation_status_label;
static lv_obj_t *conversation_detail_label;
static lv_obj_t *conversation_action_button;
static lv_obj_t *conversation_action_label;
static lv_obj_t *conversation_transcript_label;
static lv_obj_t *conversation_timeline_scroll;
static lv_obj_t *conversation_orb_core;
static lv_obj_t *conversation_orb_halo;
static lv_obj_t *conversation_voice_bars[5];
static lv_obj_t *conversation_voice_overlay;
static lv_obj_t *conversation_volume_overlay;
static lv_obj_t *conversation_volume_value_label;
static lv_obj_t *conversation_volume_slider;
static lv_obj_t *conversation_voice_chip_label;
static lv_obj_t *conversation_device_label;
static lv_obj_t *conversation_online_label;
static lv_obj_t *conversation_activity_title_label;
static lv_obj_t *conversation_panel_title_label;
static lv_obj_t *conversation_language_label;
static lv_obj_t *conversation_language_chip;
static rti_voice_profile_list_t conversation_voice_profiles;
static bool conversation_voice_profiles_loading;
static bool conversation_voice_profiles_loaded;
static board_conversation_action_cb_t conversation_action_callback;
static board_volume_action_cb_t conversation_volume_callback;
static void *conversation_action_context;
static bool conversation_action_is_stop;
static board_wifi_connected_cb_t wifi_connected_callback;
static void *wifi_connected_context;
static char conversation_user_text[TRANSCRIPT_TEXT_MAX + 1];
static char conversation_assistant_text[TRANSCRIPT_TEXT_MAX + 1];
static board_conversation_state_t conversation_current_state = BOARD_CONVERSATION_IDLE;
static char conversation_current_detail[96];
static bool conversation_language_chinese = true;
static uint8_t conversation_volume = 55;

typedef struct {
    char user[TRANSCRIPT_HISTORY_USER_MAX];
    char assistant[TRANSCRIPT_HISTORY_ASSISTANT_MAX];
} transcript_turn_t;

static transcript_turn_t transcript_history[TRANSCRIPT_HISTORY_TURNS];
static size_t transcript_history_count;
static char transcript_timeline[TRANSCRIPT_TIMELINE_MAX];

LV_FONT_DECLARE(rti_font_source_han_sans_sc_16_gb2312);
LV_FONT_DECLARE(shenglu_brand_24);
LV_FONT_DECLARE(shenglu_brand_48);
#define TRANSCRIPT_FONT (&rti_font_source_han_sans_sc_16_gb2312)

/* Keep digits visible in both text modes so SSIDs/passwords never need a
 * separate number page. The last row also exposes common Wi-Fi characters. */
static const char *const rti_keyboard_lower_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
    "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
    "ABC", "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
    "!", "_", "-", ".", "@", "$", "/", LV_SYMBOL_CLOSE, " ", LV_SYMBOL_OK, ""
};

static const char *const rti_keyboard_upper_map[] = {
    "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
    "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
    "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
    "abc", "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
    "!", "_", "-", ".", "@", "$", "/", LV_SYMBOL_CLOSE, " ", LV_SYMBOL_OK, ""
};

#define KEY_CTRL (LV_KEYBOARD_CTRL_BUTTON_FLAGS | LV_BUTTONMATRIX_CTRL_WIDTH_2)
static const lv_buttonmatrix_ctrl_t rti_keyboard_ctrl_map[] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1,
    KEY_CTRL, 1, 1, 1, 1, 1, 1, 1, KEY_CTRL,
    1, 1, 1, 1, 1, 1, 1, KEY_CTRL, LV_BUTTONMATRIX_CTRL_WIDTH_6, KEY_CTRL,
};
#undef KEY_CTRL

static void wifi_scan_task(void *arg);
static void wifi_connect_task(void *arg);
static void voice_profiles_task(void *arg);
static void show_password_screen(void);
static void wifi_connect_screen_async(void *arg);
static void show_manual_ssid_screen(void);
static void apply_conversation_language(void);

bool board_display_is_chinese(void)
{
    return conversation_language_chinese;
}

static void load_conversation_language(void)
{
    conversation_language_chinese = true;
    nvs_handle_t handle;
    if (nvs_open(UI_SETTINGS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        uint8_t language = 0;
        if (nvs_get_u8(handle, UI_LANGUAGE_KEY, &language) == ESP_OK) {
            conversation_language_chinese = language == 0;
        }
        nvs_close(handle);
    }
}

static void save_conversation_language(void)
{
    nvs_handle_t handle;
    if (nvs_open(UI_SETTINGS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        if (nvs_set_u8(handle, UI_LANGUAGE_KEY, conversation_language_chinese ? 0 : 1) == ESP_OK) {
            nvs_commit(handle);
        }
        nvs_close(handle);
    }
}

static lv_obj_t *create_rti_keyboard(lv_obj_t *screen, lv_obj_t *textarea)
{
    lv_obj_t *keyboard = lv_keyboard_create(screen);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER, rti_keyboard_lower_map,
                        rti_keyboard_ctrl_map);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_TEXT_UPPER, rti_keyboard_upper_map,
                        rti_keyboard_ctrl_map);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(keyboard, textarea);
    lv_keyboard_set_popovers(keyboard, false);
    lv_obj_set_size(keyboard, 776, 286);
    /* Keyboard defaults to bottom alignment; use screen coordinates explicitly. */
    lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, 12, 190);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_pad_row(keyboard, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_column(keyboard, 3, LV_PART_MAIN);
    return keyboard;
}

static void launch_scan_task(void)
{
    if (xTaskCreate(wifi_scan_task, "wifi_scan_ui", WIFI_TASK_STACK_SIZE, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start Wi-Fi scan task");
    }
}

static lv_obj_t *prepare_screen(const char *title, const char *subtitle)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x071116), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x102630), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);

    lv_obj_t *eyebrow = lv_label_create(screen);
    lv_label_set_text(eyebrow, APP_DISPLAY_NAME);
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(0x48E0C2), 0);
    lv_obj_set_style_text_font(eyebrow, &shenglu_brand_24, 0);
    lv_obj_set_style_text_letter_space(eyebrow, 2, 0);
    lv_obj_align(eyebrow, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *heading = lv_label_create(screen);
    lv_label_set_text(heading, title);
    lv_obj_set_style_text_color(heading, lv_color_hex(0xF5FAFB), 0);
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_32, 0);
    lv_obj_align(heading, LV_ALIGN_TOP_MID, 0, 58);

    lv_obj_t *description = lv_label_create(screen);
    lv_label_set_text(description, subtitle);
    lv_obj_set_style_text_color(description, lv_color_hex(0x93A9B2), 0);
    lv_obj_set_style_text_font(description, &lv_font_montserrat_14, 0);
    lv_obj_align(description, LV_ALIGN_TOP_MID, 0, 102);

    return screen;
}

static void show_scanning_screen(void)
{
    lv_obj_t *screen = prepare_screen("WIFI SETUP", "Scanning for nearby networks");
    lv_obj_t *spinner = lv_spinner_create(screen);
    lv_obj_set_size(spinner, 72, 72);
    lv_obj_set_style_arc_color(spinner, lv_color_hex(0x48E0C2), LV_PART_INDICATOR);
    lv_obj_align(spinner, LV_ALIGN_CENTER, 0, 30);
}

static void start_scan(void)
{
    if (bsp_display_lock(1000)) {
        show_scanning_screen();
        bsp_display_unlock();
    }

    launch_scan_task();
}

static void start_scan_async(void *arg)
{
    show_scanning_screen();
    launch_scan_task();
}

static void rescan_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        lv_async_call(start_scan_async, NULL);
    }
}

static void show_message_screen(const char *title, const char *message, bool retry)
{
    lv_obj_t *screen = prepare_screen(title, message);

    lv_obj_t *icon = lv_label_create(screen);
    lv_label_set_text(icon, retry ? LV_SYMBOL_WARNING : LV_SYMBOL_OK);
    lv_obj_set_style_text_color(icon, lv_color_hex(retry ? 0xFFB45C : 0x48E0C2), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, 10);

    if (retry) {
        lv_obj_t *button = lv_button_create(screen);
        lv_obj_set_size(button, 280, 54);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x1B7568), 0);
        lv_obj_set_style_radius(button, 27, 0);
        lv_obj_align(button, LV_ALIGN_BOTTOM_MID, 0, -52);
        lv_obj_add_event_cb(button, rescan_event_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, "CHOOSE ANOTHER NETWORK");
        lv_obj_center(label);
    }
}

static void keyboard_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CANCEL) {
        lv_async_call(start_scan_async, NULL);
        return;
    }
    if (code != LV_EVENT_READY) {
        return;
    }

    lv_obj_t *keyboard = lv_event_get_target_obj(event);
    lv_obj_t *textarea = lv_keyboard_get_textarea(keyboard);
    strlcpy(selected_password, lv_textarea_get_text(textarea), sizeof(selected_password));
    lv_async_call(wifi_connect_screen_async, NULL);
}

static void wifi_connect_screen_async(void *arg)
{
    show_message_screen("CONNECTING", selected_ssid, false);

    if (xTaskCreate(wifi_connect_task, "wifi_connect_ui", WIFI_TASK_STACK_SIZE, NULL, 5, NULL) != pdPASS) {
        show_message_screen("CONNECTION FAILED", "Could not start connection task", true);
    }
}

static void show_password_screen(void)
{
    char subtitle[80];
    snprintf(subtitle, sizeof(subtitle), "Enter the password for %s", selected_ssid);
    lv_obj_t *screen = prepare_screen("NETWORK PASSWORD", subtitle);

    lv_obj_t *textarea = lv_textarea_create(screen);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_password_mode(textarea, true);
    lv_textarea_set_placeholder_text(textarea, "Password (leave empty for open Wi-Fi)");
    lv_obj_set_size(textarea, 680, 54);
    lv_obj_set_pos(textarea, 60, 126);

    lv_obj_t *keyboard = create_rti_keyboard(screen, textarea);
    lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, keyboard_event_cb, LV_EVENT_CANCEL, NULL);
}

static void show_password_screen_async(void *arg)
{
    show_password_screen();
}

static void manual_ssid_ready_async(void *arg)
{
    if (selected_ssid[0] == '\0') {
        show_manual_ssid_screen();
        return;
    }

    selected_password[0] = '\0';
    show_password_screen();
}

static void manual_ssid_keyboard_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_CANCEL) {
        lv_async_call(start_scan_async, NULL);
        return;
    }
    if (code != LV_EVENT_READY) {
        return;
    }

    lv_obj_t *keyboard = lv_event_get_target_obj(event);
    lv_obj_t *textarea = lv_keyboard_get_textarea(keyboard);
    strlcpy(selected_ssid, lv_textarea_get_text(textarea), sizeof(selected_ssid));
    lv_async_call(manual_ssid_ready_async, NULL);
}

static void show_manual_ssid_screen(void)
{
    lv_obj_t *screen = prepare_screen("NETWORK NAME", "Enter a hidden or unlisted 2.4 GHz Wi-Fi name");

    lv_obj_t *textarea = lv_textarea_create(screen);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_max_length(textarea, sizeof(selected_ssid) - 1);
    lv_textarea_set_placeholder_text(textarea, "Wi-Fi name (SSID)");
    lv_obj_set_size(textarea, 680, 54);
    lv_obj_set_pos(textarea, 60, 126);

    lv_obj_t *keyboard = create_rti_keyboard(screen, textarea);
    lv_obj_add_event_cb(keyboard, manual_ssid_keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, manual_ssid_keyboard_event_cb, LV_EVENT_CANCEL, NULL);
}

static void show_manual_ssid_screen_async(void *arg)
{
    show_manual_ssid_screen();
}

static void manual_ssid_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        lv_async_call(show_manual_ssid_screen_async, NULL);
    }
}

static void network_selected_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    const uintptr_t index = (uintptr_t)lv_event_get_user_data(event);
    if (index >= scan_result_count) {
        return;
    }

    strlcpy(selected_ssid, (const char *)scan_results[index].ssid, sizeof(selected_ssid));
    selected_password[0] = '\0';
    lv_async_call(show_password_screen_async, NULL);
}

static void show_network_list(esp_err_t scan_result)
{
    const bool has_results = scan_result == ESP_OK && scan_result_count > 0;
    lv_obj_t *screen = prepare_screen("CHOOSE A NETWORK",
                                      has_results ? "Tap a Wi-Fi name, or add an unlisted network"
                                                  : "No networks found. Add a hidden network manually");
    lv_obj_t *list = lv_list_create(screen);
    lv_obj_set_size(list, 650, 325);
    lv_obj_set_pos(list, 75, 135);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x101E25), 0);
    lv_obj_set_style_border_color(list, lv_color_hex(0x28414B), 0);
    lv_obj_set_style_radius(list, 18, 0);

    for (uint16_t i = 0; has_results && i < scan_result_count; ++i) {
        char row[72];
        const char *security = scan_results[i].authmode == WIFI_AUTH_OPEN ? "OPEN" : "LOCKED";
        snprintf(row, sizeof(row), "%s    %s  %d dBm", scan_results[i].ssid, security, scan_results[i].rssi);
        lv_obj_t *button = lv_list_add_button(list, NULL, row);
        lv_obj_set_height(button, 54);
        lv_obj_add_event_cb(button, network_selected_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
    }

    lv_obj_t *manual_button = lv_list_add_button(list, LV_SYMBOL_PLUS, "ENTER NETWORK MANUALLY");
    lv_obj_set_height(manual_button, 58);
    lv_obj_set_style_text_color(manual_button, lv_color_hex(0x48E0C2), 0);
    lv_obj_add_event_cb(manual_button, manual_ssid_event_cb, LV_EVENT_CLICKED, NULL);
}

static void wifi_scan_task(void *arg)
{
    scan_result_count = WIFI_SCAN_LIMIT;
    const esp_err_t result = wifi_manager_scan(scan_results, &scan_result_count);

    if (bsp_display_lock(0)) {
        show_network_list(result);
        bsp_display_unlock();
    }
    vTaskDelete(NULL);
}

static void wifi_connect_task(void *arg)
{
    const esp_err_t result = wifi_manager_connect(selected_ssid, selected_password);

    if (result == ESP_OK && wifi_connected_callback != NULL) {
        const esp_err_t callback_result = wifi_connected_callback(wifi_connected_context);
        if (callback_result != ESP_OK && bsp_display_lock(0)) {
            show_message_screen("START FAILED", "Restart the device or try again", true);
            bsp_display_unlock();
        }
    } else if (bsp_display_lock(0)) {
        if (result != ESP_OK) {
            show_message_screen("CONNECTION FAILED", "Check the password or choose another network", true);
        } else {
            show_message_screen("WIFI CONNECTED", "Credentials saved. Restart to open the app.", false);
        }
        bsp_display_unlock();
    }
    vTaskDelete(NULL);
}

static void create_status_chip(lv_obj_t *parent, const char *text, lv_color_t color, int32_t x)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, 190, 48);
    lv_obj_set_pos(chip, x, 308);
    lv_obj_set_style_bg_color(chip, lv_color_hex(0x17232B), 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(chip, color, 0);
    lv_obj_set_style_border_width(chip, 2, 0);
    lv_obj_set_style_radius(chip, 24, 0);

    lv_obj_t *dot = lv_obj_create(chip);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_bg_color(dot, color, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(dot, LV_ALIGN_LEFT_MID, 18, 0);

    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(0xE8F0F2), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 42, 0);
}

static lv_obj_t *create_conversation_panel(lv_obj_t *screen, const char *speaker,
                                           int32_t x, int32_t y, int32_t width,
                                           int32_t height, lv_color_t accent)
{
    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, width, height);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0D1C27), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_90, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x203B49), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *speaker_label = lv_label_create(card);
    conversation_panel_title_label = speaker_label;
    lv_label_set_text(speaker_label, speaker);
    lv_obj_set_style_text_color(speaker_label, accent, 0);
    lv_obj_set_style_text_font(speaker_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(speaker_label, 3, 0);
    lv_obj_align(speaker_label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *live_dot = lv_obj_create(card);
    lv_obj_remove_style_all(live_dot);
    lv_obj_set_size(live_dot, 7, 7);
    lv_obj_set_style_bg_color(live_dot, accent, 0);
    lv_obj_set_style_bg_opa(live_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(live_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(live_dot, LV_ALIGN_TOP_RIGHT, 0, 4);

    conversation_timeline_scroll = lv_obj_create(card);
    lv_obj_remove_style_all(conversation_timeline_scroll);
    lv_obj_set_size(conversation_timeline_scroll, width - 32, height - 58);
    lv_obj_set_pos(conversation_timeline_scroll, 0, 36);
    lv_obj_add_flag(conversation_timeline_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(conversation_timeline_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(conversation_timeline_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(conversation_timeline_scroll, accent, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(conversation_timeline_scroll, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(conversation_timeline_scroll, 5, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(conversation_timeline_scroll, 3, LV_PART_SCROLLBAR);

    lv_obj_t *text_label = lv_label_create(conversation_timeline_scroll);
    lv_label_set_text(text_label, "");
    lv_label_set_long_mode(text_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(text_label, width - 52);
    lv_obj_set_height(text_label, LV_SIZE_CONTENT);
    lv_obj_set_pos(text_label, 0, 0);
    lv_obj_set_style_text_color(text_label, lv_color_hex(0xE8F0F2), 0);
    lv_obj_set_style_text_font(text_label, TRANSCRIPT_FONT, 0);
    lv_obj_set_style_text_line_space(text_label, 6, 0);
    lv_label_set_recolor(text_label, true);
    return text_label;
}

static void append_timeline_text(char *output, size_t capacity, const char *speaker, const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return;
    }
    const size_t used = strlen(output);
    if (used >= capacity - 1) {
        return;
    }
    const char *speaker_color = strcmp(speaker, APP_DISPLAY_NAME) == 0 ? "49E5C2" : "72A7FF";
    snprintf(output + used, capacity - used, "#%s ● %s#  %s\n",
             speaker_color, speaker, text);
}

static void copy_utf8_bounded(char *destination, size_t capacity, const char *source)
{
    if (capacity == 0) {
        return;
    }
    size_t length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1;
        while (length > 0 && (((unsigned char)source[length] & 0xC0) == 0x80)) {
            --length;
        }
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void render_conversation_timeline(bool sync_layout)
{
    if (conversation_transcript_label == NULL) {
        return;
    }
    transcript_timeline[0] = '\0';
    const char *user_role = conversation_language_chinese ? "我" : "ME";
    append_timeline_text(transcript_timeline, sizeof(transcript_timeline), user_role,
                         conversation_user_text);
    append_timeline_text(transcript_timeline, sizeof(transcript_timeline), APP_DISPLAY_NAME,
                         conversation_assistant_text);
    if (conversation_user_text[0] != '\0' || conversation_assistant_text[0] != '\0') {
        strlcat(transcript_timeline, "\n", sizeof(transcript_timeline));
    }
    for (size_t i = transcript_history_count; i > 0; --i) {
        const transcript_turn_t *turn = &transcript_history[i - 1];
        append_timeline_text(transcript_timeline, sizeof(transcript_timeline), user_role, turn->user);
        append_timeline_text(transcript_timeline, sizeof(transcript_timeline), APP_DISPLAY_NAME, turn->assistant);
        if (i > 1) {
            strlcat(transcript_timeline, "\n", sizeof(transcript_timeline));
        }
    }
    if (transcript_timeline[0] == '\0') {
        strlcpy(transcript_timeline,
                conversation_language_chinese ? "点击开始，体验实时语音对话。"
                                              : "Tap Start to begin a realtime conversation.",
                sizeof(transcript_timeline));
    }
    lv_label_set_text(conversation_transcript_label, transcript_timeline);
    if (sync_layout) {
        lv_obj_update_layout(conversation_timeline_scroll);
        lv_obj_scroll_to_y(conversation_timeline_scroll, 0, LV_ANIM_OFF);
    }
}

static void conversation_halo_opa_cb(void *object, int32_t value)
{
    lv_obj_set_style_border_opa((lv_obj_t *)object, (lv_opa_t)value, 0);
}

static void conversation_bar_height_cb(void *object, int32_t value)
{
    lv_obj_t *bar = (lv_obj_t *)object;
    lv_obj_set_height(bar, value);
    for (size_t i = 0; i < 5; ++i) {
        if (conversation_voice_bars[i] == bar) {
            lv_obj_align(bar, LV_ALIGN_CENTER, -22 + (int32_t)i * 11, 0);
            break;
        }
    }
}

static void start_conversation_animations(void)
{
    lv_anim_t pulse;
    lv_anim_init(&pulse);
    lv_anim_set_var(&pulse, conversation_orb_halo);
    lv_anim_set_exec_cb(&pulse, conversation_halo_opa_cb);
    lv_anim_set_values(&pulse, LV_OPA_20, LV_OPA_70);
    lv_anim_set_duration(&pulse, 1200);
    lv_anim_set_playback_duration(&pulse, 1200);
    lv_anim_set_repeat_count(&pulse, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&pulse);

    static const int32_t peak_heights[5] = {20, 36, 50, 32, 18};
    for (size_t i = 0; i < 5; ++i) {
        lv_anim_t wave;
        lv_anim_init(&wave);
        lv_anim_set_var(&wave, conversation_voice_bars[i]);
        lv_anim_set_exec_cb(&wave, conversation_bar_height_cb);
        lv_anim_set_values(&wave, 10, peak_heights[i]);
        lv_anim_set_duration(&wave, 420 + (uint32_t)i * 70);
        lv_anim_set_playback_duration(&wave, 420 + (uint32_t)i * 70);
        lv_anim_set_repeat_count(&wave, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&wave);
    }
}

static void conversation_button_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_RELEASED && conversation_action_callback != NULL) {
        ESP_LOGI(TAG, "conversation action released: stop=%d", conversation_action_is_stop);
        conversation_action_callback(conversation_action_is_stop, conversation_action_context);
    }
}

static void close_volume_sheet(void)
{
    if (conversation_volume_overlay != NULL) {
        lv_obj_delete(conversation_volume_overlay);
        conversation_volume_overlay = NULL;
        conversation_volume_value_label = NULL;
        conversation_volume_slider = NULL;
    }
}

static void volume_sheet_close_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        close_volume_sheet();
    }
}

static void volume_slider_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code != LV_EVENT_VALUE_CHANGED && code != LV_EVENT_RELEASED) {
        return;
    }
    lv_obj_t *slider = lv_event_get_target_obj(event);
    conversation_volume = (uint8_t)lv_slider_get_value(slider);
    if (conversation_volume_value_label != NULL) {
        char value[8];
        snprintf(value, sizeof(value), "%u%%", (unsigned)conversation_volume);
        lv_label_set_text(conversation_volume_value_label, value);
    }
    if (conversation_volume_callback != NULL) {
        conversation_volume_callback(conversation_volume, code == LV_EVENT_RELEASED,
                                     conversation_action_context);
    }
}

static void volume_sheet_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || conversation_volume_overlay != NULL ||
        conversation_voice_overlay != NULL) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    conversation_volume_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(conversation_volume_overlay);
    lv_obj_set_size(conversation_volume_overlay, 800, 480);
    lv_obj_set_pos(conversation_volume_overlay, 0, 0);
    lv_obj_set_style_bg_color(conversation_volume_overlay, lv_color_hex(0x020509), 0);
    lv_obj_set_style_bg_opa(conversation_volume_overlay, LV_OPA_70, 0);
    lv_obj_add_flag(conversation_volume_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(conversation_volume_overlay, volume_sheet_close_event_cb,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *sheet = lv_obj_create(conversation_volume_overlay);
    lv_obj_set_size(sheet, 480, 218);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(0x111A23), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sheet, lv_color_hex(0x2A3947), 0);
    lv_obj_set_style_border_width(sheet, 1, 0);
    lv_obj_set_style_radius(sheet, 30, 0);
    lv_obj_set_style_pad_all(sheet, 24, 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(sheet);
    lv_label_set_text(title, conversation_language_chinese ? "扬声器音量" : "SPEAKER VOLUME");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5F7FA), 0);
    lv_obj_set_style_text_font(title, conversation_language_chinese ? TRANSCRIPT_FONT
                                                                   : &lv_font_montserrat_20, 0);
    lv_obj_set_pos(title, 0, 4);

    conversation_volume_value_label = lv_label_create(sheet);
    char value[8];
    snprintf(value, sizeof(value), "%u%%", (unsigned)conversation_volume);
    lv_label_set_text(conversation_volume_value_label, value);
    lv_obj_set_style_text_color(conversation_volume_value_label, lv_color_hex(0x49E5C2), 0);
    lv_obj_set_style_text_font(conversation_volume_value_label, &lv_font_montserrat_20, 0);
    lv_obj_align(conversation_volume_value_label, LV_ALIGN_TOP_RIGHT, -50, 4);

    lv_obj_t *close = lv_button_create(sheet);
    lv_obj_set_size(close, 38, 38);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, -2);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x25313B), 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(close, volume_sheet_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_label, lv_color_hex(0xCAD3DA), 0);
    lv_obj_center(close_label);

    conversation_volume_slider = lv_slider_create(sheet);
    lv_obj_set_size(conversation_volume_slider, 384, 18);
    lv_obj_align(conversation_volume_slider, LV_ALIGN_CENTER, 0, 26);
    lv_slider_set_range(conversation_volume_slider, 0, 100);
    lv_slider_set_value(conversation_volume_slider, conversation_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(conversation_volume_slider, lv_color_hex(0x29343D),
                              LV_PART_MAIN);
    lv_obj_set_style_bg_color(conversation_volume_slider, lv_color_hex(0x0A84FF),
                              LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(conversation_volume_slider, lv_color_hex(0xF5F7FA),
                              LV_PART_KNOB);
    lv_obj_set_style_pad_all(conversation_volume_slider, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(conversation_volume_slider, volume_slider_event_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(conversation_volume_slider, volume_slider_event_cb,
                        LV_EVENT_RELEASED, NULL);

    lv_obj_t *low = lv_label_create(sheet);
    lv_label_set_text(low, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_color(low, lv_color_hex(0x71818C), 0);
    lv_obj_align(low, LV_ALIGN_BOTTOM_LEFT, 2, 0);
    lv_obj_t *high = lv_label_create(sheet);
    lv_label_set_text(high, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(high, lv_color_hex(0xDDE6EB), 0);
    lv_obj_align(high, LV_ALIGN_BOTTOM_RIGHT, -2, 0);
}

static void close_voice_sheet(void)
{
    if (conversation_voice_overlay != NULL) {
        lv_obj_delete(conversation_voice_overlay);
        conversation_voice_overlay = NULL;
    }
}

static void launch_voice_profiles_task(void)
{
    if (conversation_voice_profiles_loading) {
        return;
    }
    conversation_voice_profiles_loading = true;
    if (xTaskCreate(voice_profiles_task, "voice_profiles", 10240, NULL, 5, NULL) != pdPASS) {
        conversation_voice_profiles_loading = false;
        ESP_LOGE(TAG, "could not start voice profile task");
    }
}

static const char *voice_profile_title(const char *voice)
{
    for (size_t i = 0; i < conversation_voice_profiles.count; ++i) {
        if (strcmp(conversation_voice_profiles.names[i], voice) == 0) {
            return conversation_voice_profiles.titles[i][0] != '\0'
                       ? conversation_voice_profiles.titles[i]
                       : voice;
        }
    }
    return voice;
}

static void voice_profiles_task(void *arg)
{
    (void)arg;
    rti_voice_profile_list_t loaded = {0};
    const esp_err_t err = rti_session_list_voice_profiles(&loaded);
    if (bsp_display_lock(1000)) {
        conversation_voice_profiles_loading = false;
        conversation_voice_profiles_loaded = err == ESP_OK;
        if (err == ESP_OK) {
            conversation_voice_profiles = loaded;
            const char *selected = rti_session_get_voice_profile();
            bool selected_available = false;
            size_t selected_index = 0;
            const char *fallback = loaded.names[0];
            const char *fallback_title = loaded.titles[0];
            for (size_t i = 0; i < loaded.count; ++i) {
                if (strcmp(loaded.names[i], "Tina") == 0) {
                    fallback = loaded.names[i];
                    fallback_title = loaded.titles[i];
                }
                if (strcmp(loaded.names[i], selected) == 0) {
                    selected_available = true;
                    selected_index = i;
                }
            }
            const char *effective_voice = selected;
            if (!selected_available &&
                rti_session_set_voice_profile(fallback, fallback_title) == ESP_OK) {
                effective_voice = fallback;
                ESP_LOGW(TAG, "unavailable saved voice replaced with %s", fallback);
            } else if (selected_available &&
                       strcmp(rti_session_get_voice_title(), loaded.titles[selected_index]) != 0) {
                rti_session_set_voice_profile(selected, loaded.titles[selected_index]);
            }
            if (conversation_voice_chip_label != NULL) {
                lv_label_set_text(conversation_voice_chip_label,
                                  voice_profile_title(effective_voice));
            }
        } else {
            memset(&conversation_voice_profiles, 0, sizeof(conversation_voice_profiles));
        }
        bsp_display_unlock();
    }
    vTaskDelete(NULL);
}

static void voice_sheet_close_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        close_voice_sheet();
    }
}

static void voice_option_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    const char *voice = lv_event_get_user_data(event);
    const char *title = voice_profile_title(voice);
    if (rti_session_set_voice_profile(voice, title) != ESP_OK) {
        return;
    }
    if (conversation_voice_chip_label != NULL) {
        lv_label_set_text(conversation_voice_chip_label, title);
    }
    if (conversation_detail_label != NULL) {
        lv_label_set_text(conversation_detail_label,
                          conversation_action_is_stop
                              ? (conversation_language_chinese ? "下次对话使用新音色"
                                                               : "Voice changes next session")
                              : (conversation_language_chinese ? "已选择音色"
                                                               : "Voice selected"));
    }
    if (!conversation_action_is_stop) {
        strlcpy(conversation_assistant_text,
                conversation_language_chinese
                    ? APP_WELCOME_ZH
                    : APP_WELCOME_EN,
                sizeof(conversation_assistant_text));
        render_conversation_timeline(true);
    }
    close_voice_sheet();
}

static void voice_retry_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        close_voice_sheet();
        launch_voice_profiles_task();
    }
}

static const char *voice_profile_detail(const char *voice)
{
    if (conversation_language_chinese) {
        if (strncmp(voice, "rtiv_", 5) == 0) return "克隆音色";
        if (strcmp(voice, "Tina") == 0) return "温暖友好";
        if (strcmp(voice, "Cherry") == 0) return "明亮积极";
        if (strcmp(voice, "Serena") == 0) return "柔和自然";
        if (strcmp(voice, "Ethan") == 0) return "清晰有活力";
        if (strcmp(voice, "Chelsie") == 0) return "鲜明个性";
        return "系统音色";
    }
    if (strncmp(voice, "rtiv_", 5) == 0) return "Custom cloned voice";
    if (strcmp(voice, "Tina") == 0) return "Warm and friendly";
    if (strcmp(voice, "Cherry") == 0) return "Bright and positive";
    if (strcmp(voice, "Serena") == 0) return "Soft and gentle";
    if (strcmp(voice, "Ethan") == 0) return "Clear and energetic";
    if (strcmp(voice, "Chelsie") == 0) return "Distinct character voice";
    return "Voice profile";
}

static void voice_sheet_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || conversation_voice_overlay != NULL ||
        conversation_volume_overlay != NULL) {
        return;
    }

    lv_obj_t *screen = lv_screen_active();
    conversation_voice_overlay = lv_obj_create(screen);
    lv_obj_remove_style_all(conversation_voice_overlay);
    lv_obj_set_size(conversation_voice_overlay, 800, 480);
    lv_obj_set_pos(conversation_voice_overlay, 0, 0);
    lv_obj_set_style_bg_color(conversation_voice_overlay, lv_color_hex(0x020509), 0);
    lv_obj_set_style_bg_opa(conversation_voice_overlay, LV_OPA_70, 0);
    lv_obj_add_flag(conversation_voice_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(conversation_voice_overlay, voice_sheet_close_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sheet = lv_obj_create(conversation_voice_overlay);
    lv_obj_set_size(sheet, 520, 404);
    lv_obj_align(sheet, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_bg_color(sheet, lv_color_hex(0x111A23), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(sheet, lv_color_hex(0x2A3947), 0);
    lv_obj_set_style_border_width(sheet, 1, 0);
    lv_obj_set_style_radius(sheet, 30, 0);
    lv_obj_set_style_pad_all(sheet, 24, 0);
    lv_obj_clear_flag(sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *handle = lv_obj_create(sheet);
    lv_obj_remove_style_all(handle);
    lv_obj_set_size(handle, 46, 5);
    lv_obj_set_style_bg_color(handle, lv_color_hex(0x596672), 0);
    lv_obj_set_style_bg_opa(handle, LV_OPA_70, 0);
    lv_obj_set_style_radius(handle, 3, 0);
    lv_obj_align(handle, LV_ALIGN_TOP_MID, 0, -12);

    lv_obj_t *title = lv_label_create(sheet);
    lv_label_set_text(title, conversation_language_chinese ? "选择音色" : "CHOOSE A VOICE");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5F7FA), 0);
    lv_obj_set_style_text_font(title, conversation_language_chinese ? TRANSCRIPT_FONT
                                                                   : &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(title, 1, 0);
    lv_obj_set_pos(title, 2, 12);

    lv_obj_t *close = lv_button_create(sheet);
    lv_obj_set_size(close, 38, 38);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 4);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x25313B), 0);
    lv_obj_set_style_shadow_width(close, 0, 0);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_event_cb(close, voice_sheet_close_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_label = lv_label_create(close);
    lv_label_set_text(close_label, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(close_label, lv_color_hex(0xCAD3DA), 0);
    lv_obj_center(close_label);

    if (!conversation_voice_profiles_loaded) {
        lv_obj_t *message = lv_label_create(sheet);
        lv_label_set_text(message, conversation_voice_profiles_loading
                                       ? (conversation_language_chinese ? "正在加载音色..."
                                                                        : "Loading voices...")
                                       : (conversation_language_chinese ? "音色加载失败"
                                                                        : "Unable to load available voices"));
        lv_obj_set_style_text_color(message, lv_color_hex(0xA8B2BA), 0);
        lv_obj_set_style_text_font(message, conversation_language_chinese ? TRANSCRIPT_FONT
                                                                         : &lv_font_montserrat_20, 0);
        lv_obj_align(message, LV_ALIGN_CENTER, 0, -16);
        if (!conversation_voice_profiles_loading) {
            lv_obj_t *retry = lv_button_create(sheet);
            lv_obj_set_size(retry, 180, 48);
            lv_obj_align(retry, LV_ALIGN_CENTER, 0, 50);
            lv_obj_set_style_bg_color(retry, lv_color_hex(0x0A84FF), 0);
            lv_obj_set_style_radius(retry, 24, 0);
            lv_obj_set_style_shadow_width(retry, 0, 0);
            lv_obj_add_event_cb(retry, voice_retry_event_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_t *retry_label = lv_label_create(retry);
            lv_label_set_text(retry_label, conversation_language_chinese ? "重试" : "RETRY");
            lv_obj_set_style_text_font(retry_label, conversation_language_chinese ? TRANSCRIPT_FONT
                                                                                  : &lv_font_montserrat_14, 0);
            lv_obj_center(retry_label);
        }
        return;
    }

    lv_obj_t *list = lv_obj_create(sheet);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, 472, 304);
    lv_obj_set_pos(list, 0, 60);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x0A84FF), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);

    const char *current_voice = rti_session_get_voice_profile();
    for (size_t i = 0; i < conversation_voice_profiles.count; ++i) {
        const char *voice = conversation_voice_profiles.names[i];
        const char *title = conversation_voice_profiles.titles[i];
        const bool active = strcmp(current_voice, voice) == 0;
        lv_obj_t *voice_row = lv_button_create(list);
        lv_obj_set_size(voice_row, 468, 56);
        lv_obj_set_pos(voice_row, 0, (int32_t)i * 60);
        lv_obj_set_style_bg_color(voice_row, lv_color_hex(active ? 0x17302F : 0x17212A), 0);
        lv_obj_set_style_bg_opa(voice_row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(voice_row,
                                      lv_color_hex(active ? 0x3CCFB0 : 0x263541), 0);
        lv_obj_set_style_border_width(voice_row, active ? 2 : 1, 0);
        lv_obj_set_style_shadow_width(voice_row, 0, 0);
        lv_obj_set_style_radius(voice_row, 17, 0);
        lv_obj_add_event_cb(voice_row, voice_option_event_cb, LV_EVENT_CLICKED, (void *)voice);

        lv_obj_t *voice_dot = lv_obj_create(voice_row);
        lv_obj_remove_style_all(voice_dot);
        lv_obj_set_size(voice_dot, 32, 32);
        lv_obj_align(voice_dot, LV_ALIGN_LEFT_MID, -8, 0);
        lv_obj_set_style_bg_color(voice_dot,
                                  lv_color_hex(active ? 0x32B89D : 0x29404D), 0);
        lv_obj_set_style_bg_opa(voice_dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(voice_dot, LV_RADIUS_CIRCLE, 0);

        lv_obj_t *voice_name = lv_label_create(voice_row);
        lv_label_set_text(voice_name, title);
        lv_obj_set_pos(voice_name, 40, -5);
        lv_obj_set_width(voice_name, 210);
        lv_label_set_long_mode(voice_name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_color(voice_name, lv_color_hex(0xF5F7FA), 0);
        lv_obj_set_style_text_font(voice_name, conversation_language_chinese ? TRANSCRIPT_FONT
                                                                           : &lv_font_montserrat_20, 0);

        lv_obj_t *voice_detail = lv_label_create(voice_row);
        lv_label_set_text(voice_detail, voice_profile_detail(voice));
        lv_obj_set_pos(voice_detail, 270, -3);
        lv_obj_set_width(voice_detail, 150);
        lv_label_set_long_mode(voice_detail, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_color(voice_detail, lv_color_hex(0x84939E), 0);
        lv_obj_set_style_text_font(voice_detail, conversation_language_chinese ? TRANSCRIPT_FONT
                                                                             : &lv_font_montserrat_14, 0);

        if (active) {
            lv_obj_t *selected = lv_label_create(voice_row);
            lv_label_set_text(selected, LV_SYMBOL_OK);
            lv_obj_align(selected, LV_ALIGN_RIGHT_MID, 6, 0);
            lv_obj_set_style_text_color(selected, lv_color_hex(0x49E5C2), 0);
        }
    }
}

static lv_color_t conversation_state_color(board_conversation_state_t state)
{
    switch (state) {
    case BOARD_CONVERSATION_ACTIVE:
        return lv_color_hex(0x48E0C2);
    case BOARD_CONVERSATION_STARTING:
    case BOARD_CONVERSATION_STOPPING:
        return lv_color_hex(0xFFB45C);
    case BOARD_CONVERSATION_ERROR:
        return lv_color_hex(0xFF6B6B);
    case BOARD_CONVERSATION_IDLE:
    default:
        return lv_color_hex(0x7FA3B2);
    }
}

static const char *conversation_hero_text(board_conversation_state_t state, const char *detail)
{
    if (state == BOARD_CONVERSATION_ACTIVE && detail != NULL) {
        if (strstr(detail, "responding") != NULL) {
            return conversation_language_chinese ? "正在回复" : "RESPONDING";
        }
        if (strstr(detail, "Understanding") != NULL) {
            return conversation_language_chinese ? "正在思考" : "THINKING";
        }
        if (strstr(detail, "Listening to you") != NULL) {
            return conversation_language_chinese ? "正在聆听你" : "LISTENING TO YOU";
        }
        return conversation_language_chinese ? "正在聆听" : "LISTENING";
    }
    switch (state) {
    case BOARD_CONVERSATION_STARTING:
        return conversation_language_chinese ? "正在连接" : "CONNECTING";
    case BOARD_CONVERSATION_STOPPING:
        return conversation_language_chinese ? "正在结束" : "CLOSING";
    case BOARD_CONVERSATION_ERROR:
        return conversation_language_chinese ? "出现问题" : "NEEDS ATTENTION";
    case BOARD_CONVERSATION_IDLE:
    default:
        return conversation_language_chinese ? "准备就绪" : "READY";
    }
}

static void conversation_detail_text(board_conversation_state_t state, const char *detail,
                                     char *output, size_t capacity)
{
    if (state == BOARD_CONVERSATION_ACTIVE && detail != NULL) {
        if (strstr(detail, "responding") != NULL) {
            strlcpy(output, conversation_language_chinese ? "你可以随时打断"
                                                          : "You can interrupt anytime", capacity);
        } else if (strstr(detail, "Understanding") != NULL) {
            strlcpy(output, conversation_language_chinese ? "正在理解并组织回答"
                                                          : "Preparing a response", capacity);
        } else if (strstr(detail, "Listening to you") != NULL) {
            strlcpy(output, conversation_language_chinese ? "可以继续说话"
                                                          : "Capturing your voice", capacity);
        } else {
            strlcpy(output, conversation_language_chinese ? "请开始说话"
                                                          : "Ready for your voice", capacity);
        }
        return;
    }
    if (state == BOARD_CONVERSATION_IDLE) {
        strlcpy(output, conversation_language_chinese ? "点击开始进行语音对话"
                                                      : "Tap below to start", capacity);
    } else if (state == BOARD_CONVERSATION_STARTING) {
        strlcpy(output, conversation_language_chinese ? "正在创建安全会话"
                                                      : "Creating secure session", capacity);
    } else if (state == BOARD_CONVERSATION_STOPPING) {
        strlcpy(output, conversation_language_chinese ? "正在关闭安全会话"
                                                      : "Closing secure session", capacity);
    } else {
        strlcpy(output, detail == NULL ? "" : detail, capacity);
    }
}

static void apply_conversation_language(void)
{
    if (conversation_device_label == NULL) {
        return;
    }
    const lv_font_t *ui_font = conversation_language_chinese
                                   ? TRANSCRIPT_FONT
                                   : &lv_font_montserrat_14;
    lv_label_set_text(conversation_device_label, APP_POWERED_BY);
    lv_obj_set_style_text_font(conversation_device_label, &lv_font_montserrat_14, 0);
    lv_label_set_text(conversation_online_label,
                      conversation_language_chinese ? "在线"
                                                    : LV_SYMBOL_WIFI " ONLINE");
    lv_obj_set_style_text_font(conversation_online_label, ui_font, 0);
    lv_label_set_text(conversation_activity_title_label,
                      conversation_language_chinese ? "语音状态" : "VOICE ACTIVITY");
    lv_obj_set_style_text_font(conversation_activity_title_label, ui_font, 0);
    lv_label_set_text(conversation_panel_title_label,
                      conversation_language_chinese ? "最近对话" : "RECENT CONVERSATION");
    lv_obj_set_style_text_font(conversation_panel_title_label, ui_font, 0);
    lv_label_set_text(conversation_language_label, conversation_language_chinese ? "中" : "EN");
    lv_obj_set_style_text_font(conversation_language_label,
                               conversation_language_chinese ? TRANSCRIPT_FONT
                                                             : &lv_font_montserrat_14, 0);

    lv_label_set_text(conversation_voice_chip_label, rti_session_get_voice_title());
    lv_obj_set_style_text_font(conversation_voice_chip_label, ui_font, 0);

    lv_label_set_text(conversation_status_label,
                      conversation_hero_text(conversation_current_state,
                                             conversation_current_detail));
    lv_obj_set_style_text_font(conversation_status_label,
                               conversation_language_chinese ? TRANSCRIPT_FONT
                                                             : &lv_font_montserrat_20, 0);
    char detail[96];
    conversation_detail_text(conversation_current_state, conversation_current_detail,
                             detail, sizeof(detail));
    lv_label_set_text(conversation_detail_label, detail);
    lv_obj_set_style_text_font(conversation_detail_label, ui_font, 0);
    lv_label_set_text(conversation_action_label,
                      conversation_action_is_stop
                          ? (conversation_language_chinese ? "结束对话"
                                                           : LV_SYMBOL_STOP "  END SESSION")
                          : (conversation_language_chinese ? "开始对话"
                                                           : LV_SYMBOL_PLAY "  START SESSION"));
    lv_obj_set_style_text_font(conversation_action_label,
                               conversation_language_chinese ? TRANSCRIPT_FONT
                                                             : &lv_font_montserrat_20, 0);
    if (!conversation_action_is_stop && transcript_history_count == 0) {
        strlcpy(conversation_assistant_text,
                conversation_language_chinese
                    ? APP_WELCOME_ZH
                    : APP_WELCOME_EN,
                sizeof(conversation_assistant_text));
    }
    render_conversation_timeline(true);
}

static void language_toggle_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    conversation_language_chinese = !conversation_language_chinese;
    save_conversation_language();
    apply_conversation_language();
}

esp_err_t board_display_init(void)
{
    const bsp_display_cfg_t display_config = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * 40,
        .double_buffer = false,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        },
    };
    lv_display_t *display = bsp_display_start_with_config(&display_config);
    if (display == NULL) {
        ESP_LOGE(TAG, "display and LVGL initialization failed");
        return ESP_FAIL;
    }

    if (!bsp_display_lock(1000)) {
        ESP_LOGE(TAG, "timed out waiting for LVGL lock");
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x071116), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x102630), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);

    lv_obj_t *eyebrow = lv_label_create(screen);
    lv_label_set_text(eyebrow, "语音协作终端");
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(0x48E0C2), 0);
    lv_obj_set_style_text_font(eyebrow, TRANSCRIPT_FONT, 0);
    lv_obj_set_style_text_letter_space(eyebrow, 2, 0);
    lv_obj_align(eyebrow, LV_ALIGN_TOP_MID, 0, 72);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, APP_DISPLAY_NAME);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5FAFB), 0);
    lv_obj_set_style_text_font(title, &shenglu_brand_48, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 125);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text(subtitle, "小组语音，随时同行");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x93A9B2), 0);
    lv_obj_set_style_text_font(subtitle, TRANSCRIPT_FONT, 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 202);

    lv_obj_t *divider = lv_obj_create(screen);
    lv_obj_remove_style_all(divider);
    lv_obj_set_size(divider, 520, 2);
    lv_obj_set_style_bg_color(divider, lv_color_hex(0x28414B), 0);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, 0);
    lv_obj_align(divider, LV_ALIGN_TOP_MID, 0, 265);

    create_status_chip(screen, "AUDIO READY", lv_color_hex(0x48E0C2), 195);
    create_status_chip(screen, "WIFI SETUP NEEDED", lv_color_hex(0xFFB45C), 415);

    lv_obj_t *footer = lv_label_create(screen);
    lv_label_set_text(footer, APP_POWERED_BY);
    lv_obj_set_style_text_font(footer, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(0x607983), 0);
    lv_obj_set_style_text_letter_space(footer, 2, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -54);

    bsp_display_unlock();

    esp_err_t err = bsp_display_backlight_on();
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "could not enable display backlight: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "%s status screen ready: 800x480", APP_DISPLAY_NAME);
    return ESP_OK;
}

esp_err_t board_display_show_wifi_setup(board_wifi_connected_cb_t callback, void *context)
{
    wifi_connected_callback = callback;
    wifi_connected_context = context;
    start_scan();
    return ESP_OK;
}

esp_err_t board_display_show_wifi_connected(void)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    show_message_screen("WIFI CONNECTED", "Saved network connected. Voice is ready.", false);
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t board_display_show_conversation(board_conversation_action_cb_t callback,
                                          board_volume_action_cb_t volume_callback,
                                          uint8_t initial_volume,
                                          void *context)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    conversation_action_callback = callback;
    conversation_volume_callback = volume_callback;
    conversation_action_context = context;
    conversation_action_is_stop = false;
    conversation_voice_overlay = NULL;
    conversation_volume_overlay = NULL;
    conversation_volume = initial_volume <= 100 ? initial_volume : 55;
    conversation_voice_profiles_loaded = false;
    conversation_voice_profiles_loading = false;
    memset(&conversation_voice_profiles, 0, sizeof(conversation_voice_profiles));
    load_conversation_language();
    conversation_current_state = BOARD_CONVERSATION_IDLE;
    conversation_current_detail[0] = '\0';

    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x030507), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x0D151D), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);

    lv_obj_t *ambient_glow = lv_obj_create(screen);
    lv_obj_remove_style_all(ambient_glow);
    lv_obj_set_size(ambient_glow, 290, 290);
    lv_obj_set_pos(ambient_glow, -92, 118);
    lv_obj_set_style_bg_color(ambient_glow, lv_color_hex(0x174664), 0);
    lv_obj_set_style_bg_opa(ambient_glow, LV_OPA_10, 0);
    lv_obj_set_style_radius(ambient_glow, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *brand_dot = lv_obj_create(screen);
    lv_obj_remove_style_all(brand_dot);
    lv_obj_set_size(brand_dot, 10, 10);
    lv_obj_set_pos(brand_dot, 24, 25);
    lv_obj_set_style_bg_color(brand_dot, lv_color_hex(0x49E5C2), 0);
    lv_obj_set_style_bg_opa(brand_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(brand_dot, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *brand = lv_label_create(screen);
    lv_label_set_text(brand, APP_DISPLAY_NAME);
    lv_obj_set_pos(brand, 44, 18);
    lv_obj_set_style_text_color(brand, lv_color_hex(0xF4FAFC), 0);
    lv_obj_set_style_text_font(brand, &shenglu_brand_24, 0);
    lv_obj_set_style_text_letter_space(brand, 2, 0);

    conversation_device_label = lv_label_create(screen);
    lv_label_set_text(conversation_device_label, "");
    lv_obj_set_pos(conversation_device_label, 44, 48);
    lv_obj_set_style_text_color(conversation_device_label, lv_color_hex(0x67808D), 0);
    lv_obj_set_style_text_letter_space(conversation_device_label, 1, 0);

    conversation_language_chip = lv_button_create(screen);
    lv_obj_set_size(conversation_language_chip, 58, 36);
    lv_obj_set_pos(conversation_language_chip, 432, 18);
    lv_obj_set_style_bg_color(conversation_language_chip, lv_color_hex(0x171A1F), 0);
    lv_obj_set_style_bg_opa(conversation_language_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(conversation_language_chip, lv_color_hex(0x34383E), 0);
    lv_obj_set_style_border_width(conversation_language_chip, 1, 0);
    lv_obj_set_style_shadow_width(conversation_language_chip, 0, 0);
    lv_obj_set_style_radius(conversation_language_chip, 18, 0);
    lv_obj_add_event_cb(conversation_language_chip, language_toggle_event_cb, LV_EVENT_CLICKED, NULL);
    conversation_language_label = lv_label_create(conversation_language_chip);
    lv_obj_set_style_text_color(conversation_language_label, lv_color_hex(0xA9B6BE), 0);
    lv_obj_center(conversation_language_label);

    lv_obj_t *voice_chip = lv_button_create(screen);
    lv_obj_set_size(voice_chip, 116, 36);
    lv_obj_set_pos(voice_chip, 502, 18);
    lv_obj_set_style_bg_color(voice_chip, lv_color_hex(0x171A1F), 0);
    lv_obj_set_style_bg_opa(voice_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(voice_chip, lv_color_hex(0x34383E), 0);
    lv_obj_set_style_border_width(voice_chip, 1, 0);
    lv_obj_set_style_shadow_width(voice_chip, 0, 0);
    lv_obj_set_style_radius(voice_chip, 18, 0);
    lv_obj_add_event_cb(voice_chip, voice_sheet_event_cb, LV_EVENT_CLICKED, NULL);

    conversation_voice_chip_label = lv_label_create(voice_chip);
    char voice_chip_text[48];
    snprintf(voice_chip_text, sizeof(voice_chip_text), "%s", rti_session_get_voice_title());
    lv_label_set_text(conversation_voice_chip_label, voice_chip_text);
    lv_obj_set_style_text_color(conversation_voice_chip_label, lv_color_hex(0xE7EDF1), 0);
    lv_obj_set_style_text_font(conversation_voice_chip_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_letter_space(conversation_voice_chip_label, 1, 0);
    lv_obj_set_width(conversation_voice_chip_label, 96);
    lv_label_set_long_mode(conversation_voice_chip_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(conversation_voice_chip_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(conversation_voice_chip_label);

    lv_obj_t *volume_chip = lv_button_create(screen);
    lv_obj_set_size(volume_chip, 36, 36);
    lv_obj_set_pos(volume_chip, 630, 18);
    lv_obj_set_style_bg_color(volume_chip, lv_color_hex(0x171A1F), 0);
    lv_obj_set_style_bg_opa(volume_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(volume_chip, lv_color_hex(0x34383E), 0);
    lv_obj_set_style_border_width(volume_chip, 1, 0);
    lv_obj_set_style_shadow_width(volume_chip, 0, 0);
    lv_obj_set_style_radius(volume_chip, 18, 0);
    lv_obj_add_event_cb(volume_chip, volume_sheet_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *volume_icon = lv_label_create(volume_chip);
    lv_label_set_text(volume_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(volume_icon, lv_color_hex(0xA9B6BE), 0);
    lv_obj_center(volume_icon);

    lv_obj_t *online_chip = lv_obj_create(screen);
    lv_obj_remove_style_all(online_chip);
    lv_obj_set_size(online_chip, 106, 36);
    lv_obj_set_pos(online_chip, 678, 18);
    lv_obj_set_style_bg_color(online_chip, lv_color_hex(0x141D1B), 0);
    lv_obj_set_style_bg_opa(online_chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(online_chip, lv_color_hex(0x1D4E4D), 0);
    lv_obj_set_style_border_width(online_chip, 1, 0);
    lv_obj_set_style_radius(online_chip, 18, 0);

    conversation_online_label = lv_label_create(online_chip);
    lv_obj_set_style_text_color(conversation_online_label, lv_color_hex(0x49E5C2), 0);
    lv_obj_set_style_text_letter_space(conversation_online_label, 1, 0);
    lv_obj_center(conversation_online_label);

    lv_obj_t *visual_panel = lv_obj_create(screen);
    lv_obj_remove_style_all(visual_panel);
    lv_obj_set_size(visual_panel, 202, 304);
    lv_obj_set_pos(visual_panel, 24, 78);
    lv_obj_set_style_bg_color(visual_panel, lv_color_hex(0x12161B), 0);
    lv_obj_set_style_bg_opa(visual_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(visual_panel, lv_color_hex(0x2A3037), 0);
    lv_obj_set_style_border_width(visual_panel, 1, 0);
    lv_obj_set_style_radius(visual_panel, 26, 0);

    conversation_activity_title_label = lv_label_create(visual_panel);
    lv_obj_set_pos(conversation_activity_title_label, 20, 16);
    lv_obj_set_style_text_color(conversation_activity_title_label, lv_color_hex(0x7D8790), 0);
    lv_obj_set_style_text_letter_space(conversation_activity_title_label, 1, 0);

    conversation_orb_halo = lv_obj_create(visual_panel);
    lv_obj_remove_style_all(conversation_orb_halo);
    lv_obj_set_size(conversation_orb_halo, 96, 96);
    lv_obj_align(conversation_orb_halo, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_border_color(conversation_orb_halo, lv_color_hex(0x49E5C2), 0);
    lv_obj_set_style_border_width(conversation_orb_halo, 2, 0);
    lv_obj_set_style_border_opa(conversation_orb_halo, LV_OPA_40, 0);
    lv_obj_set_style_radius(conversation_orb_halo, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *middle_ring = lv_obj_create(visual_panel);
    lv_obj_remove_style_all(middle_ring);
    lv_obj_set_size(middle_ring, 76, 76);
    lv_obj_align(middle_ring, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_color(middle_ring, lv_color_hex(0x0C2A36), 0);
    lv_obj_set_style_bg_opa(middle_ring, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(middle_ring, lv_color_hex(0x286876), 0);
    lv_obj_set_style_border_width(middle_ring, 1, 0);
    lv_obj_set_style_radius(middle_ring, LV_RADIUS_CIRCLE, 0);

    conversation_orb_core = lv_obj_create(visual_panel);
    lv_obj_remove_style_all(conversation_orb_core);
    lv_obj_set_size(conversation_orb_core, 58, 58);
    lv_obj_align(conversation_orb_core, LV_ALIGN_TOP_MID, 0, 73);
    lv_obj_set_style_bg_color(conversation_orb_core, lv_color_hex(0x123F49), 0);
    lv_obj_set_style_bg_grad_color(conversation_orb_core, lv_color_hex(0x1B776D), 0);
    lv_obj_set_style_bg_grad_dir(conversation_orb_core, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(conversation_orb_core, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(conversation_orb_core, LV_RADIUS_CIRCLE, 0);

    for (size_t i = 0; i < 5; ++i) {
        conversation_voice_bars[i] = lv_obj_create(conversation_orb_core);
        lv_obj_remove_style_all(conversation_voice_bars[i]);
        lv_obj_set_size(conversation_voice_bars[i], 5, 10);
        lv_obj_set_style_bg_color(conversation_voice_bars[i], lv_color_hex(0xE9FFFA), 0);
        lv_obj_set_style_bg_opa(conversation_voice_bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(conversation_voice_bars[i], 3, 0);
        lv_obj_align(conversation_voice_bars[i], LV_ALIGN_CENTER, -22 + (int32_t)i * 11, 0);
    }

    conversation_status_label = lv_label_create(screen);
    lv_label_set_text(conversation_status_label, "READY");
    lv_label_set_long_mode(conversation_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(conversation_status_label, 190, 28);
    lv_obj_set_pos(conversation_status_label, 30, 242);
    lv_obj_set_style_text_align(conversation_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(conversation_status_label, lv_color_hex(0xF4FAFC), 0);
    lv_obj_set_style_text_font(conversation_status_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_letter_space(conversation_status_label, 1, 0);

    conversation_detail_label = lv_label_create(screen);
    lv_label_set_text(conversation_detail_label, "Connected and ready");
    lv_label_set_long_mode(conversation_detail_label, LV_LABEL_LONG_DOT);
    lv_obj_set_size(conversation_detail_label, 190, 20);
    lv_obj_set_pos(conversation_detail_label, 30, 278);
    lv_obj_set_style_text_align(conversation_detail_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(conversation_detail_label, lv_color_hex(0x718B96), 0);
    lv_obj_set_style_text_font(conversation_detail_label, &lv_font_montserrat_14, 0);

    conversation_transcript_label = create_conversation_panel(screen, "",
                                                              242, 78, 534, 304,
                                                              lv_color_hex(0x0A84FF));
    memset(transcript_history, 0, sizeof(transcript_history));
    transcript_history_count = 0;
    conversation_user_text[0] = '\0';
    strlcpy(conversation_assistant_text,
            conversation_language_chinese
                ? APP_WELCOME_ZH
                : APP_WELCOME_EN,
            sizeof(conversation_assistant_text));

    conversation_action_button = lv_button_create(screen);
    lv_obj_set_size(conversation_action_button, 752, 58);
    lv_obj_set_pos(conversation_action_button, 24, 398);
    lv_obj_set_style_bg_color(conversation_action_button, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_bg_grad_color(conversation_action_button, lv_color_hex(0x38A4FF), 0);
    lv_obj_set_style_bg_grad_dir(conversation_action_button, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_shadow_color(conversation_action_button, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_shadow_opa(conversation_action_button, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(conversation_action_button, 14, 0);
    lv_obj_set_style_radius(conversation_action_button, 29, 0);
    lv_obj_add_event_cb(conversation_action_button, conversation_button_event_cb,
                        LV_EVENT_RELEASED, NULL);

    conversation_action_label = lv_label_create(conversation_action_button);
    lv_label_set_text(conversation_action_label, LV_SYMBOL_PLAY "  START SESSION");
    lv_obj_set_style_text_font(conversation_action_label, &lv_font_montserrat_20, 0);
    lv_obj_center(conversation_action_label);

    apply_conversation_language();

    start_conversation_animations();

    bsp_display_unlock();
    launch_voice_profiles_task();
    return ESP_OK;
}

esp_err_t board_display_set_conversation_state(board_conversation_state_t state,
                                               const char *detail,
                                               bool session_active)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (conversation_status_label == NULL || conversation_detail_label == NULL ||
        conversation_action_button == NULL || conversation_action_label == NULL) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    const bool busy = state == BOARD_CONVERSATION_STARTING || state == BOARD_CONVERSATION_STOPPING;
    const lv_color_t color = conversation_state_color(state);
    conversation_current_state = state;
    strlcpy(conversation_current_detail, detail == NULL ? "" : detail,
            sizeof(conversation_current_detail));
    lv_label_set_text(conversation_status_label, conversation_hero_text(state, detail));
    lv_obj_set_style_text_color(conversation_status_label,
                                state == BOARD_CONVERSATION_ERROR ? color : lv_color_hex(0xF4FAFC), 0);
    char short_detail[96];
    conversation_detail_text(state, detail, short_detail, sizeof(short_detail));
    lv_label_set_text(conversation_detail_label, short_detail);
    lv_obj_set_style_border_color(conversation_orb_halo, color, 0);
    lv_obj_set_style_bg_color(conversation_orb_core, lv_color_mix(color, lv_color_hex(0x09202A), LV_OPA_40), 0);
    lv_obj_set_style_bg_grad_color(conversation_orb_core, color, 0);
    for (size_t i = 0; i < 5; ++i) {
        lv_obj_set_style_bg_opa(conversation_voice_bars[i],
                                state == BOARD_CONVERSATION_ACTIVE ? LV_OPA_COVER : LV_OPA_50, 0);
    }

    conversation_action_is_stop = session_active;
    lv_label_set_text(conversation_action_label,
                      session_active
                          ? (conversation_language_chinese ? "结束对话"
                                                           : LV_SYMBOL_STOP "  END SESSION")
                          : (conversation_language_chinese ? "开始对话"
                                                           : LV_SYMBOL_PLAY "  START SESSION"));
    lv_obj_set_style_bg_color(conversation_action_button,
                              session_active ? lv_color_hex(0xD84B55) : lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_bg_grad_color(conversation_action_button,
                                   session_active ? lv_color_hex(0xF06B6F) : lv_color_hex(0x38A4FF), 0);
    if (busy) {
        lv_obj_add_state(conversation_action_button, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(conversation_action_button, LV_STATE_DISABLED);
    }
    if (session_active) {
        lv_obj_add_state(conversation_language_chip, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(conversation_language_chip, LV_STATE_DISABLED);
    }

    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t board_display_set_transcript(board_transcript_role_t role, const char *text, bool streaming)
{
    if (text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    char bounded[TRANSCRIPT_TEXT_MAX + 1];
    const size_t length = strlen(text);
    if (length <= TRANSCRIPT_TEXT_MAX) {
        strlcpy(bounded, text, sizeof(bounded));
    } else {
        size_t copy_length = TRANSCRIPT_TEXT_MAX - 3;
        while (copy_length > 0 && (((unsigned char)text[copy_length] & 0xC0) == 0x80)) {
            --copy_length;
        }
        memcpy(bounded, text, copy_length);
        memcpy(bounded + copy_length, "...", 4);
    }

    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (conversation_transcript_label == NULL) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    char *current = role == BOARD_TRANSCRIPT_USER ? conversation_user_text : conversation_assistant_text;
    strlcpy(current, bounded, TRANSCRIPT_TEXT_MAX + 1);
    render_conversation_timeline(!streaming);
    lv_obj_set_style_text_opa(conversation_transcript_label,
                              streaming ? LV_OPA_80 : LV_OPA_COVER, 0);
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t board_display_commit_transcript_turn(void)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (conversation_transcript_label == NULL ||
        (conversation_user_text[0] == '\0' && conversation_assistant_text[0] == '\0')) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (conversation_user_text[0] == '\0') {
        render_conversation_timeline(true);
        bsp_display_unlock();
        return ESP_OK;
    }
    if (conversation_assistant_text[0] != '\0' && transcript_history_count > 0 &&
        strcmp(transcript_history[transcript_history_count - 1].assistant,
               conversation_assistant_text) == 0) {
        conversation_user_text[0] = '\0';
        conversation_assistant_text[0] = '\0';
        render_conversation_timeline(true);
        bsp_display_unlock();
        return ESP_OK;
    }
    if (transcript_history_count == TRANSCRIPT_HISTORY_TURNS) {
        memmove(&transcript_history[0], &transcript_history[1],
                sizeof(transcript_history[0]) * (TRANSCRIPT_HISTORY_TURNS - 1));
        --transcript_history_count;
    }
    transcript_turn_t *turn = &transcript_history[transcript_history_count++];
    copy_utf8_bounded(turn->user, sizeof(turn->user), conversation_user_text);
    copy_utf8_bounded(turn->assistant, sizeof(turn->assistant), conversation_assistant_text);
    conversation_user_text[0] = '\0';
    conversation_assistant_text[0] = '\0';
    render_conversation_timeline(true);
    lv_obj_set_style_text_opa(conversation_transcript_label, LV_OPA_COVER, 0);
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t board_display_reset_transcript(void)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    memset(transcript_history, 0, sizeof(transcript_history));
    transcript_history_count = 0;
    conversation_user_text[0] = '\0';
    conversation_assistant_text[0] = '\0';
    render_conversation_timeline(true);
    bsp_display_unlock();
    return ESP_OK;
}

/* Group mode owns a bounded PSRAM copy of its choices. No controller-owned
 * pointers or cJSON nodes survive a publication. Only the worker publishes;
 * callbacks merely queue intent while the existing display lock is held. */
typedef struct {
    size_t count;
    uint32_t generation;
    bool known;
    bool can_select;
    bool switching;
    char current_id[81];
    board_group_item_t items[];
} group_choices_t;

static lv_obj_t *group_name_label;
static lv_obj_t *group_detail_label;
static lv_obj_t *group_status_label;
static lv_obj_t *group_ptt_button;
static lv_obj_t *group_ptt_label;
static lv_obj_t *group_action_label;
static lv_obj_t *group_volume_label;
static lv_obj_t *group_dropdown;
static lv_obj_t *group_switch_button;
static lv_obj_t *group_switch_label;
static lv_obj_t *group_join_button;
static lv_obj_t *group_voice_button;
static lv_obj_t *group_command_overlay;
static lv_obj_t *group_command_title;
static lv_obj_t *group_command_detail;
static lv_obj_t *group_command_submit;
static lv_obj_t *group_command_submit_label;
static lv_obj_t *group_command_cancel;
static lv_obj_t *group_command_cancel_label;
static lv_obj_t *group_join_textarea;
static board_group_join_action_cb_t group_join_callback;
static board_group_voice_action_cb_t group_voice_callback;
static void *group_command_context;
typedef enum { GROUP_MODAL_NONE, GROUP_MODAL_JOIN, GROUP_MODAL_VOICE } group_modal_t;
static group_modal_t group_modal;
static bool group_joining;
static bool group_voice_active;
static bool group_voice_can_cancel;
static bool group_command_pending;
static bool group_voice_submitted;
static uint32_t group_command_revision;
static uint32_t group_command_pending_revision;
static char group_command_detail_text[192];
static board_conversation_action_cb_t group_action_callback;
static board_ptt_action_cb_t group_ptt_callback;
static board_group_switch_action_cb_t group_switch_callback;
static board_volume_action_cb_t group_volume_callback;
static void *group_context;
static group_choices_t *group_choices;
static char *group_choice_options;
static bool group_choices_valid;
static bool group_candidate_valid;
static bool group_switch_pending;
static uint32_t group_pending_generation;
static bool group_connected;
static bool group_transport_ready;
static bool group_speaking;
static bool group_pressed;
static bool group_stop_action;

static void group_render_controls(void);
static void group_render_command(void);
static void group_close_command(void);
static lv_obj_t *group_label(lv_obj_t *parent, const char *text, int x, int y, int width, uint32_t color);

static bool group_command_busy(void)
{
    return group_joining || group_voice_active || group_command_pending;
}

static void group_cancel_ptt(void)
{
    if (group_pressed) {
        group_pressed = false;
        if (group_ptt_callback) {
            group_ptt_callback(false, group_context);
        }
    }
}

static void group_force_release(void)
{
    group_pressed = false;
    group_speaking = false;
    /* USB and touch share the controller. Opening the selector must also
     * cancel an externally initiated press which has no local touch flag. */
    if (group_ptt_callback) {
        group_ptt_callback(false, group_context);
    }
}

static int group_current_index(void)
{
    if (!group_choices_valid || !group_choices || !group_choices->known) {
        return -1;
    }
    for (size_t i = 0; i < group_choices->count; ++i) {
        if (strcmp(group_choices->items[i].id, group_choices->current_id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool group_is_switching(void)
{
    return group_switch_pending || (group_choices && group_choices->switching);
}

static bool group_can_choose(void)
{
    return group_choices_valid && group_choices && group_choices->known &&
           group_choices->can_select && group_choices->count &&
           group_stop_action && !group_is_switching() && !group_command_busy() &&
           group_modal == GROUP_MODAL_NONE;
}

static void group_render_controls(void)
{
    if (!group_ptt_button || !group_dropdown || !group_switch_button) {
        return;
    }
    const bool selecting = lv_dropdown_is_open(group_dropdown);
    const bool command_ready = group_choices_valid && group_choices && group_choices->known &&
                               group_stop_action && !group_is_switching() && !group_command_busy();
    if (group_join_button) {
        if (command_ready && group_join_callback && group_modal == GROUP_MODAL_NONE) {
            lv_obj_remove_state(group_join_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(group_join_button, LV_STATE_DISABLED);
        }
    }
    if (group_voice_button) {
        if (command_ready && group_transport_ready && group_current_index() >= 0 &&
            group_voice_callback && group_modal == GROUP_MODAL_NONE) {
            lv_obj_remove_state(group_voice_button, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(group_voice_button, LV_STATE_DISABLED);
        }
    }
    const bool choosing = group_can_choose();
    const uint32_t selected = lv_dropdown_get_selected(group_dropdown);
    const bool different = group_candidate_valid && group_choices &&
                           selected < group_choices->count &&
                           strcmp(group_choices->items[selected].id, group_choices->current_id) != 0;
    if (choosing) {
        lv_obj_remove_state(group_dropdown, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(group_dropdown, LV_STATE_DISABLED);
    }
    if (choosing && different && !selecting && group_switch_callback) {
        lv_obj_remove_state(group_switch_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(group_switch_button, LV_STATE_DISABLED);
    }
    lv_label_set_text(group_switch_label, group_is_switching() ? "切换中" : "切换群组");
    group_connected = group_transport_ready && group_stop_action &&
                      group_current_index() >= 0 && !group_is_switching() && !selecting &&
                      !group_command_busy() && group_modal == GROUP_MODAL_NONE;
    if (!group_connected) {
        group_cancel_ptt();
        lv_obj_add_state(group_ptt_button, LV_STATE_DISABLED);
    } else {
        lv_obj_remove_state(group_ptt_button, LV_STATE_DISABLED);
    }
    lv_label_set_text(group_status_label,
                      group_joining ? "正在加入群组" :
                      group_voice_active ? "语音切群进行中" :
                      group_command_pending ? "正在提交操作" :
                      group_is_switching() ? "正在切换群组" :
                      !group_stop_action ? "已停止" :
                      !group_connected ? "群音频尚未就绪" :
                      group_speaking ? "持有麦权，正在发言" : "群语音已就绪");
    lv_label_set_text(group_ptt_label,
                      group_command_busy() ? "操作完成后可发言" :
                      group_modal != GROUP_MODAL_NONE ? "关闭窗口后可发言" :
                      group_is_switching() ? "切换完成后可发言" :
                      !group_connected ? "连接后可发言" :
                      group_speaking ? "松手停止发言" :
                      group_pressed ? "正在申请麦权" : "按住说话");
}

static void group_render_current(void)
{
    const int current = group_current_index();
    if (current >= 0) {
        char text[176];
        snprintf(text, sizeof(text), "当前群：%s", group_choices->items[current].name);
        lv_label_set_text(group_name_label, text);
    } else if (!group_choices_valid || !group_choices || !group_choices->known) {
        lv_label_set_text(group_name_label, "当前群：尚未同步");
    } else if (!group_choices->count) {
        lv_label_set_text(group_name_label, "尚未加入群组");
    } else if (!group_choices->current_id[0]) {
        lv_label_set_text(group_name_label, "尚未选择当前群");
    } else {
        lv_label_set_text(group_name_label, "当前群待确认，请等待同步");
    }
}

static void group_ptt_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED && group_connected && !group_pressed) {
        group_pressed = true;
        lv_label_set_text(group_ptt_label, "正在申请麦权");
        if (group_ptt_callback) {
            group_ptt_callback(true, group_context);
        }
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST || code == LV_EVENT_DELETE) {
        group_cancel_ptt();
        if (code == LV_EVENT_DELETE) {
            group_ptt_button = NULL;
            group_ptt_label = NULL;
            group_name_label = NULL;
            group_detail_label = NULL;
            group_status_label = NULL;
            group_action_label = NULL;
            group_volume_label = NULL;
            group_join_button = NULL;
            group_voice_button = NULL;
            group_connected = false;
            group_transport_ready = false;
        } else {
            group_render_controls();
        }
    }
}

static void group_dropdown_event_cb(lv_event_t *event)
{
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_DELETE) {
        /* Detach both static string users before freeing the PSRAM storage. */
        lv_obj_t *list = lv_dropdown_get_list(group_dropdown);
        if (list && lv_obj_get_child_count(list)) {
            lv_label_set_text_static(lv_obj_get_child(list, 0), "");
        }
        lv_dropdown_set_options_static(group_dropdown, "");
        free(group_choice_options);
        free(group_choices);
        group_choice_options = NULL;
        group_choices = NULL;
        group_dropdown = NULL;
        group_switch_button = NULL;
        group_switch_label = NULL;
        group_choices_valid = false;
        group_candidate_valid = false;
        group_switch_pending = false;
        return;
    }
    if (code == LV_EVENT_READY) {
        group_force_release();
    } else if (code == LV_EVENT_VALUE_CHANGED) {
        const uint32_t selected = lv_dropdown_get_selected(group_dropdown);
        group_candidate_valid = group_choices_valid && group_choices &&
                                selected < group_choices->count;
        lv_dropdown_set_text_static(group_dropdown, NULL);
    }
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL || code == LV_EVENT_VALUE_CHANGED) {
        group_render_controls();
    }
}

static void group_screen_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_SCREEN_UNLOAD_START) {
        group_force_release();
        group_close_command();
        group_transport_ready = false;
        if (group_dropdown) {
            lv_dropdown_close(group_dropdown);
        }
        group_render_controls();
    }
}

static void group_action_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_RELEASED) {
        return;
    }
    group_force_release();
    const bool stop = group_stop_action;
    group_stop_action = !stop;
    group_transport_ready = false;
    if (stop) {
        group_switch_pending = false;
    }
    lv_dropdown_close(group_dropdown);
    group_render_controls();
    lv_label_set_text(group_action_label, group_stop_action ? "停止连接" : "连接群语音");
    if (group_action_callback) {
        group_action_callback(stop, group_context);
    }
}

static void group_switch_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || !group_can_choose() ||
        !group_candidate_valid || !group_switch_callback) {
        return;
    }
    const uint32_t selected = lv_dropdown_get_selected(group_dropdown);
    if (selected >= group_choices->count ||
        strcmp(group_choices->items[selected].id, group_choices->current_id) == 0) {
        return;
    }
    group_force_release();
    const esp_err_t err = group_switch_callback(group_choices->items[selected].id,
                                               group_choices->generation, group_context);
    if (err == ESP_OK) {
        group_switch_pending = true;
        group_pending_generation = group_choices->generation;
        group_transport_ready = false;
        lv_label_set_text(group_detail_label, "正在请求切换群组");
    } else {
        lv_label_set_text(group_detail_label,
                          err == ESP_ERR_INVALID_STATE ? "状态已变化，请重新选择群组" :
                          err == ESP_ERR_NO_MEM ? "切换申请未提交：内存不足" :
                          "无法提交切换，请稍后重试");
    }
    group_render_controls();
}

static void group_volume_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) {
        return;
    }
    const uint8_t value = (uint8_t)lv_slider_get_value(lv_event_get_target_obj(event));
    char text[48];
    snprintf(text, sizeof(text), value ? "收听音量 %u%%" : "收听已静音 %u%%", (unsigned)value);
    lv_label_set_text(group_volume_label, text);
    if (group_volume_callback) {
        group_volume_callback(value, false, group_context);
    }
}

static lv_obj_t *group_label(lv_obj_t *parent, const char *text, int x, int y, int width, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_font(label, TRANSCRIPT_FONT, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

/* The service issues eight symbols from this exact 32-character alphabet.
 * Keep ambiguous I/O/0/1 off the keypad rather than accepting a bad code. */
static const char *const group_join_keymap[] = {
    "A", "B", "C", "D", "E", "F", "G", "H", "\n",
    "J", "K", "L", "M", "N", "P", "Q", "R", "\n",
    "S", "T", "U", "V", "W", "X", "Y", "Z", "\n",
    "2", "3", "4", "5", "6", "7", "8", "9", "\n",
    LV_SYMBOL_BACKSPACE, LV_SYMBOL_CLOSE, LV_SYMBOL_OK, ""
};
static const lv_buttonmatrix_ctrl_t group_join_keyctrl[] = {
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | LV_BUTTONMATRIX_CTRL_WIDTH_2,
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | LV_BUTTONMATRIX_CTRL_WIDTH_2,
    LV_KEYBOARD_CTRL_BUTTON_FLAGS | LV_BUTTONMATRIX_CTRL_WIDTH_2,
};

static bool group_command_ready(void)
{
    return group_choices_valid && group_choices && group_choices->known && group_stop_action &&
           !group_is_switching() && !group_command_busy();
}

static void group_modal_delete_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_DELETE) {
        return;
    }
    /* No code copy survives the overlay. This also covers screen teardown. */
    if (group_join_textarea) {
        lv_textarea_set_text(group_join_textarea, "");
    }
    group_join_textarea = NULL;
    group_command_overlay = NULL;
    group_command_title = NULL;
    group_command_detail = NULL;
    group_command_submit = NULL;
    group_command_submit_label = NULL;
    group_command_cancel = NULL;
    group_command_cancel_label = NULL;
    group_modal = GROUP_MODAL_NONE;
    group_voice_submitted = false;
}

static void group_close_command(void)
{
    if (group_command_overlay) {
        /* Clear visible input before deleting the keyboard/textarea objects. */
        if (group_join_textarea) {
            lv_textarea_set_text(group_join_textarea, "");
        }
        lv_obj_delete(group_command_overlay);
    }
    group_modal = GROUP_MODAL_NONE;
    group_render_controls();
}

static void group_render_command(void)
{
    if (!group_command_overlay || group_modal != GROUP_MODAL_VOICE) {
        return;
    }
    const bool waiting = group_voice_active || group_command_pending;
    lv_label_set_text(group_command_title,
                      !group_voice_submitted ? "准备语音切群" :
                      group_command_pending ? "正在准备" :
                      group_voice_active ? "语音切群" : "语音切群已结束");
    lv_label_set_text(group_command_detail,
                      group_voice_submitted && group_command_detail_text[0] ? group_command_detail_text :
                      "点击开始后，说出目标群的完整名称。\n只会切换到你已加入的群组。");
    if (!group_voice_submitted) {
        lv_obj_remove_flag(group_command_submit, LV_OBJ_FLAG_HIDDEN);
        if (group_command_ready() && group_transport_ready && group_current_index() >= 0 && group_voice_callback) {
            lv_obj_remove_state(group_command_submit, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(group_command_submit, LV_STATE_DISABLED);
        }
    } else {
        lv_obj_add_flag(group_command_submit, LV_OBJ_FLAG_HIDDEN);
    }
    const bool can_close = !waiting || (!group_command_pending && group_voice_active && group_voice_can_cancel);
    if (can_close) {
        lv_obj_remove_state(group_command_cancel, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(group_command_cancel, LV_STATE_DISABLED);
    }
    lv_label_set_text(group_command_cancel_label,
                      !group_voice_submitted ? "取消" :
                      waiting ? (can_close ? "取消语音" : "请等待恢复") : "关闭");
}

static void group_command_cancel_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    if (group_modal != GROUP_MODAL_VOICE || !group_voice_submitted ||
        (!group_voice_active && !group_command_pending)) {
        group_close_command();
        return;
    }
    if (!group_voice_active || !group_voice_can_cancel || group_command_pending || !group_voice_callback) {
        return;
    }
    const esp_err_t err = group_voice_callback(true, group_command_context);
    if (err == ESP_OK) {
        group_command_pending = true;
        group_command_pending_revision = group_command_revision;
        strlcpy(group_command_detail_text, "正在取消并恢复群音频", sizeof(group_command_detail_text));
    } else {
        strlcpy(group_command_detail_text, "无法取消，请等待当前操作恢复", sizeof(group_command_detail_text));
    }
    group_render_command();
    group_render_controls();
}

static void group_voice_submit_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || group_modal != GROUP_MODAL_VOICE ||
        group_voice_submitted || !group_command_ready() || !group_transport_ready ||
        group_current_index() < 0 || !group_voice_callback) {
        return;
    }
    group_force_release();
    const esp_err_t err = group_voice_callback(false, group_command_context);
    if (err == ESP_OK) {
        group_voice_submitted = true;
        group_command_pending = true;
        group_command_pending_revision = group_command_revision;
        strlcpy(group_command_detail_text, "正在准备语音，请稍候", sizeof(group_command_detail_text));
    } else {
        lv_label_set_text(group_command_detail,
                          err == ESP_ERR_INVALID_STATE ? "当前状态不可发起语音，请关闭后重试" : "无法开始语音，请稍后重试");
    }
    if (err == ESP_OK) {
        group_render_command();
    }
    group_render_controls();
}

static void group_join_submit(void)
{
    if (group_modal != GROUP_MODAL_JOIN || !group_join_textarea ||
        !group_command_ready() || !group_join_callback) {
        if (group_command_detail) {
            lv_label_set_text(group_command_detail, "状态已变化，请关闭后重新打开");
        }
        return;
    }
    const char *text = lv_textarea_get_text(group_join_textarea);
    if (strlen(text) != 8 || strspn(text, "ABCDEFGHJKLMNPQRSTUVWXYZ23456789") != 8) {
        lv_label_set_text(group_command_detail, "请输入完整的 8 位字母数字口令");
        return;
    }
    char code[9];
    memcpy(code, text, sizeof(code));
    const esp_err_t err = group_join_callback(code, group_command_context);
    /* The callback must copy before returning. Never log or retain the code. */
    volatile char *erase = code;
    for (size_t i = 0; i < sizeof(code); ++i) {
        erase[i] = 0;
    }
    if (err == ESP_OK) {
        group_command_pending = true;
        group_command_pending_revision = group_command_revision;
        strlcpy(group_command_detail_text, "正在加入群组", sizeof(group_command_detail_text));
        group_close_command();
        lv_label_set_text(group_detail_label, group_command_detail_text);
    } else {
        lv_label_set_text(group_command_detail,
                          err == ESP_ERR_INVALID_STATE ? "状态已变化，请关闭后重试" : "未能提交入群，请稍后重试");
    }
    group_render_controls();
}

static void group_join_submit_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        group_join_submit();
    }
}

static void group_join_keyboard_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CANCEL) {
        group_close_command();
    } else if (lv_event_get_code(event) == LV_EVENT_READY) {
        group_join_submit();
    }
}

static lv_obj_t *group_command_button(lv_obj_t *parent, const char *text,
                                       int x, int y, int width, lv_event_cb_t callback,
                                       lv_obj_t **label_out)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, 52);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x245675), 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = group_label(button, text, 0, 0, width - 20, 0xFFFFFF);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    if (label_out) {
        *label_out = label;
    }
    return button;
}

static lv_obj_t *group_create_command_overlay(group_modal_t modal)
{
    group_force_release();
    lv_dropdown_close(group_dropdown);
    group_modal = modal;
    group_voice_submitted = false;
    group_command_overlay = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(group_command_overlay, 0, 0);
    lv_obj_set_size(group_command_overlay, 800, 480);
    lv_obj_remove_flag(group_command_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(group_command_overlay, 0, 0);
    lv_obj_set_style_radius(group_command_overlay, 0, 0);
    lv_obj_set_style_border_width(group_command_overlay, 0, 0);
    lv_obj_set_style_bg_color(group_command_overlay, lv_color_hex(0x071018), 0);
    lv_obj_add_event_cb(group_command_overlay, group_modal_delete_event_cb, LV_EVENT_DELETE, NULL);
    group_render_controls();
    return group_command_overlay;
}

static void group_join_open_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || group_modal != GROUP_MODAL_NONE ||
        !group_command_ready() || !group_join_callback) {
        return;
    }
    lv_obj_t *overlay = group_create_command_overlay(GROUP_MODAL_JOIN);
    group_command_title = group_label(overlay, "口令加入群组", 28, 16, 550, 0xF4FAFC);
    group_command_detail = group_label(overlay, "输入群主分享的 8 位字母数字口令", 28, 54, 540, 0xAABCC7);
    group_command_cancel = group_command_button(overlay, "取消", 624, 12, 148,
                                                group_command_cancel_event_cb, NULL);
    group_join_textarea = lv_textarea_create(overlay);
    lv_textarea_set_one_line(group_join_textarea, true);
    lv_textarea_set_max_length(group_join_textarea, 8);
    lv_textarea_set_accepted_chars(group_join_textarea, "ABCDEFGHJKLMNPQRSTUVWXYZ23456789");
    lv_textarea_set_placeholder_text(group_join_textarea, "8 位入群口令");
    lv_obj_set_style_text_font(group_join_textarea, TRANSCRIPT_FONT, 0);
    lv_obj_set_pos(group_join_textarea, 28, 98);
    lv_obj_set_size(group_join_textarea, 550, 56);
    group_command_submit = group_command_button(overlay, "确认加入", 600, 100, 172,
                                                group_join_submit_event_cb, NULL);
    lv_obj_t *keyboard = lv_keyboard_create(overlay);
    lv_keyboard_set_map(keyboard, LV_KEYBOARD_MODE_USER_1, group_join_keymap, group_join_keyctrl);
    lv_keyboard_set_mode(keyboard, LV_KEYBOARD_MODE_USER_1);
    lv_keyboard_set_textarea(keyboard, group_join_textarea);
    lv_keyboard_set_popovers(keyboard, false);
    /* Reset the keyboard constructor's bottom anchor before placing it. */
    lv_obj_align(keyboard, LV_ALIGN_TOP_LEFT, 12, 178);
    lv_obj_set_size(keyboard, 776, 290);
    lv_obj_set_style_text_font(keyboard, &lv_font_montserrat_20, LV_PART_ITEMS);
    lv_obj_set_style_pad_row(keyboard, 4, 0);
    lv_obj_set_style_pad_column(keyboard, 4, 0);
    lv_obj_add_event_cb(keyboard, group_join_keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(keyboard, group_join_keyboard_event_cb, LV_EVENT_CANCEL, NULL);
}

static void group_voice_open_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED || group_modal != GROUP_MODAL_NONE ||
        !group_command_ready() || !group_transport_ready || group_current_index() < 0 || !group_voice_callback) {
        return;
    }
    lv_obj_t *overlay = group_create_command_overlay(GROUP_MODAL_VOICE);
    group_command_title = group_label(overlay, "准备语音切群", 48, 54, 704, 0xF4FAFC);
    group_command_detail = group_label(overlay, "", 48, 122, 704, 0xAABCC7);
    lv_obj_set_height(group_command_detail, 174);
    lv_label_set_long_mode(group_command_detail, LV_LABEL_LONG_MODE_WRAP);
    group_command_submit = group_command_button(overlay, "开始语音", 80, 336, 280,
                                                group_voice_submit_event_cb, &group_command_submit_label);
    group_command_cancel = group_command_button(overlay, "取消", 440, 336, 280,
                                                group_command_cancel_event_cb, &group_command_cancel_label);
    group_label(overlay, "语音指令期间暂时听不到群音频；完成后自动恢复", 48, 430, 704, 0x6F8998);
    group_render_command();
}

esp_err_t board_display_show_group(board_conversation_action_cb_t startstop,
                                   board_ptt_action_cb_t ptt,
                                   board_group_switch_action_cb_t switch_group,
                                   board_volume_action_cb_t volume,
                                   uint8_t initial_volume,
                                   void *context)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    group_cancel_ptt();
    group_close_command();
    lv_obj_t *screen = lv_screen_active();
    lv_obj_clean(screen);
    lv_obj_remove_event_cb(screen, group_screen_event_cb);
    lv_obj_add_event_cb(screen, group_screen_event_cb, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    group_action_callback = startstop;
    group_ptt_callback = ptt;
    group_switch_callback = switch_group;
    group_volume_callback = volume;
    group_context = context;
    group_connected = false;
    group_transport_ready = false;
    group_speaking = false;
    group_pressed = false;
    group_stop_action = false;
    group_joining = false;
    group_voice_active = false;
    group_voice_can_cancel = false;
    group_command_pending = false;
    group_voice_submitted = false;
    group_command_revision = 0;
    group_command_detail_text[0] = 0;
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x071018), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_NONE, 0);
    lv_obj_t *brand = group_label(screen, APP_DISPLAY_NAME, 28, 22, 420, 0xF4FAFC);
    lv_obj_set_style_text_font(brand, &shenglu_brand_24, 0);
    group_join_button = lv_button_create(screen);
    lv_obj_set_pos(group_join_button, 450, 16);
    lv_obj_set_size(group_join_button, 150, 44);
    lv_obj_set_style_bg_color(group_join_button, lv_color_hex(0x245675), 0);
    lv_obj_add_event_cb(group_join_button, group_join_open_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *join_label = group_label(group_join_button, "加入群组", 0, 0, 130, 0xFFFFFF);
    lv_obj_set_style_text_align(join_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(join_label);
    group_voice_button = lv_button_create(screen);
    lv_obj_set_pos(group_voice_button, 618, 16);
    lv_obj_set_size(group_voice_button, 154, 44);
    lv_obj_set_style_bg_color(group_voice_button, lv_color_hex(0x245675), 0);
    lv_obj_add_event_cb(group_voice_button, group_voice_open_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *voice_label = group_label(group_voice_button, "语音切群", 0, 0, 134, 0xFFFFFF);
    lv_obj_set_style_text_align(voice_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(voice_label);
    group_name_label = group_label(screen, "当前群：尚未同步", 28, 70, 744, 0xF4FAFC);
    lv_label_set_long_mode(group_name_label, LV_LABEL_LONG_MODE_DOTS);
    group_dropdown = lv_dropdown_create(screen);
    lv_obj_set_pos(group_dropdown, 28, 108);
    lv_obj_set_size(group_dropdown, 568, 52);
    lv_obj_set_style_text_font(group_dropdown, TRANSCRIPT_FONT, LV_PART_MAIN);
    lv_obj_set_style_text_font(group_dropdown, TRANSCRIPT_FONT, LV_PART_SELECTED);
    lv_obj_set_style_text_color(group_dropdown, lv_color_hex(0xF4FAFC), 0);
    lv_obj_set_style_bg_color(group_dropdown, lv_color_hex(0x152934), 0);
    lv_dropdown_set_symbol(group_dropdown, "v");
    lv_dropdown_set_options_static(group_dropdown, "群列表尚未同步");
    lv_dropdown_set_text_static(group_dropdown, "群列表尚未同步");
    lv_obj_add_state(group_dropdown, LV_STATE_DISABLED);
    lv_obj_t *list = lv_dropdown_get_list(group_dropdown);
    lv_obj_set_style_text_font(list, TRANSCRIPT_FONT, 0);
    lv_obj_set_style_text_line_space(list, 22, 0);
    lv_obj_set_style_max_height(list, 220, 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x152934), 0);
    lv_obj_set_style_text_color(list, lv_color_hex(0xF4FAFC), 0);
    lv_obj_set_style_bg_color(list, lv_color_hex(0x245675), LV_PART_SELECTED);
    lv_obj_add_event_cb(group_dropdown, group_dropdown_event_cb, LV_EVENT_ALL, NULL);
    group_switch_button = lv_button_create(screen);
    lv_obj_set_pos(group_switch_button, 612, 108);
    lv_obj_set_size(group_switch_button, 160, 52);
    lv_obj_set_style_bg_color(group_switch_button, lv_color_hex(0x245675), 0);
    lv_obj_add_state(group_switch_button, LV_STATE_DISABLED);
    lv_obj_add_event_cb(group_switch_button, group_switch_event_cb, LV_EVENT_CLICKED, NULL);
    group_switch_label = group_label(group_switch_button, "切换群组", 0, 0, 140, 0xFFFFFF);
    lv_obj_set_style_text_align(group_switch_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(group_switch_label);
    group_status_label = group_label(screen, "已停止", 28, 176, 744, 0x49E5C2);
    group_detail_label = group_label(screen, "等待 USB 配置后连接群语音", 28, 208, 744, 0xAABCC7);
    lv_obj_set_height(group_detail_label, 42);
    lv_label_set_long_mode(group_detail_label, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *action = lv_button_create(screen);
    lv_obj_set_pos(action, 28, 268);
    lv_obj_set_size(action, 218, 78);
    lv_obj_set_style_bg_color(action, lv_color_hex(0x245675), 0);
    lv_obj_add_event_cb(action, group_action_event_cb, LV_EVENT_RELEASED, NULL);
    group_action_label = group_label(action, "连接群语音", 0, 0, 190, 0xFFFFFF);
    lv_obj_set_style_text_align(group_action_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(group_action_label);
    group_ptt_button = lv_button_create(screen);
    lv_obj_set_pos(group_ptt_button, 270, 268);
    lv_obj_set_size(group_ptt_button, 502, 78);
    lv_obj_set_style_bg_color(group_ptt_button, lv_color_hex(0x148A75), 0);
    lv_obj_add_state(group_ptt_button, LV_STATE_DISABLED);
    lv_obj_add_event_cb(group_ptt_button, group_ptt_event_cb, LV_EVENT_ALL, NULL);
    group_ptt_label = group_label(group_ptt_button, "连接后可发言", 0, 0, 470, 0xFFFFFF);
    lv_obj_set_style_text_align(group_ptt_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(group_ptt_label);

    const uint8_t level = initial_volume <= 100 ? initial_volume : 0;
    char volume_text[48];
    snprintf(volume_text, sizeof(volume_text), level ? "收听音量 %u%%" : "收听已静音 %u%%", (unsigned)level);
    group_volume_label = group_label(screen, volume_text, 28, 378, 230, 0xAABCC7);
    lv_obj_t *slider = lv_slider_create(screen);
    lv_obj_set_pos(slider, 270, 386);
    lv_obj_set_size(slider, 480, 16);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, level, LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, group_volume_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    group_label(screen, "先选择群组，再点击切换；松手停止发言", 28, 435, 744, 0x6F8998);
    group_render_controls();
    bsp_display_unlock();
    if (volume) {
        volume(level, false, context);
    }
    return ESP_OK;
}

esp_err_t board_display_set_group_state(const char *group, const char *detail,
                                        bool connected, bool speaking)
{
    (void)group; /* The choices publication supplies the authoritative ID/name. */
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!group_ptt_button || !group_status_label || !group_name_label || !group_detail_label) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    group_transport_ready = connected;
    group_speaking = speaking;
    group_render_current();
    lv_label_set_text(group_detail_label,
                      group_command_busy() && group_command_detail_text[0] ? group_command_detail_text :
                      group_switch_pending ? "正在请求切换群组" : detail ? detail : "");
    group_render_controls();
    lv_label_set_text(group_action_label, group_stop_action ? "停止连接" : "连接群语音");
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t board_display_set_group_active(bool active)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!group_ptt_button || !group_action_label) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    group_stop_action = active;
    if (!active) {
        group_force_release();
        group_transport_ready = false;
        group_switch_pending = false;
        group_close_command();
        lv_dropdown_close(group_dropdown);
    }
    group_render_controls();
    lv_label_set_text(group_action_label, active ? "停止连接" : "连接群语音");
    bsp_display_unlock();
    return ESP_OK;
}

static bool group_valid_text(const char *text, size_t capacity, bool empty)
{
    if (!text) {
        return false;
    }
    const size_t size = strnlen(text, capacity);
    if (size >= capacity || (!empty && !size)) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        const unsigned char c = (unsigned char)text[i];
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
    }
    return true;
}

static esp_err_t group_choices_error(esp_err_t err, const char *message)
{
    group_choices_valid = false;
    group_candidate_valid = false;
    group_force_release();
    lv_dropdown_close(group_dropdown);
    lv_dropdown_set_text_static(group_dropdown, "群列表不可用");
    lv_label_set_text(group_detail_label, message);
    group_render_current();
    group_render_controls();
    return err;
}

esp_err_t board_display_set_group_choices(const board_group_item_t *items,
                                          size_t count,
                                          const char *current_group_id,
                                          uint32_t view_generation,
                                          bool known,
                                          bool can_select,
                                          bool switching)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!group_dropdown || !group_ptt_button) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    if (count > BOARD_GROUP_CHOICES_MAX) {
        err = group_choices_error(ESP_ERR_INVALID_SIZE, "已加入群超过128个，无法选择");
        goto done;
    }
    if ((count && !items) || !group_valid_text(current_group_id, 81, true)) {
        err = group_choices_error(ESP_ERR_INVALID_ARG, "群列表无效，请重新同步");
        goto done;
    }
    size_t options_size = 1;
    for (size_t i = 0; i < count; ++i) {
        if (!group_valid_text(items[i].id, sizeof(items[i].id), false) ||
            !group_valid_text(items[i].name, sizeof(items[i].name), false)) {
            err = group_choices_error(ESP_ERR_INVALID_ARG, "群列表无效，请重新同步");
            goto done;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(items[i].id, items[j].id) == 0) {
                err = group_choices_error(ESP_ERR_INVALID_ARG, "群列表重复，请重新同步");
                goto done;
            }
        }
        options_size += strlen(items[i].name) + (i != 0);
    }
    bool content_changed = !group_choices || group_choices->count != count ||
                           strcmp(group_choices->current_id, current_group_id) != 0;
    for (size_t i = 0; !content_changed && i < count; ++i) {
        content_changed = strcmp(group_choices->items[i].id, items[i].id) != 0 ||
                          strcmp(group_choices->items[i].name, items[i].name) != 0;
    }
    const bool generation_changed = !group_choices || group_choices->generation != view_generation;
    const bool was_switching = group_is_switching();
    const bool pending_completed = was_switching && !switching &&
                                   (!group_switch_pending || content_changed || generation_changed);
    const bool known_changed = !group_choices || group_choices->known != known;
    const bool reset_candidate = content_changed || generation_changed || pending_completed ||
                                 known_changed || !group_choices_valid;
    if (content_changed) {
        group_choices_t *replacement = heap_caps_calloc(1, sizeof(*replacement) + count * sizeof(*items),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        char *options = count ? heap_caps_malloc(options_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (!replacement || (count && !options)) {
            free(replacement);
            free(options);
            err = group_choices_error(ESP_ERR_NO_MEM, "群列表内存不足，请稍后重试");
            goto done;
        }
        replacement->count = count;
        strlcpy(replacement->current_id, current_group_id, sizeof(replacement->current_id));
        if (count) {
            memcpy(replacement->items, items, count * sizeof(*items));
            char *cursor = options;
            for (size_t i = 0; i < count; ++i) {
                if (i) {
                    *cursor++ = '\n';
                }
                const size_t length = strlen(items[i].name);
                memcpy(cursor, items[i].name, length);
                cursor += length;
            }
            *cursor = 0;
        }
        lv_dropdown_close(group_dropdown);
        lv_obj_t *list = lv_dropdown_get_list(group_dropdown);
        if (list && lv_obj_get_child_count(list)) {
            lv_label_set_text_static(lv_obj_get_child(list, 0), "");
        }
        lv_dropdown_set_options_static(group_dropdown, count ? options : "尚未加入群组");
        free(group_choice_options);
        free(group_choices);
        group_choice_options = options;
        group_choices = replacement;
    }
    if (group_switch_pending && (switching || content_changed || view_generation != group_pending_generation)) {
        group_switch_pending = false;
    }
    group_choices->generation = view_generation;
    group_choices->known = known;
    group_choices->can_select = can_select;
    group_choices->switching = switching;
    group_choices_valid = true;
    if (reset_candidate) {
        const int current = group_current_index();
        group_candidate_valid = current >= 0;
        lv_dropdown_set_selected(group_dropdown, current >= 0 ? (uint32_t)current : 0);
    }
    if (!known) {
        lv_dropdown_set_text_static(group_dropdown, "群列表尚未同步");
    } else if (!count) {
        lv_dropdown_set_text_static(group_dropdown, "尚未加入群组");
    } else if (!group_candidate_valid) {
        lv_dropdown_set_text_static(group_dropdown, "请选择已加入群组");
    } else {
        lv_dropdown_set_text_static(group_dropdown, NULL);
    }
    if (!group_can_choose()) {
        lv_dropdown_close(group_dropdown);
    }
    group_render_current();
    group_render_controls();
done:
    bsp_display_unlock();
    return err;
}


esp_err_t board_display_set_group_commands(board_group_join_action_cb_t join,
                                            board_group_voice_action_cb_t voice,
                                            void *context)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    group_join_callback = join;
    group_voice_callback = voice;
    group_command_context = context;
    group_render_controls();
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t board_display_set_group_command_state(uint32_t revision,
                                                bool joining,
                                                bool voice_active,
                                                bool can_cancel,
                                                const char *detail)
{
    if (!bsp_display_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }
    if (!group_ptt_button || !group_detail_label) {
        bsp_display_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    /* Modular comparison tolerates wraparound. A worker frame constructed
     * before the callback reservation must not acknowledge that request. */
    const bool fresh = (int32_t)(revision - group_command_revision) > 0;
    const bool same = revision == group_command_revision;
    if (!fresh && !same) {
        bsp_display_unlock();
        return ESP_OK;
    }
    if (group_command_pending &&
        (int32_t)(revision - group_command_pending_revision) <= 0) {
        bsp_display_unlock();
        return ESP_OK;
    }
    group_command_revision = revision;
    group_command_pending = false;
    group_joining = joining;
    group_voice_active = voice_active;
    group_voice_can_cancel = voice_active && can_cancel;
    strlcpy(group_command_detail_text, detail ? detail : "", sizeof(group_command_detail_text));
    if (joining || voice_active) {
        /* Worker publications are one-way rendering. The accepted command
         * already cancelled PTT under the controller lock. Calling it again
         * here would advance the connection generation during refresh_state
         * and invalidate the new group's in-flight connection ticket. */
        group_pressed = false;
        group_speaking = false;
        lv_dropdown_close(group_dropdown);
    }
    if (detail && detail[0]) {
        lv_label_set_text(group_detail_label, group_command_detail_text);
    }
    group_render_command();
    group_render_controls();
    bsp_display_unlock();
    return ESP_OK;
}

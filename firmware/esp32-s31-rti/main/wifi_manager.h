#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types_generic.h"

esp_err_t wifi_manager_init(void);
bool wifi_manager_has_saved_credentials(void);
esp_err_t wifi_manager_scan(wifi_ap_record_t *records, uint16_t *count);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_connect_saved(void);

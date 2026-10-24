#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* Hex USB transport must fit the existing 4096-byte command buffer. */
#define GROUP_CONFIG_JSON_MAX 2000

typedef struct {
    char server_url[192];
    char ca_pem[2048];
    char device_id[81];
    char token[129];
    char group_code[17];
} group_config_t;

esp_err_t group_config_load(group_config_t *out);
bool group_config_ready(void);
esp_err_t group_config_store_json(const char *json, size_t size);

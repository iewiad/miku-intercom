#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define RTI_APP_ID_MAX 128
#define RTI_API_TOKEN_MAX 1536

typedef struct {
    char app_id[RTI_APP_ID_MAX + 1];
    char api_token[RTI_API_TOKEN_MAX + 1];
} rti_credentials_t;

esp_err_t rti_credentials_load(rti_credentials_t *credentials);
esp_err_t rti_credentials_save(const char *app_id, const char *api_token);
esp_err_t rti_credentials_clear(void);
bool rti_credentials_are_configured(void);

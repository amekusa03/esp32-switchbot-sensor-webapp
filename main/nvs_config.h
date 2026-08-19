#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char wifi_ssid[64];
    char wifi_pass[64];
    char sb_token[128];
    char sb_secret[128];
} app_config_t;

esp_err_t nvs_config_init(void);
esp_err_t nvs_config_load(app_config_t *config);
esp_err_t nvs_config_save(const app_config_t *config);
esp_err_t nvs_config_save_wifi(const char *ssid, const char *pass);
esp_err_t nvs_config_save_switchbot(const char *token, const char *secret);

typedef struct {
    int   raw1;   // Point 1 ADC raw value
    float lux1;   // Point 1 illuminance (Lux)
    int   raw2;   // Point 2 ADC raw value
    float lux2;   // Point 2 illuminance (Lux)
} cds_calib_t;

/**
 * @brief CdS校正値をNVSからロード（未保存時はデフォルト値を設定）
 */
esp_err_t nvs_config_load_cds_calib(cds_calib_t *calib);

/**
 * @brief CdS校正値をNVSに保存
 */
esp_err_t nvs_config_save_cds_calib(const cds_calib_t *calib);

#endif // NVS_CONFIG_H

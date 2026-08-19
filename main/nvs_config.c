#include "nvs_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "NVS_CONFIG";
#define NVS_NAMESPACE "sb_storage"

esp_err_t nvs_config_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t nvs_config_load(app_config_t *config) {
    if (!config) return ESP_ERR_INVALID_ARG;
    memset(config, 0, sizeof(app_config_t));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found or empty");
        return err;
    }

    size_t len = sizeof(config->wifi_ssid);
    nvs_get_str(handle, "wifi_ssid", config->wifi_ssid, &len);

    len = sizeof(config->wifi_pass);
    nvs_get_str(handle, "wifi_pass", config->wifi_pass, &len);

    len = sizeof(config->sb_token);
    nvs_get_str(handle, "sb_token", config->sb_token, &len);

    len = sizeof(config->sb_secret);
    nvs_get_str(handle, "sb_secret", config->sb_secret, &len);

    nvs_close(handle);
    ESP_LOGI(TAG, "Config loaded from NVS");
    return ESP_OK;
}

esp_err_t nvs_config_save(const app_config_t *config) {
    if (!config) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_str(handle, "wifi_ssid", config->wifi_ssid);
    nvs_set_str(handle, "wifi_pass", config->wifi_pass);
    nvs_set_str(handle, "sb_token", config->sb_token);
    nvs_set_str(handle, "sb_secret", config->sb_secret);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config successfully saved to NVS");
    }
    return err;
}

esp_err_t nvs_config_save_wifi(const char *ssid, const char *pass) {
    app_config_t config;
    nvs_config_load(&config);
    if (ssid) strncpy(config.wifi_ssid, ssid, sizeof(config.wifi_ssid) - 1);
    if (pass) strncpy(config.wifi_pass, pass, sizeof(config.wifi_pass) - 1);
    return nvs_config_save(&config);
}

esp_err_t nvs_config_save_switchbot(const char *token, const char *secret) {
    app_config_t config;
    nvs_config_load(&config);
    if (token) strncpy(config.sb_token, token, sizeof(config.sb_token) - 1);
    if (secret) strncpy(config.sb_secret, secret, sizeof(config.sb_secret) - 1);
    return nvs_config_save(&config);
}

esp_err_t nvs_config_load_cds_calib(cds_calib_t *calib) {
    if (!calib) return ESP_ERR_INVALID_ARG;
    // デフォルト値 (実測: RAW 1300 = 13 Lux, RAW 70 = 0.1 Lux)
    calib->raw1 = 1300;
    calib->lux1 = 13.0f;
    calib->raw2 = 70;
    calib->lux2 = 0.1f;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CdS calib: NVS not found, using defaults");
        return ESP_OK; // デフォルト値で続行
    }

    int32_t v32 = 0;
    size_t sz = sizeof(float);
    float fv = 0.0f;

    if (nvs_get_i32(handle, "cds_raw1", &v32) == ESP_OK) calib->raw1 = (int)v32;
    if (nvs_get_blob(handle, "cds_lux1", &fv, &sz) == ESP_OK) calib->lux1 = fv;
    sz = sizeof(float);
    if (nvs_get_i32(handle, "cds_raw2", &v32) == ESP_OK) calib->raw2 = (int)v32;
    if (nvs_get_blob(handle, "cds_lux2", &fv, &sz) == ESP_OK) calib->lux2 = fv;

    nvs_close(handle);
    ESP_LOGI(TAG, "CdS calib loaded: P1(RAW=%d, %.2flx) P2(RAW=%d, %.3flx)",
             calib->raw1, calib->lux1, calib->raw2, calib->lux2);
    return ESP_OK;
}

esp_err_t nvs_config_save_cds_calib(const cds_calib_t *calib) {
    if (!calib) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CdS calib: failed to open NVS for writing: %s", esp_err_to_name(err));
        return err;
    }

    nvs_set_i32(handle, "cds_raw1", (int32_t)calib->raw1);
    nvs_set_blob(handle, "cds_lux1", &calib->lux1, sizeof(float));
    nvs_set_i32(handle, "cds_raw2", (int32_t)calib->raw2);
    nvs_set_blob(handle, "cds_lux2", &calib->lux2, sizeof(float));

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "CdS calib saved: P1(RAW=%d, %.2flx) P2(RAW=%d, %.3flx)",
                 calib->raw1, calib->lux1, calib->raw2, calib->lux2);
    }
    return err;
}

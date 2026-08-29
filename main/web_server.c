#include "web_server.h"
#include "web_page.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "switchbot_api.h"
#include "cds_sensor.h"
#include "temp_sensor.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "WEB_SERVER";
static httpd_handle_t server_handle = NULL;

extern bool get_latest_cds_sensor_data(cds_sensor_data_t *out_data);
extern bool get_latest_temp_sensor_data(temp_sensor_data_t *out_data);

static char *read_http_body(httpd_req_t *req) {
    int total_len = req->content_len;
    if (total_len <= 0 || total_len > 4096) return NULL;
    char *buf = malloc(total_len + 1);
    if (!buf) return NULL;
    int received = 0;
    while (received < total_len) {
        int ret = httpd_req_recv(req, buf + received, total_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            return NULL;
        }
        received += ret;
    }
    buf[total_len] = '\0';
    return buf;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_PAGE_CONTENT, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t api_status_get_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ip", wifi_manager_get_ip());
    cJSON_AddNumberToObject(root, "rssi", wifi_manager_get_rssi());
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_manager_is_connected());
    cJSON_AddBoolToObject(root, "ap_mode", wifi_manager_is_ap_mode());
    cJSON_AddBoolToObject(root, "time_synced", wifi_manager_is_time_synced());
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());

    const char *resp = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    free((void*)resp);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_sensor_get_handler(httpd_req_t *req) {
    cds_sensor_data_t data = {0};
    bool valid = get_latest_cds_sensor_data(&data);

    temp_sensor_data_t tdata = {0};
    bool tvalid = get_latest_temp_sensor_data(&tdata);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "valid", valid);
    cJSON_AddNumberToObject(root, "raw_adc", data.raw_adc);
    cJSON_AddNumberToObject(root, "voltage_mv", data.voltage_mv);
    cJSON_AddNumberToObject(root, "resistance_ohm", data.resistance_ohm);
    cJSON_AddNumberToObject(root, "lux", data.lux);
    cJSON_AddNumberToObject(root, "matter_value", data.matter_value);

    // B3950 Temp sensor fields
    cJSON_AddBoolToObject(root, "temp_valid", tvalid);
    cJSON_AddNumberToObject(root, "temp_celsius", tdata.temp_celsius);
    cJSON_AddNumberToObject(root, "temp_raw_celsius", tdata.temp_raw_celsius);
    cJSON_AddNumberToObject(root, "temp_offset", tdata.offset);
    cJSON_AddNumberToObject(root, "temp_raw_adc", tdata.raw_adc);
    cJSON_AddNumberToObject(root, "temp_voltage_mv", tdata.voltage_mv);
    cJSON_AddNumberToObject(root, "temp_resistance_ohm", tdata.resistance_ohm);

    const char *resp = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    free((void*)resp);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_config_post_handler(httpd_req_t *req) {
    char *body = read_http_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(body);
    free(body);

    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_FAIL;
    }

    app_config_t config;
    nvs_config_load(&config);

    cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    cJSON *pass = cJSON_GetObjectItem(json, "pass");
    cJSON *token = cJSON_GetObjectItem(json, "token");
    cJSON *secret = cJSON_GetObjectItem(json, "secret");

    if (cJSON_IsString(ssid) && strlen(ssid->valuestring) > 0) strncpy(config.wifi_ssid, ssid->valuestring, sizeof(config.wifi_ssid)-1);
    if (cJSON_IsString(pass) && strlen(pass->valuestring) > 0) strncpy(config.wifi_pass, pass->valuestring, sizeof(config.wifi_pass)-1);
    if (cJSON_IsString(token) && strlen(token->valuestring) > 0) strncpy(config.sb_token, token->valuestring, sizeof(config.sb_token)-1);
    if (cJSON_IsString(secret) && strlen(secret->valuestring) > 0) strncpy(config.sb_secret, secret->valuestring, sizeof(config.sb_secret)-1);

    cJSON_Delete(json);

    esp_err_t err = nvs_config_save(&config);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Configuration saved successfully\"}");
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to save to NVS\"}");
    }
    return ESP_OK;
}

static esp_err_t api_devices_get_handler(httpd_req_t *req) {
    app_config_t config;
    nvs_config_load(&config);

    char *resp_json = NULL;
    esp_err_t err = switchbot_get_devices(config.sb_token, config.sb_secret, &resp_json);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK && resp_json) {
        httpd_resp_sendstr(req, resp_json);
        free(resp_json);
    } else {
        httpd_resp_sendstr(req, "{\"statusCode\":400,\"message\":\"Failed to communicate with SwitchBot Cloud API\"}");
    }
    return ESP_OK;
}

static esp_err_t api_device_command_post_handler(httpd_req_t *req) {
    char *body = read_http_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_FAIL;
    }

    cJSON *dev_id = cJSON_GetObjectItem(json, "deviceId");
    cJSON *cmd = cJSON_GetObjectItem(json, "command");
    cJSON *param = cJSON_GetObjectItem(json, "parameter");
    cJSON *type = cJSON_GetObjectItem(json, "commandType");

    if (!cJSON_IsString(dev_id)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing deviceId");
        return ESP_FAIL;
    }

    app_config_t config;
    nvs_config_load(&config);

    char *resp_json = NULL;
    esp_err_t err = switchbot_send_command(config.sb_token, config.sb_secret,
                                           dev_id->valuestring,
                                           cJSON_IsString(cmd) ? cmd->valuestring : "turnOn",
                                           cJSON_IsString(param) ? param->valuestring : "default",
                                           cJSON_IsString(type) ? type->valuestring : "command",
                                           &resp_json);

    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK && resp_json) {
        httpd_resp_sendstr(req, resp_json);
        free(resp_json);
    } else {
        httpd_resp_sendstr(req, "{\"statusCode\":400,\"message\":\"Command execution failed\"}");
    }
    return ESP_OK;
}

static esp_err_t api_scenes_get_handler(httpd_req_t *req) {
    app_config_t config;
    nvs_config_load(&config);

    char *resp_json = NULL;
    esp_err_t err = switchbot_get_scenes(config.sb_token, config.sb_secret, &resp_json);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK && resp_json) {
        httpd_resp_sendstr(req, resp_json);
        free(resp_json);
    } else {
        httpd_resp_sendstr(req, "{\"statusCode\":400,\"message\":\"Failed to fetch scenes\"}");
    }
    return ESP_OK;
}

static esp_err_t api_sensor_calib_get_handler(httpd_req_t *req) {
    int raw1, raw2;
    float lux1, lux2, A, Rref, n;
    cds_sensor_get_calib_params(&raw1, &lux1, &raw2, &lux2, &A, &Rref, &n);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "raw1",  raw1);
    cJSON_AddNumberToObject(root, "lux1",  lux1);
    cJSON_AddNumberToObject(root, "raw2",  raw2);
    cJSON_AddNumberToObject(root, "lux2",  lux2);
    cJSON_AddNumberToObject(root, "A",     A);
    cJSON_AddNumberToObject(root, "R_ref", Rref);
    cJSON_AddNumberToObject(root, "n",     n);

    const char *resp = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);
    free((void*)resp);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_sensor_calib_post_handler(httpd_req_t *req) {
    char *body = read_http_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_FAIL;
    }

    cJSON *action = cJSON_GetObjectItem(json, "action");
    if (!cJSON_IsString(action)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing action");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");

    if (strcmp(action->valuestring, "set_point") == 0) {
        cJSON *pt_j  = cJSON_GetObjectItem(json, "point");
        cJSON *lux_j = cJSON_GetObjectItem(json, "lux");
        if (!cJSON_IsNumber(pt_j) || !cJSON_IsNumber(lux_j)) {
            cJSON_Delete(json);
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing point or lux\"}");
            return ESP_OK;
        }
        int   pt  = (int)pt_j->valuedouble;
        float lux = (float)lux_j->valuedouble;

        // 現在のADC RAW値を最新センサーデータから取得
        cds_sensor_data_t data = {0};
        get_latest_cds_sensor_data(&data);
        int raw = data.raw_adc;
        cJSON_Delete(json);

        esp_err_t err = cds_sensor_set_calib_point(pt, raw, lux);
        if (err == ESP_OK) {
            float A, Rref, n;
            cds_sensor_get_calib_params(NULL, NULL, NULL, NULL, &A, &Rref, &n);
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "status", "ok");
            cJSON_AddNumberToObject(resp, "raw",   raw);
            cJSON_AddNumberToObject(resp, "A",     A);
            cJSON_AddNumberToObject(resp, "R_ref", Rref);
            cJSON_AddNumberToObject(resp, "n",     n);
            const char *s = cJSON_PrintUnformatted(resp);
            httpd_resp_sendstr(req, s);
            free((void*)s);
            cJSON_Delete(resp);
        } else {
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"set_point failed\"}");
        }
    } else if (strcmp(action->valuestring, "set_temp_offset") == 0) {
        cJSON *offset_j = cJSON_GetObjectItem(json, "offset");
        if (!cJSON_IsNumber(offset_j)) {
            cJSON_Delete(json);
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Missing offset\"}");
            return ESP_OK;
        }
        float offset = (float)offset_j->valuedouble;
        cJSON_Delete(json);
        temp_sensor_set_offset(offset);
        esp_err_t err = temp_sensor_save_offset_to_nvs();
        if (err == ESP_OK) {
            httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Temp offset saved to NVS\"}");
        } else {
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Failed to save temp offset\"}");
        }
    } else if (strcmp(action->valuestring, "save") == 0) {
        cJSON_Delete(json);
        esp_err_t err = cds_sensor_save_calib_to_nvs();
        temp_sensor_save_offset_to_nvs();
        if (err == ESP_OK) {
            httpd_resp_sendstr(req, "{\"status\":\"ok\",\"message\":\"Calibration saved to NVS\"}");
        } else {
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"NVS save failed\"}");
        }
        cJSON_Delete(json);
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"Unknown action\"}");
    }

    return ESP_OK;
}

static esp_err_t api_scene_execute_post_handler(httpd_req_t *req) {
    char *body = read_http_body(req);
    if (!body) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request body");
        return ESP_FAIL;
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON parse error");
        return ESP_FAIL;
    }

    cJSON *scene_id = cJSON_GetObjectItem(json, "sceneId");
    if (!cJSON_IsString(scene_id)) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing sceneId");
        return ESP_FAIL;
    }

    app_config_t config;
    nvs_config_load(&config);

    char *resp_json = NULL;
    esp_err_t err = switchbot_execute_scene(config.sb_token, config.sb_secret, scene_id->valuestring, &resp_json);
    cJSON_Delete(json);

    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK && resp_json) {
        httpd_resp_sendstr(req, resp_json);
        free(resp_json);
    } else {
        httpd_resp_sendstr(req, "{\"statusCode\":400,\"message\":\"Scene execution failed\"}");
    }
    return ESP_OK;
}

esp_err_t web_server_start(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 14;
    config.stack_size = 10240;
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting Web Server on port: %d", config.server_port);
    if (httpd_start(&server_handle, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Error starting httpd server!");
        return ESP_FAIL;
    }

    httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
    httpd_uri_t uri_status = { .uri = "/api/status", .method = HTTP_GET, .handler = api_status_get_handler };
    httpd_uri_t uri_sensor = { .uri = "/api/sensor", .method = HTTP_GET, .handler = api_sensor_get_handler };
    httpd_uri_t uri_config_post = { .uri = "/api/config", .method = HTTP_POST, .handler = api_config_post_handler };
    httpd_uri_t uri_devices_get = { .uri = "/api/devices", .method = HTTP_GET, .handler = api_devices_get_handler };
    httpd_uri_t uri_cmd_post = { .uri = "/api/device/command", .method = HTTP_POST, .handler = api_device_command_post_handler };
    httpd_uri_t uri_scenes_get     = { .uri = "/api/scenes",            .method = HTTP_GET,  .handler = api_scenes_get_handler };
    httpd_uri_t uri_scene_exec     = { .uri = "/api/scene/execute",     .method = HTTP_POST, .handler = api_scene_execute_post_handler };
    httpd_uri_t uri_calib_get      = { .uri = "/api/sensor/calibrate",  .method = HTTP_GET,  .handler = api_sensor_calib_get_handler };
    httpd_uri_t uri_calib_post     = { .uri = "/api/sensor/calibrate",  .method = HTTP_POST, .handler = api_sensor_calib_post_handler };

    httpd_register_uri_handler(server_handle, &uri_root);
    httpd_register_uri_handler(server_handle, &uri_status);
    httpd_register_uri_handler(server_handle, &uri_sensor);
    httpd_register_uri_handler(server_handle, &uri_config_post);
    httpd_register_uri_handler(server_handle, &uri_devices_get);
    httpd_register_uri_handler(server_handle, &uri_cmd_post);
    httpd_register_uri_handler(server_handle, &uri_scenes_get);
    httpd_register_uri_handler(server_handle, &uri_scene_exec);
    httpd_register_uri_handler(server_handle, &uri_calib_get);
    httpd_register_uri_handler(server_handle, &uri_calib_post);

    ESP_LOGI(TAG, "Web Server started successfully!");
    return ESP_OK;
}

void web_server_stop(void) {
    if (server_handle) {
        httpd_stop(server_handle);
        server_handle = NULL;
    }
}


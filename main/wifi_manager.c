#include "wifi_manager.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "WIFI_MGR";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAXIMUM_RETRY      5

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static bool s_is_connected = false;
static bool s_is_ap_mode = false;
static bool s_time_synced = false;
static char s_ip_str[16] = "0.0.0.0";

static bool parse_http_date(const char *date_str, time_t *out_time) {
    int day, year, hour, min, sec;
    char mon_str[4];
    if (sscanf(date_str, "%*s %d %3s %d %d:%d:%d", &day, mon_str, &year, &hour, &min, &sec) == 6) {
        const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        int month = -1;
        for (int i = 0; i < 12; i++) {
            if (strcasecmp(mon_str, months[i]) == 0) {
                month = i;
                break;
            }
        }
        if (month >= 0 && year >= 2020) {
            struct tm tm = {0};
            tm.tm_sec = sec;
            tm.tm_min = min;
            tm.tm_hour = hour;
            tm.tm_mday = day;
            tm.tm_mon = month;
            tm.tm_year = year - 1900;
            *out_time = mktime(&tm);
            return true;
        }
    }
    return false;
}

static esp_err_t _http_header_event_handler(esp_http_client_event_t *evt) {
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (strcasecmp(evt->header_key, "Date") == 0) {
            time_t http_time = 0;
            if (parse_http_date(evt->header_value, &http_time) && http_time > 1600000000) {
                struct timeval tv = { .tv_sec = http_time, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                s_time_synced = true;
                ESP_LOGI(TAG, "Time successfully synced via HTTP Date Header: %s", evt->header_value);
            }
        }
    }
    return ESP_OK;
}

static void try_http_time_sync(void) {
    ESP_LOGI(TAG, "Attempting HTTP Date header time sync fallback...");
    esp_http_client_config_t config = {
        .url = "http://www.google.com",
        .method = HTTP_METHOD_HEAD,
        .event_handler = _http_header_event_handler,
        .timeout_ms = 4000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
        esp_http_client_perform(client);
        esp_http_client_cleanup(client);
    }
}

static void sntp_sync_task(void *pvParameters) {
    ESP_LOGI(TAG, "Starting Classic SNTP Engine & Time Sync Task...");

    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.jst.mbed.ad.jp");
    esp_sntp_setservername(1, "time.google.com");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_init();

    time_t now = 0;
    struct tm timeinfo = { 0 };
    int attempts = 0;

    while (1) {
        time(&now);
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year > (2020 - 1900)) {
            s_time_synced = true;
            ESP_LOGI(TAG, "============================================================");
            ESP_LOGI(TAG, "NTP Time Synchronized Successfully!");
            ESP_LOGI(TAG, "Current Time: %s", asctime(&timeinfo));
            ESP_LOGI(TAG, "============================================================");
            break;
        }

        attempts++;
        ESP_LOGI(TAG, "Waiting for Time Sync... (Attempt %d, Year: %d)", attempts, timeinfo.tm_year + 1900);

        if (attempts >= 2 && !s_time_synced) {
            try_http_time_sync();
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    vTaskDelete(NULL);
}

static void event_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_is_connected = false;
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying WiFi connection (%d/%d)...", s_retry_num, MAXIMUM_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGE(TAG, "WiFi connection failed after max retries.");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "WiFi Connected. IP: %s", s_ip_str);
        s_retry_num = 0;
        s_is_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        xTaskCreate(sntp_sync_task, "sntp_task", 4096, NULL, 5, NULL);
    }
}

static void start_softap(void) {
    s_is_ap_mode = true;
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32-SwitchBot-Setup",
            .ssid_len = strlen("ESP32-SwitchBot-Setup"),
            .channel = 1,
            .password = "",
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif) {
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_ip_info_t ip_info;
        memset(&ip_info, 0, sizeof(esp_netif_ip_info_t));
        ip_info.ip.addr = esp_ip4addr_aton("192.168.4.1");
        ip_info.gw.addr = esp_ip4addr_aton("192.168.4.1");
        ip_info.netmask.addr = esp_ip4addr_aton("255.255.255.0");
        esp_netif_set_ip_info(ap_netif, &ip_info);
        esp_netif_dhcps_start(ap_netif);
    }
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    strcpy(s_ip_str, "192.168.4.1");
    ESP_LOGI(TAG, "============================================================");
    ESP_LOGI(TAG, "SoftAP Mode Started!");
    ESP_LOGI(TAG, "1. Connect your PC/Phone Wi-Fi to SSID: ESP32-SwitchBot-Setup");
    ESP_LOGI(TAG, "2. Open Browser at: http://192.168.4.1/");
    ESP_LOGI(TAG, "============================================================");
}

esp_err_t wifi_manager_init(const char *ssid, const char *password) {
    s_wifi_event_group = xEventGroupCreate();

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default failed: %s", esp_err_to_name(err));
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
    }

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT,
                                        ESP_EVENT_ANY_ID,
                                        &event_handler,
                                        NULL,
                                        &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT,
                                        IP_EVENT_STA_GOT_IP,
                                        &event_handler,
                                        NULL,
                                        &instance_got_ip);

    if (ssid != NULL && strlen(ssid) > 0) {
        if (!esp_netif_get_handle_from_ifkey("WIFI_STA_DEF")) {
            esp_netif_create_default_wifi_sta();
        }
        wifi_config_t wifi_config = {0};
        strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
        if (password) {
            strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
        }

        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
        esp_wifi_start();

        ESP_LOGI(TAG, "Connecting to WiFi SSID: %s...", ssid);

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                pdFALSE,
                pdFALSE,
                pdMS_TO_TICKS(15000));

        if (bits & WIFI_CONNECTED_BIT) {
            return ESP_OK;
        } else {
            ESP_LOGW(TAG, "Failed to connect to target SSID. Fallback to SoftAP mode.");
            esp_wifi_stop();
            start_softap();
            return ESP_FAIL;
        }
    } else {
        ESP_LOGI(TAG, "No Wi-Fi credentials found. Launching SoftAP mode.");
        start_softap();
        return ESP_OK;
    }
}

bool wifi_manager_is_connected(void) {
    return s_is_connected;
}

bool wifi_manager_is_ap_mode(void) {
    return s_is_ap_mode;
}

const char *wifi_manager_get_ip(void) {
    return s_ip_str;
}

int wifi_manager_get_rssi(void) {
    if (!s_is_connected) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

bool wifi_manager_is_time_synced(void) {
    return s_time_synced;
}

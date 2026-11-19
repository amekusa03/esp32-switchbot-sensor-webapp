#include <stdio.h>
#include <string.h>
#include <math.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_mac.h>
#include <esp_now.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <app/server/Server.h>
#include <setup_payload/OnboardingCodesUtil.h>

extern "C" {
#include "nvs_config.h"
#include "wifi_manager.h"
#include "web_server.h"
#include "cds_sensor.h"
#include "temp_sensor.h"
#include "esp_now_packet.h"
}

static const char *TAG = "MAIN_APP";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

// Matter エンドポイントID
// Endpoint 1: ベランダ照度 (Light Sensor - ESP-NOW)
// Endpoint 2: 室内温度 (Temperature Sensor - 親機直結 GPIO3)
// Endpoint 3: ベランダ外気温 (Temperature Sensor - ESP-NOW)
static uint16_t s_endpoint_id = 1;
static uint16_t s_temp_endpoint_id = 2;
static uint16_t s_outdoor_temp_endpoint_id = 3;
static volatile bool s_matter_ready = false;

#define GPIO_FACTORY_RESET          GPIO_NUM_9
#define FACTORY_RESET_HOLD_MS       3000
#define MATTER_REPORT_DEADBAND      50     // Matter Value 変化閾値
#define MATTER_REPORT_HEARTBEAT_MS  60000  // 最大更新間隔 (60秒)

// センサーデータ保持（スレッドセーフアクセス用）
static cds_sensor_data_t s_latest_sensor_data = {0};
static temp_sensor_data_t s_latest_temp_data = {0};
static outdoor_sensor_data_t s_latest_outdoor_data = {0};
static portMUX_TYPE s_sensor_spinlock = portMUX_INITIALIZER_UNLOCKED;
static bool s_sensor_valid = false;
static bool s_temp_valid = false;

extern "C" bool get_latest_cds_sensor_data(cds_sensor_data_t *out_data) {
    if (!out_data) return false;
    taskENTER_CRITICAL(&s_sensor_spinlock);
    *out_data = s_latest_sensor_data;
    bool valid = s_sensor_valid;
    taskEXIT_CRITICAL(&s_sensor_spinlock);
    return valid;
}

extern "C" bool get_latest_temp_sensor_data(temp_sensor_data_t *out_data) {
    if (!out_data) return false;
    taskENTER_CRITICAL(&s_sensor_spinlock);
    *out_data = s_latest_temp_data;
    bool valid = s_temp_valid;
    taskEXIT_CRITICAL(&s_sensor_spinlock);
    return valid;
}

extern "C" bool get_latest_outdoor_sensor_data(outdoor_sensor_data_t *out_data) {
    if (!out_data) return false;
    taskENTER_CRITICAL(&s_sensor_spinlock);
    *out_data = s_latest_outdoor_data;
    bool valid = s_latest_outdoor_data.valid;
    taskEXIT_CRITICAL(&s_sensor_spinlock);
    return valid;
}

// 子機設定用情報のログ出力 (STA MACアドレス & チャンネル)
static void print_esp_now_info(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    uint8_t primary_ch = 0;
    wifi_second_chan_t second_ch;
    esp_wifi_get_channel(&primary_ch, &second_ch);

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " [ESP-NOW Config Info for Child Device]           ");
    ESP_LOGI(TAG, "  Parent STA MAC Address : %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "  Current Wi-Fi Channel   : %d", primary_ch);
    ESP_LOGI(TAG, "==================================================");
}

// ESP-NOW パケット受信コールバック (ベランダ子機からのデータ受信)
static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    if (len != sizeof(esp_now_sensor_packet_t)) {
        ESP_LOGW(TAG, "ESP-NOW packet size mismatch: got %d, expected %d", len, (int)sizeof(esp_now_sensor_packet_t));
        return;
    }

    const esp_now_sensor_packet_t *pkt = (const esp_now_sensor_packet_t *)data;

    // Matter measured_value 換算: 10000 * log10(lux) + 1
    uint16_t matter_lux = 1;
    if (pkt->lux > 0.0001f) {
        float val = 10000.0f * log10f(pkt->lux) + 1.0f;
        if (val < 1.0f) val = 1.0f;
        if (val > 65534.0f) val = 65534.0f;
        matter_lux = (uint16_t)roundf(val);
    }

    taskENTER_CRITICAL(&s_sensor_spinlock);
    s_latest_outdoor_data.lux = pkt->lux;
    s_latest_outdoor_data.matter_value = matter_lux;
    s_latest_outdoor_data.temp_celsius = pkt->temp_celsius;
    s_latest_outdoor_data.battery_voltage = pkt->battery_voltage;
    s_latest_outdoor_data.last_received_ms = esp_timer_get_time() / 1000;
    if (recv_info && recv_info->src_addr) {
        memcpy(s_latest_outdoor_data.sender_mac, recv_info->src_addr, 6);
    }
    s_latest_outdoor_data.valid = true;

    // 既存 Web UI / API との互換性のため照度データを同期
    s_latest_sensor_data.lux = pkt->lux;
    s_latest_sensor_data.matter_value = matter_lux;
    s_sensor_valid = true;
    taskEXIT_CRITICAL(&s_sensor_spinlock);

    ESP_LOGI(TAG, "[ESP-NOW Recv] Outdoor Lux: %6.1f (MatterVal: %u) | Temp: %4.1f C | Batt: %.2f V | From: %02X:%02X:%02X:%02X:%02X:%02X",
             pkt->lux, matter_lux, pkt->temp_celsius, pkt->battery_voltage,
             s_latest_outdoor_data.sender_mac[0], s_latest_outdoor_data.sender_mac[1],
             s_latest_outdoor_data.sender_mac[2], s_latest_outdoor_data.sender_mac[3],
             s_latest_outdoor_data.sender_mac[4], s_latest_outdoor_data.sender_mac[5]);

    // Matter が開始されていれば属性を更新
    if (s_matter_ready) {
        // Endpoint 1: ベランダ照度 (Light Sensor)
        esp_matter_attr_val_t lux_val = esp_matter_nullable_uint16(matter_lux);
        esp_matter::attribute::update(s_endpoint_id,
                                      IlluminanceMeasurement::Id,
                                      IlluminanceMeasurement::Attributes::MeasuredValue::Id,
                                      &lux_val);

        // Endpoint 3: ベランダ外気温 (Temperature Sensor)
        int16_t matter_outdoor_temp = (int16_t)roundf(pkt->temp_celsius * 100.0f);
        esp_matter_attr_val_t temp_val = esp_matter_nullable_int16(matter_outdoor_temp);
        esp_matter::attribute::update(s_outdoor_temp_endpoint_id,
                                      TemperatureMeasurement::Id,
                                      TemperatureMeasurement::Attributes::MeasuredValue::Id,
                                      &temp_val);

        ESP_LOGD(TAG, "Matter Outdoor updated: Lux=%u, Temp=%d", matter_lux, matter_outdoor_temp);
    }
}

// 親機直結 B3950 サーミスタ (GPIO3) による室内温度計測タスク
static void indoor_temp_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Indoor Temp Sensor sampling task started (2000ms cycle)");

    temp_sensor_data_t temp_data;
    int16_t last_reported_temp = 0;
    bool temp_has_reported = false;
    int64_t last_temp_report_time_ms = 0;

    while (true) {
        if (temp_sensor_read(&temp_data) == ESP_OK) {
            taskENTER_CRITICAL(&s_sensor_spinlock);
            s_latest_temp_data = temp_data;
            s_temp_valid = true;
            taskEXIT_CRITICAL(&s_sensor_spinlock);

            ESP_LOGI(TAG, "[Indoor Temp] RAW: %4d | Volt: %4d mV | Res: %7.1f Ohm | RawTemp: %4.1f C | Temp: %4.1f C (Offset: %+.1f C)",
                     temp_data.raw_adc, temp_data.voltage_mv, temp_data.resistance_ohm,
                     temp_data.temp_raw_celsius, temp_data.temp_celsius, temp_data.offset);

            // Matter Endpoint 2 (室内温度) の更新
            if (s_matter_ready) {
                int64_t now_ms = esp_timer_get_time() / 1000;
                int16_t matter_temp = (int16_t)roundf(temp_data.temp_celsius * 100.0f);
                int temp_diff = abs((int)matter_temp - (int)last_reported_temp);
                // 0.5℃以上の変化またはハートビート周期 (60秒) で送信
                bool should_report = !temp_has_reported ||
                                     (temp_diff >= 50) ||
                                     (now_ms - last_temp_report_time_ms >= MATTER_REPORT_HEARTBEAT_MS);

                if (should_report) {
                    esp_matter_attr_val_t temp_val = esp_matter_nullable_int16(matter_temp);
                    esp_matter::attribute::update(s_temp_endpoint_id,
                                                  TemperatureMeasurement::Id,
                                                  TemperatureMeasurement::Attributes::MeasuredValue::Id,
                                                  &temp_val);
                    last_reported_temp = matter_temp;
                    last_temp_report_time_ms = now_ms;
                    temp_has_reported = true;

                    ESP_LOGD(TAG, "Matter Indoor Temp updated: %d (diff: %d)", matter_temp, temp_diff);
                }
            }
        } else {
            ESP_LOGW(TAG, "Failed to read Indoor Temp sensor");
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static void factory_reset_task(void *pvParameters)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << GPIO_FACTORY_RESET),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    while (true) {
        if (gpio_get_level(GPIO_FACTORY_RESET) == 0) {
            int held = 0;
            while (gpio_get_level(GPIO_FACTORY_RESET) == 0 && held < FACTORY_RESET_HOLD_MS) {
                vTaskDelay(pdMS_TO_TICKS(100));
                held += 100;
            }
            if (held >= FACTORY_RESET_HOLD_MS) {
                ESP_LOGI(TAG, "Factory reset triggered!");
                esp_matter::factory_reset();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged: {
        esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        if (netif) {
            esp_netif_ip_info_t info;
            if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
                char ip[20];
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
                ESP_LOGI(TAG, "Wi-Fi Connected for Matter. IP: %s", ip);
                s_matter_ready = true;
                print_esp_now_info();
            }
        }
        break;
    }
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;
    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric removed");
        break;
    default:
        break;
    }
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id,
                                        uint8_t effect_id, uint8_t effect_variant, void *priv_data)
{
    return ESP_OK;
}

static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id,
                                          uint32_t cluster_id, uint32_t attribute_id,
                                          esp_matter_attr_val_t *val, void *priv_data)
{
    return ESP_OK;
}

static void ensure_unique_id(void)
{
    nvs_handle_t nvs;
    if (nvs_open_from_partition("nvs", "chip-config", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "ensure_unique_id: failed to open NVS");
        return;
    }
    char buf[48] = {};
    size_t len = sizeof(buf);
    bool missing = (nvs_get_str(nvs, "unique-id", buf, &len) != ESP_OK || strlen(buf) < 2);
    if (missing) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(buf, sizeof(buf),
                 "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 (uint8_t)(~mac[0]), (uint8_t)(~mac[1]),
                 (uint8_t)(~mac[2]), (uint8_t)(~mac[3]),
                 (uint8_t)(~mac[4]), (uint8_t)(~mac[5]),
                 mac[0] ^ 0x55u, mac[1] ^ 0xAAu,
                 mac[2] ^ 0x33u, mac[3] ^ 0xCCu);
        nvs_set_str(nvs, "unique-id", buf);
        nvs_commit(nvs);
        ESP_LOGI(TAG, "Generated UniqueID: %s", buf);
    } else {
        ESP_LOGI(TAG, "UniqueID exists: %s", buf);
    }
    nvs_close(nvs);
}

static void store_wifi_credentials(const char *ssid, const char *pass)
{
    if (!ssid || strlen(ssid) == 0) return;

    nvs_handle_t nvs;
    if (nvs_open_from_partition("nvs", "chip-config", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open chip-config namespace");
        return;
    }

    char existing_ssid[64] = {};
    char existing_pass[64] = {};
    size_t ssid_len = sizeof(existing_ssid);
    size_t pass_len = sizeof(existing_pass);
    bool need_write = !(nvs_get_blob(nvs, "wifi-ssid", existing_ssid, &ssid_len) == ESP_OK
                        && ssid_len == strlen(ssid)
                        && memcmp(existing_ssid, ssid, ssid_len) == 0
                        && nvs_get_blob(nvs, "wifi-pass", existing_pass, &pass_len) == ESP_OK
                        && pass_len == strlen(pass)
                        && memcmp(existing_pass, pass, pass_len) == 0);

    if (need_write) {
        nvs_set_blob(nvs, "wifi-ssid", ssid, strlen(ssid));
        nvs_set_blob(nvs, "wifi-pass", pass, strlen(pass));
        nvs_commit(nvs);
        ESP_LOGI(TAG, "WiFi credentials written to chip-config: %s", ssid);
    } else {
        ESP_LOGI(TAG, "WiFi credentials unchanged: %s", ssid);
    }
    nvs_close(nvs);
}

extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " ESP32-C3 SwitchBot Hub & Dual Temp/Lux Sensor    ");
    ESP_LOGI(TAG, "==================================================");

    // Initialize NVS Storage
    ESP_ERROR_CHECK(nvs_config_init());

    // Ensure Unique ID for Matter
    ensure_unique_id();

    // Load saved WiFi and SwitchBot API credentials
    app_config_t config;
    nvs_config_load(&config);

    if (strlen(config.wifi_ssid) > 0) {
        store_wifi_credentials(config.wifi_ssid, config.wifi_pass);
    }

    // ADC1 ユニットの初期化 (cds_sensor_init 経由)
    if (cds_sensor_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize ADC1 unit");
    }

    // 親機直結 B3950 室内温度センサ (ADC1_CH3 / GPIO3) の初期化
    if (temp_sensor_init(cds_sensor_get_adc_handle()) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize Indoor Temp sensor on GPIO3");
    }

    // 室内温度サンプリングタスクの作成 (2秒周期)
    xTaskCreate(indoor_temp_task, "indoor_temp_task", 4096, NULL, 5, NULL);

    // Matter ノードおよびマルチエンドポイントの作成
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    if (node) {
        // Endpoint 1: ベランダ照度 (Light Sensor - ESP-NOW 受信値)
        light_sensor::config_t sensor_config;
        sensor_config.illuminance_measurement.measured_value = 1;
        sensor_config.illuminance_measurement.min_measured_value = 1;
        sensor_config.illuminance_measurement.max_measured_value = 65534;

        endpoint_t *endpoint = light_sensor::create(node, &sensor_config, ENDPOINT_FLAG_NONE, NULL);
        if (endpoint) {
            s_endpoint_id = endpoint::get_id(endpoint);
            ESP_LOGI(TAG, "Matter Outdoor Light Sensor Endpoint ID: %d", s_endpoint_id);
        } else {
            ESP_LOGE(TAG, "Failed to create Outdoor Light Sensor endpoint");
        }

        // Endpoint 2: 室内温度 (Temperature Sensor - 親機直結 GPIO3)
        temperature_sensor::config_t indoor_temp_config;
        indoor_temp_config.temperature_measurement.measured_value = 2500; // default 25.00 C
        indoor_temp_config.temperature_measurement.min_measured_value = -4000; // -40.00 C
        indoor_temp_config.temperature_measurement.max_measured_value = 12000; // 120.00 C

        endpoint_t *indoor_endpoint = temperature_sensor::create(node, &indoor_temp_config, ENDPOINT_FLAG_NONE, NULL);
        if (indoor_endpoint) {
            s_temp_endpoint_id = endpoint::get_id(indoor_endpoint);
            ESP_LOGI(TAG, "Matter Indoor Temperature Sensor Endpoint ID: %d", s_temp_endpoint_id);
        } else {
            ESP_LOGE(TAG, "Failed to create Indoor Temperature Sensor endpoint");
        }

        // Endpoint 3: ベランダ外気温 (Temperature Sensor - ESP-NOW 受信値)
        temperature_sensor::config_t outdoor_temp_config;
        outdoor_temp_config.temperature_measurement.measured_value = 2500; // default 25.00 C
        outdoor_temp_config.temperature_measurement.min_measured_value = -4000; // -40.00 C
        outdoor_temp_config.temperature_measurement.max_measured_value = 12000; // 120.00 C

        endpoint_t *outdoor_endpoint = temperature_sensor::create(node, &outdoor_temp_config, ENDPOINT_FLAG_NONE, NULL);
        if (outdoor_endpoint) {
            s_outdoor_temp_endpoint_id = endpoint::get_id(outdoor_endpoint);
            ESP_LOGI(TAG, "Matter Outdoor Temperature Sensor Endpoint ID: %d", s_outdoor_temp_endpoint_id);
        } else {
            ESP_LOGE(TAG, "Failed to create Outdoor Temperature Sensor endpoint");
        }

        esp_err_t err = esp_matter::start(app_event_cb);
        if (err == ESP_OK) {
            s_matter_ready = true;
            PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kOnNetwork));
        } else {
            ESP_LOGE(TAG, "Failed to start Matter: %d", err);
        }
    } else {
        ESP_LOGE(TAG, "Failed to create Matter node");
    }

    // Initialize Wi-Fi Connection & Web Server
    wifi_manager_init(config.wifi_ssid, config.wifi_pass);
    web_server_start();

    // ESP-NOW 受信機の初期化
    esp_err_t now_err = esp_now_init();
    if (now_err == ESP_OK) {
        ESP_LOGI(TAG, "ESP-NOW Receiver initialized successfully");
        esp_now_register_recv_cb(esp_now_recv_cb);
    } else {
        ESP_LOGE(TAG, "Failed to initialize ESP-NOW: %d", now_err);
    }

    // 子機設定用情報の初期表示
    print_esp_now_info();

    // ファクトリーリセット用タスクの作成 (GPIO9)
    xTaskCreate(factory_reset_task, "factory_reset", 4096, nullptr, 1, nullptr);

    ESP_LOGI(TAG, "Initialization finished successfully!");
    ESP_LOGI(TAG, "Access Web Control Dashboard at: http://esp32-switchbot.local/ or http://%s/", wifi_manager_get_ip());
}

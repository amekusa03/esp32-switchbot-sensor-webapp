#include <stdio.h>
#include <string.h>
#include <math.h>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <nvs_flash.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

// 親機と共通のパケットヘッダー
#include "esp_now_packet.h"

static const char *TAG = "OUTDOOR_SENSOR";

// =============================================================================
// 設定パラメータ
// =============================================================================

// 親機 (Hub) の Wi-Fi STA MAC アドレス
static const uint8_t PARENT_MAC[6] = {0x90, 0xDA, 0x72, 0x8B, 0x23, 0xFC};

// テストモード設定
// 1: 連続送信テストモード (スリープせず 3秒おきにシリアル出力 & 送信)
// 0: 本番電池駆動モード (送信後即 Deep Sleep 5分)
#define TEST_CONTINUOUS_MODE        0

// Deep Sleep 間隔 (秒) : 通常運用は 300秒 (5分)
#define SLEEP_DURATION_SEC          300

// ADC チャンネル設定 (ESP32-C3)
#define ADC_CHAN_CDS                ADC_CHANNEL_1   // GPIO1 (CdS 照度)
#define ADC_CHAN_TEMP               ADC_CHANNEL_3   // GPIO3 (B3950 サーミスタ外気)
#define ADC_CHAN_BATT               ADC_CHANNEL_0   // GPIO0 (電池電圧分圧)

// センサー回路定数
#define VCC_MV                      3300.0f
#define CDS_PULLDOWN_RESISTOR       9100.0f         // CdS用プルダウン抵抗 (約9.1kΩ)
#define TEMP_PULLDOWN_RESISTOR      10000.0f        // サーミスタ用プルダウン抵抗 (10kΩ)
#define B3950_B_CONSTANT            3950.0f
#define B3950_R0                    10000.0f
#define B3950_T0                    (273.15f + 25.0f)

// =============================================================================
// RTC メモリ (Deep Sleep 中も保持される変数)
// =============================================================================
RTC_DATA_ATTR static uint8_t  s_cached_channel = 1; // 前回成功したWi-Fiチャンネル
RTC_DATA_ATTR static uint32_t s_boot_count = 0;     // 起動回数カウンタ

// =============================================================================
// ESP-NOW 送信同期用
// =============================================================================
static SemaphoreHandle_t s_send_sem = NULL;
static volatile bool s_send_success = false;

static void esp_now_send_cb(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    s_send_success = (status == ESP_NOW_SEND_SUCCESS);
    if (s_send_sem) {
        xSemaphoreGive(s_send_sem);
    }
}

// =============================================================================
// センサー計測処理 (ADC)
// =============================================================================
static void read_sensors(adc_oneshot_unit_handle_t adc_handle,
                         adc_cali_handle_t cali_handle_cds,
                         adc_cali_handle_t cali_handle_temp,
                         adc_cali_handle_t cali_handle_batt,
                         esp_now_sensor_packet_t *pkt)
{
    // --- 1. CdS 照度サンプリング (10回平均) ---
    int cds_sum = 0;
    for (int i = 0; i < 10; i++) {
        int r = 0;
        adc_oneshot_read(adc_handle, ADC_CHAN_CDS, &r);
        cds_sum += r;
        esp_rom_delay_us(500);
    }
    int cds_raw = cds_sum / 10;
    int cds_mv = 0;
    if (cali_handle_cds) {
        adc_cali_raw_to_voltage(cali_handle_cds, cds_raw, &cds_mv);
    } else {
        cds_mv = (cds_raw * 3300) / 4095;
    }

    float v_cds = (float)cds_mv;
    if (v_cds < 1.0f) v_cds = 1.0f;
    if (v_cds >= VCC_MV) v_cds = VCC_MV - 1.0f;
    float r_cds = CDS_PULLDOWN_RESISTOR * ((VCC_MV / v_cds) - 1.0f);
    if (r_cds < 1.0f) r_cds = 1.0f;

    // 簡易照度換算 (特性近似: Lux = 500 / (R_cds(kΩ) ^ 1.4))
    float r_k = r_cds / 1000.0f;
    float lux = (r_k > 0.0f) ? (500.0f / powf(r_k, 1.4f)) : 0.0f;
    if (lux < 0.1f) lux = 0.1f;
    if (lux > 100000.0f) lux = 100000.0f;
    pkt->lux = roundf(lux * 10.0f) / 10.0f;

    // --- 2. B3950 サーミスタ外気温サンプリング (10回平均) ---
    int temp_sum = 0;
    for (int i = 0; i < 10; i++) {
        int r = 0;
        adc_oneshot_read(adc_handle, ADC_CHAN_TEMP, &r);
        temp_sum += r;
        esp_rom_delay_us(500);
    }
    int temp_raw = temp_sum / 10;
    int temp_mv = 0;
    if (cali_handle_temp) {
        adc_cali_raw_to_voltage(cali_handle_temp, temp_raw, &temp_mv);
    } else {
        temp_mv = (temp_raw * 3300) / 4095;
    }

    float v_temp = (float)temp_mv;
    if (v_temp < 1.0f) v_temp = 1.0f;
    if (v_temp >= VCC_MV) v_temp = VCC_MV - 1.0f;
    float r_ntc = TEMP_PULLDOWN_RESISTOR * ((VCC_MV / v_temp) - 1.0f);
    if (r_ntc < 1.0f) r_ntc = 1.0f;

    // B3950 Steinhart-Hart 式
    float steinhart = logf(r_ntc / B3950_R0) / B3950_B_CONSTANT + (1.0f / B3950_T0);
    float temp_c = (1.0f / steinhart) - 273.15f;
    pkt->temp_celsius = roundf(temp_c * 10.0f) / 10.0f;

    // --- 3. 電池電圧測定 (GPIO0 分圧 100k+100k の場合: x2) ---
    int batt_raw = 0;
    adc_oneshot_read(adc_handle, ADC_CHAN_BATT, &batt_raw);
    int batt_mv = 0;
    if (cali_handle_batt) {
        adc_cali_raw_to_voltage(cali_handle_batt, batt_raw, &batt_mv);
    } else {
        batt_mv = (batt_raw * 3300) / 4095;
    }
    // 未接続または0Vの場合は 3.0V デフォルト、分圧接続時は x2
    float v_batt = (batt_mv > 100) ? ((float)batt_mv * 2.0f / 1000.0f) : 3.0f;
    pkt->battery_voltage = roundf(v_batt * 100.0f) / 100.0f;

    ESP_LOGI(TAG, "[Sensor Read] CdS: %.1f Lux (RAW:%d) | Outdoor Temp: %.1f C (RAW:%d) | Batt: %.2f V",
             pkt->lux, cds_raw, pkt->temp_celsius, temp_raw, pkt->battery_voltage);
}

// =============================================================================
// ESP-NOW 送信実行 (指定チャンネルで親機へユニキャスト送信)
// =============================================================================
static bool try_send_packet(const esp_now_sensor_packet_t *pkt, uint8_t channel)
{
    // Wi-Fi チャンネル切り替え
    esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);

    // ピア情報更新
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, PARENT_MAC, 6);
    peer.channel = channel;
    peer.encrypt = false;

    // 既存ピアを削除して再登録
    esp_now_del_peer(PARENT_MAC);
    if (esp_now_add_peer(&peer) != ESP_OK) {
        return false;
    }

    s_send_success = false;
    xSemaphoreTake(s_send_sem, 0); // セマフォクリア

    esp_err_t err = esp_now_send(PARENT_MAC, (const uint8_t *)pkt, sizeof(*pkt));
    if (err != ESP_OK) {
        return false;
    }

    // ACK 応答待ち (最大 50ms)
    if (xSemaphoreTake(s_send_sem, pdMS_TO_TICKS(50)) == pdTRUE) {
        return s_send_success;
    }

    return false;
}

// =============================================================================
// メインエントリーポイント (起動 → 計測 → 送信 → Deep Sleep)
// =============================================================================
void app_main(void)
{
    int64_t start_us = esp_timer_get_time();
    s_boot_count++;

    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, " Outdoor Sensor Child (Boot #%" PRIu32 ", Cached CH: %d)", s_boot_count, s_cached_channel);
    ESP_LOGI(TAG, "==================================================");

    s_send_sem = xSemaphoreCreateBinary();

    // 1. NVS 初期化 (Wi-Fi 動作に必要)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. ADC 初期化
    adc_oneshot_unit_handle_t adc_handle = NULL;
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = (adc_oneshot_clk_src_t)0,
    };
    adc_oneshot_new_unit(&init_config, &adc_handle);

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    adc_oneshot_config_channel(adc_handle, ADC_CHAN_CDS, &chan_cfg);
    adc_oneshot_config_channel(adc_handle, ADC_CHAN_TEMP, &chan_cfg);
    adc_oneshot_config_channel(adc_handle, ADC_CHAN_BATT, &chan_cfg);

    adc_cali_handle_t cali_cds = NULL, cali_temp = NULL, cali_batt = NULL;
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    cali_cfg.chan = ADC_CHAN_CDS;
    adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_cds);
    cali_cfg.chan = ADC_CHAN_TEMP;
    adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_temp);
    cali_cfg.chan = ADC_CHAN_BATT;
    adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_batt);

    // 4. Wi-Fi & ESP-NOW 初期化 (Station モード)
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(esp_now_send_cb));

#if TEST_CONTINUOUS_MODE
    ESP_LOGI(TAG, "=== Running in CONTINUOUS TEST MODE (Every 3 seconds) ===");
    while (true) {
        esp_now_sensor_packet_t packet = {0};
        read_sensors(adc_handle, cali_cds, cali_temp, cali_batt, &packet);

        bool sent = false;
        if (try_send_packet(&packet, s_cached_channel)) {
            sent = true;
            ESP_LOGI(TAG, ">> [TEST] SUCCESS on cached channel %d! (ACK received)", s_cached_channel);
        } else {
            ESP_LOGW(TAG, ">> [TEST] Cached channel %d failed. Scanning 1..13...", s_cached_channel);
            for (uint8_t ch = 1; ch <= 13; ch++) {
                if (ch == s_cached_channel) continue;
                if (try_send_packet(&packet, ch)) {
                    s_cached_channel = ch;
                    sent = true;
                    ESP_LOGI(TAG, ">> [TEST] Found parent on Channel %d!", ch);
                    break;
                }
            }
        }
        if (!sent) {
            ESP_LOGE(TAG, ">> [TEST] Failed to reach Parent Hub (Parent may be offline/different MAC).");
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
#else
    // 3. センサーサンプリング
    esp_now_sensor_packet_t packet = {0};
    read_sensors(adc_handle, cali_cds, cali_temp, cali_batt, &packet);

    // 5. 送信処理 (キャッシュチャンネル優先 ➔ 失敗時 1〜13ch スキャン)
    bool sent = false;
    ESP_LOGI(TAG, "Trying cached channel: %d...", s_cached_channel);
    if (try_send_packet(&packet, s_cached_channel)) {
        sent = true;
        ESP_LOGI(TAG, ">> SUCCESS on cached channel %d! (ACK received)", s_cached_channel);
    } else {
        ESP_LOGW(TAG, "Cached channel %d failed. Scanning channels 1 to 13...", s_cached_channel);
        for (uint8_t ch = 1; ch <= 13; ch++) {
            if (ch == s_cached_channel) continue; // 既に試行済み
            if (try_send_packet(&packet, ch)) {
                s_cached_channel = ch;
                sent = true;
                ESP_LOGI(TAG, ">> Found active parent on Channel %d! Updated cached channel.", ch);
                break;
            }
        }
    }

    if (!sent) {
        ESP_LOGE(TAG, "Failed to reach Parent Hub on any channel. Will retry next boot.");
    }

    // 6. 電源断 / Deep Sleep 準備
    esp_now_deinit();
    esp_wifi_stop();
    adc_oneshot_del_unit(adc_handle);

    int64_t elapsed_ms = (esp_timer_get_time() - start_us) / 1000;
    ESP_LOGI(TAG, "Active time: %" PRIu64 " ms. Entering Deep Sleep for %d seconds...", elapsed_ms, SLEEP_DURATION_SEC);

    // Deep Sleep 突入
    esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_DURATION_SEC * 1000000ULL);
    esp_deep_sleep_start();
#endif
}

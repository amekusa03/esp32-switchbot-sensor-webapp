#ifndef TEMP_SENSOR_H
#define TEMP_SENSOR_H

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>
#include <esp_adc/adc_oneshot.h>

#ifdef __cplusplus
extern "C" {
#endif

// B3950 10k サーミスタ回路設定 (10kΩ プルダウン抵抗, 電源 3.3V)
#define TEMP_PULLDOWN_RESISTOR 10000.0f       // 10kΩ プルダウン抵抗
#define TEMP_VCC_MV            3300.0f        // 電源電圧 3.3V (3300mV)
#define B3950_B_CONSTANT       3950.0f        // B定数
#define B3950_R0               10000.0f       // 25℃における基準抵抗値
#define B3950_T0               298.15f        // 25℃ (Kelvin)

typedef struct {
    int raw_adc;             // ADC生データ (0 - 4095)
    int voltage_mv;          // 電圧 (mV)
    float resistance_ohm;    // サーミスタの推定抵抗値 (Ω)
    float temp_raw_celsius;  // 補正前の算出温度 (℃)
    float temp_celsius;      // オフセット適用後の最終温度 (℃)
    float offset;            // 適用中の温度オフセット (℃)
} temp_sensor_data_t;

/**
 * @brief B3950サーミスタ（ADC1_CH3 / GPIO3）の初期化
 * @param adc_handle 初期化済みのADC1ユニットハンドル
 * @return esp_err_t ESP_OK成功時
 */
esp_err_t temp_sensor_init(adc_oneshot_unit_handle_t adc_handle);

/**
 * @brief 温度センサの現在値を計測して取得
 * @param data 計測結果格納先
 * @return esp_err_t ESP_OK成功時
 */
esp_err_t temp_sensor_read(temp_sensor_data_t *data);

/**
 * @brief 温度オフセット値を設定（メモリ上）
 * @param offset オフセット (℃)
 */
void temp_sensor_set_offset(float offset);

/**
 * @brief 現在設定されている温度オフセット値を取得
 * @return float オフセット (℃)
 */
float temp_sensor_get_offset(void);

/**
 * @brief NVSから温度オフセット値をロードして適用
 * @return esp_err_t ESP_OK成功時
 */
esp_err_t temp_sensor_load_offset_from_nvs(void);

/**
 * @brief 現在の温度オフセット値をNVSに保存
 * @return esp_err_t ESP_OK成功時
 */
esp_err_t temp_sensor_save_offset_to_nvs(void);

#ifdef __cplusplus
}
#endif

#endif // TEMP_SENSOR_H

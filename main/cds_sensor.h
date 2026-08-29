#ifndef CDS_SENSOR_H
#define CDS_SENSOR_H

#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// CdS センサ回路設定 (約9.1kΩ プルダウン抵抗: 100kΩ // 10kΩ, 電源 3.3V)
#define CDS_PULLDOWN_RESISTOR  9091.0f        // 約9.1kΩ プルダウン抵抗 (100kΩ // 10kΩ)
#define CDS_VCC_MV             3300.0f         // 電源電圧 3.3V (3300mV)

typedef struct {
    int raw_adc;            // ADC生データ (0 - 4095)
    int voltage_mv;         // キャリブレーション済み電圧 (mV)
    float resistance_ohm;   // CdSセルの推定抵抗値 (Ω)
    float lux;              // 推定照度 (Lux)
    uint16_t matter_value;  // Matter Illuminance Measurement (10000 * log10(lux) + 1)
} cds_sensor_data_t;

/**
 * @brief CdSセンサ（ADC1_CH1 / GPIO1）の初期化
 * @return esp_err_t ESP_OK成功時
 */
#include <esp_adc/adc_oneshot.h>

esp_err_t cds_sensor_init(void);

/**
 * @brief 初期化済みのADC1ユニットハンドルを取得
 */
adc_oneshot_unit_handle_t cds_sensor_get_adc_handle(void);

/**
 * @brief CdSセンサの現在値を計測して取得
 * @param data 計測結果格納先
 * @return esp_err_t ESP_OK成功時
 */
esp_err_t cds_sensor_read(cds_sensor_data_t *data);

/**
 * @brief Lux値からMatter規格のMeasuredValue(10000*log10(lux)+1)を計算
 * @param lux 照度(Lux)
 * @return uint16_t Matter MeasuredValue
 */
uint16_t cds_sensor_lux_to_matter_value(float lux);

/**
 * @brief 校正点を設定する（2点校正: point=1 or 2）
 * @param point 校正点番号 (1=明側, 2=暗側)
 * @param raw   その時点のADC生値
 * @param lux   実測照度 (Lux)
 * @return esp_err_t ESP_OK成功時
 */
esp_err_t cds_sensor_set_calib_point(int point, int raw, float lux);

/**
 * @brief NVSから校正値をロードして適用（init後に呼ぶ）
 * @return esp_err_t ESP_OK成功時
 */
esp_err_t cds_sensor_load_calib_from_nvs(void);

/**
 * @brief 現在の校正点をNVSに保存
 * @return esp_err_t ESP_OK成功時
 */
esp_err_t cds_sensor_save_calib_to_nvs(void);

/**
 * @brief 現在の校正パラメータを取得（NULL可: 不要な出力は省略可）
 */
void cds_sensor_get_calib_params(int *raw1, float *lux1, int *raw2, float *lux2,
                                  float *out_A, float *out_Rref, float *out_n);

#ifdef __cplusplus
}
#endif

#endif // CDS_SENSOR_H

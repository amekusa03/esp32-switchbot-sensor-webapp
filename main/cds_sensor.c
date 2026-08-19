#include <esp_log.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include "cds_sensor.h"
#include "nvs_config.h"

static const char *TAG = "cds_sensor";

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali_handle = NULL;
static bool s_cali_enabled = false;

// 2点校正データ (デフォルト: RAW 1300 = 13 Lux, RAW 70 = 0.1 Lux)
static int   s_calib_raw1 = 1300;
static float s_calib_lux1 = 13.0f;
static int   s_calib_raw2 = 70;
static float s_calib_lux2 = 0.1f;

// 校正パラメータ (computed from 2-point calibration)
static float s_calib_A    = 13.0f;
static float s_calib_Rref = 19540.0f;
static float s_calib_n    = 1.481f;

// 校正パラメータを2点から再計算
static void cds_sensor_compute_calib(void) {
    // 線形ADC近似でRAW → 抵抗値変換
    float v1 = (float)s_calib_raw1 * (CDS_VCC_MV / 4095.0f);
    if (v1 < 1.0f)        v1 = 1.0f;
    if (v1 >= CDS_VCC_MV) v1 = CDS_VCC_MV - 1.0f;
    float r1 = CDS_PULLDOWN_RESISTOR * ((CDS_VCC_MV / v1) - 1.0f);
    if (r1 < 1.0f) r1 = 1.0f;

    float v2 = (float)s_calib_raw2 * (CDS_VCC_MV / 4095.0f);
    if (v2 < 1.0f)        v2 = 1.0f;
    if (v2 >= CDS_VCC_MV) v2 = CDS_VCC_MV - 1.0f;
    float r2 = CDS_PULLDOWN_RESISTOR * ((CDS_VCC_MV / v2) - 1.0f);
    if (r2 < 1.0f) r2 = 1.0f;

    if (r1 != r2 && s_calib_lux1 > 0.0f && s_calib_lux2 > 0.0f && s_calib_lux1 != s_calib_lux2) {
        s_calib_n    = logf(s_calib_lux1 / s_calib_lux2) / logf(r2 / r1);
        s_calib_Rref = r1;
        s_calib_A    = s_calib_lux1;
    }
    ESP_LOGI(TAG, "Calib params updated: A=%.3f R_ref=%.1f n=%.4f",
             s_calib_A, s_calib_Rref, s_calib_n);
}

esp_err_t cds_sensor_init(void)
{
    // ADC1 Units init
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
        .clk_src = (adc_oneshot_clk_src_t)0,
    };
    esp_err_t err = adc_oneshot_new_unit(&init_config, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ADC1 unit: %d", err);
        return err;
    }

    // Channel config: GPIO1 is ADC_CHANNEL_1 on ESP32-C3
    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_1, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config ADC channel: %d", err);
        return err;
    }

    // Calibration scheme setup
#if CONFIG_IDF_TARGET_ESP32C3
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_config, &s_adc_cali_handle);
    if (err == ESP_OK) {
        s_cali_enabled = true;
        ESP_LOGI(TAG, "ADC Calibration curve fitting initialized successfully");
    } else {
        ESP_LOGW(TAG, "ADC Calibration curve fitting failed (%d), using linear fallback", err);
    }
#endif

    ESP_LOGI(TAG, "CdS Sensor initialized on GPIO1 (ADC1_CH1)");

    // NVSから校正値をロード・適用
    cds_sensor_load_calib_from_nvs();

    return ESP_OK;
}

uint16_t cds_sensor_lux_to_matter_value(float lux)
{
    if (lux < 1.0f) {
        return 1;
    }
    float val = 10000.0f * log10f(lux) + 1.0f;
    if (val > 65534.0f) {
        return 65534;
    }
    return (uint16_t)roundf(val);
}

esp_err_t cds_sensor_read(cds_sensor_data_t *data)
{
    if (!s_adc_handle || !data) {
        return ESP_ERR_INVALID_STATE;
    }

    // 多重サンプリングでノイズ軽減 (10回分散平均: 各サンプル間1msディレイ)
    int samples = 10;
    int sum_raw = 0;
    for (int i = 0; i < samples; i++) {
        int r = 0;
        adc_oneshot_read(s_adc_handle, ADC_CHANNEL_1, &r);
        sum_raw += r;
        if (i < samples - 1) {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
    int raw = sum_raw / samples;

    int voltage_mv = 0;
    if (s_cali_enabled && s_adc_cali_handle) {
        adc_cali_raw_to_voltage(s_adc_cali_handle, raw, &voltage_mv);
    } else {
        voltage_mv = (raw * 3300) / 4095;
    }

    float v_mv = (float)voltage_mv;
    if (v_mv <= 1.0f) {
        v_mv = 1.0f;
    }
    if (v_mv >= CDS_VCC_MV) {
        v_mv = CDS_VCC_MV - 1.0f;
    }

    // 分圧回路計算: Vout = Vcc * R_pulldown / (R_cds + R_pulldown)
    // => R_cds = R_pulldown * ((Vcc / Vout) - 1.0)
    float r_cds = CDS_PULLDOWN_RESISTOR * ((CDS_VCC_MV / v_mv) - 1.0f);
    if (r_cds < 1.0f) {
        r_cds = 1.0f;
    }

    // 動的校正パラメータに基づく照度計算 (NVSから読み込み、未設定時はデフォルト値)
    float lux = s_calib_A * powf(s_calib_Rref / r_cds, s_calib_n);
    // RAW 10以下は完全な暗闇 (0 lux) として処理
    if (raw <= 10) {
        lux = 0.0f;
    } else {
        if (lux < 0.1f) {
            lux = 0.1f;
        }
        if (lux > 100000.0f) {
            lux = 100000.0f;
        }
        lux = roundf(lux * 100.0f) / 100.0f;
    }

    data->raw_adc = raw;
    data->voltage_mv = voltage_mv;
    data->resistance_ohm = r_cds;
    data->lux = lux;
    data->matter_value = cds_sensor_lux_to_matter_value(lux);

    return ESP_OK;
}

esp_err_t cds_sensor_set_calib_point(int point, int raw, float lux) {
    if (point != 1 && point != 2) return ESP_ERR_INVALID_ARG;
    if (lux <= 0.0f || raw < 0)  return ESP_ERR_INVALID_ARG;

    if (point == 1) {
        s_calib_raw1 = raw;
        s_calib_lux1 = lux;
    } else {
        s_calib_raw2 = raw;
        s_calib_lux2 = lux;
    }
    ESP_LOGI(TAG, "Calib point %d set: RAW=%d lux=%.3f", point, raw, lux);
    cds_sensor_compute_calib();
    return ESP_OK;
}

esp_err_t cds_sensor_load_calib_from_nvs(void) {
    cds_calib_t calib;
    esp_err_t err = nvs_config_load_cds_calib(&calib);
    // nvs_config_load_cds_calib はエラー時もデフォルト値を設定するので常に適用
    s_calib_raw1 = calib.raw1;
    s_calib_lux1 = calib.lux1;
    s_calib_raw2 = calib.raw2;
    s_calib_lux2 = calib.lux2;
    cds_sensor_compute_calib();
    return err;
}

esp_err_t cds_sensor_save_calib_to_nvs(void) {
    cds_calib_t calib = {
        .raw1 = s_calib_raw1,
        .lux1 = s_calib_lux1,
        .raw2 = s_calib_raw2,
        .lux2 = s_calib_lux2,
    };
    return nvs_config_save_cds_calib(&calib);
}

void cds_sensor_get_calib_params(int *raw1, float *lux1, int *raw2, float *lux2,
                                  float *out_A, float *out_Rref, float *out_n) {
    if (raw1)    *raw1    = s_calib_raw1;
    if (lux1)    *lux1    = s_calib_lux1;
    if (raw2)    *raw2    = s_calib_raw2;
    if (lux2)    *lux2    = s_calib_lux2;
    if (out_A)    *out_A    = s_calib_A;
    if (out_Rref) *out_Rref = s_calib_Rref;
    if (out_n)    *out_n    = s_calib_n;
}

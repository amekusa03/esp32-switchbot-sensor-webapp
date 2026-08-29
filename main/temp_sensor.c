#include <esp_log.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include "temp_sensor.h"
#include "nvs_config.h"

static const char *TAG = "temp_sensor";

static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_adc_cali_handle = NULL;
static bool s_cali_enabled = false;

static float s_temp_offset = 0.0f;

esp_err_t temp_sensor_init(adc_oneshot_unit_handle_t adc_handle)
{
    if (!adc_handle) {
        ESP_LOGE(TAG, "ADC handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    s_adc_handle = adc_handle;

    // Channel config: GPIO3 is ADC_CHANNEL_3 on ESP32-C3
    adc_oneshot_chan_cfg_t chan_config = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_oneshot_config_channel(s_adc_handle, ADC_CHANNEL_3, &chan_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to config ADC channel 3 (GPIO3): %d", err);
        return err;
    }

    // Calibration scheme setup
#if CONFIG_IDF_TARGET_ESP32C3
    adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .chan = ADC_CHANNEL_3,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cali_config, &s_adc_cali_handle);
    if (err == ESP_OK) {
        s_cali_enabled = true;
        ESP_LOGI(TAG, "ADC Calibration curve fitting initialized for GPIO3");
    } else {
        ESP_LOGW(TAG, "ADC Calibration curve fitting failed (%d) for GPIO3, fallback to linear", err);
    }
#endif

    ESP_LOGI(TAG, "Temp Sensor initialized on GPIO3 (ADC1_CH3)");

    // Load offset from NVS
    temp_sensor_load_offset_from_nvs();

    return ESP_OK;
}

void temp_sensor_set_offset(float offset) {
    s_temp_offset = offset;
    ESP_LOGI(TAG, "Temp offset set to %.2f C", offset);
}

float temp_sensor_get_offset(void) {
    return s_temp_offset;
}

esp_err_t temp_sensor_load_offset_from_nvs(void) {
    float offset = 0.0f;
    esp_err_t err = nvs_config_load_temp_offset(&offset);
    s_temp_offset = offset;
    ESP_LOGI(TAG, "Temp offset loaded from NVS: %.2f C", s_temp_offset);
    return err;
}

esp_err_t temp_sensor_save_offset_to_nvs(void) {
    return nvs_config_save_temp_offset(s_temp_offset);
}

esp_err_t temp_sensor_read(temp_sensor_data_t *data)
{
    if (!s_adc_handle || !data) {
        return ESP_ERR_INVALID_STATE;
    }

    // 多重サンプリング (10回平均)
    int samples = 10;
    int sum_raw = 0;
    for (int i = 0; i < samples; i++) {
        int r = 0;
        adc_oneshot_read(s_adc_handle, ADC_CHANNEL_3, &r);
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
    if (v_mv >= TEMP_VCC_MV) {
        v_mv = TEMP_VCC_MV - 1.0f;
    }

    // 分圧回路: 3.3V --- Thermistor(R_ntc) --- GPIO3 --- Pulldown(10k) --- GND
    // Vout = Vcc * R_pulldown / (R_ntc + R_pulldown)
    // => R_ntc = R_pulldown * ((Vcc / Vout) - 1.0)
    float r_ntc = TEMP_PULLDOWN_RESISTOR * ((TEMP_VCC_MV / v_mv) - 1.0f);
    if (r_ntc < 1.0f) {
        r_ntc = 1.0f;
    }

    // B定数演算 (B3950)
    // 1/T = 1/T0 + (1/B) * ln(R/R0)
    float steinhart = r_ntc / B3950_R0;        // (R / R0)
    steinhart = logf(steinhart);              // ln(R / R0)
    steinhart /= B3950_B_CONSTANT;            // (1/B) * ln(R / R0)
    steinhart += 1.0f / B3950_T0;             // + (1 / T0)
    steinhart = 1.0f / steinhart;             // Invert to get Kelvin
    float temp_raw = steinhart - 273.15f;     // Convert to Celsius

    // オフセット適用
    float temp_calibrated = temp_raw + s_temp_offset;

    // 丸め (少数第1位)
    temp_raw = roundf(temp_raw * 10.0f) / 10.0f;
    temp_calibrated = roundf(temp_calibrated * 10.0f) / 10.0f;

    data->raw_adc = raw;
    data->voltage_mv = voltage_mv;
    data->resistance_ohm = r_ntc;
    data->temp_raw_celsius = temp_raw;
    data->temp_celsius = temp_calibrated;
    data->offset = s_temp_offset;

    return ESP_OK;
}

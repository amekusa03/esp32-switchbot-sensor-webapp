#ifndef ESP_NOW_PACKET_H
#define ESP_NOW_PACKET_H

#include <stdint.h>

// 子機（ベランダ）から親機へ送信するセンサーデータ構造体 (12 bytes)
typedef struct __attribute__((packed)) {
    float lux;              // ベランダ照度 (Lux)
    float temp_celsius;     // ベランダ外気温 (℃)
    float battery_voltage;  // 子機の電池電圧 (V)
} esp_now_sensor_packet_t;

// 親機が保持するベランダセンサー状態データ
typedef struct {
    float lux;
    uint16_t matter_value;
    float temp_celsius;
    float battery_voltage;
    int64_t last_received_ms;
    uint8_t sender_mac[6];
    bool valid;
} outdoor_sensor_data_t;

#endif // ESP_NOW_PACKET_H

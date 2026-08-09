#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_manager_init(const char *ssid, const char *password);
bool wifi_manager_is_connected(void);
bool wifi_manager_is_ap_mode(void);
const char *wifi_manager_get_ip(void);
int wifi_manager_get_rssi(void);
bool wifi_manager_is_time_synced(void);

#endif // WIFI_MANAGER_H

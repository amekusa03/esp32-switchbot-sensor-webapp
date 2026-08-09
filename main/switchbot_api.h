#ifndef SWITCHBOT_API_H
#define SWITCHBOT_API_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

esp_err_t switchbot_get_devices(const char *token, const char *secret, char **response_json_out);
esp_err_t switchbot_get_device_status(const char *token, const char *secret, const char *device_id, char **response_json_out);
esp_err_t switchbot_send_command(const char *token, const char *secret, const char *device_id, const char *command, const char *parameter, const char *command_type, char **response_json_out);
esp_err_t switchbot_get_scenes(const char *token, const char *secret, char **response_json_out);
esp_err_t switchbot_execute_scene(const char *token, const char *secret, const char *scene_id, char **response_json_out);

#endif // SWITCHBOT_API_H

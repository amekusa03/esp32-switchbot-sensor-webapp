#include "switchbot_api.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "SB_API";
#define BASE_URL "https://api.switchbot.net/v1.1"

typedef struct {
    char *buffer;
    size_t size;
    size_t capacity;
} response_buffer_t;

static esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
    response_buffer_t *buf = (response_buffer_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (buf) {
            if (buf->size + evt->data_len + 1 > buf->capacity) {
                size_t new_cap = buf->capacity == 0 ? 4096 : buf->capacity * 2;
                while (new_cap < buf->size + evt->data_len + 1) {
                    new_cap *= 2;
                }
                char *new_buf = realloc(buf->buffer, new_cap);
                if (!new_buf) {
                    ESP_LOGE(TAG, "Failed to allocate memory for HTTP response");
                    return ESP_FAIL;
                }
                buf->buffer = new_buf;
                buf->capacity = new_cap;
            }
            memcpy(buf->buffer + buf->size, evt->data, evt->data_len);
            buf->size += evt->data_len;
            buf->buffer[buf->size] = '\0';
        }
    }
    return ESP_OK;
}

static void trim_string(char *str) {
    if (!str) return;
    char *end = str + strlen(str) - 1;
    while (end >= str && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    char *start = str;
    while (*start && (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n')) {
        start++;
    }
    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

static void generate_nonce(char *nonce_out, size_t max_len) {
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    snprintf(nonce_out, max_len, "%08" PRIx32 "%08" PRIx32, r1, r2);
}

static uint64_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec * 1000ULL) + (tv.tv_usec / 1000ULL);
}

static esp_err_t generate_signature(const char *token, const char *secret, const char *t_str, const char *nonce, char *sign_out, size_t sign_max_len) {
    char clean_token[128];
    char clean_secret[128];
    strncpy(clean_token, token, sizeof(clean_token) - 1);
    clean_token[sizeof(clean_token) - 1] = '\0';
    trim_string(clean_token);

    strncpy(clean_secret, secret, sizeof(clean_secret) - 1);
    clean_secret[sizeof(clean_secret) - 1] = '\0';
    trim_string(clean_secret);

    char data[512];
    snprintf(data, sizeof(data), "%s%s%s", clean_token, t_str, nonce);

    unsigned char hmac[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) {
        mbedtls_md_free(&ctx);
        return ESP_FAIL;
    }
    if (mbedtls_md_setup(&ctx, info, 1) != 0) {
        mbedtls_md_free(&ctx);
        return ESP_FAIL;
    }
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)clean_secret, strlen(clean_secret));
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)data, strlen(data));
    mbedtls_md_hmac_finish(&ctx, hmac);
    mbedtls_md_free(&ctx);

    size_t olen = 0;
    mbedtls_base64_encode((unsigned char *)sign_out, sign_max_len - 1, &olen, hmac, 32);
    sign_out[olen] = '\0';
    return ESP_OK;
}

static esp_err_t http_request_send(const char *url, esp_http_client_method_t method, const char *token, const char *secret, const char *post_data, char **response_json_out) {
    if (!token || strlen(token) == 0 || !secret || strlen(secret) == 0) {
        ESP_LOGE(TAG, "SwitchBot Token or Secret is missing!");
        return ESP_ERR_INVALID_ARG;
    }

    char clean_token[128];
    char clean_secret[128];
    strncpy(clean_token, token, sizeof(clean_token) - 1);
    clean_token[sizeof(clean_token) - 1] = '\0';
    trim_string(clean_token);

    strncpy(clean_secret, secret, sizeof(clean_secret) - 1);
    clean_secret[sizeof(clean_secret) - 1] = '\0';
    trim_string(clean_secret);

    uint64_t t_ms = get_timestamp_ms();
    if (t_ms < 1600000000000ULL) {
        ESP_LOGE(TAG, "NTP Time NOT synchronized! Current timestamp: %" PRIu64 " (Year 1970). Waiting for NTP...", t_ms);
        return ESP_ERR_INVALID_STATE;
    }

    char t_str[32];
    snprintf(t_str, sizeof(t_str), "%" PRIu64, t_ms);

    char nonce[33];
    generate_nonce(nonce, sizeof(nonce));

    char sign[128];
    if (generate_signature(clean_token, clean_secret, t_str, nonce, sign, sizeof(sign)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to calculate HMAC-SHA256 signature");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Sending SwitchBot API Request -> t: %s, nonce: %s", t_str, nonce);

    response_buffer_t resp_buf = { .buffer = NULL, .size = 0, .capacity = 0 };

    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .event_handler = _http_event_handler,
        .user_data = &resp_buf,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", clean_token);
    esp_http_client_set_header(client, "sign", sign);
    esp_http_client_set_header(client, "t", t_str);
    esp_http_client_set_header(client, "nonce", nonce);
    esp_http_client_set_header(client, "Content-Type", "application/json; charset=utf8");

    if (post_data && (method == HTTP_METHOD_POST || method == HTTP_METHOD_PUT)) {
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP %s -> Status: %d, Err: %s", url, status_code, esp_err_to_name(err));
    if (resp_buf.buffer) {
        ESP_LOGI(TAG, "SwitchBot API Response Body: %s", resp_buf.buffer);
    }

    esp_http_client_cleanup(client);

    if (err == ESP_OK && resp_buf.buffer) {
        *response_json_out = resp_buf.buffer;
        return ESP_OK;
    } else {
        if (resp_buf.buffer) free(resp_buf.buffer);
        *response_json_out = NULL;
        return (err != ESP_OK) ? err : ESP_FAIL;
    }
}

esp_err_t switchbot_get_devices(const char *token, const char *secret, char **response_json_out) {
    char url[256];
    snprintf(url, sizeof(url), "%s/devices", BASE_URL);
    return http_request_send(url, HTTP_METHOD_GET, token, secret, NULL, response_json_out);
}

esp_err_t switchbot_get_device_status(const char *token, const char *secret, const char *device_id, char **response_json_out) {
    char url[256];
    snprintf(url, sizeof(url), "%s/devices/%s/status", BASE_URL, device_id);
    return http_request_send(url, HTTP_METHOD_GET, token, secret, NULL, response_json_out);
}

esp_err_t switchbot_send_command(const char *token, const char *secret, const char *device_id, const char *command, const char *parameter, const char *command_type, char **response_json_out) {
    char url[256];
    snprintf(url, sizeof(url), "%s/devices/%s/commands", BASE_URL, device_id);

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"command\":\"%s\",\"parameter\":\"%s\",\"commandType\":\"%s\"}",
             command ? command : "turnOn",
             parameter ? parameter : "default",
             command_type ? command_type : "command");

    return http_request_send(url, HTTP_METHOD_POST, token, secret, payload, response_json_out);
}

esp_err_t switchbot_get_scenes(const char *token, const char *secret, char **response_json_out) {
    char url[256];
    snprintf(url, sizeof(url), "%s/scenes", BASE_URL);
    return http_request_send(url, HTTP_METHOD_GET, token, secret, NULL, response_json_out);
}

esp_err_t switchbot_execute_scene(const char *token, const char *secret, const char *scene_id, char **response_json_out) {
    char url[256];
    snprintf(url, sizeof(url), "%s/scenes/%s/execute", BASE_URL, scene_id);
    return http_request_send(url, HTTP_METHOD_POST, token, secret, "{}", response_json_out);
}

#include "web_server.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "platform/board_config.h"

static httpd_handle_t s_httpd;
static web_view_provider_t s_get_view;
static void *s_view_context;
static const char *TAG = "web_server";

/* ESP-IDF embeds web_page.html into the application image at build time. */
extern const uint8_t web_page_html_start[] asm("_binary_web_page_html_start");
extern const uint8_t web_page_html_end[] asm("_binary_web_page_html_end");

static void wifi_event_handler(void *context, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)context;
    if (event_base != WIFI_EVENT) return;

    switch (event_id) {
    case WIFI_EVENT_AP_START:
        ESP_LOGI(TAG, "SoftAP radio started");
        break;
    case WIFI_EVENT_AP_STOP:
        ESP_LOGW(TAG, "SoftAP radio stopped");
        break;
    case WIFI_EVENT_AP_STACONNECTED: {
        const wifi_event_ap_staconnected_t *event = event_data;
        ESP_LOGI(TAG, "Station connected: " MACSTR " (aid=%u)",
                 MAC2STR(event->mac), event->aid);
        break;
    }
    case WIFI_EVENT_AP_STADISCONNECTED: {
        const wifi_event_ap_stadisconnected_t *event = event_data;
        ESP_LOGI(TAG, "Station disconnected: " MACSTR " (aid=%u reason=%u)",
                 MAC2STR(event->mac), event->aid, event->reason);
        break;
    }
    default:
        break;
    }
}

static const char *mode_name(load_mode_t mode)
{
    static const char *const names[] = {"CC", "CV", "CP", "CR"};
    return mode >= 0 && mode < LOAD_MODE_COUNT ? names[mode] : "UNKNOWN";
}

static const char *status_name(app_status_t status)
{
    static const char *const names[] = {
        "NO_INPUT", "OFF", "EDIT", "RUN", "WAITING_INPUT",
        "INA_ERR", "LIMIT", "OUTPUT_ERR"
    };
    return status >= 0 && status < APP_STATUS_COUNT ? names[status] : "UNKNOWN";
}

static float finite_float(float value)
{
    return isfinite(value) ? value : 0.0f;
}

static double finite_double(double value)
{
    return isfinite(value) ? value : 0.0;
}

static esp_err_t status_handler(httpd_req_t *request)
{
    app_view_t view;
    if (!s_get_view || !s_get_view(s_view_context, &view)) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "snapshot unavailable");
    }

    char json[512];
    int length = snprintf(json, sizeof(json),
        "{\"mode\":\"%s\",\"status\":\"%s\",\"setpoint\":%.5f,"
        "\"voltage_v\":%.5f,\"current_a\":%.5f,\"power_w\":%.5f,"
        "\"energy_wh\":%.6f,\"runtime_seconds\":%.3f,\"duty\":%.5f,"
        "\"enabled\":%s,\"runtime_counting\":%s,\"sequence\":%lu}",
        mode_name(view.mode), status_name(view.status), finite_float(view.setpoint),
        finite_float(view.voltage_v), finite_float(view.current_a),
        finite_float(view.power_w), finite_double(view.energy_wh),
        finite_double(view.runtime_seconds), finite_float(view.duty),
        view.enabled ? "true" : "false",
        view.runtime_counting ? "true" : "false",
        (unsigned long)view.measurement_sequence);
    if (length < 0 || (size_t)length >= sizeof(json)) {
        httpd_resp_set_status(request, "500 Internal Server Error");
        return httpd_resp_sendstr(request, "response too large");
    }
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, json, length);
}

static esp_err_t page_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    size_t page_length = (size_t)(web_page_html_end - web_page_html_start);
    return httpd_resp_send(request, (const char *)web_page_html_start,
                           (ssize_t)page_length);
}

esp_err_t web_server_start(web_view_provider_t get_view, void *context)
{
    if (!get_view) return ESP_ERR_INVALID_ARG;
    if (s_httpd) return ESP_ERR_INVALID_STATE;
    s_get_view = get_view;
    s_view_context = context;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (!ap_netif) return ESP_FAIL;

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     wifi_event_handler, NULL);
    if (err != ESP_OK) return err;

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wifi_cfg);
    if (err != ESP_OK) return err;
    wifi_config_t ap_cfg = {0};
    _Static_assert(sizeof(BOARD_WIFI_AP_SSID) - 1 <= sizeof(ap_cfg.ap.ssid),
                   "Wi-Fi AP SSID exceeds the 32-byte limit");
    _Static_assert(sizeof(BOARD_WIFI_AP_PASSWORD) - 1 <= sizeof(ap_cfg.ap.password),
                   "Wi-Fi AP password exceeds the field limit");
    memcpy(ap_cfg.ap.ssid, BOARD_WIFI_AP_SSID, sizeof(BOARD_WIFI_AP_SSID) - 1);
    memcpy(ap_cfg.ap.password, BOARD_WIFI_AP_PASSWORD,
           sizeof(BOARD_WIFI_AP_PASSWORD) - 1);
    ap_cfg.ap.ssid_len = (uint8_t)(sizeof(BOARD_WIFI_AP_SSID) - 1);
    ap_cfg.ap.channel = BOARD_WIFI_AP_CHANNEL;
    ap_cfg.ap.max_connection = BOARD_WIFI_AP_MAX_CLIENTS;
    ap_cfg.ap.ssid_hidden = 0;
    ap_cfg.ap.beacon_interval = 100;
    if (strlen(BOARD_WIFI_AP_PASSWORD) == 0) {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
        ap_cfg.ap.pmf_cfg.required = false;
    }
    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    wifi_config_t active_ap_cfg = {0};
    err = esp_wifi_get_config(WIFI_IF_AP, &active_ap_cfg);
    if (err != ESP_OK) return err;
    int8_t tx_power = 0;
    err = esp_wifi_get_max_tx_power(&tx_power);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "AP config: ssid=%.*s channel=%u hidden=%u auth=%u tx_power=%d qdBm",
             active_ap_cfg.ap.ssid_len, active_ap_cfg.ap.ssid,
             active_ap_cfg.ap.channel, active_ap_cfg.ap.ssid_hidden,
             active_ap_cfg.ap.authmode, tx_power);
    esp_netif_ip_info_t ip_info;
    err = esp_netif_get_ip_info(ap_netif, &ip_info);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "AP address: " IPSTR, IP2STR(&ip_info.ip));

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 4;
    err = httpd_start(&s_httpd, &http_cfg);
    if (err != ESP_OK) return err;

    const httpd_uri_t page_uri = {
        .uri = "/", .method = HTTP_GET, .handler = page_handler
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status", .method = HTTP_GET, .handler = status_handler
    };
    err = httpd_register_uri_handler(s_httpd, &page_uri);
    if (err != ESP_OK) return err;
    err = httpd_register_uri_handler(s_httpd, &status_uri);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "HTTP server ready; page=http://192.168.4.1/ status=/api/status");
    ESP_LOGI(TAG, "AP configured: SSID=%s",
             BOARD_WIFI_AP_SSID);
    return ESP_OK;
}

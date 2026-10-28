#include "wiimote_web.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_RETRY_LIMIT 10
#define MAX_HTTP_BODY 96

static const char *TAG = "wiimote_web";
static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_button_lock;
static uint16_t s_button_mask;
static wiimote_web_button_callback_t s_button_callback;
static esp_netif_t *s_station;
static httpd_handle_t s_http_server;
static unsigned s_retry_count;
static bool s_hid_connected;

static const struct {
    const char *name;
    uint16_t mask;
} s_buttons[] = {
    {"A", 0x0008}, {"B", 0x0004}, {"1", 0x0002}, {"2", 0x0001},
    {"Up", 0x0800}, {"Down", 0x0400}, {"Left", 0x0100}, {"Right", 0x0200},
    {"Plus", 0x1000}, {"Minus", 0x0010}, {"Home", 0x0080},
    {"AB", 0x000c}, {"ONE_TWO", 0x0003},
};

static const char INDEX_HTML[] =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
    "<title>ESPmote Controller</title><style>"
    "*{box-sizing:border-box}body{margin:0;background:#101522;color:#edf2ff;"
    "font:16px system-ui,sans-serif;display:grid;place-items:center;min-height:100vh}"
    ".wrap{width:min(760px,100%);padding:20px}h1{text-align:center;margin:4px}"
    "#status{text-align:center;color:#8fa0bf;margin:6px 0 22px;min-height:1.5em}"
    ".remote{background:#e8eaf0;color:#172033;border-radius:48px;padding:24px;"
    "box-shadow:0 18px 45px #0008;display:grid;grid-template-columns:1fr 1fr 1fr;"
    "grid-template-rows:repeat(6,56px);gap:12px;max-width:620px;margin:auto}"
    "button{border:0;border-radius:18px;background:#263650;color:white;font-weight:700;"
    "font-size:18px;touch-action:none;user-select:none;-webkit-user-select:none;"
    "box-shadow:0 5px 0 #111a;cursor:pointer}button:active,button.down{"
    "transform:translateY(4px);box-shadow:0 1px 0 #111a;background:#13a89e}"
    ".dpad{grid-column:1;grid-row:2/5;display:grid;grid-template:repeat(3,1fr)/repeat(3,1fr);gap:5px}"
    ".dpad button{font-size:20px;border-radius:12px;background:#40516d}"
    ".up{grid-area:1/2}.left{grid-area:2/1}.right{grid-area:2/3}.down{grid-area:3/2}"
    ".ab{grid-column:3;grid-row:2/5;display:flex;flex-direction:column;"
    "align-items:center;justify-content:space-around} .ab button{width:74px;height:74px;"
    "border-radius:50%;font-size:24px;background:#c74455}.ab button:first-child{"
    "background:#426ec6;width:88px;height:88px}.sys{grid-row:5;align-self:center;"
    "height:50px;background:#66728a}.sys.one{grid-column:1}.sys.two{grid-column:3}"
    ".sys.home{grid-column:2;background:#d09b43}.combo{grid-column:1/4;grid-row:6;"
    "height:48px;background:#506a88}.combo-row{grid-column:1/4;grid-row:6;display:grid;"
    "grid-template-columns:1fr 1fr;gap:10px}@media(max-width:460px){.remote{"
    "padding:18px;gap:8px;grid-template-rows:repeat(5,56px)}.ab button{width:62px;"
    "height:62px}.ab button:first-child{width:76px;height:76px}}"
    "</style></head><body><main class='wrap'><h1>ESPmote</h1>"
    "<div id='status'>Connecting to ESP32…</div><section class='remote' aria-label='Wii Remote'>"
    "<div class='dpad'><button class='up' data-button='Up'>▲</button>"
    "<button class='left' data-button='Left'>◀</button><button class='right' data-button='Right'>▶</button>"
    "<button class='down' data-button='Down'>▼</button></div>"
    "<div class='ab'><button data-button='A'>A</button><button data-button='B'>B</button></div>"
    "<button class='sys one' data-button='1'>1</button><button class='sys home' data-button='Home'>⌂</button>"
    "<button class='sys two' data-button='2'>2</button>"
    "<button class='sys' style='grid-column:1;grid-row:1' data-button='Minus'>−</button>"
    "<button class='sys' style='grid-column:3;grid-row:1' data-button='Plus'>+</button>"
    "<div class='combo-row'><button class='combo' data-button='AB'>A + B</button>"
    "<button class='combo' data-button='ONE_TWO'>1 + 2</button></div>"
    "</section></main><script>"
    "const statusEl=document.querySelector('#status');let held=new Set(),queue=Promise.resolve();"
    "function send(name,pressed){queue=queue.then(async()=>{try{let r=await fetch('/api/button',{method:'POST',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'name='+encodeURIComponent(name)+'&pressed='+(pressed?'1':'0')});"
    "if(!r.ok)throw Error(await r.text());statusEl.textContent='Connected · '+"
    "(pressed?'Pressed ':'Released ')+name}catch(e){statusEl.textContent='Error: '+e.message}})}"
    "function press(b){if(held.has(b))return;held.add(b);b.classList.add('down');"
    "send(b.dataset.button,true)}function release(b){if(!held.has(b))return;"
    "held.delete(b);b.classList.remove('down');send(b.dataset.button,false)}"
    "document.querySelectorAll('[data-button]').forEach(b=>{b.addEventListener('pointerdown',e=>{"
    "e.preventDefault();b.setPointerCapture(e.pointerId);press(b)});"
    "for(const ev of ['pointerup','pointercancel','lostpointercapture'])b.addEventListener(ev,()=>release(b));"
    "b.addEventListener('contextmenu',e=>e.preventDefault())});"
    "window.addEventListener('blur',()=>{for(const b of document.querySelectorAll('.down'))release(b)});"
    "document.addEventListener('visibilitychange',()=>{if(document.hidden){for(const b of document.querySelectorAll('.down'))release(b)}});"
    "function updateStatus(s){statusEl.textContent=s.wifi?"
    "'Wi-Fi connected · '+s.ip+(s.connected?' · HID connected':' · waiting for DolphinBar'):"
    "'Wi-Fi not connected yet';statusEl.dataset.connected=s.connected?'1':'0';"
    "if(!s.connected)for(const b of document.querySelectorAll('.down'))release(b)}"
    "fetch('/api/status').then(r=>r.json()).then(updateStatus).catch(()=>{"
    "statusEl.textContent='Controller page loaded; status unavailable'})"
    ".finally(()=>setInterval(()=>fetch('/api/status').then(r=>r.json()).then(updateStatus)"
    ".catch(()=>{}),1000));</script></body></html>";

static uint16_t get_button_mask(const char *name)
{
    for (size_t i = 0; i < sizeof(s_buttons) / sizeof(s_buttons[0]); ++i) {
        if (strcmp(name, s_buttons[i].name) == 0) return s_buttons[i].mask;
    }
    return 0;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char ip[16] = "0.0.0.0";
    esp_netif_ip_info_t info = {0};
    if (s_station && esp_netif_get_ip_info(s_station, &info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }
    char response[128];
    snprintf(response, sizeof(response), "{\"wifi\":%s,\"connected\":%s,\"ip\":\"%s\"}",
             (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) ? "true" : "false",
             s_hid_connected ? "true" : "false", ip);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t button_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_HTTP_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request size");
        return ESP_FAIL;
    }
    char body[MAX_HTTP_BODY + 1];
    size_t total = 0;
    while (total < req->content_len) {
        int received = httpd_req_recv(req, body + total, req->content_len - total);
        if (received <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request body missing");
            return ESP_FAIL;
        }
        total += (size_t)received;
    }
    body[total] = '\0';

    char name[12] = {0};
    char pressed_text[2] = {0};
    bool have_name = false, have_pressed = false;
    char *save = NULL;
    for (char *token = strtok_r(body, "&", &save); token != NULL; token = strtok_r(NULL, "&", &save)) {
        if (strncmp(token, "name=", 5) == 0 && strlen(token + 5) < sizeof(name)) {
            strcpy(name, token + 5);
            have_name = true;
        } else if (strncmp(token, "pressed=", 8) == 0 && strlen(token + 8) == 1 &&
                   (token[8] == '0' || token[8] == '1')) {
            pressed_text[0] = token[8];
            have_pressed = true;
        }
    }
    uint16_t mask = have_name ? get_button_mask(name) : 0;
    if (!mask || !have_pressed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown button or state");
        return ESP_FAIL;
    }

    xSemaphoreTake(s_button_lock, portMAX_DELAY);
    if (!s_hid_connected) {
        xSemaphoreGive(s_button_lock);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "DolphinBar HID is not connected");
        return ESP_FAIL;
    }
    bool pressed = pressed_text[0] == '1';
    if (pressed) s_button_mask |= mask;
    else s_button_mask &= (uint16_t)~mask;
    uint16_t new_state = s_button_mask;
    xSemaphoreGive(s_button_lock);

    if (s_button_callback) s_button_callback(new_state);
    ESP_LOGI(TAG, "%s %s (mask=0x%04x state=0x%04x)", pressed ? "Press" : "Release", name, mask, new_state);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static const httpd_uri_t s_index_uri = {
    .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_status_uri = {
    .uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_button_uri = {
    .uri = "/api/button", .method = HTTP_POST, .handler = button_handler, .user_ctx = NULL,
};

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_button_uri));

    esp_netif_ip_info_t info = {0};
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_station, &info));
    ESP_LOGI(TAG, "HTTP server ready: http://" IPSTR "/", IP2STR(&info.ip));
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Configured Wi-Fi SSID: %s", CONFIG_EXAMPLE_WIFI_SSID);
        ESP_LOGI(TAG, "Connecting to configured Wi-Fi network");
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *event = event_data;
        ESP_LOGI(TAG, "Associated with AP channel=%u auth=%u", event->channel, event->authmode);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = event_data;
        ESP_LOGW(TAG, "Wi-Fi disconnect reason=%u", event->reason);
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_http_server) {
            httpd_stop(s_http_server);
            s_http_server = NULL;
        }
        if (s_retry_count < WIFI_RETRY_LIMIT) {
            ++s_retry_count;
            ESP_LOGW(TAG, "Wi-Fi disconnected; retry %u/%u", s_retry_count, WIFI_RETRY_LIMIT);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Wi-Fi retry limit reached");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected; IP=" IPSTR, IP2STR(&event->ip_info.ip));
        start_http_server();
    }
}

void wiimote_web_start(void)
{
#if CONFIG_EXAMPLE_WIFI_WEB_CONTROL
    if (strlen(CONFIG_EXAMPLE_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "Wi-Fi SSID is empty; configure the local sdkconfig");
        return;
    }
    s_wifi_events = xEventGroupCreate();
    s_button_lock = xSemaphoreCreateMutex();
    if (!s_wifi_events || !s_button_lock) {
        ESP_LOGE(TAG, "Failed to allocate Wi-Fi web-control synchronization objects");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_station = esp_netif_create_default_wifi_sta();
    if (!s_station) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi station interface");
        return;
    }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", CONFIG_EXAMPLE_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", CONFIG_EXAMPLE_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_LOGI(TAG, "Connecting to configured Wi-Fi network");
    ESP_ERROR_CHECK(esp_wifi_start());
#endif
}

void wiimote_web_set_hid_connected(bool connected)
{
    s_hid_connected = connected;
    if (!connected && s_button_lock) {
        xSemaphoreTake(s_button_lock, portMAX_DELAY);
        s_button_mask = 0;
        xSemaphoreGive(s_button_lock);
        if (s_button_callback) s_button_callback(0);
    }
}

void wiimote_web_set_button_callback(wiimote_web_button_callback_t callback)
{
    s_button_callback = callback;
}

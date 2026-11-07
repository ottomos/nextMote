#include "nextmote_setup.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nextmote_form.h"
#include "nextmote_wifi_config.h"

#ifndef OFFER_DNS
#define OFFER_DNS 0x02
#endif

#define SETUP_TEST_ATTEMPTS 4
#define SETUP_TEST_TIMEOUT_US (25 * 1000 * 1000)
#define SETUP_RESTART_DELAY_US (10 * 1000 * 1000)
#define SETUP_SCAN_MAX 20
#define SETUP_BODY_MAX 256

typedef enum { TEST_IDLE, TEST_RUNNING, TEST_OK, TEST_FAILED } test_state_t;

static const char *TAG = "nextmote_setup";
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_netif_t *s_ap;
static httpd_handle_t s_server;
static esp_timer_handle_t s_test_timer;
static esp_timer_handle_t s_restart_timer;
static char s_ap_ssid[33];
static test_state_t s_state = TEST_IDLE;
static unsigned s_attempts;
static uint8_t s_reason;
static char s_test_ssid[NEXTMOTE_WIFI_SSID_MAX + 1];
static char s_test_password[NEXTMOTE_WIFI_PASSWORD_MAX + 1];
static char s_ip[16];
static bool s_scanning;

static const char SETUP_HTML[] =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>nextMote Wi-Fi setup</title><style>"
    "*{box-sizing:border-box}body{margin:0;background:#101522;color:#edf2ff;font:16px system-ui,sans-serif}"
    "main{max-width:420px;margin:0 auto;padding:24px 18px}h1{font-size:1.4rem;margin:0 0 4px}"
    "p{color:#aab6d3;margin:6px 0}label{display:block;margin:14px 0 6px;font-weight:600}"
    "select,input,button{width:100%;font:inherit;padding:12px;border-radius:10px;border:1px solid #33405e;"
    "background:#18213a;color:#edf2ff}button{background:#426ec6;border:0;font-weight:700;margin-top:16px}"
    "button.secondary{background:#2a3555}button:disabled{opacity:.5}"
    "#msg{margin-top:16px;padding:12px;border-radius:10px;background:#18213a;min-height:1em}"
    ".ok{color:#7ee2a8}.err{color:#ff8f9c}a{color:#8fb4ff}.row{display:flex;gap:8px;align-items:center}"
    ".row input{width:auto}</style></head><body><main>"
    "<h1>nextMote Wi-Fi setup</h1>"
    "<p>Choose the network nextMote should join. Only 2.4 GHz networks are supported.</p>"
    "<p id='saved'></p>"
    "<form id='f'><label for='net'>Network</label>"
    "<select id='net'><option value=''>Scanning…</option></select>"
    "<button type='button' id='rescan' class='secondary'>Scan again</button>"
    "<label for='ssid'>Network name (SSID)</label><input id='ssid' maxlength='32' autocomplete='off' required>"
    "<label for='pw'>Password</label><input id='pw' type='password' maxlength='64' autocomplete='off'>"
    "<div class='row'><input id='show' type='checkbox'><span>Show password</span></div>"
    "<button id='go'>Connect</button></form>"
    "<button type='button' id='exit' class='secondary' hidden>Keep saved network and restart</button>"
    "<div id='msg'></div></main><script>"
    "const $=s=>document.querySelector(s),msg=$('#msg');"
    "function say(t,c){msg.className=c||'';msg.innerHTML=t}"
    "function esc(s){return s.replace(/[&<>\"']/g,c=>'&#'+c.charCodeAt(0)+';')}"
    "async function scan(){$('#net').innerHTML='<option value=\"\">Scanning…</option>';"
    "try{const r=await fetch('/api/scan');const l=await r.json();"
    "$('#net').innerHTML='<option value=\"\">'+(l.length?'Select a network':'No networks found')+'</option>'+"
    "l.map(n=>'<option value=\"'+esc(n.ssid)+'\">'+esc(n.ssid)+' ('+n.rssi+' dBm'+(n.secure?'':', open')+')</option>').join('')"
    "}catch(e){$('#net').innerHTML='<option value=\"\">Scan failed</option>'}}"
    "$('#net').onchange=e=>{if(e.target.value)$('#ssid').value=e.target.value;$('#pw').focus()};"
    "$('#rescan').onclick=scan;$('#show').onchange=e=>$('#pw').type=e.target.checked?'text':'password';"
    "$('#exit').onclick=async()=>{await fetch('/api/setup/exit',{method:'POST'});"
    "say('Restarting with the saved network…')};"
    "$('#f').onsubmit=async e=>{e.preventDefault();$('#go').disabled=true;"
    "const b=new URLSearchParams({ssid:$('#ssid').value,password:$('#pw').value});"
    "say('Connecting to '+esc($('#ssid').value)+'…');"
    "try{const r=await fetch('/api/wifi',{method:'POST',body:b});if(!r.ok){say(esc(await r.text()),'err');"
    "$('#go').disabled=false;return}}catch(e){}poll(0)};"
    "async function poll(n){let s=null;try{s=await(await fetch('/api/wifi/state')).json()}catch(e){}"
    "if(s&&s.state==='ok'){say('Connected to <b>'+esc(s.ssid)+'</b>. nextMote restarts in a few seconds.<br>"
    "Switch this device back to <b>'+esc(s.ssid)+'</b>, then open <a href=\"http://'+s.ip+'/\">http://'+s.ip+'/</a>','ok');return}"
    "if(s&&s.state==='failed'){say('Could not connect: '+esc(s.error)+'. Check the password and try again.','err');"
    "$('#go').disabled=false;return}"
    "if(n>45){say('No answer. Reconnect to this setup network and reload the page.','err');$('#go').disabled=false;return}"
    "if(!s&&n>3)say('Waiting… if this page stops updating, rejoin the nextMote setup network.');"
    "setTimeout(()=>poll(n+1),1000)}"
    "fetch('/api/wifi/state').then(r=>r.json()).then(s=>{if(s.saved_ssid){"
    "$('#saved').innerHTML='Saved network: <b>'+esc(s.saved_ssid)+'</b>';$('#exit').hidden=false}"
    "if(s.state==='running'||s.state==='ok')poll(0)}).catch(()=>{});scan();"
    "</script></body></html>";

/* Last scan results (httpd handlers run on one task). Used to pin the test
 * connection to a known channel so the setup AP does not hop channels. */
static wifi_ap_record_t s_scan_records[SETUP_SCAN_MAX];
static uint16_t s_scan_count;

static uint8_t scanned_channel(const char *ssid)
{
    uint8_t channel = 0;
    int8_t best = -128;
    for (uint16_t i = 0; i < s_scan_count; ++i) {
        if (strcmp((const char *)s_scan_records[i].ssid, ssid) == 0 && s_scan_records[i].rssi > best) {
            best = s_scan_records[i].rssi;
            channel = s_scan_records[i].primary;
        }
    }
    return channel;
}

static const char *reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND: return "network not found (is it 2.4 GHz and in range?)";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE: return "wrong password";
    case WIFI_REASON_AUTH_EXPIRE: return "wrong password or the router rejected the connection";
    case WIFI_REASON_ASSOC_FAIL: return "the router rejected the connection";
    case WIFI_REASON_CONNECTION_FAIL: return "could not connect (check the password)";
    case 0: return "timed out";
    default: return "connection failed";
    }
}

static void restart_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "Restarting into normal mode");
    esp_restart();
}

static void fail_test_locked(uint8_t reason)
{
    s_state = TEST_FAILED;
    s_reason = reason;
}

static void test_timeout_cb(void *arg)
{
    (void)arg;
    bool failed = false;
    portENTER_CRITICAL(&s_mux);
    if (s_state == TEST_RUNNING) {
        fail_test_locked(s_reason);
        failed = true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (failed) {
        ESP_LOGW(TAG, "Connection test timed out");
        esp_wifi_disconnect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Setup client joined");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "Setup client left");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        bool retry = false;
        bool failed = false;
        portENTER_CRITICAL(&s_mux);
        if (s_state == TEST_RUNNING) {
            s_reason = event->reason;
            if (++s_attempts < SETUP_TEST_ATTEMPTS) {
                retry = true;
            } else {
                fail_test_locked(event->reason);
                failed = true;
            }
        }
        portEXIT_CRITICAL(&s_mux);
        ESP_LOGW(TAG, "Station disconnect reason=%u%s", event->reason,
                 retry ? " (retrying)" : failed ? " (test failed)" : "");
        if (retry) esp_wifi_connect();
        if (failed) esp_timer_stop(s_test_timer);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        char ssid[sizeof(s_test_ssid)];
        char password[sizeof(s_test_password)];
        bool ok = false;
        portENTER_CRITICAL(&s_mux);
        if (s_state == TEST_RUNNING) {
            snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
            memcpy(ssid, s_test_ssid, sizeof(ssid));
            memcpy(password, s_test_password, sizeof(password));
            ok = true;
        }
        portEXIT_CRITICAL(&s_mux);
        if (!ok) return;
        esp_timer_stop(s_test_timer);
        esp_err_t err = nextmote_wifi_config_save(ssid, password);
        memset(password, 0, sizeof(password));
        portENTER_CRITICAL(&s_mux);
        if (err == ESP_OK) {
            s_state = TEST_OK;
        } else {
            fail_test_locked(0);
        }
        portEXIT_CRITICAL(&s_mux);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Connection test OK: \"%s\" IP=" IPSTR "; restarting in %d s", ssid,
                     IP2STR(&event->ip_info.ip), (int)(SETUP_RESTART_DELAY_US / 1000000));
            esp_timer_start_once(s_restart_timer, SETUP_RESTART_DELAY_US);
        } else {
            ESP_LOGE(TAG, "Saving Wi-Fi settings failed: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_handler(httpd_req_t *req)
{
    portENTER_CRITICAL(&s_mux);
    const bool busy = s_state == TEST_RUNNING || s_scanning;
    if (!busy) s_scanning = true;
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Busy");
    }
    wifi_scan_config_t scan = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    uint16_t count = SETUP_SCAN_MAX;
    wifi_ap_record_t *records = s_scan_records;
    if (err == ESP_OK) err = esp_wifi_scan_get_ap_records(&count, records);
    s_scan_count = err == ESP_OK ? count : 0;
    portENTER_CRITICAL(&s_mux);
    s_scanning = false;
    portEXIT_CRITICAL(&s_mux);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr_chunk(req, "[");
    bool first = true;
    for (uint16_t i = 0; i < count; ++i) {
        const char *ssid = (const char *)records[i].ssid;
        if (ssid[0] == '\0') continue;
        bool duplicate = false;
        for (uint16_t j = 0; j < i; ++j) {
            if (strcmp(ssid, (const char *)records[j].ssid) == 0) duplicate = true;
        }
        if (duplicate) continue;
        char escaped[6 * 32 + 1];
        char item[sizeof(escaped) + 64];
        if (!nextmote_json_escape(ssid, escaped, sizeof(escaped))) continue;
        snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s,\"ch\":%u}",
                 first ? "" : ",", escaped, records[i].rssi,
                 records[i].authmode == WIFI_AUTH_OPEN ? "false" : "true", records[i].primary);
        httpd_resp_sendstr_chunk(req, item);
        first = false;
    }
    httpd_resp_sendstr_chunk(req, "]");
    return httpd_resp_sendstr_chunk(req, NULL);
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    char body[SETUP_BODY_MAX + 1];
    if (req->content_len == 0 || req->content_len > SETUP_BODY_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
    }
    int received = 0;
    while (received < (int)req->content_len) {
        const int r = httpd_req_recv(req, body + received, req->content_len - received);
        if (r <= 0) return ESP_FAIL;
        received += r;
    }
    body[received] = '\0';
    char ssid[NEXTMOTE_WIFI_SSID_MAX + 1];
    char password[NEXTMOTE_WIFI_PASSWORD_MAX + 1];
    if (!nextmote_form_field(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Enter a network name (max 32 characters)");
    }
    if (!nextmote_form_field(body, "password", password, sizeof(password))) password[0] = '\0';
    memset(body, 0, sizeof(body));
    const size_t pw_len = strlen(password);
    if (pw_len != 0 && pw_len < 8) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Wi-Fi passwords have at least 8 characters");
    }

    portENTER_CRITICAL(&s_mux);
    const bool busy = s_state == TEST_RUNNING || s_state == TEST_OK || s_scanning;
    if (!busy) {
        s_state = TEST_RUNNING;
        s_attempts = 0;
        s_reason = 0;
        s_ip[0] = '\0';
        memcpy(s_test_ssid, ssid, sizeof(s_test_ssid));
        memcpy(s_test_password, password, sizeof(s_test_password));
    }
    portEXIT_CRITICAL(&s_mux);
    if (busy) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Busy, try again in a moment");
    }

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, pw_len);
    memset(password, 0, sizeof(password));
    config.sta.threshold.authmode = pw_len ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.channel = scanned_channel(ssid);
    config.sta.scan_method = config.sta.channel ? WIFI_FAST_SCAN : WIFI_ALL_CHANNEL_SCAN;
    config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(&config, 0, sizeof(config));
    if (err == ESP_OK) err = esp_wifi_connect();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_mux);
        fail_test_locked(0);
        portEXIT_CRITICAL(&s_mux);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }
    esp_timer_stop(s_test_timer);
    esp_timer_start_once(s_test_timer, SETUP_TEST_TIMEOUT_US);
    ESP_LOGI(TAG, "Testing connection to \"%s\" (channel %u)", ssid, scanned_channel(ssid));
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "Testing");
}

static esp_err_t wifi_state_handler(httpd_req_t *req)
{
    static const char *const names[] = {"idle", "running", "ok", "failed"};
    char ssid[NEXTMOTE_WIFI_SSID_MAX + 1];
    char ip[sizeof(s_ip)];
    portENTER_CRITICAL(&s_mux);
    const test_state_t state = s_state;
    const uint8_t reason = s_reason;
    memcpy(ssid, s_test_ssid, sizeof(ssid));
    memcpy(ip, s_ip, sizeof(ip));
    portEXIT_CRITICAL(&s_mux);

    char saved[NEXTMOTE_WIFI_SSID_MAX + 1];
    char saved_password[NEXTMOTE_WIFI_PASSWORD_MAX + 1];
    if (!nextmote_wifi_config_load(saved, saved_password)) saved[0] = '\0';
    memset(saved_password, 0, sizeof(saved_password));

    char ssid_json[6 * 32 + 1];
    char saved_json[6 * 32 + 1];
    nextmote_json_escape(ssid, ssid_json, sizeof(ssid_json));
    nextmote_json_escape(saved, saved_json, sizeof(saved_json));
    char out[768];
    snprintf(out, sizeof(out),
             "{\"mode\":\"setup\",\"ap\":\"%s\",\"state\":\"%s\",\"ssid\":\"%s\",\"ip\":\"%s\","
             "\"reason\":%u,\"error\":\"%s\",\"saved_ssid\":\"%s\"}",
             s_ap_ssid, names[state], ssid_json, ip, reason,
             state == TEST_FAILED ? reason_text(reason) : "", saved_json);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, out);
}

static esp_err_t exit_handler(httpd_req_t *req)
{
    char ssid[NEXTMOTE_WIFI_SSID_MAX + 1];
    char password[NEXTMOTE_WIFI_PASSWORD_MAX + 1];
    const bool have = nextmote_wifi_config_load(ssid, password);
    memset(password, 0, sizeof(password));
    if (!have) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "No saved network");
    }
    (void)nextmote_wifi_config_clear_setup_request();
    esp_timer_start_once(s_restart_timer, 1000 * 1000);
    httpd_resp_set_status(req, "202 Accepted");
    return httpd_resp_sendstr(req, "Restarting");
}

/* Every unknown URL (OS captive-portal probes included) goes to the setup page. */
static esp_err_t redirect_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static void start_http(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_open_sockets = 7;
    ESP_ERROR_CHECK(httpd_start(&s_server, &config));
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/api/scan", .method = HTTP_GET, .handler = scan_handler},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler},
        {.uri = "/api/wifi/state", .method = HTTP_GET, .handler = wifi_state_handler},
        {.uri = "/api/setup/exit", .method = HTTP_POST, .handler = exit_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uris[i]));
    }
    ESP_ERROR_CHECK(httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND, redirect_handler));
}

/* Captive DNS: answers every A query with the setup address. */
static void dns_task(void *arg)
{
    const uint32_t ip = (uint32_t)(uintptr_t)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(53),
                               .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (sock < 0 || bind(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Captive DNS socket failed");
        if (sock >= 0) close(sock);
        vTaskDelete(NULL);
        return;
    }
    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
        if (len < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] == 0) continue;
        size_t pos = 12;
        while (pos < (size_t)len && buf[pos] != 0 && (buf[pos] & 0xc0) == 0) pos += buf[pos] + 1u;
        if (pos >= (size_t)len || buf[pos] != 0 || pos + 5 > (size_t)len) continue;
        pos += 1;
        const uint16_t qtype = (uint16_t)((buf[pos] << 8) | buf[pos + 1]);
        pos += 4;
        buf[2] = 0x84 | (buf[2] & 0x01); /* response, authoritative, keep RD */
        buf[3] = 0x80;                   /* recursion available, no error */
        buf[4] = 0; buf[5] = 1;
        buf[6] = 0; buf[7] = 0;
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        if (qtype == 1 && pos + 16 <= sizeof(buf)) {
            const uint8_t answer[16] = {0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0, 0, 0, 60, 0, 4,
                                        (uint8_t)ip, (uint8_t)(ip >> 8), (uint8_t)(ip >> 16),
                                        (uint8_t)(ip >> 24)};
            memcpy(&buf[pos], answer, sizeof(answer));
            pos += sizeof(answer);
            buf[7] = 1;
        }
        sendto(sock, buf, pos, 0, (struct sockaddr *)&from, from_len);
    }
}

/* Puts the setup network on the strongest nearby channel, so testing the most
 * likely home network does not force a channel switch that drops the phone. */
static uint8_t pick_ap_channel(void)
{
    wifi_scan_config_t scan = {.show_hidden = false};
    uint16_t count = 1;
    wifi_ap_record_t best;
    if (esp_wifi_scan_start(&scan, true) == ESP_OK &&
        esp_wifi_scan_get_ap_records(&count, &best) == ESP_OK && count == 1 &&
        best.primary >= 1 && best.primary <= 13) {
        ESP_LOGI(TAG, "Strongest network on channel %u", best.primary);
        return best.primary;
    }
    esp_wifi_clear_ap_list();
    return 1;
}

void nextmote_setup_run(void)
{
    ESP_LOGW(TAG, "SETUP MODE: Bluetooth and sensors are off");
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap = esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    const esp_timer_create_args_t test_args = {.callback = test_timeout_cb, .name = "setup_test"};
    const esp_timer_create_args_t restart_args = {.callback = restart_cb, .name = "setup_restart"};
    ESP_ERROR_CHECK(esp_timer_create(&test_args, &s_test_timer));
    ESP_ERROR_CHECK(esp_timer_create(&restart_args, &s_restart_timer));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    const uint8_t channel = pick_ap_channel();

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "nextMote-Setup-%02X%02X", mac[4], mac[5]);
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, s_ap_ssid, strlen(s_ap_ssid));
    ap.ap.ssid_len = (uint8_t)strlen(s_ap_ssid);
    ap.ap.channel = channel;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));

    esp_netif_ip_info_t info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_ap, &info));
    esp_netif_dns_info_t dns = {0};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = info.ip.addr;
    uint8_t offer = OFFER_DNS;
    esp_netif_dhcps_stop(s_ap);
    ESP_ERROR_CHECK(esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                           &offer, sizeof(offer)));
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_ap, ESP_NETIF_DNS_MAIN, &dns));
    ESP_ERROR_CHECK(esp_netif_dhcps_start(s_ap));

    start_http();
    xTaskCreate(dns_task, "captive_dns", 3072, (void *)(uintptr_t)info.ip.addr, 5, NULL);
    ESP_LOGW(TAG, "Join Wi-Fi \"%s\" (open) and open http://" IPSTR "/", s_ap_ssid, IP2STR(&info.ip));
}

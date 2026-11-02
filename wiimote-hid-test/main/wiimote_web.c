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
#include "imu_service.h"
#if CONFIG_EXAMPLE_BT_DISCOVERY_TRACE
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "wiimote_bond_json.h"
#include "wiimote_bt_trace.h"
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_RETRY_LIMIT 10
#define MAX_HTTP_BODY 96
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
#define PAIRING_PROFILE "upstream-general-continuous"
#else
#define PAIRING_PROFILE "timed"
#endif

static const char *TAG = "wiimote_web";
static EventGroupHandle_t s_wifi_events;
static SemaphoreHandle_t s_button_lock;
static uint16_t s_button_mask;
static portMUX_TYPE s_camera_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_sync_status_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_link_status_mux = portMUX_INITIALIZER_UNLOCKED;
static ir_camera_state_t s_camera_state;
static wiimote_web_button_callback_t s_button_callback;
static wiimote_web_sync_callback_t s_sync_callback;
static wiimote_web_virtual_cable_unplug_callback_t s_virtual_cable_unplug_callback;
static wiimote_web_sync_rearm_callback_t s_sync_rearm_callback;
static esp_netif_t *s_station;
static httpd_handle_t s_http_server;
static unsigned s_retry_count;
static bool s_hid_connected;
static bool s_sync_window_active;
static char s_last_host_address[18];
static bool s_reconnect_pending;

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
    "#button-status{text-align:center;color:#8fa0bf;min-height:1.5em;margin:-16px 0 14px}"
    ".remote{background:#e8eaf0;color:#172033;border-radius:48px;padding:24px;"
    "box-shadow:0 18px 45px #0008;display:grid;grid-template-columns:1fr 1fr 1fr;"
    "grid-template-rows:repeat(7,56px);gap:12px;max-width:620px;margin:auto}"
    "button{border:0;border-radius:18px;background:#263650;color:white;font-weight:700;"
    "font-size:18px;touch-action:none;user-select:none;-webkit-user-select:none;"
    "box-shadow:0 5px 0 #111a;cursor:pointer}button:active,button.pressed{"
    "transform:translateY(4px);box-shadow:0 1px 0 #111a;background:#13a89e}"
    ".dpad{grid-column:1;grid-row:2/5;display:grid;grid-template:repeat(3,1fr)/repeat(3,1fr);gap:5px}"
    ".dpad button{font-size:20px;border-radius:12px;background:#40516d}"
    ".up{grid-area:1/2}.left{grid-area:2/1}.right{grid-area:2/3}.dpad .down{grid-area:3/2}"
    ".ab{grid-column:3;grid-row:2/5;display:flex;flex-direction:column;"
    "align-items:center;justify-content:space-around} .ab button{width:74px;height:74px;"
    "border-radius:50%;font-size:24px;background:#c74455}.ab button:first-child{"
    "background:#426ec6;width:88px;height:88px}.sys{grid-row:5;align-self:center;"
    "height:50px;background:#66728a}.sys.one{grid-column:1}.sys.two{grid-column:3}"
    ".sys.home{grid-column:2;background:#d09b43}.combo-row{grid-column:1/4;grid-row:7;"
    "display:grid;grid-template-columns:1fr 1fr;gap:10px}.combo{height:48px;background:#506a88}"
    ".pairing-panel{margin:18px auto;padding:18px;background:#161e2d;border:1px solid #35415a;"
    "border-radius:22px;text-align:center}.pairing-panel h2{margin:0}.pairing-panel p{"
    "color:#aab7cf;line-height:1.5}.sync-button{width:min(360px,100%);height:56px;"
    "background:#c74455}.sync-button:disabled{opacity:.55;cursor:not-allowed}"
    "@media(max-width:460px){.remote{padding:18px;gap:8px;grid-template-rows:repeat(7,56px)}"
    ".ab button{width:62px;"
    "height:62px}.ab button:first-child{width:76px;height:76px}}"
    ".camera-panel{margin:18px auto;padding:18px;background:#161e2d;border:1px solid #35415a;"
    "border-radius:22px}.camera-panel h2{text-align:center;margin:0}"
    "#camera-status{text-align:center;color:#8fa0bf;margin:8px 0;min-height:1.5em}"
    "#camera-tracker{width:min(640px,92vw);aspect-ratio:4/3;border:2px solid #62708a;"
    "margin:12px auto;position:relative;background:linear-gradient(90deg,rgba(255,255,255,.07) 1px,"
    "transparent 1px) 0 0/10% 100%,linear-gradient(rgba(255,255,255,.07) 1px,transparent 1px)"
    " 0 0/100% 10%,#111827;overflow:hidden;border-radius:12px}"
    ".camera-dot{position:absolute;width:14px;height:14px;border-radius:50%;"
    "transform:translate(-50%,-50%);display:grid;place-items:center;color:#fff;"
    "font-size:10px;font-weight:bold;box-shadow:0 0 0 2px #0008}"
    ".camera-dot.off{display:none}.camera-values{width:min(420px,100%);margin:12px auto;"
    "border-collapse:collapse}.camera-values th,.camera-values td{border:1px solid #46516a;"
    "padding:8px;text-align:center}"
    ".imu-panel{margin:18px auto;padding:18px;background:#161e2d;border:1px solid #35415a;"
    "border-radius:22px}.imu-panel h2{text-align:center;margin:0}"
    "#imu-status{text-align:center;color:#8fa0bf;margin:8px 0;min-height:1.5em}"
    ".imu-layout{display:flex;gap:24px;align-items:center;justify-content:center;flex-wrap:wrap}"
    ".imu-values{min-width:180px;line-height:2}"
    ".imu-cube-wrap{perspective:500px}"
    "#imu-cube{width:72px;height:72px;border:2px solid #172033;"
    "background:linear-gradient(145deg,#f29900,#f8c96b);transform-style:preserve-3d;"
    "transition:transform 90ms linear}"
    "</style></head><body><main class='wrap'><h1>ESPmote</h1>"
    "<div id='status'>Connecting to ESP32…</div><div id='button-status' aria-live='polite'></div>"
    "<section class='pairing-panel' aria-label='Bluetooth pairing'>"
    "<button id='sync-button' type='button' class='sync-button'>Pair</button>"
    "</section>"
    "<section class='remote' aria-label='Wii Remote'>"
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
    "</section><section class='camera-panel' aria-label='IR camera view'>"
    "<h2>IR camera · SEN0158</h2><p id='camera-status'>Waiting for camera data…</p>"
    "<div id='camera-tracker' role='img' aria-label='Live IR point plot'></div>"
    "<table class='camera-values'><thead><tr><th>Point</th><th>X</th><th>Y</th></tr></thead>"
    "<tbody><tr><td>P0</td><td id='x0'>-</td><td id='y0'>-</td></tr>"
    "<tr><td>P1</td><td id='x1'>-</td><td id='y1'>-</td></tr>"
    "<tr><td>P2</td><td id='x2'>-</td><td id='y2'>-</td></tr>"
    "<tr><td>P3</td><td id='x3'>-</td><td id='y3'>-</td></tr></tbody></table></section>"
    "<section class='imu-panel' aria-label='IMU orientation'>"
    "<h2>BNO08x orientation</h2><p id='imu-status'>Waiting for IMU data…</p>"
    "<div class='imu-layout'><div class='imu-values'>"
    "<div>Roll: <span id='imu-roll'>-</span>°</div>"
    "<div>Pitch: <span id='imu-pitch'>-</span>°</div>"
    "<div>Yaw: <span id='imu-yaw'>-</span>°</div>"
    "</div><div class='imu-cube-wrap'><div id='imu-cube' aria-label='IMU orientation box'></div>"
    "</div></div></section></main><script>"
    "const statusEl=document.querySelector('#status');const buttonStatusEl=document.querySelector('#button-status');"
    "const syncButton=document.querySelector('#sync-button');"
    "let syncRequestPending=false,pairingPending=false,pairingSawDisconnect=false,syncCountdownTimer=null;"
    "let held=new Set(),queue=Promise.resolve();"
    "const cameraStatusEl=document.querySelector('#camera-status');"
    "const cameraTracker=document.querySelector('#camera-tracker');const cameraDots=[];"
    "const imuStatusEl=document.querySelector('#imu-status');const imuCube=document.querySelector('#imu-cube');"
    "const cameraColors=['#ea4335','#1a73e8','#188038','#f29900'];"
    "for(let i=0;i<4;i++){const dot=document.createElement('div');dot.className='camera-dot off';"
    "dot.style.background=cameraColors[i];dot.textContent=i;cameraTracker.appendChild(dot);cameraDots.push(dot)}"
    "function send(name,pressed){queue=queue.then(async()=>{try{let r=await fetch('/api/button',{method:'POST',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'name='+encodeURIComponent(name)+'&pressed='+(pressed?'1':'0')});"
    "if(!r.ok)throw Error(await r.text());buttonStatusEl.textContent="
    "(pressed?'Pressed ':'Released ')+name}catch(e){buttonStatusEl.textContent='Button error: '+e.message}})}"
    "function press(b){if(held.has(b))return;held.add(b);b.classList.add('pressed');"
    "send(b.dataset.button,true)}function release(b){if(!held.has(b))return;"
    "held.delete(b);b.classList.remove('pressed');send(b.dataset.button,false)}"
    "document.querySelectorAll('[data-button]').forEach(b=>{b.addEventListener('pointerdown',e=>{"
    "e.preventDefault();b.setPointerCapture(e.pointerId);press(b)});"
    "for(const ev of ['pointerup','pointercancel','lostpointercapture'])b.addEventListener(ev,()=>release(b));"
    "b.addEventListener('contextmenu',e=>e.preventDefault())});"
    "window.addEventListener('blur',()=>{for(const b of document.querySelectorAll('.pressed'))release(b)});"
    "document.addEventListener('visibilitychange',()=>{if(document.hidden){for(const b of document.querySelectorAll('.pressed'))release(b)}});"
    "async function requestSync(){if(syncRequestPending||pairingPending)return;"
    "syncRequestPending=true;syncButton.disabled=true;buttonStatusEl.textContent='';"
    "try{const r=await fetch('/api/sync',{method:'POST'});"
    "if(!r.ok)throw Error(await r.text());const d=await r.json();"
    "if(d.pairing_profile==='upstream-general-continuous'){"
    "syncButton.textContent='Pair';return;}"
    "pairingPending=true;pairingSawDisconnect=statusEl.dataset.connected!=='1';"
    "const expires=Date.now()+d.window_seconds*1000;"
    "if(syncCountdownTimer)clearInterval(syncCountdownTimer);"
    "const refreshSync=()=>{const left=Math.max(0,Math.ceil((expires-Date.now())/1000));"
    "syncButton.textContent=left?'Pair · '+left+'s':'Pair';"
    "if(!left){clearInterval(syncCountdownTimer);syncCountdownTimer=null;"
    "pairingPending=false;pairingSawDisconnect=false;syncButton.disabled=false}};"
    "refreshSync();syncCountdownTimer=setInterval(refreshSync,250)}"
    "catch(e){buttonStatusEl.textContent='Pair failed: '+e.message;syncButton.textContent='Pair'}"
    "finally{syncRequestPending=false;syncButton.disabled=pairingPending}}"
    "syncButton.addEventListener('click',requestSync);"
    "function updateStatus(s){const host=s.last_host?' '+s.last_host:'';"
    "const link=s.connected?'HID connected'+(s.last_host?' · '+s.last_host:''):"
    "s.sync_window?'Pairing window open':"
    "s.reconnecting?'Trying saved host'+host:"
    "s.last_host?'Disconnected · saved host'+host:'Bluetooth ready for Pair';"
    "statusEl.textContent=s.wifi?'Wi-Fi connected · '+s.ip+' · '+link:'Wi-Fi not connected yet';"
    "statusEl.dataset.connected=s.connected?'1':'0';"
    "syncButton.disabled=syncRequestPending||pairingPending;"
    "if(pairingPending&&!s.connected)pairingSawDisconnect=true;"
    "if(pairingPending&&pairingSawDisconnect&&s.connected){"
    "if(syncCountdownTimer){clearInterval(syncCountdownTimer);syncCountdownTimer=null}"
    "pairingPending=false;pairingSawDisconnect=false;syncButton.textContent='Pair';syncButton.disabled=false}"
    "if(!s.connected)for(const b of document.querySelectorAll('.pressed'))release(b)}"
    "async function refreshStatus(){try{const r=await fetch('/api/status',{cache:'no-store'});"
    "if(!r.ok)throw Error('HTTP '+r.status);updateStatus(await r.json())}"
    "catch(e){statusEl.textContent='ESP32 status unavailable';statusEl.dataset.connected='0'}}"
    "refreshStatus();setInterval(refreshStatus,1000);"
    "async function updateCamera(){try{const r=await fetch('/api/camera',{cache:'no-store'});"
    "if(!r.ok)throw Error('HTTP '+r.status);const d=await r.json();"
    "cameraStatusEl.textContent=!d.detected?'Camera not detected · check wiring':"
    "!d.available?'Camera detected · waiting for a frame':'Live camera frame';"
    "for(let i=0;i<4;i++){const p=d.points[i];"
    "document.getElementById('x'+i).textContent=d.available?p.x:'-';"
    "document.getElementById('y'+i).textContent=d.available?p.y:'-';"
    "if(!d.available||p.x>=1023||p.y>=1023){cameraDots[i].classList.add('off');continue}"
    "cameraDots[i].style.left=(p.x/1023*cameraTracker.clientWidth)+'px';"
    "cameraDots[i].style.top=(p.y/1023*cameraTracker.clientHeight)+'px';"
    "cameraDots[i].classList.remove('off')}}catch(e){cameraStatusEl.textContent='Camera data request failed'}}"
    "async function updateImu(){try{const r=await fetch('/api/imu',{cache:'no-store'});"
    "if(!r.ok)throw Error('HTTP '+r.status);const d=await r.json();"
    "imuStatusEl.textContent=!d.detected?'BNO08x not detected · check wiring':"
    "!d.available?'BNO08x found at 0x'+d.address.toString(16).padStart(2,'0')+' · waiting for orientation':"
    "'Live orientation · 0x'+d.address.toString(16).padStart(2,'0');"
    "if(d.available){const roll=d.roll,pitch=d.pitch,yaw=d.yaw;"
    "document.querySelector('#imu-roll').textContent=roll.toFixed(1);"
    "document.querySelector('#imu-pitch').textContent=pitch.toFixed(1);"
    "document.querySelector('#imu-yaw').textContent=yaw.toFixed(1);"
    "imuCube.style.transform=`rotateX(${pitch}deg) rotateY(${roll}deg) rotateZ(${yaw}deg)`}"
    "else{document.querySelector('#imu-roll').textContent='-';"
    "document.querySelector('#imu-pitch').textContent='-';"
    "document.querySelector('#imu-yaw').textContent='-';imuCube.style.transform='none'}}"
    "catch(e){imuStatusEl.textContent='IMU data request failed'}}"
    "updateCamera();setInterval(updateCamera,120);updateImu();setInterval(updateImu,120);"
    "</script></body></html>";

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

static bool hid_connected_snapshot(void)
{
    if (!s_button_lock) return false;

    xSemaphoreTake(s_button_lock, portMAX_DELAY);
    bool connected = s_hid_connected;
    xSemaphoreGive(s_button_lock);
    return connected;
}

static bool sync_window_snapshot(void)
{
    portENTER_CRITICAL(&s_sync_status_mux);
    bool active = s_sync_window_active;
    portEXIT_CRITICAL(&s_sync_status_mux);
    return active;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char ip[16] = "0.0.0.0";
    char last_host[sizeof(s_last_host_address)];
    bool reconnect_pending;
    portENTER_CRITICAL(&s_link_status_mux);
    memcpy(last_host, s_last_host_address, sizeof(last_host));
    reconnect_pending = s_reconnect_pending;
    portEXIT_CRITICAL(&s_link_status_mux);

    esp_netif_ip_info_t info = {0};
    if (s_station && esp_netif_get_ip_info(s_station, &info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }
    char response[256];
    bool hid_connected = hid_connected_snapshot();
    bool sync_window_active = sync_window_snapshot();
    int length = snprintf(response, sizeof(response),
             "{\"wifi\":%s,\"connected\":%s,\"sync_window\":%s,\"ip\":\"%s\",\"pairing_profile\":\"%s\",\"last_host\":\"%s\",\"reconnecting\":%s}",
             (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) ? "true" : "false",
             hid_connected ? "true" : "false",
             sync_window_active ? "true" : "false", ip, PAIRING_PROFILE, last_host,
             reconnect_pending ? "true" : "false");
    if (length < 0 || (size_t)length >= sizeof(response)) {
        ESP_LOGE(TAG, "Status response formatting failed");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Status response formatting failed");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t camera_handler(httpd_req_t *req)
{
    ir_camera_state_t state;
    portENTER_CRITICAL(&s_camera_mux);
    state = s_camera_state;
    portEXIT_CRITICAL(&s_camera_mux);

    char response[256];
    int length = snprintf(response, sizeof(response),
                         "{\"detected\":%s,\"available\":%s,\"points\":["
                         "{\"x\":%u,\"y\":%u},{\"x\":%u,\"y\":%u},"
                         "{\"x\":%u,\"y\":%u},{\"x\":%u,\"y\":%u}]}",
                         state.detected ? "true" : "false",
                         state.available ? "true" : "false",
                         (unsigned)state.x[0], (unsigned)state.y[0],
                         (unsigned)state.x[1], (unsigned)state.y[1],
                         (unsigned)state.x[2], (unsigned)state.y[2],
                         (unsigned)state.x[3], (unsigned)state.y[3]);
    if (length < 0 || (size_t)length >= sizeof(response)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera response formatting failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, response, (ssize_t)length);
}

static esp_err_t imu_handler(httpd_req_t *req)
{
    imu_state_t state;
    imu_service_get_state(&state);

    char response[192];
    int length = snprintf(response, sizeof(response),
                          "{\"detected\":%s,\"available\":%s,\"address\":%u,"
                          "\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f}",
                          state.detected ? "true" : "false",
                          state.available ? "true" : "false",
                          (unsigned)state.address,
                          (double)state.roll_deg,
                          (double)state.pitch_deg,
                          (double)state.yaw_deg);
    if (length < 0 || (size_t)length >= sizeof(response)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "IMU response formatting failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, response, (ssize_t)length);
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
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Bluetooth HID is not connected");
    }
    bool pressed = pressed_text[0] == '1';
    if (pressed) s_button_mask |= mask;
    else s_button_mask &= (uint16_t)~mask;
    uint16_t new_state = s_button_mask;
    xSemaphoreGive(s_button_lock);

    if (!s_button_callback) {
        ESP_LOGE(TAG, "Web button callback is unavailable");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "HID button callback is unavailable");
    }
    esp_err_t send_err = s_button_callback(new_state);
    if (send_err != ESP_OK) {
        ESP_LOGE(TAG, "HID button report was not queued: %s", esp_err_to_name(send_err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, esp_err_to_name(send_err));
    }
    ESP_LOGI(TAG, "%s %s (mask=0x%04x state=0x%04x)", pressed ? "Press" : "Release", name, mask, new_state);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t sync_handler(httpd_req_t *req, bool general_discoverable)
{
#if WIIMOTE_UPSTREAM_DISCOVERY_BASELINE
    general_discoverable = true;
#endif
    if (req->content_len != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SYNC request must not have a body");
        return ESP_FAIL;
    }
    if (!s_sync_callback) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Bluetooth SYNC callback is unavailable");
    }

    esp_err_t err = s_sync_callback(general_discoverable);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, "Bluetooth is connected or not ready for pairing");
        }
        ESP_LOGE(TAG, "Failed to open Bluetooth SYNC window: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }

    char response[256];
    int length = snprintf(response, sizeof(response),
                          "{\"ok\":true,\"window_seconds\":%u,\"discovery\":\"%s\",\"diagnostic_giac_only\":%s,\"pairing_profile\":\"%s\"}",
                          WIIMOTE_UPSTREAM_DISCOVERY_BASELINE ? 0U : (unsigned)WIIMOTE_SYNC_WINDOW_SECONDS,
                          general_discoverable ? "general" : "limited",
                          WIIMOTE_DIAGNOSTIC_GIAC_ONLY ? "true" : "false", PAIRING_PROFILE);
    if (length < 0 || (size_t)length >= sizeof(response)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "SYNC response formatting failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, response, (ssize_t)length);
}

static esp_err_t sync_general_handler(httpd_req_t *req)
{
    return sync_handler(req, true);
}

static esp_err_t sync_limited_handler(httpd_req_t *req)
{
    return sync_handler(req, false);
}

#if CONFIG_EXAMPLE_BT_DISCOVERY_TRACE
static esp_err_t controller_readback_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Controller readback request must not have a body");
    }
    esp_err_t err = wiimote_bt_trace_read_controller();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Controller readback queueing failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"queued\":true,\"results\":\"BT_HCI_RX serial logs\"}");
}

static esp_err_t bond_snapshot_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Bond snapshot request must not have a body");
    }
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGW(TAG, "Bond snapshot requested before Bluetooth is ready");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Bluetooth is not ready");
    }
    int number = esp_bt_gap_get_bond_device_num();
    if (number < 0 || esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGE(TAG, "Could not read bond count");
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Bond count is unavailable");
    }
    if (number == 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_sendstr(req, "{\"bonded_count\":0,\"peers\":[]}");
    }
    if ((size_t)number > (SIZE_MAX - 64U) / 20U) {
        ESP_LOGE(TAG, "Bond count is too large to encode");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Bond count is too large");
    }

    size_t capacity = 64U + (size_t)number * 20U;
    esp_bd_addr_t *peers = calloc((size_t)number, sizeof(esp_bd_addr_t));
    char *body = malloc(capacity);
    esp_err_t result;
    if (!peers || !body) {
        ESP_LOGE(TAG, "Could not allocate bond snapshot");
        result = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                     "Bond snapshot allocation failed");
        goto cleanup;
    }
    int actual = number;
    esp_err_t err = esp_bt_gap_get_bond_device_list(&actual, peers);
    if (err != ESP_OK || actual != number ||
        esp_bt_gap_get_bond_device_num() != number ||
        esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED) {
        ESP_LOGW(TAG, "Bond snapshot unavailable or changed during read: %s",
                 esp_err_to_name(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        result = httpd_resp_sendstr(req, "Bond snapshot changed or is unavailable; retry");
        goto cleanup;
    }

    int length = wiimote_bond_json(peers, (size_t)actual, body, capacity);
    if (length < 0) goto format_error;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    result = httpd_resp_send(req, body, (ssize_t)length);
    goto cleanup;

format_error:
    ESP_LOGE(TAG, "Bond snapshot response formatting failed");
    result = httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                 "Bond snapshot formatting failed");
cleanup:
    free(peers);
    free(body);
    return result;
}

static esp_err_t virtual_cable_unplug_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Virtual-cable unplug request must not have a body");
    }
    if (!s_virtual_cable_unplug_callback) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Virtual-cable unplug callback is unavailable");
    }

    esp_err_t err = s_virtual_cable_unplug_callback();
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Bluetooth is busy or not ready for virtual-cable unplug");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "No saved bonded HID host is available to unplug");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Virtual-cable unplug request failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"queued\":true}");
}
#endif

static esp_err_t sync_rearm_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "SYNC rearm request must not have a body");
    }
    if (!s_sync_rearm_callback) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "SYNC rearm callback is unavailable");
    }

    esp_err_t err = s_sync_rearm_callback();
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Bluetooth is not ready to disconnect and reopen SYNC");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SYNC rearm request failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"queued\":true,\"action\":\"disconnect_then_sync\"}");
}

static const httpd_uri_t s_index_uri = {
    .uri = "/", .method = HTTP_GET, .handler = index_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_status_uri = {
    .uri = "/api/status", .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_camera_uri = {
    .uri = "/api/camera", .method = HTTP_GET, .handler = camera_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_imu_uri = {
    .uri = "/api/imu", .method = HTTP_GET, .handler = imu_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_button_uri = {
    .uri = "/api/button", .method = HTTP_POST, .handler = button_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_sync_uri = {
    .uri = "/api/sync", .method = HTTP_POST, .handler = sync_limited_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_sync_limited_uri = {
    .uri = "/api/sync/limited", .method = HTTP_POST, .handler = sync_limited_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_sync_general_uri = {
    .uri = "/api/sync/general", .method = HTTP_POST, .handler = sync_general_handler, .user_ctx = NULL,
};
#if CONFIG_EXAMPLE_BT_DISCOVERY_TRACE
static const httpd_uri_t s_controller_readback_uri = {
    .uri = "/api/bt/controller", .method = HTTP_GET, .handler = controller_readback_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_bond_snapshot_uri = {
    .uri = "/api/bt/bonds", .method = HTTP_GET, .handler = bond_snapshot_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_virtual_cable_unplug_uri = {
    .uri = "/api/bt/virtual-cable-unplug", .method = HTTP_POST,
    .handler = virtual_cable_unplug_handler, .user_ctx = NULL,
};
#endif
static const httpd_uri_t s_sync_rearm_uri = {
    .uri = "/api/bt/rearm-sync", .method = HTTP_POST,
    .handler = sync_rearm_handler, .user_ctx = NULL,
};

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    esp_err_t err = httpd_start(&s_http_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_camera_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_imu_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_button_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_sync_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_sync_limited_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_sync_general_uri));
#if CONFIG_EXAMPLE_BT_DISCOVERY_TRACE
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_controller_readback_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_bond_snapshot_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_virtual_cable_unplug_uri));
#endif
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_sync_rearm_uri));

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
    bool changed;
    if (s_button_lock) {
        xSemaphoreTake(s_button_lock, portMAX_DELAY);
        changed = s_hid_connected != connected;
        s_hid_connected = connected;
        if (!connected) s_button_mask = 0;
        xSemaphoreGive(s_button_lock);
    } else {
        changed = s_hid_connected != connected;
        s_hid_connected = connected;
    }

    if (changed) ESP_LOGI(TAG, "Bluetooth HID %s", connected ? "connected" : "disconnected");
    if (connected) wiimote_web_set_sync_window(false);
    if (!connected && changed && s_button_callback) s_button_callback(0);
}

void wiimote_web_set_last_host(const uint8_t address[6])
{
    char formatted[sizeof(s_last_host_address)] = {0};
    if (address != NULL) {
        int length = snprintf(formatted, sizeof(formatted),
                              "%02x:%02x:%02x:%02x:%02x:%02x",
                              (unsigned)address[0], (unsigned)address[1],
                              (unsigned)address[2], (unsigned)address[3],
                              (unsigned)address[4], (unsigned)address[5]);
        if (length < 0 || (size_t)length >= sizeof(formatted)) {
            ESP_LOGE(TAG, "Last-host address formatting failed");
            return;
        }
    }

    portENTER_CRITICAL(&s_link_status_mux);
    memcpy(s_last_host_address, formatted, sizeof(s_last_host_address));
    portEXIT_CRITICAL(&s_link_status_mux);
}

void wiimote_web_set_reconnect_pending(bool pending)
{
    portENTER_CRITICAL(&s_link_status_mux);
    s_reconnect_pending = pending;
    portEXIT_CRITICAL(&s_link_status_mux);
}

void wiimote_web_set_sync_callback(wiimote_web_sync_callback_t callback)
{
    s_sync_callback = callback;
}

void wiimote_web_set_virtual_cable_unplug_callback(
    wiimote_web_virtual_cable_unplug_callback_t callback)
{
    s_virtual_cable_unplug_callback = callback;
}

void wiimote_web_set_sync_rearm_callback(wiimote_web_sync_rearm_callback_t callback)
{
    s_sync_rearm_callback = callback;
}

void wiimote_web_set_sync_window(bool active)
{
    portENTER_CRITICAL(&s_sync_status_mux);
    s_sync_window_active = active;
    portEXIT_CRITICAL(&s_sync_status_mux);
}

void wiimote_web_set_button_callback(wiimote_web_button_callback_t callback)
{
    s_button_callback = callback;
}

void wiimote_web_set_camera_state(const ir_camera_state_t *state)
{
    if (state == NULL) {
        ESP_LOGE(TAG, "Cannot publish a null IR camera state");
        return;
    }

    portENTER_CRITICAL(&s_camera_mux);
    s_camera_state = *state;
    portEXIT_CRITICAL(&s_camera_mux);
}

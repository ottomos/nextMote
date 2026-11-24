#include "wiimote_web.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "imu_service.h"
#include "nextmote_form.h"
#include "nextmote_wifi_config.h"
#include "sensor_i2c_bus.h"
#include "esp_timer.h"
#include "wiimote_accel.h"
#include "wiimote_state.h"
#include "wiimote_physical_buttons.h"
#include "wiimote_motion_plus.h"
#include "wiimote_nunchuk.h"
#include "wiimote_reports.h"
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
static wiimote_web_reconnect_callback_t s_reconnect_callback;
static wiimote_web_virtual_cable_unplug_callback_t s_virtual_cable_unplug_callback;
static wiimote_web_sync_rearm_callback_t s_sync_rearm_callback;
static esp_netif_t *s_station;
static char s_wifi_ssid[NEXTMOTE_WIFI_SSID_MAX + 1];
static httpd_handle_t s_http_server;
static unsigned s_retry_count;
static bool s_hid_connected;
static bool s_sync_window_active;
static char s_last_host_address[18];
static bool s_reconnect_pending;
static bool s_manual_reconnect_pending;
static char s_pairing_phase[24] = "idle";

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
    "<title>nextMote Controller</title><style>"
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
    "color:#aab7cf;line-height:1.5}.pair-actions{display:flex;gap:12px;width:min(460px,100%);"
    "margin:auto}.pair-action{flex:1;min-width:0;min-height:64px;padding:8px 10px;"
    "display:flex;flex-direction:column;align-items:center;justify-content:center;gap:2px;"
    "line-height:1.15}.pair-action small{font-size:12px;font-weight:500;opacity:.82}"
    ".sync-button{background:#c74455}.reconnect-button{background:#426ec6}"
    ".pair-action:disabled{opacity:.55;cursor:not-allowed}"
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
".nunchuk-panel{margin:18px auto;padding:18px;background:#161e2d;border:1px solid #35415a;"
    "border-radius:22px;text-align:center}.nunchuk-panel h2{margin:0}"
    "#nc-status{color:#8fa0bf;margin:8px 0;min-height:1.5em}"
    "#nc-toggle{min-height:52px;padding:0 22px;background:#66728a}#nc-toggle.on{background:#13a89e}"
    ".nc-layout{display:flex;gap:28px;align-items:center;justify-content:center;flex-wrap:wrap;margin-top:14px}"
    "#nc-pad{width:170px;height:170px;border-radius:50%;background:#263650;position:relative;"
    "touch-action:none;border:2px solid #46516a}#nc-knob{width:54px;height:54px;border-radius:50%;"
    "background:#d4d8e2;position:absolute;left:50%;top:50%;transform:translate(-50%,-50%);pointer-events:none}"
    ".nc-buttons{display:flex;flex-direction:column;gap:14px}.nc-buttons button{width:72px;height:56px}"
    ".nunchuk-panel.off .nc-layout{opacity:.45}"
    ".wifi-setup{margin:22px 0 8px;text-align:center}.wifi-setup button{background:#2a3555;color:#edf2ff;"
    "border:0;border-radius:10px;padding:10px 18px;font:inherit}.wifi-setup small{display:block;margin-top:8px;color:#aab6d3}"
    "</style></head><body><main class='wrap'><h1>nextMote</h1>"
    "<div id='status'>Loading controller status…</div><div id='button-status' aria-live='polite'></div>"
    "<div id='physical-button-status' aria-live='polite' style='text-align:center;margin-bottom:12px'>PHY: loading</div>"
    "<section class='pairing-panel' aria-label='Bluetooth host actions'>"
    "<div class='pair-actions'>"
    "<button id='reconnect-button' type='button' class='pair-action reconnect-button'>"
    "<span class='action-label'>Reconnect</span><small>last host</small></button>"
    "<button id='sync-button' type='button' class='pair-action sync-button'>"
    "<span class='action-label'>Pair</span><small>next host</small></button>"
    "</div>"
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
    "</section>"
    "<section class='nunchuk-panel off' id='nc-panel' aria-label='Web Nunchuk'>"
    "<h2>Nunchuk · web</h2><p id='nc-status'>Loading Nunchuk state…</p>"
    "<button id='nc-toggle' type='button'>Connect Nunchuk</button>"
    "<div class='nc-layout'><div id='nc-pad' role='slider' aria-label='Nunchuk stick'>"
    "<div id='nc-knob'></div></div><div class='nc-buttons'>"
    "<button data-nc='c'>C</button><button data-nc='z'>Z</button></div></div></section>"
    "<section class='camera-panel' aria-label='IR camera view'>"
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
    "<div id='imu-accel'>Waiting for accelerometer data</div>"
    "</div><div class='imu-cube-wrap'><div id='imu-cube' aria-label='IMU orientation box'></div>"
    "</div></div></section>"
    "<section class='wifi-setup'><button id='wifi-setup-button' type='button'>Wi-Fi setup</button>"
    "<small id='wifi-setup-note'></small></section></main><script>"
    "const statusEl=document.querySelector('#status');const buttonStatusEl=document.querySelector('#button-status');"
    "const syncButton=document.querySelector('#sync-button');"
    "const syncButtonLabel=syncButton.querySelector('.action-label');"
    "const reconnectButton=document.querySelector('#reconnect-button');"
    "const reconnectButtonLabel=reconnectButton.querySelector('.action-label');"
    "let syncRequestPending=false,pairingPending=false;"
    "let reconnectRequestPending=false,manualReconnectActive=false;"
    "let pairingWindowStarted=false,pairingWindowSeconds=0,pairingExpires=0,pairingInitialHost='';"
    "let syncCountdownTimer=null,statusRequestController=null;"
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
    "document.addEventListener('visibilitychange',()=>{if(document.hidden){"
    "for(const b of document.querySelectorAll('.pressed'))release(b)}else refreshStatus(true)});"
    "window.addEventListener('focus',()=>{if(!document.hidden)refreshStatus(true)});"
    "function finishPairing(message){if(syncCountdownTimer){clearInterval(syncCountdownTimer);syncCountdownTimer=null}"
    "pairingPending=false;pairingWindowStarted=false;pairingWindowSeconds=0;"
    "pairingInitialHost='';syncButtonLabel.textContent='Pair';"
    "syncButton.disabled=syncRequestPending||reconnectRequestPending||manualReconnectActive;"
    "buttonStatusEl.textContent=message||''}"
    "function startPairingCountdown(){pairingWindowStarted=true;"
    "pairingExpires=Date.now()+pairingWindowSeconds*1000;buttonStatusEl.textContent='Pairing window open';"
    "const refreshSync=()=>{const left=Math.max(0,Math.ceil((pairingExpires-Date.now())/1000));"
    "syncButtonLabel.textContent=left?'Pair · '+left+'s':'Pair';if(!left)finishPairing('')};"
    "if(syncCountdownTimer)clearInterval(syncCountdownTimer);refreshSync();"
    "syncCountdownTimer=setInterval(refreshSync,250)}"
    "function pairingPhaseLabel(phase){switch(phase){case'restarting-bluetooth':return'Restarting Bluetooth';"
    "case'removing-bond':return'Forgetting current host';"
    "case'opening-window':return'Opening pairing window';case'failed':return'Host release needs retry';"
    "default:return'Preparing pairing window'}}"
    "function pairingButtonPhase(phase){switch(phase){case'restarting-bluetooth':return'Restarting';"
    "case'removing-bond':return'Forgetting';"
    "case'opening-window':return'Opening';case'failed':return'Retry';default:return'Opening'}}"
    "async function requestSync(){if(syncRequestPending||pairingPending||reconnectRequestPending||manualReconnectActive)return;"
    "syncRequestPending=true;syncButton.disabled=true;reconnectButton.disabled=true;buttonStatusEl.textContent='';"
    "try{const r=await fetch('/api/sync',{method:'POST'});"
    "if(!r.ok)throw Error(await r.text());const d=await r.json();"
    "if(d.pairing_profile==='upstream-general-continuous'){"
    "syncButtonLabel.textContent='Pair';return;}"
    "pairingPending=true;"
    "pairingInitialHost=statusEl.dataset.host||'';pairingWindowStarted=false;"
    "pairingWindowSeconds=d.window_seconds;const hostReleasePending=!!pairingInitialHost;"
    "buttonStatusEl.textContent=hostReleasePending?'Releasing current host...':'Opening pairing window...';"
    "syncButtonLabel.textContent=hostReleasePending?'Pair · Releasing':'Pair · Opening';"
    "refreshStatus(true)}"
    "catch(e){finishPairing('Pair failed: '+e.message);syncButtonLabel.textContent='Pair'}"
    "finally{syncRequestPending=false;syncButton.disabled=pairingPending||reconnectRequestPending||manualReconnectActive;"
    "reconnectButton.disabled=reconnectRequestPending||manualReconnectActive||!statusEl.dataset.host}}"
    "syncButton.addEventListener('click',requestSync);"
    "async function requestReconnect(){if(reconnectRequestPending||manualReconnectActive||syncRequestPending||pairingPending)return;"
    "if(!statusEl.dataset.host){buttonStatusEl.textContent='No saved host to reconnect';return}"
    "reconnectRequestPending=true;reconnectButton.disabled=true;syncButton.disabled=true;"
    "reconnectButtonLabel.textContent='Reconnecting…';buttonStatusEl.textContent='Refreshing saved host connection...';"
    "try{const r=await fetch('/api/reconnect',{method:'POST'});"
    "if(!r.ok)throw Error(await r.text());manualReconnectActive=true;await refreshStatus(true);"
    "if(!statusEl.dataset.connected)buttonStatusEl.textContent='Reconnecting to last host'}"
    "catch(e){buttonStatusEl.textContent='Reconnect failed: '+e.message}"
    "finally{reconnectRequestPending=false;reconnectButton.disabled=manualReconnectActive||!statusEl.dataset.host;"
    "syncButton.disabled=pairingPending||manualReconnectActive||syncRequestPending;"
    "if(!manualReconnectActive)reconnectButtonLabel.textContent='Reconnect'}}"
    "reconnectButton.addEventListener('click',requestReconnect);"
    "let phyPrevious=null;"
    "function updatePhysical(s){const el=document.querySelector('#physical-button-status');if(!el)return;"
    "if(!s.phy_enabled){el.textContent='PHY: disabled';phyPrevious=null;return}"
    "const names=['Up','Right','Down','Left','A','B'],masks=[2048,512,1024,256,8,4];"
    "const counts=s.phy_counts;if(!Array.isArray(counts)||counts.length!==12)return;"
    "const events=[];if(phyPrevious)for(let i=0;i<6;i++){"
    "if(counts[i*2]!==phyPrevious[i*2])events.push('PHY Pressed '+names[i]);"
    "if(counts[i*2+1]!==phyPrevious[i*2+1])events.push('PHY Released '+names[i])}"
    "const heldNames=names.filter((_,i)=>s.phy_mask&masks[i]);"
    "if(events.length)el.textContent=events.join(' · ')+(heldNames.length?' | Held: '+heldNames.join(', '):'');"
    "else if(!phyPrevious)el.textContent=heldNames.length?'PHY Held: '+heldNames.join(', '):'PHY: no buttons pressed';"
    "phyPrevious=counts.slice()}"
    "function updateStatus(s){updatePhysical(s);const host=s.last_host?' '+s.last_host:'';"
    "const link=s.connected?'Bluetooth HID link active'+(s.last_host?' · '+s.last_host:''):"
    "s.sync_window?'Pairing window open':"
    "s.reconnecting?'Trying saved host'+host:"
    "s.last_host?'Disconnected · saved host'+host:'Bluetooth ready for Pair';"
    "statusEl.textContent=s.wifi?'Wi-Fi connected · '+s.ip+' · '+link:'Wi-Fi not connected yet';"
    "statusEl.dataset.connected=s.connected?'1':'0';statusEl.dataset.host=s.last_host||'';"
    "statusEl.dataset.pairingPhase=s.pairing_phase||'idle';"
    "manualReconnectActive=!!s.manual_reconnect;"
    "reconnectButtonLabel.textContent=manualReconnectActive||reconnectRequestPending?'Reconnecting…':'Reconnect';"
    "reconnectButton.disabled=!s.last_host||manualReconnectActive||reconnectRequestPending||"
    "syncRequestPending||pairingPending||s.sync_window||statusEl.dataset.pairingPhase!=='idle';"
    "syncButton.disabled=syncRequestPending||pairingPending||reconnectRequestPending||manualReconnectActive;"
    "if(pairingPending&&s.sync_window){if(!pairingWindowStarted)startPairingCountdown()}"
    "else if(pairingPending){const phase=pairingPhaseLabel(statusEl.dataset.pairingPhase);"
    "buttonStatusEl.textContent=phase;syncButtonLabel.textContent='Pair · '+"
    "pairingButtonPhase(statusEl.dataset.pairingPhase)}"
    "if(pairingPending&&s.pairing_phase==='failed')"
    "finishPairing('Pair failed to release the host; Pair can be retried');"
    "else if(pairingPending&&s.connected&&s.pairing_phase==='idle'&&!s.sync_window)"
    "finishPairing(pairingInitialHost&&s.last_host===pairingInitialHost?"
    "'Previous host reconnected':'Connected to host');"
    "if(!s.connected)for(const b of document.querySelectorAll('.pressed'))release(b)}"
    "async function refreshStatus(force=false){if(document.hidden)return;"
    "if(statusRequestController&&!force)return;"
    "if(statusRequestController)statusRequestController.abort();"
    "const controller=new AbortController();statusRequestController=controller;"
    "const timeout=setTimeout(()=>controller.abort(),3000);"
    "try{const r=await fetch('/api/status',{cache:'no-store',signal:controller.signal});"
    "if(!r.ok)throw Error('HTTP '+r.status);updateStatus(await r.json())}"
    "catch(e){if(statusRequestController===controller){"
    "statusEl.textContent='Controller status unavailable · check Wi-Fi';statusEl.dataset.connected='0'}}"
    "finally{clearTimeout(timeout);if(statusRequestController===controller)statusRequestController=null}}"
    "refreshStatus();setInterval(refreshStatus,1000);"
    "async function fetchSensorData(url){const controller=new AbortController();"
    "const timeout=setTimeout(()=>controller.abort(),2000);"
    "try{const r=await fetch(url,{cache:'no-store',signal:controller.signal});"
    "if(!r.ok)throw Error('HTTP '+r.status);return await r.json()}"
    "finally{clearTimeout(timeout)}}"
    "async function updateCamera(){try{const d=await fetchSensorData('/api/camera');"
    "cameraStatusEl.textContent=!d.detected?'Camera not detected · check wiring':"
    "!d.available?'Camera detected · waiting for a frame':'Live camera frame';"
    "for(let i=0;i<4;i++){const p=d.points[i];"
    "document.getElementById('x'+i).textContent=d.available?p.x:'-';"
    "document.getElementById('y'+i).textContent=d.available?p.y:'-';"
    "if(!d.available||p.x>=1023||p.y>=1023){cameraDots[i].classList.add('off');continue}"
    "cameraDots[i].style.left=(p.x/1023*cameraTracker.clientWidth)+'px';"
    "cameraDots[i].style.top=(p.y/1023*cameraTracker.clientHeight)+'px';"
    "cameraDots[i].classList.remove('off')}}catch(e){cameraStatusEl.textContent='Camera data request failed'}}"
    "async function updateImu(){try{const d=await fetchSensorData('/api/imu');"
    "imuStatusEl.textContent=!d.detected?'BNO08x not detected · check wiring':"
    "!d.available?'BNO08x found at 0x'+d.address.toString(16).padStart(2,'0')+' · waiting for orientation':"
    "'Live orientation · 0x'+d.address.toString(16).padStart(2,'0');"
    "document.querySelector('#imu-accel').textContent=d.accel_available?"
    "'HID accel: '+d.accel_raw.join(', ')+' · mode 0x'+d.report_mode.toString(16)+"
    "' · '+(!d.hid_connected?'no HID host':[0x31,0x33,0x35,0x37].includes(d.report_mode)?"
    "'motion mode requested (receipt unverified)':'host not requesting motion'):"
    "'HID accel: neutral fallback (missing or stale data)';"
    "if(d.available){const roll=d.roll,pitch=d.pitch,yaw=d.yaw;"
    "document.querySelector('#imu-roll').textContent=roll.toFixed(1);"
    "document.querySelector('#imu-pitch').textContent=pitch.toFixed(1);"
    "document.querySelector('#imu-yaw').textContent=yaw.toFixed(1);"
    "imuCube.style.transform=`rotateX(${pitch}deg) rotateY(${roll}deg) rotateZ(${yaw}deg)`}"
    "else{document.querySelector('#imu-roll').textContent='-';"
    "document.querySelector('#imu-pitch').textContent='-';"
    "document.querySelector('#imu-yaw').textContent='-';imuCube.style.transform='none'}}"
    "catch(e){imuStatusEl.textContent='IMU data request failed';"
    "document.querySelector('#imu-accel').textContent='HID accel data unavailable'}}"
    "let sensorPollPending=false;"
    "async function pollSensors(){if(document.hidden||sensorPollPending)return;"
    "sensorPollPending=true;try{await updateImu();"
    "if(!document.hidden)await updateCamera()}"
    "finally{sensorPollPending=false}}"
    "pollSensors();setInterval(pollSensors,200);"
    "const ncPanel=document.querySelector('#nc-panel'),ncStatus=document.querySelector('#nc-status');"
    "const ncToggle=document.querySelector('#nc-toggle'),ncPad=document.querySelector('#nc-pad');"
    "const ncKnob=document.querySelector('#nc-knob');let ncAttached=false,ncDirty=false,ncBusy=false;"
    "const nc={sx:128,sy:128,c:0,z:0};"
    "function ncShow(d){ncAttached=d.attached;ncPanel.classList.toggle('off',!d.attached);"
    "ncToggle.classList.toggle('on',d.attached);ncToggle.textContent=d.attached?'Disconnect Nunchuk':'Connect Nunchuk';"
    "ncStatus.textContent=(d.attached?'Connected':'Disconnected')+' · stick '+d.stick.join(', ')+"
    "(d.c?' · C':'')+(d.z?' · Z':'')+(d.encrypted?(d.unsupported_key?"
    "' · encryption key unsupported':' · encrypted'):'')+(d.attached&&d.motion_plus_active?"
    "(d.mp_mode===5?' · via MotionPlus passthrough':' · behind MotionPlus (not passthrough)'):'')}"
    "async function ncPost(body){const r=await fetch('/api/nunchuk',{method:'POST',"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'},body});"
    "if(!r.ok)throw Error(await r.text());ncShow(await r.json())}"
    "async function ncFlush(){if(ncBusy)return;ncBusy=true;try{while(ncDirty){ncDirty=false;"
    "await ncPost('sx='+nc.sx+'&sy='+nc.sy+'&c='+nc.c+'&z='+nc.z)}}"
    "catch(e){ncStatus.textContent='Nunchuk error: '+e.message}finally{ncBusy=false}}"
    "function ncSend(){ncDirty=true;ncFlush()}"
    "ncToggle.addEventListener('click',()=>ncPost('attached='+(ncAttached?0:1)).catch(e=>"
    "ncStatus.textContent='Nunchuk error: '+e.message));"
    "function ncStick(e){const r=ncPad.getBoundingClientRect(),rad=r.width/2;"
    "let dx=(e.clientX-r.left-rad)/rad,dy=(e.clientY-r.top-rad)/rad;const m=Math.hypot(dx,dy);"
    "if(m>1){dx/=m;dy/=m}ncKnob.style.left=(50+dx*50)+'%';ncKnob.style.top=(50+dy*50)+'%';"
    "nc.sx=Math.round(128+dx*96);nc.sy=Math.round(128-dy*96);ncSend()}"
    "function ncCenter(){ncKnob.style.left='50%';ncKnob.style.top='50%';nc.sx=128;nc.sy=128;ncSend()}"
    "ncPad.addEventListener('pointerdown',e=>{e.preventDefault();ncPad.setPointerCapture(e.pointerId);ncStick(e)});"
    "ncPad.addEventListener('pointermove',e=>{if(ncPad.hasPointerCapture(e.pointerId))ncStick(e)});"
    "for(const ev of ['pointerup','pointercancel'])ncPad.addEventListener(ev,ncCenter);"
    "document.querySelectorAll('[data-nc]').forEach(b=>{const k=b.dataset.nc;"
    "b.addEventListener('pointerdown',e=>{e.preventDefault();b.setPointerCapture(e.pointerId);"
    "b.classList.add('pressed');nc[k]=1;ncSend()});"
    "for(const ev of ['pointerup','pointercancel'])b.addEventListener(ev,()=>{"
    "if(!nc[k])return;b.classList.remove('pressed');nc[k]=0;ncSend()});"
    "b.addEventListener('contextmenu',e=>e.preventDefault())});"
    "async function ncRefresh(){if(document.hidden||ncBusy)return;try{const r=await fetch('/api/nunchuk',"
    "{cache:'no-store'});if(r.ok)ncShow(await r.json())}catch(e){}}ncRefresh();setInterval(ncRefresh,2000);"
    "document.querySelector('#wifi-setup-button').onclick=async()=>{"
    "if(!confirm('Restart nextMote into Wi-Fi setup mode? Bluetooth stops until setup is finished or the device is reset.'))return;"
    "const n=document.querySelector('#wifi-setup-note');try{const r=await fetch('/api/wifi/setup',{method:'POST'});"
    "n.textContent=r.ok?'Restarting. Join the nextMote-Setup Wi-Fi network and open http://192.168.4.1/':"
    "'Request failed'}catch(e){n.textContent='Request failed'}};"
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
    char pairing_phase[sizeof(s_pairing_phase)];
    bool reconnect_pending;
    bool manual_reconnect_pending;
    portENTER_CRITICAL(&s_link_status_mux);
    memcpy(last_host, s_last_host_address, sizeof(last_host));
    reconnect_pending = s_reconnect_pending;
    manual_reconnect_pending = s_manual_reconnect_pending;
    memcpy(pairing_phase, s_pairing_phase, sizeof(pairing_phase));
    portEXIT_CRITICAL(&s_link_status_mux);

    esp_netif_ip_info_t info = {0};
    if (s_station && esp_netif_get_ip_info(s_station, &info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }
    char ssid_json[6 * NEXTMOTE_WIFI_SSID_MAX + 1];
    nextmote_json_escape(s_wifi_ssid, ssid_json, sizeof(ssid_json));
    wiimote_physical_buttons_snapshot_t phy;
    wiimote_physical_buttons_get_snapshot(&phy);
    char phy_json[192];
    int phy_length = snprintf(phy_json, sizeof(phy_json),
        "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu",
        (unsigned long)phy.presses[0], (unsigned long)phy.releases[0],
        (unsigned long)phy.presses[1], (unsigned long)phy.releases[1],
        (unsigned long)phy.presses[2], (unsigned long)phy.releases[2],
        (unsigned long)phy.presses[3], (unsigned long)phy.releases[3],
        (unsigned long)phy.presses[4], (unsigned long)phy.releases[4],
        (unsigned long)phy.presses[5], (unsigned long)phy.releases[5]);
    if (phy_length < 0 || (size_t)phy_length >= sizeof(phy_json)) {
        ESP_LOGE(TAG, "Physical button status formatting failed");
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Button status formatting failed");
    }
    char response[832];
    bool hid_connected = hid_connected_snapshot();
    bool sync_window_active = sync_window_snapshot();
    int length = snprintf(response, sizeof(response),
             "{\"wifi\":%s,\"ssid\":\"%s\",\"connected\":%s,\"sync_window\":%s,\"ip\":\"%s\",\"pairing_profile\":\"%s\",\"last_host\":\"%s\",\"reconnecting\":%s,\"manual_reconnect\":%s,\"pairing_phase\":\"%s\",\"phy_enabled\":%s,\"phy_mask\":%u,\"phy_counts\":[%s]}",
             (xEventGroupGetBits(s_wifi_events) & WIFI_CONNECTED_BIT) ? "true" : "false",
             ssid_json, hid_connected ? "true" : "false",
             sync_window_active ? "true" : "false", ip, PAIRING_PROFILE, last_host,
             reconnect_pending ? "true" : "false",
             manual_reconnect_pending ? "true" : "false", pairing_phase,
             phy.enabled ? "true" : "false", phy.mask, phy_json);
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

    wiimote_state_snapshot_t remote;
    wiimote_state_get_snapshot(&remote);
    uint8_t accel[5] = {0};
    if (!state.accel_available || !wiimote_accel_encode(accel, state.accel_ms2)) {
        wiimote_accel_encode_neutral(accel);
    }
    const unsigned raw_x = (accel[2] << 2) | ((accel[0] >> 5) & 3U);
    const unsigned raw_y = (accel[3] << 2) | ((accel[1] >> 4) & 2U);
    const unsigned raw_z = (accel[4] << 2) | ((accel[1] >> 5) & 2U);
    const long long age_ms = state.accel_samples ?
        (esp_timer_get_time() - state.accel_time_us) / 1000 : -1;

    uint32_t hid_data_sent, hid_send_failures;
    wiimote_reports_get_counters(&hid_data_sent, &hid_send_failures);

    char response[640];
    int length = snprintf(response, sizeof(response),
                          "{\"detected\":%s,\"available\":%s,\"address\":%u,"
                          "\"roll\":%.2f,\"pitch\":%.2f,\"yaw\":%.2f,"
                          "\"accel_available\":%s,\"accel_samples\":%lu,\"accel_age_ms\":%lld,"
                          "\"accel_ms2\":[%.4f,%.4f,%.4f],\"accel_raw\":[%u,%u,%u],"
                          "\"recoveries\":%lu,\"gyro_samples\":%lu,\"motion_plus_active\":%s,"
                          "\"hid_data_sent\":%lu,\"hid_send_failures\":%lu,"
                          "\"report_mode\":%u,\"hid_connected\":%s}",
                          state.detected ? "true" : "false",
                          state.available ? "true" : "false",
                          (unsigned)state.address,
                          (double)state.roll_deg,
                          (double)state.pitch_deg,
                          (double)state.yaw_deg,
                          state.accel_available ? "true" : "false",
                          (unsigned long)state.accel_samples, age_ms,
                          (double)state.accel_ms2[0], (double)state.accel_ms2[1],
                          (double)state.accel_ms2[2], raw_x, raw_y, raw_z,
                          (unsigned long)state.recoveries, (unsigned long)state.gyro_samples,
                          wiimote_motion_plus_is_active() ? "true" : "false",
                          (unsigned long)hid_data_sent, (unsigned long)hid_send_failures,
                          remote.report_mode, remote.connected ? "true" : "false");
    if (length < 0 || (size_t)length >= sizeof(response)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "IMU response formatting failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, response, (ssize_t)length);
}

static esp_err_t bus_handler(httpd_req_t *req)
{
    char query[32];
    char value[8];
    const bool reset = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                       httpd_query_key_value(query, "reset", value, sizeof(value)) == ESP_OK &&
                       value[0] == '1';

    sensor_i2c_bus_stats_t stats;
    sensor_i2c_bus_get_stats(&stats);
    if (reset) sensor_i2c_bus_reset_stats();

    static const char *const names[SENSOR_I2C_USER_COUNT] = {"camera", "imu", "other"};
    char response[1024];
    size_t used = 0;
    int length = snprintf(response, sizeof(response), "{\"window_ms\":%lld,\"reset\":%s",
                          (long long)((esp_timer_get_time() - stats.since_us) / 1000),
                          reset ? "true" : "false");
    if (length < 0) goto fail;
    used = (size_t)length;
    for (int i = 0; i < SENSOR_I2C_USER_COUNT; ++i) {
        const sensor_i2c_user_stats_t *u = &stats.user[i];
        length = snprintf(response + used, sizeof(response) - used,
                          ",\"%s\":{\"locks\":%lu,\"timeouts\":%lu,"
                          "\"wait_avg_us\":%lu,\"wait_max_us\":%lu,"
                          "\"waits_over_20ms\":%lu,\"waits_over_100ms\":%lu,"
                          "\"hold_avg_us\":%lu,\"hold_max_us\":%lu,"
                          "\"holds_over_20ms\":%lu,\"holds_over_100ms\":%lu}",
                          names[i], (unsigned long)u->locks, (unsigned long)u->timeouts,
                          (unsigned long)(u->locks ? u->wait_sum_us / u->locks : 0),
                          (unsigned long)u->wait_max_us,
                          (unsigned long)u->waits_over_20ms, (unsigned long)u->waits_over_100ms,
                          (unsigned long)(u->locks ? u->hold_sum_us / u->locks : 0),
                          (unsigned long)u->hold_max_us,
                          (unsigned long)u->holds_over_20ms, (unsigned long)u->holds_over_100ms);
        if (length < 0 || (size_t)length >= sizeof(response) - used) goto fail;
        used += (size_t)length;
    }
    length = snprintf(response + used, sizeof(response) - used,
                      ",\"imu_packets\":{\"count\":%lu,\"avg_us\":%lu,\"max_us\":%lu,"
                      "\"over_5ms\":%lu,\"over_20ms\":%lu}}",
                      (unsigned long)stats.imu_packets,
                      (unsigned long)(stats.imu_packets ?
                                      stats.imu_packet_sum_us / stats.imu_packets : 0),
                      (unsigned long)stats.imu_packet_max_us,
                      (unsigned long)stats.imu_packets_over_5ms,
                      (unsigned long)stats.imu_packets_over_20ms);
    if (length < 0 || (size_t)length >= sizeof(response) - used) goto fail;
    used += (size_t)length;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, response, (ssize_t)used);

fail:
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "bus response formatting failed");
    return ESP_FAIL;
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

static esp_err_t send_nunchuk_state(httpd_req_t *req)
{
    wiimote_nunchuk_status_t status;
    wiimote_nunchuk_get_status(&status);
    char response[256];
    int length = snprintf(response, sizeof(response),
                          "{\"attached\":%s,\"source\":\"web\",\"stick\":[%u,%u],"
                          "\"accel\":[%u,%u,%u],\"c\":%s,\"z\":%s,\"encrypted\":%s,"
                          "\"unsupported_key\":%s,\"encryption_enables\":%lu,"
                          "\"motion_plus_active\":%s,\"mp_mode\":%u}",
                          status.attached ? "true" : "false",
                          status.input.stick_x, status.input.stick_y,
                          status.input.accel[0], status.input.accel[1], status.input.accel[2],
                          status.input.c ? "true" : "false", status.input.z ? "true" : "false",
                          status.encrypted ? "true" : "false",
                          status.unsupported_key ? "true" : "false",
                          (unsigned long)status.encryption_enables,
                          wiimote_motion_plus_is_active() ? "true" : "false",
                          wiimote_motion_plus_mode());
    if (length < 0 || (size_t)length >= sizeof(response)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "nunchuk response formatting failed");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, response, (ssize_t)length);
}

static esp_err_t nunchuk_get_handler(httpd_req_t *req)
{
    return send_nunchuk_state(req);
}

static bool parse_flag(const char *text, bool *value)
{
    if ((text[0] != '0' && text[0] != '1') || text[1] != '\0') return false;
    *value = text[0] == '1';
    return true;
}

static bool parse_byte(const char *text, uint8_t *value)
{
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < 0 || parsed > 255) return false;
    *value = (uint8_t)parsed;
    return true;
}

/* Form fields (all optional): attached=0|1, sx/sy=0..255, c=0|1, z=0|1. */
static esp_err_t nunchuk_post_handler(httpd_req_t *req)
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

    wiimote_nunchuk_status_t status;
    wiimote_nunchuk_get_status(&status);
    wiimote_nunchuk_input_t input = status.input;
    bool attached = status.attached;
    bool have_attached = false;
    char *save = NULL;
    for (char *token = strtok_r(body, "&", &save); token != NULL;
         token = strtok_r(NULL, "&", &save)) {
        bool ok;
        if (strncmp(token, "attached=", 9) == 0) {
            ok = parse_flag(token + 9, &attached);
            have_attached = ok;
        } else if (strncmp(token, "sx=", 3) == 0) {
            ok = parse_byte(token + 3, &input.stick_x);
        } else if (strncmp(token, "sy=", 3) == 0) {
            ok = parse_byte(token + 3, &input.stick_y);
        } else if (strncmp(token, "c=", 2) == 0) {
            ok = parse_flag(token + 2, &input.c);
        } else if (strncmp(token, "z=", 2) == 0) {
            ok = parse_flag(token + 2, &input.z);
        } else {
            ok = false;
        }
        if (!ok) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid nunchuk field");
            return ESP_FAIL;
        }
    }
    wiimote_nunchuk_set_input(&input);
    if (have_attached && wiimote_reports_set_nunchuk_attached(attached)) {
        ESP_LOGI(TAG, "Web Nunchuk %s", attached ? "connected" : "disconnected");
    }
    return send_nunchuk_state(req);
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
            return httpd_resp_sendstr(req, "Pair could not start: Bluetooth is not ready or a host release is still pending");
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

static esp_err_t reconnect_handler(httpd_req_t *req)
{
    if (req->content_len != 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Reconnect request must not have a body");
    }
    if (!s_reconnect_callback) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Saved-host reconnect callback is unavailable");
    }

    esp_err_t err = s_reconnect_callback();
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "No saved bonded host is available to reconnect");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "Reconnect is unavailable while Pair or another connection transition is active");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saved-host reconnect request failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req,
                              "{\"ok\":true,\"queued\":true,\"action\":\"reconnect_saved_host\"}");
}

static void restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t wifi_setup_handler(httpd_req_t *req)
{
    esp_err_t err = nextmote_wifi_config_request_setup();
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, esp_err_to_name(err));
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"action\":\"restart_into_setup\"}");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static const httpd_uri_t s_wifi_setup_uri = {
    .uri = "/api/wifi/setup", .method = HTTP_POST, .handler = wifi_setup_handler, .user_ctx = NULL,
};
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
static const httpd_uri_t s_bus_uri = {
    .uri = "/api/bus", .method = HTTP_GET, .handler = bus_handler, .user_ctx = NULL,
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
static const httpd_uri_t s_nunchuk_get_uri = {
    .uri = "/api/nunchuk", .method = HTTP_GET, .handler = nunchuk_get_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_nunchuk_post_uri = {
    .uri = "/api/nunchuk", .method = HTTP_POST, .handler = nunchuk_post_handler, .user_ctx = NULL,
};
static const httpd_uri_t s_reconnect_uri = {
    .uri = "/api/reconnect", .method = HTTP_POST,
    .handler = reconnect_handler, .user_ctx = NULL,
};

static void start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
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
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_bus_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_button_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_nunchuk_get_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_nunchuk_post_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_sync_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_sync_limited_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_sync_general_uri));
#if CONFIG_EXAMPLE_BT_DISCOVERY_TRACE
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_controller_readback_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_bond_snapshot_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_virtual_cable_unplug_uri));
#endif
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_sync_rearm_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_reconnect_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_http_server, &s_wifi_setup_uri));

    esp_netif_ip_info_t info = {0};
    ESP_ERROR_CHECK(esp_netif_get_ip_info(s_station, &info));
    ESP_LOGI(TAG, "HTTP server ready: http://" IPSTR "/", IP2STR(&info.ip));
}

/* Wi-Fi shares one 2.4 GHz radio with Bluetooth Classic. A 20 MHz channel
 * leaves more of the band free for Bluetooth hopping. Set before esp_wifi_start(). */
static void configure_wifi_bandwidth(void)
{
    esp_err_t err = esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20);
    ESP_LOGI(TAG, "Wi-Fi bandwidth 20 MHz: %s", esp_err_to_name(err));
}

/* Applied once associated so the coexistence scheduler is active; keeps the
 * station from sleeping through buffered frames for the web page. */
static void configure_wifi_power_save(void)
{
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi power save off rejected (%s); keeping minimum modem sleep",
                 esp_err_to_name(err));
        (void)esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    } else {
        ESP_LOGI(TAG, "Wi-Fi power save: off");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "Stored Wi-Fi SSID: %s", s_wifi_ssid);
        ESP_LOGI(TAG, "Connecting to configured Wi-Fi network");
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) ESP_LOGW(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *event = event_data;
        ESP_LOGI(TAG, "Associated with AP channel=%u auth=%u", event->channel, event->authmode);
        configure_wifi_power_save();
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
    char password[NEXTMOTE_WIFI_PASSWORD_MAX + 1];
    if (!nextmote_wifi_config_load(s_wifi_ssid, password)) {
        ESP_LOGE(TAG, "No stored Wi-Fi network; use serial command 'setup'");
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
    memcpy(wifi_config.sta.ssid, s_wifi_ssid, strlen(s_wifi_ssid));
    memcpy(wifi_config.sta.password, password, strlen(password));
    wifi_config.sta.threshold.authmode = password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    memset(password, 0, sizeof(password));
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    configure_wifi_bandwidth();
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

void wiimote_web_set_manual_reconnect_pending(bool pending)
{
    portENTER_CRITICAL(&s_link_status_mux);
    s_manual_reconnect_pending = pending;
    portEXIT_CRITICAL(&s_link_status_mux);
}

void wiimote_web_set_pairing_phase(const char *phase)
{
    if (phase == NULL) {
        ESP_LOGE(TAG, "Cannot set Pair phase from a null value");
        return;
    }

    char copy[sizeof(s_pairing_phase)];
    snprintf(copy, sizeof(copy), "%s", phase);
    portENTER_CRITICAL(&s_link_status_mux);
    memcpy(s_pairing_phase, copy, sizeof(s_pairing_phase));
    portEXIT_CRITICAL(&s_link_status_mux);
}

void wiimote_web_set_sync_callback(wiimote_web_sync_callback_t callback)
{
    s_sync_callback = callback;
}

void wiimote_web_set_reconnect_callback(
    wiimote_web_reconnect_callback_t callback)
{
    s_reconnect_callback = callback;
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

// Run with: node test/test_sensor_polling.js
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(
    path.join(__dirname, '../wiimote-hid-test/main/wiimote_web.c'), 'utf8');
const block = source.split('static const char INDEX_HTML[] =')[1]
    .split('\n\nstatic uint16_t')[0];
const html = [...block.matchAll(/"(?:[^"\\]|\\.)*"/g)]
    .map(match => JSON.parse(match[0])).join('');
const script = html.match(/<script>([\s\S]*)<\/script>/)[1];
new Function(script);
const sensorCode = script.slice(script.indexOf('async function fetchSensorData'),
    script.indexOf('pollSensors();setInterval'));
const elements = {};
const requests = [];
const timers = new Map();
let nextTimer = 0;
const context = {
    AbortController,
    document: {
        hidden: false,
        querySelector: id => elements[id] ??= {textContent: ''},
        getElementById: id => elements[id] ??= {textContent: ''},
    },
    imuStatusEl: {},
    imuCube: {style: {}},
    cameraStatusEl: {},
    cameraDots: Array.from({length: 4}, () => ({
        classList: {add() {}, remove() {}}, style: {},
    })),
    cameraTracker: {clientWidth: 100, clientHeight: 100},
    setTimeout(callback, ms) {
        assert.equal(ms, 2000);
        timers.set(++nextTimer, callback);
        return nextTimer;
    },
    clearTimeout(id) { timers.delete(id); },
    fetch(url, options) {
        return new Promise((resolve, reject) => {
            requests.push({url, resolve, reject, signal: options.signal});
            options.signal.addEventListener('abort', () => reject(Error('aborted')));
        });
    },
};
const poll = vm.runInNewContext(sensorCode + ';pollSensors', context);
const settle = () => new Promise(resolve => setImmediate(resolve));
const imu = {
    detected: true, available: true, address: 75, roll: 1, pitch: 2, yaw: 3,
    accel_available: true, accel_raw: [512, 512, 616], report_mode: 48,
    hid_connected: false,
};
const camera = {
    detected: false, available: false,
    points: Array.from({length: 4}, () => ({x: 1023, y: 1023})),
};
function respond(request, data) {
    request.resolve({ok: true, json: async () => data});
}

(async () => {
    const first = poll();
    assert.equal(requests.length, 1);
    for (let i = 0; i < 100; ++i) await poll();
    assert.equal(requests.length, 1, 'Slow IMU must not accumulate requests');
    respond(requests[0], imu);
    await settle();
    assert.equal(requests[1].url, '/api/camera');
    assert.equal(elements['#imu-roll'].textContent, '1.0');
    assert.match(elements['#imu-accel'].textContent, /no HID host/);
    await poll();
    assert.equal(requests.length, 2, 'Camera and IMU share a single pending cycle');
    respond(requests[1], camera);
    await first;
    assert.equal(timers.size, 0);

    context.document.hidden = true;
    await poll();
    assert.equal(requests.length, 2, 'Hidden page must stop sensor requests');
    context.document.hidden = false;
    const timedOut = poll();
    [...timers.values()][0]();
    await settle();
    assert.equal(context.imuStatusEl.textContent, 'IMU data request failed');
    assert.equal(elements['#imu-accel'].textContent, 'HID accel data unavailable');
    respond(requests.at(-1), camera);
    await timedOut;
    assert.equal(timers.size, 0);

    const retry = poll();
    respond(requests.at(-1), imu);
    await settle();
    requests.at(-1).resolve({ok: false, status: 503});
    await retry;
    assert.equal(context.cameraStatusEl.textContent, 'Camera data request failed');
    assert.equal(timers.size, 0);

    const bodyTimeout = poll();
    const bodySignal = requests.at(-1).signal;
    requests.at(-1).resolve({
        ok: true,
        json: () => new Promise((resolve, reject) => {
            bodySignal.addEventListener('abort', () => reject(Error('body aborted')));
        }),
    });
    await settle();
    assert.equal(timers.size, 1, 'Timeout must cover response body parsing too');
    [...timers.values()][0]();
    await settle();
    respond(requests.at(-1), camera);
    await bodyTimeout;
    assert.equal(timers.size, 0);
    console.log('Sensor polling: overlap, timeout, hidden page, disconnected updates, failures/retry PASS');
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const source = fs.readFileSync(path.join(__dirname,
    '../wiimote-hid-test/main/wiimote_web.c'), 'utf8');
const start = source.indexOf('static const char INDEX_HTML[] =');
const endMarker = '    "</script></body></html>";';
const end = source.indexOf(endMarker, start);
assert.notEqual(start, -1, 'INDEX_HTML declaration exists');
assert.notEqual(end, -1, 'INDEX_HTML closing literal exists');

const literals = source.slice(start, end + endMarker.length)
    .match(/"(?:\\.|[^"\\])*"/g);
const html = literals.map(literal => JSON.parse(literal)).join('');
const scriptMatch = html.match(/<script>([\s\S]*?)<\/script>/);
assert.ok(scriptMatch, 'controller page has an inline script');
new vm.Script(scriptMatch[1]);

assert.match(html, /id='nc-simulator-tab'[^>]*>Simulator<\/button>/);
assert.match(html, /id='nc-real-tab'[^>]*>Real Nunchuk<\/button>/);
assert.match(html, /id='nc-simulator-view'/);
assert.match(html, /id='nc-real-view'[^>]*hidden/);
assert.match(html, /id='nc-real-rate'/);
assert.match(html, /id='nc-real-attempt-errors'/);
assert.match(html, /id='nc-real-error'/);
assert.match(scriptMatch[1], /source=physical/);
assert.match(scriptMatch[1], /source=simulator/);
assert.match(scriptMatch[1], /nc-real-raw/);

console.log('Nunchuk tabs: exclusive views, source requests and embedded JS syntax PASS');

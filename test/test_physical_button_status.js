const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(require('node:path').join(__dirname,
    '../wiimote-hid-test/main/wiimote_web.c'), 'utf8');
const start = source.indexOf('    "let phyPrevious=null;"');
const end = source.indexOf('    "function updateStatus(s)', start);
const script = source.slice(start, end).trim().split('\n')
    .map(line => JSON.parse(line.trim())).join('');
const element = {textContent: ''};
const context = vm.createContext({document: {querySelector: () => element}});
vm.runInContext(script, context);
const counts = Array(12).fill(0);
function update(enabled, mask) {
    context.status = {phy_enabled: enabled, phy_mask: mask, phy_counts: counts.slice()};
    vm.runInContext('updatePhysical(status)', context);
}
update(false, 0);
assert.equal(element.textContent, 'PHY: disabled');
update(true, 0);
assert.equal(element.textContent, 'PHY: no buttons pressed');
counts[8]++;
update(true, 8);
assert.match(element.textContent, /PHY Pressed A/);
counts[9]++;
update(true, 0);
assert.equal(element.textContent, 'PHY Released A');
// Both transitions between status polls remain visible, without Bluetooth.
counts[10]++;
counts[11]++;
update(true, 0);
assert.equal(element.textContent, 'PHY Pressed B · PHY Released B');
update(true, 0);
assert.equal(element.textContent, 'PHY Pressed B · PHY Released B');
console.log('PHY web status: disabled, initial state, press/release and between-poll tap PASS');

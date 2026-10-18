const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const root = path.join(__dirname, "..");
const source = fs.readFileSync(path.join(root, "ESP8266_OLED_Hello_modif_OTA.ino"), "utf8");
const backup = fs.readFileSync(path.join(root, "..", "ESP8266_OLED_Hello", "ESP8266_OLED_Hello.ino"), "utf8");
const section = text => text.slice(text.indexOf("    const setConnectionStatus"), text.indexOf("    const programLabels"));
assert.equal(section(source), section(backup));
for (const text of [source, backup]) {
  const page = text.split('R"rawliteral(')[1].split(')rawliteral";')[0];
  assert.ok(!page.includes("data.ip"), "UI must not display device IPs");
  assert.ok(!page.includes("statusText"));
  for (const id of ["dashboardStatus", "relayStatus"]) {
    assert.ok(page.includes(`id="${id}" role="status"`));
    assert.ok(page.includes(`setConnectionStatus("${id}",`));
  }
  assert.ok(page.includes('setConnectionStatus("dashboardStatus", "Dashboard", Boolean(data.wifi))'));
  assert.ok(page.includes('setConnectionStatus("relayStatus", "Relai", Boolean(data.wifi))'));
  assert.ok(page.includes('setConnectionStatus("dashboardStatus", "Dashboard", false)'));
  assert.ok(page.includes('setConnectionStatus("relayStatus", "Relai", false)'));
}
const elements = {};
const $ = id => elements[id] || (elements[id] = {
  online:false, attributes:{},
  classList:{toggle(name, value) { assert.equal(name, "online"); elements[id].online = value; }},
  setAttribute(name, value) { this.attributes[name] = value; }
});
const context = vm.createContext({$});
vm.runInContext(section(source), context);
for (const dashboard of [true, false]) {
  for (const relay of [true, false]) {
    vm.runInContext(`setConnectionStatus("dashboardStatus", "Dashboard", ${dashboard});
      setConnectionStatus("relayStatus", "Relai", ${relay});`, context);
    for (const [id, label, online] of [["dashboardStatus", "Dashboard", dashboard], ["relayStatus", "Relai", relay]]) {
      assert.equal($(id).online, online);
      assert.equal($(id).attributes["aria-label"], `${label} : ${online ? "en ligne" : "hors ligne"}`);
      assert.equal($(id).title, $(id).attributes["aria-label"]);
    }
  }
}
console.log("PASS: independent Dashboard/Relai indicators, all online/offline combinations, accessible labels, no displayed IPs, identical variants");

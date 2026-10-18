const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const source = fs.readFileSync(path.join(__dirname, "..", "ESP8266_OLED_Hello_modif_OTA.ino"), "utf8");
const backup = fs.readFileSync(path.join(__dirname, "..", "..", "ESP8266_OLED_Hello", "ESP8266_OLED_Hello.ino"), "utf8");
const start = source.indexOf("    let growDatesDirty");
const end = source.indexOf("    const refresh = async", start);
const code = source.slice(start, end);
assert.equal(code, backup.slice(backup.indexOf("    let growDatesDirty"), backup.indexOf("    const refresh = async")));
for (const text of [source, backup]) {
  const phasePanel = text.match(/<details class="seed-bank phase-panel" aria-label="Phase suivie">([\s\S]*?)<\/details>/);
  assert.ok(phasePanel, "phase panel uses the shared collapsible seed-bank card");
  assert.ok(phasePanel[1].trimStart().startsWith("<summary>Phase suivie</summary>"));
  for (const id of ["growDatesForm", "trackedSeed", "germinationAge", "floweringForecast"]) {
    assert.ok(phasePanel[1].includes(`id="${id}"`), `${id} remains inside the collapsible card`);
  }
  for (const removed of ["phaseSelect", "Etiquette informative, sans consigne ni seuil automatique.",
    "Dates sauvegardees sur l'ESP. Aucun changement automatique de phase.",
    "Choix memorise dans ce navigateur. Actualiser apres modification des fiches.", " · germination +"]) {
    assert.ok(!text.includes(removed), `removed UI text/control remains: ${removed}`);
  }
  for (const id of ["growthRemaining", "floweringRemaining", "floweringForecast", "trackedSeed", "reloadTrackedSeeds", "trackedSeedMessage"]) {
    assert.ok(text.includes(`id="${id}"`), `missing dashboard element ${id}`);
  }
}
const elements = {};
const $ = id => elements[id] || (elements[id] = {
  value:"", textContent:"", disabled:false, validity:"", handlers:{}, children:[],
  replaceChildren() { this.children = []; },
  append(child) { this.children.push(child); },
  addEventListener(event, callback) { this.handlers[event] = callback; },
  setCustomValidity(message) { this.validity = message; },
  reportValidity() { this.reported = true; }
});
let pending;
let request;
const stored = new Map([["trackedSeedId", "0"]]);
let seedResponse = {ok:true, entries:[
  {id:0, name:"Test <b>seed</b>", growthDays:"30", floweringDays:"60"},
  {id:1, name:"Sans duree", growthDays:"", floweringDays:""},
  {id:2, name:"Zero", growthDays:"0", floweringDays:"0"}
]};
const context = vm.createContext({
  $, URLSearchParams,
  document:{createElement:() => ({})},
  localStorage:{
    getItem:key => stored.get(key) ?? null,
    setItem:(key,value) => stored.set(key,value)
  },
  fetch: (url, options) => {
    if (url === "/seed-bank/api") {
      if (seedResponse instanceof Error) return Promise.reject(seedResponse);
      return Promise.resolve({ok:seedResponse.ok, json:async () => seedResponse});
    }
    request = {url, options};
    return new Promise((resolve, reject) => { pending = {resolve, reject}; });
  }
});
vm.runInContext(code, context);
const render = data => {
  context.data = data;
  vm.runInContext("renderGrowDates(data)", context);
};
const initial = {
  germinationDate:"2026-10-08", germinationDays:10,
  floweringDate:"", floweringDays:null, growDatesAvailable:true, clockReady:true
};
const input = () => $("growDatesForm").handlers.input();
const submit = () => $("growDatesForm").handlers.submit({preventDefault() {}});

(async () => {
  render(initial);
  await new Promise(resolve => setImmediate(resolve));
  assert.equal($("trackedSeed").value, "0", "restore selection from browser storage");
  assert.equal($("trackedSeed").children[1].textContent, "Test <b>seed</b>", "seed names remain literal text");
  assert.equal($("growthRemaining").textContent,
    "Croissance : 20 jours restants theoriques · 30 jours prevus depuis germination");
  assert.equal($("germinationDate").value, "2026-10-08");
  assert.match($("germinationAge").textContent, /J\+10.*08\/10\/2026/);
  assert.equal($("floweringAge").textContent, "Floraison non renseignee");
  assert.equal($("floweringForecast").textContent,
    "Passage en floraison previsionnel : 07/11/2026");
  assert.equal($("growDatesMessage").textContent, "");
  assert.equal($("trackedSeedMessage").textContent, "");
  assert.match($("floweringRemaining").textContent, /date de passage en floraison non renseignee/);
  $("germinationDate").value = "2026-10-09";
  $("floweringDate").value = "2026-10-15";
  input();
  render(initial);
  assert.equal($("germinationDate").value, "2026-10-09", "poll must preserve draft");
  const saving = submit();
  assert.equal($("growDatesSave").disabled, true);
  assert.equal($("germinationDate").disabled, true);
  assert.equal(request.url, "/grow/dates");
  assert.equal(request.options.method, "POST");
  assert.equal(request.options.body.toString(), "germination=2026-10-09&flowering=2026-10-15");
  pending.resolve({ok:true, json:async () => ({
    ...initial, germinationDate:"2026-10-09", germinationDays:9,
    floweringDate:"2026-10-15", floweringDays:3
  })});
  await saving;
  assert.match($("growthRemaining").textContent, /21 jours restants theoriques/);
  assert.match($("floweringAge").textContent, /J\+3.*15\/10\/2026/);
  assert.match($("floweringForecast").textContent, /08\/11\/2026/, "forecast follows saved germination, not actual flowering date");
  assert.equal($("floweringRemaining").textContent,
    "Floraison : 57 jours restants theoriques · 60 jours prevus depuis le passage en floraison");
  assert.equal($("growDatesSave").disabled, false);
  $("floweringDate").value = "2026-10-07";
  input();
  await submit();
  assert.match($("floweringDate").validity, /preceder/);
  assert.equal($("floweringDate").reported, true);
  $("floweringDate").value = "";
  input();
  const failing = submit();
  assert.equal(request.options.body.get("flowering"), "");
  pending.resolve({ok:false, json:async () => ({error:"Stockage plein"})});
  await failing;
  assert.match($("growDatesMessage").textContent, /Echec.*Stockage plein/);
  render(initial);
  assert.equal($("germinationDate").value, "2026-10-09", "failure keeps draft");
  assert.equal($("floweringDate").value, "");
  const offline = submit();
  pending.reject(new Error("Network error"));
  await offline;
  assert.match($("growDatesMessage").textContent, /Network error/);
  render({...initial, clockReady:false, germinationDays:null});
  assert.match($("germinationAge").textContent, /Heure indisponible/);
  assert.match($("growthRemaining").textContent, /heure indisponible/);
  assert.match($("floweringForecast").textContent, /07\/11\/2026/, "fixed calendar forecast does not require a synced clock");
  render({...initial, germinationDays:null});
  assert.match($("germinationAge").textContent, /Date a venir/);
  assert.match($("growthRemaining").textContent, /germination a venir/);
  render({...initial, growDatesAvailable:false});
  assert.equal($("growDatesSave").disabled, true);
  assert.match($("growDatesMessage").textContent, /Erreur de stockage/);
  assert.match($("growthRemaining").textContent, /dates non disponibles/);
  assert.match($("floweringRemaining").textContent, /dates non disponibles/);
  assert.match($("floweringForecast").textContent, /dates non disponibles/);
  for (const [germinationDate, expected] of [
    ["2026-10-08","07/11/2026"],
    ["2026-12-15","14/01/2027"],
    ["2028-02-01","02/03/2028"],
    ["2026-02-01","03/03/2026"],
    ["2026-03-15","14/04/2026"]
  ]) {
    render({...initial, germinationDate});
    assert.ok($("floweringForecast").textContent.includes(expected), `calendar addition for ${germinationDate}`);
    assert.equal(context.data.floweringDate, "", "forecast must not fill in real flowering date");
  }
  for (const germinationDate of ["", "2026-02-30", "bad-date"]) {
    render({...initial, germinationDate, growDatesAvailable:true});
    assert.match($("floweringForecast").textContent, /absente ou invalide/);
  }
  const flowering = {...initial, floweringDate:"2026-10-15", floweringDays:3};
  render({...flowering, clockReady:false, floweringDays:null});
  assert.match($("floweringRemaining").textContent, /heure indisponible/);
  render({...flowering, floweringDays:null});
  assert.match($("floweringRemaining").textContent, /floraison a venir/);
  render({...flowering, floweringDays:-1});
  assert.match($("floweringRemaining").textContent, /floraison a venir/);
  for (const [days, remaining] of [[0,60],[3,57],[59,1],[60,0],[75,0]]) {
    render({...flowering, floweringDays:days});
    assert.match($("floweringAge").textContent, new RegExp(`J\\+${days}`));
    assert.ok($("floweringRemaining").textContent.startsWith(
      `Floraison : ${remaining} jour${remaining === 1 ? "" : "s"} restant${remaining === 1 ? "" : "s"} theorique${remaining === 1 ? "" : "s"} ·`));
    assert.match($("growthRemaining").textContent, /20 jours restants/, "flowering uses its own date, not germination");
  }
  for (const [days, remaining] of [[0,30],[29,1],[30,0],[45,0]]) {
    render({...initial, germinationDays:days});
    assert.match($("germinationAge").textContent, new RegExp(`J\\+${days}`));
    assert.ok($("growthRemaining").textContent.startsWith(
      `Croissance : ${remaining} jour${remaining === 1 ? "" : "s"} restant${remaining === 1 ? "" : "s"} theorique${remaining === 1 ? "" : "s"} ·`));
  }
  const selectSeed = value => {
    $("trackedSeed").value = value;
    $("trackedSeed").handlers.change();
    assert.equal(stored.get("trackedSeedId"), value);
  };
  render(flowering);
  selectSeed("1");
  assert.match($("growthRemaining").textContent, /non renseignee/);
  assert.match($("floweringRemaining").textContent, /Duree de floraison non renseignee/);
  assert.match($("floweringForecast").textContent, /duree de croissance non renseignee/);
  selectSeed("2");
  assert.match($("growthRemaining").textContent, /0 jours restants theoriques/);
  assert.match($("floweringRemaining").textContent, /0 jours restants theoriques/);
  assert.match($("floweringForecast").textContent, /08\/10\/2026/);
  selectSeed("");
  assert.match($("growthRemaining").textContent, /Choisir une graine/);
  assert.match($("floweringRemaining").textContent, /Choisir une graine/);
  assert.match($("floweringForecast").textContent, /choisir une graine/);
  selectSeed("0");
  assert.match($("growthRemaining").textContent, /20 jours restants/);
  assert.match($("floweringRemaining").textContent, /57 jours restants/);
  const reload = () => $("reloadTrackedSeeds").handlers.click();
  seedResponse = new Error("Network error");
  await reload();
  assert.match($("trackedSeedMessage").textContent, /Network error/);
  assert.match($("growthRemaining").textContent, /fiches non chargees/);
  assert.match($("floweringRemaining").textContent, /fiches non chargees/);
  assert.match($("floweringForecast").textContent, /fiches non chargees/);
  assert.equal($("trackedSeed").disabled, true);
  assert.equal($("reloadTrackedSeeds").disabled, false);
  seedResponse = {ok:false, error:"Stockage indisponible"};
  await reload();
  assert.match($("trackedSeedMessage").textContent, /Stockage indisponible/);
  seedResponse = {ok:true, entries:[{id:0,name:"Mise a jour",growthDays:"40",floweringDays:"70"}]};
  await reload();
  assert.equal($("trackedSeed").disabled, false);
  assert.match($("growthRemaining").textContent, /30 jours restants/);
  assert.match($("floweringRemaining").textContent, /67 jours restants/);
  assert.match($("floweringForecast").textContent, /17\/11\/2026/, "Purple Bud: October 8 + 40 days");
  seedResponse.entries[0].floweringDays = undefined;
  await reload();
  assert.match($("floweringRemaining").textContent, /Duree de floraison non renseignee/);
  seedResponse = {ok:true, entries:[]};
  await reload();
  assert.equal($("trackedSeed").value, "");
  assert.match($("growthRemaining").textContent, /Choisir une graine/);
  assert.match($("floweringRemaining").textContent, /Choisir une graine/);
  assert.match($("trackedSeedMessage").textContent, /Aucune fiche/);
  console.log("PASS: flowering forecast calendar arithmetic, leap/year/DST boundaries, Purple Bud November 17, actual dates preserved, growth/flowering remaining days, J+ counters, selection and explicit failures; identical backup");
})().catch(error => { console.error(error); process.exitCode = 1; });

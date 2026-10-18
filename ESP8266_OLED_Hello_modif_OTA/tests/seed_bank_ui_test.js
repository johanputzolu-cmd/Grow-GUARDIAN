const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");
const {TextEncoder,TextDecoder} = require("node:util");
const root = path.join(__dirname, "..");
const ui = fs.readFileSync(path.join(root, "SeedBankUi.h"), "utf8");
for (const filename of ["SeedBank.h", "SeedBankUi.h"]) {
  assert.equal(fs.readFileSync(path.join(root, filename), "utf8"),
    fs.readFileSync(path.join(root, "..", "ESP8266_OLED_Hello", filename), "utf8"));
}
class Element {
  constructor() { this.value = ""; this.children = []; this.disabled = false; this.handlers = {}; }
  append(child) { this.children.push(child); }
  replaceChildren() { this.children = []; }
  addEventListener(event, handler) { this.handlers[event] = handler; }
  scrollIntoView() {}
}
const elements = {};
const element = id => elements[id] || (elements[id] = new Element());
const form = element("editor");
form.elements = Object.fromEntries(["name","notes","growthDays","floweringDays"].map(name=>[name,new Element()]));
form.reset = () => Object.values(form.elements).forEach(el=>el.value = "");
const queue = [];
const requests = [];
let confirmResult = true;
const context = vm.createContext({
  document:{getElementById:element,createElement:()=>new Element(),
    querySelectorAll:()=>[...Object.values(elements),...Object.values(form.elements)]},
  URLSearchParams, TextEncoder, TextDecoder,
  confirm:()=>confirmResult,
  fetch:async (url, options) => {
    requests.push({url, options});
    if (!queue.length) throw new Error("Unexpected request");
    const next = queue.shift();
    if (next instanceof Error) throw next;
    return {ok:next.ok !== false,json:async()=>next.data};
  }
});
const empty = {capacity:8,entries:[]};
const seed = {id:0,name:'Test <b>"seed"</b>',supplier:"Supplier",lot:"Lot A",
  url:"https://example.org/seed",notes:"Line 1\nLine 2",growthDays:"",floweringDays:""};
const settle = () => new Promise(resolve=>setImmediate(resolve));
const submit = () => form.handlers.submit({preventDefault(){}});

(async () => {
  queue.push({data:empty});
  vm.runInContext(ui.match(/<script>([\s\S]*?)<\/script>/)[1], context);
  await settle();
  assert.equal(element("save").disabled,false);
  assert.match(element("entries").textContent,/Aucune fiche/);
  Object.entries(seed).filter(([key])=>key in form.elements).forEach(([key,value])=>form.elements[key].value = value);
  queue.push({data:{capacity:8,entries:[seed]}});
  await submit();
  assert.equal(requests.at(-1).url,"/seed-bank/api");
  assert.equal(requests.at(-1).options.body.get("id"),"0");
  assert.equal(requests.at(-1).options.body.get("action"),"save");
  assert.equal(requests.at(-1).options.body.get("growthDays"),"");
  for (const removed of ["supplier","lot","url"]) {
    assert.equal(requests.at(-1).options.body.get(removed),"");
    assert.ok(!ui.includes(`name="${removed}"`));
  }
  const card = element("entries").children[0];
  assert.equal(card.children[0].textContent,seed.name, "untrusted names are literal text");
  assert.match(card.children[1].textContent,/Variete/);
  card.children[2].onclick();
  assert.equal(form.elements.name.value,seed.name);
  form.elements.notes.value = "Updated";
  queue.push({ok:false,data:{error:"Storage failed"}});
  await submit();
  assert.equal(form.elements.notes.value,"Updated");
  assert.match(element("status").textContent,/Storage failed/);
  const count = requests.length;
  form.elements.growthDays.value = "-1";
  await submit();
  assert.equal(requests.length,count);
  assert.match(element("status").textContent,/Duree indicative/);
  form.elements.growthDays.value = "";
  form.elements.name.value = "é".repeat(33);
  await submit();
  assert.equal(requests.length,count);
  assert.match(element("status").textContent,/octets/);
  form.elements.name.value = seed.name;
  queue.push(new Error("Offline"));
  await submit();
  assert.match(element("status").textContent,/Offline/);
  confirmResult = false;
  await card.children[3].onclick();
  assert.equal(requests.length,count+1);
  confirmResult = true;
  queue.push({data:empty});
  await card.children[3].onclick();
  assert.equal(requests.at(-1).options.body.get("action"),"delete");
  assert.equal(form.elements.name.value,"");
  context.records = [seed,{...seed,name:"=SUM(A1:A2)",notes:"'quoted; \r\nnotes",growthDays:"0",floweringDays:"9999"}];
  const roundtrip = vm.runInContext("JSON.stringify(parseCsv(makeCsv(records)))",context);
  assert.deepEqual(JSON.parse(roundtrip),context.records.map(({name,notes,growthDays,floweringDays})=>({name,notes,growthDays,floweringDays})));
  assert.match(vm.runInContext("makeCsv(records)",context), /"'=SUM/);
  const parse = text => {
    context.csvText = text;
    return JSON.parse(vm.runInContext("JSON.stringify(parseCsv(csvText))",context));
  };
  assert.deepEqual(parse("\ufeffnom;fournisseur;lot;url;notes\r\n"),[]);
  assert.equal(parse('nom,fournisseur,lot,url,notes\n"Test, seed",,,,')[0].name,"Test, seed");
  assert.equal(parse('nom;fournisseur;lot;url;notes\nTest;;;;')[0].growthDays,"");
  for (const value of ["-1","1.5","10000","abc","1e2"]) {
    assert.throws(()=>parse("nom;fournisseur;lot;url;notes;croissance_jours;floraison_jours\nTest;;;;;"+value+";"));
  }
  assert.equal(parse("nom;fournisseur;lot;url;notes;croissance_jours;floraison_jours\nTest;;;;;0;")[0].growthDays,"0");
  for (const bad of [
    'nom;fournisseur;lot;url;notes\n"Unclosed;;;;',
    'nom;fournisseur;lot;url;notes\n"x"bad;;;;',
    'wrong;headers',
    'nom;fournisseur;lot;url;notes\nx;short',
    'nom;fournisseur;lot;url;notes\n;;;;notes',
    'nom;fournisseur;lot;url;notes\n' + Array(9).fill("x;;;;").join("\n"),
    'nom;fournisseur;lot;url;notes\n' + "é".repeat(33) + ";;;;",
    'nom;fournisseur;lot;url;notes\nx;;;;\0'
  ]) assert.throws(()=>parse(bad));
  assert.equal(parse("nom;variete;croissance_jours;floraison_jours\nTest;Cerise;0;")[0].notes,"Cerise");
  assert.equal(parse("nom;fournisseur;lot;url;notes;croissance_jours;floraison_jours\nTest;Old supplier;Old lot;https://example.org;Variete;2;3")[0].floweringDays,"3");
  const csvBytes = new TextEncoder().encode("nom;fournisseur;lot;url;notes\r\nTest;Supplier;;https://example.org;Notes\r\n");
  element("importCsv").files = [{size:csvBytes.length,arrayBuffer:async()=>csvBytes.buffer}];
  const beforePreview = requests.length;
  await element("importCsv").onchange();
  assert.match(element("csvPreview").textContent,/1 fiche.*\n1. Test/);
  assert.equal(requests.length,beforePreview,"preview must not write");
  assert.equal(element("applyCsv").disabled,false);
  element("importCsv").files = [{size:16385}];
  await element("importCsv").onchange();
  assert.match(element("csvPreview").textContent,/16 Ko/);
  assert.equal(element("applyCsv").disabled,true);
  element("importCsv").files = [{size:1,arrayBuffer:async()=>new Uint8Array([255]).buffer}];
  await element("importCsv").onchange();
  assert.match(element("csvPreview").textContent,/Import refuse/);
  assert.equal(element("applyCsv").disabled,true);
  vm.runInContext("csvDraft = [records[0]]",context);
  for (let i=0;i<8;i++) queue.push({data:{capacity:8,entries:[seed]}});
  const before = requests.length;
  await element("applyCsv").onclick();
  assert.equal(requests.length-before,8);
  assert.equal(requests[before].options.body.get("action"),"save");
  assert.equal(requests[before+1].options.body.get("action"),"delete");
  assert.match(element("status").textContent,/importee/);
  vm.runInContext("csvDraft = [records[0]]",context);
  queue.push({data:{capacity:8,entries:[seed]}},new Error("Connection lost"));
  await element("applyCsv").onclick();
  assert.match(element("status").textContent,/1\/8.*Connection lost.*partiel/);
  assert.equal(element("applyCsv").disabled,true);
  assert.equal(element("save").disabled,true);
  console.log("PASS: CSV roundtrip, BOM, delimiters, quotes, multiline, formula protection, invalid inputs, replacement and partial-import errors");
  console.log("PASS: Seed Bank list/add/edit/delete, confirmation, safe text/links, payloads, UTF-8 limits, draft retention on storage/network error, identical variants");
})().catch(error=>{ console.error(error); process.exitCode = 1; });

#pragma once

const char SEED_BANK_PAGE[] PROGMEM = R"seedbank(
<!doctype html><html lang="fr"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>SEED BANK</title>
<style>
:root{color-scheme:dark}body{margin:0;background:#07140b;color:#e6f1e9;font:15px system-ui}
main{max-width:800px;margin:auto;padding:20px}a{color:#a1f778}
article,form{border:1px solid #8cff4544;border-radius:16px;padding:16px;margin:16px 0;background:#102318}
label{display:grid;gap:6px;margin:12px 0}input,textarea,button{box-sizing:border-box;width:100%;padding:10px;border:1px solid #8cff4544;border-radius:8px;background:#07140b;color:inherit;font:inherit}
button{cursor:pointer;margin-top:8px}button:disabled{opacity:.5;cursor:wait}p{white-space:pre-wrap;overflow-wrap:anywhere}h2{overflow-wrap:anywhere}textarea{min-height:100px}
</style></head><body><main>
<a href="/">Retour au tableau</a><h1>SEED BANK</h1>
<p id="status" role="status">Chargement...</p><button id="reload" type="button">Actualiser la liste</button>
<article>
<h2>Partager le catalogue CSV</h2>
<button id="exportCsv" type="button" disabled>Exporter le catalogue CSV</button>
<button id="templateCsv" type="button">Telecharger le modele vide</button>
<label>Importer le fichier rempli<input id="importCsv" type="file" accept=".csv,text/csv"></label>
<p id="csvPreview" role="status">Choisir un CSV pour afficher un apercu. Aucun changement avant confirmation.</p>
<button id="applyCsv" type="button" disabled>Remplacer le catalogue par cet import</button>
</article>
<div id="entries"></div>
<form id="editor">
<h2 id="editorTitle">Nouvelle fiche</h2>
<label>Nom (64 octets)<input name="name" maxlength="64" required></label>
<label>Variete (256 octets)<textarea name="notes" maxlength="256"></textarea></label>
<label>Temps de croissance indicatif (jours)<input name="growthDays" type="number" min="0" max="9999" step="1"></label>
<label>Temps de floraison indicatif (jours)<input name="floweringDays" type="number" min="0" max="9999" step="1"></label>
<p>Facultatif : valeurs saisies par vos soins, a titre indicatif uniquement. Dans Phase suivie, choisir une graine pour afficher les jours restants theoriques : croissance depuis la germination, floraison depuis la date de passage en floraison, en plus des compteurs J+. Le passage en floraison previsionnel correspond a la date de germination + la duree de croissance. Cette estimation ne modifie pas la date reelle enregistree. Aucun changement automatique de phase.</p>
<button id="save" type="submit" disabled>Enregistrer la fiche</button>
<button id="cancel" type="button">Nouvelle fiche / annuler la modification</button>
</form></main><script>
const $ = id => document.getElementById(id);
const form = $("editor");
const names = ["name","notes","growthDays","floweringDays"];
const limits = [64,256,4,4];
const savePayload = id => new URLSearchParams({id:String(id),action:"save",supplier:"",lot:"",url:""});
const validDays = value => value === "" || /^\d{1,4}$/.test(value);
let entries = [], capacity = 8, editing = null, busy = false, loaded = false;
let csvDraft = null;
const message = text => $("status").textContent = text;
const setBusy = value => {
  busy = value;
  document.querySelectorAll("button,input,textarea").forEach(el => el.disabled = value);
  $("save").disabled = value || !loaded;
  $("exportCsv").disabled = value || !loaded;
  $("applyCsv").disabled = value || !loaded || csvDraft === null;
};
const reset = () => { form.reset(); editing = null; $("editorTitle").textContent = "Nouvelle fiche"; };
const render = data => {
  entries = data.entries; capacity = data.capacity; loaded = true;
  $("entries").replaceChildren();
  for (const entry of entries) {
    const card = document.createElement("article");
    const title = document.createElement("h2"); title.textContent = entry.name; card.append(title);
    for (const [label,key] of [["Variete","notes"]]) {
      if (entry[key]) { const p = document.createElement("p"); p.textContent = `${label} : ${entry[key]}`; card.append(p); }
    }
    for (const [label,key] of [["Croissance indicative","growthDays"],["Floraison indicative","floweringDays"]]) {
      if (entry[key] !== "" && entry[key] !== undefined) {
        const p = document.createElement("p"); p.textContent = `${label} : ${entry[key]} jours`; card.append(p);
      }
    }
    const edit = document.createElement("button"); edit.textContent = "Modifier"; edit.type = "button";
    edit.onclick = () => {
      editing = entry.id; names.forEach(name => form.elements[name].value = entry[name]);
      $("editorTitle").textContent = `Modifier : ${entry.name}`; form.scrollIntoView({behavior:"smooth"});
    }; card.append(edit);
    const remove = document.createElement("button"); remove.textContent = "Supprimer"; remove.type = "button";
    remove.onclick = async () => {
      if (busy || !confirm(`Supprimer la fiche "${entry.name}" ?`)) return;
      await mutate(new URLSearchParams({id:String(entry.id),action:"delete"}), entry.id);
    }; card.append(remove); $("entries").append(card);
  }
  if (!entries.length) $("entries").textContent = "Aucune fiche. Ajoute ta premiere graine ci-dessous.";
};
const request = async options => {
  const response = await fetch("/seed-bank/api", options);
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
  return data;
};
const mutate = async (body, deleted = null) => {
  setBusy(true); message("Enregistrement...");
  try {
    render(await request({method:"POST",body}));
    if (deleted === null || deleted === editing) reset();
    message(`${entries.length}/${capacity} fiches. Modification sauvegardee sur l'ESP.`);
  } catch (error) {
    message(`Echec : ${error.message}. Brouillon conserve ; actualise la liste avant de reessayer.`);
  } finally { setBusy(false); }
};
form.addEventListener("submit", async event => {
  event.preventDefault(); if (busy || !loaded) return;
  let id = editing;
  if (id === null) id = Array.from({length:capacity},(_,i)=>i).find(i=>!entries.some(entry=>entry.id === i));
  if (id === undefined) { message("Catalogue plein : supprime une fiche pour en ajouter une."); return; }
  const body = savePayload(id);
  for (let i = 0; i < names.length; i++) {
    const value = form.elements[names[i]].value.trim();
    if (new TextEncoder().encode(value).length > limits[i]) {
      message(`Champ ${names[i]} trop long : ${limits[i]} octets UTF-8 maximum.`); return;
    }
    if (names[i] === "name" && !value) { message("Le nom est requis."); return; }
    if (i >= 2 && !validDays(value)) { message("Duree indicative : entier de 0 a 9999 jours ou vide."); return; }
    body.set(names[i],value);
  }
  await mutate(body);
});
$("cancel").onclick = reset;
const load = async () => {
  if (busy) return;
  setBusy(true);
  try { render(await request({cache:"no-store"})); message(`${entries.length}/${capacity} fiches sauvegardees sur l'ESP.`); }
  catch (error) { message(`Chargement impossible : ${error.message}`); }
  finally { setBusy(false); }
};
$("reload").onclick = load;
const csvHeaders = ["nom","variete","croissance_jours","floraison_jours"];
const oldCsvHeaders = ["nom","fournisseur","lot","url","notes","croissance_jours","floraison_jours"];
const csvEscape = value => {
  // Neutralize spreadsheet formulas; the paired importer reverses this prefix.
  const safe = /^[\s]*[=+\-@']/.test(value) ? "'" + value : value;
  return '"' + safe.replace(/"/g,'""') + '"';
};
const makeCsv = records => "\ufeff" + [
  csvHeaders.join(";"),
  ...records.map(record => names.map(name=>csvEscape(record[name])).join(";"))
].join("\r\n") + "\r\n";
const parseCsv = text => {
  text = text.replace(/^\ufeff/,"");
  const header = text.split(/\r?\n/,1)[0];
  const delimiter = header.includes(";") ? ";" : ",";
  const rows = [];
  let row = [], cell = "", quoted = false, closed = false;
  const pushCell = () => { row.push(cell); cell = ""; closed = false; };
  const pushRow = () => { pushCell(); if (row.some(value=>value.trim())) rows.push(row); row = []; };
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (quoted) {
      if (c === '"' && text[i+1] === '"') { cell += '"'; i++; }
      else if (c === '"') { quoted = false; closed = true; }
      else cell += c;
    } else if (c === '"' && !cell && !closed) quoted = true;
    else if (c === delimiter) pushCell();
    else if (c === "\n" || c === "\r") { if (c === "\r" && text[i+1] === "\n") i++; pushRow(); }
    else {
      if (closed || c === '"') throw new Error("Guillemets CSV invalides");
      cell += c;
    }
  }
  if (quoted) throw new Error("Guillemet CSV non ferme");
  if (cell || row.length || closed) pushRow();
  const headers = rows.length ? rows.shift().map(v=>v.trim().toLowerCase()).join(";") : "";
  const legacyColumns = headers === oldCsvHeaders.slice(0,5).join(";") ? 5
    : headers === oldCsvHeaders.join(";") ? 7 : 0;
  if (!legacyColumns && headers !== csvHeaders.join(";"))
    throw new Error(`Colonnes attendues : ${csvHeaders.join(";")}`);
  if (rows.length > capacity) throw new Error(`${capacity} fiches maximum`);
  return rows.map((values,index) => {
    if (values.length !== (legacyColumns || names.length)) throw new Error(`Fiche ${index+1} : ${legacyColumns || names.length} colonnes requises`);
    if (legacyColumns) values = [values[0],values[4],values[5] || "",values[6] || ""];
    const record = {};
    values.forEach((raw,i) => {
      const value = raw.startsWith("'") && /^[\s]*[=+\-@']/.test(raw.slice(1)) ? raw.slice(1) : raw;
      if (new TextEncoder().encode(value).length > limits[i] || value.includes("\0"))
        throw new Error(`Fiche ${index+1}, ${csvHeaders[i]} : valeur invalide ou trop longue (${limits[i]} octets)`);
      if (i >= 2 && !validDays(value)) throw new Error(`Fiche ${index+1} : duree entiere de 0 a 9999 jours ou vide`);
      record[names[i]] = value;
    });
    if (!record.name.trim()) throw new Error(`Fiche ${index+1} : nom requis`);
    return record;
  });
};
const downloadCsv = (records,filename) => {
  const url = URL.createObjectURL(new Blob([makeCsv(records)],{type:"text/csv;charset=utf-8"}));
  const link = document.createElement("a"); link.href = url; link.download = filename;
  document.body.append(link); link.click(); link.remove();
  setTimeout(()=>URL.revokeObjectURL(url),1000);
};
$("exportCsv").onclick = async () => {
  if (busy) return;
  setBusy(true);
  try {
    render(await request({cache:"no-store"}));
    downloadCsv(entries,"seed-bank.csv");
    message("");
  } catch (error) { message(`Export impossible : ${error.message}`); }
  finally { setBusy(false); }
};
$("templateCsv").onclick = () => downloadCsv([],"seed-bank-modele.csv");
$("importCsv").onchange = async () => {
  if (busy) return;
  csvDraft = null;
  const file = $("importCsv").files[0];
  if (!file) { $("csvPreview").textContent = "Aucun fichier selectionne."; setBusy(false); return; }
  setBusy(true);
  try {
    if (file.size > 16384) throw new Error("Fichier trop volumineux (16 Ko maximum)");
    const text = new TextDecoder("utf-8",{fatal:true}).decode(await file.arrayBuffer());
    csvDraft = parseCsv(text);
    $("csvPreview").textContent = `${csvDraft.length} fiche(s) :\n` +
      csvDraft.map((record,i)=>`${i+1}. ${record.name} — ${record.notes}`).join("\n") +
      (csvDraft.length ? "" : "\nCatalogue vide : cet import effacera toutes les fiches.");
  } catch (error) { $("csvPreview").textContent = `Import refuse : ${error.message}`; }
  finally { setBusy(false); }
};
$("applyCsv").onclick = async () => {
  if (busy || !loaded || csvDraft === null ||
      !confirm(`Remplacer TOUT le catalogue par ${csvDraft.length} fiche(s) ? Sauvegarde CSV recommandee. Un import interrompu peut etre partiel.`)) return;
  setBusy(true);
  let completed = 0;
  try {
    for (let id = 0; id < capacity; id++) {
      const body = id < csvDraft.length ? savePayload(id) : new URLSearchParams({id:String(id),action:"delete"});
      if (id < csvDraft.length) names.forEach(name=>body.set(name,csvDraft[id][name]));
      render(await request({method:"POST",body}));
      completed++;
    }
    reset(); csvDraft = null; $("importCsv").value = "";
    $("csvPreview").textContent = "Import termine.";
    message(`${entries.length} fiche(s) importee(s) et sauvegardee(s) sur l'ESP.`);
  } catch (error) {
    loaded = false;
    message(`Import interrompu apres ${completed}/${capacity} operations confirmees : ${error.message}. Catalogue potentiellement partiel ; cliquer Actualiser la liste avant de reessayer.`);
  } finally { setBusy(false); }
};
load();
</script></body></html>
)seedbank";

void handleSeedBankPage() {
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send_P(200, "text/html; charset=utf-8", SEED_BANK_PAGE);
}

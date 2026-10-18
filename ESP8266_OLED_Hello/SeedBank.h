#pragma once

struct SeedRecord {
  char name[65];
  char supplier[65];
  char lot[49];
  char url[193];
  char notes[257];
  char growthDays[5];
  char floweringDays[5];
};

static_assert(offsetof(SeedRecord, growthDays) == 629, "Preserve legacy seed record layout");

const uint8_t SEED_BANK_CAPACITY = 8;
bool seedBankReady = false;

bool validSeedDays(const char* value) {
  if (memchr(value, '\0', 5) == nullptr) return false;
  for (const char* p = value; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
  }
  return true;
}

bool validSeedRecord(const SeedRecord& record) {
  const char* fields[] = {record.name, record.supplier, record.lot, record.url, record.notes};
  const size_t sizes[] = {sizeof(record.name), sizeof(record.supplier), sizeof(record.lot),
                          sizeof(record.url), sizeof(record.notes)};
  for (size_t i = 0; i < 5; i++) {
    if (memchr(fields[i], '\0', sizes[i]) == nullptr) return false;
  }
  if (record.name[0] == '\0') return false;
  if (!validSeedDays(record.growthDays) || !validSeedDays(record.floweringDays)) return false;
  const String url(record.url);
  return url.length() == 0 || url.startsWith("https://") || url.startsWith("http://");
}

String seedRecordPath(uint8_t id) {
  return String(F("/seed-")) + id + F(".bin");
}

bool readSeedRecord(const String& path, SeedRecord& record) {
  File file = LittleFS.open(path, "r");
  record = {};
  const size_t legacySize = offsetof(SeedRecord, growthDays);
  const size_t size = file ? file.size() : 0;
  const bool valid = file && (size == sizeof(record) || size == legacySize) &&
      file.read(reinterpret_cast<uint8_t*>(&record), size) == size &&
      validSeedRecord(record);
  if (file) file.close();
  if (!valid) Serial.println(F("Seed bank record unreadable or invalid"));
  return valid;
}

bool saveSeedRecord(uint8_t id, const SeedRecord& record) {
  File file = LittleFS.open("/seed.tmp", "w");
  if (!file) {
    Serial.println(F("Seed bank temporary file unavailable"));
    return false;
  }
  const bool written = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
  file.close();
  SeedRecord check = {};
  if (!written || !readSeedRecord("/seed.tmp", check) ||
      memcmp(&check, &record, sizeof(record)) != 0 ||
      !LittleFS.rename("/seed.tmp", seedRecordPath(id))) {
    Serial.println(F("Seed bank save failed; previous record retained"));
    return false;
  }
  return true;
}

String seedJsonString(const char* value) {
  String result = "\"";
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(value); *p; ++p) {
    if (*p == '"' || *p == '\\') {
      result += '\\';
      result += static_cast<char>(*p);
    } else if (*p < 32) {
      char escaped[7];
      snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned int>(*p));
      result += escaped;
    } else {
      result += static_cast<char>(*p);
    }
  }
  result += '"';
  return result;
}

void handleSeedBank() {
  if (!seedBankReady) {
    server.send(503, F("application/json"), F("{\"error\":\"Stockage SEED BANK indisponible\"}"));
    return;
  }
  String json = F("{\"capacity\":8,\"entries\":[");
  bool first = true;
  for (uint8_t id = 0; id < SEED_BANK_CAPACITY; id++) {
    const String path = seedRecordPath(id);
    if (!LittleFS.exists(path)) continue;
    SeedRecord record = {};
    if (!readSeedRecord(path, record)) {
      server.send(500, F("application/json"), F("{\"error\":\"Fiche SEED BANK illisible; aucune fiche effacee\"}"));
      return;
    }
    if (!first) json += ',';
    first = false;
    json += F("{\"id\":");
    json += id;
    json += F(",\"name\":");
    json += seedJsonString(record.name);
    json += F(",\"supplier\":");
    json += seedJsonString(record.supplier);
    json += F(",\"lot\":");
    json += seedJsonString(record.lot);
    json += F(",\"url\":");
    json += seedJsonString(record.url);
    json += F(",\"notes\":");
    json += seedJsonString(record.notes);
    json += F(",\"growthDays\":");
    json += seedJsonString(record.growthDays);
    json += F(",\"floweringDays\":");
    json += seedJsonString(record.floweringDays);
    json += '}';
  }
  json += F("]}");
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), json);
}

void handleSeedBankSave() {
  if (!seedBankReady) {
    server.send(503, F("application/json"), F("{\"error\":\"Stockage SEED BANK indisponible\"}"));
    return;
  }
  const String idText = server.arg("id");
  if (idText.length() != 1 || idText[0] < '0' || idText[0] >= '0' + SEED_BANK_CAPACITY) {
    server.send(400, F("application/json"), F("{\"error\":\"Numero de fiche invalide\"}"));
    return;
  }
  const uint8_t id = idText[0] - '0';
  const String action = server.arg("action");
  if (action == "delete") {
    const String path = seedRecordPath(id);
    if (LittleFS.exists(path) && !LittleFS.remove(path)) {
      Serial.println(F("Seed bank deletion failed"));
      server.send(500, F("application/json"), F("{\"error\":\"Suppression impossible\"}"));
      return;
    }
  } else if (action == "save") {
    SeedRecord record = {};
    char* fields[] = {record.name, record.supplier, record.lot, record.url, record.notes};
    const size_t sizes[] = {sizeof(record.name), sizeof(record.supplier), sizeof(record.lot),
                            sizeof(record.url), sizeof(record.notes)};
    const char* names[] = {"name", "supplier", "lot", "url", "notes"};
    for (size_t i = 0; i < 5; i++) {
      const String value = server.arg(names[i]);
      if (!server.hasArg(names[i]) || value.length() >= sizes[i] ||
          strlen(value.c_str()) != value.length()) {
        server.send(400, F("application/json"), F("{\"error\":\"Champ manquant ou trop long (limite en octets UTF-8)\"}"));
        return;
      }
      value.toCharArray(fields[i], sizes[i]);
    }
    char* daysFields[] = {record.growthDays, record.floweringDays};
    const char* daysNames[] = {"growthDays", "floweringDays"};
    for (size_t i = 0; i < 2; i++) {
      const String value = server.arg(daysNames[i]);
      if (value.length() > 4 || strlen(value.c_str()) != value.length()) {
        server.send(400, F("application/json"), F("{\"error\":\"Duree indicative : entier de 0 a 9999 jours ou vide\"}"));
        return;
      }
      value.toCharArray(daysFields[i], 5);
      if (!validSeedDays(daysFields[i])) {
        server.send(400, F("application/json"), F("{\"error\":\"Duree indicative : entier de 0 a 9999 jours ou vide\"}"));
        return;
      }
    }
    if (!validSeedRecord(record) || String(record.name).length() == 0) {
      server.send(400, F("application/json"), F("{\"error\":\"Nom requis et lien HTTP/HTTPS uniquement\"}"));
      return;
    }
    if (!saveSeedRecord(id, record)) {
      server.send(500, F("application/json"), F("{\"error\":\"Sauvegarde impossible; ancienne fiche conservee\"}"));
      return;
    }
  } else {
    server.send(400, F("application/json"), F("{\"error\":\"Action SEED BANK invalide\"}"));
    return;
  }
  handleSeedBank();
}

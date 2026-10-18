#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <LittleFS.h>
#include <time.h>
#include "Scheduler.h"
#include "Recovery.h"
#include "Condition.h"

#include "secrets.h"
const uint32_t SETTINGS_MAGIC = 0x524C5933;
const uint32_t LEGACY_SETTINGS_MAGIC = 0x524C5932;
const uint32_t MIN_VALID_TIMESTAMP = 1700000000UL;
const uint32_t RELAY_UART_BAUD = 115200;
const uint16_t EEPROM_SIZE = 256;
const uint32_t RECOVERY_INITIALIZED = 0x52535431;
const uint16_t RECOVERY_MARKER_OFFSET = 252;

enum ProgramMode : uint8_t {
  MODE_OFF = 0,
  MODE_ON = 1,
  MODE_PROGRAM = 2
};

enum ScheduleType : uint8_t {
  SCHEDULE_WINDOW = 0,
  SCHEDULE_CYCLE = 1
};

enum TaskType : uint8_t {
  TASK_LIGHT = 0,
  TASK_VENTILATION = 1,
  TASK_PUMP = 2,
  TASK_OPTION = 3
};

struct TaskProgram {
  uint8_t mode;
  uint8_t channel;
  uint8_t scheduleType;
  uint16_t durationMinute;
  uint16_t intervalMinute;
  uint16_t startMinute;
  uint16_t stopMinute;
};

struct LegacyRelaySettings {
  uint32_t magic;
  TaskProgram programs[4];
};

struct RelaySettings {
  uint32_t magic;
  WeeklyProgram programs[4];
};

struct ConditionStore {
  uint32_t magic;
  ConditionConfig config;
  uint32_t checksum;
};

const uint16_t CONDITION_OFFSET = sizeof(RelaySettings);
static_assert(sizeof(RelaySettings) + sizeof(ConditionStore) <= RECOVERY_MARKER_OFFSET,
              "Relay and condition settings exceed EEPROM allocation");

ESP8266WebServer server(80);
RelaySettings settings = {};
bool relayStates[4] = {};
bool relayStatesKnown[4] = {};
bool settingsSaved = true;
unsigned long lastWifiAttempt = 0;
uint32_t temporaryStart[4] = {};
uint32_t temporaryDuration[4] = {};
time_t transitionChecked[4] = {};
time_t nextTransition[4] = {};
uint32_t temporaryEnd[4] = {};
ConditionConfig conditionConfig = {0, 2, 1, 0, 0, 0, 0, 0, 280, 700, 5, 0};
ConditionSample conditionSample;
ConditionRuntime conditionRuntime;
bool conditionSaved = true;
uint32_t conditionEnd = 0;
bool conditionResumePending = false;
uint32_t lastRelayCommand[4] = {};
const uint32_t RELAY_REFRESH_MS = 5000;
RecoveryState savedRecovery = {};
bool recoveryReady = false;
bool recoverySaved = false;
bool recoveryRestored = false;
uint32_t lastRecoveryAttempt = 0;

const char* const TASK_NAMES[] = {"light", "ventilation", "pump", "option"};

bool saveSettings() {
  EEPROM.put(0, settings);
  settingsSaved = EEPROM.commit();
  return settingsSaved;
}

void loadCondition() {
  ConditionStore stored = {};
  EEPROM.get(CONDITION_OFFSET, stored);
  if (stored.magic != 0x434E4431) {
    conditionSaved = stored.magic == 0xFFFFFFFF || stored.magic == 0;
    return;
  }
  conditionSaved = validCondition(stored.config) &&
      stored.checksum == recoveryHash(&stored, offsetof(ConditionStore, checksum));
  if (conditionSaved) conditionConfig = stored.config;
}

bool saveCondition() {
  ConditionStore stored = {};
  stored.magic = 0x434E4431;
  stored.config = conditionConfig;
  stored.checksum = recoveryHash(&stored, offsetof(ConditionStore, checksum));
  EEPROM.put(CONDITION_OFFSET, stored);
  conditionSaved = EEPROM.commit();
  return conditionSaved;
}

void loadRecovery() {
  uint32_t marker = 0;
  EEPROM.get(RECOVERY_MARKER_OFFSET, marker);
  LittleFSConfig config;
  config.setAutoFormat(false);
  LittleFS.setConfig(config);
  recoveryReady = LittleFS.begin();
  // Initialize the previously unused filesystem once, never reformat saved recovery data.
  if (!recoveryReady && marker != RECOVERY_INITIALIZED) {
    recoveryReady = LittleFS.format() && LittleFS.begin();
  }
  if (!recoveryReady) return;
  if (marker != RECOVERY_INITIALIZED) {
    EEPROM.put(RECOVERY_MARKER_OFFSET, RECOVERY_INITIALIZED);
    if (!EEPROM.commit()) {
      settingsSaved = false;
      recoveryReady = false;
      return;
    }
  }
  if (!LittleFS.exists("/recovery.bin")) {
    recoverySaved = true;
    return;
  }
  File file = LittleFS.open("/recovery.bin", "r");
  RecoveryState state = {};
  if (!file) return;
  const uint32_t settingsHash = recoveryHash(&settings, sizeof(settings));
  if (file.size() == sizeof(LegacyRecoveryState)) {
    LegacyRecoveryState legacy = {};
    if (file.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy)) != sizeof(legacy) ||
        !validLegacyRecovery(legacy, settingsHash)) return;
    state.magic = 0x52535432;
    state.settingsHash = settingsHash;
    state.relayMask = legacy.relayMask;
    for (uint8_t i = 0; i < 4; i++) state.temporaryEnd[i] = legacy.temporaryEnd[i];
  } else if (file.size() != sizeof(state) ||
             file.read(reinterpret_cast<uint8_t*>(&state), sizeof(state)) != sizeof(state) ||
             !validRecovery(state, settingsHash)) {
    return;
  }
  savedRecovery = state;
  if (state.conditionHash == recoveryHash(&conditionConfig, sizeof(conditionConfig)) &&
      conditionConfig.enabled) {
    conditionEnd = state.conditionEnd;
    conditionResumePending = conditionConfig.action == 1 && conditionEnd != 0;
    conditionRuntime.matched = (state.conditionFlags & 1) != 0;
    conditionRuntime.temperatureHigh = (state.conditionFlags & 2) != 0;
    conditionRuntime.humidityHigh = (state.conditionFlags & 4) != 0;
  }
  for (uint8_t i = 0; i < 4; i++) {
    temporaryEnd[i] = state.temporaryEnd[i];
    relayStates[i] = (state.relayMask & (1 << i)) != 0;
  }
  recoverySaved = true;
  recoveryRestored = true;
}

bool persistRecovery(uint32_t relayMask) {
  RecoveryState state = {};
  state.magic = 0x52535432;
  state.settingsHash = recoveryHash(&settings, sizeof(settings));
  for (uint8_t i = 0; i < 4; i++) state.temporaryEnd[i] = temporaryEnd[i];
  state.relayMask = relayMask;
  state.conditionHash = recoveryHash(&conditionConfig, sizeof(conditionConfig));
  state.conditionEnd = conditionEnd;
  state.conditionFlags = (conditionRuntime.matched ? 1 : 0) |
      (conditionRuntime.temperatureHigh ? 2 : 0) | (conditionRuntime.humidityHigh ? 4 : 0);
  state.checksum = recoveryHash(&state, offsetof(RecoveryState, checksum));
  if (recoverySaved && memcmp(&state, &savedRecovery, sizeof(state)) == 0) return true;
  if (!recoveryReady) return recoverySaved = false;
  if (!recoverySaved && static_cast<uint32_t>(millis() - lastRecoveryAttempt) < 5000) return false;
  lastRecoveryAttempt = millis();
  File file = LittleFS.open("/recovery.tmp", "w");
  if (!file || file.write(reinterpret_cast<const uint8_t*>(&state), sizeof(state)) != sizeof(state)) {
    return recoverySaved = false;
  }
  file.close();
  File verify = LittleFS.open("/recovery.tmp", "r");
  RecoveryState readback = {};
  if (!verify || verify.size() != sizeof(readback) ||
      verify.read(reinterpret_cast<uint8_t*>(&readback), sizeof(readback)) != sizeof(readback) ||
      memcmp(&readback, &state, sizeof(state)) != 0) return recoverySaved = false;
  verify.close();
  if (!LittleFS.rename("/recovery.tmp", "/recovery.bin")) return recoverySaved = false;
  savedRecovery = state;
  return recoverySaved = true;
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, settings);
  bool valid = settings.magic == SETTINGS_MAGIC;
  for (uint8_t i = 0; i < 4 && valid; i++) {
    const WeeklyProgram& program = settings.programs[i];
    valid = program.mode <= MODE_PROGRAM &&
        program.channel >= 1 && program.channel <= 4;
    for (const ScheduleSlot& slot : program.slots) valid = valid && validSlot(slot);
    for (uint8_t j = 0; j < i && valid; j++) {
      valid = program.channel != settings.programs[j].channel;
    }
  }
  if (!valid) {
    LegacyRelaySettings legacy = {};
    EEPROM.get(0, legacy);
    bool migrate = legacy.magic == LEGACY_SETTINGS_MAGIC;
    for (uint8_t i = 0; i < 4 && migrate; i++) {
      const TaskProgram& old = legacy.programs[i];
      migrate = old.mode <= MODE_PROGRAM && old.channel >= 1 && old.channel <= 4 &&
          old.scheduleType <= SCHEDULE_CYCLE && old.startMinute < 1440 &&
          old.stopMinute < 1440 && old.durationMinute >= 1 &&
          old.durationMinute <= old.intervalMinute && old.intervalMinute <= 1440;
      for (uint8_t j = 0; j < i; j++) migrate = migrate && old.channel != legacy.programs[j].channel;
    }
    settings = {};
    settings.magic = SETTINGS_MAGIC;
    for (uint8_t i = 0; i < 4; i++) {
      WeeklyProgram& program = settings.programs[i];
      program.mode = migrate ? legacy.programs[i].mode :
          static_cast<uint8_t>(i == TASK_LIGHT ? MODE_PROGRAM : MODE_OFF);
      program.channel = migrate ? legacy.programs[i].channel : i + 1;
      for (uint8_t s = 0; s < 4; s++) {
        ScheduleSlot& slot = program.slots[s];
        slot.enabled = s == 0;
        slot.days = 127;
        slot.scheduleType = migrate ? legacy.programs[i].scheduleType :
            static_cast<uint8_t>(i == TASK_LIGHT ? SCHEDULE_WINDOW : SCHEDULE_CYCLE);
        slot.startMinute = migrate ? legacy.programs[i].startMinute : (i == TASK_LIGHT ? 360 : 0);
        slot.stopMinute = migrate ? legacy.programs[i].stopMinute : 0;
        slot.durationMinute = migrate ? legacy.programs[i].durationMinute : (i == TASK_PUMP ? 25 : 5);
        slot.intervalMinute = migrate ? legacy.programs[i].intervalMinute : (i == TASK_PUMP ? 120 : 60);
      }
    }
    saveSettings();
  }
}

bool parseUnsigned(const String& value, uint16_t minimum, uint16_t maximum, uint16_t& result) {
  if (value.length() == 0) {
    return false;
  }
  uint32_t parsed = 0;
  for (uint16_t i = 0; i < value.length(); i++) {
    if (!isDigit(value[i])) {
      return false;
    }
    parsed = parsed * 10 + static_cast<uint8_t>(value[i] - '0');
    if (parsed > maximum) {
      return false;
    }
  }
  if (parsed < minimum) {
    return false;
  }
  result = static_cast<uint16_t>(parsed);
  return true;
}

bool parseTime(const String& value, uint16_t& result) {
  if (value.length() != 5 || value[2] != ':' ||
      !isDigit(value[0]) || !isDigit(value[1]) ||
      !isDigit(value[3]) || !isDigit(value[4])) {
    return false;
  }

  const uint8_t hour = static_cast<uint8_t>((value[0] - '0') * 10 + value[1] - '0');
  const uint8_t minute = static_cast<uint8_t>((value[3] - '0') * 10 + value[4] - '0');
  if (hour > 23 || minute > 59) {
    return false;
  }

  result = static_cast<uint16_t>(hour * 60 + minute);
  return true;
}

String formatTime(uint16_t minuteOfDay) {
  char value[6];
  snprintf(value, sizeof(value), "%02u:%02u", minuteOfDay / 60, minuteOfDay % 60);
  return String(value);
}

bool scheduledProgramState(time_t now, const WeeklyProgram& program) {
  if (now < MIN_VALID_TIMESTAMP) {
    return false;
  }

  struct tm localTime = {};
  if (localtime_r(&now, &localTime) == nullptr) {
    return false;
  }

  return weeklyState(localTime, program);
}

time_t findNextTransition(time_t now, uint8_t index) {
  const WeeklyProgram& program = settings.programs[index];
  if (now < MIN_VALID_TIMESTAMP || program.mode != MODE_PROGRAM) return 0;
  if (transitionChecked[index] > 0 && now >= transitionChecked[index] &&
      now - transitionChecked[index] < 3600 &&
      (nextTransition[index] == 0 || now < nextTransition[index])) return nextTransition[index];
  const bool current = scheduledProgramState(now, program);
  nextTransition[index] = 0;
  // Scan real minutes across DST changes; eight days covers the next weekly window.
  const time_t first = now - now % 60 + 60;
  for (uint16_t minute = 0; minute < 8 * 24 * 60; minute++) {
    const time_t candidate = first + minute * 60UL;
    if (scheduledProgramState(candidate, program) != current) {
      nextTransition[index] = candidate;
      break;
    }
    if (minute % 64 == 0) yield();
  }
  transitionChecked[index] = now;
  return nextTransition[index];
}

void sendRelayCommand(uint8_t channel, bool enabled) {
  const uint8_t state = enabled ? 1 : 0;
  const uint8_t command[] = {
    0xA0,
    channel,
    state,
    static_cast<uint8_t>(0xA0 + channel + state)
  };
  Serial.write(command, sizeof(command));
  Serial.flush();
}

void updateRelay() {
  const time_t now = time(nullptr);
  bool desiredStates[4] = {};
  uint32_t mask = 0;
  bool conditionProgramMode = false;
  for (const WeeklyProgram& program : settings.programs) {
    if (program.channel == conditionConfig.channel) conditionProgramMode = program.mode == MODE_PROGRAM;
  }
  if (!conditionConfig.enabled || !conditionProgramMode) {
    conditionEnd = 0;
    conditionResumePending = false;
  }
  if (conditionResumePending) {
    conditionRuntime.fresh = conditionSampleFresh(conditionConfig, conditionSample, millis());
    conditionRuntime.demand = false;
    if (now >= MIN_VALID_TIMESTAMP && now >= conditionEnd) {
      conditionEnd = 0;
      conditionResumePending = false;
    } else if (conditionRuntime.fresh && now >= MIN_VALID_TIMESTAMP) {
      conditionRuntime.startedAt = millis();
      conditionRuntime.durationMs = (conditionEnd - static_cast<uint32_t>(now)) * 1000UL;
      conditionResumePending = false;
    } else if (!conditionRuntime.fresh && millis() >= 30000) {
      conditionEnd = 0;
      conditionResumePending = false;
    }
  }
  if (!conditionResumePending) {
    if (conditionConfig.enabled && conditionProgramMode && conditionConfig.action == 1 &&
        now < MIN_VALID_TIMESTAMP) {
      conditionRuntime.fresh = conditionSampleFresh(conditionConfig, conditionSample, millis());
      conditionRuntime.demand = false;
    } else {
      if (conditionEnd != 0 && now >= MIN_VALID_TIMESTAMP && now >= conditionEnd) conditionRuntime.durationMs = 0;
      const uint32_t previousDuration = conditionRuntime.durationMs;
      updateCondition(conditionConfig, conditionSample, millis(), conditionProgramMode, conditionRuntime);
      if (conditionRuntime.durationMs == 0) conditionEnd = 0;
      else if (previousDuration == 0) {
        conditionEnd = static_cast<uint32_t>(now) + conditionRuntime.durationMs / 1000;
      }
    }
  }
  for (uint8_t i = 0; i < 4; i++) {
    const WeeklyProgram& program = settings.programs[i];
    if (now >= MIN_VALID_TIMESTAMP && temporaryEnd[i] != 0) {
      if (now >= temporaryEnd[i]) {
        temporaryEnd[i] = 0;
        temporaryDuration[i] = 0;
      } else if (temporaryDuration[i] == 0) {
        temporaryStart[i] = millis();
        temporaryDuration[i] = static_cast<uint32_t>(temporaryEnd[i] - now) * 1000UL;
      }
    }
    const bool elapsed = temporaryDuration[i] != 0 &&
        !temporaryActive(millis(), temporaryStart[i], temporaryDuration[i]);
    if (elapsed) temporaryEnd[i] = 0;
    if (!temporaryActive(millis(), temporaryStart[i], temporaryDuration[i])) temporaryDuration[i] = 0;
    bool desiredState = false;
    if (temporaryActive(millis(), temporaryStart[i], temporaryDuration[i])) {
      desiredState = true;
    } else if (program.mode == MODE_ON) {
      desiredState = true;
    } else if (program.mode == MODE_PROGRAM) {
      desiredState = scheduledProgramState(now, program) ||
          (program.channel == conditionConfig.channel && conditionRuntime.demand);
    }
    const uint8_t relayIndex = program.channel - 1;
    if (now < MIN_VALID_TIMESTAMP && recoveryRestored &&
        (program.mode == MODE_PROGRAM || temporaryEnd[i] != 0)) {
      desiredState = relayStates[relayIndex];
      if (program.channel == conditionConfig.channel && conditionConfig.enabled &&
          temporaryEnd[i] == 0) {
        desiredState = conditionRuntime.demand;
      }
    }
    desiredStates[relayIndex] = desiredState;
    if (desiredState) mask |= 1 << relayIndex;
  }
  if (now >= MIN_VALID_TIMESTAMP) recoveryRestored = false;
  persistRecovery(mask);
  for (uint8_t relayIndex = 0; relayIndex < 4; relayIndex++) {
    const bool desiredState = desiredStates[relayIndex];
    // The relay controller has no acknowledgement and can miss frames during power-up.
    if (!relayStatesKnown[relayIndex] || desiredState != relayStates[relayIndex] ||
        static_cast<uint32_t>(millis() - lastRelayCommand[relayIndex]) >= RELAY_REFRESH_MS) {
      sendRelayCommand(relayIndex + 1, desiredState);
      lastRelayCommand[relayIndex] = millis();
      relayStates[relayIndex] = desiredState;
      relayStatesKnown[relayIndex] = true;
    }
  }
}

const char* modeName(uint8_t mode) {
  if (mode == MODE_ON) return "on";
  if (mode == MODE_PROGRAM) return "prog";
  return "off";
}

String statusJson() {
  const time_t now = time(nullptr);
  String response;
  response.reserve(3200);
  response = F("{\"wifi\":");
  response += WiFi.status() == WL_CONNECTED ? F("true") : F("false");
  response += F(",\"ip\":\"");
  response += WiFi.localIP().toString();
  response += F("\",\"clockReady\":");
  response += now >= MIN_VALID_TIMESTAMP ? F("true") : F("false");
  response += F(",\"timestamp\":");
  response += String(static_cast<uint32_t>(now));
  response += F(",\"localTime\":");
  struct tm localTime = {};
  char formattedTime[32];
  if (now >= MIN_VALID_TIMESTAMP &&
      localtime_r(&now, &localTime) != nullptr &&
      strftime(formattedTime, sizeof(formattedTime), "%Y-%m-%d %H:%M:%S %Z", &localTime) > 0) {
    response += '"';
    response += formattedTime;
    response += '"';
  } else {
    response += F("null");
  }
  response += F(",\"settingsSaved\":");
  response += settingsSaved ? F("true") : F("false");
  response += F(",\"recoverySaved\":");
  response += recoverySaved ? F("true") : F("false");
  response += F(",\"restoringOutputs\":");
  response += recoveryRestored ? F("true") : F("false");
  response += F(",\"schedulerVersion\":3");
  response += F(",\"conditionVersion\":1,\"condition\":{");
  response += F("\"enabled\":");
  response += conditionConfig.enabled ? F("true") : F("false");
  response += F(",\"channel\":"); response += String(conditionConfig.channel);
  response += F(",\"temperatureEnabled\":"); response += conditionConfig.temperatureEnabled ? F("true") : F("false");
  response += F(",\"humidityEnabled\":"); response += conditionConfig.humidityEnabled ? F("true") : F("false");
  response += F(",\"humiditySource\":\""); response += conditionConfig.humiditySource == 0 ? F("air") : F("soil");
  response += F("\",\"logic\":\""); response += conditionConfig.logic == 0 ? F("or") : F("and");
  response += F("\",\"action\":\""); response += conditionConfig.action == 0 ? F("hold") : F("timed");
  response += F("\",\"temperatureLimit\":"); response += String(conditionConfig.temperatureLimit / 10.0f, 1);
  response += F(",\"humidityLimit\":"); response += String(conditionConfig.humidityLimit / 10.0f, 1);
  response += F(",\"duration\":"); response += String(conditionConfig.durationMinute);
  response += F(",\"saved\":"); response += conditionSaved ? F("true") : F("false");
  response += F(",\"fresh\":"); response += conditionRuntime.fresh ? F("true") : F("false");
  response += F(",\"matched\":"); response += conditionRuntime.matched ? F("true") : F("false");
  response += F(",\"active\":"); response += conditionRuntime.demand ? F("true") : F("false");
  response += F(",\"remaining\":"); response += String(conditionRemaining(conditionRuntime, millis()));
  response += F(",\"resumePending\":"); response += conditionResumePending ? F("true") : F("false");
  response += F(",\"sampleAge\":");
  response += conditionSample.received ? String(static_cast<uint32_t>(millis() - conditionSample.receivedAt) / 1000) : String(F("null"));
  response += F(",\"temperature\":");
  response += conditionSample.temperatureValid ? String(conditionSample.temperature / 10.0f, 1) : String(F("null"));
  response += F(",\"air\":"); response += conditionSample.airValid ? String(conditionSample.air / 10.0f, 1) : String(F("null"));
  response += F(",\"soil\":"); response += conditionSample.soilValid ? String(conditionSample.soil / 10.0f, 1) : String(F("null"));
  response += '}';
  response += F(",\"programs\":[");
  for (uint8_t i = 0; i < 4; i++) {
    if (i > 0) response += ',';
    const WeeklyProgram& program = settings.programs[i];
    response += F("{\"task\":\"");
    response += TASK_NAMES[i];
    response += F("\",\"mode\":\"");
    response += modeName(program.mode);
    response += F("\",\"channel\":");
    response += String(program.channel);
    response += F(",\"slots\":[");
    for (uint8_t s = 0; s < 4; s++) {
      if (s > 0) response += ',';
      const ScheduleSlot& slot = program.slots[s];
      response += F("{\"enabled\":");
      response += slot.enabled ? F("true") : F("false");
      response += F(",\"days\":");
      response += String(slot.days);
      response += F(",\"type\":\"");
      response += slot.scheduleType == SCHEDULE_CYCLE ? F("cycle") : F("window");
      response += F("\",\"start\":\"");
      response += formatTime(slot.startMinute);
      response += F("\",\"stop\":\"");
      response += formatTime(slot.stopMinute);
      response += F("\",\"duration\":");
      response += String(slot.durationMinute);
      response += F(",\"interval\":");
      response += String(slot.intervalMinute);
      response += '}';
    }
    response += F("],\"temporaryRemaining\":");
    const uint32_t elapsed = static_cast<uint32_t>(millis() - temporaryStart[i]);
    const uint32_t remaining = temporaryActive(millis(), temporaryStart[i], temporaryDuration[i])
        ? (temporaryDuration[i] - elapsed + 999) / 1000 : 0;
    response += String(remaining);
    response += F(",\"temporaryAwaitingClock\":");
    response += temporaryEnd[i] != 0 && now < MIN_VALID_TIMESTAMP ? F("true") : F("false");
    response += F(",\"nextChange\":");
    const bool conditionControlled = conditionConfig.enabled && program.mode == MODE_PROGRAM &&
        conditionConfig.channel == program.channel;
    const time_t next = remaining == 0 && !conditionControlled ? findNextTransition(now, i) : 0;
    response += next > 0 ? String(static_cast<uint32_t>(next)) : String(F("null"));
    response += F(",\"conditionControlled\":");
    response += conditionControlled ? F("true") : F("false");
    response += F(",\"relayOn\":");
    response += relayStates[program.channel - 1] ? F("true") : F("false");
    response += '}';
  }
  response += ']';
  response += '}';
  return response;
}

void handleStatus() {
  updateRelay();
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), statusJson());
}

int taskIndex(const String& task) {
  for (uint8_t i = 0; i < 4; i++) {
    if (task == TASK_NAMES[i]) return i;
  }
  return -1;
}

void handleProgramControl() {
  if (WiFi.status() != WL_CONNECTED) {
    server.send(503, F("application/json"), F("{\"error\":\"relay module is offline\"}"));
    return;
  }

  const int index = taskIndex(server.arg("task"));
  if (index < 0) {
    server.send(400, F("application/json"), F("{\"error\":\"unknown task\"}"));
    return;
  }

  const String mode = server.arg("mode");
  uint8_t requestedMode = MODE_OFF;
  if (mode == "on") {
    requestedMode = MODE_ON;
  } else if (mode == "off") {
    requestedMode = MODE_OFF;
  } else if (mode == "prog") {
    requestedMode = MODE_PROGRAM;
  } else {
    server.send(400, F("application/json"), F("{\"error\":\"mode must be on, prog, or off\"}"));
    return;
  }

  WeeklyProgram requested = settings.programs[index];

  const String channelValue = server.arg("channel");
  if (channelValue.length() != 1 || channelValue[0] < '1' || channelValue[0] > '4') {
    server.send(400, F("application/json"), F("{\"error\":\"channel must be between 1 and 4\"}"));
    return;
  }
  const uint8_t channel = static_cast<uint8_t>(channelValue[0] - '0');
  const uint8_t previousChannel = settings.programs[index].channel;
  int conflictingIndex = -1;
  for (uint8_t i = 0; i < 4; i++) {
    if (i != index && settings.programs[i].channel == channel) {
      conflictingIndex = i;
      break;
    }
  }

  for (uint8_t s = 0; s < 4; s++) {
    ScheduleSlot& slot = requested.slots[s];
    const String prefix = String(F("s")) + s + '_';
    const String enabled = server.arg(prefix + F("enabled"));
    const String type = server.arg(prefix + F("type"));
    uint16_t days = 0;
    if ((enabled != "0" && enabled != "1") || (type != "window" && type != "cycle") ||
        !parseUnsigned(server.arg(prefix + F("days")), 0, 127, days) ||
        !parseTime(server.arg(prefix + F("start")), slot.startMinute) ||
        !parseTime(server.arg(prefix + F("stop")), slot.stopMinute) ||
        !parseUnsigned(server.arg(prefix + F("duration")), 1, 1440, slot.durationMinute) ||
        !parseUnsigned(server.arg(prefix + F("interval")), 1, 1440, slot.intervalMinute)) {
      server.send(400, F("application/json"), F("{\"error\":\"invalid schedule slot\"}"));
      return;
    }
    slot.enabled = enabled == "1";
    slot.days = days;
    slot.scheduleType = type == "cycle" ? SCHEDULE_CYCLE : SCHEDULE_WINDOW;
    if (!validSlot(slot)) {
      server.send(400, F("application/json"),
                  F("{\"error\":\"select weekdays and set duration no longer than interval\"}"));
      return;
    }
  }

  requested.mode = requestedMode;
  requested.channel = channel;

  const RelaySettings previousSettings = settings;
  if (conflictingIndex >= 0) {
    settings.programs[conflictingIndex].channel = previousChannel;
  }
  settings.programs[index] = requested;
  if (!saveSettings()) {
    settings = previousSettings;
    EEPROM.put(0, settings);
    server.send(500, F("application/json"), F("{\"error\":\"could not save relay settings\"}"));
    return;
  }

  if ((previousChannel == conditionConfig.channel || channel == conditionConfig.channel) &&
      (conflictingIndex >= 0 || previousSettings.programs[index].mode != requestedMode)) {
    conditionRuntime = {};
    conditionEnd = 0;
    conditionResumePending = false;
  }
  temporaryDuration[index] = 0;
  temporaryEnd[index] = 0;
  if (conflictingIndex >= 0) {
    temporaryDuration[conflictingIndex] = 0;
    temporaryEnd[conflictingIndex] = 0;
  }
  for (uint8_t i = 0; i < 4; i++) transitionChecked[i] = 0;
  relayStatesKnown[previousChannel - 1] = false;
  relayStatesKnown[channel - 1] = false;
  updateRelay();
  if (!recoverySaved) {
    server.send(500, F("application/json"), F("{\"error\":\"settings applied but power-loss recovery could not be saved; check relay status\"}"));
    return;
  }
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), statusJson());
}

void handleTemporaryControl() {
  const int index = taskIndex(server.arg("task"));
  uint16_t minutes = 0;
  if (index < 0 || !parseUnsigned(server.arg("minutes"), 0, 1440, minutes)) {
    server.send(400, F("application/json"),
                F("{\"error\":\"unknown task or temporary duration outside 0..1440 minutes\"}"));
    return;
  }
  const time_t now = time(nullptr);
  if (minutes != 0 && now < MIN_VALID_TIMESTAMP) {
    server.send(503, F("application/json"), F("{\"error\":\"wait for clock synchronization before starting a temporary run\"}"));
    return;
  }
  temporaryStart[index] = millis();
  temporaryDuration[index] = static_cast<uint32_t>(minutes) * 60000UL;
  temporaryEnd[index] = minutes == 0 ? 0 : static_cast<uint32_t>(now) + minutes * 60UL;
  updateRelay();
  if (!recoverySaved) {
    server.send(500, F("application/json"), F("{\"error\":\"command applied but power-loss recovery could not be saved; check relay status\"}"));
    return;
  }
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), statusJson());
}

bool parseConditionNumber(const String& value, int minimum, int maximum, int& result) {
  if (value.length() == 0 || value.length() > 6) return false;
  const bool negative = value[0] == '-';
  if (negative && value.length() == 1) return false;
  int parsed = 0;
  for (uint16_t i = negative ? 1 : 0; i < value.length(); i++) {
    if (!isDigit(value[i])) return false;
    parsed = parsed * 10 + value[i] - '0';
    if (parsed > 100000) return false;
  }
  result = negative ? -parsed : parsed;
  return result >= minimum && result <= maximum;
}

void handleConditionControl() {
  ConditionConfig requested = {};
  const char* fields[] = {"enabled", "channel", "temperatureEnabled", "humidityEnabled",
                        "humiditySource", "logic", "action", "temperatureLimit", "humidityLimit", "duration"};
  const int minimum[] = {0, 1, 0, 0, 0, 0, 0, -400, 0, 1};
  const int maximum[] = {1, 4, 1, 1, 1, 1, 1, 800, 1000, 1440};
  int values[10] = {};
  for (uint8_t i = 0; i < 10; i++) {
    if (!parseConditionNumber(server.arg(fields[i]), minimum[i], maximum[i], values[i])) {
      server.send(400, F("application/json"), F("{\"error\":\"invalid condition field; thresholds use tenths\"}"));
      return;
    }
  }
  requested.enabled = values[0]; requested.channel = values[1];
  requested.temperatureEnabled = values[2]; requested.humidityEnabled = values[3];
  requested.humiditySource = values[4]; requested.logic = values[5]; requested.action = values[6];
  requested.temperatureLimit = values[7]; requested.humidityLimit = values[8]; requested.durationMinute = values[9];
  if (!validCondition(requested)) {
    server.send(400, F("application/json"), F("{\"error\":\"enable at least one threshold for an active condition\"}"));
    return;
  }
  const ConditionConfig previous = conditionConfig;
  conditionConfig = requested;
  if (!saveCondition()) {
    conditionConfig = previous;
    ConditionStore stored = {};
    stored.magic = 0x434E4431; stored.config = previous;
    stored.checksum = recoveryHash(&stored, offsetof(ConditionStore, checksum));
    EEPROM.put(CONDITION_OFFSET, stored);
    server.send(500, F("application/json"), F("{\"error\":\"condition could not be saved\"}"));
    return;
  }
  conditionRuntime = {};
  conditionEnd = 0;
  conditionResumePending = false;
  updateRelay();
  if (!recoverySaved) {
    server.send(500, F("application/json"), F("{\"error\":\"condition saved and applied but power-loss state could not be saved; check relay status\"}"));
    return;
  }
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), statusJson());
}

void handleSensorSample() {
  ConditionSample sample;
  int valid[3] = {};
  int values[3] = {};
  const char* flags[] = {"temperatureValid", "airValid", "soilValid"};
  const char* fields[] = {"temperature", "air", "soil"};
  for (uint8_t i = 0; i < 3; i++) {
    if (!parseConditionNumber(server.arg(flags[i]), 0, 1, valid[i]) ||
        !parseConditionNumber(server.arg(fields[i]), i == 0 ? -400 : 0, i == 0 ? 800 : 1000, values[i])) {
      server.send(400, F("application/json"), F("{\"error\":\"invalid sensor sample\"}"));
      return;
    }
  }
  sample.received = true; sample.receivedAt = millis();
  sample.temperatureValid = valid[0]; sample.airValid = valid[1]; sample.soilValid = valid[2];
  sample.temperature = values[0]; sample.air = values[1]; sample.soil = values[2];
  conditionSample = sample;
  updateRelay();
  server.send(200, F("application/json"), F("{\"ok\":true}"));
}

void handleRoot() {
  server.send(200, F("text/plain; charset=utf-8"),
              F("4-channel ESP8266 relay module. Use GET /api/status and POST /api/program."));
}

void setup() {
  loadSettings();
  loadCondition();
  loadRecovery();
  Serial.begin(RELAY_UART_BAUD);
  delay(100);
  updateRelay();

  configTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");

  WiFi.mode(WIFI_STA);
  WiFi.hostname("grow-relay");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/program", HTTP_POST, handleProgramControl);
  server.on("/api/temporary", HTTP_POST, handleTemporaryControl);
  server.on("/api/condition", HTTP_POST, handleConditionControl);
  server.on("/api/sensors", HTTP_POST, handleSensorSample);
  server.onNotFound([]() {
    server.send(404, F("application/json"), F("{\"error\":\"not found\"}"));
  });
  server.begin();
}

void loop() {
  server.handleClient();

  if (WiFi.status() != WL_CONNECTED &&
      millis() - lastWifiAttempt >= 10000) {
    lastWifiAttempt = millis();
    WiFi.reconnect();
  }

  static bool mdnsStarted = false;
  if (WiFi.status() == WL_CONNECTED && !mdnsStarted) {
    mdnsStarted = MDNS.begin("grow-relay");
    if (mdnsStarted) {
      MDNS.addService("http", "tcp", 80);
    }
  }
  if (mdnsStarted) {
    MDNS.update();
  }

  static unsigned long lastScheduleCheck = 0;
  if (millis() - lastScheduleCheck >= 1000) {
    lastScheduleCheck = millis();
    updateRelay();
  }
}

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <EEPROM.h>
#include <time.h>

#include "secrets.h"
const uint32_t SETTINGS_MAGIC = 0x524C5932;
const uint32_t MIN_VALID_TIMESTAMP = 1700000000UL;
const uint32_t RELAY_UART_BAUD = 115200;
const uint16_t EEPROM_SIZE = 128;

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

struct RelaySettings {
  uint32_t magic;
  TaskProgram programs[4];
};

ESP8266WebServer server(80);
RelaySettings settings = {};
bool relayStates[4] = {};
bool relayStatesKnown[4] = {};
bool settingsSaved = true;
unsigned long lastWifiAttempt = 0;

const char* const TASK_NAMES[] = {"light", "ventilation", "pump", "option"};

bool saveSettings() {
  EEPROM.put(0, settings);
  settingsSaved = EEPROM.commit();
  return settingsSaved;
}

void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, settings);
  bool valid = settings.magic == SETTINGS_MAGIC;
  for (uint8_t i = 0; i < 4 && valid; i++) {
    const TaskProgram& program = settings.programs[i];
    valid = program.mode <= MODE_PROGRAM &&
        program.channel >= 1 && program.channel <= 4 &&
        program.scheduleType <= SCHEDULE_CYCLE &&
        program.durationMinute >= 1 && program.durationMinute <= 1440 &&
        program.intervalMinute >= program.durationMinute &&
        program.intervalMinute <= 1440 &&
        program.startMinute < 1440 && program.stopMinute < 1440;
    for (uint8_t j = 0; j < i && valid; j++) {
      valid = program.channel != settings.programs[j].channel;
    }
  }
  if (!valid) {
    settings.magic = SETTINGS_MAGIC;
    for (uint8_t i = 0; i < 4; i++) {
      TaskProgram& program = settings.programs[i];
      program.mode = i == TASK_LIGHT ? MODE_PROGRAM : MODE_OFF;
      program.channel = i + 1;
      program.scheduleType = i == TASK_LIGHT ? SCHEDULE_WINDOW : SCHEDULE_CYCLE;
      program.startMinute = i == TASK_LIGHT ? 6 * 60 : 0;
      program.stopMinute = 0;
      program.durationMinute = i == TASK_PUMP ? 25 : 5;
      program.intervalMinute = i == TASK_PUMP ? 120 : 60;
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

bool isInsideSchedule(uint16_t minuteOfDay, const TaskProgram& program) {
  if (program.startMinute == program.stopMinute) {
    // Equal boundaries mean the whole day; OFF is handled by the mode.
    return true;
  }
  if (program.startMinute < program.stopMinute) {
    return minuteOfDay >= program.startMinute && minuteOfDay < program.stopMinute;
  }
  return minuteOfDay >= program.startMinute || minuteOfDay < program.stopMinute;
}

uint16_t scheduleOffset(uint16_t minuteOfDay, const TaskProgram& program) {
  return minuteOfDay >= program.startMinute
      ? minuteOfDay - program.startMinute
      : static_cast<uint16_t>(minuteOfDay + 1440 - program.startMinute);
}

bool scheduledProgramState(time_t now, const TaskProgram& program) {
  if (now < MIN_VALID_TIMESTAMP) {
    return false;
  }

  struct tm localTime = {};
  if (localtime_r(&now, &localTime) == nullptr) {
    return false;
  }

  const uint16_t minuteOfDay = static_cast<uint16_t>(localTime.tm_hour * 60 + localTime.tm_min);
  if (!isInsideSchedule(minuteOfDay, program)) {
    return false;
  }
  if (program.scheduleType == SCHEDULE_WINDOW) {
    return true;
  }
  return scheduleOffset(minuteOfDay, program) % program.intervalMinute < program.durationMinute;
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
  for (uint8_t i = 0; i < 4; i++) {
    const TaskProgram& program = settings.programs[i];
    bool desiredState = false;
    if (program.mode == MODE_ON) {
      desiredState = true;
    } else if (program.mode == MODE_PROGRAM) {
      desiredState = scheduledProgramState(now, program);
    }
    const uint8_t relayIndex = program.channel - 1;
    if (!relayStatesKnown[relayIndex] || desiredState != relayStates[relayIndex]) {
      sendRelayCommand(program.channel, desiredState);
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
  response.reserve(640);
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
  response += F(",\"programs\":[");
  for (uint8_t i = 0; i < 4; i++) {
    if (i > 0) response += ',';
    const TaskProgram& program = settings.programs[i];
    response += F("{\"task\":\"");
    response += TASK_NAMES[i];
    response += F("\",\"mode\":\"");
    response += modeName(program.mode);
    response += F("\",\"channel\":");
    response += String(program.channel);
    response += F(",\"type\":\"");
    response += program.scheduleType == SCHEDULE_CYCLE ? F("cycle") : F("window");
    response += F("\",\"start\":\"");
    response += formatTime(program.startMinute);
    response += F("\",\"stop\":\"");
    response += formatTime(program.stopMinute);
    response += F("\",\"duration\":");
    response += String(program.durationMinute);
    response += F(",\"interval\":");
    response += String(program.intervalMinute);
    response += F(",\"relayOn\":");
    response += relayStates[program.channel - 1] ? F("true") : F("false");
    response += '}';
  }
  response += ']';
  response += '}';
  return response;
}

void handleStatus() {
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

  TaskProgram requested = settings.programs[index];
  uint16_t startMinute = 0;
  uint16_t stopMinute = 0;
  if (!parseTime(server.arg("start"), startMinute) ||
      !parseTime(server.arg("stop"), stopMinute)) {
    server.send(400, F("application/json"), F("{\"error\":\"invalid schedule time\"}"));
    return;
  }

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

  const String type = server.arg("type");
  if (type != "window" && type != "cycle") {
    server.send(400, F("application/json"), F("{\"error\":\"type must be window or cycle\"}"));
    return;
  }
  uint16_t durationMinute = 0;
  uint16_t intervalMinute = 0;
  if (!parseUnsigned(server.arg("duration"), 1, 1440, durationMinute) ||
      !parseUnsigned(server.arg("interval"), 1, 1440, intervalMinute) ||
      durationMinute > intervalMinute) {
    server.send(400, F("application/json"),
                F("{\"error\":\"duration and interval must be between 1 and 1440 minutes, with duration no longer than interval\"}"));
    return;
  }

  requested.mode = requestedMode;
  requested.channel = channel;
  requested.scheduleType = type == "cycle" ? SCHEDULE_CYCLE : SCHEDULE_WINDOW;
  requested.startMinute = startMinute;
  requested.stopMinute = stopMinute;
  requested.durationMinute = durationMinute;
  requested.intervalMinute = intervalMinute;

  const RelaySettings previousSettings = settings;
  if (conflictingIndex >= 0) {
    settings.programs[conflictingIndex].channel = previousChannel;
  }
  settings.programs[index] = requested;
  if (!saveSettings()) {
    settings = previousSettings;
    server.send(500, F("application/json"), F("{\"error\":\"could not save relay settings\"}"));
    return;
  }

  relayStatesKnown[previousChannel - 1] = false;
  relayStatesKnown[channel - 1] = false;
  updateRelay();
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), statusJson());
}

void handleRoot() {
  server.send(200, F("text/plain; charset=utf-8"),
              F("4-channel ESP8266 relay module. Use GET /api/status and POST /api/program."));
}

void setup() {
  loadSettings();
  Serial.begin(RELAY_UART_BAUD);
  delay(100);

  configTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");

  WiFi.mode(WIFI_STA);
  WiFi.hostname("grow-relay");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/program", HTTP_POST, handleProgramControl);
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

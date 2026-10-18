#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
#include <map>
#include <memory>
#include <string>
#include "Scheduler.h"
#include "Recovery.h"
#include "Condition.h"

using String = std::string;
bool isDigit(char value) { return value >= '0' && value <= '9'; }

struct LittleFSConfig {
  void setAutoFormat(bool) {}
};

struct File {
  std::shared_ptr<std::vector<uint8_t>> data;
  explicit operator bool() const { return bool(data); }
  size_t size() const { return data->size(); }
  size_t read(uint8_t* output, size_t length) {
    length = std::min(length, data->size());
    std::memcpy(output, data->data(), length);
    return length;
  }
  size_t write(const uint8_t* input, size_t length) {
    data->assign(input, input + length);
    return length;
  }
  void close() { data.reset(); }
};

struct MemoryLittleFS {
  std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> files;
  bool mounted = false;
  bool failRename = false;
  bool failOpen = false;
  int formats = 0;
  int writes = 0;
  void setConfig(const LittleFSConfig&) {}
  bool begin() { return mounted; }
  bool format() { formats++; files.clear(); mounted = true; return true; }
  bool exists(const char* path) { return files.count(path) != 0; }
  File open(const char* path, const char* mode) {
    if (failOpen) return {};
    if (mode[0] == 'w') {
      writes++;
      files[path] = std::make_shared<std::vector<uint8_t>>();
    }
    auto found = files.find(path);
    if (found == files.end()) return {};
    return {found->second};
  }
  bool rename(const char* from, const char* to) {
    if (failRename) return false;
    files[to] = files.at(from);
    files.erase(from);
    return true;
  }
} LittleFS;

struct MemoryEEPROM {
  std::vector<uint8_t> flash = std::vector<uint8_t>(128, 255);
  std::vector<uint8_t> buffer;
  void begin(size_t size) { buffer = flash; buffer.resize(size, 255); }
  template<typename T> void get(size_t offset, T& value) {
    std::memcpy(&value, buffer.data() + offset, sizeof(value));
  }
  template<typename T> void put(size_t offset, const T& value) {
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
  }
  bool commit() { flash = buffer; return true; }
} EEPROM;

struct SerialSink {
  std::vector<uint8_t> bytes;
  bool controllerReady = true;
  bool physicalStates[4] = {};
  void write(const uint8_t* data, size_t size) {
    bytes.insert(bytes.end(), data, data + size);
    if (controllerReady && size == 4 && data[0] == 0xA0 &&
        data[1] >= 1 && data[1] <= 4) physicalStates[data[1] - 1] = data[2] != 0;
  }
  void flush() {}
} Serial;

uint32_t fakeMillis = 0;
time_t fakeNow = 1767225600; // 2026-01-01 UTC.
uint32_t millis() { return fakeMillis; }
time_t testTime(time_t*) { return fakeNow; }
void yield() {}

// Europe/Paris 2026 DST boundaries in UTC; conversion is injected, not the scheduler.
tm* parisTime(const time_t* timestamp, tm* output) {
  const time_t adjusted = *timestamp +
      (*timestamp >= 1774746000 && *timestamp < 1792890000 ? 7200 : 3600);
  *output = *gmtime(&adjusted);
  return output;
}

bool relayStates[4] = {};
bool relayStatesKnown[4] = {};
uint32_t temporaryStart[4] = {};
uint32_t temporaryDuration[4] = {};
time_t transitionChecked[4] = {};
time_t nextTransition[4] = {};
#define time testTime
#define localtime_r parisTime
#include "RelayUnderTest.h"
#undef time
#undef localtime_r

tm at(int weekday, int hour, int minute) {
  tm local = {};
  local.tm_wday = weekday;
  local.tm_hour = hour;
  local.tm_min = minute;
  return local;
}

ScheduleSlot slot(uint8_t days, int start, int stop, int duration = 5, int interval = 60,
                  bool cycle = false) {
  ScheduleSlot result = {};
  result.enabled = 1;
  result.days = days;
  result.startMinute = start;
  result.stopMinute = stop;
  result.durationMinute = duration;
  result.intervalMinute = interval;
  result.scheduleType = cycle;
  return result;
}

void resetTransitions() {
  std::fill(transitionChecked, transitionChecked + 4, 0);
}

int main() {
  int parsedCondition = 0;
  assert(parseConditionNumber("-400", -400, 800, parsedCondition) && parsedCondition == -400);
  assert(parseConditionNumber("800", -400, 800, parsedCondition) && parsedCondition == 800);
  assert(parseConditionNumber("0", 0, 1, parsedCondition) && parsedCondition == 0);
  for (const char* bad : {"", "-", "--1", "+1", " 1", "1 ", "28.5", "1&mode=on", "999999999", "801", "-401"}) {
    assert(!parseConditionNumber(bad, -400, 800, parsedCondition));
  }
  const ScheduleSlot monday = slot(1, 360, 720);
  assert(validSlot(monday));
  assert(!slotState(at(1, 5, 59), monday));
  assert(slotState(at(1, 6, 0), monday));
  assert(slotState(at(1, 11, 59), monday));
  assert(!slotState(at(1, 12, 0), monday));
  assert(!slotState(at(2, 6, 0), monday));
  const ScheduleSlot night = slot(64, 1320, 120);
  assert(slotState(at(0, 22, 0), night));
  assert(slotState(at(1, 1, 59), night));
  assert(!slotState(at(1, 2, 0), night));
  assert(!slotState(at(0, 1, 0), night));
  const ScheduleSlot fullDay = slot(1, 360, 360);
  assert(!slotState(at(1, 5, 59), fullDay));
  assert(slotState(at(1, 6, 0), fullDay));
  assert(slotState(at(2, 5, 59), fullDay));
  assert(!slotState(at(2, 6, 0), fullDay));
  const ScheduleSlot weekend = slot(96, 0, 0);
  assert(slotState(at(6, 12, 0), weekend));
  assert(slotState(at(0, 12, 0), weekend));
  assert(!slotState(at(1, 12, 0), weekend));
  for (int duration : {5, 25}) {
    const int interval = duration == 5 ? 60 : 120;
    const ScheduleSlot cycle = slot(127, 360, 0, duration, interval, true);
    assert(slotState(at(1, 6, duration - 1), cycle));
    assert(!slotState(at(1, 6, duration), cycle));
    const int restart = 360 + interval;
    assert(slotState(at(1, restart / 60, restart % 60), cycle));
  }
  auto invalid = monday;
  invalid.days = 0;
  assert(!validSlot(invalid));
  invalid.enabled = 0;
  assert(validSlot(invalid));
  invalid = monday;
  invalid.durationMinute = 61;
  assert(!validSlot(invalid));
  invalid = monday;
  invalid.startMinute = 1440;
  assert(!validSlot(invalid));
  invalid = monday;
  invalid.days = 128;
  assert(!validSlot(invalid));
  invalid = monday;
  invalid.scheduleType = 2;
  assert(!validSlot(invalid));
  invalid = monday;
  invalid.enabled = 2;
  assert(!validSlot(invalid));
  invalid = monday;
  invalid.intervalMinute = 0;
  assert(!validSlot(invalid));
  assert(validSlot(slot(127, 1439, 0, 1440, 1440, true)));
  assert(temporaryActive(20, UINT32_MAX - 9, 31));
  assert(!temporaryActive(21, UINT32_MAX - 9, 31));
  assert(!temporaryActive(0, 0, 0));

  EEPROM.begin(128);
  LegacyRelaySettings legacy = {};
  legacy.magic = LEGACY_SETTINGS_MAGIC;
  for (int i = 0; i < 4; i++) {
    legacy.programs[i].mode = i % 3;
    legacy.programs[i].channel = 4 - i;
    legacy.programs[i].scheduleType = i % 2;
    legacy.programs[i].durationMinute = 5 + i;
    legacy.programs[i].intervalMinute = 30 + i;
    legacy.programs[i].startMinute = 360 + i;
    legacy.programs[i].stopMinute = i;
  }
  EEPROM.put(0, legacy);
  EEPROM.commit();
  loadSettings();
  assert(settings.magic == SETTINGS_MAGIC && settingsSaved);
  for (int i = 0; i < 4; i++) {
    const auto& program = settings.programs[i];
    const auto& old = legacy.programs[i];
    assert(program.mode == old.mode && program.channel == old.channel);
    for (int s = 0; s < 4; s++) {
      const auto& migrated = program.slots[s];
      assert(validSlot(migrated) && migrated.days == 127 && migrated.enabled == (s == 0));
      assert(migrated.startMinute == old.startMinute && migrated.stopMinute == old.stopMinute);
      assert(migrated.scheduleType == old.scheduleType);
      assert(migrated.durationMinute == old.durationMinute && migrated.intervalMinute == old.intervalMinute);
    }
  }
  const auto saved = settings;
  loadSettings();
  assert(std::memcmp(&settings, &saved, sizeof(settings)) == 0);
  legacy.programs[1].channel = legacy.programs[0].channel;
  EEPROM.put(0, legacy);
  EEPROM.commit();
  loadSettings();
  for (int i = 0; i < 4; i++) {
    assert(settings.programs[i].channel == i + 1);
    assert(settings.programs[i].mode == (i == TASK_LIGHT ? MODE_PROGRAM : MODE_OFF));
  }

  WeeklyProgram program = {};
  program.mode = MODE_PROGRAM;
  program.channel = 1;
  program.slots[0] = slot(127, 360, 720);
  program.slots[1] = slot(127, 660, 780);
  assert(weeklyState(at(1, 12, 30), program));
  settings.programs[0] = program;
  // Jan 1 2026, 10:30 UTC = 11:30 Paris. No false stop at noon.
  fakeNow = 1767263400;
  resetTransitions();
  assert(findNextTransition(fakeNow, 0) == 1767268800);
  assert(findNextTransition(fakeNow + 1, 0) == 1767268800);
  settings.programs[0].mode = MODE_ON;
  assert(findNextTransition(fakeNow, 0) == 0);
  settings.programs[0].mode = MODE_PROGRAM;
  settings.programs[0].slots[0] = slot(127, 0, 0);
  settings.programs[0].slots[1].enabled = 0;
  resetTransitions();
  assert(findNextTransition(fakeNow, 0) == 0);
  assert(findNextTransition(0, 0) == 0);
  settings.programs[0].slots[0].enabled = 0;
  resetTransitions();
  assert(findNextTransition(fakeNow, 0) == 0);
  settings.programs[0].slots[0] = slot(8, 360, 420);
  resetTransitions();
  // Thursday after its window: the next activation is seven days later.
  assert(findNextTransition(fakeNow, 0) == 1767848400);

  // Spring: 01:59 CET jumps to 03:00 CEST; autumn repeats 02:00.
  settings.programs[0].slots[0] = slot(64, 150, 210);
  resetTransitions();
  assert(findNextTransition(1774745940, 0) == 1774746000);
  settings.programs[0].slots[0] = slot(64, 150, 165);
  resetTransitions();
  assert(findNextTransition(1792889940, 0) == 1792891800);

  // The real output function resumes OFF / ON / PROG and clears expired timers.
  for (int i = 0; i < 4; i++) {
    settings.programs[i].mode = MODE_OFF;
    settings.programs[i].channel = i + 1;
  }
  fakeNow = 1767263400;
  for (uint8_t mode : {MODE_OFF, MODE_ON, MODE_PROGRAM}) {
    settings.programs[0].mode = mode;
    settings.programs[0].slots[0] = slot(127, 360, 720);
    temporaryStart[0] = UINT32_MAX - 9;
    temporaryDuration[0] = 31;
    fakeMillis = 20;
    updateRelay();
    assert(relayStates[0]);
    fakeMillis = 21;
    updateRelay();
    assert(temporaryDuration[0] == 0);
    assert(relayStates[0] == (mode != MODE_OFF));
    fakeNow += 4 * 3600;
    updateRelay();
    assert(relayStates[0] == (mode == MODE_ON));
    fakeNow -= 4 * 3600;
  }
  assert(Serial.bytes.size() % 4 == 0);
  for (size_t i = 0; i < Serial.bytes.size(); i += 4) {
    assert(Serial.bytes[i] == 0xA0);
    assert(Serial.bytes[i + 3] == uint8_t(0xA0 + Serial.bytes[i + 1] + Serial.bytes[i + 2]));
  }
  // Persist actual outputs, simulate RAM loss, restore before NTP, then correct.
  fakeMillis = 10000;
  fakeNow = 1767263400;
  settings.programs[0].mode = MODE_PROGRAM;
  settings.programs[0].slots[0] = slot(127, 360, 720);
  settings.programs[1].mode = MODE_ON;
  settings.programs[2].mode = MODE_OFF;
  loadRecovery();
  assert(recoveryReady && LittleFS.formats == 1);
  updateRelay();
  assert(recoverySaved && relayStates[0] && relayStates[1] && !relayStates[2]);
  const int writes = LittleFS.writes;
  for (int i = 0; i < 100; i++) updateRelay();
  assert(LittleFS.writes == writes);
  const auto restart = []() {
    std::fill(relayStates, relayStates + 4, false);
    std::fill(relayStatesKnown, relayStatesKnown + 4, false);
    std::fill(temporaryStart, temporaryStart + 4, 0);
    std::fill(temporaryDuration, temporaryDuration + 4, 0);
    std::fill(temporaryEnd, temporaryEnd + 4, 0);
    conditionSample = {};
    conditionRuntime = {};
    conditionEnd = 0;
    conditionResumePending = false;
    recoveryReady = recoverySaved = recoveryRestored = false;
    savedRecovery = {};
    lastRecoveryAttempt = 0;
    fakeMillis = 100;
    fakeNow = 0;
    loadRecovery();
    updateRelay();
  };
  restart();
  assert(recoveryRestored && relayStates[0] && relayStates[1] && !relayStates[2]);
  fakeNow = 1767263400 + 4 * 3600;
  updateRelay();
  assert(!recoveryRestored && !relayStates[0] && relayStates[1]);

  // OFF base + temporary ON: downtime counts toward the original deadline.
  settings.programs[0].mode = MODE_OFF;
  temporaryStart[0] = fakeMillis;
  temporaryDuration[0] = 600000;
  temporaryEnd[0] = fakeNow + 600;
  const uint32_t deadline = temporaryEnd[0];
  updateRelay();
  assert(relayStates[0] && recoverySaved);
  restart();
  assert(relayStates[0] && temporaryEnd[0] == deadline && temporaryDuration[0] == 0);
  fakeNow = deadline - 120;
  updateRelay();
  assert(relayStates[0] && temporaryDuration[0] == 120000);
  restart();
  fakeNow = deadline + 1;
  updateRelay();
  assert(!relayStates[0] && temporaryDuration[0] == 0 && temporaryEnd[0] == 0);
  restart();
  fakeNow = deadline + 2;
  updateRelay();
  assert(!relayStates[0] && temporaryEnd[0] == 0);
  for (uint8_t mode : {MODE_ON, MODE_PROGRAM}) {
    fakeNow = 1767263400;
    settings.programs[0].mode = mode;
    temporaryStart[0] = fakeMillis;
    temporaryDuration[0] = 60000;
    temporaryEnd[0] = fakeNow + 60;
    const uint32_t end = temporaryEnd[0];
    updateRelay();
    assert(recoverySaved);
    restart();
    fakeNow = end + 1;
    updateRelay();
    assert(relayStates[0] && temporaryEnd[0] == 0);
  }
  settings.programs[0].mode = MODE_OFF;
  updateRelay();
  // Persist cancellation, then verify a reboot never resurrects the timer.
  temporaryStart[0] = fakeMillis;
  temporaryDuration[0] = 60000;
  temporaryEnd[0] = fakeNow + 60;
  updateRelay();
  temporaryDuration[0] = temporaryEnd[0] = 0;
  updateRelay();
  restart();
  assert(!relayStates[0] && temporaryEnd[0] == 0);

  // A failed atomic rename keeps the previous valid snapshot.
  LittleFS.failRename = true;
  settings.programs[0].mode = MODE_ON;
  fakeMillis = 10000;
  updateRelay();
  assert(!recoverySaved && relayStates[0]);
  LittleFS.failRename = false;
  settings.programs[0].mode = MODE_OFF;
  restart();
  assert(!relayStates[0] && recoverySaved);

  // Corrupt data and changed settings are rejected, not restored as ON.
  LittleFS.files.at("/recovery.bin")->at(0) ^= 1;
  restart();
  assert(!recoverySaved && !recoveryRestored && !relayStates[0]);
  fakeMillis = 10000;
  updateRelay();
  assert(recoverySaved);
  settings.programs[0].mode = MODE_ON;
  restart();
  assert(!recoveryRestored && relayStates[0]);
  fakeMillis = 10000;
  updateRelay();
  assert(recoverySaved);
  LittleFS.failOpen = true;
  settings.programs[0].mode = MODE_OFF;
  fakeMillis += 10000;
  updateRelay();
  assert(!recoverySaved);
  LittleFS.failOpen = false;
  fakeMillis += 5000;
  updateRelay();
  assert(recoverySaved);
  LittleFS.mounted = false;
  const int formats = LittleFS.formats;
  restart();
  assert(!recoveryReady && !recoverySaved && LittleFS.formats == formats);
  LittleFS.mounted = true;
  // Reproduce the real failure: startup UART frames are lost while STC boots.
  recoveryReady = true;
  recoverySaved = true;
  fakeNow = 1767263400;
  settings.programs[0].mode = MODE_PROGRAM;
  settings.programs[0].slots[0] = slot(127, 360, 720);
  Serial.controllerReady = false;
  std::fill(Serial.physicalStates, Serial.physicalStates + 4, false);
  std::fill(relayStatesKnown, relayStatesKnown + 4, false);
  fakeMillis = 100;
  updateRelay();
  assert(relayStates[0] && !Serial.physicalStates[0]);
  const int startupWrites = LittleFS.writes;
  Serial.controllerReady = true;
  fakeMillis = 5099;
  updateRelay();
  assert(!Serial.physicalStates[0]);
  fakeMillis = 5100;
  updateRelay();
  assert(Serial.physicalStates[0]);
  assert(LittleFS.writes == startupWrites);
  // A controller-only reset is repaired without PROG, toggles, or flash writes.
  std::fill(Serial.physicalStates, Serial.physicalStates + 4, true);
  fakeMillis += RELAY_REFRESH_MS;
  updateRelay();
  for (int i = 0; i < 4; i++) assert(Serial.physicalStates[i] == relayStates[i]);
  assert(LittleFS.writes == startupWrites);
  fakeMillis = UINT32_MAX - 100;
  updateRelay();
  Serial.physicalStates[0] = false;
  fakeMillis += RELAY_REFRESH_MS;
  updateRelay();
  assert(Serial.physicalStates[0]);
  // Condition persistence lives after the unchanged weekly EEPROM record.
  const RelaySettings weeklyBefore = settings;
  assert(saveSettings());
  conditionConfig = {1, 1, 1, 0, 0, 0, 0, 0, 280, 700, 5, 0};
  assert(saveCondition());
  conditionConfig.enabled = 0;
  loadCondition();
  assert(conditionSaved && conditionConfig.enabled == 1 && conditionConfig.temperatureLimit == 280);
  assert(std::memcmp(&settings, &weeklyBefore, sizeof(settings)) == 0);
  RelaySettings weeklyFromFlash = {};
  EEPROM.get(0, weeklyFromFlash);
  assert(std::memcmp(&weeklyFromFlash, &weeklyBefore, sizeof(settings)) == 0);
  // Conditions add to PROG but never override OFF, and do not require valid NTP.
  recoveryRestored = false;
  fakeNow = 0;
  conditionSample = {};
  conditionSample.received = conditionSample.temperatureValid = true;
  conditionSample.temperature = 290;
  conditionSample.receivedAt = fakeMillis;
  settings.programs[0].mode = MODE_PROGRAM;
  updateRelay();
  assert(conditionRuntime.demand && relayStates[0]);
  settings.programs[0].mode = MODE_OFF;
  updateRelay();
  assert(!conditionRuntime.demand && !relayStates[0]);
  settings.programs[0].mode = MODE_ON;
  updateRelay();
  assert(!conditionRuntime.demand && relayStates[0]);
  settings.programs[0].mode = MODE_PROGRAM;
  fakeNow = 1767263400;
  conditionSample.temperature = 270;
  updateRelay();
  assert(!conditionRuntime.demand && relayStates[0]); // The window still requests ON.
  fakeNow += 4 * 3600;
  conditionSample.temperature = 290;
  updateRelay();
  assert(conditionRuntime.demand && relayStates[0]);
  fakeMillis += 30000;
  updateRelay();
  assert(!conditionRuntime.demand && !relayStates[0]);
  conditionConfig.enabled = 0;
  conditionRuntime = {};
  // Upgrade a real v1 recovery layout without losing its relay mask or manual timer.
  LegacyRecoveryState legacyRecovery = {};
  legacyRecovery.magic = 0x52535431;
  legacyRecovery.settingsHash = recoveryHash(&settings, sizeof(settings));
  legacyRecovery.relayMask = 1;
  legacyRecovery.temporaryEnd[0] = 1767264000;
  legacyRecovery.checksum = recoveryHash(&legacyRecovery, offsetof(LegacyRecoveryState, checksum));
  File oldFile = LittleFS.open("/recovery.bin", "w");
  oldFile.write(reinterpret_cast<const uint8_t*>(&legacyRecovery), sizeof(legacyRecovery));
  oldFile.close();
  restart();
  assert(recoveryRestored && relayStates[0] && temporaryEnd[0] == 1767264000);
  std::fill(temporaryEnd, temporaryEnd + 4, 0);
  conditionConfig = {1, 1, 1, 0, 0, 0, 1, 0, 280, 700, 5, 0};
  assert(saveCondition());
  settings.programs[0].mode = MODE_PROGRAM;
  for (auto& scheduled : settings.programs[0].slots) scheduled.enabled = 0;
  recoveryRestored = false;
  conditionRuntime = {};
  fakeNow = 1767263400;
  fakeMillis = 10000;
  conditionSample.received = conditionSample.temperatureValid = true;
  conditionSample.temperature = 290;
  conditionSample.receivedAt = fakeMillis;
  updateRelay();
  assert(conditionRuntime.demand && recoverySaved && conditionEnd == fakeNow + 300);
  const int conditionWrites = LittleFS.writes;
  for (int i = 0; i < 20; i++) {
    conditionSample.receivedAt = ++fakeMillis;
    updateRelay();
  }
  assert(LittleFS.writes == conditionWrites);
  const uint32_t conditionDeadline = conditionEnd;
  restart();
  assert(conditionResumePending && conditionEnd == conditionDeadline);
  assert(!conditionRuntime.demand && !relayStates[0]); // Never resume on stale readings.
  fakeNow = conditionDeadline - 120;
  updateRelay();
  assert(conditionResumePending && !relayStates[0]); // Time alone is insufficient.
  conditionSample.received = conditionSample.temperatureValid = true;
  conditionSample.temperature = 290;
  conditionSample.receivedAt = fakeMillis;
  updateRelay();
  assert(!conditionResumePending && relayStates[0]);
  assert(conditionRuntime.durationMs == 120000 && conditionEnd == conditionDeadline);
  restart();
  fakeNow = conditionDeadline + 1;
  conditionSample.received = conditionSample.temperatureValid = true;
  conditionSample.temperature = 290;
  conditionSample.receivedAt = fakeMillis;
  updateRelay();
  assert(!conditionRuntime.demand && !conditionResumePending && !relayStates[0] && conditionEnd == 0);
  // Still high after reboot/expiry is not a new crossing.
  restart();
  fakeNow = conditionDeadline + 2;
  conditionSample.received = conditionSample.temperatureValid = true;
  conditionSample.temperature = 290;
  conditionSample.receivedAt = fakeMillis;
  updateRelay();
  assert(!conditionRuntime.demand && !relayStates[0]);
  conditionSample.temperature = 275;
  updateRelay();
  conditionSample.temperature = 290;
  updateRelay();
  assert(conditionRuntime.demand && relayStates[0]);
  restart();
  fakeMillis = 30100;
  updateRelay();
  assert(!conditionResumePending && !conditionRuntime.demand && conditionEnd == 0);
  std::cout << "PASS: weekdays, overnight, 24h, cycles, union, validation, migration, "
               "reload, next transitions, DST, temporary resume, rollover, UART frames, "
               "power loss before NTP, original deadlines, atomic storage, no repeated "
               "writes, corrupt recovery rejection, no destructive reformat, lost "
               "startup frames, controller-only reset and refresh rollover\n";
  std::cout << "PASS: condition EEPROM isolation, PROG/window union, OFF/ON priority, "
               "legacy recovery migration, durable original conditional deadlines, "
               "fresh measurements required after power loss and persisted rearm latch\n";
}

"""Exercise the actual OLED rendering and FLASH debounce without hardware."""

import os
from pathlib import Path
import subprocess
import tempfile


root = Path(__file__).resolve().parents[1]
source = (root / "ESP8266_OLED_Hello_modif_OTA.ino").read_text(encoding="utf-8")
backup = (root.parent / "ESP8266_OLED_Hello" / "ESP8266_OLED_Hello.ino").read_text(encoding="utf-8")


def section(text, start, end):
    return text[text.index(start):text.index(end)]


functions = section(source, "void handleOledButton()", "void setup()")
assert functions == section(backup, "void handleOledButton()", "void setup()")
assert 'drawNetworkStatus' not in source
assert 'F("IP: ")' not in functions
globals_code = section(source, "const uint8_t OLED_BUTTON_PIN", "const uint32_t HISTORY_MAGIC")
soil = section(source, "const __FlashStringHelper* soilCondition", "int soilMoisturePercent()")
age = section(source, "int calendarDay(", "void handleGrowDates()")
assert age == section(backup, "int calendarDay(", "void handleGrowDates()")
types = section(source, "struct GrowDates", "#define SCREEN_WIDTH")
for text in (source, backup):
    assert 'configTime("CET-1CEST,M3.5.0/2,M10.5.0/3"' in text
    assert 'id="germinationAge"' in text

test = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <cstdio>
#include <string>
#include <vector>
using std::isnan;
using __FlashStringHelper = char;
#define F(value) value
const bool HIGH = true, LOW = false;
uint32_t ticks = 0;
bool button = HIGH;
uint32_t millis() { return ticks; }
bool digitalRead(uint8_t pin) { assert(pin == 0); return button; }
time_t wallClock = 1792324800;
int parisOffset = 7200;
time_t fakeTime(time_t*) { return wallClock; }
tm* parisLocalTime(const time_t* now, tm* date) {
  time_t shifted = *now + parisOffset;
  const tm* result = std::gmtime(&shifted);
  if (!result) return nullptr;
  *date = *result;
  return date;
}
#define time fakeTime
#define localtime_r parisLocalTime
float temperature = 23.4f, humidity = 60.5f;
int soilRaw = 900;
int soilMoisturePercent() { return soilRaw <= 300 ? 100 : soilRaw >= 900 ? 0 : 50; }
struct Screen {
  int x = 0, y = 0, size = 0;
  std::vector<std::string> rows;
  void clearDisplay() { rows.assign(4, ""); }
  void setTextSize(int value) { size = value; assert(size == 2); }
  void setTextWrap(bool value) { assert(!value); }
  void setCursor(int left, int top) { x = left; y = top; assert(y >= 0 && y + 16 <= 64); }
  void print(const char* text) {
    x += std::string(text).size() * 6 * size;
    assert(x <= 128);
    rows[y / 16] += text;
  }
  void print(int value) { print(std::to_string(value).c_str()); }
  void print(float value, int) { char text[32]; snprintf(text, sizeof(text), "%.1f", value); print(text); }
  void display() {}
} display;
struct Logger {
  void println(const char*) {}
} Serial;
struct File {
  std::shared_ptr<std::vector<uint8_t>> bytes;
  bool writable = false;
  explicit operator bool() const { return bool(bytes); }
  size_t size() const { return bytes ? bytes->size() : 0; }
  size_t read(uint8_t* target, size_t count) {
    if (!bytes) return 0;
    count = count < bytes->size() ? count : bytes->size();
    memcpy(target, bytes->data(), count);
    return count;
  }
  size_t write(const uint8_t* data, size_t count) {
    if (!bytes || !writable) return 0;
    bytes->assign(data, data + count);
    return count;
  }
  void close() {}
};
struct Filesystem {
  std::map<std::string, std::shared_ptr<std::vector<uint8_t>>> files;
  bool failOpen = false, failWrite = false, failRename = false, corruptReadback = false;
  bool exists(const char* name) { return files.count(name) != 0; }
  File open(const char* name, const char* mode) {
    if (failOpen) return {};
    const bool write = mode[0] == 'w';
    if (write) files[name] = std::make_shared<std::vector<uint8_t>>();
    if (!exists(name)) return {};
    if (!write && corruptReadback && std::string(name) == "/grow-dates.tmp" && !files[name]->empty()) {
      (*files[name])[0] ^= 1;
    }
    File file;
    file.bytes = files[name];
    file.writable = write && !failWrite;
    return file;
  }
  bool rename(const char* from, const char* to) {
    if (failRename) return false;
    files[to] = files[from];
    files.erase(from);
    return true;
  }
} LittleFS;
'''
test += types + globals_code + soil + age + functions
test += r'''
void sample(uint32_t now, bool level) { ticks = now; button = level; handleOledButton(); }
int main() {
  growDatesReady = loadGrowDates();
  assert(growDatesReady);
  assert(parseGrowDate("2028-02-29") >= 0);
  for (const char* invalid : {"", "2026-02-29", "2026-04-31", "2026-13-01",
      "2026-00-01", "2026-01-00", "2026/10/08", "2026-1a-08", "1969-01-01",
      "2100-01-01", "2026-10-080", "26-10-08"}) assert(parseGrowDate(invalid) == -1);
  GrowDates next = {"2026-10-09", "2026-10-15"};
  assert(validGrowDates(next));
  assert(saveGrowDates(next));
  growDates = {};
  assert(loadGrowDates());
  assert(memcmp(&growDates, &next, sizeof(next)) == 0);
  assert(germinationDays(wallClock) == 9);
  assert(daysSinceGrowDate(wallClock, growDates.flowering) == 3);
  GrowDates previous = growDates;
  next = {"2026-10-08", ""};
  LittleFS.failOpen = true;
  assert(!saveGrowDates(next));
  LittleFS.failOpen = false;
  LittleFS.failWrite = true;
  assert(!saveGrowDates(next));
  LittleFS.failWrite = false;
  LittleFS.failRename = true;
  assert(!saveGrowDates(next));
  LittleFS.failRename = false;
  LittleFS.corruptReadback = true;
  assert(!saveGrowDates(next));
  LittleFS.corruptReadback = false;
  growDates = {};
  assert(loadGrowDates() && memcmp(&growDates, &previous, sizeof(previous)) == 0);
  (*LittleFS.files["/grow-dates.bin"])[0] = 'x';
  assert(!loadGrowDates());
  assert(saveGrowDates(next) && loadGrowDates());
  assert(daysSinceGrowDate(wallClock, "") == -1);
  assert(daysSinceGrowDate(wallClock, "2026-10-19") == -1);
  next = {"2026-10-08", "2026-10-07"};
  assert(!validGrowDates(next));
  next = {"2026-10-08", "2026-10-08"};
  assert(validGrowDates(next));
  growDatesReady = false;
  assert(germinationDays(wallClock) == -1);
  growDatesReady = true;
  assert(germinationDays(0) == -1);
  assert(germinationDays(1791410399) == -1); // Oct 7, 23:59:59 Paris.
  assert(germinationDays(1791410400) == 0); // Oct 8 midnight Paris.
  assert(germinationDays(1792274399) == 9); // Oct 17, 23:59:59 Paris.
  assert(germinationDays(1792274400) == 10); // Oct 18 midnight Paris.
  assert(germinationDays(wallClock) == 10);
  assert(germinationDays(1792886399) == 17); // Before autumn DST change.
  parisOffset = 3600;
  assert(germinationDays(1792890000) == 17); // After autumn DST change.
  assert(germinationDays(1792969200) == 18); // Oct 26 midnight Paris.
  assert(germinationDays(1798758000) == 85); // Jan 1, 2027 Paris.
  assert(germinationDays(1835391600) == 509); // Feb 29, 2028 Paris.
  assert(germinationDays(1835478000) == 510); // Mar 1, 2028 Paris.
  parisOffset = 7200;
  drawOledPage();
  assert(display.rows[0] == "23.4 C" && display.rows[1] == "60.5 %");
  assert(display.rows[2].empty() && display.rows[3] == "J+10");
  sample(10, LOW); sample(20, HIGH); sample(25, LOW); sample(64, LOW);
  assert(!oledSoilPage);
  oledRedraw = false;
  sample(65, LOW);
  assert(oledSoilPage && oledRedraw);
  sample(3000, LOW);
  assert(oledSoilPage);
  drawOledPage();
  assert(display.rows[1] == "0 %");
  assert(display.rows[0].empty() && display.rows[2].empty() && display.rows[3] == "J+10");
  wallClock = 0;
  drawOledPage();
  assert(display.rows[3] == "J+--");
  wallClock = 1792324800;
  sample(3001, HIGH); sample(3041, HIGH); sample(3050, LOW); sample(3090, LOW);
  assert(!oledSoilPage);
  temperature = -40; humidity = 100;
  drawOledPage();
  temperature = NAN; humidity = NAN;
  drawOledPage();
  assert(display.rows[0] == "Erreur" && display.rows[1] == "Erreur");
  oledSoilPage = true;
  for (int raw : {0, 300, 330, 331, 400, 401, 847, 848, 900, 1023}) {
    soilRaw = raw;
    drawOledPage();
  }
  soilRaw = 350; drawOledPage();
  assert(display.rows[1] == "50 %");
  assert(std::string(soilCondition()) == "Tres humide");
  oledSoilPage = false;
  oledButtonReading = oledButtonStable = HIGH;
  sample(UINT32_MAX - 20, LOW);
  sample(18, LOW); assert(!oledSoilPage);
  sample(19, LOW); assert(oledSoilPage);
  puts("PASS: germination J+0/J+10, midnight, DST, year/leap day, unknown clock, both pages, size-2 128x64 bounds, errors, debounce, hold, release, rollover and identical backup");
}
'''
compiler = Path(r"C:\msys64\mingw64\bin\g++.exe")
os.environ["PATH"] = str(compiler.parent) + os.pathsep + os.environ["PATH"]
with tempfile.TemporaryDirectory(prefix="grow-oled-tests-") as directory:
    cpp = Path(directory) / "oled_test.cpp"
    executable = Path(directory) / "oled_test.exe"
    cpp.write_text(test, encoding="utf-8")
    subprocess.run([str(compiler), "-std=c++11", "-Wall", "-Wextra", "-Werror",
                    str(cpp), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True)

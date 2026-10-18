#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266mDNS.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <time.h>
#include <stdlib.h>

struct GrowDates {
  char germination[11];
  char flowering[11];
};

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C
#define DHT_PIN D4
#define DHT_TYPE DHT22
#define SOIL_SENSOR_PIN A0
#define SOIL_DRIER_REFERENCE 900
#define SOIL_WETTEST_REFERENCE 300

#include "secrets.h"

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
DHT dht(DHT_PIN, DHT_TYPE);
ESP8266WebServer server(80);
IPAddress relayAddress;
bool relayAddressKnown = false;
bool relayLookupAttempted = false;
unsigned long lastRelayLookup = 0;
bool dashboardMdnsStarted = false;
bool dashboardOtaStarted = false;

void beginDashboardOTA() {
  if (dashboardOtaStarted || WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (!dashboardMdnsStarted) {
    dashboardMdnsStarted = MDNS.begin("grow-dashboard-ota");
    if (dashboardMdnsStarted) {
      MDNS.addService("http", "tcp", 80);
    }
  }
  if (!dashboardMdnsStarted) {
    Serial.println(F("OTA unavailable: mDNS could not start"));
    return;
  }

  ArduinoOTA.setHostname("grow-dashboard-ota");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println(F("OTA update started"));
  });
  ArduinoOTA.onEnd([]() {
    Serial.println(F("\nOTA update finished"));
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA progress: %u%%\r", (progress * 100) / total);
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA error [%u]\n", error);
  });
  ArduinoOTA.begin();
  dashboardOtaStarted = true;
  Serial.println(F("OTA ready at grow-dashboard-ota.local"));
}

float temperature = NAN;
float humidity = NAN;
int soilRaw = 0;
unsigned long lastSensorRead = 0;
bool sensorReadReady = false;
bool relaySensorsForwarded = false;
uint32_t lastRelaySensorAttempt = 0;
unsigned long lastDisplayUpdate = 0;
const uint8_t OLED_BUTTON_PIN = 0;
bool oledSoilPage = false;
bool oledButtonReading = HIGH;
bool oledButtonStable = HIGH;
bool oledRedraw = true;
uint32_t oledButtonChangedAt = 0;
GrowDates growDates = {"2026-10-08", ""};
bool growDatesReady = false;
const uint32_t HISTORY_MAGIC = 0x47524F32;
const uint32_t LEGACY_HISTORY_MAGIC = 0x47524F57;
const uint32_t MONTH_HISTORY_MAGIC = 0x47524D31;
const uint32_t RECORDS_MAGIC = 0x47525231;
const uint32_t WATERING_MAGIC = 0x47525732;
const uint32_t LEGACY_WATERING_MAGIC = 0x47525731;
const uint16_t HISTORY_CAPACITY = 288;
const uint32_t HISTORY_INTERVAL_SECONDS = 300;
const uint16_t MONTH_HISTORY_CAPACITY = 720;
const uint32_t MONTH_HISTORY_INTERVAL_SECONDS = 3600;
const uint16_t WATERING_CAPACITY = 256;

struct HistorySample {
  uint32_t timestamp;
  int16_t temperatureTenths;
  uint16_t humidityTenths;
  uint8_t soilPercent;
};

struct LegacyHistorySample {
  uint32_t timestamp;
  int16_t temperatureTenths;
  uint16_t humidityTenths;
};

struct HistoryStore {
  uint32_t magic;
  uint16_t head;
  uint16_t count;
  HistorySample samples[HISTORY_CAPACITY];
};

struct LegacyHistoryStore {
  uint32_t magic;
  uint16_t head;
  uint16_t count;
  LegacyHistorySample samples[HISTORY_CAPACITY];
};

struct MonthHistoryStore {
  uint32_t magic;
  uint16_t head;
  uint16_t count;
  HistorySample samples[MONTH_HISTORY_CAPACITY];
};

struct AllTimeRecords {
  uint32_t magic;
  uint32_t sampleCount;
  int16_t temperatureMinTenths;
  int16_t temperatureMaxTenths;
  uint16_t airHumidityMinTenths;
  uint16_t airHumidityMaxTenths;
  uint8_t soilMoistureMin;
  uint8_t soilMoistureMax;
  uint32_t soilSampleCount;
};

struct WateringStore {
  uint32_t magic;
  uint16_t head;
  uint16_t count;
  uint32_t timestamps[WATERING_CAPACITY];
  uint8_t fertilizer[WATERING_CAPACITY];
};

struct LegacyWateringStore {
  uint32_t magic;
  uint16_t head;
  uint16_t count;
  uint32_t timestamps[WATERING_CAPACITY];
};

HistoryStore historyStore = {};
MonthHistoryStore monthHistoryStore = {};
AllTimeRecords allTimeRecords = {};
WateringStore wateringStore = {};
bool saveAllTimeRecords();

const char PAGE[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="fr">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <meta name="theme-color" content="#07140b">
  <title>Grow Guardian | Suivi du potager</title>
  <style>
    :root{color-scheme:dark;--bg:#07140b;--card:#102318;--muted:#a1b9a4;--text:#efffea;--mint:#8cff45;--leaf:#36ec75;--gold:#d6ff70}
    *{box-sizing:border-box}
    body{margin:0;min-height:100vh;padding:32px 18px;color:var(--text);font:16px/1.5 system-ui,-apple-system,"Segoe UI",sans-serif;background:radial-gradient(ellipse at 12% 0%,#55ff2933 0,transparent 42%),radial-gradient(ellipse at 90% 70%,#00bd5933 0,transparent 42%),repeating-linear-gradient(135deg,#ffffff03 0 1px,transparent 1px 13px),var(--bg)}
    main{display:flex;flex-direction:column;max-width:760px;margin:0 auto}
    main>header{order:0}
    main>.grid,main>.seed-bank{order:2}
    main>.phase-panel{order:1}
    .seed-bank{margin:0 0 18px;padding:16px 18px;border:1px solid #8cff4544;border-radius:18px;background:#102318}
    .seed-bank summary{cursor:pointer;font-weight:750;color:var(--mint)}
    .seed-bank a{color:var(--mint)}
    main>.chart-panel,main>.combined-panel{order:3}
    main>.light-panel{order:4}
    main>footer{order:5}
    main>.plant-section{order:6}
    header{display:flex;align-items:center;justify-content:space-between;gap:16px;margin:14px 0 30px}
    .brand{display:flex;align-items:center;gap:16px}
    .leaf-mark{display:grid;width:76px;height:76px;flex:none;place-items:center;border:1px solid #8cff4580;border-radius:24px;background:linear-gradient(145deg,#8cff4530,#00bd5910);box-shadow:0 0 30px #45ff3930,inset 0 0 20px #45ff3912}
    .leaf-mark svg{width:68px;height:68px;filter:drop-shadow(0 0 8px #8cff4570)}
    .eyebrow{margin:0 0 4px;color:var(--mint);font-size:.75rem;font-weight:850;letter-spacing:.2em;text-transform:uppercase}
    h1{margin:0;font-size:clamp(1.65rem,5vw,2.3rem);letter-spacing:-.04em;text-shadow:0 0 22px #59ff3a2b}
    .connection-status{display:flex;flex-wrap:wrap;justify-content:flex-end;gap:8px}
    .status{display:flex;align-items:center;gap:9px;padding:9px 13px;border:1px solid #8cff4544;border-radius:999px;background:#8cff4510;color:var(--muted);font-size:.83rem;white-space:nowrap}
    .dot{width:9px;height:9px;border-radius:50%;background:#718096;box-shadow:0 0 0 4px #71809622}
    .status.online .dot{background:var(--mint);box-shadow:0 0 0 4px #8cff4522,0 0 12px #8cff45}
    .phase-content{display:grid;gap:16px;margin-top:16px}
    .phase-copy strong{display:block;color:var(--text);font-size:.92rem}
    .phase-copy span{display:block;margin-top:2px;color:var(--muted);font-size:.78rem}
    .phase-select{min-width:145px;padding:10px 34px 10px 12px;border:1px solid #8cff4544;border-radius:12px;background:#07140b;color:var(--mint);font:inherit;font-weight:650}
    .grow-dates{display:flex;flex-wrap:wrap;align-items:end;gap:12px;width:100%}
    .grow-dates label{display:grid;gap:5px;font-size:.8rem;flex:1;min-width:180px}
    .grow-dates input,.grow-dates button{box-sizing:border-box;min-width:0;max-width:100%;padding:10px;border:1px solid #8cff4544;border-radius:10px;background:#07140b;color:var(--mint);font:inherit}
    .grow-dates select{box-sizing:border-box;min-width:0;width:100%}
    .grow-dates p{width:100%;margin:0;font-size:.8rem}
    .grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:16px}
    .card{position:relative;overflow:hidden;min-height:230px;padding:24px;border:1px solid #8cff4538;border-radius:24px;background:linear-gradient(145deg,#8cff4518,transparent 72%),var(--card);box-shadow:0 18px 55px #0006,0 0 24px #36ec7510}
    .metric-card{cursor:pointer;transition:transform .2s,border-color .2s,box-shadow .2s}
    .metric-card:hover,.metric-card:focus-visible{transform:translateY(-3px);border-color:#8cff4580;box-shadow:0 18px 55px #0006,0 0 28px #36ec7525;outline:none}
    .chart-panel{margin:18px 0 0;padding:20px;border:1px solid #8cff4538;border-radius:22px;background:linear-gradient(145deg,#8cff4510,transparent 70%),var(--card)}
    .chart-panel[hidden]{display:none}
    .chart-heading{display:flex;align-items:center;justify-content:space-between;gap:12px}
    .chart-heading h2{margin:0;font-size:1rem}
    .chart-close{width:34px;height:34px;border:1px solid #8cff4533;border-radius:10px;background:#8cff4512;color:var(--mint);font-size:1.2rem;cursor:pointer}
    .chart-summary{margin:6px 0 12px;color:var(--muted);font-size:.82rem}
    .chart-wrap{position:relative;width:100%;overflow:hidden}
    .history-chart{display:block;width:100%;height:auto;min-height:180px;overflow:visible}
    .chart-grid{stroke:#ffffff18;stroke-width:1}
    .chart-label{fill:#a1b9a4;font:12px system-ui,sans-serif}
    .chart-line{fill:none;stroke:#8cff45;stroke-width:3;stroke-linecap:round;stroke-linejoin:round}
    .chart-area{fill:url(#chartFill);opacity:.35}
    .chart-dot{fill:var(--chart-point-color,#d6ff70);stroke:#102318;stroke-width:2}
    .chart-empty{fill:#a1b9a4;font:14px system-ui,sans-serif;text-anchor:middle}
    .history-launch-card{grid-column:1/-1;display:flex;align-items:center;justify-content:space-between;min-height:96px;width:100%;color:var(--text);font:inherit;text-align:left;cursor:pointer}
    .history-copy{display:block;margin:0;color:var(--text)}
    .history-launch-card strong{display:block;font-size:1rem;font-weight:750;color:var(--mint)}
    .history-launch-card .icon{display:grid;flex:0 0 38px;width:38px;height:38px;margin:0;place-items:center;font-size:1.2rem}
    .light-panel{margin:18px 0 0;padding:20px;border:1px solid #8cff4538;border-radius:22px;background:linear-gradient(145deg,#8cff4510,transparent 70%),var(--card)}
    .light-heading{display:flex;align-items:center;justify-content:space-between;gap:12px}
    .light-heading{list-style:none;cursor:pointer}
    .light-heading::-webkit-details-marker{display:none}
    .light-heading::after{content:"+";display:grid;flex:none;width:30px;height:30px;place-items:center;border:1px solid #8cff4544;border-radius:10px;color:var(--mint);font-size:1.25rem}
    .light-panel[open]>.light-heading::after{content:"−"}
    .light-heading:focus-visible{outline:2px solid var(--mint);outline-offset:4px;border-radius:6px}
    .light-heading h2{margin:0;font-size:1rem;font-weight:750;color:var(--mint)}
    .light-connection,.light-state{margin:6px 0 0;color:var(--muted);font-size:.84rem}
    .program-help{margin:6px 0 14px;color:var(--muted);font-size:.8rem}
    .program-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:12px}
    .program-card{min-width:0;padding:14px;border:1px solid #8cff4530;border-radius:16px;background:#07140b88}
    .program-card h3{margin:0 0 10px;font-size:.95rem}
    .program-card .light-state{min-height:2.5em}
    .light-form{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:9px;margin-top:12px}
    .light-form label{display:grid;gap:6px;color:var(--muted);font-size:.82rem}
    .light-form input,.light-form select{width:100%;min-width:0;padding:9px 10px;border:1px solid #8cff4544;border-radius:10px;background:#07140b;color:var(--text);font:inherit}
    .cycle-fields{grid-column:1/-1;display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:9px}
    .light-controls{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px;margin-top:12px}
    .light-controls button{min-height:42px;border:1px solid #8cff4544;border-radius:10px;background:#8cff4518;color:var(--mint);font:inherit;font-weight:750;cursor:pointer}
    .light-controls button[aria-pressed="true"]{border-color:#8cff4580;background:#8cff4530}
    .light-controls button:disabled{opacity:.55;cursor:wait}
    .light-feedback{min-height:1.3em;margin:8px 2px 0;color:var(--muted);font-size:.82rem}
    .cycle-fields[hidden]{display:none}
    #relayClock{color:var(--mint);font-size:1rem;font-weight:750;white-space:normal}
    .schedule-slot{grid-column:1/-1;min-width:0;margin:0;padding:10px;border:1px solid #8cff4533;border-radius:12px}
    .schedule-slot>summary{display:flex;align-items:center;justify-content:space-between;gap:10px;list-style:none;cursor:pointer;color:var(--mint);font-weight:650}
    .schedule-slot>summary::-webkit-details-marker{display:none}
    .schedule-slot>summary::after{content:"+";font-size:1.2rem}
    .schedule-slot[open]>summary::after{content:"-"}
    .schedule-slot>summary:focus-visible{outline:2px solid var(--mint);outline-offset:3px;border-radius:4px}
    .schedule-slot-body{margin-top:10px}
    #conditionDurationField[hidden]{display:none}
    .schedule-slot .light-form{margin-top:8px}
    .schedule-slot input[type=checkbox]{width:auto;accent-color:var(--mint)}
    .schedule-days{display:flex;flex-wrap:wrap;gap:8px;margin:8px 0}
    .schedule-days label{display:flex;align-items:center;gap:3px;font-size:.78rem}
    .program-countdown{color:var(--gold);font-weight:650;font-size:.85rem}
    .temporary-controls{display:flex;flex-wrap:wrap;gap:8px;align-items:center;margin-top:12px}
    .temporary-controls input{width:75px;padding:8px;background:var(--bg);color:var(--text);border:1px solid #8cff4544;border-radius:8px}
    .temporary-controls button{padding:8px;border:1px solid #8cff4544;border-radius:8px;background:#8cff4518;color:var(--mint);cursor:pointer}
    @media(max-width:600px){.program-grid{grid-template-columns:1fr}}
    @media(max-width:520px){.light-form{grid-template-columns:repeat(2,minmax(0,1fr))}.light-controls{grid-template-columns:repeat(3,minmax(0,1fr))}}
    .combined-panel{margin:18px 0 0;padding:20px;border:1px solid #8cff4538;border-radius:22px;background:linear-gradient(145deg,#8cff4510,transparent 70%),var(--card)}
    .combined-panel[hidden]{display:none}
    .combined-controls{display:flex;align-items:center;gap:10px}
    .range-select{padding:8px 30px 8px 10px;border:1px solid #8cff4544;border-radius:10px;background:#07140b;color:var(--mint);font:inherit}
    .combined-chart{display:block;width:100%;height:auto;overflow:visible}
    .combined-line{fill:none;stroke-width:3;stroke-linecap:round;stroke-linejoin:round}
    .watering-marker{stroke-width:2;stroke-dasharray:5 4}
    .watering-marker-simple{stroke:#ffad91}
    .watering-marker-fertilizer{stroke:#d6ff70}
    .watering-marker-dot{stroke:#102318;stroke-width:2}
    .watering-marker-dot-simple{fill:#ffad91}
    .watering-marker-dot-fertilizer{fill:#d6ff70}
    .combined-temp{stroke:#f1d47a}
    .combined-air{stroke:#73baff}
    .combined-soil{stroke:#8cff45}
    .combined-point{stroke:#07140b;stroke-width:2}
    .combined-temp.combined-point{fill:#f1d47a}
    .combined-air.combined-point{fill:#73baff}
    .combined-soil.combined-point{fill:#8cff45}
    .combined-legend{display:flex;flex-wrap:wrap;gap:10px 18px;margin:12px 0 0;color:var(--text);font-size:.82rem}
    .combined-legend span{display:flex;align-items:center;gap:7px}
    .legend-dot{width:9px;height:9px;border-radius:50%}
    .legend-temp{background:#f1d47a}.legend-air{background:#73baff}.legend-soil{background:#8cff45}
    .watering-form{display:grid;grid-template-columns:minmax(0,1fr) minmax(0,.8fr) auto;align-items:end;gap:10px;margin-top:18px;padding:15px;border:1px solid #8cff4528;border-radius:16px;background:#07140b55}
    .watering-form label{display:grid;gap:6px;color:var(--muted);font-size:.82rem}
    .watering-form input,.watering-form select{width:100%;min-width:0;padding:9px 10px;border:1px solid #8cff4544;border-radius:10px;background:#07140b;color:var(--text);font:inherit}
    .watering-form button{min-height:40px;padding:8px 14px;border:1px solid #8cff4544;border-radius:10px;background:#8cff4518;color:var(--mint);font:inherit;font-weight:700;cursor:pointer}
    .watering-feedback{min-height:1.3em;margin:8px 2px 0;color:var(--muted);font-size:.82rem}
    .watering-list{margin:4px 0 0;padding-left:20px;color:var(--muted);font-size:.82rem}
    .all-time-records{margin-top:18px;padding:15px;border:1px solid #8cff4528;border-radius:16px;background:#07140b55}
    .records-heading{display:flex;align-items:center;justify-content:space-between;gap:12px}
    .records-heading h3{margin:0 0 10px;font-size:.95rem}
    .records-reset{margin-bottom:10px;padding:7px 10px;border:1px solid #ffad9166;border-radius:9px;background:#ffad9112;color:#ffad91;font:inherit;font-size:.78rem;font-weight:650;cursor:pointer}
    .records-reset:hover{background:#ffad9128}
    .all-time-records h3{margin:0 0 10px;font-size:.95rem}
    .records-grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:10px}
    .record-item{display:grid;gap:3px;color:var(--muted);font-size:.78rem}
    .record-item strong{color:var(--text);font-size:.82rem}
    @media(max-width:520px){.watering-form{grid-template-columns:1fr}.records-grid{grid-template-columns:1fr}}
    .card:after{position:absolute;right:-36px;bottom:-68px;width:175px;height:175px;border:1px solid #8cff4527;border-radius:50%;content:""}
    .card-top{position:relative;z-index:1;display:flex;align-items:center;justify-content:space-between}
    .label{color:var(--muted);font-size:.9rem}
    .icon{display:grid;width:38px;height:38px;place-items:center;border-radius:13px;background:#8cff451a;color:var(--mint);font-size:1.2rem}
    .value{margin:23px 0 4px;font-size:clamp(2.5rem,10vw,4rem);font-weight:720;letter-spacing:-.07em;line-height:1}
    .unit{font-size:1.25rem;font-weight:550;letter-spacing:0}
    .hint{margin:12px 0 0;color:var(--muted);font-size:.86rem}
    .track{height:6px;margin-top:21px;overflow:hidden;border-radius:20px;background:#ffffff12}
    .bar{height:100%;width:0;border-radius:inherit;transition:width .6s ease}
    .temperature .icon{color:#f1c95c}.temperature .value{color:#f1d47a;text-shadow:0 0 20px #f1c95c36}.temperature .bar{background:linear-gradient(90deg,#36ec75,#d6ff70)}
    .humidity .icon{color:#73ff9a}.humidity .value{color:#8cffaa;text-shadow:0 0 20px #36ec7540}.humidity .bar{background:linear-gradient(90deg,#00bd59,#8cff45)}
    .soil-card{grid-column:1/-1;min-height:175px}
    .soil-card .value{display:flex;align-items:baseline;gap:10px;margin:14px 0 4px;color:#b9dcff;font-size:clamp(2rem,8vw,3.2rem);text-shadow:0 0 20px #73baff36}
    .soil-card{--soil-color:#b9dcff}
    .soil-card .soil-percent-reading,.soil-card .soil-status-label{color:var(--soil-color);text-shadow:none}
    .soil-card .unit{font-size:1.4rem}
    .soil-card .track{margin-top:14px}
    .soil-card .bar{background:linear-gradient(90deg,#ff453a 0%,#ff8c42 25%,#ffe34d 50%,#51df74 75%,#3d9dff 100%)}
    .soil-status-label{font-size:clamp(1.35rem,5vw,2rem);letter-spacing:-.03em;text-transform:uppercase}
    .soil-card .icon{color:#9bcaff}
    .soil-alert[hidden]{display:none}
    .soil-alert{position:fixed;inset:0;z-index:10;display:grid;place-items:center;padding:20px;background:#000b}
    .soil-alert-card{width:min(100%,390px);padding:26px;border:1px solid #ff8f6b88;border-radius:22px;background:linear-gradient(145deg,#ff7b3b20,transparent 70%),#102318;box-shadow:0 20px 70px #000a;text-align:center}
    .soil-alert-card h2{margin:0 0 8px;color:#ffad91;font-size:1.5rem}
    .soil-alert-card p{margin:0 0 20px;color:var(--text)}
    .soil-alert-card button{padding:10px 20px;border:1px solid #ffad9180;border-radius:12px;background:#ffad9120;color:var(--text);font:inherit;font-weight:700;cursor:pointer}
    footer{display:flex;justify-content:space-between;gap:12px;margin:18px 4px;color:var(--muted);font-size:.82rem}
    #updated{color:#d4deeb}
    .error{color:#ffad9a!important}
    .plant-section{position:relative;display:flex;flex-direction:column;align-items:center;margin:40px auto 0;padding:24px 18px 10px;overflow:hidden;border:1px solid #8cff4528;border-radius:26px;background:radial-gradient(ellipse at 50% 70%,#36ec7514,transparent 55%),linear-gradient(145deg,#8cff4509,transparent 65%),#0c1d12}
    .plant-heading{align-self:stretch;display:flex;align-items:center;justify-content:space-between;gap:12px}
    .plant-heading h2{margin:0;color:var(--text);font-size:1rem;font-weight:650;letter-spacing:.02em}
    .grow-status{padding:6px 10px;border:1px solid #8cff4533;border-radius:999px;color:var(--mint);font-size:.74rem;letter-spacing:.04em}
    .plant-art{display:block;width:min(100%,280px);height:auto;margin:4px auto 0;overflow:visible}
    .stage-controls{display:flex;align-items:center;justify-content:center;gap:12px;margin:0 auto 12px}
    .stage-controls label{color:var(--muted);font-size:.85rem}
    .stage-stepper{display:flex;align-items:center;gap:8px;padding:5px;border:1px solid #8cff4533;border-radius:14px;background:#07140b88}
    .stage-stepper button{width:34px;height:34px;border:0;border-radius:10px;background:#8cff4518;color:var(--mint);font-size:1.25rem;cursor:pointer}
    .stage-stepper button:hover{background:#8cff4530}
    .stage-stepper input{width:42px;border:0;outline:0;background:transparent;color:var(--text);font:inherit;font-weight:700;text-align:center}
    .stage-stepper input::-webkit-inner-spin-button,.stage-stepper input::-webkit-outer-spin-button{-webkit-appearance:none;margin:0}
    .stage-stepper input[type=number]{-moz-appearance:textfield}
    .stage-count{min-width:48px;color:var(--mint);font-size:.82rem}
    .plant-growth{transform-box:fill-box;transform-origin:50% 100%;animation:plant-grow 5s cubic-bezier(.2,.75,.25,1) both}
    .plant-leaf{transform-box:fill-box;transform-origin:50% 100%;animation:leaf-sway 4s ease-in-out infinite alternate}
    .plant-leaf.delay-1{animation-delay:-1.2s}
    .plant-leaf.delay-2{animation-delay:-2.4s}
    .plant-leaf.delay-3{animation-delay:-.7s}
    @keyframes plant-grow{from{transform:scaleY(.04);opacity:.35}to{transform:scaleY(1);opacity:1}}
    @keyframes leaf-sway{from{transform:rotate(-2deg)}to{transform:rotate(2deg)}}
    @media(prefers-reduced-motion:reduce){.plant-growth,.plant-leaf{animation:none}}
    @media(max-width:520px){.phase-select{width:100%}}
    @media(max-width:520px){body{padding:20px 14px}header{align-items:flex-start}.brand{gap:10px}.leaf-mark{width:58px;height:58px;border-radius:18px}.leaf-mark svg{width:54px;height:54px}.grid{gap:11px}.card{min-height:205px;padding:18px;border-radius:20px}.status{padding:8px 10px;font-size:.75rem}.value{margin-top:22px}footer{font-size:.75rem}}
    @media(max-width:370px){header{flex-direction:column}.grid{grid-template-columns:1fr}.card{min-height:190px}}
  </style>
</head>
<body>
  <main>
    <header>
      <div class="brand">
        <div><p class="eyebrow">Suivi du potager</p><h1>Grow Guardian</h1></div>
      </div>
      <div class="connection-status" aria-label="Etat des connexions">
        <div class="status" id="dashboardStatus" role="status" aria-label="Dashboard : connexion en cours"><span class="dot" aria-hidden="true"></span><span>Dashboard</span></div>
        <div class="status" id="relayStatus" role="status" aria-label="Relai : connexion en cours"><span class="dot" aria-hidden="true"></span><span>Relai</span></div>
      </div>
    </header>
    <details class="seed-bank phase-panel" aria-label="Phase suivie">
      <summary>Phase suivie</summary>
      <div class="phase-content">
      <div class="phase-copy">
        <strong id="germinationAge" aria-live="polite">J+-- depuis germination</strong>
        <strong id="growthRemaining" aria-live="polite">Choisir une graine pour estimer la croissance restante.</strong>
        <strong id="floweringForecast" aria-live="polite">Passage en floraison previsionnel : choisir une graine.</strong>
        <strong id="floweringAge" aria-live="polite">Floraison non renseignee</strong>
        <strong id="floweringRemaining" aria-live="polite">Choisir une graine pour estimer la floraison restante.</strong>
      </div>
      <form id="growDatesForm" class="grow-dates">
        <label>Germination<input id="germinationDate" name="germination" type="date" min="1970-01-01" max="2099-12-31" required></label>
        <label>Passage en floraison<input id="floweringDate" name="flowering" type="date" min="1970-01-01" max="2099-12-31"></label>
        <button id="growDatesSave" type="submit" disabled>Enregistrer les dates</button>
        <p id="growDatesMessage" role="status">Chargement des dates...</p>
      </form>
      <div class="grow-dates">
        <label>Graine suivie<select id="trackedSeed" class="phase-select" disabled><option value="">Aucune graine selectionnee</option></select></label>
        <button id="reloadTrackedSeeds" type="button">Actualiser les fiches</button>
        <p id="trackedSeedMessage" role="status">Chargement des fiches...</p>
      </div>
      </div>
    </details>
    <details class="seed-bank">
      <summary>SEED BANK</summary>
      <p>Catalogue personnel : nom, variete, croissance et floraison indicatives.</p>
      <a href="/seed-bank">Ouvrir SEED BANK · ajouter / modifier les fiches</a>
    </details>
    <section class="grid" aria-label="Mesures du capteur">
      <article class="card metric-card temperature" id="temperatureCard" role="button" tabindex="0" aria-label="Afficher le graphique de temperature sur 24 heures" aria-expanded="false">
        <div class="card-top"><span class="label">Temperature</span><span class="icon" aria-hidden="true">&#9728;</span></div>
        <div class="value"><span id="temperature">--.-</span><span class="unit"> &deg;C</span></div>
        <p class="hint" id="temperatureHint">En attente du capteur</p>
        <div class="track"><div class="bar" id="temperatureBar"></div></div>
      </article>
      <article class="card metric-card humidity" id="humidityCard" role="button" tabindex="0" aria-label="Afficher le graphique d'humidite sur 24 heures" aria-expanded="false">
        <div class="card-top"><span class="label">Humidite</span><span class="icon" aria-hidden="true">&#128167;</span></div>
        <div class="value"><span id="humidity">--</span><span class="unit"> %</span></div>
        <p class="hint" id="humidityHint">En attente du capteur</p>
        <div class="track"><div class="bar" id="humidityBar"></div></div>
      </article>
      <article class="card metric-card soil-card" id="soilCard" role="button" tabindex="0" aria-label="Afficher le graphique d'humidite du sol sur 24 heures" aria-expanded="false" aria-controls="chartPanel">
        <div class="card-top"><span class="label">Etat du sol</span><span class="icon" aria-hidden="true">&#127806;</span></div>
        <div class="value soil-reading"><span class="soil-percent-reading"><span id="soilPercent">--</span><span class="unit">%</span></span><span class="soil-status-label" id="soilStatus">En attente</span></div>
        <p class="hint">Lecture Analogue : <span id="soilRaw">---</span>/1023</p>
        <div class="track"><div class="bar" id="soilBar"></div></div>
      </article>
      <button class="card history-launch-card" id="combinedHistoryOpen" type="button" aria-expanded="false" aria-controls="combinedChartPanel">
        <span class="history-copy"><strong>Graphique des mesures</strong></span>
        <span class="icon" aria-hidden="true">&#128202;</span>
      </button>
    </section>
    <details class="light-panel">
      <summary class="light-heading">
        <h2 id="lightTitle">Programmateur</h2>
        <span id="relayClock" class="label">Heure du relais en attente</span>
      </summary>
      <p class="light-connection" id="relayConnection">Recherche du module relais sur le Wi-Fi...</p>
      <details class="schedule-slot" id="conditionCard">
        <summary>Conditions</summary>
        <div class="schedule-slot-body">
          <p class="program-help">En mode PROG : plages OU condition. OFF ignore la condition, ON force la marche. Marges de retour : 0,5 °C et 2 %. Sans mesures pendant 30 s, seule la condition est annulee.</p>
          <form id="conditionForm" class="light-form">
            <label><span>Activer la regle</span><input type="checkbox" name="enabled"></label>
            <label>Relais a commander<select name="channel"><option value="1">Relais 1</option><option value="2" selected>Relais 2</option><option value="3">Relais 3</option><option value="4">Relais 4</option></select></label>
            <label><span>Utiliser la temperature</span><input type="checkbox" name="temperatureEnabled" checked></label>
            <label>Temperature superieure a (°C)<input name="temperatureLimit" type="number" min="-40" max="80" step="0.1" value="28" required></label>
            <label><span>Utiliser l'humidite</span><input type="checkbox" name="humidityEnabled"></label>
            <label>Humidite<select name="humiditySource"><option value="0">Air (DHT22)</option><option value="1">Sol (YL-69)</option></select></label>
            <label>Humidite superieure a (%)<input name="humidityLimit" type="number" min="0" max="100" step="0.1" value="70" required></label>
            <label>Combinaison<select name="logic"><option value="0">OU : un seuil suffit</option><option value="1">ET : les deux seuils</option></select></label>
            <label>Action<select name="action"><option value="0">Tant que le seuil est depasse</option><option value="1">Duree fixe au franchissement</option></select></label>
            <label id="conditionDurationField" hidden>Duree (min)<input name="duration" type="number" min="1" max="1440" value="5" required></label>
            <div class="temporary-controls"><button type="submit" id="conditionSave" disabled>Enregistrer les conditions</button></div>
          </form>
          <p class="light-state" id="conditionState">En attente du relais...</p>
          <p class="program-feedback" id="conditionFeedback" role="status"></p>
        </div>
      </details>
      <p class="program-help">Chaque fonction a son propre relais, mode et programme. En mode PROG, debut = fin signifie toute la journee ; en mode cyclique, le cycle repart a l'heure de debut. Choisissez un relais puis ON, PROG ou OFF pour enregistrer. Un relais déjà affecté échange son affectation avec l'autre fonction.</p>
      <div class="program-grid">
        <article class="program-card" data-task-card="light">
          <h3>Lumiere</h3>
          <p class="light-state" data-state></p>
          <form class="light-form" data-program-form="light">
            <label>Relais
              <select data-field="channel"><option value="1">Relais 1</option><option value="2">Relais 2</option><option value="3">Relais 3</option><option value="4">Relais 4</option></select>
            </label>
            <label>Programme
              <select data-field="type"><option value="window">Plage continue</option><option value="cycle">Cycles</option></select>
            </label>
            <label>Debut de la plage
              <input data-field="start" type="time" value="06:00" required>
            </label>
            <label>Fin de la plage
              <input data-field="stop" type="time" value="00:00" required>
            </label>
            <div class="cycle-fields" data-cycle-fields hidden>
              <label>Duree de marche (min)
                <input data-field="duration" type="number" min="1" max="1440" value="5" required>
              </label>
              <label>Intervalle (min)
                <input data-field="interval" type="number" min="1" max="1440" value="60" required>
              </label>
            </div>
          </form>
          <div class="light-controls" role="group" aria-label="Mode de la lumiere">
            <button type="button" data-mode="on">ON</button><button type="button" data-mode="prog">PROG</button><button type="button" data-mode="off">OFF</button>
          </div>
          <p class="light-feedback" data-feedback role="status"></p>
        </article>
        <article class="program-card" data-task-card="ventilation">
          <h3>Ventilation</h3>
          <p class="light-state" data-state></p>
          <form class="light-form" data-program-form="ventilation">
            <label>Relais
              <select data-field="channel"><option value="1">Relais 1</option><option value="2" selected>Relais 2</option><option value="3">Relais 3</option><option value="4">Relais 4</option></select>
            </label>
            <label>Programme
              <select data-field="type"><option value="window">Plage continue</option><option value="cycle" selected>Cycles</option></select>
            </label>
            <label>Debut de la plage
              <input data-field="start" type="time" value="00:00" required>
            </label>
            <label>Fin de la plage
              <input data-field="stop" type="time" value="00:00" required>
            </label>
            <div class="cycle-fields" data-cycle-fields>
              <label>Duree de marche (min)
                <input data-field="duration" type="number" min="1" max="1440" value="5" required>
              </label>
              <label>Intervalle (min)
                <input data-field="interval" type="number" min="1" max="1440" value="60" required>
              </label>
            </div>
          </form>
          <div class="light-controls" role="group" aria-label="Mode de la ventilation">
            <button type="button" data-mode="on">ON</button><button type="button" data-mode="prog">PROG</button><button type="button" data-mode="off">OFF</button>
          </div>
          <p class="light-feedback" data-feedback role="status"></p>
        </article>
        <article class="program-card" data-task-card="pump">
          <h3>Pompe</h3>
          <p class="light-state" data-state></p>
          <form class="light-form" data-program-form="pump">
            <label>Relais
              <select data-field="channel"><option value="1">Relais 1</option><option value="2">Relais 2</option><option value="3" selected>Relais 3</option><option value="4">Relais 4</option></select>
            </label>
            <label>Programme
              <select data-field="type"><option value="window">Plage continue</option><option value="cycle" selected>Cycles</option></select>
            </label>
            <label>Debut de la plage
              <input data-field="start" type="time" value="00:00" required>
            </label>
            <label>Fin de la plage
              <input data-field="stop" type="time" value="00:00" required>
            </label>
            <div class="cycle-fields" data-cycle-fields>
              <label>Duree de marche (min)
                <input data-field="duration" type="number" min="1" max="1440" value="25" required>
              </label>
              <label>Intervalle (min)
                <input data-field="interval" type="number" min="1" max="1440" value="120" required>
              </label>
            </div>
          </form>
          <div class="light-controls" role="group" aria-label="Mode de la pompe">
            <button type="button" data-mode="on">ON</button><button type="button" data-mode="prog">PROG</button><button type="button" data-mode="off">OFF</button>
          </div>
          <p class="light-feedback" data-feedback role="status"></p>
        </article>
        <article class="program-card" data-task-card="option">
          <h3>Option</h3>
          <p class="light-state" data-state></p>
          <form class="light-form" data-program-form="option">
            <label>Relais
              <select data-field="channel"><option value="1">Relais 1</option><option value="2">Relais 2</option><option value="3">Relais 3</option><option value="4" selected>Relais 4</option></select>
            </label>
            <label>Programme
              <select data-field="type"><option value="window">Plage continue</option><option value="cycle" selected>Cycles</option></select>
            </label>
            <label>Debut de la plage
              <input data-field="start" type="time" value="00:00" required>
            </label>
            <label>Fin de la plage
              <input data-field="stop" type="time" value="00:00" required>
            </label>
            <div class="cycle-fields" data-cycle-fields>
              <label>Duree de marche (min)
                <input data-field="duration" type="number" min="1" max="1440" value="5" required>
              </label>
              <label>Intervalle (min)
                <input data-field="interval" type="number" min="1" max="1440" value="60" required>
              </label>
            </div>
          </form>
          <div class="light-controls" role="group" aria-label="Mode de l'option">
            <button type="button" data-mode="on">ON</button><button type="button" data-mode="prog">PROG</button><button type="button" data-mode="off">OFF</button>
          </div>
          <p class="light-feedback" data-feedback role="status"></p>
        </article>
      </div>
    </details>
    <section class="chart-panel" id="chartPanel" hidden aria-live="polite">
      <div class="chart-heading">
        <h2 id="chartTitle">Historique sur 24 heures</h2>
        <button class="chart-close" id="chartClose" type="button" aria-label="Fermer le graphique">×</button>
      </div>
      <p class="chart-summary" id="chartSummary">Chargement de l'historique...</p>
      <div class="chart-wrap">
        <svg class="history-chart" id="historyChart" viewBox="0 0 720 280" role="img" aria-label="Graphique des mesures sur 24 heures">
          <defs>
            <linearGradient id="chartFill" x1="0" y1="0" x2="0" y2="1">
              <stop offset="0" stop-color="#8cff45"/>
              <stop offset="1" stop-color="#8cff45" stop-opacity="0"/>
            </linearGradient>
          </defs>
          <g id="chartGrid"></g>
          <path class="chart-area" id="chartArea"></path>
          <path class="chart-line" id="chartLine"></path>
          <g id="chartPoints"></g>
          <text class="chart-empty" id="chartEmpty" x="360" y="145" hidden>Historique en cours de constitution</text>
        </svg>
      </div>
    </section>
    <section class="combined-panel" id="combinedChartPanel" hidden aria-live="polite">
      <div class="chart-heading">
        <h2>Historique</h2>
        <div class="combined-controls">
          <select class="range-select" id="historyRange" aria-label="Periode de l'historique">
            <option value="1h">1 heure</option>
            <option value="3h">3 heures</option>
            <option value="6h">6 heures</option>
            <option value="12h">12 heures</option>
            <option value="24h" selected>24 heures</option>
            <option value="1d">1 jour</option>
            <option value="1m">1 mois</option>
          </select>
          <button class="chart-close" id="combinedChartClose" type="button" aria-label="Fermer le graphique">×</button>
        </div>
      </div>
      <p class="chart-summary" id="combinedChartSummary">Chargement de l'historique...</p>
      <div class="chart-wrap">
        <svg class="combined-chart" id="combinedHistoryChart" viewBox="0 0 720 300" role="img" aria-label="Graphique combine de temperature, humidite de l'air et humidite du sol">
          <g id="combinedChartGrid"></g>
          <path class="combined-line combined-temp" id="combinedTempLine"></path>
          <path class="combined-line combined-air" id="combinedAirLine"></path>
          <path class="combined-line combined-soil" id="combinedSoilLine"></path>
          <g id="wateringMarkers"></g>
          <text class="chart-empty" id="combinedChartEmpty" x="360" y="150" hidden>Historique en cours de constitution</text>
        </svg>
      </div>
      <div class="combined-legend">
        <span><i class="legend-dot legend-temp"></i>Temperature (°C)</span>
        <span><i class="legend-dot legend-air"></i>Humidite air (%)</span>
        <span><i class="legend-dot legend-soil"></i>Humidite sol (% relatif)</span>
        <span><i class="legend-dot" style="background:#ffad91"></i>Arrosage simple</span>
        <span><i class="legend-dot" style="background:#d6ff70"></i>Arrosage + engrais</span>
      </div>
      <form class="watering-form" id="wateringForm">
        <label for="wateringDate">Date et heure de l'arrosage
          <input id="wateringDate" name="wateringDate" type="datetime-local" required>
        </label>
        <label for="wateringType">Type d'arrosage
          <select id="wateringType" name="wateringType">
            <option value="simple">Arrosage simple</option>
            <option value="engrais">Arrosage + engrais</option>
          </select>
        </label>
        <button type="submit">Enregistrer</button>
      </form>
      <p class="watering-feedback" id="wateringFeedback" role="status"></p>
      <ol class="watering-list" id="wateringList"></ol>
      <section class="all-time-records" aria-labelledby="allTimeRecordsTitle">
        <div class="records-heading">
          <h3 id="allTimeRecordsTitle">Min / max depuis le début du suivi</h3>
          <button class="records-reset" id="resetRecords" type="button">Reset min/max</button>
        </div>
        <div class="records-grid" id="allTimeRecords">
          <span class="record-item">Temperature<strong>En attente</strong></span>
          <span class="record-item">Humidite air<strong>En attente</strong></span>
          <span class="record-item">Humidite sol<strong>En attente</strong></span>
        </div>
      </section>
    </section>
    <footer><span>Capteurs DHT22 + YL-69</span><span>Actualise <span id="updated">--</span></span></footer>
    <section class="plant-section" aria-label="Animation de croissance du plant">
      <div class="plant-heading">
        <h2>La pousse continue</h2>
        <span class="grow-status" id="phaseBadge">CROISSANCE</span>
      </div>
      <svg id="plant-art" class="plant-art" viewBox="0 0 240 340" role="img" aria-label="Plant vert qui pousse dans un pot">
        <defs>
          <linearGradient id="stemColor" x1="0" y1="1" x2="1" y2="0">
            <stop offset="0" stop-color="#168443"/>
            <stop offset="1" stop-color="#9aff55"/>
          </linearGradient>
          <linearGradient id="leafColor" x1="0" y1="1" x2="1" y2="0">
            <stop offset="0" stop-color="#159447"/>
            <stop offset=".55" stop-color="#44dc57"/>
            <stop offset="1" stop-color="#b1ff58"/>
          </linearGradient>
          <linearGradient id="potColor" x1="0" y1="0" x2="1" y2="0">
            <stop offset="0" stop-color="#b9c9c0"/>
            <stop offset=".48" stop-color="#f2f7ed"/>
            <stop offset="1" stop-color="#9cac9f"/>
          </linearGradient>
          <linearGradient id="soilColor" x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stop-color="#9a5727"/>
            <stop offset="1" stop-color="#4a2818"/>
          </linearGradient>
          <g id="plant-leaf-shape" fill="url(#leafColor)" stroke="#126238" stroke-linejoin="round" stroke-width="1">
            <path d="M0 0C-5-8-8-18-6-28L-13-34-5-33-11-43-3-39-5-51 1-44 7-53 5-40 14-44 9-34 17-35 9-27C9-16 5-7 0 0Z"/>
            <path d="M-1-2C-9-7-17-14-22-24L-30-25-23-30-33-34-22-34-27-44-15-38-8-25Z"/>
            <path d="M1-2C9-7 17-14 22-24L30-25 23-30 33-34 22-34 27-44 15-38 8-25Z"/>
            <path d="M-3-5C-13-8-23-14-30-22L-39-20-33-27-43-28-33-33-38-41-25-37-13-24Z"/>
            <path d="M3-5C13-8 23-14 30-22L39-20 33-27 43-28 33-33 38-41 25-37 13-24Z"/>
            <path d="M-5-9C-15-10-26-14-35-20L-43-16-39-23-48-23-39-29-45-36-31-34-17-23Z"/>
            <path d="M5-9C15-10 26-14 35-20L43-16 39-23 48-23 39-29 45-36 31-34 17-23Z"/>
          </g>
        </defs>
        <ellipse cx="120" cy="316" rx="68" ry="9" fill="#000" opacity=".28"/>
        <g class="plant-growth">
          <path d="M120 263C117 227 124 199 119 171 114 143 123 115 119 87 117 70 120 54 120 42" fill="none" stroke="url(#stemColor)" stroke-linecap="round" stroke-width="5"/>
          <g id="stage-branches"></g>
          <g class="plant-leaf delay-1"><use href="#plant-leaf-shape" transform="translate(120 42) scale(.52)"/></g>
        </g>
        <path d="M51 264 189 264 169 321Q167 327 158 327H82Q73 327 71 321Z" fill="url(#potColor)" stroke="#e7f0e8" stroke-width="2"/>
        <path d="M51 264H189L177 276H63Z" fill="#dce8de" stroke="#f5faf4" stroke-width="2"/>
        <path d="M64 268H176L166 282 120 302 74 282Z" fill="url(#soilColor)"/>
        <path d="M65 268H175" stroke="#c27a3e" stroke-linecap="round" stroke-width="2" opacity=".8"/>
      </svg>
      <div class="stage-controls">
        <label for="stageInput">Etages</label>
        <div class="stage-stepper">
          <button id="stageDown" type="button" aria-label="Retirer un etage">−</button>
          <input id="stageInput" type="number" min="1" max="8" value="2" inputmode="numeric" aria-label="Nombre d'etages">
          <button id="stageUp" type="button" aria-label="Ajouter un etage">+</button>
        </div>
        <span class="stage-count" id="stageCount">2 etages</span>
      </div>
    </section>
  </main>
  <div class="soil-alert" id="soilAlert" hidden role="alertdialog" aria-modal="true" aria-labelledby="soilAlertTitle" aria-describedby="soilAlertMessage">
    <div class="soil-alert-card">
      <h2 id="soilAlertTitle">Alerte : sol sec</h2>
      <p id="soilAlertMessage">La sonde indique que le sol est sec.</p>
      <button id="soilAlertClose" type="button">Fermer</button>
    </div>
  </div>
  <script>
    const $ = id => document.getElementById(id);
    const svgNS = "http://www.w3.org/2000/svg";
    const stageInput = $("stageInput");
    const soilColorAt = value => {
      const stops = [[0, 0], [20, 30], [40, 55], [60, 120], [80, 210]];
      const percent = Math.max(0, Math.min(80, value));
      for (let i = 1; i < stops.length; i++) {
        if (percent <= stops[i][0]) {
          const [startPercent, startHue] = stops[i - 1];
          const [endPercent, endHue] = stops[i];
          const hue = startHue + (endHue - startHue) *
            (percent - startPercent) / (endPercent - startPercent);
          return `hsl(${hue} 90% 62%)`;
        }
      }
      return "hsl(210 90% 62%)";
    };
    let previousSoilStatus = localStorage.getItem("lastSoilStatus");
    let chartMetric = "temperature";
    let chartTimer = 0;
    let combinedChartTimer = 0;
    const clampStages = value => {
      return Math.max(1, Math.min(8, Math.round(Number(value) || 2)));
    };
    const svgElement = (name, attributes) => {
      const element = document.createElementNS(svgNS, name);
      Object.entries(attributes).forEach(([key, value]) => element.setAttribute(key, value));
      return element;
    };
    const updatePlant = stages => {
      const branches = $("stage-branches");
      branches.replaceChildren();

      for (let i = 0; i < stages; i++) {
        const y = stages === 1 ? 168 : 218 - i * (132 / (stages - 1));
        const size = 0.78 + (i / Math.max(stages - 1, 1)) * 0.2;
        const leftX = 120 - (31 + size * 12);
        const rightX = 120 + (31 + size * 12);
        const branchGroup = svgElement("g", {
          fill: "none",
          stroke: "url(#stemColor)",
          "stroke-linecap": "round",
          "stroke-width": "3"
        });
        branchGroup.append(
          svgElement("path", {d: `M120 ${y - 18}Q${120 - size * 12} ${y - 8} ${leftX} ${y}`}),
          svgElement("path", {d: `M120 ${y - 18}Q${120 + size * 12} ${y - 8} ${rightX} ${y}`})
        );
        branches.appendChild(branchGroup);

        [[leftX, -48], [rightX, 48]].forEach(([x, angle], side) => {
          const leaf = svgElement("g", {
            class: `plant-leaf delay-${(i + side) % 3 + 1}`
          });
          leaf.appendChild(svgElement("use", {
            href: "#plant-leaf-shape",
            transform: `translate(${x} ${y}) rotate(${angle}) scale(${size})`
          }));
          branches.appendChild(leaf);
        });
      }

      $("stageCount").textContent = `${stages} ${stages === 1 ? "etage" : "etages"}`;
      $("plant-art").setAttribute("aria-label", `Plant avec ${stages} ${stages === 1 ? "etage" : "etages"} dans un pot`);
      stageInput.value = stages;
      localStorage.setItem("plantStages", String(stages));

      const growth = document.querySelector(".plant-growth");
      growth.classList.remove("plant-growth");
      void growth.getBoundingClientRect();
      growth.classList.add("plant-growth");
    };
    const setPlantStages = value => {
      updatePlant(clampStages(value));
    };
    $("stageDown").addEventListener("click", () => setPlantStages(Number(stageInput.value) - 1));
    $("stageUp").addEventListener("click", () => setPlantStages(Number(stageInput.value) + 1));
    stageInput.addEventListener("change", () => setPlantStages(stageInput.value));
    setPlantStages(localStorage.getItem("plantStages") || 2);
    const savedPhase = localStorage.getItem("plantPhase");
    $("phaseBadge").textContent = savedPhase === "floraison" ? "FLORAISON" : "CROISSANCE";

    const drawHistory = history => {
      const grid = $("chartGrid");
      const line = $("chartLine");
      const area = $("chartArea");
      const points = $("chartPoints");
      const empty = $("chartEmpty");
      grid.replaceChildren();
      points.replaceChildren();
      line.setAttribute("d", "");
      area.setAttribute("d", "");

      const left = 58;
      const right = 700;
      const top = 20;
      const bottom = 224;
      const values = history.samples
        .filter(sample => sample[0] > history.now - 86400 && sample[0] <= history.now)
        .map(sample => ({
          timestamp: sample[0],
          value: sample[chartMetric === "temperature" ? 1 : chartMetric === "humidity" ? 2 : 3]
        }))
        .filter(sample => sample.value !== null && sample.value !== undefined);
      const unit = chartMetric === "temperature" ? "°C" :
        chartMetric === "soil" ? "% relatif" : "%";

      [
        [0, "-24h"],
        [6, "-18h"],
        [12, "-12h"],
        [18, "-6h"],
        [24, "Maintenant"]
      ].forEach(([hours, labelText]) => {
        const x = left + (right - left) * hours / 24;
        grid.appendChild(svgElement("line", {x1: x, y1: top, x2: x, y2: bottom, class: "chart-grid"}));
        const label = svgElement("text", {
          x,
          y: 251,
          class: "chart-label",
          "text-anchor": hours === 0 ? "start" : hours === 24 ? "end" : "middle"
        });
        label.textContent = labelText;
        grid.appendChild(label);
      });

      empty.style.display = values.length > 0 ? "none" : "block";
      if (!values.length) {
        $("chartSummary").textContent = history.clockReady
          ? "Pas encore de mesures historiques. Les points sont enregistres toutes les 5 minutes."
          : "En attente de la synchronisation de l'heure pour enregistrer l'historique.";
        return;
      }

      const rawMinimum = Math.min(...values.map(sample => sample.value));
      const rawMaximum = Math.max(...values.map(sample => sample.value));
      const padding = Math.max((rawMaximum - rawMinimum) * 0.15, chartMetric === "temperature" ? 0.5 : 1);
      const scaleMinimum = rawMinimum - padding;
      const scaleMaximum = rawMaximum + padding;
      const roughStep = (scaleMaximum - scaleMinimum) / 4;
      const stepPower = Math.pow(10, Math.floor(Math.log10(roughStep)));
      const normalizedStep = roughStep / stepPower;
      const stepFactor = normalizedStep <= 1 ? 1 : normalizedStep <= 2 ? 2 : normalizedStep <= 5 ? 5 : 10;
      const step = stepFactor * stepPower;
      const minimum = Math.floor(scaleMinimum / step) * step;
      const maximum = Math.ceil(scaleMaximum / step) * step;
      const tickCount = Math.round((maximum - minimum) / step);
      const decimals = step < 1 ? Math.min(2, Math.ceil(-Math.log10(step))) : 0;

      for (let i = 0; i <= tickCount; i++) {
        const value = minimum + step * i;
        const y = bottom - ((value - minimum) / (maximum - minimum)) * (bottom - top);
        const horizontal = svgElement("line", {x1: left, y1: y, x2: right, y2: y, class: "chart-grid"});
        const label = svgElement("text", {x: left - 9, y: y + 4, class: "chart-label", "text-anchor": "end"});
        label.textContent = `${value.toFixed(decimals)}${unit}`;
        grid.append(horizontal, label);
      }

      const coords = values.map(sample => {
        const hoursAgo = Math.max(0, (history.now - sample.timestamp) / 3600);
        return {
          ...sample,
          x: right - (hoursAgo / 24) * (right - left),
          y: bottom - ((sample.value - minimum) / (maximum - minimum)) * (bottom - top)
        };
      });
      const path = coords.map((point, index) =>
        `${index === 0 ? "M" : "L"}${point.x.toFixed(1)} ${point.y.toFixed(1)}`
      ).join(" ");
      line.setAttribute("d", path);
      area.setAttribute("d", `${path} L${coords[coords.length - 1].x.toFixed(1)} ${bottom} L${coords[0].x.toFixed(1)} ${bottom} Z`);

      coords.forEach(point => {
        const circle = svgElement("circle", {cx: point.x, cy: point.y, r: "3.5", class: "chart-dot"});
        const tooltip = svgElement("title", {});
        tooltip.textContent = `${new Date(point.timestamp * 1000).toLocaleString("fr-FR")}: ${point.value.toFixed(1)} ${unit}`;
        circle.appendChild(tooltip);
        points.appendChild(circle);
      });

      const latest = values[values.length - 1];
      $("chartSummary").textContent = `${values.length} mesures · min ${rawMinimum.toFixed(1)} ${unit} · max ${rawMaximum.toFixed(1)} ${unit} · derniere ${latest.value.toFixed(1)} ${unit} a ${new Date(latest.timestamp * 1000).toLocaleTimeString("fr-FR")}`;
    };
    const loadHistory = async () => {
      try {
        const response = await fetch("/history", {cache:"no-store"});
        if (!response.ok) throw new Error("HTTP " + response.status);
        drawHistory(await response.json());
      } catch (error) {
        $("chartSummary").textContent = "Impossible de charger l'historique.";
      }
    };
    const drawCombinedHistory = (history, range) => {
      const grid = $("combinedChartGrid");
      const empty = $("combinedChartEmpty");
      const wateringMarkers = $("wateringMarkers");
      grid.replaceChildren();
      wateringMarkers.replaceChildren();

      const left = 62;
      const right = 660;
      const top = 28;
      const bottom = 244;
      const periodSeconds = range === "1m" ? 30 * 86400 :
        range === "1d" || range === "24h" ? 86400 : Number(range.slice(0, -1)) * 3600;
      const samples = history.samples.filter(sample =>
        sample[0] > history.now - periodSeconds && sample[0] <= history.now
      );
      const series = [
        {path:"combinedTempLine", index:1, minimum:15, maximum:35},
        {path:"combinedAirLine", index:2, minimum:0, maximum:100},
        {path:"combinedSoilLine", index:3, minimum:0, maximum:100}
      ];
      series.forEach(item => $(item.path).setAttribute("d", ""));
      const events = (history.events || []).map(event => Array.isArray(event)
        ? {timestamp:Number(event[0]), fertilizer:Number(event[1]) === 1}
        : {timestamp:Number(event), fertilizer:false}
      ).filter(event =>
        event.timestamp > history.now - periodSeconds && event.timestamp <= history.now
      );
      events.forEach(event => {
        const x = right - ((history.now - event.timestamp) / periodSeconds) * (right - left);
        const markerType = event.fertilizer ? "fertilizer" : "simple";
        const wateringLabel = event.fertilizer ? "Arrosage + engrais" : "Arrosage simple";
        const marker = svgElement("line", {
          x1:x, y1:top, x2:x, y2:bottom, class:`watering-marker watering-marker-${markerType}`
        });
        const title = svgElement("title", {});
        title.textContent = `${wateringLabel} · ${new Date(event.timestamp * 1000).toLocaleString("fr-FR")}`;
        marker.appendChild(title);
        wateringMarkers.appendChild(marker);
        wateringMarkers.appendChild(svgElement("circle", {
          cx:x, cy:top, r:5, class:`watering-marker-dot watering-marker-dot-${markerType}`
        }));
      });

      const wateringList = $("wateringList");
      wateringList.replaceChildren();
      const visibleEvents = [...events].sort((a, b) => b.timestamp - a.timestamp).slice(0, 8);
      if (visibleEvents.length) {
        visibleEvents.forEach(event => {
          const item = document.createElement("li");
          const wateringLabel = event.fertilizer ? "Arrosage + engrais" : "Arrosage simple";
          item.textContent = `${wateringLabel} · ${new Date(event.timestamp * 1000).toLocaleString("fr-FR")}`;
          wateringList.appendChild(item);
        });
      } else {
        const item = document.createElement("li");
        item.textContent = "Aucun arrosage enregistre sur cette periode.";
        wateringList.appendChild(item);
      }

      const records = history.records;
      const recordEntries = [
        ["Temperature", records && records.temperature, "°C", 1],
        ["Humidite air", records && records.airHumidity, "%", 1],
        ["Humidite sol", records && records.soilMoisture, "%", 0]
      ];
      const recordsGrid = $("allTimeRecords");
      recordsGrid.replaceChildren();
      recordEntries.forEach(([label, values, unit, decimals]) => {
        const item = document.createElement("span");
        item.className = "record-item";
        item.append(document.createTextNode(label));
        const detail = document.createElement("strong");
        detail.textContent = values
          ? `Min ${Number(values.min).toFixed(decimals)}${unit} · Max ${Number(values.max).toFixed(decimals)}${unit}`
          : "En attente";
        item.appendChild(detail);
        recordsGrid.appendChild(item);
      });

      for (let tick = 0; tick <= 4; tick++) {
        const y = bottom - (tick / 4) * (bottom - top);
        grid.appendChild(svgElement("line", {
          x1:left, y1:y, x2:right, y2:y, class:"chart-grid"
        }));
        const temperatureLabel = svgElement("text", {
          x:left - 9, y:y + 4, class:"chart-label", "text-anchor":"end"
        });
        temperatureLabel.textContent = `${15 + tick * 5}°`;
        grid.appendChild(temperatureLabel);

        const humidityLabel = svgElement("text", {
          x:right + 9, y:y + 4, class:"chart-label", "text-anchor":"start"
        });
        humidityLabel.textContent = `${tick * 25}%`;
        grid.appendChild(humidityLabel);
      }

      for (let tick = 0; tick <= 4; tick++) {
        const x = left + (tick / 4) * (right - left);
        grid.appendChild(svgElement("line", {
          x1:x, y1:top, x2:x, y2:bottom, class:"chart-grid"
        }));
        const label = svgElement("text", {
          x, y:270, class:"chart-label",
          "text-anchor":tick === 0 ? "start" : tick === 4 ? "end" : "middle"
        });
        if (tick === 4) {
          label.textContent = "Maintenant";
        } else if (range === "1m") {
          label.textContent = `-${Math.round(30 * (4 - tick) / 4)}j`;
        } else if (periodSeconds === 86400) {
          label.textContent = `-${24 - tick * 6}h`;
        } else {
          const hours = periodSeconds / 3600;
          label.textContent = `-${Math.round(hours * (4 - tick) / 4)}h`;
        }
        grid.appendChild(label);
      }

      empty.style.display = samples.length > 0 ? "none" : "block";
      if (!samples.length) {
        $("combinedChartSummary").textContent = history.clockReady
          ? range === "1m"
            ? "Pas encore de releves mensuels. Un point est enregistre chaque heure depuis l'installation."
            : "Aucun releve disponible pour cette periode."
          : "En attente de la synchronisation de l'heure pour enregistrer l'historique.";
      } else {
        series.forEach((item, seriesIndex) => {
        const coords = samples
          .filter(sample => sample[item.index] !== null && sample[item.index] !== undefined)
          .map(sample => {
            const value = Math.max(item.minimum, Math.min(item.maximum, Number(sample[item.index])));
            const elapsed = history.now - sample[0];
            return {
              x: right - (elapsed / periodSeconds) * (right - left),
              y: bottom - ((value - item.minimum) / (item.maximum - item.minimum)) * (bottom - top)
            };
          });
        if (coords.length) {
          $(item.path).setAttribute("d", coords.map((point, index) =>
            `${index === 0 ? "M" : "L"}${point.x.toFixed(1)} ${point.y.toFixed(1)}`
          ).join(" "));
          if (coords.length === 1) {
            const point = svgElement("circle", {
              cx:coords[0].x, cy:coords[0].y, r:5,
              class:`combined-point ${["combined-temp", "combined-air", "combined-soil"][seriesIndex]}`
            });
            grid.appendChild(point);
          }
        }
        });

        const latest = samples[samples.length - 1];
        const soilText = latest[3] === null || latest[3] === undefined ? "sol en attente" : `sol ${latest[3]}%`;
        const periodLabel = range === "1m" ? "30 jours" :
          range === "1d" ? "1 jour" : range === "24h" ? "24 heures" :
          `${range.slice(0, -1)} heure${range === "1h" ? "" : "s"}`;
        $("combinedChartSummary").textContent =
          `${samples.length} releve${samples.length === 1 ? "" : "s"} sur ${periodLabel} · dernier : ${Number(latest[1]).toFixed(1)} °C, ${Number(latest[2]).toFixed(1)} % air, ${soilText}`;
      }
    };
    const loadCombinedHistory = async () => {
      const range = $("historyRange").value;
      try {
        const response = await fetch(`/history?range=${encodeURIComponent(range)}`, {cache:"no-store"});
        if (!response.ok) throw new Error("HTTP " + response.status);
        drawCombinedHistory(await response.json(), range);
      } catch (error) {
        $("combinedChartSummary").textContent = "Impossible de charger l'historique combine.";
      }
    };
    const openCombinedHistory = async () => {
      $("combinedChartPanel").hidden = false;
      $("combinedHistoryOpen").setAttribute("aria-expanded", "true");
      await loadCombinedHistory();
      clearInterval(combinedChartTimer);
      combinedChartTimer = setInterval(loadCombinedHistory, 30000);
    };
    const closeCombinedHistory = () => {
      $("combinedChartPanel").hidden = true;
      $("combinedHistoryOpen").setAttribute("aria-expanded", "false");
      clearInterval(combinedChartTimer);
    };
    $("combinedHistoryOpen").addEventListener("click", openCombinedHistory);
    $("combinedChartClose").addEventListener("click", closeCombinedHistory);
    $("historyRange").addEventListener("change", loadCombinedHistory);
    const setWateringDateToNow = () => {
      const now = new Date();
      const localNow = new Date(now.getTime() - now.getTimezoneOffset() * 60000);
      $("wateringDate").value = localNow.toISOString().slice(0, 16);
    };
    setWateringDateToNow();
    $("wateringForm").addEventListener("submit", async event => {
      event.preventDefault();
      const timestamp = Math.floor(new Date($("wateringDate").value).getTime() / 1000);
      if (!Number.isFinite(timestamp)) {
        $("wateringFeedback").textContent = "Choisis une date et une heure valides.";
        return;
      }

      const wateringType = $("wateringType").value;
      const form = new URLSearchParams({
        timestamp:String(timestamp),
        type:wateringType
      });
      try {
        const response = await fetch("/watering", {
          method:"POST",
          headers:{"Content-Type":"application/x-www-form-urlencoded"},
          body:form
        });
        const result = await response.json();
        if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
        const wateringLabel = result.type === "engrais" ? "Arrosage + engrais" : "Arrosage simple";
        $("wateringFeedback").textContent =
          `${wateringLabel} enregistre : ${new Date(result.timestamp * 1000).toLocaleString("fr-FR")}`;
        setWateringDateToNow();
        await loadCombinedHistory();
      } catch (error) {
        $("wateringFeedback").textContent = `Impossible d'enregistrer l'arrosage : ${error.message}`;
      }
    });
    $("resetRecords").addEventListener("click", async () => {
      if (!window.confirm("Reinitialiser les min/max ? L'historique et les arrosages seront conserves.")) {
        return;
      }

      $("wateringFeedback").textContent = "Reinitialisation des min/max...";
      try {
        const response = await fetch("/records/reset", {method:"POST"});
        const result = await response.json();
        if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
        $("wateringFeedback").textContent = "Min/max reinitialises. L'historique et les arrosages sont conserves.";
        await loadCombinedHistory();
      } catch (error) {
        $("wateringFeedback").textContent = `Impossible de reinitialiser les min/max : ${error.message}`;
      }
    });
    const openChart = async metric => {
      chartMetric = metric;
      const isTemperature = metric === "temperature";
      const isSoil = metric === "soil";
      const metricLabel = isTemperature ? "Temperature" : isSoil ? "Humidite du sol" : "Humidite";
      $("chartTitle").textContent = isTemperature ? "Temperature sur 24 heures" :
        isSoil ? "Humidite du sol sur 24 heures" : "Humidite sur 24 heures";
      const chartColor = isTemperature ? "#f1d47a" : isSoil ? "#8cff45" : "#73baff";
      $("chartLine").style.stroke = chartColor;
      $("chartPoints").style.setProperty("--chart-point-color", chartColor);
      $("chartFill").querySelector("stop").setAttribute("stop-color", chartColor);
      $("historyChart").setAttribute("aria-label", `Graphique ${metricLabel.toLowerCase()} sur 24 heures`);
      $("chartPanel").hidden = false;
      $("temperatureCard").setAttribute("aria-expanded", String(isTemperature));
      $("humidityCard").setAttribute("aria-expanded", String(metric === "humidity"));
      $("soilCard").setAttribute("aria-expanded", String(isSoil));
      await loadHistory();
      clearInterval(chartTimer);
      chartTimer = setInterval(loadHistory, 30000);
    };
    const closeChart = () => {
      $("chartPanel").hidden = true;
      $("temperatureCard").setAttribute("aria-expanded", "false");
      $("humidityCard").setAttribute("aria-expanded", "false");
      $("soilCard").setAttribute("aria-expanded", "false");
      clearInterval(chartTimer);
    };
    $("temperatureCard").addEventListener("click", () => openChart("temperature"));
    $("humidityCard").addEventListener("click", () => openChart("humidity"));
    $("soilCard").addEventListener("click", () => openChart("soil"));
    $("temperatureCard").addEventListener("keydown", event => {
      if (event.key === "Enter" || event.key === " ") {
        event.preventDefault();
        openChart("temperature");
      }
    });
    $("humidityCard").addEventListener("keydown", event => {
      if (event.key === "Enter" || event.key === " ") {
        event.preventDefault();
        openChart("humidity");
      }
    });
    $("soilCard").addEventListener("keydown", event => {
      if (event.key === "Enter" || event.key === " ") {
        event.preventDefault();
        openChart("soil");
      }
    });
    $("chartClose").addEventListener("click", closeChart);
    const closeSoilAlert = () => { $("soilAlert").hidden = true; };
    $("soilAlertClose").addEventListener("click", closeSoilAlert);
    $("soilAlert").addEventListener("click", event => {
      if (event.target === $("soilAlert")) closeSoilAlert();
    });
    document.addEventListener("keydown", event => {
      if (event.key === "Escape" && !$("soilAlert").hidden) closeSoilAlert();
    });

    const setConnectionStatus = (id, label, online) => {
      const status = $(id);
      const description = `${label} : ${online ? "en ligne" : "hors ligne"}`;
      status.classList.toggle("online", online);
      status.setAttribute("aria-label", description);
      status.title = description;
    };
    const programLabels = {
      light:"Lumiere",
      ventilation:"Ventilation",
      pump:"Pompe",
      option:"Option"
    };
    const formatMinutes = minutes => {
      if (minutes % 60 === 0) return `${minutes / 60} h`;
      if (minutes > 60) return `${Math.floor(minutes / 60)} h ${minutes % 60} min`;
      return `${minutes} min`;
    };
    let lastRelayStatus = null;
    let lastRelayStatusAt = 0;
    let relayRequestBusy = false;
    const conditionForm = $("conditionForm");
    let conditionCompatible = false;
    const setConditionFields = () => {
      $("conditionDurationField").hidden = conditionForm.elements.namedItem("action").value !== "1";
    };
    const conditionDirty = () => {
      conditionForm.dataset.dirty = "true";
      conditionForm.dataset.revision = String(Number(conditionForm.dataset.revision || 0) + 1);
      $("conditionFeedback").textContent = "Modifications non enregistrees.";
      setConditionFields();
    };
    conditionForm.addEventListener("input", conditionDirty);
    conditionForm.addEventListener("change", conditionDirty);
    const updateConditionStatus = data => {
      conditionCompatible = data.conditionVersion === 1 && !!data.condition;
      $("conditionSave").disabled = relayRequestBusy || !conditionCompatible;
      if (!conditionCompatible) {
        $("conditionState").textContent = "Mettre a jour le firmware relais pour utiliser les conditions.";
        return;
      }
      const config = data.condition;
      if (conditionForm.dataset.dirty !== "true") {
        ["enabled","temperatureEnabled","humidityEnabled"].forEach(key => {
          conditionForm.elements.namedItem(key).checked = config[key];
        });
        ["channel","temperatureLimit","humidityLimit","duration"].forEach(key => {
          conditionForm.elements.namedItem(key).value = config[key];
        });
        conditionForm.elements.namedItem("humiditySource").value = config.humiditySource === "soil" ? "1" : "0";
        conditionForm.elements.namedItem("logic").value = config.logic === "and" ? "1" : "0";
        conditionForm.elements.namedItem("action").value = config.action === "timed" ? "1" : "0";
        setConditionFields();
      }
      const target = data.programs.find(program => program.channel === config.channel);
      const state = !config.saved ? "ERREUR : regle non sauvegardee"
        : !config.enabled ? "Regle desactivee"
        : target?.mode !== "prog" ? `Inactive : relais ${config.channel} en mode ${target?.mode.toUpperCase() || "inconnu"} (PROG requis)`
        : config.resumePending ? "Reprise du minuteur en attente de l'heure et de mesures valides"
        : config.action === "timed" && !data.clockReady ? "Minuteur en attente de l'heure du relais"
        : !config.fresh ? "Mesures requises indisponibles ou perimees : condition inactive"
        : config.active ? (config.action === "timed" ? `Marche conditionnelle · ${countdownText(config.remaining)} restantes` : "Seuil depasse : marche conditionnelle")
        : config.matched && config.action === "timed" ? "Duree terminee ; retour sous les seuils requis avant un nouveau declenchement"
        : "En veille : aucun declenchement";
      const measurement = (value, unit) => value == null ? "indisponible" : `${value} ${unit}`;
      const age = config.sampleAge == null ? "aucune mesure recue" : `mesures il y a ${config.sampleAge} s`;
      $("conditionState").textContent = `${state} · Relais ${config.channel} · Temperature : ${measurement(config.temperature,"°C")} · Air : ${measurement(config.air,"%")} · Sol : ${measurement(config.soil,"%")} · ${age}`;
    };
    const setRelayRequestBusy = busy => {
      relayRequestBusy = busy;
      document.querySelectorAll("[data-task-card] button").forEach(button => {
        button.disabled = busy || lastRelayStatus?.schedulerVersion !== 3;
      });
      $("conditionSave").disabled = busy || !conditionCompatible || !lastRelayStatus;
    };
    const countdownText = seconds => {
      seconds = Math.max(0, Math.ceil(seconds));
      const days = Math.floor(seconds / 86400);
      return `${days ? `${days} j ` : ""}${String(Math.floor(seconds / 3600) % 24).padStart(2,"0")}:${String(Math.floor(seconds / 60) % 60).padStart(2,"0")}:${String(seconds % 60).padStart(2,"0")}`;
    };
    const updateCycleFields = form => {
      form.querySelectorAll("[data-slot]").forEach(slot => {
        slot.querySelector("[data-cycle-fields]").hidden =
          slot.querySelector('[data-field="type"]').value !== "cycle";
      });
    };
    const initialiseProgramEditor = card => {
      const form = card.querySelector("[data-program-form]");
      form.innerHTML = `<label>Relais<select data-field="channel">${[1,2,3,4].map(channel => `<option value="${channel}">Relais ${channel}</option>`).join("")}</select></label>` +
        [0,1,2,3].map(index => `<details class="schedule-slot" data-slot="${index}">
          <summary>Plage ${index + 1}</summary>
          <div class="schedule-slot-body">
          <label><input type="checkbox" data-field="enabled"> Active</label>
          <div class="schedule-days">${["Lun","Mar","Mer","Jeu","Ven","Sam","Dim"].map((day,i) => `<label><input type="checkbox" data-day="${i}">${day}</label>`).join("")}</div>
          <div class="light-form">
            <label>Programme<select data-field="type"><option value="window">Plage continue</option><option value="cycle">Cycles</option></select></label>
            <label>Debut<input type="time" data-field="start" value="00:00" required></label>
            <label>Fin<input type="time" data-field="stop" value="00:00" required></label>
            <div data-cycle-fields class="cycle-fields">
              <label>Marche (min)<input type="number" data-field="duration" min="1" max="1440" value="5" required></label>
              <label>Intervalle (min)<input type="number" data-field="interval" min="1" max="1440" value="60" required></label>
            </div>
          </div>
          </div>
        </details>`).join("");
      form.addEventListener("submit", event => event.preventDefault());
      form.addEventListener("invalid", event => {
        const editor = event.target.closest("[data-slot]");
        if (editor) editor.open = true;
      }, true);
      const markDirty = () => {
        form.dataset.dirty = "true";
        form.dataset.revision = String(Number(form.dataset.revision || 0) + 1);
        card.querySelector("[data-feedback]").textContent = "Modifications non enregistrees.";
      };
      form.addEventListener("input", markDirty);
      form.addEventListener("change", () => { markDirty(); updateCycleFields(form); });
      const countdown = document.createElement("p");
      countdown.className = "program-countdown";
      countdown.dataset.countdown = "";
      card.querySelector("[data-state]").after(countdown);
      const temporary = document.createElement("div");
      temporary.className = "temporary-controls";
      temporary.innerHTML = `<label>Marche temporaire (min) <input data-temporary-minutes type="number" min="1" max="1440" value="5"></label><button type="button" data-temporary-start>Demarrer</button><button type="button" data-temporary-cancel>Annuler</button>`;
      card.querySelector("[data-feedback]").before(temporary);
      card.querySelectorAll("button").forEach(button => { button.disabled = true; });
      temporary.querySelector("[data-temporary-start]").addEventListener("click", () => sendTemporary(card, false));
      temporary.querySelector("[data-temporary-cancel]").addEventListener("click", () => sendTemporary(card, true));
    };
    const tickRelayClock = () => {
      if (!lastRelayStatus) return;
      const elapsed = (performance.now() - lastRelayStatusAt) / 1000;
      if (elapsed > 15) {
        $("relayClock").textContent = "Heure du relais indisponible";
        document.querySelectorAll("[data-countdown]").forEach(node => { node.textContent = "Etat a actualiser"; });
        $("conditionState").textContent = "Etat des conditions a actualiser.";
        return;
      }
      const now = lastRelayStatus.timestamp + elapsed;
      $("relayClock").textContent = lastRelayStatus.clockReady
        ? new Intl.DateTimeFormat("fr-FR", {timeZone:"Europe/Paris",weekday:"short",hour:"2-digit",minute:"2-digit",second:"2-digit"}).format(new Date(now * 1000)) + " · Paris"
        : "Synchronisation de l'heure...";
      lastRelayStatus.programs.forEach(program => {
        const node = document.querySelector(`[data-task-card="${program.task}"] [data-countdown]`);
        if (!node) return;
        const remaining = Math.max(0, program.temporaryRemaining - elapsed);
        node.textContent = remaining > 0
          ? `Fin de marche temporaire dans ${countdownText(remaining)} · retour ${program.mode.toUpperCase()}`
          : program.temporaryAwaitingClock ? "Marche temporaire restauree ; verification de l'heure de fin en attente"
          : program.temporaryRemaining > 0 ? "Reprise du mode precedent..."
          : program.mode !== "prog" ? `Mode ${program.mode.toUpperCase()} permanent`
          : !lastRelayStatus.clockReady ? (lastRelayStatus.restoringOutputs ? "Dernier etat restaure ; horaires en attente de l'heure" : "Programmation en attente de l'heure")
          : program.conditionControlled ? "Plages + condition : prochain changement selon les mesures"
          : program.nextChange ? `${program.relayOn ? "Arret" : "Prochaine activation"} dans ${countdownText(program.nextChange - now)}`
          : "Aucun changement prevu dans les 8 prochains jours";
      });
      if (lastRelayStatus.conditionVersion === 1) {
        const condition = {...lastRelayStatus.condition};
        condition.remaining = Math.max(0, condition.remaining - elapsed);
        updateConditionStatus({...lastRelayStatus,condition});
      }
    };
    const updateProgramStatus = data => {
      lastRelayStatus = data;
      lastRelayStatusAt = performance.now();
      updateConditionStatus(data);
      $("relayConnection").textContent = data.wifi
        ? "Module relais en ligne"
        : "Module relais hors ligne";
       if (data.clockReady && data.localTime) {
        $("relayConnection").textContent += ` · Heure du relais : ${data.localTime}`;
       }
      setConnectionStatus("relayStatus", "Relai", Boolean(data.wifi));
      const clockPending = !data.clockReady &&
        data.programs.some(program => program.mode === "prog");
      if (clockPending) {
        $("relayConnection").textContent += data.restoringOutputs
          ? " · dernier etat restaure, en attente de l'heure"
          : " · horloge en attente, programmation inactive";
      }
      if (data.recoverySaved === false) {
        $("relayConnection").textContent += " · ERREUR : sauvegarde de reprise apres coupure indisponible";
      }
      data.programs.forEach(program => {
        const card = document.querySelector(`[data-task-card="${program.task}"]`);
        if (!card) return;
        const form = card.querySelector("[data-program-form]");
        if (form.dataset.dirty !== "true") {
          form.querySelector('[data-field="channel"]').value = String(program.channel);
          const slots = program.slots || [{enabled:true,days:127,...program}];
          form.querySelectorAll("[data-slot]").forEach((editor, index) => {
            const slot = slots[index] || {enabled:false,days:127,type:"window",start:"00:00",stop:"00:00",duration:5,interval:60};
            editor.querySelector('[data-field="enabled"]').checked = slot.enabled;
            ["type","start","stop","duration","interval"].forEach(field => {
              editor.querySelector(`[data-field="${field}"]`).value = slot[field];
            });
            editor.querySelectorAll("[data-day]").forEach(input => { input.checked = !!(slot.days & (1 << Number(input.dataset.day))); });
          });
        }
        updateCycleFields(form);
        const currentState = program.relayOn ? "en marche" : "arrêté";
        card.querySelector("[data-state]").textContent =
          `${programLabels[program.task]} · relais ${program.channel} · ${currentState} · mode ${program.mode.toUpperCase()}`;
        card.querySelectorAll("button").forEach(button => { button.disabled = data.schedulerVersion !== 3; });
        if (data.schedulerVersion !== 3) card.querySelector("[data-feedback]").textContent = "Mettre a jour le firmware relais pour activer les nouvelles fonctions.";
        card.querySelectorAll("[data-mode]").forEach(button => {
          button.setAttribute("aria-pressed", String(program.mode === button.dataset.mode));
        });
      });
      tickRelayClock();
    };
    const refreshProgramStatus = async () => {
      if (relayRequestBusy) return;
      setRelayRequestBusy(true);
      try {
        const response = await fetch("/relay/status", {cache:"no-store"});
        const result = await response.json();
        if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
        updateProgramStatus(result);
      } catch (error) {
        lastRelayStatus = null;
        conditionCompatible = false;
        $("conditionSave").disabled = true;
        $("conditionState").textContent = "Relais indisponible : etat des conditions inconnu.";
        $("relayClock").textContent = "Heure du relais indisponible";
        document.querySelectorAll("[data-countdown]").forEach(node => { node.textContent = "Etat indisponible"; });
        $("relayConnection").textContent = "Module relais introuvable sur le Wi-Fi local.";
        setConnectionStatus("relayStatus", "Relai", false);
        document.querySelectorAll("[data-task-card] [data-state]").forEach(state => {
          state.textContent = "État du relais indisponible.";
        });
        document.querySelectorAll("[data-task-card] button").forEach(button => { button.disabled = true; });
      } finally {
        setRelayRequestBusy(false);
      }
    };
    const sendProgramMode = async (card, mode) => {
      const form = card.querySelector("[data-program-form]");
      if (!form.reportValidity()) return;
      if (relayRequestBusy || lastRelayStatus?.schedulerVersion !== 3) return;
      const buttons = [...card.querySelectorAll("button")];
      const revision = form.dataset.revision;
      buttons.forEach(button => { button.disabled = true; });
      card.querySelector("[data-feedback]").textContent = "Enregistrement du programme...";
      try {
        const body = new URLSearchParams({
          task:card.dataset.taskCard,
          mode,
          channel:form.querySelector('[data-field="channel"]').value
        });
        for (const editor of form.querySelectorAll("[data-slot]")) {
          const prefix = `s${editor.dataset.slot}_`;
          const value = field => editor.querySelector(`[data-field="${field}"]`);
          const days = [...editor.querySelectorAll("[data-day]:checked")].reduce((mask,input) => mask | (1 << Number(input.dataset.day)),0);
          if ((value("enabled").checked && !days) || Number(value("duration").value) > Number(value("interval").value)) {
            editor.open = true;
            throw new Error("Choisir au moins un jour par plage active et une duree inferieure ou egale a l'intervalle.");
          }
          body.set(prefix + "enabled", value("enabled").checked ? "1" : "0");
          body.set(prefix + "days", String(days));
          ["type","start","stop","duration","interval"].forEach(field => body.set(prefix + field, value(field).value));
        }
        setRelayRequestBusy(true);
        const response = await fetch("/relay/program", {
          method:"POST",
          headers:{"Content-Type":"application/x-www-form-urlencoded"},
          body
        });
        const result = await response.json();
        if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
        if (form.dataset.revision === revision) delete form.dataset.dirty;
        updateProgramStatus(result);
        card.querySelector("[data-feedback]").textContent = form.dataset.dirty === "true"
          ? "Programme enregistre ; les nouvelles modifications restent a enregistrer."
          : mode === "prog" && !result.clockReady
          ? "Programme enregistré ; en attente de synchronisation de l’heure."
          : `Réglages enregistrés · mode ${mode.toUpperCase()} activé.`;
      } catch (error) {
        card.querySelector("[data-feedback]").textContent = `Commande impossible : ${error.message}`;
      } finally {
        setRelayRequestBusy(false);
        buttons.forEach(button => { button.disabled = lastRelayStatus?.schedulerVersion !== 3; });
      }
    };
    const sendTemporary = async (card, cancel) => {
      if (relayRequestBusy || lastRelayStatus?.schedulerVersion !== 3) return;
      const input = card.querySelector("[data-temporary-minutes]");
      if (!cancel && !input.reportValidity()) return;
      setRelayRequestBusy(true);
      try {
        const response = await fetch("/relay/temporary", {
          method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},
          body:new URLSearchParams({task:card.dataset.taskCard,minutes:cancel ? "0" : input.value})
        });
        const result = await response.json();
        if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
        updateProgramStatus(result);
        card.querySelector("[data-feedback]").textContent = cancel ? "Marche temporaire annulee." : "Marche temporaire activee ; retour automatique au mode precedent.";
      } catch (error) {
        card.querySelector("[data-feedback]").textContent = `Commande impossible : ${error.message}`;
      } finally {
        setRelayRequestBusy(false);
      }
    };
    conditionForm.addEventListener("submit", async event => {
      event.preventDefault();
      if (relayRequestBusy || !conditionCompatible || !conditionForm.reportValidity()) return;
      const value = name => conditionForm.elements.namedItem(name);
      if (value("enabled").checked && !value("temperatureEnabled").checked && !value("humidityEnabled").checked) {
        $("conditionFeedback").textContent = "Choisir au moins un seuil pour activer la regle.";
        return;
      }
      const revision = conditionForm.dataset.revision;
      const body = new URLSearchParams();
      ["enabled","temperatureEnabled","humidityEnabled"].forEach(key => body.set(key,value(key).checked ? "1" : "0"));
      ["channel","humiditySource","logic","action","duration"].forEach(key => body.set(key,value(key).value));
      ["temperatureLimit","humidityLimit"].forEach(key => body.set(key,String(Math.round(Number(value(key).value) * 10))));
      setRelayRequestBusy(true);
      $("conditionFeedback").textContent = "Enregistrement...";
      try {
        const response = await fetch("/relay/condition", {
          method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body
        });
        const result = await response.json();
        if (!response.ok) throw new Error(result.error || `HTTP ${response.status}`);
        if (conditionForm.dataset.revision === revision) delete conditionForm.dataset.dirty;
        updateProgramStatus(result);
        $("conditionFeedback").textContent = conditionForm.dataset.dirty === "true"
          ? "Regle enregistree ; les nouvelles modifications restent a enregistrer."
          : "Conditions enregistrees. La sortie cible doit etre en mode PROG.";
      } catch (error) {
        $("conditionFeedback").textContent = `Enregistrement impossible : ${error.message}`;
      } finally {
        setRelayRequestBusy(false);
      }
    });
    document.querySelectorAll("[data-task-card]").forEach(card => {
      initialiseProgramEditor(card);
      const form = card.querySelector("[data-program-form]");
      card.querySelectorAll("[data-mode]").forEach(button => {
        button.addEventListener("click", () => sendProgramMode(card, button.dataset.mode));
      });
      updateCycleFields(form);
    });
    refreshProgramStatus();
    setInterval(refreshProgramStatus, 5000);
    setInterval(tickRelayClock, 1000);

    let growDatesDirty = false;
    let growDatesBusy = false;
    let growDatesRevision = 0;
    let lastGrowDates = null;
    let trackedSeeds = [];
    let trackedSeedsReady = false;
    const renderGrowthRemaining = () => {
      const seed = trackedSeeds.find(entry => String(entry.id) === $("trackedSeed").value);
      const renderRemaining = (id, label, phase, durationKey, dateKey, daysKey, origin) => {
        const output = $(id);
        if (!trackedSeedsReady) {
          output.textContent = "Estimation indisponible : fiches non chargees.";
        } else if (!seed) {
          output.textContent = `Choisir une graine pour estimer la ${phase} restante.`;
        } else if (!/^\d{1,4}$/.test(seed[durationKey])) {
          output.textContent = `Duree de ${phase} non renseignee dans la fiche.`;
        } else if (!lastGrowDates || !lastGrowDates.growDatesAvailable) {
          output.textContent = "Estimation indisponible : dates non disponibles.";
        } else if (!lastGrowDates[dateKey]) {
          output.textContent = `Estimation indisponible : date de ${phase === "floraison" ? "passage en floraison" : "germination"} non renseignee.`;
        } else if (!lastGrowDates.clockReady) {
          output.textContent = "Estimation indisponible : heure indisponible.";
        } else if (!Number.isInteger(lastGrowDates[daysKey]) || lastGrowDates[daysKey] < 0) {
          output.textContent = `Estimation indisponible : ${phase === "floraison" ? "floraison" : "germination"} a venir.`;
        } else {
          const duration = Number(seed[durationKey]);
          const remaining = Math.max(0, duration - lastGrowDates[daysKey]);
          output.textContent = `${label} : ${remaining} jour${remaining === 1 ? "" : "s"} restant${remaining === 1 ? "" : "s"} theorique${remaining === 1 ? "" : "s"} · ${duration} jours prevus depuis ${origin}`;
        }
      };
      renderRemaining("growthRemaining", "Croissance", "croissance", "growthDays", "germinationDate", "germinationDays", "germination");
      renderRemaining("floweringRemaining", "Floraison", "floraison", "floweringDays", "floweringDate", "floweringDays", "le passage en floraison");
      const forecast = $("floweringForecast");
      const prefix = "Passage en floraison previsionnel : ";
      if (!trackedSeedsReady) {
        forecast.textContent = prefix + "fiches non chargees.";
      } else if (!seed) {
        forecast.textContent = prefix + "choisir une graine.";
      } else if (!/^\d{1,4}$/.test(seed.growthDays)) {
        forecast.textContent = prefix + "duree de croissance non renseignee.";
      } else if (!lastGrowDates || !lastGrowDates.growDatesAvailable) {
        forecast.textContent = prefix + "dates non disponibles.";
      } else {
        const germination = lastGrowDates.germinationDate;
        const date = new Date(`${germination}T00:00:00Z`);
        if (!/^\d{4}-\d{2}-\d{2}$/.test(germination) || !Number.isFinite(date.getTime()) ||
            date.toISOString().slice(0, 10) !== germination) {
          forecast.textContent = prefix + "date de germination absente ou invalide.";
        } else {
          date.setUTCDate(date.getUTCDate() + Number(seed.growthDays));
          forecast.textContent = prefix + date.toISOString().slice(0, 10).split("-").reverse().join("/");
        }
      }
    };
    $("trackedSeed").addEventListener("change", () => {
      localStorage.setItem("trackedSeedId", $("trackedSeed").value);
      renderGrowthRemaining();
    });
    const loadTrackedSeeds = async () => {
      $("reloadTrackedSeeds").disabled = true;
      $("trackedSeed").disabled = true;
      $("trackedSeedMessage").textContent = "Chargement des fiches...";
      try {
        const response = await fetch("/seed-bank/api", {cache:"no-store"});
        const data = await response.json();
        if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
        trackedSeeds = data.entries;
        const select = $("trackedSeed");
        select.replaceChildren();
        const empty = document.createElement("option");
        empty.value = "";
        empty.textContent = "Aucune graine selectionnee";
        select.append(empty);
        for (const seed of trackedSeeds) {
          const option = document.createElement("option");
          option.value = String(seed.id);
          option.textContent = seed.name;
          select.append(option);
        }
        const saved = localStorage.getItem("trackedSeedId");
        select.value = trackedSeeds.some(seed => String(seed.id) === saved) ? saved : "";
        trackedSeedsReady = true;
        $("trackedSeedMessage").textContent = trackedSeeds.length
          ? ""
          : "Aucune fiche : ajouter une graine dans SEED BANK.";
      } catch (error) {
        trackedSeedsReady = false;
        $("trackedSeedMessage").textContent = `Chargement des fiches impossible : ${error.message}. Reessayer avec Actualiser les fiches.`;
      } finally {
        $("reloadTrackedSeeds").disabled = false;
        $("trackedSeed").disabled = !trackedSeedsReady;
        renderGrowthRemaining();
      }
    };
    $("reloadTrackedSeeds").addEventListener("click", loadTrackedSeeds);
    loadTrackedSeeds();
    const renderGrowDates = data => {
      lastGrowDates = data;
      renderGrowthRemaining();
      const dateLabel = value => value.split("-").reverse().join("/");
      const ageLabel = (days, date, label) => Number.isInteger(days) && days >= 0
        ? `J+${days} depuis ${label} · ${dateLabel(date)}`
        : `${data.clockReady ? "Date a venir" : "Heure indisponible"} · ${label} le ${dateLabel(date)}`;
      $("germinationAge").textContent = data.growDatesAvailable
        ? ageLabel(data.germinationDays, data.germinationDate, "germination")
        : "Dates indisponibles : erreur de stockage";
      $("floweringAge").textContent = data.growDatesAvailable && data.floweringDate
        ? ageLabel(data.floweringDays, data.floweringDate, "floraison")
        : "Floraison non renseignee";
      if (!growDatesDirty && !growDatesBusy && data.growDatesAvailable) {
        $("germinationDate").value = data.germinationDate;
        $("floweringDate").value = data.floweringDate;
        $("growDatesMessage").textContent = "";
      }
      $("growDatesSave").disabled = growDatesBusy || !data.growDatesAvailable;
      if (!data.growDatesAvailable) $("growDatesMessage").textContent = "Erreur de stockage des dates : verifier le module.";
    };
    $("growDatesForm").addEventListener("input", () => {
      growDatesDirty = true;
      $("floweringDate").setCustomValidity("");
      $("growDatesMessage").textContent = "Modifications non enregistrees";
    });
    $("growDatesForm").addEventListener("submit", async event => {
      event.preventDefault();
      if (growDatesBusy) return;
      const germination = $("germinationDate").value;
      const flowering = $("floweringDate").value;
      if (flowering && flowering < germination) {
        $("floweringDate").setCustomValidity("La floraison ne peut pas preceder la germination.");
        $("floweringDate").reportValidity();
        return;
      }
      growDatesBusy = true;
      growDatesRevision++;
      const fields = [$("germinationDate"), $("floweringDate"), $("growDatesSave")];
      fields.forEach(field => field.disabled = true);
      $("growDatesMessage").textContent = "Enregistrement...";
      try {
        const response = await fetch("/grow/dates", {
          method:"POST", body:new URLSearchParams({germination, flowering})
        });
        const data = await response.json();
        if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
        growDatesDirty = false;
        growDatesBusy = false;
        renderGrowDates(data);
        $("growDatesMessage").textContent = "Dates enregistrees sur l'ESP.";
      } catch (error) {
        $("growDatesMessage").textContent = `Echec de l'enregistrement : ${error.message}. Verifier les dates avant de reessayer.`;
      } finally {
        growDatesBusy = false;
        growDatesRevision++;
        fields.forEach(field => field.disabled = false);
      }
    });
    const refresh = async () => {
      const datesRevision = growDatesRevision;
      try {
        const response = await fetch("/api", {cache:"no-store"});
        if (!response.ok) throw new Error("HTTP " + response.status);
        const data = await response.json();
        if (datesRevision === growDatesRevision && !growDatesBusy) renderGrowDates(data);
        setConnectionStatus("dashboardStatus", "Dashboard", Boolean(data.wifi));
        $("soilRaw").textContent = Number(data.soilRaw).toString();
        const soilPercent = Math.max(0, Math.min(100, Number(data.soilPercent) || 0));
        $("soilPercent").textContent = soilPercent.toString();
        $("soilCard").style.setProperty("--soil-color", soilColorAt(soilPercent));
        $("soilBar").style.width = soilPercent + "%";
        const soilStatus = data.soilStatus;
        const soilLabels = {"Sec":"Sec","Humide":"Humide","Tres humide":"Très humide","Trempe":"Trempé"};
        $("soilStatus").textContent = soilLabels[soilStatus] || "Indisponible";
        if (previousSoilStatus && soilStatus === "Sec" && previousSoilStatus !== "Sec") {
          $("soilAlert").hidden = false;
          $("soilAlertClose").focus();
        }
        previousSoilStatus = soilStatus;
        localStorage.setItem("lastSoilStatus", soilStatus);
        if (data.valid) {
          $("temperature").textContent = Number(data.temperature).toFixed(1);
          $("humidity").textContent = Number(data.humidity).toFixed(1);
          $("temperatureHint").textContent = "";
          $("humidityHint").textContent = "";
          $("temperatureBar").style.width = Math.max(0, Math.min(100, (data.temperature + 10) * 2.5)) + "%";
          $("humidityBar").style.width = Math.max(0, Math.min(100, data.humidity)) + "%";
          $("updated").textContent = new Date().toLocaleTimeString("fr-FR");
          $("updated").classList.remove("error");
        } else {
          $("temperatureHint").textContent = "Lecture du DHT22 impossible";
          $("humidityHint").textContent = "Verifier le branchement DATA";
          $("updated").textContent = "capteur indisponible";
          $("updated").classList.add("error");
        };
      } catch (error) {
        setConnectionStatus("dashboardStatus", "Dashboard", false);
        setConnectionStatus("relayStatus", "Relai", false);
        $("updated").textContent = "serveur inaccessible";
        $("updated").classList.add("error");
      }
    }
    refresh();
    setInterval(refresh, 2000);
  </script>
</body>
</html>
)rawliteral";

#include "SeedBank.h"
#include "SeedBankUi.h"

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", PAGE);
}

const __FlashStringHelper* soilCondition(bool compact = false) {
  if (soilRaw <= 330) return F("Detrempe");
  if (soilRaw <= 400) return compact ? F("Tres hum.") : F("Tres humide");
  if (soilRaw <= 847) return F("Humide");
  return F("Sec");
}

int soilMoisturePercent() {
  const float relativeMoisture =
      (SOIL_DRIER_REFERENCE - soilRaw) * 100.0f /
      (SOIL_DRIER_REFERENCE - SOIL_WETTEST_REFERENCE);
  return constrain(static_cast<int>(lroundf(relativeMoisture)), 0, 100);
}

void updateAllTimeRecords(const HistorySample& sample) {
  if (allTimeRecords.sampleCount == 0) {
    allTimeRecords.temperatureMinTenths = sample.temperatureTenths;
    allTimeRecords.temperatureMaxTenths = sample.temperatureTenths;
    allTimeRecords.airHumidityMinTenths = sample.humidityTenths;
    allTimeRecords.airHumidityMaxTenths = sample.humidityTenths;
  } else {
    allTimeRecords.temperatureMinTenths =
        min(allTimeRecords.temperatureMinTenths, sample.temperatureTenths);
    allTimeRecords.temperatureMaxTenths =
        max(allTimeRecords.temperatureMaxTenths, sample.temperatureTenths);
    allTimeRecords.airHumidityMinTenths =
        min(allTimeRecords.airHumidityMinTenths, sample.humidityTenths);
    allTimeRecords.airHumidityMaxTenths =
        max(allTimeRecords.airHumidityMaxTenths, sample.humidityTenths);
  }
  allTimeRecords.sampleCount++;

  if (sample.soilPercent != 255) {
    if (allTimeRecords.soilSampleCount == 0) {
      allTimeRecords.soilMoistureMin = sample.soilPercent;
      allTimeRecords.soilMoistureMax = sample.soilPercent;
    } else {
      allTimeRecords.soilMoistureMin =
          min(allTimeRecords.soilMoistureMin, sample.soilPercent);
      allTimeRecords.soilMoistureMax =
          max(allTimeRecords.soilMoistureMax, sample.soilPercent);
    }
    allTimeRecords.soilSampleCount++;
  }
}

bool loadAllTimeRecords() {
  File file = LittleFS.open("/records.bin", "r");
  if (file) {
    const bool validFile = file.size() == sizeof(allTimeRecords) &&
        file.read(reinterpret_cast<uint8_t*>(&allTimeRecords), sizeof(allTimeRecords)) ==
            sizeof(allTimeRecords);
    file.close();
    if (validFile && allTimeRecords.magic == RECORDS_MAGIC) {
      return true;
    }
    Serial.println(F("All-time records file invalid; rebuilding from saved history"));
  }

  allTimeRecords = {};
  allTimeRecords.magic = RECORDS_MAGIC;
  for (uint16_t i = 0; i < monthHistoryStore.count; i++) {
    const uint16_t index =
        (monthHistoryStore.head + MONTH_HISTORY_CAPACITY - monthHistoryStore.count + i) %
        MONTH_HISTORY_CAPACITY;
    updateAllTimeRecords(monthHistoryStore.samples[index]);
  }
  for (uint16_t i = 0; i < historyStore.count; i++) {
    const uint16_t index =
        (historyStore.head + HISTORY_CAPACITY - historyStore.count + i) % HISTORY_CAPACITY;
    updateAllTimeRecords(historyStore.samples[index]);
  }
  if (allTimeRecords.sampleCount > 0) {
    return saveAllTimeRecords();
  }
  return true;
}

bool saveAllTimeRecords() {
  File file = LittleFS.open("/records.bin", "w");
  if (!file) {
    Serial.println(F("Could not open all-time records file for writing"));
    return false;
  }

  const size_t written = file.write(
      reinterpret_cast<const uint8_t*>(&allTimeRecords), sizeof(allTimeRecords));
  file.flush();
  file.close();
  if (written != sizeof(allTimeRecords)) {
    Serial.println(F("All-time records write was incomplete"));
    return false;
  }
  return true;
}

void handleResetRecords() {
  const AllTimeRecords previousRecords = allTimeRecords;
  allTimeRecords = {};
  allTimeRecords.magic = RECORDS_MAGIC;

  if (!isnan(temperature) && !isnan(humidity)) {
    HistorySample currentSample = {};
    currentSample.temperatureTenths = static_cast<int16_t>(lroundf(temperature * 10.0f));
    currentSample.humidityTenths = static_cast<uint16_t>(lroundf(humidity * 10.0f));
    currentSample.soilPercent = static_cast<uint8_t>(soilMoisturePercent());
    updateAllTimeRecords(currentSample);
  }

  if (!saveAllTimeRecords()) {
    allTimeRecords = previousRecords;
    server.send(500, F("application/json"),
                F("{\"error\":\"could not save reset min/max records\"}"));
    return;
  }

  server.send(200, F("application/json"), F("{\"ok\":true}"));
}

bool loadWateringHistory() {
  File file = LittleFS.open("/watering.bin", "r");
  if (!file) {
    wateringStore = {};
    wateringStore.magic = WATERING_MAGIC;
    return true;
  }

  bool validFile = false;
  if (file.size() == sizeof(wateringStore)) {
    validFile = file.read(reinterpret_cast<uint8_t*>(&wateringStore), sizeof(wateringStore)) ==
        sizeof(wateringStore);
  } else if (file.size() == sizeof(LegacyWateringStore)) {
    LegacyWateringStore legacyStore = {};
    validFile = file.read(reinterpret_cast<uint8_t*>(&legacyStore), sizeof(legacyStore)) ==
        sizeof(legacyStore);
    if (validFile && legacyStore.magic == LEGACY_WATERING_MAGIC &&
        legacyStore.head < WATERING_CAPACITY && legacyStore.count <= WATERING_CAPACITY) {
      wateringStore = {};
      wateringStore.magic = WATERING_MAGIC;
      wateringStore.head = legacyStore.head;
      wateringStore.count = legacyStore.count;
      memcpy(wateringStore.timestamps, legacyStore.timestamps, sizeof(legacyStore.timestamps));
      file.close();
      return true;
    }
  }
  file.close();
  if (!validFile || wateringStore.magic != WATERING_MAGIC ||
      wateringStore.head >= WATERING_CAPACITY || wateringStore.count > WATERING_CAPACITY) {
    wateringStore = {};
    wateringStore.magic = WATERING_MAGIC;
    Serial.println(F("Watering history invalid; starting empty"));
    return false;
  }
  return true;
}

bool saveWateringHistory() {
  File file = LittleFS.open("/watering.bin", "w");
  if (!file) {
    Serial.println(F("Could not open watering history file for writing"));
    return false;
  }
  const size_t written = file.write(
      reinterpret_cast<const uint8_t*>(&wateringStore), sizeof(wateringStore));
  file.flush();
  file.close();
  if (written != sizeof(wateringStore)) {
    Serial.println(F("Watering history write was incomplete"));
    return false;
  }
  return true;
}

void handleWatering() {
  const time_t now = time(nullptr);
  if (now < 1700000000) {
    server.send(503, F("application/json"),
                F("{\"error\":\"clock is not synchronized\"}"));
    return;
  }

  const String value = server.arg("timestamp");
  if (value.length() == 0) {
    server.send(400, F("application/json"),
                F("{\"error\":\"timestamp is required\"}"));
    return;
  }
  for (size_t i = 0; i < value.length(); i++) {
    if (!isDigit(value[i])) {
      server.send(400, F("application/json"),
                  F("{\"error\":\"timestamp must be a positive Unix time\"}"));
      return;
    }
  }

  const uint32_t timestamp = static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
  if (timestamp < 1700000000UL || timestamp > static_cast<uint32_t>(now) + 60UL) {
    server.send(400, F("application/json"),
                F("{\"error\":\"watering date is outside the valid range\"}"));
    return;
  }

  const String type = server.hasArg("type") ? server.arg("type") : String("simple");
  if (type != "simple" && type != "engrais") {
    server.send(400, F("application/json"),
                F("{\"error\":\"watering type must be simple or engrais\"}"));
    return;
  }

  const uint16_t index = wateringStore.head;
  const uint16_t previousHead = wateringStore.head;
  const uint16_t previousCount = wateringStore.count;
  const uint32_t previousTimestamp = wateringStore.timestamps[index];
  const uint8_t previousFertilizer = wateringStore.fertilizer[index];
  const uint8_t fertilizer = type == "engrais" ? 1 : 0;
  wateringStore.timestamps[index] = timestamp;
  wateringStore.fertilizer[index] = fertilizer;
  wateringStore.head = (wateringStore.head + 1) % WATERING_CAPACITY;
  if (wateringStore.count < WATERING_CAPACITY) {
    wateringStore.count++;
  }
  if (!saveWateringHistory()) {
    wateringStore.timestamps[index] = previousTimestamp;
    wateringStore.fertilizer[index] = previousFertilizer;
    wateringStore.head = previousHead;
    wateringStore.count = previousCount;
    server.send(500, F("application/json"),
                F("{\"error\":\"could not save watering record\"}"));
    return;
  }

  String response = F("{\"ok\":true,\"timestamp\":");
  response += String(timestamp);
  response += F(",\"type\":\"");
  response += type;
  response += '"';
  response += '}';
  server.send(200, F("application/json"), response);
}

bool validScheduleTime(const String& value) {
  if (value.length() != 5 || value[2] != ':' ||
      !isDigit(value[0]) || !isDigit(value[1]) ||
      !isDigit(value[3]) || !isDigit(value[4])) {
    return false;
  }
  const uint8_t hour = static_cast<uint8_t>((value[0] - '0') * 10 + value[1] - '0');
  const uint8_t minute = static_cast<uint8_t>((value[3] - '0') * 10 + value[4] - '0');
  return hour < 24 && minute < 60;
}

bool resolveRelayAddress(IPAddress& address, bool forceLookup = false) {
  if (relayAddressKnown && !forceLookup) {
    address = relayAddress;
    return true;
  }
  if (!forceLookup && relayLookupAttempted && millis() - lastRelayLookup < 10000) {
    return false;
  }

  relayLookupAttempted = true;
  lastRelayLookup = millis();
  const uint32_t answerCount = MDNS.queryService("http", "tcp", 500);
  for (uint32_t i = 0; i < answerCount; i++) {
    const char* hostname = MDNS.answerHostname(i);
    if (hostname != nullptr && String(hostname).startsWith("grow-relay")) {
      const IPAddress discovered = MDNS.answerIP(i);
      MDNS.removeQuery();
      if (discovered[0] == 0 && discovered[1] == 0 &&
          discovered[2] == 0 && discovered[3] == 0) {
        relayAddressKnown = false;
        return false;
      }
      relayAddress = discovered;
      relayAddressKnown = true;
      address = relayAddress;
      return true;
    }
  }

  MDNS.removeQuery();
  relayAddressKnown = false;
  return false;
}

bool requestRelay(bool control, const String& payload, String& responseBody, int& responseCode,
                  const char* controlPath = "/api/program") {
  if (WiFi.status() != WL_CONNECTED) {
    responseCode = 0;
    return false;
  }

  for (uint8_t attempt = 0; attempt < (control ? 1 : 2); attempt++) {
    IPAddress address;
    if (!resolveRelayAddress(address, attempt > 0)) {
      responseCode = 0;
      continue;
    }

    WiFiClient client;
    HTTPClient relayClient;
    relayClient.setTimeout(2000);
    const String url = String(F("http://")) + address.toString() +
        (control ? String(controlPath) : String(F("/api/status")));
    if (!relayClient.begin(client, url)) {
      responseCode = 0;
      relayAddressKnown = false;
      continue;
    }

    if (control) {
      relayClient.addHeader(F("Content-Type"), F("application/x-www-form-urlencoded"));
      responseCode = relayClient.POST(payload);
    } else {
      responseCode = relayClient.GET();
    }

    if (responseCode > 0) {
      responseBody = relayClient.getString();
      relayClient.end();
      return true;
    }

    relayClient.end();
    relayAddressKnown = false;
    responseBody = "";
  }
  return false;
}

void handleRelayStatus() {
  String responseBody;
  int responseCode = 0;
  if (!requestRelay(false, String(), responseBody, responseCode)) {
    server.send(503, F("application/json"),
                F("{\"error\":\"relay module not found on local Wi-Fi\"}"));
    return;
  }
  if (responseCode != 200) {
    server.send(502, F("application/json"),
                F("{\"error\":\"relay module returned an unexpected status\"}"));
    return;
  }

  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), responseBody);
}

void forwardRelaySensors() {
  if (!sensorReadReady || WiFi.status() != WL_CONNECTED ||
      static_cast<uint32_t>(millis() - lastRelaySensorAttempt) < 5000) return;
  lastRelaySensorAttempt = millis();
  const bool fresh = static_cast<uint32_t>(millis() - lastSensorRead) < 5000;
  const bool temperatureValid = fresh && isfinite(temperature) && temperature >= -40 && temperature <= 80;
  const bool airValid = fresh && isfinite(humidity) && humidity >= 0 && humidity <= 100;
  // A saturated ADC can indicate an unplugged or shorted probe.
  const bool soilValid = fresh && soilRaw > 0 && soilRaw < 1023;
  String payload = String(F("temperatureValid=")) + (temperatureValid ? '1' : '0') +
      F("&airValid=") + (airValid ? '1' : '0') + F("&soilValid=") + (soilValid ? '1' : '0') +
      F("&temperature=") + String(temperatureValid ? static_cast<int>(roundf(temperature * 10)) : 0) +
      F("&air=") + String(airValid ? static_cast<int>(roundf(humidity * 10)) : 0) +
      F("&soil=") + String(soilValid ? soilMoisturePercent() * 10 : 0);
  String responseBody;
  int responseCode = 0;
  const bool sent = requestRelay(true, payload, responseBody, responseCode, "/api/sensors");
  const bool success = sent && responseCode == 200;
  if (!success && (relaySensorsForwarded || responseCode != 404)) {
    Serial.printf("Relay sensor forwarding failed: HTTP %d\n", responseCode);
  }
  relaySensorsForwarded = success;
}

void handleRelayCondition() {
  const char* fields[] = {"enabled", "channel", "temperatureEnabled", "humidityEnabled",
                        "humiditySource", "logic", "action", "temperatureLimit", "humidityLimit", "duration"};
  String payload;
  for (uint8_t i = 0; i < 10; i++) {
    const String value = server.arg(fields[i]);
    if (value.length() == 0 || value.length() > 6) {
      server.send(400, F("application/json"), F("{\"error\":\"invalid condition value\"}"));
      return;
    }
    for (uint16_t j = 0; j < value.length(); j++) {
      if (!isDigit(value[j]) && !(i == 7 && j == 0 && value[j] == '-' && value.length() > 1)) {
        server.send(400, F("application/json"), F("{\"error\":\"condition values must be integers; thresholds use tenths\"}"));
        return;
      }
    }
    if (i != 0) payload += '&';
    payload += String(fields[i]) + '=' + value;
  }
  String responseBody;
  int responseCode = 0;
  if (!requestRelay(true, payload, responseBody, responseCode, "/api/condition")) {
    server.send(503, F("application/json"), F("{\"error\":\"condition save not confirmed; check relay status before retrying\"}"));
    return;
  }
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(responseCode, F("application/json"), responseBody);
}

bool validProgramMinute(const String& value) {
  if (value.length() == 0) return false;
  uint32_t parsed = 0;
  for (uint16_t i = 0; i < value.length(); i++) {
    if (!isDigit(value[i])) return false;
    parsed = parsed * 10 + value[i] - '0';
    if (parsed > 1440) return false;
  }
  return parsed >= 1;
}

void handleRelayProgram() {
  const String task = server.arg("task");
  if (task != "light" && task != "ventilation" && task != "pump" && task != "option") {
    server.send(400, F("application/json"),
                F("{\"error\":\"task must be light, ventilation, pump, or option\"}"));
    return;
  }

  const String mode = server.arg("mode");
  if (mode != "on" && mode != "prog" && mode != "off") {
    server.send(400, F("application/json"),
                F("{\"error\":\"mode must be on, prog, or off\"}"));
    return;
  }

  const String channel = server.arg("channel");
  if (channel.length() != 1 || channel[0] < '1' || channel[0] > '4') {
    server.send(400, F("application/json"),
                F("{\"error\":\"channel must be between 1 and 4\"}"));
    return;
  }

  String payload = String(F("task=")) + task + F("&mode=") + mode + F("&channel=") + channel;
  for (uint8_t s = 0; s < 4; s++) {
    const String prefix = String(F("s")) + s + '_';
    const String enabled = server.arg(prefix + F("enabled"));
    const String days = server.arg(prefix + F("days"));
    const String type = server.arg(prefix + F("type"));
    const String start = server.arg(prefix + F("start"));
    const String stop = server.arg(prefix + F("stop"));
    const String duration = server.arg(prefix + F("duration"));
    const String interval = server.arg(prefix + F("interval"));
    bool validDays = days.length() > 0 && days.length() <= 3;
    for (uint16_t j = 0; j < days.length(); j++) validDays = validDays && isDigit(days[j]);
    if ((enabled != "0" && enabled != "1") || !validDays || days.toInt() > 127 ||
        (enabled == "1" && days.toInt() == 0) ||
        (type != "window" && type != "cycle") ||
        !validScheduleTime(start) || !validScheduleTime(stop) ||
        !validProgramMinute(duration) || !validProgramMinute(interval) ||
        duration.toInt() > interval.toInt()) {
      server.send(400, F("application/json"), F("{\"error\":\"invalid schedule slot\"}"));
      return;
    }
    payload += '&' + prefix + F("enabled=") + enabled + '&' + prefix + F("days=") + days +
        '&' + prefix + F("type=") + type + '&' + prefix + F("start=") + start +
        '&' + prefix + F("stop=") + stop + '&' + prefix + F("duration=") + duration +
        '&' + prefix + F("interval=") + interval;
  }
  String responseBody;
  int responseCode = 0;
  if (!requestRelay(true, payload, responseBody, responseCode)) {
    server.send(503, F("application/json"),
                F("{\"error\":\"could not reach relay module on local Wi-Fi\"}"));
    return;
  }
  if (responseCode < 200 || responseCode >= 300) {
    server.send(responseCode >= 400 && responseCode < 500 ? responseCode : 502,
                F("application/json"), responseBody);
    return;
  }

  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), responseBody);
}

void handleRelayTemporary() {
  const String task = server.arg("task");
  const String minutes = server.arg("minutes");
  if ((task != "light" && task != "ventilation" && task != "pump" && task != "option") ||
      (minutes != "0" && !validProgramMinute(minutes))) {
    server.send(400, F("application/json"), F("{\"error\":\"invalid task or temporary duration\"}"));
    return;
  }
  String responseBody;
  int responseCode = 0;
  const String payload = String(F("task=")) + task + F("&minutes=") + minutes;
  if (!requestRelay(true, payload, responseBody, responseCode, "/api/temporary")) {
    server.send(503, F("application/json"), F("{\"error\":\"temporary command not confirmed; check relay status before retrying\"}"));
    return;
  }
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(responseCode, F("application/json"), responseBody);
}

int calendarDay(int year, int month, int day) {
  static const int beforeMonth[] = {0,31,59,90,120,151,181,212,243,273,304,334};
  const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
  const int previous = year - 1;
  return 365 * previous + previous / 4 - previous / 100 + previous / 400 +
      beforeMonth[month - 1] + (leap && month > 2 ? 1 : 0) + day - 1;
}

int parseGrowDate(const char* value) {
  if (strlen(value) != 10 || value[4] != '-' || value[7] != '-') return -1;
  for (int i = 0; i < 10; i++) {
    if (i != 4 && i != 7 && (value[i] < '0' || value[i] > '9')) return -1;
  }
  const int year = atoi(value);
  const int month = atoi(value + 5);
  const int day = atoi(value + 8);
  if (year < 1970 || year > 2099 || month < 1 || month > 12 || day < 1) return -1;
  static const int monthDays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  const int limit = monthDays[month - 1] +
      (month == 2 && year % 4 == 0 && (year % 100 != 0 || year % 400 == 0) ? 1 : 0);
  return day <= limit ? calendarDay(year, month, day) : -1;
}

bool validGrowDates(const GrowDates& dates) {
  if (dates.germination[10] != '\0' || dates.flowering[10] != '\0') return false;
  const int germination = parseGrowDate(dates.germination);
  const int flowering = parseGrowDate(dates.flowering);
  return germination >= 0 &&
      (dates.flowering[0] == '\0' || (flowering >= 0 && flowering >= germination));
}

bool loadGrowDates() {
  if (!LittleFS.exists("/grow-dates.bin")) return true;
  File file = LittleFS.open("/grow-dates.bin", "r");
  GrowDates saved = {};
  const bool valid = file && file.size() == sizeof(saved) &&
      file.read(reinterpret_cast<uint8_t*>(&saved), sizeof(saved)) == sizeof(saved) &&
      validGrowDates(saved);
  if (file) file.close();
  if (!valid) {
    Serial.println(F("Grow dates invalid or unreadable; dates unavailable"));
    return false;
  }
  growDates = saved;
  return true;
}

bool saveGrowDates(const GrowDates& dates) {
  File file = LittleFS.open("/grow-dates.tmp", "w");
  if (!file) {
    Serial.println(F("Cannot open grow dates temporary file"));
    return false;
  }
  const bool written = file.write(reinterpret_cast<const uint8_t*>(&dates), sizeof(dates)) == sizeof(dates);
  file.close();
  File check = LittleFS.open("/grow-dates.tmp", "r");
  GrowDates saved = {};
  const bool verified = written && check && check.size() == sizeof(saved) &&
      check.read(reinterpret_cast<uint8_t*>(&saved), sizeof(saved)) == sizeof(saved) &&
      memcmp(&saved, &dates, sizeof(saved)) == 0;
  if (check) check.close();
  if (!verified || !LittleFS.rename("/grow-dates.tmp", "/grow-dates.bin")) {
    Serial.println(F("Grow dates save failed; previous dates retained"));
    return false;
  }
  return true;
}

int daysSinceGrowDate(time_t now, const char* startDate) {
  if (!growDatesReady) return -1;
  const int start = parseGrowDate(startDate);
  if (start < 0) return -1;
  if (now < 1700000000) return -1;
  struct tm date;
  if (localtime_r(&now, &date) == nullptr) return -1;
  const int year = date.tm_year + 1900 - 1;
  // Calendar days, not elapsed 24-hour periods, so DST cannot shift J+.
  const int today = 365 * year + year / 4 - year / 100 + year / 400 + date.tm_yday;
  return today >= start ? today - start : -1;
}

int germinationDays(time_t now) {
  return daysSinceGrowDate(now, growDates.germination);
}

void handleApi();

void handleGrowDates() {
  if (!growDatesReady) {
    server.send(503, F("application/json"), F("{\"error\":\"Stockage des dates indisponible\"}"));
    return;
  }
  if (!server.hasArg("germination") || !server.hasArg("flowering") ||
      server.arg("germination").length() != 10 || server.arg("flowering").length() > 10) {
    server.send(400, F("application/json"), F("{\"error\":\"Dates requises au format AAAA-MM-JJ\"}"));
    return;
  }
  GrowDates next = {};
  server.arg("germination").toCharArray(next.germination, sizeof(next.germination));
  server.arg("flowering").toCharArray(next.flowering, sizeof(next.flowering));
  if (!validGrowDates(next)) {
    server.send(400, F("application/json"), F("{\"error\":\"Dates invalides ou floraison avant germination\"}"));
    return;
  }
  if (!saveGrowDates(next)) {
    server.send(500, F("application/json"), F("{\"error\":\"Echec de sauvegarde; anciennes dates conservees\"}"));
    return;
  }
  growDates = next;
  oledRedraw = true;
  handleApi();
}

void handleApi() {
  String json = F("{\"wifi\":");
  json += WiFi.status() == WL_CONNECTED ? F("true") : F("false");
  json += F(",\"ip\":\"");
  json += WiFi.localIP().toString();
  json += F("\",\"valid\":");

  const bool valid = !isnan(temperature) && !isnan(humidity);
  json += valid ? F("true") : F("false");
  json += F(",\"temperature\":");
  json += valid ? String(temperature, 1) : String(F("null"));
  json += F(",\"humidity\":");
  json += valid ? String(humidity, 1) : String(F("null"));
  json += F(",\"soilRaw\":");
  json += String(soilRaw);
  json += F(",\"soilPercent\":");
  json += String(soilMoisturePercent());
  json += F(",\"soilStatus\":\"");
  json += soilCondition();
  json += '"';
  json += F(",\"germinationDays\":");
  const int days = germinationDays(time(nullptr));
  json += days >= 0 ? String(days) : String(F("null"));
  json += F(",\"germinationDate\":\"");
  json += growDatesReady ? growDates.germination : "";
  json += F("\",\"floweringDate\":\"");
  json += growDatesReady ? growDates.flowering : "";
  json += F("\",\"floweringDays\":");
  const int floweringDays = daysSinceGrowDate(time(nullptr), growDates.flowering);
  json += floweringDays >= 0 ? String(floweringDays) : String(F("null"));
  json += F(",\"growDatesAvailable\":");
  json += growDatesReady ? F("true") : F("false");
  json += F(",\"clockReady\":");
  json += time(nullptr) >= 1700000000 ? F("true") : F("false");
  json += '}';

  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), json);
}

bool loadHistory() {
  File file = LittleFS.open("/history.bin", "r");
  if (!file) {
    historyStore = {};
    historyStore.magic = HISTORY_MAGIC;
    return true;
  }

  bool validFile = false;
  if (file.size() == sizeof(historyStore)) {
    validFile = file.read(reinterpret_cast<uint8_t*>(&historyStore), sizeof(historyStore)) == sizeof(historyStore);
  } else if (file.size() == sizeof(LegacyHistoryStore)) {
    LegacyHistoryStore legacy = {};
    const bool readLegacy =
        file.read(reinterpret_cast<uint8_t*>(&legacy), sizeof(legacy)) == sizeof(legacy);
    if (readLegacy && legacy.magic == LEGACY_HISTORY_MAGIC &&
        legacy.head < HISTORY_CAPACITY && legacy.count <= HISTORY_CAPACITY) {
      historyStore = {};
      historyStore.magic = HISTORY_MAGIC;
      historyStore.head = legacy.head;
      historyStore.count = legacy.count;
      for (uint16_t i = 0; i < HISTORY_CAPACITY; i++) {
        historyStore.samples[i].timestamp = legacy.samples[i].timestamp;
        historyStore.samples[i].temperatureTenths = legacy.samples[i].temperatureTenths;
        historyStore.samples[i].humidityTenths = legacy.samples[i].humidityTenths;
        historyStore.samples[i].soilPercent = 255;
      }
      file.close();
      Serial.println(F("Migrated existing 24-hour history; soil readings begin now"));
      return true;
    }
  }
  file.close();

  if (!validFile || historyStore.magic != HISTORY_MAGIC ||
      historyStore.head >= HISTORY_CAPACITY || historyStore.count > HISTORY_CAPACITY) {
    historyStore = {};
    historyStore.magic = HISTORY_MAGIC;
    Serial.println(F("History file invalid; starting with empty history"));
    return false;
  }

  return true;
}

bool loadMonthHistory() {
  File file = LittleFS.open("/month.bin", "r");
  if (!file) {
    monthHistoryStore = {};
    monthHistoryStore.magic = MONTH_HISTORY_MAGIC;
    return true;
  }

  const bool validFile = file.size() == sizeof(monthHistoryStore) &&
      file.read(reinterpret_cast<uint8_t*>(&monthHistoryStore), sizeof(monthHistoryStore)) ==
          sizeof(monthHistoryStore);
  file.close();

  if (!validFile || monthHistoryStore.magic != MONTH_HISTORY_MAGIC ||
      monthHistoryStore.head >= MONTH_HISTORY_CAPACITY ||
      monthHistoryStore.count > MONTH_HISTORY_CAPACITY) {
    monthHistoryStore = {};
    monthHistoryStore.magic = MONTH_HISTORY_MAGIC;
    Serial.println(F("Monthly history file invalid; starting with empty history"));
    return false;
  }

  return true;
}

bool saveHistory() {
  File file = LittleFS.open("/history.bin", "w");
  if (!file) {
    Serial.println(F("Could not open history file for writing"));
    return false;
  }

  const size_t written = file.write(reinterpret_cast<const uint8_t*>(&historyStore), sizeof(historyStore));
  file.flush();
  file.close();

  if (written != sizeof(historyStore)) {
    Serial.println(F("History file write was incomplete"));
    return false;
  }

  return true;
}

bool saveMonthHistory() {
  File file = LittleFS.open("/month.bin", "w");
  if (!file) {
    Serial.println(F("Could not open monthly history file for writing"));
    return false;
  }

  const size_t written = file.write(
      reinterpret_cast<const uint8_t*>(&monthHistoryStore), sizeof(monthHistoryStore));
  file.flush();
  file.close();

  if (written != sizeof(monthHistoryStore)) {
    Serial.println(F("Monthly history write was incomplete"));
    return false;
  }

  return true;
}

void recordHistory(time_t timestamp) {
  if (timestamp < 1700000000 || isnan(temperature) || isnan(humidity)) {
    return;
  }

  const uint32_t currentTimestamp = static_cast<uint32_t>(timestamp);
  HistorySample currentSample = {};
  currentSample.timestamp = currentTimestamp;
  currentSample.temperatureTenths = static_cast<int16_t>(lroundf(temperature * 10.0f));
  currentSample.humidityTenths = static_cast<uint16_t>(lroundf(humidity * 10.0f));
  currentSample.soilPercent = static_cast<uint8_t>(soilMoisturePercent());
  updateAllTimeRecords(currentSample);

  bool newShortTermSample = true;
  if (historyStore.count > 0) {
    const uint16_t lastIndex = (historyStore.head + HISTORY_CAPACITY - 1) % HISTORY_CAPACITY;
    if (historyStore.samples[lastIndex].timestamp / HISTORY_INTERVAL_SECONDS ==
        currentTimestamp / HISTORY_INTERVAL_SECONDS) {
      newShortTermSample = false;
    }
  }

  if (newShortTermSample) {
    HistorySample& sample = historyStore.samples[historyStore.head];
    sample.timestamp = currentTimestamp;
    sample.temperatureTenths = currentSample.temperatureTenths;
    sample.humidityTenths = currentSample.humidityTenths;
    sample.soilPercent = currentSample.soilPercent;
    historyStore.head = (historyStore.head + 1) % HISTORY_CAPACITY;
    if (historyStore.count < HISTORY_CAPACITY) {
      historyStore.count++;
    }

    const uint32_t cutoff = currentTimestamp - 86400;
    while (historyStore.count > 0) {
      const uint16_t oldestIndex = (historyStore.head + HISTORY_CAPACITY - historyStore.count) % HISTORY_CAPACITY;
      if (historyStore.samples[oldestIndex].timestamp >= cutoff) {
        break;
      }
      historyStore.count--;
    }

    saveHistory();
    saveAllTimeRecords();
  }

  if (monthHistoryStore.count > 0) {
    const uint16_t lastIndex =
        (monthHistoryStore.head + MONTH_HISTORY_CAPACITY - 1) % MONTH_HISTORY_CAPACITY;
    if (monthHistoryStore.samples[lastIndex].timestamp / MONTH_HISTORY_INTERVAL_SECONDS ==
        currentTimestamp / MONTH_HISTORY_INTERVAL_SECONDS) {
      return;
    }
  }

  HistorySample& monthSample = monthHistoryStore.samples[monthHistoryStore.head];
  monthSample.timestamp = currentTimestamp;
  monthSample.temperatureTenths = static_cast<int16_t>(lroundf(temperature * 10.0f));
  monthSample.humidityTenths = static_cast<uint16_t>(lroundf(humidity * 10.0f));
  monthSample.soilPercent = static_cast<uint8_t>(soilMoisturePercent());
  monthHistoryStore.head = (monthHistoryStore.head + 1) % MONTH_HISTORY_CAPACITY;
  if (monthHistoryStore.count < MONTH_HISTORY_CAPACITY) {
    monthHistoryStore.count++;
  }

  const uint32_t monthCutoff = currentTimestamp - 30UL * 86400UL;
  while (monthHistoryStore.count > 0) {
    const uint16_t oldestIndex =
        (monthHistoryStore.head + MONTH_HISTORY_CAPACITY - monthHistoryStore.count) %
        MONTH_HISTORY_CAPACITY;
    if (monthHistoryStore.samples[oldestIndex].timestamp >= monthCutoff) {
      break;
    }
    monthHistoryStore.count--;
  }
  saveMonthHistory();
}

void handleHistory() {
  const time_t now = time(nullptr);
  const bool clockReady = now >= 1700000000;
  const String range = server.hasArg("range") ? server.arg("range") : String("24h");
  uint32_t periodSeconds = 0;
  if (range == "1h") periodSeconds = 3600;
  else if (range == "3h") periodSeconds = 3UL * 3600UL;
  else if (range == "6h") periodSeconds = 6UL * 3600UL;
  else if (range == "12h") periodSeconds = 12UL * 3600UL;
  else if (range == "24h" || range == "1d") periodSeconds = 86400;
  else if (range == "1m") periodSeconds = 30UL * 86400UL;
  else {
    server.send(400, F("application/json"), F("{\"error\":\"unsupported history range\"}"));
    return;
  }

  const bool useMonthlyHistory = range == "1m";
  const uint16_t sampleCount = useMonthlyHistory ? monthHistoryStore.count : historyStore.count;
  const uint16_t capacity = useMonthlyHistory ? MONTH_HISTORY_CAPACITY : HISTORY_CAPACITY;
  const uint16_t head = useMonthlyHistory ? monthHistoryStore.head : historyStore.head;
  const HistorySample* samples = useMonthlyHistory
      ? monthHistoryStore.samples : historyStore.samples;
  const uint32_t currentTimestamp = clockReady ? static_cast<uint32_t>(now) : 0;
  const uint32_t cutoff = clockReady && currentTimestamp > periodSeconds
      ? currentTimestamp - periodSeconds : 0;
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.sendHeader(F("Cache-Control"), F("no-store"));
  server.send(200, F("application/json"), "");

  String chunk;
  chunk.reserve(512);
  chunk = F("{\"clockReady\":");
  chunk += clockReady ? F("true") : F("false");
  chunk += F(",\"now\":");
  chunk += clockReady ? String(currentTimestamp) : String(F("0"));
  chunk += F(",\"rangeSeconds\":");
  chunk += String(periodSeconds);
  chunk += F(",\"samples\":[");
  bool first = true;

  for (uint16_t i = 0; i < sampleCount; i++) {
    const uint16_t index = (head + capacity - sampleCount + i) % capacity;
    const HistorySample& sample = samples[index];
    if (!clockReady || sample.timestamp < cutoff || sample.timestamp > currentTimestamp) {
      continue;
    }

    if (!first) chunk += ',';
    first = false;
    chunk += '[';
    chunk += String(sample.timestamp);
    chunk += ',';
    chunk += String(sample.temperatureTenths / 10.0f, 1);
    chunk += ',';
    chunk += String(sample.humidityTenths / 10.0f, 1);
    chunk += ',';
    chunk += sample.soilPercent == 255 ? String(F("null")) : String(sample.soilPercent);
    chunk += ']';
    if (chunk.length() >= 400) {
      server.sendContent(chunk);
      chunk = "";
    }
  }

  chunk += F("],\"events\":[");
  bool firstEvent = true;
  for (uint16_t i = 0; i < wateringStore.count; i++) {
      const uint16_t index =
          (wateringStore.head + WATERING_CAPACITY - wateringStore.count + i) %
          WATERING_CAPACITY;
      const uint32_t eventTimestamp = wateringStore.timestamps[index];
      if (!clockReady || eventTimestamp < cutoff || eventTimestamp > currentTimestamp) {
        continue;
      }
      if (!firstEvent) chunk += ',';
      firstEvent = false;
      chunk += '[';
      chunk += String(eventTimestamp);
      chunk += ',';
      chunk += String(wateringStore.fertilizer[index]);
      chunk += ']';
      if (chunk.length() >= 400) {
        server.sendContent(chunk);
        chunk = "";
      }
  }

  chunk += F("],\"records\":{");
  if (allTimeRecords.sampleCount > 0) {
      chunk += F("\"temperature\":{\"min\":");
      chunk += String(allTimeRecords.temperatureMinTenths / 10.0f, 1);
      chunk += F(",\"max\":");
      chunk += String(allTimeRecords.temperatureMaxTenths / 10.0f, 1);
      chunk += F("},\"airHumidity\":{\"min\":");
      chunk += String(allTimeRecords.airHumidityMinTenths / 10.0f, 1);
      chunk += F(",\"max\":");
      chunk += String(allTimeRecords.airHumidityMaxTenths / 10.0f, 1);
      chunk += '}';
  } else {
      chunk += F("\"temperature\":null,\"airHumidity\":null");
  }
  chunk += F(",\"soilMoisture\":");
  if (allTimeRecords.soilSampleCount > 0) {
      chunk += F("{\"min\":");
      chunk += String(allTimeRecords.soilMoistureMin);
      chunk += F(",\"max\":");
      chunk += String(allTimeRecords.soilMoistureMax);
      chunk += '}';
  } else {
      chunk += F("null");
  }
  chunk += F("}}");
  server.sendContent(chunk);
  server.sendContent("");
}

void handleOledButton() {
  const uint32_t now = millis();
  const bool reading = digitalRead(OLED_BUTTON_PIN);
  if (reading != oledButtonReading) {
    oledButtonReading = reading;
    oledButtonChangedAt = now;
  }
  if (reading != oledButtonStable &&
      static_cast<uint32_t>(now - oledButtonChangedAt) >= 40) {
    oledButtonStable = reading;
    if (reading == LOW) {
      oledSoilPage = !oledSoilPage;
      oledRedraw = true;
    }
  }
}

void drawOledPage() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextWrap(false);
  display.setCursor(0, 0);
  if (oledSoilPage) {
    display.setCursor(0, 24);
    display.print(soilMoisturePercent());
    display.print(F(" %"));
  } else {
    display.setCursor(0, 0);
    if (isnan(temperature)) display.print(F("Erreur"));
    else {
      display.print(temperature, 1);
      display.print(F(" C"));
    }
    display.setCursor(0, 24);
    if (isnan(humidity)) display.print(F("Erreur"));
    else {
      display.print(humidity, 1);
      display.print(F(" %"));
    }
  }
  display.setCursor(0, 48);
  display.print(F("J+"));
  const int days = germinationDays(time(nullptr));
  if (days >= 0) display.print(days);
  else display.print(F("--"));
  display.display();
}

void setup() {
  Serial.begin(115200);
  pinMode(OLED_BUTTON_PIN, INPUT_PULLUP);
  oledButtonReading = oledButtonStable = digitalRead(OLED_BUTTON_PIN);

  // This board wires the onboard OLED with SDA=GPIO5 (D1), SCL=GPIO4 (D2).
  Wire.begin(5, 4);
  dht.begin();

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println(F("OLED initialization failed"));
    while (true) {
      delay(1000);
    }
  }

  if (LittleFS.begin()) {
    seedBankReady = true;
    growDatesReady = loadGrowDates();
    loadHistory();
    loadMonthHistory();
    loadAllTimeRecords();
    loadWateringHistory();
  } else {
    Serial.println(F("LittleFS mount failed; sensor history will not be saved"));
  }

  display.setTextColor(SSD1306_WHITE);
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println(F("Connexion"));
  display.println(F("WiFi..."));
  display.display();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(250);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("WiFi connected. IP: "));
    Serial.println(WiFi.localIP());
    configTime("CET-1CEST,M3.5.0/2,M10.5.0/3", "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println(F("WiFi connection timed out"));
  }

  if (WiFi.status() == WL_CONNECTED) {
    dashboardMdnsStarted = MDNS.begin("grow-dashboard-ota");
    if (dashboardMdnsStarted) {
      MDNS.addService("http", "tcp", 80);
    }
  }
  beginDashboardOTA();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api", HTTP_GET, handleApi);
  server.on("/grow/dates", HTTP_POST, handleGrowDates);
  server.on("/seed-bank", HTTP_GET, handleSeedBankPage);
  server.on("/seed-bank/api", HTTP_GET, handleSeedBank);
  server.on("/seed-bank/api", HTTP_POST, handleSeedBankSave);
  server.on("/history", HTTP_GET, handleHistory);
  server.on("/watering", HTTP_POST, handleWatering);
  server.on("/relay/status", HTTP_GET, handleRelayStatus);
  server.on("/relay/program", HTTP_POST, handleRelayProgram);
  server.on("/relay/temporary", HTTP_POST, handleRelayTemporary);
  server.on("/relay/condition", HTTP_POST, handleRelayCondition);
  server.on("/records/reset", HTTP_POST, handleResetRecords);
  server.begin();
  Serial.println(F("Web server started on port 80"));

  lastSensorRead = millis() - 2000;
}

void loop() {
  handleOledButton();
  server.handleClient();
  beginDashboardOTA();
  if (dashboardMdnsStarted) {
    MDNS.update();
  }
  if (dashboardOtaStarted) {
    ArduinoOTA.handle();
  }

  static unsigned long lastReconnectAttempt = 0;
  if (WiFi.status() != WL_CONNECTED &&
      millis() - lastReconnectAttempt >= 10000) {
    lastReconnectAttempt = millis();
    WiFi.reconnect();
  }

  const unsigned long now = millis();
  if (now - lastSensorRead >= 2000) {
    lastSensorRead = now;
    soilRaw = analogRead(SOIL_SENSOR_PIN);
    Serial.printf("Soil ADC raw: %d/1023, status: ", soilRaw);
    Serial.println(soilCondition());
    humidity = dht.readHumidity();
    temperature = dht.readTemperature();
    sensorReadReady = true;

    if (isnan(humidity) || isnan(temperature)) {
      Serial.println(F("DHT22 read failed"));
    } else {
      Serial.printf("Temperature: %.1f C  Humidity: %.1f%%\n", temperature, humidity);
      recordHistory(time(nullptr));
    }
  }

  forwardRelaySensors();

  handleOledButton();
  if (oledRedraw || static_cast<uint32_t>(millis() - lastDisplayUpdate) >= 1000) {
    lastDisplayUpdate = millis();
    oledRedraw = false;
    drawOledPage();
  }
}


/*
  BMP280 Logger V7.4.3 Full MultiNode Hybrid - WebGUI OTA MQTT HA

  Ziel:
  - Alle Logger-Funktionen aus V5 wieder drin
  - MultiNode Secure Pairing / Sensor / Gateway / Hybrid drin
  - Lokales OLED-Menü mit Encoder/Tasten
  - Windows GUI kompatible NODEDATA Ausgabe
  - Sensor kann alleine laufen, Gateway sein oder Hybrid am PC
  - DeepSleep für Batterie-Logger
  - USB Auto Normal
  - CSV Logging lokal und Gateway-CSV pro Node
  - WebGUI lokal im WLAN/AP mit Login
  - OTA Firmware Update ueber Browser

  Hardware:
  - ESP32-C3
  - SH1106 OLED 128x64 I2C 0x3C
  - BMP280 I2C 0x76
  - Encoder/Tasten:
      ENCODER_TRA   GPIO0
      ENCODER_TRB   GPIO1
      ENCODER_PUSH  GPIO3
      BAK           GPIO4
      CONTR         GPIO5

  Arduino IDE:
  - Board: ESP32C3 Dev Module
  - USB CDC On Boot: Enabled
  - Partition Scheme: Default 4MB with spiffs oder OTA-faehiges Schema

  Libraries:
  - U8g2
  - Adafruit BMP280 Library
  - Adafruit Unified Sensor
  - PubSubClient by Nick O'Leary fuer MQTT/Home Assistant
  - WebServer/Update sind im ESP32 Boardpaket enthalten
*/

#include <Arduino.h>
// USB console integrated for single-file installation.

// Main-loop-only console. Complete lines are queued before any bytes are sent.
// A missing/slow host must never hold up measurement, keys or OLED updates.
class LoggerConsole : public Print {
 public:
  using Print::write;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *data, size_t size) override {
    for (size_t i = 0; i < size; ++i) {
      uint8_t c = data[i];
      if (!lineOverflow) {
        if (lineUsed < sizeof(line)) line[lineUsed++] = c;
        else lineOverflow = true;
      }
      if (c == '\n') {
        if (!lineOverflow && lineUsed <= freeBytes()) {
          for (size_t j = 0; j < lineUsed; ++j) {
            queue[tail] = line[j];
            tail = (tail + 1) % sizeof(queue);
          }
          used += lineUsed;
        } else ++dropped;
        lineUsed = 0;
        lineOverflow = false;
      }
    }
    // Diagnostics may be dropped offline; authoritative data remains in LittleFS.
    return size;
  }
  size_t freeBytes() const { return sizeof(queue) - used; }
  uint32_t droppedLines() const { return dropped; }
  template<class Transport> void pump(Transport &out) {
    if (!used) return;
    int room = out.availableForWrite();
    if (room <= 0) return;
    size_t n = used;
    if (n > 256) n = 256;  // bounded work, one transport write per call
    if (n > (size_t)room) n = (size_t)room;
    if (n > sizeof(queue) - head) n = sizeof(queue) - head;
    size_t sent = out.write(queue + head, n);
    if (sent > n) sent = n;
    head = (head + sent) % sizeof(queue);
    used -= sent;  // retain unsent bytes after a short write
  }
 private:
  uint8_t queue[8192] = {};
  uint8_t line[1024] = {};
  size_t head = 0, tail = 0, used = 0, lineUsed = 0;
  bool lineOverflow = false;
  uint32_t dropped = 0;
};

LoggerConsole loggerConsole;
#include <Wire.h>
#include <WiFi.h>
#define MQTT_MAX_PACKET_SIZE 1024
#include <PubSubClient.h>
#define BMP_HAS_MQTT 1
#include <WebServer.h>
#include <Update.h>
// V7.4.3: use IP access; omit mDNS to fit the standard OTA slot with Core 3.3.11.
#include <esp_now.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <U8g2lib.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#if __has_include(<esp_arduino_version.h>)
  #include <esp_arduino_version.h>
#endif

// --------------------------------------------------
// Version / Netzwerk
// --------------------------------------------------
#define FW_VERSION "V7.4.3"
#define PROTO_VERSION 3 // V74 acknowledged packets: update all nodes/gateway together
// Public example only: set the same private secret on your own nodes before pairing.
#define PAIR_SECRET "EXAMPLE-CHANGE-THIS-PAIR-SECRET"

#define NODE_TYPE 'B'
#define MFG_YEAR 2026
#define MFG_DAY_OF_YEAR 184
#define FW_FAMILY 'F'

// --------------------------------------------------
// Hardware
// --------------------------------------------------
#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9

#define PIN_ENC_A     0
#define PIN_ENC_B     1
#define PIN_ENC_PUSH  3
#define PIN_BAK       4
#define PIN_CONTR     5

#define OLED_ADDR_8BIT (0x3C * 2)
#define BMP280_ADDR 0x76

// --------------------------------------------------
// Betriebsmodi
// --------------------------------------------------
#define MODE_SENSOR   0
#define MODE_GATEWAY  1
#define MODE_HYBRID   2

#define POWER_NORMAL  0
#define POWER_ECO     1
#define POWER_DEEP    2

#define PT_PAIR_REQ    1
#define PT_PAIR_ACCEPT 2
#define PT_DATA        3
#define PT_CMD         4

#define MAX_TRUSTED 20
#define MAX_PENDING 10

const char *LOCAL_LOG_FILE = "/local_v74.csv";

uint8_t broadcastAddress[] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);
Adafruit_BMP280 bmp;
Preferences prefs;

// --------------------------------------------------
// Config
// --------------------------------------------------
struct Config {
  uint8_t mode;
  uint8_t powerMode;
  bool loggingEnabled;
  bool liveOutput;

  uint32_t intervalSec;
  float maxDays;

  float tempOffsetC;
  float pressureOffsetHpa;

  uint8_t screenMode;    // 0 Auto, 1 Dashboard, 2 Temp, 3 Graph, 4 Pressure, 5 Storage, 6 Cal, 7 Net, 8 Power
  uint8_t graphMode;     // 0 Temp, 1 Druck
  uint8_t graphWindow;   // 0=1h, 1=6h, 2=12h, 3=24h
  uint8_t contrast;

  uint16_t displayTimeoutSec;
  uint16_t wakeDisplaySec;
  bool usbAutoNormal;
  uint8_t usbWindowSec;

  bool pairMode;
  bool paired;
  uint8_t gatewayMac[6];

  char nodeId[11];
  char nodeName[16];
  uint8_t language;       // 0 = DE, 1 = EN

  bool webEnabled;
  char wifiSsid[33];
  char wifiPass[65];
  char webUser[17];
  char webPass[33];
  char webHost[33];

  bool mqttEnabled;
  bool mqttDiscovery;
  char mqttHost[65];
  uint16_t mqttPort;
  char mqttUser[33];
  char mqttPass[33];
  char mqttPrefix[33];
};

Config cfg;
RTC_DATA_ATTR uint32_t rtcMagic = 0;
RTC_DATA_ATTR uint32_t sessionId = 0;
RTC_DATA_ATTR uint32_t sampleSequence = 0;
RTC_DATA_ATTR uint64_t elapsedBaseMs = 0;
bool fsReady = false;
bool storageFault = false;
bool sensorValid = false;
bool sampleStored = false;
String storageMessage = "OK";
uint32_t rxDropped = 0;
uint32_t lastSensorRetryMs = 0;
uint32_t lastChannelTryMs = 0;
uint32_t lastGoodRxMs = 0;
RTC_DATA_ATTR uint32_t radioCursor = 0;
RTC_DATA_ATTR uint32_t cachedLogRows=0;
RTC_DATA_ATTR uint32_t cachedLogBytes=0;
RTC_DATA_ATTR uint8_t missedRadioWakes = 0;
uint8_t radioChannel = 1;
uint32_t lastRxSession[MAX_TRUSTED] = {};
uint32_t lastRxSequence[MAX_TRUSTED] = {};

uint32_t sampleAtMs = 0;
uint64_t sampleElapsed = 0;
uint64_t sampleEpoch = 0;
const size_t STORAGE_RESERVE = 16384;


// --------------------------------------------------
// Trusted / Pending
// --------------------------------------------------
struct TrustedNode {
  bool used;
  char id[11];
  uint8_t mac[6];
  char name[16];
  uint32_t lastLog;
  uint32_t packets;
  uint32_t lastSeenMs;
};

struct PendingNode {
  bool used;
  char id[11];
  char fw[8];
  uint8_t mac[6];
  uint32_t lastSeenMs;
};

TrustedNode trusted[MAX_TRUSTED];
PendingNode pendingNodes[MAX_PENDING];
uint32_t nodeTimeoutMs[MAX_TRUSTED] = {};
bool nodeOnline[MAX_TRUSTED] = {};


// --------------------------------------------------
// Pakete
// --------------------------------------------------
struct __attribute__((packed)) PairReqPacket {
  char magic[4];        // BPN2
  uint8_t proto;
  uint8_t type;
  char id[11];
  char fw[8];
  uint8_t mac[6];
  uint32_t nonce;
};

struct __attribute__((packed)) PairAcceptPacket {
  char magic[4];
  uint8_t proto;
  uint8_t type;
  char id[11];
  uint8_t nodeMac[6];
  uint8_t gatewayMac[6];
  uint32_t token;
};

struct __attribute__((packed)) DataPacket {
  char magic[4];
  uint8_t proto;
  uint8_t type;
  char id[11];
  char name[16];
  char fw[8];
  uint8_t mac[6];
  uint32_t logNo;
  uint32_t ms;
  float rawTempC;
  float tempC;
  float rawPressureHpa;
  float pressureHpa;
  float tempOffsetC;
  float pressureOffsetHpa;
  uint8_t mode;
  uint8_t powerMode;
  uint8_t wakeCause;
  uint8_t flags; // bit0 logging enabled; bit1 valid sensor; bit2 stored locally
  uint32_t sequence;
  uint32_t session;
  uint64_t elapsedMs;
  uint64_t epochMs;
  uint32_t intervalSec;
};

struct __attribute__((packed)) CommandPacket {
  char magic[4];
  uint8_t proto;
  uint8_t type;
  char id[11];
  uint8_t command;
  float value;
  uint32_t token;
};

struct __attribute__((packed)) AckPacket {
  char magic[4]; uint8_t proto; uint8_t type; char id[11]; uint32_t session; uint32_t sequence;
};
DataPacket radioPending = {};
bool radioWaiting = false;
uint32_t radioNextCursor = 0;
uint32_t radioLastSend = 0;
uint8_t radioTries = 0;
uint32_t radioConfirmed = 0;
struct RxEnvelope { uint8_t source[6]; uint16_t length; uint8_t bytes[250]; };
QueueHandle_t rxQueue = nullptr;
File syncFile;
int syncSlot = -2;
bool syncAll = false;
uint32_t syncRemaining = 0;
String syncLine;
File dumpStream;

// --------------------------------------------------
// Messwerte / Status
// --------------------------------------------------
float rawTempC = NAN;
float tempC = NAN;
float rawPressureHpa = NAN;
float pressureHpa = NAN;

float minTempC = NAN;
float maxTempC = NAN;
float minPressureHpa = NAN;
float maxPressureHpa = NAN;

uint32_t readCount = 0;
uint32_t logCount = 0;

bool oledOk = false;
bool bmpOk = false;
bool displayOn = true;

unsigned long lastReadMs = 0;
unsigned long lastLogMs = 0;
unsigned long lastDisplayMs = 0;
unsigned long lastAutoPageMs = 0;
unsigned long lastUserActionMs = 0;
unsigned long lastPairBroadcastMs = 0;
unsigned long lastWebStatusMs = 0;

WebServer webServer(80);
bool webRunning = false;
bool webRoutesConfigured = false;
String webLastUpdateError = "";

#if BMP_HAS_MQTT
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
#endif
bool mqttConnected = false;
bool mqttDiscoverySent = false;
uint32_t mqttPublishFailures=0;
uint32_t lastDiscoveryTryMs=0;
String mqttLastState = "MQTT off";
unsigned long lastMqttAttemptMs = 0;
unsigned long lastMqttPublishMs = 0;

uint8_t autoPage = 0;
esp_sleep_wakeup_cause_t wakeCause;

// --------------------------------------------------
// OLED-History
// --------------------------------------------------
// GRAPH743_BEGIN
struct HourPoint {
  uint64_t timeMs = 0;
  float temperature = NAN, pressure = NAN;
  bool valid = false;
};
class HourHistory {
 public:
  static const uint16_t CAPACITY = 1441;
  void add(uint64_t now, float temperature, float pressure) {
    HourPoint &point = points[(now / 60000ULL) % CAPACITY];
    point.timeMs = now; point.temperature = temperature; point.pressure = pressure;
    point.valid = true;
  }
  const HourPoint *atAge(uint64_t now, uint16_t minutes) const {
    uint64_t current = now / 60000ULL;
    if (minutes > current || minutes >= CAPACITY) return nullptr;
    uint64_t wanted = current - minutes;
    const HourPoint &point = points[wanted % CAPACITY];
    if (!point.valid || point.timeMs / 60000ULL != wanted || point.timeMs > now) return nullptr;
    return &point;
  }
  void clear() { for (auto &point : points) point.valid = false; }
 private:
  HourPoint points[CAPACITY];
};
// GRAPH743_END
HourHistory hourHistory;
const uint8_t graphHours[4] = {1, 6, 12, 24};

#define HISTORY_SIZE 96
float tempHistory[HISTORY_SIZE];
float pressHistory[HISTORY_SIZE];
uint8_t historyIndex = 0;
uint8_t historyCount = 0;

// --------------------------------------------------
// UI
// --------------------------------------------------
enum UiMode {
  UI_DASHBOARD,
  UI_MENU,
  UI_EDIT_INTERVAL,
  UI_EDIT_MAXDAYS,
  UI_EDIT_TIMEOUT,
  UI_EDIT_WAKE_DISPLAY,
  UI_EDIT_USB_WINDOW,
  UI_EDIT_TEMP_OFFSET,
  UI_EDIT_PRESS_OFFSET,
  UI_CONFIRM_CLEAR
};

UiMode uiMode = UI_DASHBOARD;
int menuIndex = 0;
int editValue = 0;
float editFloat = 0.0f;

const char *menuItemsDe[] = {
  "Zurueck",
  "Logging Start/Stop",
  "1x Messung loggen",
  "Intervall",
  "Max Tage",
  "Kalibrierung Temp",
  "Kalibrierung Druck",
  "Power Mode",
  "Display Timeout",
  "Wake Display",
  "USB Auto Normal",
  "USB Window",
  "USB Live",
  "Screen",
  "Graph",
  "Mode Sensor/GW/Hybrid",
  "Pairing AN/AUS",
  "Pair Code",
  "Pair Reset",
  "Reset Statistik",
  "Log loeschen",
  "Sprache DE/EN"
};

const char *menuItemsEn[] = {
  "Back",
  "Start/Stop Log",
  "Log 1 Sample",
  "Interval",
  "Max Days",
  "Temp Cal",
  "Pressure Cal",
  "Power Mode",
  "Display Timeout",
  "Wake Display",
  "USB Auto Normal",
  "USB Window",
  "USB Live",
  "Screen",
  "Graph",
  "Mode Sensor/GW/Hyb",
  "Pairing ON/OFF",
  "Pair Code",
  "Pair Reset",
  "Reset Stats",
  "Clear Log",
  "Language DE/EN"
};

const int MENU_COUNT = sizeof(menuItemsDe) / sizeof(menuItemsDe[0]);
const char *menuItemText(int idx) {
  if (idx < 0 || idx >= MENU_COUNT) return "";
  return cfg.language == 1 ? menuItemsEn[idx] : menuItemsDe[idx];
}
const char *languageCode() {
  return cfg.language == 1 ? "en" : "de";
}
const char *languageLabel() {
  return cfg.language == 1 ? "English" : "Deutsch";
}

String lastEvent = "Bereit";

// --------------------------------------------------
// Buttons / Encoder
// --------------------------------------------------
const uint32_t BUTTON_DEBOUNCE_MS = 50;
const uint32_t ENCODER_DEBOUNCE_US = 800;

struct DebouncedButton {
  const char *name;
  uint8_t pin;
  bool stableState;
  bool lastRawState;
  uint32_t lastChangeMs;
  bool pressedEvent;
  bool releasedEvent;
};

DebouncedButton btnPush = {"ENC_PUSH", PIN_ENC_PUSH, HIGH, HIGH, 0, false, false};
DebouncedButton btnBak  = {"BAK",      PIN_BAK,      HIGH, HIGH, 0, false, false};
DebouncedButton btnCon  = {"CONTR",    PIN_CONTR,    HIGH, HIGH, 0, false, false};

int8_t encoderDelta = 0;
uint8_t lastEncoderState = 0;
int8_t encoderAccumulator = 0;
uint32_t lastEncoderChangeUs = 0;

const int8_t encoderTable[16] = {
   0, -1,  1,  0,
   1,  0,  0, -1,
  -1,  0,  0,  1,
   0,  1, -1,  0
};

// --------------------------------------------------
// Explizite Funktions-Prototypen
// Wichtig für Arduino-IDE Preprocessor bei eigenen Struct-Typen
// --------------------------------------------------
void updateButton(DebouncedButton &btn);
void handleIncoming(const uint8_t *data, int len, const uint8_t *source);
String packetLine(const DataPacket &p);
DataPacket localPacket();
bool appendPacket(const String &path, const DataPacket &p);
void mqttDisconnect();
void serviceRadioSpool();
bool parseJournal(const String &line, DataPacket &p);
void acknowledgePacket(const DataPacket &p);
void sendOledIfChanged();
String mqttBaseTopic(const String &id);

void printAck(const String &cmd, const String &msg = "OK");
void printErr(const String &cmd, const String &msg);
void printStatusData();
void logLocal(bool force = false);
void wakeDisplay(const String &reason);
void handleSetCommand(String cmd);
void handleCalCommand(String cmd);
void setupWebIfAllowed();
void handleWeb();
void stopWebServer();
void printWebStatus();
void handleMqtt();
void printMqttStatus();
void mqttPublishLocalState(bool force = false);
void mqttPublishPacketState(const DataPacket &p);
void mqttPublishDiscoveryAll();


// --------------------------------------------------
// Role helpers
// --------------------------------------------------
bool isGatewayRole() {
  return cfg.mode == MODE_GATEWAY || cfg.mode == MODE_HYBRID;
}

bool isSensorRole() {
  return cfg.mode == MODE_SENSOR || cfg.mode == MODE_HYBRID;
}

// --------------------------------------------------
// Helpers
// --------------------------------------------------
char b36Digit(uint8_t v) {
  if (v < 10) return '0' + v;
  return 'A' + (v - 10);
}

void base36N(uint32_t value, uint8_t digits, char *out) {
  for (int i = digits - 1; i >= 0; i--) {
    out[i] = b36Digit(value % 36);
    value /= 36;
  }
  out[digits] = 0;
}

uint32_t fnv1a(const uint8_t *data, size_t len) {
  uint32_t h = 2166136261UL;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 16777619UL;
  }
  return h;
}

uint32_t hashString(const String &s) {
  return fnv1a((const uint8_t*)s.c_str(), s.length());
}

String macToString(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

bool macEqual(const uint8_t a[6], const uint8_t b[6]) {
  for (int i = 0; i < 6; i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

String cleanInput(String s) {
  s.trim();
  return s;
}

float parseFloatValue(String s) {
  s = cleanInput(s);
  s.replace(",", ".");
  return s.toFloat();
}

String uptimeString() {
  uint32_t total = millis() / 1000;
  uint32_t h = total / 3600;
  uint32_t m = (total % 3600) / 60;
  uint32_t s = total % 60;
  char buf[20];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
           (unsigned long)h, (unsigned long)m, (unsigned long)s);
  return String(buf);
}

const char* modeName() {
  if (cfg.mode == MODE_GATEWAY) return "GATEWAY";
  if (cfg.mode == MODE_HYBRID) return "HYBRID";
  return "SENSOR";
}

const char* powerName() {
  if (cfg.powerMode == POWER_DEEP) return "DEEP";
  if (cfg.powerMode == POWER_ECO) return "ECO";
  return "NORMAL";
}

const char* wakeCauseName() {
  switch (wakeCause) {
    case ESP_SLEEP_WAKEUP_TIMER: return "TIMER";
    case ESP_SLEEP_WAKEUP_GPIO: return "GPIO";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "POWERON";
    default: return "OTHER";
  }
}

void setEvent(const String &msg) {
  lastEvent = msg;
  loggerConsole.println(msg);
}

void updateMinMax(float value, float &minValue, float &maxValue) {
  if (isnan(value)) return;
  if (isnan(minValue) || value < minValue) minValue = value;
  if (isnan(maxValue) || value > maxValue) maxValue = value;
}

float clampFloat(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

uint32_t getMaxSamples() {
  float samples = (cfg.maxDays * 24.0f * 60.0f * 60.0f) / (float)cfg.intervalSec;
  if (samples < 1.0f) samples = 1.0f;
  return (uint32_t)samples;
}

float getLogProgressPercent() {
  uint32_t maxSamples = getMaxSamples();
  if (maxSamples == 0) return 0;
  return ((float)logCount / (float)maxSamples) * 100.0f;
}

float getRemainingDaysBySampleLimit() {
  uint32_t maxSamples = getMaxSamples();
  if (logCount >= maxSamples) return 0.0f;
  uint32_t remainingSamples = maxSamples - logCount;
  return (remainingSamples * (float)cfg.intervalSec) / 86400.0f;
}

size_t getFileSize(const char *fileName) {
  if (!LittleFS.exists(fileName)) return 0;
  File f = LittleFS.open(fileName, "r");
  if (!f) return 0;
  size_t s = f.size();
  f.close();
  return s;
}

// --------------------------------------------------
// ID / Pairing
// --------------------------------------------------
void makeNodeId(char out[11]) {
  uint8_t mac[6];
  WiFi.macAddress(mac);

  int yearCode = MFG_YEAR - 2020;
  if (yearCode < 0) yearCode = 0;
  if (yearCode > 35) yearCode %= 36;

  char dayCode[4];
  char hashCode[5];

  base36N((uint32_t)MFG_DAY_OF_YEAR, 3, dayCode);
  uint32_t h = fnv1a(mac, 6);
  base36N(h % 1679616UL, 4, hashCode); // 36^4

  out[0] = NODE_TYPE;
  out[1] = b36Digit(yearCode);
  out[2] = dayCode[0];
  out[3] = dayCode[1];
  out[4] = dayCode[2];
  out[5] = FW_FAMILY;
  out[6] = hashCode[0];
  out[7] = hashCode[1];
  out[8] = hashCode[2];
  out[9] = hashCode[3];
  out[10] = 0;
}

uint32_t pairingCodeFor(const char *id, const uint8_t mac[6]) {
  String s = String(PAIR_SECRET) + "|" + String(id) + "|" + macToString(mac);
  return hashString(s) % 1000000UL;
}

uint32_t pairingTokenFor(const char *id, const uint8_t nodeMac[6], const uint8_t gatewayMac[6], uint32_t code) {
  String s = String(PAIR_SECRET) + "|TOKEN|" + String(id) + "|" + macToString(nodeMac) + "|" + macToString(gatewayMac) + "|" + String(code);
  return hashString(s);
}

void makeLmkFor(const char *id, const uint8_t nodeMac[6], uint8_t lmk[16]) {
  String s = String(PAIR_SECRET) + "|LMK|" + String(id) + "|" + macToString(nodeMac);
  uint32_t h1 = hashString(s + "|1");
  uint32_t h2 = hashString(s + "|2");
  uint32_t h3 = hashString(s + "|3");
  uint32_t h4 = hashString(s + "|4");
  memcpy(lmk + 0, &h1, 4);
  memcpy(lmk + 4, &h2, 4);
  memcpy(lmk + 8, &h3, 4);
  memcpy(lmk + 12, &h4, 4);
}

// --------------------------------------------------
// Config speichern/laden
// --------------------------------------------------
void saveConfig() {
  prefs.putUChar("mode", cfg.mode);
  prefs.putUChar("power", cfg.powerMode);
  prefs.putBool("logging", cfg.loggingEnabled);
  prefs.putBool("live", cfg.liveOutput);
  prefs.putUInt("interval", cfg.intervalSec);
  prefs.putFloat("maxdays", cfg.maxDays);
  prefs.putFloat("toffset", cfg.tempOffsetC);
  prefs.putFloat("poffset", cfg.pressureOffsetHpa);
  prefs.putUChar("screen", cfg.screenMode);
  prefs.putUChar("graph", cfg.graphMode);
  prefs.putUChar("graphwin", cfg.graphWindow);
  prefs.putUChar("contrast", cfg.contrast);
  prefs.putUShort("dto", cfg.displayTimeoutSec);
  prefs.putUShort("wds", cfg.wakeDisplaySec);
  prefs.putBool("usbauto", cfg.usbAutoNormal);
  prefs.putUChar("usbwin", cfg.usbWindowSec);
  prefs.putBool("pairmode", cfg.pairMode);
  prefs.putBool("paired", cfg.paired);
  prefs.putBytes("gwmac", cfg.gatewayMac, 6);
  prefs.putString("nodeid", cfg.nodeId);
  prefs.putString("nodename", cfg.nodeName);
  prefs.putUChar("lang", cfg.language);
  prefs.putBool("weben", cfg.webEnabled);
  prefs.putString("wssid", cfg.wifiSsid);
  prefs.putString("wpass", cfg.wifiPass);
  prefs.putString("webuser", cfg.webUser);
  prefs.putString("webpass", cfg.webPass);
  prefs.putString("webhost", cfg.webHost);
  prefs.putBool("mqtten", cfg.mqttEnabled);
  prefs.putBool("mqtdisc", cfg.mqttDiscovery);
  prefs.putString("mqtthost", cfg.mqttHost);
  prefs.putUShort("mqttport", cfg.mqttPort);
  prefs.putString("mqttuser", cfg.mqttUser);
  prefs.putString("mqttpass", cfg.mqttPass);
  prefs.putString("mqttprefix", cfg.mqttPrefix);
}

void factoryDefaults() {
  cfg.mode = MODE_HYBRID;
  cfg.powerMode = POWER_NORMAL;
  cfg.loggingEnabled = true;
  cfg.liveOutput = true;
  cfg.intervalSec = 60;
  cfg.maxDays = 7.0f;
  cfg.tempOffsetC = 0.0f;
  cfg.pressureOffsetHpa = 0.0f;
  cfg.screenMode = 0;
  cfg.graphMode = 0;
  cfg.graphWindow = 0;
  cfg.contrast = 180;
  cfg.displayTimeoutSec = 0;
  cfg.wakeDisplaySec = 20;
  cfg.usbAutoNormal = true;
  cfg.usbWindowSec = 2;
  cfg.pairMode = false;
  cfg.paired = false;
  memset(cfg.gatewayMac, 0, 6);
  makeNodeId(cfg.nodeId);
  strncpy(cfg.nodeName, "Logger", sizeof(cfg.nodeName)-1);
  cfg.language = 0;
  cfg.webEnabled = true;
  memset(cfg.wifiSsid, 0, sizeof(cfg.wifiSsid));
  memset(cfg.wifiPass, 0, sizeof(cfg.wifiPass));
  strncpy(cfg.webUser, "admin", sizeof(cfg.webUser)-1);
  strncpy(cfg.webPass, "admin", sizeof(cfg.webPass)-1);
  strncpy(cfg.webHost, "bmp280-logger", sizeof(cfg.webHost)-1);
  cfg.mqttEnabled = false;
  cfg.mqttDiscovery = true;
  memset(cfg.mqttHost, 0, sizeof(cfg.mqttHost));
  cfg.mqttPort = 1883;
  memset(cfg.mqttUser, 0, sizeof(cfg.mqttUser));
  memset(cfg.mqttPass, 0, sizeof(cfg.mqttPass));
  strncpy(cfg.mqttPrefix, "bmp280", sizeof(cfg.mqttPrefix)-1);
  saveConfig();
}

void loadConfig() {
  prefs.begin("logger_v6", false);

  cfg.mode = prefs.getUChar("mode", MODE_HYBRID);
  cfg.powerMode = prefs.getUChar("power", POWER_NORMAL);
  cfg.loggingEnabled = prefs.getBool("logging", true);
  cfg.liveOutput = prefs.getBool("live", true);
  cfg.intervalSec = prefs.getUInt("interval", 60);
  cfg.maxDays = prefs.getFloat("maxdays", 7.0f);
  cfg.tempOffsetC = prefs.getFloat("toffset", 0.0f);
  cfg.pressureOffsetHpa = prefs.getFloat("poffset", 0.0f);
  cfg.screenMode = prefs.getUChar("screen", 0);
  cfg.graphMode = prefs.getUChar("graph", 0);
  cfg.graphWindow = prefs.getUChar("graphwin", 0);
  cfg.contrast = prefs.getUChar("contrast", 180);
  cfg.displayTimeoutSec = prefs.getUShort("dto", 0);
  cfg.wakeDisplaySec = prefs.getUShort("wds", 20);
  cfg.usbAutoNormal = prefs.getBool("usbauto", true);
  cfg.usbWindowSec = prefs.getUChar("usbwin", 2);
  cfg.pairMode = prefs.getBool("pairmode", false);
  cfg.paired = prefs.getBool("paired", false);

  memset(cfg.gatewayMac, 0, 6);
  prefs.getBytes("gwmac", cfg.gatewayMac, 6);

  String sid = prefs.getString("nodeid", "");
  if (sid.length() == 10) strncpy(cfg.nodeId, sid.c_str(), sizeof(cfg.nodeId)-1);
  else {
    makeNodeId(cfg.nodeId);
    prefs.putString("nodeid", cfg.nodeId);
  }

  String n = prefs.getString("nodename", "Logger");
  memset(cfg.nodeName, 0, sizeof(cfg.nodeName));
  strncpy(cfg.nodeName, n.c_str(), sizeof(cfg.nodeName)-1);

  cfg.language = prefs.getUChar("lang", 0);
  if (cfg.language > 1) cfg.language = 0;

  cfg.webEnabled = prefs.getBool("weben", true);
  String ws = prefs.getString("wssid", "");
  memset(cfg.wifiSsid, 0, sizeof(cfg.wifiSsid));
  strncpy(cfg.wifiSsid, ws.c_str(), sizeof(cfg.wifiSsid)-1);
  String wp = prefs.getString("wpass", "");
  memset(cfg.wifiPass, 0, sizeof(cfg.wifiPass));
  strncpy(cfg.wifiPass, wp.c_str(), sizeof(cfg.wifiPass)-1);
  String wu = prefs.getString("webuser", "admin");
  memset(cfg.webUser, 0, sizeof(cfg.webUser));
  strncpy(cfg.webUser, wu.length() ? wu.c_str() : "admin", sizeof(cfg.webUser)-1);
  String wpa = prefs.getString("webpass", "admin");
  memset(cfg.webPass, 0, sizeof(cfg.webPass));
  strncpy(cfg.webPass, wpa.length() ? wpa.c_str() : "admin", sizeof(cfg.webPass)-1);
  String wh = prefs.getString("webhost", "bmp280-logger");
  memset(cfg.webHost, 0, sizeof(cfg.webHost));
  strncpy(cfg.webHost, wh.length() ? wh.c_str() : "bmp280-logger", sizeof(cfg.webHost)-1);

  cfg.mqttEnabled = prefs.getBool("mqtten", false);
  cfg.mqttDiscovery = prefs.getBool("mqtdisc", true);
  String mh = prefs.getString("mqtthost", "");
  memset(cfg.mqttHost, 0, sizeof(cfg.mqttHost));
  strncpy(cfg.mqttHost, mh.c_str(), sizeof(cfg.mqttHost)-1);
  cfg.mqttPort = prefs.getUShort("mqttport", 1883);
  String mu = prefs.getString("mqttuser", "");
  memset(cfg.mqttUser, 0, sizeof(cfg.mqttUser));
  strncpy(cfg.mqttUser, mu.c_str(), sizeof(cfg.mqttUser)-1);
  String mp = prefs.getString("mqttpass", "");
  memset(cfg.mqttPass, 0, sizeof(cfg.mqttPass));
  strncpy(cfg.mqttPass, mp.c_str(), sizeof(cfg.mqttPass)-1);
  String mx = prefs.getString("mqttprefix", "bmp280");
  memset(cfg.mqttPrefix, 0, sizeof(cfg.mqttPrefix));
  strncpy(cfg.mqttPrefix, mx.length() ? mx.c_str() : "bmp280", sizeof(cfg.mqttPrefix)-1);

  if (cfg.mode > MODE_HYBRID) cfg.mode = MODE_HYBRID;
  if (cfg.powerMode > POWER_DEEP) cfg.powerMode = POWER_NORMAL;
  if (cfg.intervalSec < 1) cfg.intervalSec = 1;
  if (cfg.intervalSec > 3600) cfg.intervalSec = 3600;
  if (cfg.maxDays < 0.1f) cfg.maxDays = 0.1f;
  if (cfg.maxDays > 7.0f) cfg.maxDays = 7.0f;
  if (cfg.screenMode > 8) cfg.screenMode = 0;
  if (cfg.graphMode > 1) cfg.graphMode = 0;
  if (cfg.graphWindow > 3) cfg.graphWindow = 0;
  if (cfg.wakeDisplaySec < 5) cfg.wakeDisplaySec = 5;
  if (cfg.wakeDisplaySec > 300) cfg.wakeDisplaySec = 300;
  if (cfg.usbWindowSec > 30) cfg.usbWindowSec = 30;
  if (cfg.mqttPort == 0) cfg.mqttPort = 1883;
  if (strlen(cfg.mqttPrefix) == 0) strncpy(cfg.mqttPrefix, "bmp280", sizeof(cfg.mqttPrefix)-1);
}

String trustKey(int idx) {
  return "tr" + String(idx);
}

void saveTrusted(int idx) {
  prefs.putBytes(trustKey(idx).c_str(), &trusted[idx], sizeof(TrustedNode));
}

void loadTrusted() {
  memset(trusted, 0, sizeof(trusted));
  for (int i = 0; i < MAX_TRUSTED; i++) {
    size_t n = prefs.getBytes(trustKey(i).c_str(), &trusted[i], sizeof(TrustedNode));
    if (n != sizeof(TrustedNode)) memset(&trusted[i], 0, sizeof(TrustedNode));
    trusted[i].lastSeenMs=0; trusted[i].packets=0;
  }
}

int findTrustedById(const char *id) {
  for (int i = 0; i < MAX_TRUSTED; i++) {
    if (trusted[i].used && strncmp(trusted[i].id, id, 10) == 0) return i;
  }
  return -1;
}

int freeTrustedSlot() {
  for (int i = 0; i < MAX_TRUSTED; i++) if (!trusted[i].used) return i;
  return -1;
}

int findPendingById(const char *id) {
  for (int i = 0; i < MAX_PENDING; i++) {
    if (pendingNodes[i].used && strncmp(pendingNodes[i].id, id, 10) == 0) return i;
  }
  return -1;
}

int upsertPending(const PairReqPacket &p) {
  int idx = findPendingById(p.id);
  if (idx < 0) {
    for (int i = 0; i < MAX_PENDING; i++) {
      if (!pendingNodes[i].used) {
        idx = i;
        break;
      }
    }
  }
  if (idx < 0) return -1;

  pendingNodes[idx].used = true;
  strncpy(pendingNodes[idx].id, p.id, 10);
  pendingNodes[idx].id[10] = 0;
  strncpy(pendingNodes[idx].fw, p.fw, 7);
  pendingNodes[idx].fw[7] = 0;
  memcpy(pendingNodes[idx].mac, p.mac, 6);
  pendingNodes[idx].lastSeenMs = millis();

  return idx;
}

// --------------------------------------------------
// Buttons / Encoder
// --------------------------------------------------
void updateButton(DebouncedButton &btn) {
  btn.pressedEvent = false;
  btn.releasedEvent = false;

  bool raw = digitalRead(btn.pin);

  if (raw != btn.lastRawState) {
    btn.lastRawState = raw;
    btn.lastChangeMs = millis();
  }

  if ((millis() - btn.lastChangeMs) >= BUTTON_DEBOUNCE_MS) {
    if (raw != btn.stableState) {
      btn.stableState = raw;
      if (btn.stableState == LOW) btn.pressedEvent = true;
      else btn.releasedEvent = true;
    }
  }
}

uint8_t readEncoderState() {
  uint8_t a = digitalRead(PIN_ENC_A);
  uint8_t b = digitalRead(PIN_ENC_B);
  return (a << 1) | b;
}

void updateEncoder() {
  encoderDelta = 0;
  uint8_t currentState = readEncoderState();
  if (currentState == lastEncoderState) return;

  uint32_t nowUs = micros();
  if ((nowUs - lastEncoderChangeUs) < ENCODER_DEBOUNCE_US) return;
  lastEncoderChangeUs = nowUs;

  uint8_t index = (lastEncoderState << 2) | currentState;
  int8_t movement = encoderTable[index];
  lastEncoderState = currentState;

  if (movement == 0) return;

  encoderAccumulator += movement;

  if (encoderAccumulator >= 4) {
    encoderAccumulator = 0;
    encoderDelta = +1;
  } else if (encoderAccumulator <= -4) {
    encoderAccumulator = 0;
    encoderDelta = -1;
  }
}


uint64_t elapsedNow() { return elapsedBaseMs + (uint64_t)(esp_timer_get_time() / 1000); }
uint64_t epochNow() {
  struct timeval tv; gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1700000000) return 0;
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
}
String u64str(uint64_t v) { char b[24]; snprintf(b, sizeof(b), "%llu", (unsigned long long)v); return String(b); }
String fieldSafe(String v) { v.replace(",", " "); v.replace("\n", " "); v.replace("\r", " "); return v; }
String finiteJson(float v, uint8_t decimals=2) { return isfinite(v) ? String(v, (unsigned int)decimals) : String("null"); }
bool storageAvailable(size_t bytes) {
  if (!fsReady || storageFault) return false;
  size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  if (used > total || total - used < bytes + STORAGE_RESERVE) {
    storageFault = true; storageMessage = "SPEICHER VOLL";
    loggerConsole.println("ERR,storage,Speicher voll - Export erforderlich"); return false;
  }
  return true;
}
String packetLine(const DataPacket &p) {
  String r = "NODEDATA," + fieldSafe(p.id) + "," + fieldSafe(p.name) + "," + String(p.fw);
  r += "," + String(p.logNo) + "," + String(p.rawTempC,2) + "," + String(p.tempC,2);
  r += "," + String(p.rawPressureHpa,2) + "," + String(p.pressureHpa,2);
  r += "," + String(p.tempOffsetC,2) + "," + String(p.pressureOffsetHpa,2);
  r += "," + String(p.mode) + "," + String(p.powerMode) + "," + String(p.wakeCause) + "," + macToString(p.mac);
  r += "," + String(p.sequence) + "," + String(p.session) + "," + u64str(p.elapsedMs) + "," + u64str(p.epochMs);
  r += "," + String(p.flags) + "," + String(p.intervalSec);
  return r+","+String(hashString(r),HEX);
}
DataPacket localPacket() {
  DataPacket p = {}; memcpy(p.magic,"BPN2",4); p.proto=PROTO_VERSION; p.type=PT_DATA;
  strncpy(p.id,cfg.nodeId,10); strncpy(p.name,cfg.nodeName,15); strncpy(p.fw,FW_VERSION,7);
  WiFi.macAddress(p.mac); p.logNo=logCount;
  p.rawTempC=rawTempC; p.tempC=tempC; p.rawPressureHpa=rawPressureHpa; p.pressureHpa=pressureHpa;
  p.tempOffsetC=cfg.tempOffsetC; p.pressureOffsetHpa=cfg.pressureOffsetHpa;
  p.mode=cfg.mode; p.powerMode=cfg.powerMode; p.wakeCause=(uint8_t)wakeCause;
  p.flags=(cfg.loggingEnabled?1:0)|(sensorValid?2:0)|(sampleStored?4:0);
  p.sequence=sampleSequence; p.session=sessionId; p.elapsedMs=sampleElapsed; p.epochMs=sampleEpoch; p.intervalSec=cfg.intervalSec;
  return p;
}
bool appendPacket(const String &path, const DataPacket &p) {
  String line=packetLine(p)+"\n";
  if (!storageAvailable(line.length()+256)) return false;
  File f=LittleFS.open(path,"a");
  if (!f) { storageFault=true; storageMessage="DATEIFEHLER"; return false; }
  // A leading newline isolates an incomplete tail left by a previous power loss.
  if (f.size()==0) f.println("record,node_id,name,fw,log_no,raw_temp_c,temp_c,raw_pressure_hpa,pressure_hpa,temp_offset_c,pressure_offset_hpa,mode,power,wake,mac,sequence,session,elapsed_ms,epoch_ms,flags,interval_s,checksum_fnv1a");
  f.print("\n"); // isolate an incomplete record after an interrupted write
  size_t written=f.print(line); f.flush(); f.close();
  if (written!=line.length()) { storageFault=true; storageMessage="SCHREIBFEHLER"; loggerConsole.println("ERR,storage,Unvollstaendige Schreiboperation"); return false; }
  return true;
}
void processRadioQueue() {
  RxEnvelope e;
  for (uint8_t n=0; n<4 && rxQueue && xQueueReceive(rxQueue,&e,0)==pdTRUE; n++) handleIncoming(e.bytes,e.length,e.source);
}
void openSyncPath(String path) {
  if (syncFile) syncFile.close(); syncLine="";
  syncFile=LittleFS.open(path,"r"); syncRemaining=syncFile ? syncFile.size() : 0;
}
void startSync(String id) {
  if (!fsReady) { printErr("sync","Speicher nicht bereit"); return; }
  syncAll=(id=="all"); syncSlot=syncAll ? -1 : MAX_TRUSTED;
  if (id=="all" || id==cfg.nodeId || id=="local") openSyncPath(LOCAL_LOG_FILE);
  else { id.toUpperCase(); if (id.length()!=10) { printErr("sync","ID fehlt"); return; } openSyncPath("/v74_"+id+".csv"); }
  loggerConsole.println("SYNC,START");
}
void serviceTransfers() {
  // Pause file reads under backpressure; never consume archive rows we cannot queue.
  if (loggerConsole.freeBytes() < 4096) return;
  if (dumpStream) {
    String row=dumpStream.readStringUntil('\n'); if(row.length()) loggerConsole.println(row);
    if (!dumpStream.available()) { dumpStream.close(); loggerConsole.println("\nDUMP,END"); }
  }
  if (syncSlot == -2) return;
  // Bounded work per loop; source size frozen at open so live append cannot extend transfer forever.
  for (int n=0;n<192 && syncFile && syncRemaining && syncFile.available();n++) {
    char c=(char)syncFile.read(); syncRemaining--;
    if(c=='\n') { if(syncLine.startsWith("NODEDATA,")) loggerConsole.println(syncLine); syncLine=""; }
    else if(syncLine.length()<600) syncLine+=c;
  }
  if (!syncFile || !syncRemaining || !syncFile.available()) {
    if(syncFile) syncFile.close();
    if(syncAll) {
      while(++syncSlot<MAX_TRUSTED) if(trusted[syncSlot].used) { openSyncPath("/v74_"+String(trusted[syncSlot].id)+".csv"); return; }
    }
    syncSlot=-2; loggerConsole.println("SYNC,END");
  }
}

// --------------------------------------------------
// ESP-NOW
// --------------------------------------------------
void addBroadcastPeer() {
  if (esp_now_is_peer_exist(broadcastAddress)) return;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddress, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);
}

bool addEncryptedPeer(const uint8_t peerMac[6], const char *nodeIdForKey, const uint8_t nodeMacForKey[6]) {
  if (esp_now_is_peer_exist(peerMac)) return true;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, peerMac, 6);
  peer.channel = 0;
  peer.encrypt = true;

  uint8_t lmk[16];
  makeLmkFor(nodeIdForKey, nodeMacForKey, lmk);
  memcpy(peer.lmk, lmk, 16);

  return esp_now_add_peer(&peer) == ESP_OK;
}

void initEspNow() {
  if (cfg.webEnabled && cfg.powerMode != POWER_DEEP) WiFi.mode(WIFI_AP_STA);
  else WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  radioChannel=prefs.getUChar("radioch",1);
  if(radioChannel<1 || radioChannel>13) radioChannel=1;
  esp_wifi_set_channel(radioChannel,WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    loggerConsole.println("ESP-NOW Init Fehler");
    return;
  }

  uint8_t pmk[16];
  String s = String(PAIR_SECRET) + "|PMK";
  uint32_t h1 = hashString(s + "1");
  uint32_t h2 = hashString(s + "2");
  uint32_t h3 = hashString(s + "3");
  uint32_t h4 = hashString(s + "4");
  memcpy(pmk + 0, &h1, 4);
  memcpy(pmk + 4, &h2, 4);
  memcpy(pmk + 8, &h3, 4);
  memcpy(pmk + 12, &h4, 4);
  esp_now_set_pmk(pmk);

  addBroadcastPeer();
}

void sendPairRequest() {
  if (!isSensorRole()) return;

  PairReqPacket p;
  memset(&p, 0, sizeof(p));
  memcpy(p.magic, "BPN2", 4);
  p.proto = PROTO_VERSION;
  p.type = PT_PAIR_REQ;
  strncpy(p.id, cfg.nodeId, 10);
  strncpy(p.fw, FW_VERSION, sizeof(p.fw)-1);
  WiFi.macAddress(p.mac);
  p.nonce = millis();

  esp_now_send(broadcastAddress, (uint8_t*)&p, sizeof(p));

  loggerConsole.print("PAIRREQ gesendet ID=");
  loggerConsole.println(cfg.nodeId);
}

bool sendPairAccept(const PendingNode &pn, uint32_t code) {
  uint8_t gwMac[6];
  WiFi.macAddress(gwMac);

  PairAcceptPacket p;
  memset(&p, 0, sizeof(p));

  memcpy(p.magic, "BPN2", 4);
  p.proto = PROTO_VERSION;
  p.type = PT_PAIR_ACCEPT;
  strncpy(p.id, pn.id, 10);
  memcpy(p.nodeMac, pn.mac, 6);
  memcpy(p.gatewayMac, gwMac, 6);
  p.token = pairingTokenFor(pn.id, pn.mac, gwMac, code);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, pn.mac, 6);
  peer.channel = 0;
  peer.encrypt = false;
  if (!esp_now_is_peer_exist(pn.mac)) esp_now_add_peer(&peer);

  esp_err_t r = esp_now_send(pn.mac, (uint8_t*)&p, sizeof(p));
  loggerConsole.print("PAIRACCEPT an ");
  loggerConsole.print(pn.id);
  loggerConsole.print(" -> ");
  loggerConsole.println(r == ESP_OK ? "OK" : "FEHLER");
  return r == ESP_OK;
}

void printNodeDataSerialLocal();

void sendDataPacket() {
  if (!isSensorRole()) return;

  // Lokale Daten für PC/GUI immer ausgeben
  if (cfg.liveOutput || cfg.mode == MODE_HYBRID) {
    printNodeDataSerialLocal();
    mqttPublishLocalState(false);
  }

  // Stored records are transmitted by the acknowledged journal spool.
  // Live-only readings are best effort and are not queued as historical logs.
  if(cfg.paired && !sampleStored) {
    DataPacket p=localPacket(); esp_now_send(cfg.gatewayMac,(uint8_t*)&p,sizeof(p));
  }
}


void acknowledgePacket(const DataPacket &p) {
  AckPacket a={}; memcpy(a.magic,"BPN2",4); a.proto=PROTO_VERSION; a.type=5;
  strncpy(a.id,p.id,10); a.session=p.session; a.sequence=p.sequence;
  esp_now_send(p.mac,(uint8_t*)&a,sizeof(a));
}
bool parseJournal(const String &line, DataPacket &p) {
  int checksumAt=line.lastIndexOf(',');
  if(checksumAt<0 || hashString(line.substring(0,checksumAt))!=strtoul(line.substring(checksumAt+1).c_str(),nullptr,16)) return false;
  String f[21]; int start=0;
  for(int i=0;i<21;i++) { int end=line.indexOf(',',start); if(end<0) end=line.length(); f[i]=line.substring(start,end); start=end+1; }
  if(f[0]!="NODEDATA" || f[1].length()!=10 || f[20].length()==0) return false;
  memset(&p,0,sizeof(p)); memcpy(p.magic,"BPN2",4); p.proto=PROTO_VERSION; p.type=PT_DATA;
  strncpy(p.id,f[1].c_str(),10); strncpy(p.name,f[2].c_str(),15); strncpy(p.fw,f[3].c_str(),7);
  p.logNo=strtoul(f[4].c_str(),nullptr,10);
  p.rawTempC=f[5].toFloat(); p.tempC=f[6].toFloat(); p.rawPressureHpa=f[7].toFloat(); p.pressureHpa=f[8].toFloat();
  p.tempOffsetC=f[9].toFloat(); p.pressureOffsetHpa=f[10].toFloat(); p.mode=f[11].toInt(); p.powerMode=f[12].toInt(); p.wakeCause=f[13].toInt();
  WiFi.macAddress(p.mac);
  p.sequence=strtoul(f[15].c_str(),nullptr,10); p.session=strtoul(f[16].c_str(),nullptr,10);
  p.elapsedMs=strtoull(f[17].c_str(),nullptr,10); p.epochMs=strtoull(f[18].c_str(),nullptr,10); p.flags=f[19].toInt(); p.intervalSec=f[20].toInt();
  return true;
}
void serviceRadioSpool() {
  if(!cfg.paired || !isSensorRole() || !fsReady) return;
  if(!radioWaiting) {
    File f=LittleFS.open(LOCAL_LOG_FILE,"r"); if(!f) return;
    if(radioCursor>f.size()) radioCursor=0;
    f.seek(radioCursor);
    for(int n=0;n<2 && f.available();n++) {
      String line=f.readStringUntil('\n');
      if(parseJournal(line,radioPending)) { radioNextCursor=f.position(); radioWaiting=true; radioTries=0; break; }
      radioCursor=f.position();
    }
    f.close();
  }
  if(!radioWaiting) return;
  uint32_t wait=radioTries>=3?5000:250;
  if(radioTries && millis()-radioLastSend<wait) return;
  if(radioTries>=3) {
    radioTries=0;
    // Sensor-only nodes can seek the gateway channel; AP-connected nodes must stay on router channel.
    if(cfg.mode==MODE_SENSOR && WiFi.status()!=WL_CONNECTED) { radioChannel=radioChannel%13+1; esp_wifi_set_channel(radioChannel,WIFI_SECOND_CHAN_NONE); }
  }
  esp_now_send(cfg.gatewayMac,(uint8_t*)&radioPending,sizeof(radioPending));
  radioLastSend=millis(); radioTries++;
}

void saveGatewayCsv(const DataPacket &p) {
  if (!(p.flags & 2)) return;
  appendPacket("/v74_" + String(p.id) + ".csv", p);
}

void printNodeDataSerialPacket(const DataPacket &p) { loggerConsole.println(packetLine(p)); }

void printNodeDataSerialLocal() { loggerConsole.println(packetLine(localPacket())); }

void handlePairReq(const PairReqPacket &p) {
  if (!isGatewayRole()) return;
  if (!cfg.pairMode) {
    loggerConsole.print("PAIRREQ ignoriert, Pairing aus: ");
    loggerConsole.println(p.id);
    return;
  }

  int idx = upsertPending(p);
  if (idx < 0) {
    loggerConsole.println("PAIRREQ: Pending voll");
    return;
  }

  loggerConsole.print("PAIRREQ,");
  loggerConsole.print(p.id); loggerConsole.print(",");
  loggerConsole.print(p.fw); loggerConsole.print(",");
  loggerConsole.println(macToString(p.mac));
}

void handlePairAccept(const PairAcceptPacket &p) {
  if (!isSensorRole()) return;
  if (strncmp(p.id, cfg.nodeId, 10) != 0) return;

  uint8_t ownMac[6];
  WiFi.macAddress(ownMac);
  if (!macEqual(p.nodeMac, ownMac)) return;

  uint32_t code = pairingCodeFor(cfg.nodeId, ownMac);
  uint32_t expected = pairingTokenFor(cfg.nodeId, ownMac, p.gatewayMac, code);

  if (p.token != expected) {
    loggerConsole.println("PAIRACCEPT falsch, Token passt nicht");
    return;
  }

  memcpy(cfg.gatewayMac, p.gatewayMac, 6);
  if (esp_now_is_peer_exist(p.gatewayMac)) esp_now_del_peer(p.gatewayMac);
  cfg.paired = true;
  cfg.pairMode = false;
  saveConfig();
  prefs.putUChar("radioch",WiFi.channel());
  addEncryptedPeer(cfg.gatewayMac,cfg.nodeId,ownMac);

  loggerConsole.print("PAIR OK Gateway=");
  loggerConsole.println(macToString(cfg.gatewayMac));
}

void handleData(const DataPacket &p) {
  if (!isGatewayRole()) return;

  if(p.intervalSec<1 || p.intervalSec>3600 || !isfinite(p.tempC) || !isfinite(p.pressureHpa)) return;
  int idx = findTrustedById(p.id);
  if (idx < 0 || !macEqual(trusted[idx].mac, p.mac)) {
    loggerConsole.print("IGNORED,UNTRUSTED,");
    loggerConsole.print(p.id);
    loggerConsole.print(",");
    loggerConsole.println(macToString(p.mac));
    return;
  }

  if((p.flags&4) && lastRxSession[idx]==p.session && p.sequence<=lastRxSequence[idx]) { acknowledgePacket(p); return; }
  if ((p.flags&4) && !appendPacket("/v74_"+String(p.id)+".csv",p)) return;
  acknowledgePacket(p);
  if(p.flags&4) { lastRxSession[idx]=p.session; lastRxSequence[idx]=p.sequence; }
  trusted[idx].lastLog = p.logNo;
  trusted[idx].packets++;
  trusted[idx].lastSeenMs = millis();
  if (p.name[0]) {
    memset(trusted[idx].name, 0, sizeof(trusted[idx].name));
    strncpy(trusted[idx].name, p.name, sizeof(trusted[idx].name)-1);
  }
  lastGoodRxMs = millis();
  nodeTimeoutMs[idx] = max((uint32_t)30000, min((uint32_t)14400000, (uint32_t)(p.intervalSec * 3000UL + 15000UL)));
  nodeOnline[idx] = true;
  printNodeDataSerialPacket(p);
  mqttPublishPacketState(p);
}

void handleIncoming(const uint8_t *data, int len, const uint8_t *source) {
  if (len < 6) return;
  if (data[0] != 'B' || data[1] != 'P' || data[2] != 'N' || data[3] != '2') return;
  if (data[4] != PROTO_VERSION) return;

  uint8_t type = data[5];
  if(type==5 && len==sizeof(AckPacket) && cfg.paired && macEqual(source,cfg.gatewayMac)) {
    AckPacket a; memcpy(&a,data,sizeof(a));
    if(radioWaiting && strncmp(a.id,cfg.nodeId,10)==0 && a.session==radioPending.session && a.sequence==radioPending.sequence) {
      radioCursor=radioNextCursor; radioWaiting=false; radioTries=0; radioConfirmed++; lastGoodRxMs=millis();
      uint8_t ch=WiFi.channel(); if(prefs.getUChar("radioch",1)!=ch) prefs.putUChar("radioch",ch);
    }
    return;
  }

  if (type == PT_PAIR_REQ && len == sizeof(PairReqPacket)) {
    PairReqPacket p;
    memcpy(&p, data, sizeof(p));
    p.id[10] = 0;
    p.fw[7] = 0;
    if (!macEqual(p.mac, source)) return;
    handlePairReq(p);
  } else if (type == PT_PAIR_ACCEPT && len == sizeof(PairAcceptPacket)) {
    PairAcceptPacket p;
    memcpy(&p, data, sizeof(p));
    p.id[10] = 0;
    if (!macEqual(p.gatewayMac, source)) return;
    handlePairAccept(p);
  } else if (type == PT_DATA && len == sizeof(DataPacket)) {
    DataPacket p;
    memcpy(&p, data, sizeof(p));
    p.id[10] = 0;
    p.name[15] = 0;
    p.fw[7] = 0;
    if (!macEqual(p.mac, source)) return;
    handleData(p);
  }
}

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
void onRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (!rxQueue || len < 6 || len > 250) return;
  RxEnvelope e = {};
  e.length = len;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  memcpy(e.source, info->src_addr, 6);
#else
  memcpy(e.source, mac, 6);
#endif
  memcpy(e.bytes, data, len);
  if (xQueueSend(rxQueue, &e, 0) != pdTRUE) rxDropped++;

}
#else
void onRecv(const uint8_t *mac, const uint8_t *data, int len) {
  if (!rxQueue || len < 6 || len > 250) return;
  RxEnvelope e = {};
  e.length = len;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  memcpy(e.source, info->src_addr, 6);
#else
  memcpy(e.source, mac, 6);
#endif
  memcpy(e.bytes, data, len);
  if (xQueueSend(rxQueue, &e, 0) != pdTRUE) rxDropped++;

}
#endif

// --------------------------------------------------
// Sensor / Logging
// --------------------------------------------------
void addHistory(float t, float p) {
  hourHistory.add(elapsedNow(), t, p);
  tempHistory[historyIndex] = t;
  pressHistory[historyIndex] = p;
  historyIndex++;
  if (historyIndex >= HISTORY_SIZE) historyIndex = 0;
  if (historyCount < HISTORY_SIZE) historyCount++;
}

float getHistoryValue(uint8_t pos, uint8_t mode) {
  if (pos >= historyCount) return NAN;
  int idx = (int)historyIndex - (int)historyCount + pos;
  while (idx < 0) idx += HISTORY_SIZE;
  idx = idx % HISTORY_SIZE;
  return mode == 0 ? tempHistory[idx] : pressHistory[idx];
}

bool initBMP280() {
  if (!bmp.begin(BMP280_ADDR)) {
    loggerConsole.println("BMP280 nicht gefunden.");
    return false;
  }

  bmp.setSampling(
    Adafruit_BMP280::MODE_NORMAL,
    Adafruit_BMP280::SAMPLING_X2,
    Adafruit_BMP280::SAMPLING_X16,
    Adafruit_BMP280::FILTER_X16,
    Adafruit_BMP280::STANDBY_MS_500
  );
  return true;
}

void readSensor() {
  sampleStored=false;
  sampleSequence++; sampleAtMs=millis(); sampleElapsed=elapsedNow(); sampleEpoch=epochNow();
  if(!bmpOk) { sensorValid=false; rawTempC=tempC=rawPressureHpa=pressureHpa=NAN; hourHistory.add(elapsedNow(), NAN, NAN); return; }
  rawTempC=bmp.readTemperature(); rawPressureHpa=bmp.readPressure()/100.0F;
  sensorValid=isfinite(rawTempC) && isfinite(rawPressureHpa) && rawTempC>=-40 && rawTempC<=85 && rawPressureHpa>=300 && rawPressureHpa<=1100;
  if(!sensorValid) { bmpOk=false; rawTempC=tempC=rawPressureHpa=pressureHpa=NAN; hourHistory.add(elapsedNow(), NAN, NAN); return; }
  tempC=rawTempC+cfg.tempOffsetC; pressureHpa=rawPressureHpa+cfg.pressureOffsetHpa; readCount++;
  updateMinMax(tempC,minTempC,maxTempC); updateMinMax(pressureHpa,minPressureHpa,maxPressureHpa);
  addHistory(tempC,pressureHpa);
}

void ensureLocalLogHeader() {
  // appendPacket creates the V74 CSV header on first successful write.
}

uint32_t countLocalLogRows() {
  if (!fsReady) return 0;
  File f=LittleFS.open(LOCAL_LOG_FILE,"r"); uint32_t count=0;
  while(f && f.available()) { String line=f.readStringUntil('\n'); DataPacket record; if(parseJournal(line,record)) count++; }
  if(f) f.close(); return count;
}

void logLocal(bool force) {
  sampleStored=false;
  if ((!force && !cfg.loggingEnabled) || !isSensorRole() || !sensorValid) return;
  if (logCount>=getMaxSamples()) { cfg.loggingEnabled=false; saveConfig(); printErr("log","Messpunktlimit erreicht"); return; }
  DataPacket p=localPacket(); p.logNo=logCount+1; p.flags |= 4;
  if(appendPacket(LOCAL_LOG_FILE,p)) { logCount++; sampleStored=true; cachedLogRows=logCount; cachedLogBytes=getFileSize(LOCAL_LOG_FILE); }
}

void clearLocalLog() {
  if(syncFile) syncFile.close(); syncSlot=-2;
  if(dumpStream) dumpStream.close();
  if (LittleFS.exists(LOCAL_LOG_FILE)) LittleFS.remove(LOCAL_LOG_FILE);
  logCount = 0;
  radioCursor=0; radioWaiting=false; cachedLogRows=0; cachedLogBytes=0;
  storageFault = !fsReady; storageMessage = fsReady ? "OK" : "FS FEHLER";
  ensureLocalLogHeader();
  cfg.loggingEnabled = true;
  saveConfig();
  setEvent(cfg.language == 1 ? "Local Log cleared" : "Local Log geloescht");
  printAck("clear", cfg.language == 1 ? "Local Log cleared" : "Local Log geloescht");
  printStatusData();
}

void dumpFile(String fn) {
  if(dumpStream) dumpStream.close();
  dumpStream=LittleFS.open(fn,"r");
  if(!dumpStream) printErr("dump","Datei fehlt"); else loggerConsole.println("DUMP,START");
}

// --------------------------------------------------
// Power / DeepSleep
// --------------------------------------------------
uint64_t buttonWakeMask() {
  return (1ULL << PIN_ENC_PUSH) |
         (1ULL << PIN_BAK) |
         (1ULL << PIN_CONTR);
}

void prepareWakePinsForDeepSleep() {
  const gpio_num_t pins[] = {
    (gpio_num_t)PIN_ENC_A,
    (gpio_num_t)PIN_ENC_B,
    (gpio_num_t)PIN_ENC_PUSH,
    (gpio_num_t)PIN_BAK,
    (gpio_num_t)PIN_CONTR
  };

  for (uint8_t i = 0; i < 5; i++) {
    gpio_reset_pin(pins[i]);
    gpio_set_direction(pins[i], GPIO_MODE_INPUT);
    gpio_pullup_en(pins[i]);
    gpio_pulldown_dis(pins[i]);
  }
}

bool serialConnectedQuick() {
  if (Serial) return true;
  if (Serial.available() > 0) return true;
  return false;
}

bool waitForUsbNormalWindow(uint8_t seconds) {
  if (!cfg.usbAutoNormal) return false;
  if (seconds == 0) return serialConnectedQuick();

  unsigned long start = millis();
  while ((millis() - start) < (seconds * 1000UL)) {
    if (serialConnectedQuick()) return true;
    if (Serial.available() > 0) return true;
    delay(50);
  }
  return false;
}

void enterDeepSleepNow(const char *reason) {
  loggerConsole.print("DEEP SLEEP: ");
  loggerConsole.println(reason);
  // Never wait for a USB reader before sleeping. Diagnostics are best effort.
  loggerConsole.pump(Serial);

  if (oledOk) oled.setPowerSave(1);
  displayOn = false;

  mqttDisconnect();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  if(bmpOk) bmp.setSampling(Adafruit_BMP280::MODE_SLEEP);
  prepareWakePinsForDeepSleep();

  // Start-to-start interval: subtract awake work (including USB window).
  uint64_t activeUs=(uint64_t)esp_timer_get_time();
  uint64_t targetUs=(uint64_t)cfg.intervalSec*1000000ULL;
  uint64_t timerUs=targetUs>activeUs ? targetUs-activeUs : 1000000ULL;
  if (timerUs < 1000000ULL) timerUs = 1000000ULL;

  elapsedBaseMs=elapsedNow()+timerUs/1000;
  esp_sleep_enable_timer_wakeup(timerUs);
  uint64_t mask=0;
  for(uint8_t pin=0;pin<6;pin++) if((buttonWakeMask() & (1ULL<<pin)) && digitalRead(pin)==HIGH) mask|=(1ULL<<pin);
  if(mask) esp_deep_sleep_enable_gpio_wakeup(mask, ESP_GPIO_WAKEUP_GPIO_LOW);

  esp_deep_sleep_start();
}

void applyPowerMode() {

  if (cfg.powerMode == POWER_ECO) {
    cfg.liveOutput = false;
    if (cfg.displayTimeoutSec == 0) cfg.displayTimeoutSec = 20;
    setCpuFrequencyMhz(80);
    setEvent("Power ECO");
  } else if (cfg.powerMode == POWER_DEEP) {
    cfg.liveOutput = false;
    if (cfg.wakeDisplaySec < 5) cfg.wakeDisplaySec = 20;
    setCpuFrequencyMhz(80);
    setEvent("Power DEEP");
  } else {
    setCpuFrequencyMhz(160);

    // V6.7 FIX:
    // Normalmodus bedeutet bewusst: Display bleibt an.
    // Der alte ECO-Display-Timeout darf hier nicht weiter wirken.
    cfg.displayTimeoutSec = 0;
    displayOn = true;
    if (oledOk) oled.setPowerSave(0);
    lastUserActionMs = millis();

    setEvent("Power NORMAL - Display dauerhaft AN");
  }
  saveConfig();
  setupWebIfAllowed();
}

void wakeDisplay(const String &reason) {
  lastUserActionMs = millis();
  if (!displayOn && oledOk) {
    oled.setPowerSave(0);
    displayOn = true;
    setEvent("Display an: " + reason);
  }
}

void maybeSleepDisplay() {
  if (!displayOn || !oledOk) return;
  if (cfg.powerMode == POWER_DEEP) return;
  if (cfg.displayTimeoutSec == 0) return;

  if ((millis() - lastUserActionMs) > (cfg.displayTimeoutSec * 1000UL)) {
    oled.setPowerSave(1);
    displayOn = false;
    loggerConsole.println("Display aus wegen Timeout.");
  }
}

void maybeReturnToDeepAfterButtonWake() {
  if (cfg.powerMode != POWER_DEEP) return;
  if (wakeCause != ESP_SLEEP_WAKEUP_GPIO && wakeCause != ESP_SLEEP_WAKEUP_UNDEFINED) return;

  if ((millis() - lastUserActionMs) > (cfg.wakeDisplaySec * 1000UL)) {
    enterDeepSleepNow("Button-Wake Timeout");
  }
}

void deepTimerWakeJobAndSleep() {
  if (cfg.powerMode != POWER_DEEP) return;
  if (wakeCause != ESP_SLEEP_WAKEUP_TIMER) return;
  if (!isSensorRole()) return;

  readSensor();
  logLocal(false);
  sendDataPacket();
  unsigned long deadline=millis();
  while(cfg.paired && millis()-deadline<1200) { processRadioQueue(); serviceRadioSpool(); delay(5); }

  if (cfg.usbAutoNormal && waitForUsbNormalWindow(cfg.usbWindowSec)) {
    cfg.powerMode = POWER_NORMAL;
    cfg.liveOutput = true;
    saveConfig();
    applyPowerMode();
    if (oledOk) oled.setPowerSave(0);
    displayOn = true;
    loggerConsole.println("USB erkannt -> Normalmodus");
    return;
  }

  if(cfg.paired && radioWaiting && WiFi.status()!=WL_CONNECTED) {
    missedRadioWakes++;
    if(missedRadioWakes>=2) { radioChannel=radioChannel%13+1; prefs.putUChar("radioch",radioChannel); missedRadioWakes=0; }
  } else missedRadioWakes=0;
  enterDeepSleepNow("Timer-Wake erledigt");
}


// --------------------------------------------------
// Display - OLED Pro UI V7.1.2 Clean Dash Pixel-Safe Layout
// OLED: 128x64 Pixel
// Bereiche:
//   0..9   Header/Status
//   11..53 Content
//   54..63 Footer
// Keine Seite darf außerhalb dieser Bereiche zeichnen.
// --------------------------------------------------
const char* txtDEEN(const char *de, const char *en) {
  return cfg.language == 1 ? en : de;
}

const char* modeShort() {
  if (cfg.mode == MODE_GATEWAY) return "GW";
  if (cfg.mode == MODE_HYBRID) return "HYB";
  return "SEN";
}

const char* powerShort() {
  if (cfg.powerMode == POWER_DEEP) return "DP";
  if (cfg.powerMode == POWER_ECO) return "ECO";
  return "NOR";
}

const char* logShort() {
  return cfg.loggingEnabled ? "LOG" : "STOP";
}

String fitTextToWidth(String s, uint8_t maxW) {
  if (maxW < 6) return "";
  if (oled.getStrWidth(s.c_str()) <= maxW) return s;
  while (s.length() > 0 && oled.getStrWidth((s + ".").c_str()) > maxW) {
    s.remove(s.length() - 1);
  }
  if (s.length() == 0) return "";
  return s + ".";
}

void drawFitStr(int x, int y, uint8_t maxW, const String &s) {
  String t = fitTextToWidth(s, maxW);
  oled.drawStr(x, y, t.c_str());
}

void drawTinyText(uint8_t x, uint8_t y, const char *s) {
  oled.setFont(u8g2_font_5x8_tf);
  drawFitStr(x, y, 126 - x, String(s));
}

void drawRightText(uint8_t y, const char *s) {
  oled.setFont(u8g2_font_5x8_tf);
  String t = fitTextToWidth(String(s), 62);
  int w = oled.getStrWidth(t.c_str());
  int x = 127 - w;
  if (x < 0) x = 0;
  oled.drawStr(x, y, t.c_str());
}

void drawHeader(const char *title) {
  if (!oledOk) return;
  oled.setDrawColor(1);
  oled.drawBox(0, 0, 128, 10);
  oled.setDrawColor(0);
  oled.setFont(u8g2_font_5x8_tf);
  drawFitStr(2, 8, 66, String(title));

  char stat[24];
  snprintf(stat, sizeof(stat), "%s %s", powerShort(), logShort());
  String st = fitTextToWidth(String(stat), 42);
  int w = oled.getStrWidth(st.c_str());
  int x = 126 - w;
  if (x < 74) x = 74;
  oled.drawStr(x, 8, st.c_str());

  oled.setDrawColor(1);
  if (cfg.loggingEnabled && ((millis() / 500) % 2 == 0)) {
    oled.drawBox(70, 3, 3, 3);
  }
}

void drawFooter(const char *left, const char *right) {
  oled.setDrawColor(1);
  oled.drawHLine(0, 54, 128);
  oled.setFont(u8g2_font_5x8_tf);
  if (left) drawFitStr(2, 63, 62, String(left));
  if (right) {
    String r = fitTextToWidth(String(right), 62);
    int w = oled.getStrWidth(r.c_str());
    oled.drawStr(126 - w, 63, r.c_str());
  }
}

void drawSoftFrame(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
  if (y + h > 54) h = 54 - y;
  if (x + w > 128) w = 128 - x;
  oled.drawFrame(x, y, w, h);
}

void drawFilledBar(uint8_t x, uint8_t y, uint8_t w, uint8_t h, float ratio) {
  if (y + h > 54) h = 54 - y;
  ratio = clampFloat(ratio, 0.0f, 1.0f);
  oled.drawFrame(x, y, w, h);
  uint8_t fillW = (uint8_t)((w - 2) * ratio);
  if (fillW > 0 && h > 2) oled.drawBox(x + 1, y + 1, fillW, h - 2);
}

void draw3DFrame(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
  drawSoftFrame(x, y, w, h);
}

void drawBar(uint8_t x, uint8_t y, uint8_t w, uint8_t h, float ratio) {
  drawFilledBar(x, y, w, h, ratio);
}

void drawValueBox(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const char *label, const String &value) {
  drawSoftFrame(x, y, w, h);
  oled.setFont(u8g2_font_5x8_tf);
  drawFitStr(x + 3, y + 8, w - 6, String(label));
  oled.setFont(u8g2_font_6x10_tf);
  drawFitStr(x + 3, y + h - 4, w - 6, value);
}

void drawMetricCard(uint8_t x, uint8_t y, uint8_t w, uint8_t h, const char *label, const String &value, const char *unit) {
  drawSoftFrame(x, y, w, h);
  oled.setFont(u8g2_font_5x8_tf);
  drawFitStr(x + 3, y + 8, w - 6, String(label));
  oled.setFont(u8g2_font_7x14B_tf);
  String v = value;
  if (unit && strlen(unit) > 0) v += String(unit);
  drawFitStr(x + 3, y + h - 4, w - 6, v);
}

void drawHistoryMini(uint8_t x0, uint8_t y0, uint8_t w, uint8_t h, uint8_t mode, bool grid) {
  if (y0 + h > 53) h = 53 - y0;
  oled.drawFrame(x0, y0, w, h);
  if (grid && w > 20 && h > 10) {
    oled.drawHLine(x0 + 1, y0 + h / 2, w - 2);
    oled.drawVLine(x0 + w / 2, y0 + 1, h - 2);
  }
  if (historyCount < 2) {
    oled.setFont(u8g2_font_5x8_tf);
    drawFitStr(x0 + 5, y0 + h / 2 + 3, w - 10, String(txtDEEN("warte Daten", "wait data")));
    return;
  }

  float vMin = NAN, vMax = NAN;
  for (uint8_t i = 0; i < historyCount; i++) {
    float v = getHistoryValue(i, mode);
    if (isnan(v)) continue;
    if (isnan(vMin) || v < vMin) vMin = v;
    if (isnan(vMax) || v > vMax) vMax = v;
  }
  if (isnan(vMin) || isnan(vMax)) return;
  if (fabs(vMax - vMin) < 0.2f) {
    vMax += 0.1f;
    vMin -= 0.1f;
  }

  int lastX = -1, lastY = -1;
  for (uint8_t i = 0; i < historyCount; i++) {
    float v = getHistoryValue(i, mode);
    if (isnan(v)) continue;
    float xNorm = historyCount > 1 ? (float)i / (float)(historyCount - 1) : 0.0f;
    float yNorm = (v - vMin) / (vMax - vMin);
    yNorm = clampFloat(yNorm, 0.0f, 1.0f);
    int x = x0 + 2 + (int)(xNorm * (w - 5));
    int y = y0 + h - 3 - (int)(yNorm * (h - 6));
    if (lastX >= 0) oled.drawLine(lastX, lastY, x, y);
    lastX = x;
    lastY = y;
  }

  // Min/Max innen in die Grafik setzen, nicht darunter.
  oled.setFont(u8g2_font_5x8_tf);
  char minBuf[12];
  char maxBuf[12];
  if (mode == 0) {
    snprintf(minBuf, sizeof(minBuf), "%.1f", vMin);
    snprintf(maxBuf, sizeof(maxBuf), "%.1f", vMax);
  } else {
    snprintf(minBuf, sizeof(minBuf), "%.0f", vMin);
    snprintf(maxBuf, sizeof(maxBuf), "%.0f", vMax);
  }
  oled.drawStr(x0 + 3, y0 + h - 3, minBuf);
  int tx = x0 + w - oled.getStrWidth(maxBuf) - 3;
  if (tx > x0 + 3) oled.drawStr(tx, y0 + 8, maxBuf);
}

String trendText(uint8_t mode) {
  if (historyCount < 10) return txtDEEN("warte", "wait");
  float firstV = getHistoryValue(0, mode);
  float lastV = getHistoryValue(historyCount - 1, mode);
  if (isnan(firstV) || isnan(lastV)) return "--";
  float diff = lastV - firstV;
  float th = mode == 0 ? 0.2f : 0.7f;
  if (diff > th) return txtDEEN("hoch", "rising");
  if (diff < -th) return txtDEEN("runter", "falling");
  return txtDEEN("stabil", "stable");
}

String shortNodeName() {
  String n = String(cfg.nodeName);
  if (n.length() == 0) n = String(cfg.nodeId);
  return n;
}

















const char* menuGroupName(int idx) {
  if (idx <= 2) return txtDEEN("Navigation", "Navigation");
  if (idx <= 4) return txtDEEN("Logging", "Logging");
  if (idx <= 6) return txtDEEN("Kalib.", "Calib.");
  if (idx <= 12) return txtDEEN("Power", "Power");
  if (idx <= 14) return txtDEEN("Ansicht", "View");
  if (idx <= 18) return txtDEEN("Netz", "Network");
  return txtDEEN("System", "System");
}

// V74 display: 128x64; header 0..9, content 13..51, footer 55..63.
// Exact renderer shared with host preview tests.
void sendOledIfChanged() {
  static uint8_t previous[1024]; static bool initialized=false;
  uint8_t *buffer=oled.getBufferPtr();
  if(!initialized) { oled.sendBuffer(); memcpy(previous,buffer,1024); initialized=true; return; }
  for(uint8_t row=0;row<8;row++) {
    int first=16,last=-1;
    for(uint8_t col=0;col<16;col++) if(memcmp(previous+row*128+col*8,buffer+row*128+col*8,8)) { if(first==16) first=col; last=col; }
    if(last>=first) oled.updateDisplayArea(first,row,last-first+1,1);
  }
  memcpy(previous,buffer,1024);
}
String shown(float value, uint8_t places=1) { return isfinite(value)?String(value,(unsigned int)places):String("--"); }
uint32_t nextLogSeconds() {
  if(!cfg.loggingEnabled || storageFault) return 0;
  uint32_t elapsed=(millis()-lastLogMs)/1000;
  return elapsed>=cfg.intervalSec?0:cfg.intervalSec-elapsed;
}
void dashBase(const char *title, uint8_t page) {
  oled.clearBuffer(); oled.setDrawColor(1);
  oled.drawBox(0,0,128,10); oled.setDrawColor(0); oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(2,8,title);
  String state=storageFault?"ERR":(!sensorValid && isSensorRole()?"SENSOR!":(cfg.loggingEnabled?"REC":"STOP"));
  oled.drawStr(126-oled.getStrWidth(state.c_str()),8,state.c_str()); oled.setDrawColor(1);
  oled.drawHLine(0,54,128); oled.setFont(u8g2_font_5x8_tf);
  String left=String(page)+"/8 "+String(modeShort()); oled.drawStr(2,63,left.c_str());
  String right=String(cfg.intervalSec)+"s "+powerShort(); oled.drawStr(126-oled.getStrWidth(right.c_str()),63,right.c_str());
}
void dashText(int x,int y,int width,const String &text) {
  oled.setFont(u8g2_font_6x10_tf);
  String fitted=fitTextToWidth(text,width); oled.drawStr(x,y,fitted.c_str());
}
void dashBig(const String &value,const char *unit,int baseline=39) {
  oled.setFont(u8g2_font_logisoso24_tn);
  if(oled.getStrWidth(value.c_str())>96) oled.setFont(u8g2_font_6x10_tf);
  int width=oled.getStrWidth(value.c_str()); int x=max(2,(104-width)/2);
  oled.drawStr(x,baseline,value.c_str());
  oled.setFont(u8g2_font_6x10_tf); oled.drawStr(106,baseline,unit);
  if(strcmp(unit,"C")==0) oled.drawCircle(103,baseline-8,1);
}
void drawMainScreen() {
  dashBase("DASHBOARD",1);
  dashBig(shown(tempC),"C");
  dashText(2,51,73,shown(pressureHpa,0)+" hPa");
  String countdown=storageFault?"VOLL":(cfg.loggingEnabled?String(nextLogSeconds())+"s":"PAUSE");
  oled.setFont(u8g2_font_6x10_tf); oled.drawStr(126-oled.getStrWidth(countdown.c_str()),51,countdown.c_str());
  sendOledIfChanged();
}
void drawGaugeScreen() {
  dashBase(txtDEEN("TEMPERATUR","TEMPERATURE"),2);
  dashText(2,23,124,"MIN "+shown(minTempC)+" C");
  dashText(2,36,124,"MAX "+shown(maxTempC)+" C");
  String stable="Trend: "+trendText(0);
  if(historyCount>=60) {
    float lo=10000,hi=-10000;
    for(int i=historyCount-60;i<historyCount;i++) {float v=getHistoryValue(i,0); if(isfinite(v)){lo=min(lo,v); hi=max(hi,v);}}
    if(hi>=lo && hi-lo<=0.2f) stable=txtDEEN("Ruhig: 60 Werte","Quiet: 60 samples");
  }
  dashText(2,50,124,stable);
  sendOledIfChanged();
}
void drawGraphScreen() {
  const uint8_t hours = graphHours[cfg.graphWindow];
  const uint64_t now = elapsedNow(), span = (uint64_t)hours * 3600000ULL;
  const uint16_t minutes = hours * 60;
  String title = String(cfg.graphMode == 0 ? "TEMP C " : "DRUCK hPa ") + String(hours) + "h";
  if(cfg.language == 1 && cfg.graphMode == 1) title = String("PRESS hPa ") + String(hours) + "h";
  oled.clearBuffer(); oled.setDrawColor(1);
  oled.drawBox(0,0,128,10); oled.setDrawColor(0); oled.setFont(u8g2_font_5x8_tf);
  oled.drawStr(2,8,title.c_str()); oled.setDrawColor(1);
  // Aggregate onto physical pixel columns, keeping the sampled min/max envelope.
  float low[93], high[93];
  for(int x=0;x<93;x++){low[x]=INFINITY;high[x]=-INFINITY;}
  float lo=INFINITY,hi=-INFINITY;
  for(uint16_t age=0;age<=minutes;age++) {
    const HourPoint *point=hourHistory.atAge(now,age);
    if(!point || now-point->timeMs>span) continue;
    float value=cfg.graphMode==0?point->temperature:point->pressure;
    if(!isfinite(value)) continue;
    int x=92-(int)(((now-point->timeMs)*92ULL)/span);
    low[x]=min(low[x],value); high[x]=max(high[x],value);
    lo=min(lo,value); hi=max(hi,value);
  }
  if(isfinite(lo) && isfinite(hi)) {
    if(hi-lo<0.4f){float middle=(hi+lo)/2;lo=middle-0.2f;hi=middle+0.2f;}
    oled.drawStr(0,21,shown(hi,1).c_str()); oled.drawStr(0,43,shown(lo,1).c_str());
    int previousX=-1,previousY=0;
    for(int x=0;x<93;x++) {
      if(!isfinite(low[x])) {previousX=-1;continue;}
      int top=42-(int)((high[x]-lo)/(hi-lo)*26);
      int bottom=42-(int)((low[x]-lo)/(hi-lo)*26);
      top=constrain(top,16,42); bottom=constrain(bottom,16,42);
      oled.drawVLine(34+x,top,bottom-top+1);
      int middle=(top+bottom)/2;
      if(previousX>=0 && previousX==x-1) oled.drawLine(34+previousX,previousY,34+x,middle);
      previousX=x;previousY=middle;
    }
  } else { oled.drawStr(37,30,txtDEEN("Keine Daten","No data")); }
  oled.drawVLine(32,14,30); oled.drawHLine(32,44,95);
  String left=String("-")+String(hours)+"h";
  oled.drawStr(32,53,left.c_str());
  oled.drawStr(102,53,txtDEEN("jetzt","now"));
  oled.drawHLine(0,55,128);
  oled.drawStr(1,63,txtDEEN("OK:C/hPa BAK:Zeit","OK:C/hPa BAK:Time"));
  sendOledIfChanged();
}
void drawPressureScreen() {
  dashBase(txtDEEN("LUFTDRUCK","PRESSURE"),4);
  dashBig(shown(pressureHpa,0),"hPa");
  dashText(2,51,124,"Trend: "+trendText(1)); sendOledIfChanged();
}
void drawLogScreen() {
  dashBase(txtDEEN("AUFZEICHNUNG","RECORDING"),5);
  size_t total=fsReady?LittleFS.totalBytes():0,used=fsReady?LittleFS.usedBytes():0;
  dashText(2,23,124,String(logCount)+txtDEEN(" Messungen"," samples"));
  dashText(2,36,124,storageFault?storageMessage:String((total>=used?total-used:0)/1024)+txtDEEN(" KB frei"," KB free"));
  oled.drawFrame(2,42,124,9);
  if(total) oled.drawBox(4,44,(int)(120.0f*min(1.0f,(float)used/total)),5);
  sendOledIfChanged();
}
void drawCalScreen() {
  dashBase(txtDEEN("KALIBRIERUNG","CALIBRATION"),6);
  dashText(2,23,124,"RAW "+shown(rawTempC,2)+" C");
  dashText(2,36,124,"T "+shown(cfg.tempOffsetC,2)+" C Offset");
  dashText(2,50,124,"P "+shown(cfg.pressureOffsetHpa,1)+" hPa");
  sendOledIfChanged();
}
void drawNetworkScreen() {
  dashBase(txtDEEN("VERBINDUNGEN","CONNECTIONS"),7);
  dashText(2,23,124,WiFi.status()==WL_CONNECTED?"WLAN "+String(WiFi.RSSI())+" dBm":"WLAN offline / AP");
  dashText(2,36,124,String("MQTT ")+(mqttConnected?txtDEEN("verbunden","connected"):"offline"));
  uint32_t online=0; for(int i=0;i<MAX_TRUSTED;i++) if(nodeOnline[i]) online++;
  dashText(2,50,124,String("CH ")+String(WiFi.channel())+"  Nodes "+String(online));
  sendOledIfChanged();
}
void drawPowerScreen() {
  dashBase(txtDEEN("DIAGNOSE","DIAGNOSTICS"),8);
  dashText(2,23,124,String("Sensor ")+(sensorValid?"OK":txtDEEN("FEHLER","ERROR")));
  dashText(2,36,124,String(txtDEEN("RX Verlust ","RX dropped "))+String(rxDropped));
  dashText(2,50,124,epochNow()?txtDEEN("Zeit: synchron","Time: synced"):txtDEEN("Zeit: relativ","Time: relative"));
  sendOledIfChanged();
}

void drawMenuScreen() {
  oled.clearBuffer();
  drawHeader(cfg.language == 1 ? "MENU" : "MENUE");

  oled.setFont(u8g2_font_5x8_tf);
  drawFitStr(2, 20, 70, String(menuGroupName(menuIndex)));
  char countBuf[12];
  snprintf(countBuf, sizeof(countBuf), "%d/%d", menuIndex + 1, MENU_COUNT);
  drawRightText(20, countBuf);
  oled.drawHLine(0, 22, 128);

  oled.setFont(u8g2_font_5x8_tf);
  int first = menuIndex - 1;
  if (first < 0) first = 0;
  if (first > MENU_COUNT - 4) first = MENU_COUNT - 4;
  if (first < 0) first = 0;

  for (int i = 0; i < 4; i++) {
    int item = first + i;
    if (item >= MENU_COUNT) break;
    int yTop = 24 + i * 7;
    int yBase = yTop + 6;
    String txt = String(menuItemText(item));
    if (item == menuIndex) {
      oled.drawBox(0, yTop, 128, 8);
      oled.setDrawColor(0);
      drawFitStr(3, yBase, 120, String("> ") + txt);
      oled.setDrawColor(1);
    } else {
      drawFitStr(3, yBase, 120, String("  ") + txt);
    }
  }
  drawFooter("BAK", "OK");
  sendOledIfChanged();
}

void drawEditScreen(const char *title, const char *label, const String &value, const char *hint) {
  oled.clearBuffer();
  drawHeader(title);
  drawSoftFrame(6, 16, 116, 31);
  oled.setFont(u8g2_font_5x8_tf);
  drawFitStr(10, 26, 106, String(label));
  oled.setFont(u8g2_font_7x14B_tf);
  drawFitStr(10, 43, 106, value);
  drawFooter(hint, txtDEEN("BAK zurueck", "BAK back"));
  sendOledIfChanged();
}

void drawConfirmClearScreen() {
  oled.clearBuffer();
  drawHeader(cfg.language == 1 ? "CLEAR?" : "LOESCHEN?");
  drawSoftFrame(6, 16, 116, 31);
  oled.setFont(u8g2_font_5x8_tf);
  drawFitStr(12, 28, 104, cfg.language == 1 ? String("Delete local CSV?") : String("Lokale CSV loeschen?"));
  drawFitStr(12, 40, 104, cfg.language == 1 ? String("Settings stay saved") : String("Settings bleiben"));
  drawFooter(cfg.language == 1 ? "OK = YES" : "OK = JA", "BAK = NO");
  sendOledIfChanged();
}

void updateDisplay() {
  if (!oledOk || !displayOn) return;

  if (uiMode == UI_MENU) { drawMenuScreen(); return; }
  if (uiMode == UI_EDIT_INTERVAL) { drawEditScreen(cfg.language == 1 ? "INTERVAL" : "INTERVALL", cfg.language == 1 ? "Sec" : "Sek", String(editValue), cfg.language == 1 ? "Push save" : "Push speichern"); return; }
  if (uiMode == UI_EDIT_MAXDAYS) { drawEditScreen(cfg.language == 1 ? "MAX DAYS" : "MAX TAGE", cfg.language == 1 ? "Days" : "Tage", String(editFloat, 1), cfg.language == 1 ? "Push save" : "Push speichern"); return; }
  if (uiMode == UI_EDIT_TIMEOUT) { drawEditScreen("DISPLAY", cfg.language == 1 ? "Off after sec" : "Aus nach Sek", String(editValue), cfg.language == 1 ? "0=always on" : "0=immer an"); return; }
  if (uiMode == UI_EDIT_WAKE_DISPLAY) { drawEditScreen("WAKE", cfg.language == 1 ? "Sec" : "Sek", String(editValue), cfg.language == 1 ? "input time" : "Bedienzeit"); return; }
  if (uiMode == UI_EDIT_USB_WINDOW) { drawEditScreen("USB WIN", cfg.language == 1 ? "Sec" : "Sek", String(editValue), cfg.language == 1 ? "0=off" : "0=aus"); return; }
  if (uiMode == UI_EDIT_TEMP_OFFSET) { drawEditScreen("T-OFFSET", "C", String(editFloat, 2), cfg.language == 1 ? "manual offset" : "Offset manuell"); return; }
  if (uiMode == UI_EDIT_PRESS_OFFSET) { drawEditScreen("P-OFFSET", "hPa", String(editFloat, 1), cfg.language == 1 ? "manual offset" : "Offset manuell"); return; }
  if (uiMode == UI_CONFIRM_CLEAR) { drawConfirmClearScreen(); return; }

  if (cfg.screenMode == 0) {
    if (millis() - lastAutoPageMs > 8000) {
      lastAutoPageMs = millis();
      autoPage++;
      if (autoPage > 7) autoPage = 0;
    }
    switch (autoPage) {
      case 0: drawMainScreen(); break;
      case 1: drawGaugeScreen(); break;
      case 2: drawGraphScreen(); break;
      case 3: drawPressureScreen(); break;
      case 4: drawLogScreen(); break;
      case 5: drawCalScreen(); break;
      case 6: drawNetworkScreen(); break;
      case 7: drawPowerScreen(); break;
      default: drawMainScreen(); break;
    }
  } else {
    switch (cfg.screenMode) {
      case 1: drawMainScreen(); break;
      case 2: drawGaugeScreen(); break;
      case 3: drawGraphScreen(); break;
      case 4: drawPressureScreen(); break;
      case 5: drawLogScreen(); break;
      case 6: drawCalScreen(); break;
      case 7: drawNetworkScreen(); break;
      case 8: drawPowerScreen(); break;
      default: drawMainScreen(); break;
    }
  }
}

void drawStartupScreen() {
  if (!oledOk) return;
  oled.clearBuffer();
  drawHeader("LOGGER V7.4.3");
  drawSoftFrame(5, 16, 118, 31);
  oled.setFont(u8g2_font_6x10_tf);
  drawFitStr(12, 29, 104, String("BMP280 MultiNode"));
  drawFitStr(12, 41, 104, String("Start..."));
  drawFooter(txtDEEN("Push = Menue", "Push = Menu"), FW_VERSION);
  sendOledIfChanged();
}

void drawErrorScreen(const char *a, const char *b) {
  if (!oledOk) return;
  oled.clearBuffer();
  drawHeader("ERROR");
  drawSoftFrame(4, 16, 120, 31);
  oled.setFont(u8g2_font_5x8_tf);
  drawFitStr(10, 30, 108, String(a));
  drawFitStr(10, 42, 108, String(b));
  drawFooter("BAK/RESET", FW_VERSION);
  sendOledIfChanged();
}


// --------------------------------------------------
// UI Aktionen
// --------------------------------------------------
void resetStats() {
  hourHistory.clear();
  minTempC = NAN;
  maxTempC = NAN;
  minPressureHpa = NAN;
  maxPressureHpa = NAN;
  readCount = 0;
  historyIndex = 0;
  historyCount = 0;
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    tempHistory[i] = NAN;
    pressHistory[i] = NAN;
  }
  setEvent("Statistik reset");
}

void toggleLogging() {
  cfg.loggingEnabled = !cfg.loggingEnabled;
  saveConfig();
  setEvent(cfg.loggingEnabled ? "Logging START" : "Logging STOP");
}

void cyclePowerMode() {
  cfg.powerMode++;
  if (cfg.powerMode > POWER_DEEP) cfg.powerMode = POWER_NORMAL;
  if (cfg.powerMode == POWER_ECO || cfg.powerMode == POWER_DEEP) cfg.liveOutput = false;
  applyPowerMode();
}

void cycleMode() {
  cfg.mode++;
  if (cfg.mode > MODE_HYBRID) cfg.mode = MODE_SENSOR;
  if (cfg.mode == MODE_GATEWAY) cfg.powerMode = POWER_NORMAL;
  saveConfig();
  setEvent(String("Mode: ") + modeName());
}

void cycleScreenMode() {
  cfg.screenMode++;
  if (cfg.screenMode > 8) cfg.screenMode = 0;
  saveConfig();
}

void cycleGraphMode() {
  cfg.graphMode = cfg.graphMode == 0 ? 1 : 0;
  saveConfig();
  setEvent(cfg.graphMode == 0 ? "Graph Temp" : "Graph Druck");
}

void toggleUsbAutoNormal() {
  cfg.usbAutoNormal = !cfg.usbAutoNormal;
  saveConfig();
  setEvent(cfg.usbAutoNormal ? "USB Auto AN" : "USB Auto AUS");
}

void toggleLiveUsb() {
  cfg.liveOutput = !cfg.liveOutput;
  if (cfg.liveOutput) cfg.powerMode = POWER_NORMAL;
  saveConfig();
  setEvent(cfg.liveOutput ? "USB Live AN" : "USB Live AUS");
}

void togglePairMode() {
  cfg.pairMode = !cfg.pairMode;
  saveConfig();
  setEvent(cfg.pairMode ? "Pairing AN" : "Pairing AUS");
}

void pairReset() {
  cfg.paired = false;
  cfg.pairMode = true;
  memset(cfg.gatewayMac, 0, 6);
  saveConfig();
  setEvent("Pair Reset");
}

void showPairCodeSerial() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  uint32_t code = pairingCodeFor(cfg.nodeId, mac);

  loggerConsole.print("PAIRCODE,");
  loggerConsole.print(cfg.nodeId);
  loggerConsole.print(",");
  if (code < 100000) loggerConsole.print("0");
  if (code < 10000) loggerConsole.print("0");
  if (code < 1000) loggerConsole.print("0");
  if (code < 100) loggerConsole.print("0");
  if (code < 10) loggerConsole.print("0");
  loggerConsole.print(code);
  loggerConsole.print(",");
  loggerConsole.println(macToString(mac));
}

void cycleLanguage() {
  cfg.language = cfg.language == 1 ? 0 : 1;
  saveConfig();
  setEvent(cfg.language == 1 ? "Language: EN" : "Sprache: DE");
  printAck("set lang", languageCode());
  printStatusData();
}

void menuSelect() {
  switch (menuIndex) {
    case 0: uiMode = UI_DASHBOARD; break;
    case 1: toggleLogging(); break;
    case 2: readSensor(); logLocal(true); sendDataPacket(); break;
    case 3: uiMode = UI_EDIT_INTERVAL; editValue = cfg.intervalSec; break;
    case 4: uiMode = UI_EDIT_MAXDAYS; editFloat = cfg.maxDays; break;
    case 5: uiMode = UI_EDIT_TEMP_OFFSET; editFloat = cfg.tempOffsetC; break;
    case 6: uiMode = UI_EDIT_PRESS_OFFSET; editFloat = cfg.pressureOffsetHpa; break;
    case 7: cyclePowerMode(); break;
    case 8: uiMode = UI_EDIT_TIMEOUT; editValue = cfg.displayTimeoutSec; break;
    case 9: uiMode = UI_EDIT_WAKE_DISPLAY; editValue = cfg.wakeDisplaySec; break;
    case 10: toggleUsbAutoNormal(); break;
    case 11: uiMode = UI_EDIT_USB_WINDOW; editValue = cfg.usbWindowSec; break;
    case 12: toggleLiveUsb(); break;
    case 13: cycleScreenMode(); break;
    case 14: cycleGraphMode(); break;
    case 15: cycleMode(); break;
    case 16: togglePairMode(); break;
    case 17: showPairCodeSerial(); cfg.screenMode = 7; saveConfig(); break;
    case 18: pairReset(); break;
    case 19: resetStats(); break;
    case 20: uiMode = UI_CONFIRM_CLEAR; break;
    case 21: cycleLanguage(); break;
  }
}

void handleUiInput() {
  bool anyUserInput = (encoderDelta != 0 || btnPush.pressedEvent || btnBak.pressedEvent || btnCon.pressedEvent);

  if (anyUserInput) {
    if (!displayOn) {
      wakeDisplay("Taste");
      return;
    }
    lastUserActionMs = millis();
    lastAutoPageMs = millis();
  }

  if (uiMode == UI_DASHBOARD) {
    bool graphVisible = cfg.screenMode == 3 || (cfg.screenMode == 0 && autoPage == 2);
    if (graphVisible && (btnPush.pressedEvent || btnBak.pressedEvent)) {
      if (btnPush.pressedEvent) cfg.graphMode = cfg.graphMode == 0 ? 1 : 0;
      if (btnBak.pressedEvent) cfg.graphWindow = (cfg.graphWindow + 1) % 4;
      cfg.screenMode = 3; // Keep the graph visible while inspecting a selected period.
      saveConfig();
      return;
    }
    if (encoderDelta > 0) {
      if (cfg.screenMode == 0) autoPage = (autoPage + 1) % 8;
      else {
        cfg.screenMode++;
        if (cfg.screenMode > 8) cfg.screenMode = 1;
        saveConfig();
      }
    } else if (encoderDelta < 0) {
      if (cfg.screenMode == 0) {
        if (autoPage == 0) autoPage = 7;
        else autoPage--;
      } else {
        if (cfg.screenMode <= 1) cfg.screenMode = 8;
        else cfg.screenMode--;
        saveConfig();
      }
    }

    if (btnPush.pressedEvent || btnCon.pressedEvent) {
      uiMode = UI_MENU;
      menuIndex = 0;
      setEvent(cfg.language == 1 ? "Menu" : "Menue");
    }
    return;
  }

  if (uiMode == UI_MENU) {
    if (encoderDelta > 0) {
      menuIndex++;
      if (menuIndex >= MENU_COUNT) menuIndex = 0;
    } else if (encoderDelta < 0) {
      menuIndex--;
      if (menuIndex < 0) menuIndex = MENU_COUNT - 1;
    }

    if (btnPush.pressedEvent || btnCon.pressedEvent) menuSelect();
    if (btnBak.pressedEvent) uiMode = UI_DASHBOARD;
    return;
  }

  if (uiMode == UI_EDIT_INTERVAL) {
    if (encoderDelta > 0) editValue += 10;
    if (encoderDelta < 0) editValue -= 10;
    if (editValue < 1) editValue = 1;
    if (editValue > 3600) editValue = 3600;
    if (btnPush.pressedEvent || btnCon.pressedEvent) {
      cfg.intervalSec = editValue;
      saveConfig();
      uiMode = UI_MENU;
    }
    if (btnBak.pressedEvent) uiMode = UI_MENU;
    return;
  }

  if (uiMode == UI_EDIT_MAXDAYS) {
    if (encoderDelta > 0) editFloat += 0.1f;
    if (encoderDelta < 0) editFloat -= 0.1f;
    if (editFloat < 0.1f) editFloat = 0.1f;
    if (editFloat > 7.0f) editFloat = 7.0f;
    if (btnPush.pressedEvent || btnCon.pressedEvent) {
      cfg.maxDays = editFloat;
      saveConfig();
      uiMode = UI_MENU;
    }
    if (btnBak.pressedEvent) uiMode = UI_MENU;
    return;
  }

  if (uiMode == UI_EDIT_TIMEOUT) {
    if (encoderDelta > 0) editValue += 5;
    if (encoderDelta < 0) editValue -= 5;
    if (editValue < 0) editValue = 0;
    if (editValue > 600) editValue = 600;
    if (btnPush.pressedEvent || btnCon.pressedEvent) {
      cfg.displayTimeoutSec = editValue;
      saveConfig();
      uiMode = UI_MENU;
    }
    if (btnBak.pressedEvent) uiMode = UI_MENU;
    return;
  }

  if (uiMode == UI_EDIT_WAKE_DISPLAY) {
    if (encoderDelta > 0) editValue += 5;
    if (encoderDelta < 0) editValue -= 5;
    if (editValue < 5) editValue = 5;
    if (editValue > 300) editValue = 300;
    if (btnPush.pressedEvent || btnCon.pressedEvent) {
      cfg.wakeDisplaySec = editValue;
      saveConfig();
      uiMode = UI_MENU;
    }
    if (btnBak.pressedEvent) uiMode = UI_MENU;
    return;
  }

  if (uiMode == UI_EDIT_USB_WINDOW) {
    if (encoderDelta > 0) editValue += 1;
    if (encoderDelta < 0) editValue -= 1;
    if (editValue < 0) editValue = 0;
    if (editValue > 30) editValue = 30;
    if (btnPush.pressedEvent || btnCon.pressedEvent) {
      cfg.usbWindowSec = editValue;
      saveConfig();
      uiMode = UI_MENU;
    }
    if (btnBak.pressedEvent) uiMode = UI_MENU;
    return;
  }

  if (uiMode == UI_EDIT_TEMP_OFFSET) {
    if (encoderDelta > 0) editFloat += 0.1f;
    if (encoderDelta < 0) editFloat -= 0.1f;
    if (btnPush.pressedEvent || btnCon.pressedEvent) {
      cfg.tempOffsetC = editFloat;
      saveConfig();
      readSensor();
      uiMode = UI_MENU;
    }
    if (btnBak.pressedEvent) uiMode = UI_MENU;
    return;
  }

  if (uiMode == UI_EDIT_PRESS_OFFSET) {
    if (encoderDelta > 0) editFloat += 0.5f;
    if (encoderDelta < 0) editFloat -= 0.5f;
    if (btnPush.pressedEvent || btnCon.pressedEvent) {
      cfg.pressureOffsetHpa = editFloat;
      saveConfig();
      readSensor();
      uiMode = UI_MENU;
    }
    if (btnBak.pressedEvent) uiMode = UI_MENU;
    return;
  }

  if (uiMode == UI_CONFIRM_CLEAR) {
    if (btnCon.pressedEvent || btnPush.pressedEvent) {
      clearLocalLog();
      uiMode = UI_MENU;
    }
    if (btnBak.pressedEvent) uiMode = UI_MENU;
    return;
  }
}


// --------------------------------------------------
// Command ACK / GUI feedback
// --------------------------------------------------
void printAck(const String &cmd, const String &msg) {
  loggerConsole.print("ACK,");
  loggerConsole.print(cmd);
  loggerConsole.print(",");
  loggerConsole.println(msg);
}

void printErr(const String &cmd, const String &msg) {
  loggerConsole.print("ERR,");
  loggerConsole.print(cmd);
  loggerConsole.print(",");
  loggerConsole.println(msg);
}

// --------------------------------------------------
// WebGUI / OTA
// --------------------------------------------------
String htmlEscape(String s) {
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  return s;
}

String jsonEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", "\\n");
  s.replace("\r", "");
  return s;
}

String webModeShort() {
  if (cfg.mode == MODE_SENSOR) return "SEN";
  if (cfg.mode == MODE_GATEWAY) return "GW";
  return "HYB";
}

String webPowerShort() {
  if (cfg.powerMode == POWER_ECO) return "ECO";
  if (cfg.powerMode == POWER_DEEP) return "SLP";
  return "NOR";
}

String webIpInfo() {
  String s = "";
  if (WiFi.status() == WL_CONNECTED) {
    s += "STA ";
    s += WiFi.localIP().toString();
  }
  IPAddress apIp = WiFi.softAPIP();
  if (apIp[0] != 0) {
    if (s.length()) s += " | ";
    s += "AP ";
    s += apIp.toString();
  }
  if (!s.length()) s = "nicht verbunden";
  return s;
}

bool webAuth() {
  if (webServer.authenticate(cfg.webUser, cfg.webPass)) return true;
  webServer.requestAuthentication(BASIC_AUTH, "BMP280 Logger");
  return false;
}

void sendJson(const String &payload) {
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", payload);
}

// --------------------------------------------------
// MQTT / Home Assistant Discovery
// --------------------------------------------------
String mqttSanitize(String s) {
  s.trim();
  if (!s.length()) s = "bmp280";
  for (uint16_t i = 0; i < s.length(); i++) {
    char c = s[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) s.setCharAt(i, '_');
  }
  return s;
}

String mqttBaseTopic(const String &id) {
  return mqttSanitize(String(cfg.mqttPrefix)) + "/" + mqttSanitize(id);
}

String mqttDeviceJson(const String &id, const String &name) {
  String did = "bmp280_" + mqttSanitize(id);
  String d = "{\"identifiers\":[\"" + did + "\"],";
  d += "\"name\":\"" + jsonEscape(name) + "\",";
  d += "\"manufacturer\":\"DIY\",";
  d += "\"model\":\"BMP280 MultiNode Logger\",";
  d += "\"sw_version\":\"" + jsonEscape(String(FW_VERSION)) + "\"}";
  return d;
}

#if BMP_HAS_MQTT
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String cmd;
  for (unsigned int i = 0; i < length && i < 80; i++) cmd += (char)payload[i];
  cmd.trim();
  String original=cmd;
  cmd.toLowerCase();
  if (!cmd.length()) return;
  if (cmd == "start") { cfg.loggingEnabled = true; saveConfig(); printAck("mqtt start", "OK"); }
  else if (cmd == "stop") { cfg.loggingEnabled = false; saveConfig(); printAck("mqtt stop", "OK"); }
  else if (cmd == "once") { readSensor(); logLocal(true); sendDataPacket(); printAck("mqtt once", "OK"); }
  else if (cmd == "clear") { clearLocalLog(); }
  else if (cmd == "wake") { wakeDisplay("MQTT"); printAck("mqtt wake", "OK"); }
  else if (cmd.startsWith("set ")) handleSetCommand(original);
  else if (cmd.startsWith("cal ")) handleCalCommand(cmd);
  mqttPublishLocalState(true);
}
#endif

bool mqttIsUsable() {
#if BMP_HAS_MQTT
  return cfg.mqttEnabled && cfg.powerMode != POWER_DEEP && strlen(cfg.mqttHost) > 0 && WiFi.status() == WL_CONNECTED;
#else
  return false;
#endif
}

void mqttDisconnect() {
#if BMP_HAS_MQTT
  if (mqttClient.connected()) { mqttClient.publish((mqttBaseTopic(cfg.nodeId)+"/availability").c_str(),"offline",true); mqttClient.disconnect(); }
#endif
  mqttConnected = false;
}

bool mqttEnsureConnected() {
#if BMP_HAS_MQTT
  if (!mqttIsUsable()) { mqttDisconnect(); return false; }
  if (mqttClient.connected()) { mqttConnected = true; return true; }
  unsigned long now = millis();
  if (now - lastMqttAttemptMs < 5000) return false;
  lastMqttAttemptMs = now;
  mqttClient.setBufferSize(1024);
  mqttClient.setSocketTimeout(1);
  mqttWifiClient.setConnectionTimeout(1000);
  mqttClient.setServer(cfg.mqttHost, cfg.mqttPort);
  mqttClient.setCallback(mqttCallback);
  String clientId = "bmp280-" + mqttSanitize(String(cfg.nodeId));
  bool ok;
  if (strlen(cfg.mqttUser) > 0) ok = mqttClient.connect(clientId.c_str(), cfg.mqttUser, cfg.mqttPass, (mqttBaseTopic(cfg.nodeId)+"/availability").c_str(), 1, true, "offline");
  else ok = mqttClient.connect(clientId.c_str(), (mqttBaseTopic(cfg.nodeId)+"/availability").c_str(), 1, true, "offline");
  mqttConnected = ok;
  mqttLastState = ok ? "MQTT connected" : "MQTT connect failed";
  if (ok) {
    mqttDiscoverySent=false;
    for(int i=0;i<MAX_TRUSTED;i++) if(trusted[i].used && !nodeOnline[i]) mqttClient.publish((mqttBaseTopic(trusted[i].id)+"/availability").c_str(),"offline",true);
    String cmdTopic = mqttBaseTopic(cfg.nodeId) + "/cmd";
    mqttClient.subscribe(cmdTopic.c_str());
    if (cfg.mqttDiscovery && !mqttDiscoverySent) mqttPublishDiscoveryAll();
    mqttPublishLocalState(true);
  }
  return ok;
#else
  mqttConnected = false;
  mqttLastState = "PubSubClient fehlt";
  return false;
#endif
}

void mqttPublishRaw(const String &topic, const String &payload, bool retained=false) {
#if BMP_HAS_MQTT
  if (!mqttEnsureConnected()) return;
  if(!mqttClient.publish(topic.c_str(), payload.c_str(), retained)) mqttPublishFailures++;
#endif
}

void mqttDiscoverySensor(const String &id, const String &name, const String &key, const String &label, const String &unit, const String &deviceClass, const String &stateClass) {
  String uid = "bmp280_" + mqttSanitize(id) + "_" + key;
  String topic = "homeassistant/sensor/" + uid + "/config";
  String stateTopic = mqttBaseTopic(id) + "/state";
  String payload = "{\"name\":\"" + jsonEscape(label) + "\",";
  payload += "\"unique_id\":\"" + uid + "\",";
  payload += "\"state_topic\":\"" + stateTopic + "\",";
  payload += "\"value_template\":\"{{ value_json." + key + " }}\",";
  if (unit.length()) payload += "\"unit_of_measurement\":\"" + unit + "\",";
  if (deviceClass.length()) payload += "\"device_class\":\"" + deviceClass + "\",";
  if (stateClass.length()) payload += "\"state_class\":\"" + stateClass + "\",";
  if(id != String(cfg.nodeId)) payload += "\"availability_mode\":\"all\",\"availability\":[{\"topic\":\""+mqttBaseTopic(id)+"/availability\"},{\"topic\":\""+mqttBaseTopic(cfg.nodeId)+"/availability\"}],";
  else payload += "\"availability_topic\":\"" + mqttBaseTopic(id) + "/availability\",";
  payload += "\"device\":" + mqttDeviceJson(id, name) + "}";
  mqttPublishRaw(topic, payload, true);
}

void mqttDiscoveryBinary(const String &id, const String &name, const String &key, const String &label) {
  String uid = "bmp280_" + mqttSanitize(id) + "_" + key;
  String topic = "homeassistant/binary_sensor/" + uid + "/config";
  String payload = "{\"name\":\"" + jsonEscape(label) + "\",";
  payload += "\"unique_id\":\"" + uid + "\",";
  payload += "\"state_topic\":\"" + mqttBaseTopic(id) + "/state\",";
  payload += "\"value_template\":\"{{ value_json." + key + " }}\",";
  payload += "\"payload_on\":\"ON\",\"payload_off\":\"OFF\",";
  if(id != String(cfg.nodeId)) payload += "\"availability_mode\":\"all\",\"availability\":[{\"topic\":\""+mqttBaseTopic(id)+"/availability\"},{\"topic\":\""+mqttBaseTopic(cfg.nodeId)+"/availability\"}],";
  else payload += "\"availability_topic\":\"" + mqttBaseTopic(id) + "/availability\",";
  payload += "\"device\":" + mqttDeviceJson(id, name) + "}";
  mqttPublishRaw(topic, payload, true);
}

void mqttDiscoveryButton(const String &id, const String &name, const String &cmd, const String &label) {
  String uid = "bmp280_" + mqttSanitize(id) + "_btn_" + mqttSanitize(cmd);
  String topic = "homeassistant/button/" + uid + "/config";
  String payload = "{\"name\":\"" + jsonEscape(label) + "\",";
  payload += "\"unique_id\":\"" + uid + "\",";
  payload += "\"command_topic\":\"" + mqttBaseTopic(id) + "/cmd\",";
  payload += "\"payload_press\":\"" + cmd + "\",";
  if(id != String(cfg.nodeId)) payload += "\"availability_mode\":\"all\",\"availability\":[{\"topic\":\""+mqttBaseTopic(id)+"/availability\"},{\"topic\":\""+mqttBaseTopic(cfg.nodeId)+"/availability\"}],";
  else payload += "\"availability_topic\":\"" + mqttBaseTopic(id) + "/availability\",";
  payload += "\"device\":" + mqttDeviceJson(id, name) + "}";
  mqttPublishRaw(topic, payload, true);
}

void mqttPublishDiscoveryFor(const String &id, const String &name, bool withButtons) {
  mqttDiscoverySensor(id, name, "temperature", "Temperature", "°C", "temperature", "measurement");
  mqttDiscoverySensor(id, name, "pressure", "Pressure", "hPa", "pressure", "measurement");
  mqttDiscoverySensor(id, name, "raw_temperature", "Raw temperature", "°C", "temperature", "measurement");
  mqttDiscoverySensor(id, name, "raw_pressure", "Raw pressure", "hPa", "pressure", "measurement");
  mqttDiscoverySensor(id, name, "log_count", "Log count", "", "", "total_increasing");
  mqttDiscoverySensor(id, name, "mode", "Mode", "", "", "");
  mqttDiscoverySensor(id, name, "power", "Power", "", "", "");
  mqttDiscoveryBinary(id, name, "logging", "Logging");
  if (withButtons) {
    mqttDiscoveryButton(id, name, "start", "Logging Start");
    mqttDiscoveryButton(id, name, "stop", "Logging Stop");
    mqttDiscoveryButton(id, name, "once", "Log 1 Sample");
    mqttDiscoveryButton(id, name, "clear", "Clear Local Log");
    mqttDiscoveryButton(id, name, "wake", "Wake Display");
  }
}

void mqttPublishDiscoveryAll() {
  if (!cfg.mqttDiscovery || !mqttIsUsable()) return;
  uint32_t failedBefore=mqttPublishFailures; lastDiscoveryTryMs=millis();
  mqttPublishDiscoveryFor(String(cfg.nodeId), String(cfg.nodeName), true);
  if (isGatewayRole()) {
    for (int i = 0; i < MAX_TRUSTED; i++) if (trusted[i].used) mqttPublishDiscoveryFor(String(trusted[i].id), String(trusted[i].name[0] ? trusted[i].name : trusted[i].id), false);
  }
  mqttDiscoverySent = (failedBefore==mqttPublishFailures && mqttConnected);
  if(mqttDiscoverySent) printAck("mqtt discovery", "sent");
}

String mqttStatePayload(const String &id, const String &name, const String &fw, uint32_t logNo, float rt, float t, float rp, float p, float to, float po, uint8_t mode, uint8_t power, uint8_t wake, const String &mac, bool logging, uint64_t elapsed, uint64_t epoch) {
  String payload = "{";
  payload += "\"id\":\"" + jsonEscape(id) + "\",";
  payload += "\"name\":\"" + jsonEscape(name) + "\",";
  payload += "\"fw\":\"" + jsonEscape(fw) + "\",";
  payload += "\"temperature\":" + finiteJson(t) + ",";
  payload += "\"pressure\":" + finiteJson(p) + ",";
  payload += "\"raw_temperature\":" + finiteJson(rt) + ",";
  payload += "\"raw_pressure\":" + finiteJson(rp) + ",";
  payload += "\"temp_offset\":" + String(to, 2) + ",";
  payload += "\"press_offset\":" + String(po, 2) + ",";
  payload += "\"log_count\":" + String(logNo) + ",";
  payload += "\"mode\":\"" + String(mode == MODE_SENSOR ? "sensor" : (mode == MODE_GATEWAY ? "gateway" : "hybrid")) + "\",";
  payload += "\"power\":\"" + String(power == POWER_ECO ? "eco" : (power == POWER_DEEP ? "deep" : "normal")) + "\",";
  payload += "\"logging\":\"" + String(logging ? "ON" : "OFF") + "\",";
  payload += "\"wake\":" + String(wake) + ",";
  payload += "\"mac\":\"" + jsonEscape(mac) + "\",";
  payload += "\"uptime_ms\":" + u64str(elapsed) + ",\"epoch_ms\":" + u64str(epoch);
  payload += "}";
  return payload;
}

void mqttPublishLocalState(bool force) {
  if (!force && millis()-lastMqttPublishMs<1000) return;
  if (!mqttEnsureConnected()) return;
  uint8_t ownMac[6]; WiFi.macAddress(ownMac);
  String base = mqttBaseTopic(cfg.nodeId);
  mqttPublishRaw(base + "/availability", "online", true);
  mqttPublishRaw(base + "/state", mqttStatePayload(cfg.nodeId, cfg.nodeName, FW_VERSION, logCount, rawTempC, tempC, rawPressureHpa, pressureHpa, cfg.tempOffsetC, cfg.pressureOffsetHpa, cfg.mode, cfg.powerMode, (uint8_t)wakeCause, macToString(ownMac), cfg.loggingEnabled, sampleElapsed, sampleEpoch), false);
  lastMqttPublishMs = millis();
}

void mqttPublishPacketState(const DataPacket &p) {
  if (!mqttEnsureConnected()) return;
  String base = mqttBaseTopic(p.id);
  mqttPublishRaw(base + "/availability", "online", true);
  mqttPublishRaw(base + "/state", mqttStatePayload(p.id, p.name, p.fw, p.logNo, p.rawTempC, p.tempC, p.rawPressureHpa, p.pressureHpa, p.tempOffsetC, p.pressureOffsetHpa, p.mode, p.powerMode, p.wakeCause, macToString(p.mac), (p.flags & 1)!=0, p.elapsedMs, p.epochMs), false);
}

void handleMqtt() {
#if BMP_HAS_MQTT
  if (!mqttIsUsable()) { mqttDisconnect(); return; }
  if (mqttEnsureConnected()) mqttClient.loop();
  for(int i=0;i<MAX_TRUSTED;i++) if(trusted[i].used && nodeOnline[i] && millis()-trusted[i].lastSeenMs>nodeTimeoutMs[i]) {
    mqttPublishRaw(mqttBaseTopic(trusted[i].id)+"/availability","offline",true); nodeOnline[i]=false;
  }
  if(mqttClient.connected() && cfg.mqttDiscovery && !mqttDiscoverySent && millis()-lastDiscoveryTryMs>5000) mqttPublishDiscoveryAll();
  if (mqttClient.connected() && millis() - lastMqttPublishMs > 10000) mqttPublishLocalState(false);
#endif
}

void printMqttStatus() {
  loggerConsole.print("MQTT,");
#if BMP_HAS_MQTT
  loggerConsole.print(cfg.mqttEnabled ? "on" : "off"); loggerConsole.print(",");
  loggerConsole.print(mqttClient.connected() ? "connected" : "disconnected"); loggerConsole.print(",host,");
  loggerConsole.print(cfg.mqttHost); loggerConsole.print(",port,");
  loggerConsole.print(cfg.mqttPort); loggerConsole.print(",prefix,");
  loggerConsole.print(cfg.mqttPrefix); loggerConsole.print(",discovery,");
  loggerConsole.println(cfg.mqttDiscovery ? "on" : "off");
#else
  loggerConsole.println("missing_pubsubclient");
#endif
}

String statusJson() {
  uint16_t trustedCount = 0;
  for (int i = 0; i < MAX_TRUSTED; i++) if (trusted[i].used) trustedCount++;
  size_t used = LittleFS.usedBytes();
  size_t total = LittleFS.totalBytes();
  String j = "{";
  j += "\"id\":\"" + jsonEscape(String(cfg.nodeId)) + "\",";
  j += "\"name\":\"" + jsonEscape(String(cfg.nodeName)) + "\",";
  j += "\"fw\":\"" + jsonEscape(String(FW_VERSION)) + "\",";
  j += "\"temp\":" + finiteJson(tempC) + ",";
  j += "\"press\":" + finiteJson(pressureHpa) + ",";
  j += "\"rawTemp\":" + finiteJson(rawTempC) + ",";
  j += "\"rawPress\":" + finiteJson(rawPressureHpa) + ",";
  j += "\"tempOffset\":" + String(cfg.tempOffsetC, 2) + ",";
  j += "\"pressOffset\":" + String(cfg.pressureOffsetHpa, 2) + ",";
  j += "\"mode\":\"" + webModeShort() + "\",";
  j += "\"power\":\"" + webPowerShort() + "\",";
  j += "\"logging\":" + String(cfg.loggingEnabled ? "true" : "false") + ",";
  j += "\"interval\":" + String(cfg.intervalSec) + ",";
  j += "\"logCount\":" + String(logCount) + ",";
  j += "\"maxSamples\":" + String(getMaxSamples()) + ",";
  j += "\"fsUsed\":" + String((uint32_t)used) + ",";
  j += "\"fsTotal\":" + String((uint32_t)total) + ",";
  j += "\"paired\":" + String(cfg.paired ? "true" : "false") + ",";
  j += "\"pairMode\":" + String(cfg.pairMode ? "true" : "false") + ",";
  j += "\"nodes\":" + String(trustedCount) + ",";
  j += "\"lang\":\"" + String(languageCode()) + "\",";
  j += "\"web\":" + String(webRunning ? "true" : "false") + ",";
  j += "\"defaultPass\":" + String((String(cfg.webUser)=="admin" && String(cfg.webPass)=="admin") ? "true" : "false") + ",";
  j += "\"ip\":\"" + jsonEscape(webIpInfo()) + "\",";
  j += "\"host\":\"" + jsonEscape(String(cfg.webHost)) + "\",";
  j += "\"event\":\"" + jsonEscape(lastEvent) + "\"";
  j += "}";
  return j;
}

void handleApiStatus() { if (!webAuth()) return; sendJson(statusJson()); }

void handleApiHistory() {
  if (!webAuth()) return;
  String j = "{\"temp\":[";
  for (uint8_t i = 0; i < historyCount; i++) {
    uint8_t idx = (historyIndex + HISTORY_SIZE - historyCount + i) % HISTORY_SIZE;
    if (i) j += ",";
    if (isnan(tempHistory[idx])) j += "null"; else j += String(tempHistory[idx], 2);
  }
  j += "],\"press\":[";
  for (uint8_t i = 0; i < historyCount; i++) {
    uint8_t idx = (historyIndex + HISTORY_SIZE - historyCount + i) % HISTORY_SIZE;
    if (i) j += ",";
    if (isnan(pressHistory[idx])) j += "null"; else j += String(pressHistory[idx], 2);
  }
  j += "]}";
  sendJson(j);
}

void handleApiCmd() {
  if (!webAuth()) return;
  String raw = webServer.arg("cmd");
  raw = cleanInput(raw);
  String lower = raw; lower.toLowerCase();
  bool ok = true;
  if (lower == "start") { cfg.loggingEnabled = true; saveConfig(); printAck("start", "web"); }
  else if (lower == "stop") { cfg.loggingEnabled = false; saveConfig(); printAck("stop", "web"); }
  else if (lower == "once") { readSensor(); logLocal(true); sendDataPacket(); printAck("once", "web"); }
  else if (lower == "clear") { clearLocalLog(); }
  else if (lower == "wake") { wakeDisplay("Web"); printAck("wake", "web"); }
  else if (lower == "storage") { printStorage(); }
  else if (lower == "web status") { printWebStatus(); }
  else if (lower == "mqtt discovery") { mqttDiscoverySent=false; mqttPublishDiscoveryAll(); }
  else if (lower == "mqtt publish") { mqttPublishLocalState(true); }
  else if (lower == "mqtt status") { printMqttStatus(); }
  else if (lower.startsWith("set ")) { handleSetCommand(raw); }
  else if (lower.startsWith("cal ")) { handleCalCommand(raw); }
  else { ok = false; }
  printStatusData();
  sendJson(String("{\"ok\":") + (ok ? "true" : "false") + ",\"cmd\":\"" + jsonEscape(raw) + "\"}");
}

void handleApiSecurity() {
  if (!webAuth()) return;
  String u = webServer.arg("user");
  String p = webServer.arg("pass");
  u.trim(); p.trim();
  if (u.length() < 1 || u.length() > 16 || p.length() < 1 || p.length() > 32) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"bad length\"}");
    return;
  }
  memset(cfg.webUser, 0, sizeof(cfg.webUser));
  memset(cfg.webPass, 0, sizeof(cfg.webPass));
  strncpy(cfg.webUser, u.c_str(), sizeof(cfg.webUser)-1);
  strncpy(cfg.webPass, p.c_str(), sizeof(cfg.webPass)-1);
  saveConfig(); sendJson("{\"ok\":true}");
}

void handleApiWifi() {
  if (!webAuth()) return;
  String ssid = webServer.arg("ssid");
  String pass = webServer.arg("pass");
  String host = webServer.arg("host");
  ssid.trim(); host.trim();
  if (ssid.length() > 32 || pass.length() > 64 || host.length() > 32) {
    webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"too long\"}"); return;
  }
  memset(cfg.wifiSsid, 0, sizeof(cfg.wifiSsid));
  memset(cfg.wifiPass, 0, sizeof(cfg.wifiPass));
  strncpy(cfg.wifiSsid, ssid.c_str(), sizeof(cfg.wifiSsid)-1);
  strncpy(cfg.wifiPass, pass.c_str(), sizeof(cfg.wifiPass)-1);
  if (host.length()) { memset(cfg.webHost, 0, sizeof(cfg.webHost)); strncpy(cfg.webHost, host.c_str(), sizeof(cfg.webHost)-1); }
  saveConfig();
  sendJson("{\"ok\":true,\"note\":\"reconnect/reboot recommended\"}");
  WiFi.disconnect(false,false); delay(50); setupWebIfAllowed();
}


void handleApiMqtt() {
  if (!webAuth()) return;
  String en = webServer.arg("enabled");
  String host = webServer.arg("host");
  String port = webServer.arg("port");
  String user = webServer.arg("user");
  String pass = webServer.arg("pass");
  String prefix = webServer.arg("prefix");
  String disc = webServer.arg("discovery");
  if (host.length() > 64 || user.length() > 32 || pass.length() > 32 || prefix.length() > 32) { webServer.send(400, "application/json", "{\"ok\":false,\"error\":\"too long\"}"); return; }
  cfg.mqttEnabled = (en == "1" || en == "on" || en == "true");
  cfg.mqttDiscovery = !(disc == "0" || disc == "off" || disc == "false");
  memset(cfg.mqttHost, 0, sizeof(cfg.mqttHost)); strncpy(cfg.mqttHost, host.c_str(), sizeof(cfg.mqttHost)-1);
  uint16_t p = (uint16_t)port.toInt(); cfg.mqttPort = p ? p : 1883;
  memset(cfg.mqttUser, 0, sizeof(cfg.mqttUser)); strncpy(cfg.mqttUser, user.c_str(), sizeof(cfg.mqttUser)-1);
  memset(cfg.mqttPass, 0, sizeof(cfg.mqttPass)); strncpy(cfg.mqttPass, pass.c_str(), sizeof(cfg.mqttPass)-1);
  memset(cfg.mqttPrefix, 0, sizeof(cfg.mqttPrefix)); strncpy(cfg.mqttPrefix, prefix.length() ? prefix.c_str() : "bmp280", sizeof(cfg.mqttPrefix)-1);
  mqttDiscoverySent = false;
  mqttDisconnect();
  saveConfig();
  webServer.send(200, "application/json", "{\"ok\":true}");
}

void handleDownloadLocal() {
  if (!webAuth()) return;
  if (!LittleFS.exists(LOCAL_LOG_FILE)) { webServer.send(404, "text/plain", "local_log.csv nicht gefunden"); return; }
  File f = LittleFS.open(LOCAL_LOG_FILE, "r");
  webServer.sendHeader("Content-Disposition", "attachment; filename=local_log.csv");
  webServer.streamFile(f, "text/csv");
  f.close();
}

void handleDownloadLegacy() {
  if(!webAuth()) return;
  File f=LittleFS.open("/local_log.csv","r");
  if(!f) { webServer.send(404,"text/plain","Keine V73-Datei"); return; }
  webServer.sendHeader("Content-Disposition","attachment; filename=local_log_v73.csv");
  webServer.streamFile(f,"text/csv"); f.close();
}

void handleDownloadNode() {
  if (!webAuth()) return;
  String id = webServer.arg("id"); id.trim(); id.toUpperCase();
  if (id.length() != 10) { webServer.send(400, "text/plain", "NODE ID fehlt"); return; }
  String fn = "/v74_" + id + ".csv";
  if (!LittleFS.exists(fn)) { webServer.send(404, "text/plain", "Node CSV nicht gefunden"); return; }
  File f = LittleFS.open(fn, "r");
  webServer.sendHeader("Content-Disposition", "attachment; filename=node_" + id + ".csv");
  webServer.streamFile(f, "text/csv"); f.close();
}

void handleUpdatePage() {
  if (!webAuth()) return;
  String p = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'><title>OTA Update</title>";
  p += "<style>body{font-family:Arial;background:#07111f;color:#e9f1ff;padding:20px}.card{background:#101b2f;border:1px solid #20314f;border-radius:18px;padding:18px;max-width:620px}button,input{font-size:16px;padding:10px;border-radius:10px}</style></head><body>";
  p += "<div class='card'><h2>BMP280 Logger OTA Update</h2><p>Nur passende <b>.bin</b> Firmware hochladen. DeepSleep vorher vermeiden.</p>";
  p += "<form method='POST' action='/update' enctype='multipart/form-data'><input type='file' name='update' accept='.bin'><br><br><button>Firmware hochladen</button></form>";
  p += "<p><a href='/' style='color:#69d2ff'>Zurueck</a></p></div></body></html>";
  webServer.send(200, "text/html", p);
}

void handleUpdateUpload() {
  if (!webServer.authenticate(cfg.webUser, cfg.webPass)) return;
  HTTPUpload &upload = webServer.upload();
  if (upload.status == UPLOAD_FILE_START) {
    webLastUpdateError = "";
    loggerConsole.printf("OTA Start: %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { webLastUpdateError = "Update.begin fehlgeschlagen"; Update.printError(loggerConsole); }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) { webLastUpdateError = "Update.write fehlgeschlagen"; Update.printError(loggerConsole); }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) loggerConsole.printf("OTA fertig: %u Bytes\n", upload.totalSize);
    else { webLastUpdateError = "Update.end fehlgeschlagen"; Update.printError(loggerConsole); }
  }
}

void handleUpdateDone() {
  if (!webAuth()) return;
  bool ok = !Update.hasError() && webLastUpdateError.length() == 0;
  webServer.sendHeader("Connection", "close");
  webServer.send(200, "text/html", ok ? "<h2>Update OK - Neustart...</h2>" : ("<h2>Update Fehler</h2><pre>" + htmlEscape(webLastUpdateError) + "</pre>"));
  if (ok) { delay(700); ESP.restart(); }
}

void handleRootPage() {
  if (!webAuth()) return;
  static const uint8_t pageGzip[] PROGMEM = {
    0x1f,0x8b,0x08,0x00,0x00,0x00,0x00,0x00,0x02,0x03,0xb5,0x5a,0x59,0x73,0xdb,0x38,0x12,0x7e,0xcf,0xaf,0xc0,0xc8,0x95,0x25,
    0x19,0x93,0x14,0x29,0x5f,0x32,0x75,0x64,0x13,0xc7,0x39,0xaa,0x92,0xd8,0x3b,0x76,0x26,0xcf,0x10,0x09,0x4a,0x1c,0xf3,0x5a,
    0x10,0xb4,0xac,0x51,0xf4,0x9f,0xf6,0x6d,0xdf,0xe7,0x97,0x6d,0x37,0x40,0xea,0x96,0xac,0xec,0xee,0x94,0xab,0x68,0x12,0x40,
    0x37,0x3e,0xf4,0x0d,0x40,0x2f,0xba,0xbf,0x04,0x99,0x2f,0x26,0x39,0x23,0x23,0x91,0xc4,0xfd,0x6e,0xf5,0x64,0x34,0xe8,0x77,
    0x13,0x26,0x28,0x49,0x69,0xc2,0x7a,0x8d,0xc7,0x88,0x8d,0xf3,0x8c,0x8b,0x06,0xf1,0xb3,0x54,0xb0,0x54,0xf4,0x1a,0xe3,0x28,
    0x10,0xa3,0x5e,0xc0,0x1e,0x23,0x9f,0x59,0xf2,0xc3,0x24,0x51,0x1a,0x89,0x88,0xc6,0x56,0xe1,0xd3,0x98,0xf5,0xdc,0x46,0xbf,
    0x2b,0x22,0x11,0xb3,0xfe,0xdb,0x2f,0xb7,0xad,0xb6,0x43,0x3e,0x67,0xc3,0x21,0xe3,0xdd,0xa6,0x6a,0xec,0x16,0x62,0x02,0xff,
    0x5e,0x78,0x3c,0xcb,0xc4,0xd4,0xb2,0x06,0x43,0xef,0xc8,0xb9,0x70,0x5d,0x37,0xec,0x58,0x56,0x4e,0x53,0x16,0x7b,0x47,0xae,
    0xe3,0x0e,0x5a,0xf3,0xef,0x16,0x34,0x9c,0xb7,0x2e,0x4e,0x4f,0xa1,0x41,0xb0,0x27,0xe1,0x1d,0x31,0x1a,0xb6,0x42,0xec,0x4f,
    0x4a,0xc1,0x02,0xef,0xa8,0x1d,0xd0,0x96,0x8f,0xdd,0xd4,0xf7,0x01,0xa5,0x77,0x74,0x7e,0x19,0xa8,0x01,0xc3,0x2c,0x83,0xfe,
    0x53,0x1a,0xb0,0xb6,0x03,0x9f,0x03,0x0a,0x5f,0x61,0x78,0xe6,0x5f,0x50,0xf8,0x1a,0x53,0x9e,0xe2,0x67,0xe0,0x9e,0x9f,0xc3,
    0x67,0x1c,0xa5,0xcc,0x3b,0x6a,0x9d,0x9f,0xd0,0x33,0x36,0x7b,0x35,0x1d,0x64,0x4f,0x56,0x11,0xfd,0x11,0xa5,0x43,0x6f,0x90,
    0xf1,0x80,0x71,0x0b,0x5a,0x66,0x83,0x2c,0x98,0x4c,0x13,0xca,0x87,0x51,0xea,0x39,0x9d,0x10,0xc4,0x62,0x85,0x34,0x89,0xe2,
    0x89,0x77,0xc7,0x86,0x19,0x23,0xdf,0x3e,0x99,0x6f,0x38,0x08,0xc3,0x2c,0x68,0x5a,0x58,0x05,0xe3,0x51,0xd8,0x19,0x50,0xff,
    0x61,0xc8,0xb3,0x32,0x0d,0x3c,0x4e,0x03,0x94,0xd4,0x10,0xff,0x03,0x52,0xdd,0x8f,0xb8,0x1f,0x33,0x42,0x05,0x71,0x9d,0x97,
    0xc4,0x79,0x69,0x1e,0xb9,0xa7,0x27,0xa7,0x67,0x3e,0x71,0xcc,0x4a,0x2a,0xe4,0xa4,0x05,0xad,0xce,0xa9,0x73,0xe9,0x9e,0xc0,
    0x20,0xe7,0xa5,0xd1,0xf1,0xb3,0x38,0xe3,0xde,0x23,0xe5,0xba,0x12,0x88,0x31,0x43,0xcd,0x31,0x3e,0x0d,0xa2,0x22,0x8f,0xe9,
    0xc4,0x0b,0x63,0xf6,0xd4,0x19,0xd2,0xdc,0x73,0x5b,0xf9,0x53,0xe7,0xf7,0xb2,0x10,0x51,0x38,0xb1,0x2a,0x1d,0x7a,0x45,0x4e,
    0x41,0x77,0x03,0x26,0xc6,0x8c,0xa5,0x1d,0x1a,0x47,0xc3,0xd4,0x8a,0x04,0x4b,0x0a,0x0f,0x85,0xc7,0x78,0x27,0xa7,0x41,0x80,
    0xcb,0x76,0x4f,0xf3,0x27,0xe2,0xb6,0x81,0xc5,0x5c,0x00,0x42,0x64,0x89,0xe7,0x42,0x73,0x91,0xc5,0x51,0x40,0x14,0x06,0x14,
    0x9c,0xb1,0xb2,0xca,0xe1,0x80,0xea,0x67,0xe6,0xa5,0xd9,0x72,0x4c,0xfb,0xa2,0x65,0x74,0xf2,0xac,0x00,0x13,0xc9,0x52,0x0f,
    0x90,0xf8,0x0f,0x93,0x8e,0xc8,0x72,0x90,0x1e,0x52,0x04,0x3c,0xcb,0xad,0x30,0x8a,0x61,0x5e,0x6f,0x10,0x97,0x5c,0x87,0xe9,
    0x60,0x3d,0xee,0x54,0x8a,0x16,0xe4,0xcf,0xbc,0x96,0x03,0x08,0x6a,0x91,0xcf,0xec,0x3c,0x8a,0xe3,0x69,0x0d,0xf1,0x02,0x11,
    0x3a,0x0b,0x84,0x28,0xd7,0xb2,0xf0,0x2e,0x2f,0x2f,0xb1,0x6d,0x81,0x48,0x01,0x55,0xe6,0xb4,0x2a,0x40,0x69,0x42,0x86,0xd2,
    0xe4,0x98,0x45,0xc3,0x91,0xf0,0x2e,0x9c,0x4a,0xb3,0x72,0x7a,0x94,0xe1,0xcc,0x46,0x5b,0x99,0x2e,0x31,0x3c,0x3a,0xb9,0x68,
    0x5d,0xba,0xed,0x15,0x56,0x38,0xc6,0xa8,0x90,0x2c,0x09,0xe9,0xe8,0x3c,0x38,0x1d,0xb8,0x17,0xb3,0x84,0x46,0xe9,0x1c,0xb8,
    0x7b,0x2e,0x17,0xf5,0xa4,0x7c,0xc8,0x73,0xdd,0xf6,0xd2,0x2a,0x69,0x29,0xb2,0x99,0x3d,0xe4,0x51,0x30,0x57,0x29,0x7e,0x74,
    0xf0,0x01,0x1a,0x4f,0xa0,0x45,0x30,0xd0,0x67,0x5c,0x26,0x69,0xe1,0x71,0x96,0x33,0x2a,0xf4,0x53,0x33,0x89,0x52,0xe0,0xa8,
    0xbb,0x27,0xc0,0xca,0x74,0x43,0x6e,0x18,0xca,0x08,0x4e,0x71,0x01,0x3e,0xe5,0xc1,0xf2,0x02,0x50,0x69,0x94,0x2f,0x2c,0x11,
    0x00,0x04,0x6c,0x68,0x4a,0xd5,0x81,0xda,0x4e,0xce,0xcc,0x73,0x50,0xde,0xe5,0x99,0xa1,0x9a,0x5c,0xc7,0x74,0xdb,0xe6,0x49,
    0x4b,0x36,0x6d,0x59,0xe4,0x8a,0x25,0xac,0xe8,0x42,0x1a,0xd0,0xb2,0x49,0x75,0xa4,0x67,0x8d,0x68,0x90,0x8d,0x3d,0x47,0x6a,
    0x8f,0x20,0x62,0x22,0xe7,0x71,0x4c,0xfc,0xb3,0x5b,0x67,0xc6,0xcc,0x8e,0xe9,0x80,0xc5,0xd3,0x55,0x4d,0x1c,0xa2,0x3b,0xf4,
    0x09,0x4b,0x70,0x70,0xc0,0x30,0xe3,0x89,0x57,0xe6,0x39,0xe3,0x3e,0x2d,0x58,0x27,0x66,0x02,0xec,0xcc,0x42,0x07,0x40,0x30,
    0xb6,0xd3,0x66,0xc9,0xcc,0x7e,0xa4,0x71,0xc9,0x96,0xa6,0x39,0xc1,0x69,0x96,0x79,0xb6,0x81,0xa7,0xd2,0x8c,0x85,0x86,0x2b,
    0xc5,0x59,0x24,0x34,0x5e,0xc1,0x76,0xb2,0x15,0xdb,0xcc,0xe6,0xd9,0x78,0xd5,0x2f,0xf1,0x61,0x8d,0x39,0x28,0x06,0x1f,0x4a,
    0x43,0x0b,0xdd,0xcb,0x19,0xf0,0x7b,0x36,0x28,0xc1,0xd9,0x52,0x33,0x4a,0xf3,0x52,0x4c,0x2b,0x79,0x3b,0xeb,0xb2,0x6d,0x2d,
    0xcb,0x16,0x85,0x28,0x5b,0x0e,0x32,0x7c,0x19,0x39,0xd6,0x65,0x57,0xcd,0x3a,0xf5,0x4b,0x5e,0xc0,0xc8,0x3c,0x8b,0x30,0x22,
    0x54,0xad,0x36,0x46,0xd3,0xe9,0x06,0x73,0x6c,0xad,0x59,0x1f,0x39,0x27,0x60,0x7e,0xb4,0x26,0x80,0x78,0xbb,0x39,0x1e,0x1a,
    0xeb,0xe1,0xe3,0x11,0x04,0x9e,0x7a,0xb0,0x0a,0xde,0x9b,0xe3,0x55,0xfb,0x62,0x86,0x73,0x0c,0x8b,0xb3,0x15,0xb9,0x6c,0xb7,
    0xc3,0xe5,0xb5,0x9d,0xc1,0xda,0x6c,0x31,0xce,0x0e,0x71,0x28,0xd7,0x3e,0x0b,0x39,0x01,0x07,0x9a,0xbb,0xcf,0x8a,0x72,0x50,
    0xff,0x3e,0x4d,0x1f,0x69,0x31,0xad,0x9c,0x17,0xa2,0x72,0x67,0xa4,0xe6,0x69,0x9d,0x3b,0xab,0x0a,0xc0,0x30,0xee,0xb8,0xc1,
    0x36,0xc5,0xed,0x03,0x3f,0xfb,0x7b,0xc2,0x20,0x55,0xe8,0x8b,0x10,0xd1,0x3e,0x03,0xce,0xc6,0x54,0x86,0x05,0x53,0x2e,0x65,
    0x07,0xfa,0x90,0xd7,0xf9,0x60,0x39,0xb6,0x4b,0xb3,0x2b,0x04,0xe5,0x42,0x59,0x60,0x10,0x71,0xe6,0xcb,0x98,0xac,0x08,0x37,
    0xfd,0xa0,0x05,0x8e,0x3b,0x9b,0xd1,0xe9,0xb2,0xd1,0x54,0xba,0x98,0x75,0x9b,0x2a,0x8d,0x77,0x9b,0xaa,0x68,0xc0,0xb4,0xd8,
    0x7f,0xd1,0x55,0xf3,0xf6,0xbb,0x41,0xf4,0x08,0xe5,0x84,0xbb,0x5a,0x01,0x90,0xef,0x6c,0xf0,0xe1,0xdb,0x27,0x20,0x71,0xe5,
    0x08,0xe2,0xc7,0xb4,0x28,0x7a,0x0d,0xe9,0x4a,0x0d,0x12,0x05,0xf0,0x5a,0x0e,0x1a,0xfd,0x18,0x78,0xd8,0xb6,0xdd,0x6d,0x4a,
    0x2e,0xea,0xb9,0x34,0x1c,0x1c,0x0a,0x2a,0x0c,0xf0,0xe1,0xb4,0x6e,0xc1,0x8c,0xa0,0xe8,0xe3,0x6c,0xd8,0xe8,0x7f,0xbe,0xf9,
    0x40,0x5e,0x03,0x40,0x18,0xb1,0x6b,0x5c,0x92,0x05,0xac,0xd1,0xff,0x72,0xf3,0xee,0xfa,0xb9,0x91,0x51,0xde,0xe8,0x7f,0xba,
    0x5d,0x8c,0xaa,0x40,0xd5,0x0b,0xc5,0x98,0x0e,0xeb,0x46,0x78,0x38,0x3a,0x07,0x6a,0xcc,0x02,0x8d,0x9a,0x11,0x46,0x5d,0xa2,
    0x5a,0xa4,0xc0,0x7a,0x8d,0xda,0xfe,0xd2,0x2c,0x65,0xb5,0x59,0xd5,0x79,0x15,0x2c,0xab,0xd1,0xbf,0x13,0x34,0x0d,0x80,0xcc,
    0xba,0x45,0x66,0x50,0x7d,0x91,0xa8,0x10,0x84,0x3e,0x88,0xe8,0xd1,0x23,0xdd,0x41,0x9f,0x06,0x10,0xe7,0x9b,0xf2,0xd9,0x6d,
    0x0e,0xfa,0x36,0x79,0x1b,0x41,0x54,0x23,0x25,0x26,0x77,0xf9,0xe4,0xe4,0x2e,0xf2,0x47,0x8c,0x83,0x45,0x02,0x1d,0x4b,0x01,
    0x68,0x5a,0x49,0xf3,0x45,0xb7,0x50,0x4a,0xaf,0xf1,0xa1,0x09,0x35,0x56,0xc4,0x8b,0x88,0x57,0x5b,0x64,0x18,0x6e,0xf4,0xef,
    0xc1,0xd0,0x18,0xa7,0xa2,0xe4,0x9b,0x3a,0x91,0xa6,0xa3,0x04,0x86,0xf6,0xd8,0xe8,0x5b,0x96,0x6d,0xfd,0xf9,0xaf,0xab,0xcd,
    0x91,0x4b,0xca,0xe6,0x74,0x2c,0x1a,0x7d,0x78,0x12,0xcb,0xda,0xa5,0xec,0x9d,0x68,0x3e,0x97,0xa1,0x08,0x78,0xe9,0x3f,0xec,
    0x05,0x93,0x73,0x56,0x14,0x88,0xc6,0xb2,0x46,0xb7,0xf4,0x39,0x34,0xf9,0x7f,0x8d,0xe6,0x2e,0x67,0x52,0xe4,0x7b,0xc1,0x24,
    0x2c,0x41,0x28,0x2f,0xf7,0xc2,0x00,0x0b,0x96,0x80,0xd1,0x67,0x8a,0x9f,0xc7,0x31,0x29,0x40,0xfe,0x7b,0x51,0x14,0x72,0x88,
    0x94,0xc9,0x5e,0x20,0xec,0x11,0x1c,0x7d,0x79,0x58,0xf5,0xac,0x0c,0x68,0xd3,0x94,0x20,0x22,0x1d,0x68,0x49,0x9f,0xa3,0x47,
    0x46,0x3e,0x40,0xf2,0x1b,0x55,0x4c,0x55,0x34,0x95,0xd3,0xfa,0x23,0x8a,0xfb,0x0d,0xb5,0xcd,0x68,0x5c,0x3a,0x4e,0x83,0xa8,
    0xc8,0xda,0x6b,0x9c,0xc0,0x07,0x00,0x50,0x83,0xb7,0xc0,0x5e,0xb2,0x50,0xf0,0x83,0x80,0xbc,0x43,0xf3,0xb0,0x7e,0x63,0x3c,
    0xa6,0x65,0x48,0x68,0x59,0x90,0x9b,0xcf,0xd7,0xef,0xac,0x8f,0xe0,0x4e,0x19,0x9f,0xfc,0xbc,0xc5,0xa9,0x20,0x76,0x27,0x58,
    0xc9,0x78,0x99,0x0e,0x77,0x45,0x26,0x95,0xcd,0xe6,0x0e,0x06,0xc9,0xb1,0x41,0xb2,0xd4,0x8f,0xa1,0xf0,0x05,0xee,0x49,0xa0,
    0x6b,0x32,0x0a,0x6b,0x86,0x74,0x75,0x2e,0xc0,0x89,0x25,0xc5,0x3a,0x25,0xa4,0xc9,0x4d,0xc2,0x2c,0x57,0x74,0x59,0xbe,0x8b,
    0x4c,0x45,0xe8,0x75,0x4a,0xf8,0x62,0x48,0xe9,0x3e,0xa9,0x60,0x9c,0x6e,0x90,0x6f,0xcc,0xc4,0xe9,0x50,0x92,0x2c,0x6c,0xfb,
    0x20,0x9c,0x31,0x54,0x95,0x30,0x85,0x6e,0xa0,0xc4,0x60,0x43,0x88,0xf3,0x91,0x38,0x63,0x05,0xf0,0x58,0x9a,0x55,0x09,0x6f,
    0xc4,0xeb,0xb0,0x58,0xa5,0xc7,0x2a,0xc7,0xab,0x4d,0x58,0x63,0xb7,0x74,0xd7,0xd0,0x32,0x41,0x30,0xa4,0x93,0x82,0xa5,0x50,
    0xb6,0x48,0xd4,0xf2,0xed,0xb9,0x55,0xd6,0x74,0x43,0x48,0xa0,0x63,0x3a,0x41,0xc2,0x0f,0xea,0xf5,0x50,0xca,0xd1,0x64,0x00,
    0x21,0x14,0x09,0x3f,0xca,0xb7,0xf5,0x15,0x1e,0xbe,0x80,0x3c,0x1b,0x83,0x79,0xa5,0x50,0xb1,0xd2,0x18,0xf9,0x7d,0x95,0x6f,
    0x07,0xe0,0x50,0x84,0xcc,0xcf,0x90,0xea,0xda,0xcf,0x0e,0x26,0x09,0x18,0x93,0xe6,0xf4,0x0e,0xfe,0xdf,0xc5,0xf0,0x78,0x86,
    0x72,0x4c,0x1f,0xa4,0x45,0xbc,0x53,0x29,0x8c,0x7c,0x87,0xef,0xff,0x45,0xa3,0x95,0x63,0x5d,0x47,0x29,0x84,0xa4,0x38,0x06,
    0x9f,0x42,0x0b,0xd9,0x2e,0x36,0x59,0xee,0xa9,0x8c,0x8c,0x29,0x0e,0x02,0x5a,0x83,0x00,0x08,0x9f,0x8d,0xb2,0x18,0xa6,0xe9,
    0x35,0x3e,0x55,0xcd,0x31,0x18,0xc1,0xc3,0x3e,0x39,0xd7,0xf4,0x44,0x3b,0x86,0xa7,0xae,0xd5,0xdf,0x9a,0x21,0xcd,0x46,0xfc,
    0xb1,0x69,0xa6,0x7b,0xd0,0x80,0x85,0xcb,0x8c,0xb7,0x0a,0x86,0xf9,0x23,0x48,0xc9,0xf7,0x32,0x15,0x6e,0x47,0x82,0x9e,0x81,
    0x84,0x35,0x8a,0x8a,0x8f,0x04,0x81,0x74,0xe4,0x6a,0x59,0xf7,0x87,0xc0,0x50,0xb9,0x6e,0x0b,0x0e,0xae,0x22,0xe1,0x3e,0x24,
    0x92,0x76,0x09,0x8a,0xfc,0x96,0x58,0x6e,0x65,0xcf,0x16,0x30,0xff,0xbf,0x74,0xb0,0xa8,0x56,0x2a,0xa6,0xf9,0x5a,0x5c,0x87,
    0xba,0x11,0xa3,0x48,0x94,0x92,0x26,0xb9,0xb9,0x7f,0x43,0xea,0xc2,0xc8,0x26,0x75,0xad,0xa4,0x4a,0xa4,0x45,0x61,0x04,0xe5,
    0x4e,0xbe,0x57,0x5e,0x65,0xc1,0xf8,0x9a,0xac,0xde,0xb2,0xb4,0x04,0xe5,0x43,0xb3,0xcc,0x96,0x10,0x49,0x91,0xd3,0x0a,0x15,
    0x96,0x77,0x6b,0x54,0x35,0x96,0x06,0xc1,0xe3,0xb4,0xaa,0x02,0xcc,0xe4,0x4a,0x9f,0x89,0xcb,0x20,0xaf,0x92,0x47,0x62,0xa2,
    0x2f,0xc5,0xd8,0x74,0xbb,0x94,0x0f,0x91,0xe2,0xf7,0xcf,0x6f,0xbe,0x2a,0xf9,0xec,0x90,0xe2,0xcd,0x28,0x65,0x04,0x47,0x59,
    0xef,0x28,0x96,0x8a,0x32,0x0b,0x81,0x3f,0xc0,0x32,0xea,0x02,0x9d,0xc1,0xce,0x23,0x85,0x60,0xfd,0x40,0x63,0xf8,0x0f,0xbe,
    0x50,0xe6,0xd6,0x1b,0xc0,0x5d,0x14,0xd6,0x2d,0xee,0x04,0x9f,0x15,0x6b,0x51,0x40,0x39,0xb9,0x2a,0x20,0x89,0xeb,0xee,0xee,
    0xd3,0xbb,0x95,0x81,0xe3,0x2d,0xa2,0x94,0x23,0xf7,0xc8,0x73,0x41,0x3d,0xca,0x0a,0xb1,0x46,0xfc,0x11,0x9a,0xf0,0xd0,0x92,
    0x0c,0x92,0x1c,0xb6,0x1c,0x56,0x2c,0x57,0xb4,0xc5,0xe6,0xc7,0x51,0x18,0xa1,0xc8,0xe5,0x6c,0xc5,0x2e,0xb9,0xc3,0x32,0x29,
    0x19,0x71,0x16,0xf6,0x1a,0xcd,0x20,0x1b,0xa7,0x71,0x46,0x83,0x66,0x8c,0xf9,0xac,0xd1,0xff,0xed,0xe2,0x94,0x5c,0xdd,0xfd,
    0x06,0x75,0x09,0x97,0xe5,0x36,0xee,0x57,0x80,0x01,0xed,0x93,0x1f,0x64,0x1b,0x15,0x1b,0x52,0x7f,0xd2,0xe8,0xbf,0x01,0x07,
    0x27,0xbf,0x5d,0x9c,0x20,0xed,0xc6,0xe8,0x32,0x0f,0x40,0x29,0x8d,0xfe,0xfb,0x88,0x27,0xb0,0x5d,0x60,0xe4,0x9b,0x6c,0xa8,
    0x15,0x4a,0xfb,0x52,0xf2,0x6b,0x3e,0xb7,0xee,0x72,0x3b,0x6d,0xe3,0x63,0x06,0x82,0x79,0x03,0xca,0x01,0x9d,0xa7,0x02,0x98,
    0x7e,0xf9,0xc7,0xfd,0xfd,0x0e,0x33,0xc1,0x2e,0xf2,0x96,0x67,0x0f,0x60,0x0f,0x51,0x42,0x3e,0x42,0xdd,0x94,0x42,0x50,0xb4,
    0x09,0x44,0x7d,0x3f,0x7b,0x64,0x7c,0x42,0x18,0x97,0xf1,0x1a,0x3c,0xae,0x84,0x6d,0x0b,0x15,0xd0,0x31,0x22,0x2a,0xe1,0x82,
    0xd9,0x80,0x9b,0xae,0xce,0xf7,0xac,0xd5,0x24,0x5b,0xf4,0xb9,0x0c,0xe3,0xd3,0xed,0xea,0x70,0x75,0x24,0xbd,0x32,0xdc,0x6d,
    0xb7,0x4f,0x56,0x07,0x6d,0xf1,0x70,0xc9,0xf3,0x5b,0x21,0xcd,0x62,0x99,0xdd,0xa6,0x29,0xca,0x91,0x07,0x99,0x62,0x02,0xa1,
    0x32,0x8c,0x9e,0xd6,0xe8,0x95,0x11,0x6e,0xb1,0xbe,0xe4,0x9f,0x42,0x14,0xf4,0x91,0xa1,0x05,0xca,0x49,0x76,0x5a,0xe0,0x4f,
    0x54,0x3e,0xc0,0x13,0x9a,0xb5,0x9a,0xe5,0x9b,0xaf,0x87,0x14,0x2f,0x92,0x28,0x0c,0x17,0x54,0xdf,0xee,0x9e,0x21,0x93,0x24,
    0x41,0x6d,0x05,0x55,0x21,0x50,0x99,0x44,0x81,0xdb,0xcc,0xf4,0x10,0x06,0x79,0x39,0x88,0xa3,0x62,0xa4,0xc9,0x3c,0x07,0x51,
    0xfb,0x56,0x7d,0x6f,0x04,0xbe,0xb9,0x91,0x37,0xe5,0x1e,0xbb,0x5b,0xf8,0x3c,0xca,0x45,0xff,0x45,0x0c,0xe0,0x41,0x2c,0xa2,
    0x37,0x9d,0x75,0xc2,0x32,0x55,0x0e,0x80,0x79,0x2b,0x0a,0x8c,0x29,0x87,0x90,0xc5,0x53,0x12,0x64,0x7e,0x99,0x40,0xb4,0xb5,
    0x87,0x4c,0x5c,0xc7,0x0c,0x5f,0xdf,0x4e,0x3e,0x05,0x38,0x44,0x9d,0x72,0xcc,0x68,0x31,0x49,0x7d,0x32,0xa7,0x47,0x78,0xbe,
    0x31,0xa5,0x63,0x0a,0x5b,0xe6,0x90,0x09,0x7f,0xa4,0x6b,0x4d,0x9a,0x47,0x4d,0xe8,0xd0,0xcc,0x69,0xc2,0xc4,0x28,0x0b,0x3c,
    0xed,0xf6,0xe6,0xee,0x5e,0x33,0xd5,0xde,0xbf,0xf0,0xa6,0xda,0x95,0x3a,0x51,0xb7,0xee,0xc1,0x3e,0x34,0x4f,0xa3,0x79,0x0e,
    0xab,0xa5,0xc8,0xb1,0xf9,0x64,0x8d,0xc7,0x63,0x0b,0xcf,0x1f,0xad,0x92,0x43,0x34,0xf5,0xa1,0x58,0x0c,0xb4,0x99,0x89,0x07,
    0x25,0x9e,0x06,0x6c,0x7b,0xda,0xb1,0x6a,0xfd,0xf6,0xeb,0xa7,0xab,0x0c,0x6c,0x3a,0x95,0xf7,0x01,0xc6,0xcc,0xe8,0x80,0x7e,
    0xee,0xa3,0x84,0x65,0xa5,0xd0,0x31,0x82,0x98,0xa7,0x8e,0x63,0x6c,0x20,0x9e,0xd7,0xd9,0xd3,0x28,0xd4,0xfd,0x2c,0x0d,0x21,
    0x72,0xe8,0xda,0xa2,0xe4,0x1e,0x47,0xfc,0x01,0xd0,0x8c,0xe6,0xb5,0xf7,0x6b,0xc8,0xe7,0x6a,0x81,0x2a,0xed,0x23,0x03,0x6d,
    0x83,0xef,0x22,0x2b,0x6d,0x91,0x46,0xdd,0xf9,0x57,0x88,0x04,0xfd,0x75,0xbb,0x4c,0x64,0x51,0x82,0xdd,0xb0,0x80,0x63,0xed,
    0x6f,0xe8,0x86,0x7b,0x06,0x62,0x37,0x0c,0x04,0x31,0x42,0x12,0xe3,0x42,0xd7,0x3e,0xb0,0xda,0xc7,0xa0,0x60,0x78,0xcb,0x20,
    0xa6,0xa5,0x14,0xea,0xa2,0x02,0x73,0xa0,0xaa,0x2a,0x52,0xd8,0xd1,0x11,0x95,0x14,0x65,0x09,0x90,0xda,0x9b,0x72,0x51,0x69,
    0x63,0x8b,0x4c,0xb0,0xe3,0xaf,0x90,0x07,0xa6,0xd2,0x3d,0xcb,0xc4,0xee,0x83,0xe4,0x31,0xae,0x04,0x02,0x23,0x31,0xcc,0xee,
    0x19,0x89,0xdd,0x2b,0x92,0x93,0x29,0x72,0xb8,0x2c,0xbe,0x0f,0xc3,0xd0,0x46,0x71,0x11,0x70,0xfa,0x41,0x84,0x2e,0xdf,0x84,
    0x2f,0x55,0x49,0x6c,0x93,0xda,0x22,0xdc,0x6d,0x91,0x1c,0x76,0xfe,0x15,0x92,0x63,0x29,0x1d,0xc4,0x2c,0xe8,0xb9,0xcf,0x2d,
    0x38,0xa9,0x56,0x8c,0x42,0x84,0x38,0xbf,0x6f,0x24,0xf6,0x6b,0xc6,0x8f,0x1f,0x1a,0x26,0x1a,0x0d,0x29,0x9e,0xb1,0xd7,0xe4,
    0x60,0x83,0x4d,0x16,0x0a,0x52,0xb9,0x64,0xef,0x58,0x39,0x42,0x22,0x51,0x39,0x46,0x62,0x99,0x07,0xe5,0x9e,0xab,0x2d,0xd4,
    0x27,0xc3,0xfa,0x8a,0xfa,0xbe,0xb3,0x34,0x95,0xf5,0x5e,0x95,0x58,0x19,0xe7,0xd8,0x37,0xa0,0x1c,0x0b,0x68,0x93,0x3c,0x50,
    0xe8,0x5f,0x04,0x75,0xa0,0xc5,0xb0,0x2e,0x08,0x6c,0x16,0x03,0xa5,0xdf,0xb9,0x66,0xf1,0x90,0x5a,0xf7,0xc5,0x93,0x49,0x39,
    0xc7,0x8b,0x27,0x33,0xa1,0x4f,0xa6,0xdc,0xeb,0x19,0x53,0x68,0xb6,0x0b,0x81,0x53,0xdc,0xc9,0xad,0xa0,0x6c,0xee,0x60,0x2b,
    0x52,0x7d,0x97,0x47,0x3b,0x2d,0xf9,0x3d,0x60,0xe0,0x7f,0xb7,0x54,0x8c,0x74,0x03,0x2f,0x67,0x48,0xf6,0xd0,0x0b,0x69,0x5c,
    0xb0,0x0e,0x70,0xb5,0x41,0xbf,0xd7,0x14,0xac,0x45,0x7f,0x34,0x23,0xa3,0xd7,0xc7,0x38,0xf7,0xd8,0xeb,0xa5,0x65,0x1c,0x1b,
    0xd3,0xf9,0x40,0x15,0xf1,0x3b,0x33,0xa4,0x7e,0xea,0xb9,0xad,0xe3,0xe8,0x95,0xde,0xbe,0x38,0x6f,0x7e,0x01,0xa6,0xb6,0xbc,
    0x0d,0x43,0x84,0x36,0x18,0xc9,0x50,0x8c,0x2c,0xd7,0x50,0xf3,0x4c,0x7a,0xad,0xf6,0xa9,0xa5,0x3f,0x5a,0x80,0xdc,0x78,0xa5,
    0xb7,0xce,0x9d,0x05,0x81,0xed,0xb8,0xb8,0x18,0xd9,0x65,0x74,0x60,0xd2,0x5f,0xb2,0x07,0xb5,0xa6,0x04,0xc4,0x72,0x9f,0xe9,
    0x4f,0xe6,0xc4,0xe8,0x00,0x00,0xc1,0x21,0x91,0x30,0x00,0x41,0xea,0xa5,0x55,0x9d,0xa0,0x82,0x85,0x08,0xf4,0x25,0xa1,0x05,
    0x9c,0x8e,0xf5,0x91,0x31,0x45,0x08,0x7e,0x6f,0x57,0x96,0xd2,0xe4,0x19,0x98,0x66,0x98,0xc0,0xa4,0xe7,0x63,0xaf,0x74,0x85,
    0x27,0x50,0x69,0x0b,0x7c,0x5e,0xf2,0x96,0xd1,0xfb,0x57,0x48,0x91,0xf2,0xc2,0xcc,0xb7,0xd5,0x4d,0xbc,0x6f,0xab,0xb3,0x32,
    0x35,0x26,0x8c,0xe2,0x58,0x29,0x40,0xab,0x2e,0x21,0xb4,0x79,0xfb,0x33,0xa4,0xcb,0xda,0xd3,0xaa,0xbd,0xbb,0xd6,0x01,0x85,
    0xe8,0x08,0x3d,0xea,0x39,0x9d,0xa8,0x7b,0xde,0x89,0x8e,0x8f,0xd5,0x5a,0x40,0x9c,0x0e,0x08,0xfe,0x6c,0x53,0xa9,0x4b,0x62,
    0x73,0x5b,0x28,0xb7,0x25,0x51,0xb5,0xdb,0xed,0xba,0x65,0x2e,0x2a,0xe4,0x26,0x7a,0x23,0x1b,0xf7,0xc2,0xb6,0xba,0x16,0x06,
    0xa5,0xf7,0x1f,0x7f,0x51,0x7a,0x37,0x73,0xe8,0x93,0x9b,0xd3,0x2d,0x9d,0x52,0xb3,0x02,0xd4,0xd6,0x53,0xba,0x8c,0x52,0xdd,
    0xb6,0x6d,0x61,0x62,0xb5,0x20,0x19,0xfe,0xf8,0xe1,0x18,0xa6,0x00,0xdd,0xf6,0x16,0xca,0x5e,0x1b,0xe0,0x2a,0x8d,0x47,0xc5,
    0x7b,0xfc,0x51,0x03,0xd3,0x85,0xb4,0x82,0xa9,0xe4,0xea,0x74,0x24,0xad,0x2b,0x41,0xe6,0xeb,0xf3,0xe4,0x8a,0x8d,0x04,0x27,
    0x27,0xca,0xd7,0x27,0x5a,0x1d,0xb1,0x3e,0x53,0xae,0x66,0xca,0xd5,0x4c,0x79,0x35,0x53,0xed,0x65,0x4a,0x22,0x26,0xe2,0xb0,
    0xec,0x96,0x5c,0xc4,0x31,0xfc,0xd7,0xaa,0x5f,0x3a,0x80,0x55,0x2c,0x0d,0x95,0x33,0x98,0xb9,0x1c,0x7b,0x26,0x71,0x1c,0xc3,
    0x7f,0xad,0xfa,0xa5,0x83,0xb6,0x69,0x1d,0xea,0xf7,0x13,0x95,0x75,0x80,0xad,0xf5,0x34,0x79,0xf5,0x5f,0xff,0xa2,0x61,0x61,
    0x36,0xf7,0xd2,0x0a,0xf1,0x7c,0x42,0xc3,0xeb,0x60,0xb7,0x6d,0xac,0x75,0xc9,0xe3,0x82,0x92,0x33,0xcd,0x6c,0xe3,0x85,0xf1,
    0x46,0x42,0xc0,0x7a,0x46,0x57,0x46,0x53,0xf4,0x64,0x4a,0xd0,0xb7,0x94,0x19,0x82,0x8a,0x12,0x43,0xa2,0xfd,0x7b,0x91,0xa5,
    0x18,0x1a,0xb0,0xde,0x2b,0x3a,0x3b,0x1d,0xa6,0x28,0x07,0x9a,0x61,0xa3,0x8b,0x54,0x49,0xa3,0x57,0xd8,0xb8,0x19,0x3c,0xd6,
    0xc8,0x9f,0xff,0x26,0xda,0x71,0x61,0x47,0xc1,0xe2,0x3d,0x1c,0xef,0xe6,0xa4,0xce,0x60,0x56,0x58,0xe9,0x85,0x94,0xbd,0x8a,
    0x3c,0xaf,0x35,0xcb,0xd2,0x3c,0xd5,0x62,0x8b,0xec,0x7d,0xf4,0xc4,0x02,0xdd,0xc5,0xe0,0xfd,0xe7,0xbf,0xae,0xb4,0xdd,0x7c,
    0xab,0x03,0x95,0x75,0xc6,0xb2,0x79,0x95,0xb3,0xb2,0xee,0x15,0xd6,0xa3,0x5b,0xba,0x87,0x35,0x5e,0xa3,0xac,0x71,0xc6,0x36,
    0x58,0x2a,0x4c,0x00,0x2f,0xf7,0x1b,0xe0,0xab,0xc6,0xf9,0x24,0x2d,0x9c,0x04,0x76,0x85,0xb0,0x1d,0x90,0x02,0xc2,0xc5,0xdd,
    0x84,0x21,0x94,0xa1,0x4b,0x43,0xf6,0x02,0xc8,0xf7,0x01,0xb8,0xdd,0x5c,0x64,0xdd,0xba,0x13,0x82,0x94,0xc2,0x4f,0x60,0x80,
    0xed,0xfe,0x86,0x05,0xe0,0x11,0x40,0x94,0x0e,0x5f,0x6b,0xbf,0x5e,0x5f,0x91,0xcf,0x37,0x1f,0xa0,0x88,0xb8,0xbb,0xbf,0xb9,
    0xdd,0x23,0x4a,0x3c,0xd1,0xdd,0x60,0x83,0x8d,0x80,0x4d,0xa2,0xc2,0x63,0xd3,0xdd,0xe4,0x51,0xbe,0x41,0x1c,0xe5,0x32,0x2a,
    0x25,0xf0,0x1a,0x16,0xf7,0x99,0xa0,0xf1,0x6b,0x19,0x12,0xe4,0xdd,0xb0,0x8e,0x8d,0xb0,0x1f,0x0d,0x9a,0xf3,0xde,0x57,0x2e,
    0xd4,0xfa,0x9e,0xb3,0x07,0x22,0x4b,0xd6,0x26,0x49,0x8e,0xb5,0x97,0xda,0x5e,0xc9,0x14,0xdb,0x44,0x73,0x05,0x08,0xc4,0xb1,
    0xd6,0xc4,0x75,0x41,0x84,0xb8,0xa3,0x49,0x1e,0xb3,0x02,0x16,0x8a,0x17,0x4f,0x73,0xb7,0xa9,0xce,0x46,0x8f,0xb5,0x62,0xcf,
    0x14,0xea,0x4a,0x69,0x63,0x92,0x31,0x1b,0xbc,0xd6,0xbe,0x5f,0xbf,0x25,0x37,0x5f,0x41,0xf4,0xf2,0xe5,0xfd,0xfb,0x3d,0x6c,
    0xe4,0x8d,0xd3,0x06,0x17,0xd9,0xba,0xc7,0xb1,0xaa,0x1b,0x57,0xa0,0x93,0xe7,0xce,0x76,0x75,0xbf,0x0a,0x94,0x01,0x0b,0x69,
    0x19,0x0b,0xdc,0xc6,0xbf,0xd6,0x06,0x71,0xe6,0x3f,0x00,0x0c,0xbc,0x76,0xd5,0xa4,0x4e,0x46,0x3b,0xe3,0xcf,0x48,0xdd,0x10,
    0x2d,0x05,0xa0,0x2a,0x71,0xcf,0x54,0xf8,0xc2,0x0d,0x5a,0x7d,0xdc,0xac,0x76,0x68,0xad,0x33,0x50,0x5b,0xe7,0x05,0xec,0x5a,
    0xd5,0x2e,0x15,0x76,0xb3,0x78,0x21,0xde,0x6d,0xca,0x1f,0xd6,0xbd,0xf8,0x0f,0x50,0x2b,0x86,0x3f,0x70,0x27,0x00,0x00,
  };
  webServer.sendHeader("Content-Encoding", "gzip");
  webServer.send_P(200, "text/html; charset=utf-8", (const char*)pageGzip, sizeof(pageGzip));
}

void setupWebRoutes() {
  if (webRoutesConfigured) return;
  webServer.on("/", HTTP_GET, handleRootPage);
  webServer.on("/api/status", HTTP_GET, handleApiStatus);
  webServer.on("/api/history", HTTP_GET, handleApiHistory);
  webServer.on("/api/cmd", HTTP_POST, handleApiCmd);
  webServer.on("/api/security", HTTP_POST, handleApiSecurity);
  webServer.on("/api/wifi", HTTP_POST, handleApiWifi);
  webServer.on("/api/mqtt", HTTP_POST, handleApiMqtt);
  webServer.on("/download/local", HTTP_GET, handleDownloadLocal);
  webServer.on("/download/legacy", HTTP_GET, handleDownloadLegacy);
  webServer.on("/download/node", HTTP_GET, handleDownloadNode);
  webServer.on("/update", HTTP_GET, handleUpdatePage);
  webServer.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  webServer.onNotFound([](){ if (!webAuth()) return; webServer.send(404, "text/plain", "404"); });
  webRoutesConfigured = true;
}

void setupWebIfAllowed() {
  if (cfg.powerMode == POWER_DEEP || (!cfg.webEnabled && !cfg.mqttEnabled)) { stopWebServer(); return; }
  if(cfg.webEnabled) setupWebRoutes();
  WiFi.mode(cfg.webEnabled ? WIFI_AP_STA : WIFI_STA);
  bool staOk = false;
  if (strlen(cfg.wifiSsid) > 0) {
    if (WiFi.status() != WL_CONNECTED) {
      if(strlen(cfg.webHost)>0) WiFi.setHostname(cfg.webHost);
      WiFi.begin(cfg.wifiSsid, cfg.wifiPass);
      unsigned long start = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - start < 3500) delay(100);
    }
    staOk = (WiFi.status() == WL_CONNECTED);
  }
  if (!staOk && cfg.webEnabled) {
    String apName = String("BMP280-Logger-") + String(cfg.nodeId);
    WiFi.softAP(apName.c_str(), "adminadmin");
  }
  if(staOk) { configTime(0,0,"pool.ntp.org","time.nist.gov"); uint8_t ch=WiFi.channel(); if(prefs.getUChar("radioch",1)!=ch) prefs.putUChar("radioch",ch); }
  if(!cfg.webEnabled) { stopWebServer(); return; }
  if (!webRunning) {
    webServer.begin();
    webRunning = true;

    printWebStatus();
  }
}

void stopWebServer() {
  if (webRunning) {
    // WebServer has no portable hard-stop on every ESP32 core.
    // We stop servicing clients by setting webRunning=false.

    webRunning = false;
    loggerConsole.println("WEB,stopped");
  }
  if (!cfg.webEnabled || cfg.powerMode == POWER_DEEP) WiFi.softAPdisconnect(true);
}

void handleWeb() { if (webRunning) webServer.handleClient(); }

void printWebStatus() {
  loggerConsole.print("WEB,");
  loggerConsole.print(webRunning ? "on" : "off"); loggerConsole.print(",");
  loggerConsole.print(webIpInfo()); loggerConsole.print(",host,");
  loggerConsole.print(cfg.webHost); loggerConsole.print(",user,");
  loggerConsole.println(cfg.webUser);
}

// --------------------------------------------------
// Serial / Terminal
// --------------------------------------------------
void printHelp() {
  loggerConsole.println();
  loggerConsole.println("========== BMP280 LOGGER V7.4.3 WEBGUI OTA MQTT ==========");
  loggerConsole.println("Allgemein:");
  loggerConsole.println("  help, status, storage, reboot");
  loggerConsole.println("  scan");
  loggerConsole.println();
  loggerConsole.println("Modus:");
  loggerConsole.println("  set mode sensor");
  loggerConsole.println("  set mode gateway");
  loggerConsole.println("  set mode hybrid");
  loggerConsole.println();
  loggerConsole.println("Logging:");
  loggerConsole.println("  start, stop, once");
  loggerConsole.println("  dump local");
  loggerConsole.println("  dump <NODEID>");
  loggerConsole.println("  clear");
  loggerConsole.println("  resetstats");
  loggerConsole.println();
  loggerConsole.println("Einstellungen:");
  loggerConsole.println("  set interval 60");
  loggerConsole.println("  set maxdays 7");
  loggerConsole.println("  cal temp 23.8");
  loggerConsole.println("  cal press 1006.9");
  loggerConsole.println("  set temp_offset -5.4");
  loggerConsole.println("  set press_offset 0");
  loggerConsole.println("  set name Wohnzimmer");
  loggerConsole.println("  set lang de");
  loggerConsole.println("  set lang en");
  loggerConsole.println();
  loggerConsole.println("Power:");
  loggerConsole.println("  set power normal");
  loggerConsole.println("  set power eco");
  loggerConsole.println("  set power deep");
  loggerConsole.println("  set display_timeout 20");
  loggerConsole.println("  set wake_display 20");
  loggerConsole.println("  set usb_auto_normal on/off");
  loggerConsole.println("  set usb_window 2");
  loggerConsole.println("  set live on/off");
  loggerConsole.println("  wake");
  loggerConsole.println();
  loggerConsole.println("WebGUI / OTA:");
  loggerConsole.println("  web status");
  loggerConsole.println("  set web on/off");
  loggerConsole.println("  set webuser admin");
  loggerConsole.println("  set webpass admin");
  loggerConsole.println("  web resetpass");
  loggerConsole.println("  set wifi_ssid MeinWLAN");
  loggerConsole.println("  set wifi_pass Passwort");
  loggerConsole.println("  set webhost bmp280-logger");
  loggerConsole.println("  mqtt status");
  loggerConsole.println("  mqtt discovery");
  loggerConsole.println("  mqtt publish");
  loggerConsole.println("  set mqtt on/off");
  loggerConsole.println("  set mqtt_host 192.168.188.x");
  loggerConsole.println("  set mqtt_port 1883");
  loggerConsole.println("  set mqtt_user homeassistant");
  loggerConsole.println("  set mqtt_pass Passwort");
  loggerConsole.println("  set mqtt_prefix bmp280");
  loggerConsole.println("  set mqtt_discovery on/off");
  loggerConsole.println();
  loggerConsole.println("Display:");
  loggerConsole.println("  set screen auto/main/gauge/graph/pressure/log/cal/net/power");
  loggerConsole.println("  set graph temp/press; set graph_hours 1/6/12/24");
  loggerConsole.println("  set contrast 180");
  loggerConsole.println();
  loggerConsole.println("Pairing / Gateway:");
  loggerConsole.println("  pair on/off");
  loggerConsole.println("  pair code");
  loggerConsole.println("  pair reset");
  loggerConsole.println("  pair add <ID> <CODE>");
  loggerConsole.println("  pending");
  loggerConsole.println("  trust list");
  loggerConsole.println("  trust remove <ID>");
  loggerConsole.println("  rename <ID> <NAME>");
  loggerConsole.println("===========================================");
  loggerConsole.println();
}


void printStatusData() {
  uint16_t trustedCount = 0;
  for (int i = 0; i < MAX_TRUSTED; i++) {
    if (trusted[i].used) trustedCount++;
  }

  String safeName = String(cfg.nodeName);
  safeName.replace(",", "_");

  loggerConsole.print("STATUSDATA,");
  loggerConsole.print(cfg.nodeId); loggerConsole.print(",");
  loggerConsole.print(safeName); loggerConsole.print(",");
  loggerConsole.print(FW_VERSION); loggerConsole.print(",");
  loggerConsole.print(cfg.mode); loggerConsole.print(",");
  loggerConsole.print(cfg.powerMode); loggerConsole.print(",");
  loggerConsole.print(cfg.loggingEnabled ? 1 : 0); loggerConsole.print(",");
  loggerConsole.print(cfg.intervalSec); loggerConsole.print(",");
  loggerConsole.print(cfg.maxDays, 2); loggerConsole.print(",");
  loggerConsole.print(logCount); loggerConsole.print(",");
  loggerConsole.print(getMaxSamples()); loggerConsole.print(",");
  loggerConsole.print(LittleFS.usedBytes() / 1024.0f, 1); loggerConsole.print(",");
  loggerConsole.print(LittleFS.totalBytes() / 1024.0f, 1); loggerConsole.print(",");
  loggerConsole.print(getFileSize(LOCAL_LOG_FILE) / 1024.0f, 1); loggerConsole.print(",");
  loggerConsole.print(cfg.paired ? 1 : 0); loggerConsole.print(",");
  loggerConsole.print(cfg.pairMode ? 1 : 0); loggerConsole.print(",");
  loggerConsole.print(trustedCount); loggerConsole.print(",");
  loggerConsole.print(languageCode()); loggerConsole.print(",");
  loggerConsole.print(webRunning ? 1 : 0); loggerConsole.print(",");
  loggerConsole.print(webIpInfo()); loggerConsole.print(",");
  loggerConsole.println(cfg.webHost);
}

void printStatus() {
  loggerConsole.println();
  loggerConsole.println("========== STATUS ==========");
  loggerConsole.print("ID: "); loggerConsole.println(cfg.nodeId);
  loggerConsole.print("Name: "); loggerConsole.println(cfg.nodeName);
  loggerConsole.print("FW: "); loggerConsole.println(FW_VERSION);
  loggerConsole.print("Language: "); loggerConsole.println(languageLabel());
  loggerConsole.print("MAC: "); loggerConsole.println(WiFi.macAddress());
  loggerConsole.print("Mode: "); loggerConsole.println(modeName());
  loggerConsole.print("Power: "); loggerConsole.println(powerName());
  loggerConsole.print("WakeCause: "); loggerConsole.println(wakeCauseName());
  loggerConsole.print("Paired: "); loggerConsole.println(cfg.paired ? "JA" : "NEIN");
  loggerConsole.print("PairMode: "); loggerConsole.println(cfg.pairMode ? "AN" : "AUS");
  loggerConsole.print("Gateway: "); loggerConsole.println(macToString(cfg.gatewayMac));
  loggerConsole.print("Logging: "); loggerConsole.println(cfg.loggingEnabled ? "AN" : "AUS");
  loggerConsole.print("USB verworfene Ausgabezeilen: "); loggerConsole.println(loggerConsole.droppedLines());
  loggerConsole.print("Live USB: "); loggerConsole.println(cfg.liveOutput ? "AN" : "AUS");
  loggerConsole.print("Interval: "); loggerConsole.print(cfg.intervalSec); loggerConsole.println(" s");
  loggerConsole.print("MaxDays: "); loggerConsole.println(cfg.maxDays, 2);
  loggerConsole.print("Logs: "); loggerConsole.print(logCount); loggerConsole.print("/"); loggerConsole.println(getMaxSamples());
  loggerConsole.print("Temp: "); loggerConsole.print(tempC, 2); loggerConsole.print(" C raw "); loggerConsole.println(rawTempC, 2);
  loggerConsole.print("Press: "); loggerConsole.print(pressureHpa, 2); loggerConsole.print(" hPa raw "); loggerConsole.println(rawPressureHpa, 2);
  loggerConsole.print("TOff: "); loggerConsole.println(cfg.tempOffsetC, 2);
  loggerConsole.print("POff: "); loggerConsole.println(cfg.pressureOffsetHpa, 2);
  loggerConsole.print("DisplayTimeout: "); loggerConsole.println(cfg.displayTimeoutSec);
  loggerConsole.print("WakeDisplay: "); loggerConsole.println(cfg.wakeDisplaySec);
  loggerConsole.print("USB Auto: "); loggerConsole.println(cfg.usbAutoNormal ? "AN" : "AUS");
  loggerConsole.print("USB Window: "); loggerConsole.println(cfg.usbWindowSec);
  loggerConsole.print("WebGUI: "); loggerConsole.print(cfg.webEnabled ? "enabled" : "disabled"); loggerConsole.print(" / "); loggerConsole.println(webRunning ? "running" : "stopped");
  loggerConsole.print("Web IP: "); loggerConsole.println(webIpInfo());
  loggerConsole.print("Web User: "); loggerConsole.println(cfg.webUser);
  loggerConsole.println("============================");
  printStatusData();
  loggerConsole.println();
}

void printStorage() {
  loggerConsole.println();
  loggerConsole.println("========== SPEICHER ==========");
  loggerConsole.print("LittleFS total KB: "); loggerConsole.println(LittleFS.totalBytes() / 1024.0f, 1);
  loggerConsole.print("LittleFS used  KB: "); loggerConsole.println(LittleFS.usedBytes() / 1024.0f, 1);
  loggerConsole.print("Local log KB: "); loggerConsole.println(getFileSize(LOCAL_LOG_FILE) / 1024.0f, 1);
  loggerConsole.println("==============================");
  printStatusData();
  loggerConsole.println();
}

void scanI2C() {
  loggerConsole.println();
  loggerConsole.println("I2C Scan:");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      loggerConsole.print("  0x");
      if (addr < 16) loggerConsole.print("0");
      loggerConsole.print(addr, HEX);
      if (addr == 0x3C) loggerConsole.print(" OLED");
      if (addr == 0x76) loggerConsole.print(" BMP280");
      loggerConsole.println();
    }
  }
  loggerConsole.println();
}

void printPending() {
  loggerConsole.println("Pending:");
  for (int i = 0; i < MAX_PENDING; i++) {
    if (!pendingNodes[i].used) continue;
    loggerConsole.print("  ");
    loggerConsole.print(pendingNodes[i].id);
    loggerConsole.print(" FW=");
    loggerConsole.print(pendingNodes[i].fw);
    loggerConsole.print(" MAC=");
    loggerConsole.print(macToString(pendingNodes[i].mac));
    loggerConsole.print(" age=");
    loggerConsole.print((millis() - pendingNodes[i].lastSeenMs) / 1000);
    loggerConsole.println("s");
  }
}

void printTrustList() {
  loggerConsole.println("Trusted:");
  for (int i = 0; i < MAX_TRUSTED; i++) {
    if (!trusted[i].used) continue;
    loggerConsole.print("  ");
    loggerConsole.print(trusted[i].id);
    loggerConsole.print(" NAME=");
    loggerConsole.print(trusted[i].name);
    loggerConsole.print(" MAC=");
    loggerConsole.print(macToString(trusted[i].mac));
    loggerConsole.print(" LastLog=");
    loggerConsole.print(trusted[i].lastLog);
    loggerConsole.print(" Packets=");
    loggerConsole.println(trusted[i].packets);
  }
}

void pairAddCommand(String id, String codeText) {
  id.trim(); id.toUpperCase(); codeText.trim();

  if (id.length() != 10) {
    loggerConsole.println("PAIRADD Fehler: ID muss 10 Zeichen haben.");
    return;
  }

  int pidx = findPendingById(id.c_str());
  if (pidx < 0) {
    loggerConsole.println("PAIRADD Fehler: Node nicht pending.");
    return;
  }

  uint32_t entered = codeText.toInt();
  uint32_t expected = pairingCodeFor(pendingNodes[pidx].id, pendingNodes[pidx].mac);

  if (entered != expected) {
    loggerConsole.println("PAIRADD Fehler: Code falsch.");
    loggerConsole.print("PAIRFAIL,");
    loggerConsole.println(id);
    return;
  }

  int tidx = findTrustedById(id.c_str());
  if (tidx < 0) tidx = freeTrustedSlot();
  if (tidx < 0) {
    loggerConsole.println("PAIRADD Fehler: Trusted voll.");
    return;
  }

  trusted[tidx].used = true;
  strncpy(trusted[tidx].id, pendingNodes[pidx].id, 10);
  trusted[tidx].id[10] = 0;
  memcpy(trusted[tidx].mac, pendingNodes[pidx].mac, 6);
  strncpy(trusted[tidx].name, "Node", sizeof(trusted[tidx].name)-1);
  trusted[tidx].lastLog = 0;
  trusted[tidx].packets = 0;
  trusted[tidx].lastSeenMs = millis();
  saveTrusted(tidx);

  if(esp_now_is_peer_exist(trusted[tidx].mac)) esp_now_del_peer(trusted[tidx].mac);

  sendPairAccept(pendingNodes[pidx], entered);
  delay(80);
  sendPairAccept(pendingNodes[pidx], entered);
  delay(80);
  sendPairAccept(pendingNodes[pidx], entered);

  delay(80);
  if(esp_now_is_peer_exist(trusted[tidx].mac)) esp_now_del_peer(trusted[tidx].mac);
  if(!addEncryptedPeer(trusted[tidx].mac,trusted[tidx].id,trusted[tidx].mac)) printErr("pair","Peer-Limit erreicht");
  mqttDiscoverySent=false;
  pendingNodes[pidx].used = false;

  loggerConsole.print("PAIRED,");
  loggerConsole.print(trusted[tidx].id);
  loggerConsole.print(",");
  loggerConsole.println(macToString(trusted[tidx].mac));
}

void removeTrusted(String id) {
  id.trim(); id.toUpperCase();
  int idx = findTrustedById(id.c_str());
  if (idx < 0) {
    loggerConsole.println("Nicht gefunden.");
    return;
  }
  memset(&trusted[idx], 0, sizeof(TrustedNode));
  saveTrusted(idx);
  loggerConsole.print("TRUSTREMOVED,");
  loggerConsole.println(id);
}

void renameTrusted(String rest) {
  rest.trim();
  int sp = rest.indexOf(' ');
  if (sp < 0) {
    loggerConsole.println("Syntax: rename <ID> <NAME>");
    return;
  }
  String id = rest.substring(0, sp);
  String name = rest.substring(sp + 1);
  id.toUpperCase();
  name.trim();

  int idx = findTrustedById(id.c_str());
  if (idx < 0) {
    loggerConsole.println("Node nicht gefunden.");
    return;
  }

  memset(trusted[idx].name, 0, sizeof(trusted[idx].name));
  strncpy(trusted[idx].name, name.c_str(), sizeof(trusted[idx].name)-1);
  saveTrusted(idx);

  loggerConsole.print("RENAMED,");
  loggerConsole.print(id);
  loggerConsole.print(",");
  loggerConsole.println(trusted[idx].name);
}

void handleSetCommand(String cmd) {
  String lowerCmd = cmd;
  lowerCmd.toLowerCase();
  if (lowerCmd.startsWith("set mode sensor")) {
    cfg.mode = MODE_SENSOR;
    if (cfg.powerMode == POWER_DEEP) cfg.powerMode = POWER_NORMAL;
    saveConfig();
    printAck("set mode sensor", "Mode SENSOR gespeichert. Reboot");
    delay(300); ESP.restart();
    return;
  }
  if (cmd.startsWith("set mode gateway")) {
    cfg.mode = MODE_GATEWAY;
    cfg.powerMode = POWER_NORMAL;
    saveConfig();
    printAck("set mode gateway", "Mode GATEWAY gespeichert. Reboot");
    delay(300); ESP.restart();
    return;
  }
  if (cmd.startsWith("set mode hybrid")) {
    cfg.mode = MODE_HYBRID;
    cfg.powerMode = POWER_NORMAL;
    cfg.liveOutput = true;
    saveConfig();
    printAck("set mode hybrid", "Mode HYBRID gespeichert. Reboot");
    delay(300); ESP.restart();
    return;
  }
  if (cmd.startsWith("set interval ")) {
    uint32_t sec = cmd.substring(13).toInt();
    if (sec < 1) sec = 1;
    if (sec > 3600) sec = 3600;
    cfg.intervalSec = sec;
    saveConfig();
    printAck("set interval", String(cfg.intervalSec) + "s");
    printStatusData();
    return;
  }
  if (cmd.startsWith("set maxdays ")) {
    float d = parseFloatValue(cmd.substring(12));
    if (d < 0.1f) d = 0.1f;
    if (d > 7.0f) d = 7.0f;
    cfg.maxDays = d;
    saveConfig();
    printAck("set maxdays", String(cfg.maxDays, 2));
    printStatusData();
    return;
  }
  if (cmd.startsWith("set temp_offset ")) {
    cfg.tempOffsetC = parseFloatValue(cmd.substring(16));
    saveConfig();
    readSensor();
    printAck("set temp_offset", String(cfg.tempOffsetC, 2));
    return;
  }
  if (cmd.startsWith("set press_offset ")) {
    cfg.pressureOffsetHpa = parseFloatValue(cmd.substring(17));
    saveConfig();
    readSensor();
    printAck("set press_offset", String(cfg.pressureOffsetHpa, 2));
    return;
  }
  if (cmd.startsWith("set name ")) {
    String n = cmd.substring(9);
    n.trim();
    memset(cfg.nodeName, 0, sizeof(cfg.nodeName));
    strncpy(cfg.nodeName, n.c_str(), sizeof(cfg.nodeName)-1);
    saveConfig();
    printAck("set name", String(cfg.nodeName));
    return;
  }
  if (cmd == "set power normal") {
    cfg.powerMode = POWER_NORMAL;
    cfg.liveOutput = true;
    cfg.displayTimeoutSec = 0;
    displayOn = true;
    if (oledOk) oled.setPowerSave(0);
    lastUserActionMs = millis();
    applyPowerMode();
    printAck("set power normal", "Display dauerhaft AN");
    printStatusData();
    return;
  }
  if (cmd == "set power eco") {
    cfg.powerMode = POWER_ECO; cfg.liveOutput = false; applyPowerMode(); printAck("set power eco"); printStatusData(); return;
  }
  if (cmd == "set power deep") {
    cfg.powerMode = POWER_DEEP; cfg.liveOutput = false; applyPowerMode();
    printAck("set power deep", "Deep aktiv");
    loggerConsole.println("Deep aktiv. Gehe schlafen...");
    delay(500);
    enterDeepSleepNow("Terminal deep on");
    return;
  }
  if (cmd.startsWith("set display_timeout ")) {
    int t = cmd.substring(20).toInt();
    if (t < 0) t = 0;
    if (t > 600) t = 600;
    cfg.displayTimeoutSec = t;
    saveConfig();
    printAck("set display_timeout", String(cfg.displayTimeoutSec));
    return;
  }
  if (cmd.startsWith("set wake_display ")) {
    int t = cmd.substring(17).toInt();
    if (t < 5) t = 5;
    if (t > 300) t = 300;
    cfg.wakeDisplaySec = t;
    saveConfig();
    printAck("set wake_display", String(cfg.wakeDisplaySec));
    return;
  }
  if (cmd == "set usb_auto_normal on") { cfg.usbAutoNormal = true; saveConfig(); printAck("set usb_auto_normal", "on"); return; }
  if (cmd == "set usb_auto_normal off") { cfg.usbAutoNormal = false; saveConfig(); printAck("set usb_auto_normal", "off"); return; }
  if (cmd.startsWith("set usb_window ")) {
    int t = cmd.substring(15).toInt();
    if (t < 0) t = 0;
    if (t > 30) t = 30;
    cfg.usbWindowSec = t;
    saveConfig();
    printAck("set usb_window", String(cfg.usbWindowSec));
    return;
  }
  if (cmd == "set live on") { cfg.liveOutput = true; cfg.powerMode = POWER_NORMAL; saveConfig(); printAck("set live", "on"); return; }
  if (cmd == "set live off") { cfg.liveOutput = false; saveConfig(); printAck("set live", "off"); return; }

  if (cmd == "set graph_hours 1" || cmd == "set graph_hours 6" || cmd == "set graph_hours 12" || cmd == "set graph_hours 24") {
    int hours=cmd.substring(16).toInt();
    for(uint8_t i=0;i<4;i++) if(graphHours[i]==hours) cfg.graphWindow=i;
    saveConfig(); printAck("set graph_hours",String(hours)); return;
  }
  if (cmd == "set graph temp") { cfg.graphMode = 0; saveConfig(); printAck("set graph", "temp"); return; }
  if (cmd == "set graph press") { cfg.graphMode = 1; saveConfig(); printAck("set graph", "press"); return; }

  if (cmd.startsWith("set contrast ")) {
    int c = cmd.substring(13).toInt();
    if (c < 0) c = 0;
    if (c > 255) c = 255;
    cfg.contrast = c;
    if (oledOk) oled.setContrast(cfg.contrast);
    saveConfig();
    printAck("set contrast", String(cfg.contrast));
    return;
  }

  if (cmd == "set screen auto") { cfg.screenMode = 0; saveConfig(); printAck("set screen", "auto"); return; }
  if (cmd == "set screen main") { cfg.screenMode = 1; saveConfig(); printAck("set screen", "main"); return; }
  if (cmd == "set screen gauge") { cfg.screenMode = 2; saveConfig(); printAck("set screen", "gauge"); return; }
  if (cmd == "set screen graph") { cfg.screenMode = 3; saveConfig(); printAck("set screen", "graph"); return; }
  if (cmd == "set screen pressure") { cfg.screenMode = 4; saveConfig(); printAck("set screen", "pressure"); return; }
  if (cmd == "set screen log") { cfg.screenMode = 5; saveConfig(); printAck("set screen", "log"); return; }
  if (cmd == "set screen cal") { cfg.screenMode = 6; saveConfig(); printAck("set screen", "cal"); return; }
  if (cmd == "set screen net") { cfg.screenMode = 7; saveConfig(); printAck("set screen", "net"); return; }
  if (cmd == "set screen power") { cfg.screenMode = 8; saveConfig(); printAck("set screen", "power"); return; }

  if (lowerCmd == "set lang de" || lowerCmd == "set language de") {
    cfg.language = 0;
    saveConfig();
    setEvent("Sprache: DE");
    printAck("set lang", "de");
    printStatusData();
    return;
  }
  if (lowerCmd == "set lang en" || lowerCmd == "set language en") {
    cfg.language = 1;
    saveConfig();
    setEvent("Language: EN");
    printAck("set lang", "en");
    printStatusData();
    return;
  }

  if (lowerCmd == "set web on") { cfg.webEnabled = true; saveConfig(); setupWebIfAllowed(); printAck("set web", "on"); printStatusData(); return; }
  if (lowerCmd == "set web off") { cfg.webEnabled = false; saveConfig(); stopWebServer(); printAck("set web", "off"); printStatusData(); return; }
  if (lowerCmd.startsWith("set webuser ")) { String u = cmd.substring(12); u.trim(); if (u.length()<1 || u.length()>16) { printErr("set webuser", "1-16 Zeichen"); return; } memset(cfg.webUser,0,sizeof(cfg.webUser)); strncpy(cfg.webUser,u.c_str(),sizeof(cfg.webUser)-1); saveConfig(); printAck("set webuser", "OK"); return; }
  if (lowerCmd.startsWith("set webpass ")) { String p = cmd.substring(12); p.trim(); if (p.length()<1 || p.length()>32) { printErr("set webpass", "1-32 Zeichen"); return; } memset(cfg.webPass,0,sizeof(cfg.webPass)); strncpy(cfg.webPass,p.c_str(),sizeof(cfg.webPass)-1); saveConfig(); printAck("set webpass", "OK"); return; }
  if (lowerCmd.startsWith("set wifi_ssid ")) { String s = cmd.substring(14); s.trim(); if (s.length()>32) { printErr("set wifi_ssid", "max 32 Zeichen"); return; } memset(cfg.wifiSsid,0,sizeof(cfg.wifiSsid)); strncpy(cfg.wifiSsid,s.c_str(),sizeof(cfg.wifiSsid)-1); saveConfig(); printAck("set wifi_ssid", "OK"); return; }
  if (lowerCmd.startsWith("set wifi_pass ")) { String p = cmd.substring(14); p.trim(); if (p.length()>64) { printErr("set wifi_pass", "max 64 Zeichen"); return; } memset(cfg.wifiPass,0,sizeof(cfg.wifiPass)); strncpy(cfg.wifiPass,p.c_str(),sizeof(cfg.wifiPass)-1); saveConfig(); printAck("set wifi_pass", "OK"); return; }
  if (lowerCmd.startsWith("set webhost ")) { String h = cmd.substring(12); h.trim(); h.toLowerCase(); if (h.length()<1 || h.length()>32) { printErr("set webhost", "1-32 Zeichen"); return; } memset(cfg.webHost,0,sizeof(cfg.webHost)); strncpy(cfg.webHost,h.c_str(),sizeof(cfg.webHost)-1); saveConfig(); printAck("set webhost", String(cfg.webHost)); return; }


  if(lowerCmd.startsWith("set mqtt")) {
    String rest=cmd.substring(4); int split=rest.indexOf(' ');
    String key=split<0 ? rest : rest.substring(0,split); key.toLowerCase();
    String value=split<0 ? "" : rest.substring(split+1);
    bool known=true;
    if(key=="mqtt" || key=="mqtt_discovery") {
      String v=value; v.toLowerCase();
      if(v!="on" && v!="off") { printErr("mqtt","on/off erwartet"); return; }
      if(key=="mqtt") cfg.mqttEnabled=(v=="on"); else cfg.mqttDiscovery=(v=="on");
    } else if(key=="mqtt_port") {
      long port=value.toInt(); if(port<1 || port>65535) { printErr("mqtt","Port 1-65535"); return; } cfg.mqttPort=port;
    } else {
      char *target=nullptr; size_t capacity=0;
      if(key=="mqtt_host") {target=cfg.mqttHost; capacity=sizeof(cfg.mqttHost);}
      else if(key=="mqtt_user") {target=cfg.mqttUser; capacity=sizeof(cfg.mqttUser);}
      else if(key=="mqtt_pass") {target=cfg.mqttPass; capacity=sizeof(cfg.mqttPass);}
      else if(key=="mqtt_prefix") {target=cfg.mqttPrefix; capacity=sizeof(cfg.mqttPrefix); value=mqttSanitize(value);}
      else known=false;
      if(target) { if(value.length()>=capacity) { printErr("mqtt","Wert zu lang"); return; } memset(target,0,capacity); strncpy(target,value.c_str(),capacity-1); }
    }
    if(!known) { printErr("mqtt","Unbekannte Einstellung"); return; }
    mqttDisconnect(); mqttDiscoverySent=false; saveConfig(); printAck("mqtt","Gespeichert"); return;
  }
  printErr("set", "Unbekannter set-Befehl");
}

void handleCalCommand(String cmd) {
  if (cmd.startsWith("cal temp ")) {
    float realT = parseFloatValue(cmd.substring(9));
    if (isnan(rawTempC)) { loggerConsole.println("Keine Roh-Temp."); return; }
    cfg.tempOffsetC = realT - rawTempC;
    saveConfig();
    readSensor();
    printAck("cal temp", String(cfg.tempOffsetC, 2));
    return;
  }
  if (cmd.startsWith("cal press ")) {
    float realP = parseFloatValue(cmd.substring(10));
    if (isnan(rawPressureHpa)) { loggerConsole.println("Kein Roh-Druck."); return; }
    cfg.pressureOffsetHpa = realP - rawPressureHpa;
    saveConfig();
    readSensor();
    printAck("cal press", String(cfg.pressureOffsetHpa, 2));
    return;
  }
}

void handleSerial() {
  static String pending;
  bool complete=false;
  for(int n=0;n<128 && Serial.available();n++) {
    char c=(char)Serial.read();
    if(c=='\n') { complete=true; break; }
    if(c!='\r') pending+=c;
    if(pending.length()>512) { pending=""; printErr("serial","Befehl zu lang"); return; }
  }
  if(!complete) return;
  String cmd=pending; pending="";
  cmd = cleanInput(cmd);
  String lower = cmd;
  lower.toLowerCase();

  if(lower.startsWith("time ")) {
    uint64_t seconds=strtoull(cmd.substring(5).c_str(),nullptr,10);
    if(seconds>=1700000000ULL && seconds<4102444800ULL) { struct timeval tv={(time_t)seconds,0}; settimeofday(&tv,nullptr); printAck("time","UTC synchronisiert"); }
    else printErr("time","Ungueltige Unixzeit");
  }
  else if(lower.startsWith("sync ")) startSync(cmd.substring(5));
  else if(lower=="formatfs confirm") { if(syncFile) syncFile.close(); syncSlot=-2; if(dumpStream) dumpStream.close(); radioCursor=0; radioWaiting=false; fsReady=LittleFS.format() && LittleFS.begin(false); storageFault=!fsReady; storageMessage=fsReady?"OK":"FS FEHLER"; logCount=0; printAck("formatfs",storageMessage); }
  else if (lower == "help") printHelp();
  else if (lower == "status") printStatus();
  else if (lower == "storage") printStorage();
  else if (lower == "scan") scanI2C();
  else if (lower == "reboot") ESP.restart();
  else if (lower == "start") { cfg.loggingEnabled = true; saveConfig(); printAck("start", "Logging gestartet"); printStatusData(); }
  else if (lower == "stop") { cfg.loggingEnabled = false; saveConfig(); printAck("stop", "Logging gestoppt"); printStatusData(); }
  else if (lower == "once") { readSensor(); logLocal(true); sendDataPacket(); printStatusData(); }
  else if (lower == "clear") clearLocalLog();
  else if (lower == "resetstats") resetStats();
  else if (lower == "wake") wakeDisplay("Terminal");
  else if (lower == "dump local") dumpFile(LOCAL_LOG_FILE);
  else if (lower == "dump legacy") dumpFile("/local_log.csv");
  else if (lower.startsWith("dump ")) {
    String id = cmd.substring(5);
    id.trim(); id.toUpperCase();
    String fn = "/v74_" + id + ".csv";
    dumpFile(fn);
  }
  else if (lower == "web status") printWebStatus();
  else if (lower == "web resetpass") { memset(cfg.webUser, 0, sizeof(cfg.webUser)); memset(cfg.webPass, 0, sizeof(cfg.webPass)); strncpy(cfg.webUser, "admin", sizeof(cfg.webUser)-1); strncpy(cfg.webPass, "admin", sizeof(cfg.webPass)-1); saveConfig(); printAck("web resetpass", "admin/admin"); printStatusData(); }
  else if (lower == "mqtt status") printMqttStatus();
  else if (lower == "mqtt discovery") { mqttDiscoverySent = false; mqttPublishDiscoveryAll(); printMqttStatus(); }
  else if (lower == "mqtt publish") { mqttPublishLocalState(true); printMqttStatus(); }
  else if (lower.startsWith("set ")) handleSetCommand(cmd);
  else if (lower.startsWith("cal ")) handleCalCommand(cmd);
  else if (lower == "defaults") { factoryDefaults(); ESP.restart(); }
  else if (lower == "pair on") { cfg.pairMode = true; saveConfig(); printAck("pair", "on"); }
  else if (lower == "pair off") { cfg.pairMode = false; saveConfig(); printAck("pair", "off"); }
  else if (lower == "pair code") showPairCodeSerial();
  else if (lower == "pair reset") pairReset();
  else if (lower.startsWith("pair add ")) {
    String rest = cmd.substring(9);
    rest.trim();
    int sp = rest.indexOf(' ');
    if (sp < 0) loggerConsole.println("Syntax: pair add <ID> <CODE>");
    else pairAddCommand(rest.substring(0, sp), rest.substring(sp + 1));
  }
  else if (lower == "pending") printPending();
  else if (lower == "trust list" || lower == "nodes") printTrustList();
  else if (lower.startsWith("trust remove ")) removeTrusted(cmd.substring(13));
  else if (lower.startsWith("rename ")) renameTrusted(cmd.substring(7));
  else loggerConsole.println("Unbekannter Befehl. help eingeben.");
}

// --------------------------------------------------
// Setup / Loop
// --------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.setTimeout(80);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  // HWCDC otherwise waits repeatedly when the PC keeps USB powered but stops reading.
  Serial.setTxTimeoutMs(0);
#endif
  delay(400);

  wakeCause = esp_sleep_get_wakeup_cause();

  pinMode(PIN_ENC_A, INPUT_PULLUP);
  pinMode(PIN_ENC_B, INPUT_PULLUP);
  pinMode(PIN_ENC_PUSH, INPUT_PULLUP);
  pinMode(PIN_BAK, INPUT_PULLUP);
  pinMode(PIN_CONTR, INPUT_PULLUP);

  delay(50);
  btnPush.stableState = digitalRead(btnPush.pin);
  btnPush.lastRawState = btnPush.stableState;
  btnBak.stableState = digitalRead(btnBak.pin);
  btnBak.lastRawState = btnBak.stableState;
  btnCon.stableState = digitalRead(btnCon.pin);
  btnCon.lastRawState = btnCon.stableState;
  lastEncoderState = readEncoderState();

  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    tempHistory[i] = NAN;
    pressHistory[i] = NAN;
  }

  WiFi.mode(WIFI_STA);
  loadConfig();
  if(rtcMagic!=0x74304C47 || wakeCause==ESP_SLEEP_WAKEUP_UNDEFINED) { rtcMagic=0x74304C47; sessionId=esp_random(); sampleSequence=0; elapsedBaseMs=0; radioCursor=0; }
  loadTrusted();
  rxQueue=xQueueCreate(12,sizeof(RxEnvelope));

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  fsReady=LittleFS.begin(false);
  if (!fsReady) {
    storageFault=true; storageMessage="FS FEHLER";
    loggerConsole.println("LittleFS Fehler.");
  }

  oled.setI2CAddress(OLED_ADDR_8BIT);
  oledOk = oled.begin();
  if (oledOk) {
    oled.setContrast(cfg.contrast);
    displayOn = !(cfg.powerMode==POWER_DEEP && wakeCause==ESP_SLEEP_WAKEUP_TIMER);
    oled.setPowerSave(displayOn ? 0 : 1);
  }

  bmpOk = initBMP280();

  ensureLocalLogHeader();
  if(wakeCause!=ESP_SLEEP_WAKEUP_UNDEFINED && cachedLogBytes==getFileSize(LOCAL_LOG_FILE)) logCount=cachedLogRows;
  else logCount=countLocalLogRows();
  cachedLogRows=logCount; cachedLogBytes=getFileSize(LOCAL_LOG_FILE);

  initEspNow();
  esp_now_register_recv_cb(onRecv);

  if (isGatewayRole()) {
    for (int i = 0; i < MAX_TRUSTED; i++) {
      if (trusted[i].used) addEncryptedPeer(trusted[i].mac, trusted[i].id, trusted[i].mac);
    }
  }

  if (isSensorRole() && cfg.paired) {
    uint8_t ownMac[6];
    WiFi.macAddress(ownMac);
    addEncryptedPeer(cfg.gatewayMac, cfg.nodeId, ownMac);
  }

  setupWebIfAllowed();

  // Deep Timer: ohne Display messen/loggen/senden/schlafen
  if (cfg.powerMode == POWER_DEEP && wakeCause == ESP_SLEEP_WAKEUP_TIMER && isSensorRole() && bmpOk) {
    deepTimerWakeJobAndSleep();
  }

  if (oledOk) drawStartupScreen();

  if (bmpOk) readSensor();
  else if (isSensorRole()) drawErrorScreen("BMP280 fehlt", "I2C 0x76 pruefen");

  applyPowerMode();

  if (cfg.powerMode == POWER_DEEP && cfg.usbAutoNormal && waitForUsbNormalWindow(cfg.usbWindowSec)) {
    cfg.powerMode = POWER_NORMAL;
    cfg.liveOutput = true;
    applyPowerMode();
    loggerConsole.println("USB erkannt -> Normalmodus");
  }

  if (cfg.powerMode == POWER_DEEP && wakeCause == ESP_SLEEP_WAKEUP_GPIO) {
    lastUserActionMs = millis();
    displayOn = true;
    if (oledOk) oled.setPowerSave(0);
    loggerConsole.println("Button Wake -> Bedienung aktiv.");
  }

  if (oledOk && displayOn) updateDisplay();

  loggerConsole.println();
  loggerConsole.println("==================================");
  loggerConsole.println(" BMP280 LOGGER V7.4.3 WEBGUI OTA MQTT");
  loggerConsole.println("==================================");
  printStatus();
  // Full help is available on request; do not flood USB during startup.

  if (cfg.mode == MODE_SENSOR && (cfg.pairMode || !cfg.paired)) {
    showPairCodeSerial();
    sendPairRequest();
  }

  lastReadMs = millis();
  lastLogMs = millis();
  lastDisplayMs = millis();
  lastAutoPageMs = millis();
  lastUserActionMs = millis();
  lastPairBroadcastMs = millis();
}

void loop() {
  loggerConsole.pump(Serial);
  handleSerial();
  processRadioQueue();
  serviceRadioSpool();
  serviceTransfers();
  handleWeb();
  handleMqtt();

  updateEncoder();
  updateButton(btnPush);
  updateButton(btnBak);
  updateButton(btnCon);
  handleUiInput();

  unsigned long now = millis();
  if(isSensorRole() && !bmpOk && now-lastSensorRetryMs>=5000) { lastSensorRetryMs=now; bmpOk=initBMP280(); if(bmpOk) readSensor(); }

  if (isSensorRole() && bmpOk && now - lastReadMs >= 1000) {
    lastReadMs = now;
    readSensor();
  }

  if (isSensorRole() && bmpOk && cfg.powerMode != POWER_DEEP && now - lastLogMs >= cfg.intervalSec * 1000UL) {
    lastLogMs = now;
    logLocal(false);
    sendDataPacket();
  }

  if (cfg.mode == MODE_SENSOR && (cfg.pairMode || !cfg.paired) && now - lastPairBroadcastMs > 5000) {
    lastPairBroadcastMs = now;
    if(WiFi.status()!=WL_CONNECTED && cfg.mode==MODE_SENSOR) { radioChannel=radioChannel%13+1; esp_wifi_set_channel(radioChannel,WIFI_SECOND_CHAN_NONE); }
    sendPairRequest();
  }

  if (displayOn && oledOk && now - lastDisplayMs >= 200) {
    lastDisplayMs = now;
    updateDisplay();
  }

  maybeSleepDisplay();
  maybeReturnToDeepAfterButtonWake();
  loggerConsole.pump(Serial);
}

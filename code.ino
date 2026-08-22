/*
  GREENHOUSE CONTROLLER Ã¢â‚¬â€ COMPLETE SINGLE-FILE SKETCH

  Open this file in Arduino IDE. No other project .ino or .h file is required.

  Code map:
    1. Configuration, shared state, setup, and loop
    2. Pump safety, cooldown, Preferences, and network route state
    3. Captive-portal page stored in flash
    4. Captive-portal endpoints and input validation
    5. A7670 modem initialization and time synchronization
    6. Firebase telemetry transport

  Critical invariant:
    Every pump start goes through startPumpSafely(), and managePumpSafety()
    remains active during the main loop and network waits.
*/

// ============================================================================
// 1. SYSTEM CONFIGURATION AND MAIN PROGRAM
// ============================================================================

#define TINY_GSM_MODEM_A7672X
#define TINY_GSM_DEBUG Serial

/*
  Smart greenhouse controller

  Keep this tab as the system map: hardware configuration, shared state,
  setup(), and loop(). Implementation is grouped into the other Arduino tabs.
  Every pump start passes through startPumpSafely(); every running pump is
  supervised by managePumpSafety(), including during modem waits.
*/

#include <TinyGsmClient.h>

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include "time.h"

WebServer server(80);
DNSServer dnsServer;
Preferences preferences;

const byte DNS_PORT = 53;

// ===== CELLULAR SETTINGS =====
#define SerialAT Serial2
#define RX_PIN 16
#define TX_PIN 17
#define MODEM_PWR_PIN 5 // <--- UPDATE THIS TO THE PIN YOU CONNECTED PWR_EN TO
const char apn[]  = "internet"; // Globe APN
const char gprsUser[] = "";
const char gprsPass[] = "";

TinyGsm modem(SerialAT);

bool cellularActive = false;
bool cloudAvailable = false;
bool cloudAttempted = false;
unsigned long lastCloudAttempt = 0;
const unsigned long cloudRetryInterval = 30000UL; // Retry Firebase after 30 seconds.
const unsigned long cellularRetryInterval = 300000UL; // Power-cycle cellular at most every 5 minutes.
unsigned long lastCellularReconnectAttempt = 0;
bool lastUploadSucceeded = false;
unsigned long lastUploadAttemptMs = 0;
unsigned long lastUploadSuccessMs = 0;
unsigned int consecutiveUploadFailures = 0;
const char* uploadState = "Waiting for first sample";
String backupWifiSsid = "";
String backupWifiPassword = "";
bool wifiBackupConfigured = false;
bool wifiBackupActive = false;
bool cellularSuspendedForWifi = false;
unsigned long lastWifiAttempt = 0;
const unsigned long wifiRetryInterval = 60000UL;
const unsigned long wifiConnectTimeout = 15000UL;
bool wifiConnectionInProgress = false;
unsigned long wifiConnectionStarted = 0;

// A fixed-size queue avoids heap allocation while preserving samples during
// short network outages. The oldest entry is overwritten only when full.
struct TelemetryRecord {
  float temperature;
  float humidity;
  int soilRaw;
  int soilPercent;
  int rainRaw;
  bool dhtError;
  bool rainDetected;
  bool pumpOn;
  unsigned long capturedUptime;
  int64_t capturedUnix;
};
const uint8_t TELEMETRY_QUEUE_SIZE = 12;
TelemetryRecord telemetryQueue[TELEMETRY_QUEUE_SIZE];
uint8_t telemetryQueueHead = 0;
uint8_t telemetryQueueCount = 0;
unsigned long droppedTelemetryRecords = 0;
String lastProcessedCommandId = "";
int64_t lastProcessedIssuedAt = 0;

void enqueueTelemetry(float temperature, float humidity, bool dhtError,
                      int soilRaw, int soilPercent, int rainRaw,
                      bool rainDetected, bool pumpOn) {
  if (telemetryQueueCount == TELEMETRY_QUEUE_SIZE) {
    telemetryQueueHead = (telemetryQueueHead + 1) % TELEMETRY_QUEUE_SIZE;
    telemetryQueueCount--;
    droppedTelemetryRecords++;
  }
  uint8_t index = (telemetryQueueHead + telemetryQueueCount) % TELEMETRY_QUEUE_SIZE;
  telemetryQueue[index] = {temperature, humidity, soilRaw, soilPercent, rainRaw,
                           dhtError, rainDetected, pumpOn, millis(),
                           clockIsValid() ? (int64_t)time(nullptr) : 0};
  telemetryQueueCount++;
}

void removeQueuedTelemetry() {
  if (telemetryQueueCount == 0) return;
  telemetryQueueHead = (telemetryQueueHead + 1) % TELEMETRY_QUEUE_SIZE;
  telemetryQueueCount--;
}

// ===== GLOBAL SENSOR DATA =====
float currentHumidity = 0.0;
float currentTemperature = 0.0;
int currentSoilRaw = 0;
int currentSoilPercent = 0;
int currentRainRaw = 0;
bool currentRainDetected = false;
bool currentDhtError = false;

// ===== MANUAL CONTROL =====
bool manualMode = false;
bool manualPumpState = false;
bool emergencyStopLatched = false;

// ===== DHT22 =====
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// ===== SENSOR PINS =====
#define SOIL_PIN 34
#define RAIN_PIN 35

// ===== RELAY PIN =====
#define RELAY_PIN 23

// Most relay modules are ACTIVE LOW
#define RELAY_ON LOW
#define RELAY_OFF HIGH

// ===== SOIL CALIBRATION =====
int soilDryValue = 4095;
int soilWetValue = 500;

// ===== RAIN CALIBRATION =====
int rainThreshold = 3800;

// ===== PUMP THRESHOLDS =====
int pumpOnThreshold = 15;    // Pump ON only below 15%
int pumpOffThreshold = 55;   // Pump OFF at 55% and above

bool pumpState = false;
String pumpStopReason = "startup";

// ===== AUTO RUN LIMIT & COOLDOWN =====
unsigned long pumpOnTime = 0;
const unsigned long maxRunTime = 120000;       // 2 minutes in ms
const unsigned long cooldownTime = 18000000;   // 5 hours in ms
unsigned long lastAutoRunTime = -cooldownTime; // Prevents cooldown on boot
int64_t cooldownUntilUnix = 0;

// ===== DRY DELAY PROTECTION =====
unsigned long dryStartTime = 0;
const unsigned long dryDelay = 10000;

// ===== SENSOR READ INTERVAL =====
unsigned long lastReadTime = 0;
const unsigned long readInterval = 2000;

// Pump schedules use Philippine Standard Time (UTC+8). Unix/Firebase
// timestamps remain UTC, so cloud freshness checks stay portable.
const long philippineUtcOffsetSeconds = 8L * 60L * 60L;
const unsigned long clockRetryInterval = 60000UL;
unsigned long lastNtpAttempt = 0;
unsigned long lastCellularTimeAttempt = 0;
bool ntpSyncRequested = false;
const char* clockSyncState = "Waiting for network time";

// ===== FIREBASE SEND INTERVAL =====
unsigned long lastSend = 0;
unsigned long sendInterval = 10000; // send every 10 seconds
unsigned long lastUploadCycle = 0;
const unsigned long backlogUploadInterval = 2000UL;

const char* SETTINGS_NAMESPACE = "greenhouse";
const char* FIREBASE_TELEMETRY_URL =
  "https://soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app/greenhouse.json";
const char* CONTROL_COMMAND_URL =
  "https://soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app/control/command.json";
const char* CONTROL_ACK_URL =
  "https://soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app/control/ack.json";

// Explicit prototype is required by Arduino's single-file preprocessor because
// the implementation has a default argument in a later generated section.
bool connectWifiBackup(bool force);
void serviceWifiBackup();
void syncCellularTime();

void serviceClockSync() {
  if (time(nullptr) >= 1609459200) {
    if (ntpSyncRequested) clockSyncState = "Wi-Fi internet time synchronized";
    return;
  }

  unsigned long nowMs = millis();
  if (WiFi.status() == WL_CONNECTED &&
      (!ntpSyncRequested || nowMs - lastNtpAttempt >= clockRetryInterval)) {
    // configTime starts the ESP32 SNTP client without blocking the portal.
    // The stored system clock remains UTC; the portal applies UTC+8 for PHT.
    configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    ntpSyncRequested = true;
    lastNtpAttempt = nowMs;
    clockSyncState = "Syncing time through Wi-Fi";
    Serial.println("Requesting UTC time through Wi-Fi NTP...");
    return;
  }

  if (cellularActive &&
      (lastCellularTimeAttempt == 0 ||
       nowMs - lastCellularTimeAttempt >= clockRetryInterval)) {
    syncCellularTime();
  }
}

// Main orchestration only: initialize hardware, then run the non-blocking sensor/control loop.
void setup() {
  Serial.begin(115200);
  delay(1000);

  preferences.begin(SETTINGS_NAMESPACE, false);
  loadSettings();

  esp_task_wdt_config_t watchdogConfig = {
    .timeout_ms = 120000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  if (esp_task_wdt_init(&watchdogConfig) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&watchdogConfig);
  }
  esp_task_wdt_add(NULL);

  Serial.println();
  Serial.println("SMART GREENHOUSE SYSTEM STARTING...");
  Serial.println("DHT22 + Soil + Rain + Relay + Cellular/WiFi + Firebase");
  Serial.println("---------------------------------------------");

  // 1. INIT SENSORS
  dht.begin();
  pinMode(SOIL_PIN, INPUT);
  pinMode(RAIN_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);
  Serial.println("Pump relay OFF at startup.");

  // 2. RUN DIAGNOSTICS
  runDiagnostics();

  // 3. Connect Cellular FIRST (Prevents power brownout by not having WiFi AP running at same time)
  // We pass 'false' because runDiagnostics() already power cycled it 2 seconds ago!
  connectNetwork(false);

  // 4. START AP AND SERVER
  WiFi.mode(WIFI_AP);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("Greenhouse_Portal");
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  dnsServer.start(DNS_PORT, "*", apIP);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/data", HTTP_GET, handleData);
  server.on("/wifi-scan", HTTP_GET, handleWifiScan);
  server.on("/set", HTTP_POST, handleSet);

  // Common captive portal detection URLs
  server.on("/generate_204", handleNotFound);
  server.on("/gen_204", handleNotFound);
  server.on("/hotspot-detect.html", handleNotFound);
  server.on("/connecttest.txt", handleNotFound);
  server.on("/ncsi.txt", handleNotFound);

  server.onNotFound(handleNotFound); // Redirect captive portal requests
  server.begin();
  Serial.println("Captive Portal started. Connect to 'Greenhouse_Portal' now!");

  if (wifiBackupConfigured) {
    connectWifiBackup(true);
  }
}

void loop() {
  esp_task_wdt_reset();
  servicePortal();
  serviceWifiBackup();
  serviceClockSync();

  if (millis() - lastReadTime >= readInterval) {
    lastReadTime = millis();

    // ===== READ DHT22 =====
    float humidity = dht.readHumidity();
    float temperature = dht.readTemperature();

    // ===== READ SENSORS =====
    int soilRaw = readSoilAverage();
    int rainRaw = analogRead(RAIN_PIN);

    // ===== CONVERT SOIL RAW TO PERCENT =====
    int soilPercent = map(soilRaw, soilDryValue, soilWetValue, 0, 100);
    soilPercent = constrain(soilPercent, 0, 100);

    bool dhtError = isnan(humidity) || isnan(temperature);
    bool rainDetected = rainRaw < rainThreshold;

    // Update global variables for web server
    currentHumidity = humidity;
    currentTemperature = temperature;
    currentSoilRaw = soilRaw;
    currentSoilPercent = soilPercent;
    currentRainRaw = rainRaw;
    currentRainDetected = rainDetected;
    currentDhtError = dhtError;

    // ===== TIME SCHEDULING LOGIC =====
    struct tm timeinfo = {0};
    bool isScheduledTime = false;
    time_t utcNow = time(nullptr);
    if (utcNow >= 1609459200) {
      time_t philippineNow = utcNow + philippineUtcOffsetSeconds;
      gmtime_r(&philippineNow, &timeinfo);
      if ((timeinfo.tm_hour == 6 || timeinfo.tm_hour == 18) && timeinfo.tm_min < 2) {
        isScheduledTime = true;
      }
    }

    // ===== PUMP LOGIC =====
    managePumpSafety();

    if (!pumpState && !emergencyStopLatched) {
      String startReason;
      if (manualMode && manualPumpState) {
        startPumpSafely("manual", startReason);
      } else if (!manualMode && isScheduledTime) {
        startPumpSafely("scheduled", startReason);
      } else if (!manualMode) {
        if (soilPercent < pumpOnThreshold && !rainDetected &&
            !sensorFaultActive() && !cooldownActive()) {
          if (dryStartTime == 0) {
            dryStartTime = millis();
            Serial.println("Soil dry and safe. Dry timer started...");
          } else if (millis() - dryStartTime >= dryDelay) {
            startPumpSafely("automatic", startReason);
            dryStartTime = 0;
          }
        } else {
          dryStartTime = 0;
        }
      }
    }

    // Re-check after mode logic; every mode uses the same safety cutoff.
    managePumpSafety();

    digitalWrite(RELAY_PIN, pumpState ? RELAY_ON : RELAY_OFF);

    // ===== SERIAL MONITOR DISPLAY =====
    Serial.println();
    Serial.println("========== GREENHOUSE DATA ==========");
    Serial.print("Network: ");
    Serial.println(cellularActive ? "Cellular (A7670E)" : "Disconnected");

    if (dhtError) {
      Serial.println("DHT22: ERROR - Check wiring or sensor.");
    } else {
      Serial.print("Temperature: ");
      Serial.print(temperature);
      Serial.println(" C");

      Serial.print("Humidity: ");
      Serial.print(humidity);
      Serial.println(" %");
    }

    Serial.print("Soil Raw: ");
    Serial.print(soilRaw);
    Serial.print(" | Soil Moisture: ");
    Serial.print(soilPercent);
    Serial.println(" %");

    if (soilPercent < 15) {
      Serial.println("Soil Status: DRY");
    } else if (soilPercent < 55) {
      Serial.println("Soil Status: MOIST");
    } else {
      Serial.println("Soil Status: WET");
    }

    Serial.print("Rain Raw: ");
    Serial.print(rainRaw);
    Serial.print(" | Rain Status: ");
    Serial.println(rainDetected ? "RAIN DETECTED" : "NO RAIN");

    Serial.print("Pump Relay: ");
    Serial.println(pumpState ? "ON" : "OFF");

    bool isCooling = cooldownActive();
    Serial.print("Cooldown: ");
    if (isCooling) {
      unsigned long remaining = cooldownRemainingSeconds();
      Serial.print("ACTIVE (");
      Serial.print(remaining / 60);
      Serial.println(" mins left)");
    } else {
      Serial.println("INACTIVE");
    }

    if (dryStartTime > 0 && !pumpState) {
      Serial.print("Dry timer: ");
      Serial.print((millis() - dryStartTime) / 1000);
      Serial.println(" seconds");
    }

    // Capture independently from upload attempts so a backlog can drain faster
    // than the ten-second sensor sampling interval after connectivity returns.
    if (millis() - lastSend >= sendInterval) {
      lastSend = millis();
      enqueueTelemetry(temperature, humidity, dhtError, soilRaw, soilPercent,
                       rainRaw, rainDetected, pumpState);
    }

    unsigned long uploadInterval = telemetryQueueCount > 1 ? backlogUploadInterval : sendInterval;
    if (telemetryQueueCount > 0 && millis() - lastUploadCycle >= uploadInterval) {
      lastUploadCycle = millis();
      bool cloudRetryDue = !cloudAttempted ||
                           (millis() - lastCloudAttempt >= cloudRetryInterval);
      bool wifiConnected = WiFi.status() == WL_CONNECTED;

      if (wifiConnected && wifiBackupConfigured) {
        preferWifiAndSuspendCellular();
      } else if (!wifiConnected && cellularSuspendedForWifi) {
        restoreCellularAfterWifiLoss();
      }

      if (!cloudAvailable && wifiBackupConfigured && !wifiConnected) {
        wifiConnected = connectWifiBackup(false);
      }
      if (wifiConnected && wifiBackupConfigured) preferWifiAndSuspendCellular();

      bool modemConnected = cellularActive && modem.isNetworkConnected();
      cloudRetryDue = !cloudAttempted ||
                      (millis() - lastCloudAttempt >= cloudRetryInterval);

      bool cellularRetryDue = lastCellularReconnectAttempt == 0 ||
                              millis() - lastCellularReconnectAttempt >= cellularRetryInterval;
      if (!modemConnected && !wifiConnected && cellularRetryDue) {
        Serial.println("Cellular disconnected. Retrying after backoff...");
        lastCellularReconnectAttempt = millis();
        connectNetwork(true);
        modemConnected = cellularActive && modem.isNetworkConnected();
        cloudRetryDue = !cloudAttempted ||
                        (millis() - lastCloudAttempt >= cloudRetryInterval);
      }

      if (telemetryQueueCount > 0 &&
          ((wifiBackupActive && wifiConnected && (cloudAvailable || cloudRetryDue)) ||
           (!wifiBackupActive && modemConnected && (cloudAvailable || cloudRetryDue)))) {
        lastCloudAttempt = millis();
        cloudAttempted = true;
        TelemetryRecord &record = telemetryQueue[telemetryQueueHead];
        if (sendToFirebase(record.temperature, record.humidity, record.dhtError,
                           record.soilRaw, record.soilPercent, record.rainRaw,
                           record.rainDetected, record.pumpOn,
                           record.capturedUptime, record.capturedUnix)) {
          removeQueuedTelemetry();
        }
      } else if (!cloudAvailable) {
        unsigned long elapsed = millis() - lastCloudAttempt;
        unsigned long waitMs = elapsed < cloudRetryInterval ? cloudRetryInterval - elapsed : 0;
        Serial.print("Cloud unavailable; local portal remains active. Retry in ");
        Serial.print(waitMs / 1000UL);
        Serial.println(" seconds.");
      }
    }

    Serial.println("=====================================");

  } // End of readInterval block
}

// ============================================================================

// ============================================================================
// 2. PUMP SAFETY, PERSISTENCE, AND ROUTE SELECTION
// ============================================================================

// SafetyAndSettings.ino
// Owns the rules that may start/stop the pump and the state that must survive power loss.

bool sensorFaultActive() {
  // This sensor legitimately reaches the ESP32 ADC ceiling when the soil is
  // fully dry. Zero remains a wiring/short-circuit fault; 4095 is valid dry.
  return currentDhtError || currentSoilRaw <= 0;
}

bool clockIsValid() {
  return time(nullptr) >= 1609459200;
}

void persistCooldown() {
  time_t now = time(nullptr);
  if (clockIsValid()) {
    cooldownUntilUnix = (int64_t)now + (cooldownTime / 1000UL);
  } else {
    // -1 means a cooldown was started before a trustworthy clock was ready.
    // On time synchronization, conservatively start the full cooldown.
    cooldownUntilUnix = -1;
  }
  preferences.putLong64("cooldownUntil", cooldownUntilUnix);
}

void clearCooldown() {
  cooldownUntilUnix = 0;
  lastAutoRunTime = -cooldownTime;
  preferences.putLong64("cooldownUntil", 0);
}

bool cooldownActive() {
  if (cooldownUntilUnix != 0) {
    if (cooldownUntilUnix < 0) {
      // The cooldown began before a trustworthy clock was available. Count
      // down with millis() instead of returning a fresh five hours each time.
      unsigned long elapsedMs = millis() - lastAutoRunTime;
      if (elapsedMs >= cooldownTime) {
        clearCooldown();
        return false;
      }

      // When cellular time later becomes valid, preserve the elapsed portion
      // and persist only the actual remaining duration.
      if (clockIsValid()) {
        unsigned long remainingMs = cooldownTime - elapsedMs;
        cooldownUntilUnix = (int64_t)time(nullptr) +
                            (int64_t)((remainingMs + 999UL) / 1000UL);
        preferences.putLong64("cooldownUntil", cooldownUntilUnix);
      }
      return true;
    }
    if (!clockIsValid()) return true;
    if ((int64_t)time(nullptr) < cooldownUntilUnix) return true;
    clearCooldown();
    return false;
  }
  return (millis() - lastAutoRunTime) < cooldownTime;
}

unsigned long cooldownRemainingSeconds() {
  if (!cooldownActive()) return 0;
  if (cooldownUntilUnix > 0 && clockIsValid()) {
    int64_t remaining = cooldownUntilUnix - (int64_t)time(nullptr);
    return remaining > 0 ? (unsigned long)remaining : 0;
  }
  if (cooldownUntilUnix < 0) {
    unsigned long elapsedMs = millis() - lastAutoRunTime;
    if (elapsedMs >= cooldownTime) return 0;
    return (cooldownTime - elapsedMs + 999UL) / 1000UL;
  }
  // A persisted absolute deadline cannot be evaluated until time returns.
  if (cooldownUntilUnix > 0) return cooldownTime / 1000UL;
  return (cooldownTime - (millis() - lastAutoRunTime)) / 1000UL;
}

void stopPump(const String &reason, bool startCooldown = true) {
  bool wasOn = pumpState;
  pumpState = false;
  manualPumpState = false;
  pumpOnTime = 0;
  dryStartTime = 0;
  pumpStopReason = reason;
  digitalWrite(RELAY_PIN, RELAY_OFF);

  if (wasOn && startCooldown) {
    lastAutoRunTime = millis();
    persistCooldown();
  }
  if (wasOn) preferences.putBool("pumpWasOn", false);
  if (wasOn) {
    Serial.print("Pump stopped: ");
    Serial.println(reason);
  }
}

bool startPumpSafely(const String &source, String &reason) {
  if (emergencyStopLatched) reason = "emergency_stop_latched";
  else if (currentRainDetected) reason = "rain_detected";
  else if (sensorFaultActive()) reason = "critical_sensor_fault";
  else if (cooldownActive()) reason = "cooldown_active";
  else {
    pumpState = true;
    manualPumpState = true;
    pumpOnTime = millis();
    if (pumpOnTime == 0) pumpOnTime = 1;
    pumpStopReason = "";
    preferences.putBool("pumpWasOn", true);
    Serial.print("Pump started safely: ");
    Serial.println(source);
    return true;
  }

  stopPump(reason, false);
  return false;
}

bool validThresholds(int onThreshold, int offThreshold) {
  return onThreshold >= 0 && offThreshold <= 100 && onThreshold < offThreshold;
}

bool parseUnsignedInt(const String &value, int &result) {
  if (value.length() == 0) return false;
  for (size_t i = 0; i < value.length(); i++) {
    if (!isDigit(value[i])) return false;
  }
  long parsed = value.toInt();
  if (parsed > 32767) return false;
  result = (int)parsed;
  return true;
}

bool validCalibration(int dryValue, int wetValue, int rainValue) {
  return wetValue > 0 && dryValue <= 4095 && wetValue < dryValue &&
         rainValue > 0 && rainValue < 4095;
}

void saveSettings() {
  preferences.putInt("soilDry", soilDryValue);
  preferences.putInt("soilWet", soilWetValue);
  preferences.putInt("rain", rainThreshold);
  preferences.putInt("pumpOn", pumpOnThreshold);
  preferences.putInt("pumpOff", pumpOffThreshold);
}

void loadSettings() {
  int savedDry = preferences.getInt("soilDry", soilDryValue);
  int savedWet = preferences.getInt("soilWet", soilWetValue);
  int savedRain = preferences.getInt("rain", rainThreshold);
  // Migrate the old factory calibration to this sensor's observed dry limit.
  if (savedDry == 1600 && savedWet == 500) savedDry = 4095;
  if (validCalibration(savedDry, savedWet, savedRain)) {
    soilDryValue = savedDry;
    soilWetValue = savedWet;
    rainThreshold = savedRain;
  }
  int savedOn = preferences.getInt("pumpOn", pumpOnThreshold);
  int savedOff = preferences.getInt("pumpOff", pumpOffThreshold);
  if (validThresholds(savedOn, savedOff)) {
    pumpOnThreshold = savedOn;
    pumpOffThreshold = savedOff;
  }
  cooldownUntilUnix = preferences.getLong64("cooldownUntil", 0);
  if (preferences.getBool("pumpWasOn", false)) {
    // A reset or power loss interrupted a pump run. Require a full cooldown
    // after the clock is restored, and ensure the relay remains off.
    cooldownUntilUnix = -1;
    preferences.putLong64("cooldownUntil", cooldownUntilUnix);
    preferences.putBool("pumpWasOn", false);
  }
  if (cooldownUntilUnix < 0) {
    // After a restart there is no safe way to reconstruct pre-clock elapsed
    // time, so start one conservative full countdown from this boot.
    lastAutoRunTime = millis();
  }
  backupWifiSsid = preferences.getString("wifiSsid", "");
  backupWifiPassword = preferences.getString("wifiPass", "");
  wifiBackupConfigured = backupWifiSsid.length() > 0;
  lastProcessedCommandId = preferences.getString("lastCommand", "");
  lastProcessedIssuedAt = preferences.getLong64("lastIssued", 0);

  // Store defaults on first boot and repair invalid persisted values.
  saveSettings();
}

bool validWifiCredentials(const String &ssid, const String &password) {
  return ssid.length() >= 1 && ssid.length() <= 32 &&
         (password.length() == 0 || (password.length() >= 8 && password.length() <= 63));
}

String escapeJsonString(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); i++) {
    char c = value[i];
    if (c == '\\' || c == '"') escaped += '\\';
    if ((uint8_t)c >= 0x20) escaped += c;
  }
  return escaped;
}

void saveWifiCredentials(const String &ssid, const String &password) {
  backupWifiSsid = ssid;
  backupWifiPassword = password;
  wifiBackupConfigured = backupWifiSsid.length() > 0;
  preferences.putString("wifiSsid", backupWifiSsid);
  preferences.putString("wifiPass", backupWifiPassword);
}

bool connectWifiBackup(bool force = false) {
  if (!wifiBackupConfigured) return false;
  if (WiFi.status() == WL_CONNECTED) return true;
  if (wifiConnectionInProgress) return false;
  if (!force && lastWifiAttempt != 0 && millis() - lastWifiAttempt < wifiRetryInterval) return false;

  lastWifiAttempt = millis();
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(backupWifiSsid.c_str(), backupWifiPassword.c_str());
  wifiConnectionInProgress = true;
  wifiConnectionStarted = millis();
  uploadState = "Connecting to Wi-Fi";
  Serial.print("Connecting Wi-Fi backup: ");
  Serial.println(backupWifiSsid);
  return false;
}

void serviceWifiBackup() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiConnectionInProgress) {
      Serial.print("Wi-Fi backup connected: ");
      Serial.println(WiFi.localIP());
    }
    wifiConnectionInProgress = false;
    if (wifiBackupConfigured) preferWifiAndSuspendCellular();
    return;
  }
  if (wifiConnectionInProgress && millis() - wifiConnectionStarted >= wifiConnectTimeout) {
    wifiConnectionInProgress = false;
    uploadState = "Wi-Fi unavailable; waiting to retry";
    Serial.println("Wi-Fi backup connection timed out; portal remains active.");
  }
  if (!wifiConnectionInProgress && wifiBackupConfigured &&
      (lastWifiAttempt == 0 || millis() - lastWifiAttempt >= wifiRetryInterval)) {
    connectWifiBackup(false);
  }
}

void preferWifiAndSuspendCellular() {
  if (WiFi.status() != WL_CONNECTED) return;

  wifiBackupActive = true;
  if (!cellularSuspendedForWifi) {
    if (cellularActive) {
      Serial.println("Wi-Fi connected. Disconnecting cellular packet data...");
      modem.gprsDisconnect();
    }
    cellularActive = false;
    cellularSuspendedForWifi = true;
    cloudAvailable = false;
    cloudAttempted = false;
    Serial.println("Wi-Fi is now the primary Firebase connection.");
  }
}

void restoreCellularAfterWifiLoss() {
  if (!cellularSuspendedForWifi) return;
  cellularSuspendedForWifi = false;
  wifiBackupActive = false;
  cellularActive = false;
  cloudAvailable = false;
  cloudAttempted = false;
  Serial.println("Wi-Fi lost. Cellular fallback will reconnect.");
}



// ============================================================================
// 3. CAPTIVE-PORTAL PAGE
// ============================================================================

// Static portal asset stored in flash to avoid consuming ESP32 heap on every request.

const char PORTAL_HTML[] PROGMEM = R"PORTAL(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#063c32"><title>Greenhouse Control</title><style>
:root{color-scheme:dark;--bg:#071b18;--panel:#0d2924;--line:#245046;--mint:#4ade80;--text:#eefbf5;--muted:#9fc5b7;--red:#ef4444;--amber:#f59e0b;--blue:#3b82f6}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at top,#12463b 0,var(--bg) 45%);color:var(--text);font:15px system-ui,sans-serif;min-height:100vh}.wrap{width:min(920px,100%);margin:auto;padding:24px 16px 48px}header{display:flex;justify-content:space-between;gap:16px;align-items:center;margin-bottom:18px}h1{font-size:clamp(22px,5vw,34px);margin:0}h2{font-size:17px;margin:28px 0 12px}.eyebrow{color:var(--mint);font-size:12px;font-weight:800;letter-spacing:.12em;text-transform:uppercase}.online{padding:8px 12px;border:1px solid var(--line);border-radius:999px;color:var(--muted)}.online.ok{color:var(--mint)}.online.bad{color:#fca5a5}.alert{display:none;padding:14px;border-radius:12px;background:#4b1616;border:1px solid #8e2d2d;margin-bottom:14px}.alert.show{display:block}.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}.card,.panel{background:var(--panel);background:color-mix(in srgb,var(--panel) 94%,white 6%);border:1px solid var(--line);border-radius:16px;box-shadow:0 12px 30px #0004}.card{padding:16px}.label{display:block;color:var(--muted);font-size:12px;margin-bottom:8px}.value{font-weight:800;font-size:20px}.panel{padding:18px}.buttons{display:grid;grid-template-columns:repeat(5,1fr);gap:9px}button{border:0;border-radius:12px;padding:13px 10px;color:white;background:#215f51;font-weight:750;cursor:pointer}button:disabled{opacity:.5;cursor:wait}.on{background:#16733c}.off,.emergency{background:#a52a2a}.emergency{outline:2px solid #f87171}.reset{background:#9a5b08}.form{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}.field label{display:block;color:var(--muted);font-size:12px;margin-bottom:6px}.field input,.field select{width:100%;border:1px solid var(--line);border-radius:10px;background:#071b18;color:var(--text);padding:11px;font:inherit}.wifi-picker{display:grid;grid-template-columns:1fr auto;gap:10px;margin-bottom:12px}.scan{min-width:145px;background:#284b67}.save{margin-top:14px;width:100%;background:#176b5a}.message{min-height:22px;margin:12px 0 0;color:var(--muted)}.message.error{color:#fca5a5}.hint{color:var(--muted);font-size:12px;line-height:1.5}.health{display:grid;grid-template-columns:repeat(4,1fr);gap:9px}.health div{padding:11px;border:1px solid var(--line);border-radius:11px;background:#071b18}.health b,.health span{display:block}.health span{color:var(--muted);font-size:11px}.statusline{display:flex;gap:8px;flex-wrap:wrap;margin:12px 0}.pill{border:1px solid var(--line);border-radius:999px;padding:7px 10px;color:var(--muted)}.pill.good{color:var(--mint)}.pill.warn{color:var(--amber)}@media(max-width:700px){.grid{grid-template-columns:repeat(2,1fr)}.buttons{grid-template-columns:repeat(2,1fr)}.emergency{grid-column:1/-1}.form{grid-template-columns:1fr}.health{grid-template-columns:repeat(2,1fr)}.wifi-picker{grid-template-columns:1fr}.scan{width:100%}header{align-items:flex-start;flex-direction:column}}
</style></head><body><main class="wrap"><header><div><div class="eyebrow">ESP32 local access</div><h1>Greenhouse Control</h1></div><div id="connection" class="online">Connecting...</div></header>
<div id="safetyAlert" class="alert" role="alert"></div><section class="grid" aria-label="Live readings">
<div class="card"><span class="label">Soil moisture</span><span id="soil" class="value">--</span></div><div class="card"><span class="label">Temperature</span><span id="temp" class="value">--</span></div><div class="card"><span class="label">Humidity</span><span id="hum" class="value">--</span></div><div class="card"><span class="label">Rain</span><span id="rain" class="value">--</span></div><div class="card"><span class="label">Pump</span><span id="pump" class="value">--</span></div><div class="card"><span class="label">Mode</span><span id="mode" class="value">--</span></div><div class="card"><span class="label">Cooldown</span><span id="cooldown" class="value">--</span></div><div class="card"><span class="label">Cloud route</span><span id="network" class="value">--</span></div><div class="card"><span class="label">Philippine time</span><span id="clock" class="value">--</span></div><div class="card"><span class="label">Next schedule</span><span id="nextSchedule" class="value">--</span></div></section>
<div class="statusline"><span id="uploadPill" class="pill">Upload waiting</span><span id="queuePill" class="pill">Queue --</span><span id="wifiPill" class="pill">Wi-Fi --</span></div>
<h2>Reliability health</h2><section class="panel health"><div><span>Upload state</span><b id="uploadState">--</b></div><div><span>Queued records</span><b id="queueHealth">--</b></div><div><span>Free / minimum heap</span><b id="heapHealth">--</b></div><div><span>Uptime / reset</span><b id="deviceHealth">--</b></div></section>
<h2>Pump control</h2><section class="panel"><div class="buttons"><button data-mode="auto">Auto / Resume</button><button class="on" data-mode="manual" data-state="on">Pump ON</button><button class="off" data-mode="manual" data-state="off">Pump OFF</button><button class="reset" data-mode="reset_cd">Reset cooldown</button><button class="emergency" data-mode="emergency_off">Emergency OFF</button></div><p id="message" class="message" aria-live="polite"></p><p class="hint">All starts are still subject to rain, sensor, cooldown and two-minute safety protection. Auto / Resume clears a latched emergency stop.</p></section>
<h2>Automation thresholds</h2><section class="panel"><div class="form"><div class="field"><label for="onThreshold">Turn pump on below (%)</label><input id="onThreshold" type="number" min="0" max="99"></div><div class="field"><label for="offThreshold">Turn pump off at (%)</label><input id="offThreshold" type="number" min="1" max="100"></div></div><button id="saveThresholds" class="save">Save thresholds</button><p class="hint">Required: 0 <= ON threshold < OFF threshold <= 100.</p></section>
<h2>Sensor calibration</h2><section class="panel"><div class="form"><div class="field"><label for="soilWet">Soil wet raw value</label><input id="soilWet" type="number" min="1" max="4094"></div><div class="field"><label for="soilDry">Soil dry raw value</label><input id="soilDry" type="number" min="2" max="4095"></div><div class="field"><label for="rainThreshold">Rain threshold</label><input id="rainThreshold" type="number" min="1" max="4094"></div></div><button id="saveCalibration" class="save">Save calibration</button><p class="hint">4095 is accepted as a valid fully-dry reading. Required: soil wet < soil dry.</p></section>
<h2>Wi-Fi cloud backup</h2><section class="panel"><div class="wifi-picker"><div class="field"><label for="wifiNetworks">Nearby 2.4 GHz networks</label><select id="wifiNetworks"><option value="">Scan to find Wi-Fi networks</option></select></div><button id="scanWifi" class="scan" type="button">Scan networks</button></div><div class="form"><div class="field"><label for="wifiSsid">Selected Wi-Fi name (SSID)</label><input id="wifiSsid" maxlength="32" autocomplete="off"></div><div class="field"><label for="wifiPassword">Wi-Fi password</label><input id="wifiPassword" type="password" minlength="8" maxlength="63" autocomplete="new-password" placeholder="Leave blank to keep saved password"></div></div><button id="saveWifi" class="save">Save and connect Wi-Fi</button><p id="wifiStatus" class="hint">Not configured</p><p class="hint">ESP32 supports 2.4 GHz Wi-Fi only. Choose a scanned network or enter a hidden SSID manually. Greenhouse_Portal stays available while connecting.</p></section></main>
<script>
var LOCAL_ORIGIN='http://192.168.4.1',busy=false,wifiSelectionDirty=false,refreshing=false,cooldownEnd=0,scheduleEnd=0,nextScheduleLabel='',lastLive=0;function $(id){return document.getElementById(id)}function editable(id){return document.activeElement!==$(id)}function text(id,value){$(id).textContent=value}function setMessage(value,error){text('message',value);$('message').className=error?'message error':'message'}function setBusy(value){busy=value;var buttons=document.getElementsByTagName('button');for(var i=0;i<buttons.length;i++)buttons[i].disabled=value}
function encode(params){var parts=[];for(var key in params)if(params.hasOwnProperty(key))parts.push(encodeURIComponent(key)+'='+encodeURIComponent(params[key]));return parts.join('&')}
function request(path,method,body,done){var xhr=new XMLHttpRequest(),finished=false;function finish(error,response){if(finished)return;finished=true;done(error,response)}xhr.open(method||'GET',LOCAL_ORIGIN+path,true);xhr.timeout=path==='/wifi-scan'?18000:6000;xhr.setRequestHeader('Cache-Control','no-cache');if(method==='POST')xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded');xhr.onreadystatechange=function(){if(xhr.readyState===4)finish(xhr.status>=200&&xhr.status<300?null:(xhr.responseText||'HTTP '+xhr.status),xhr.responseText)};xhr.ontimeout=function(){finish('Local ESP32 request timed out','')};xhr.onerror=function(){finish('Cannot reach 192.168.4.1','')};xhr.send(body||null)}
function duration(seconds){seconds=Math.max(0,Math.ceil(Number(seconds)||0));var h=Math.floor(seconds/3600),m=Math.floor(seconds%3600/60),s=seconds%60;return h+'h '+('0'+m).slice(-2)+'m '+('0'+s).slice(-2)+'s'}
function refresh(){if(refreshing)return;refreshing=true;request('/data','GET','',function(error,body){refreshing=false;if(error){text('connection','Offline');$('connection').className='online bad';return}try{var d=JSON.parse(body),remaining=Math.max(0,Number(d.cooldownRemainingSeconds)||0);lastLive=Date.now();cooldownEnd=d.cooldown?Date.now()+remaining*1000:0;scheduleEnd=d.clockValid?Date.now()+Math.max(0,Number(d.nextScheduleSeconds)||0)*1000:0;nextScheduleLabel=d.nextSchedule||'';text('soil',d.soilPercent+'% ('+d.soilRaw+')');text('temp',d.dhtError?'Sensor error':d.temperature+' C');text('hum',d.dhtError?'Sensor error':d.humidity+'%');text('rain',d.rainDetected?'Detected':'Clear');text('pump',d.pumpState?'ON':'OFF');text('mode',d.emergencyStop?'EMERGENCY':(d.manualMode?'MANUAL':'AUTO'));text('network',d.network);text('clock',d.clockValid?d.philippineTime:d.clockSyncState);text('nextSchedule',d.clockValid?duration(d.nextScheduleSeconds)+' to '+nextScheduleLabel:d.nextSchedule);text('wifiStatus',d.wifiConnecting?'Connecting to '+d.wifiSsid:(d.wifiConnected?'Connected to '+d.wifiSsid:(d.wifiConfigured?'Saved: '+d.wifiSsid+' (waiting)':'Not configured')));text('uploadState',d.uploadState);text('queueHealth',d.queueDepth+' / '+d.queueCapacity+(d.droppedRecords?' · '+d.droppedRecords+' replaced':''));text('heapHealth',Math.round(d.freeHeap/1024)+' KB / '+Math.round(d.minimumFreeHeap/1024)+' KB');text('deviceHealth',Math.floor(d.uptimeSeconds/3600)+'h · reset '+d.resetReason);text('uploadPill',d.cloudAvailable?'Firebase online':d.uploadState);$('uploadPill').className='pill '+(d.cloudAvailable?'good':'warn');text('queuePill','Queue '+d.queueDepth+'/'+d.queueCapacity);$('queuePill').className='pill '+(d.queueDepth>=d.queueCapacity?'warn':'good');text('wifiPill',d.wifiConnected?'Wi-Fi '+d.wifiRssi+' dBm':(d.wifiConnecting?'Wi-Fi connecting':'Wi-Fi offline'));$('wifiPill').className='pill '+(d.wifiConnected?'good':'warn');text('connection',d.cloudAvailable?'Live + cloud':'Live local');$('connection').className='online '+(d.cloudAvailable?'ok':'');var fault=d.emergencyStop||d.rainDetected||d.dhtError||d.soilFault;$('safetyAlert').className=fault?'alert show':'alert';text('safetyAlert',d.emergencyStop?'Emergency stop is latched.':d.rainDetected?'Rain detected; pump starts are blocked.':d.dhtError?'DHT22 fault; pump starts are blocked.':d.soilFault?'Soil sensor fault; pump starts are blocked.':'');if(editable('onThreshold'))$('onThreshold').value=d.onThresh;if(editable('offThreshold'))$('offThreshold').value=d.offThresh;if(editable('soilWet'))$('soilWet').value=d.soilWet;if(editable('soilDry'))$('soilDry').value=d.soilDry;if(editable('rainThreshold'))$('rainThreshold').value=d.rainThreshold;if(!wifiSelectionDirty&&editable('wifiSsid'))$('wifiSsid').value=d.wifiSsid||''}catch(parseError){setMessage('Invalid local sensor response',true)}})}
function update(params,confirmText){if(busy)return;if(confirmText&&!confirm(confirmText))return;setBusy(true);setMessage('Applying...',false);request('/set','POST',encode(params),function(error){setBusy(false);if(error){setMessage(error,true);return}setMessage('Saved and applied by ESP32.',false);refresh()})}
var modeButtons=document.querySelectorAll('[data-mode]');for(var i=0;i<modeButtons.length;i++)modeButtons[i].onclick=function(){var mode=this.getAttribute('data-mode'),state=this.getAttribute('data-state')||'',question=mode==='emergency_off'?'Stop the pump and latch emergency mode?':mode==='reset_cd'?'Reset the five-hour safety cooldown?':'';update({mode:mode,state:state},question)};
$('saveThresholds').onclick=function(){var on=Number($('onThreshold').value),off=Number($('offThreshold').value);if(Math.floor(on)!==on||Math.floor(off)!==off||on<0||off>100||on>=off){setMessage('Thresholds must satisfy 0 <= ON < OFF <= 100.',true);return}update({on_thresh:on,off_thresh:off},'')};
$('saveCalibration').onclick=function(){var wet=Number($('soilWet').value),dry=Number($('soilDry').value),rain=Number($('rainThreshold').value);if(Math.floor(wet)!==wet||Math.floor(dry)!==dry||Math.floor(rain)!==rain||wet<=0||wet>=dry||dry>4095||rain<=0||rain>=4095){setMessage('Calibration values are invalid.',true);return}update({soil_wet:wet,soil_dry:dry,rain_threshold:rain},'')};
function scanWifi(){if(busy)return;$('scanWifi').disabled=true;text('scanWifi','Scanning 2.4 GHz...');request('/wifi-scan','GET','',function(error,body){if(error){$('scanWifi').disabled=false;text('scanWifi','Scan networks');setMessage(error,true);return}try{var result=JSON.parse(body),select=$('wifiNetworks');while(select.options.length)select.remove(0);if(!result.networks||!result.networks.length){var empty=document.createElement('option');empty.value='';empty.text='No 2.4 GHz networks found';select.add(empty);setMessage(result.error?'Wi-Fi scan failed (code '+result.error+'). Check Serial Monitor.':'No networks found. Move closer and scan again.',true)}else{var prompt=document.createElement('option');prompt.value='';prompt.text='Select a network...';select.add(prompt);for(var i=0;i<result.networks.length;i++){var network=result.networks[i],option=document.createElement('option'),quality=network.rssi>=-55?'Strong':network.rssi>=-70?'Good':'Weak';option.value=network.ssid;option.text=network.ssid+' - '+quality+' ('+network.rssi+' dBm) '+(network.secure?'[Locked]':'[Open]');select.add(option)}select.selectedIndex=0;setMessage(result.networks.length+' network(s) found. Choose one below.',false)}}catch(parseError){setMessage('Invalid Wi-Fi scan response',true)}$('scanWifi').disabled=false;text('scanWifi','Scan again')})}
$('wifiNetworks').onchange=function(){if(this.value){wifiSelectionDirty=true;$('wifiSsid').value=this.value}};$('wifiSsid').oninput=function(){wifiSelectionDirty=true};$('scanWifi').onclick=scanWifi;
$('saveWifi').onclick=function(){var ssid=$('wifiSsid').value.replace(/^\s+|\s+$/g,''),password=$('wifiPassword').value;if(!ssid||ssid.length>32){setMessage('Enter a valid Wi-Fi name.',true);return}if(password&&password.length<8){setMessage('Wi-Fi password must be at least 8 characters.',true);return}update({wifi_ssid:ssid,wifi_password:password},'');$('wifiPassword').value=''};
function tick(){text('cooldown',cooldownEnd?duration((cooldownEnd-Date.now())/1000):'Ready');if(scheduleEnd)text('nextSchedule',duration((scheduleEnd-Date.now())/1000)+' to '+nextScheduleLabel);if(lastLive&&Date.now()-lastLive>7000){text('connection','Connection stale');$('connection').className='online bad'}}refresh();setInterval(refresh,2000);setInterval(tick,1000);
</script></body></html>
)PORTAL";



// ============================================================================
// 4. CAPTIVE-PORTAL SERVER AND VALIDATION
// ============================================================================

// Portal.ino
// Serves local live data and validated setup actions; cloud access is never required here.

// ===== HELPER FUNCTIONS TO KEEP PORTAL ALIVE =====
void servicePortal() {
  esp_task_wdt_reset();
  dnsServer.processNextRequest();
  server.handleClient();
}

void managePumpSafety() {
  if (!pumpState) return;
  if (emergencyStopLatched) stopPump("emergency_stop");
  else if (currentRainDetected) stopPump("rain_detected");
  else if (sensorFaultActive()) stopPump("critical_sensor_fault");
  else if (pumpOnTime == 0 || (millis() - pumpOnTime) >= maxRunTime) {
    stopPump("two_minute_limit");
  }
}

void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    servicePortal();
    managePumpSafety();
    delay(1);
  }
}
void handleRoot() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send_P(200, "text/html; charset=utf-8", PORTAL_HTML);
}

void getPhilippineScheduleStatus(String &currentTime, String &nextSchedule,
                                 unsigned long &nextScheduleSeconds) {
  time_t utcNow = time(nullptr);
  if (utcNow < 1609459200) {
    currentTime = "Waiting for network time";
    nextSchedule = "Clock not synchronized";
    nextScheduleSeconds = 0;
    return;
  }

  time_t philippineNow = utcNow + philippineUtcOffsetSeconds;
  struct tm localTime = {0};
  gmtime_r(&philippineNow, &localTime);

  char timeBuffer[30];
  snprintf(timeBuffer, sizeof(timeBuffer), "%04d-%02d-%02d %02d:%02d:%02d PHT",
           localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday,
           localTime.tm_hour, localTime.tm_min, localTime.tm_sec);
  currentTime = timeBuffer;

  unsigned long secondsToday = (unsigned long)localTime.tm_hour * 3600UL +
                               (unsigned long)localTime.tm_min * 60UL +
                               (unsigned long)localTime.tm_sec;
  unsigned long targetSeconds;
  if (secondsToday < 6UL * 3600UL) {
    targetSeconds = 6UL * 3600UL;
    nextSchedule = "6:00 AM";
  } else if (secondsToday < 18UL * 3600UL) {
    targetSeconds = 18UL * 3600UL;
    nextSchedule = "6:00 PM";
  } else {
    targetSeconds = 30UL * 3600UL;
    nextSchedule = "6:00 AM";
  }
  nextScheduleSeconds = targetSeconds - secondsToday;
}

void handleData() {
  String philippineTime;
  String nextSchedule;
  unsigned long nextScheduleSeconds = 0;
  getPhilippineScheduleStatus(philippineTime, nextSchedule, nextScheduleSeconds);
  String json = "{";
  String localNetworkStatus = wifiBackupActive && WiFi.status() == WL_CONNECTED ?
                              (cloudAvailable ? "Wi-Fi + cloud" : "Wi-Fi; cloud unavailable") :
                              (!cellularActive ? "Cellular disconnected" :
                              (cloudAvailable ? "Cellular + cloud" : "Cellular; cloud unavailable"));
  json += "\"network\":\"" + localNetworkStatus + "\",";
  json += "\"cloudAvailable\":" + String(cloudAvailable ? "true" : "false") + ",";
  json += "\"lastUploadSucceeded\":" + String(lastUploadSucceeded ? "true" : "false") + ",";
  json += "\"secondsSinceUploadSuccess\":" + String(lastUploadSuccessMs == 0 ? -1L : (long)((millis() - lastUploadSuccessMs) / 1000UL)) + ",";
  json += "\"uploadFailures\":" + String(consecutiveUploadFailures) + ",";
  json += "\"uploadState\":\"" + String(uploadState) + "\",";
  json += "\"queueDepth\":" + String(telemetryQueueCount) + ",";
  json += "\"queueCapacity\":" + String(TELEMETRY_QUEUE_SIZE) + ",";
  json += "\"droppedRecords\":" + String(droppedTelemetryRecords) + ",";
  json += "\"uptimeSeconds\":" + String(millis() / 1000UL) + ",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"minimumFreeHeap\":" + String(ESP.getMinFreeHeap()) + ",";
  json += "\"resetReason\":" + String((int)esp_reset_reason()) + ",";
  json += "\"wifiRssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0) + ",";
  json += "\"wifiConnecting\":" + String(wifiConnectionInProgress ? "true" : "false") + ",";
  json += "\"clockValid\":" + String(clockIsValid() ? "true" : "false") + ",";
  json += "\"clockSyncState\":\"" + String(clockSyncState) + "\",";
  json += "\"philippineTime\":\"" + philippineTime + "\",";
  json += "\"nextSchedule\":\"" + nextSchedule + "\",";
  json += "\"nextScheduleSeconds\":" + String(nextScheduleSeconds) + ",";
  json += "\"wifiConfigured\":" + String(wifiBackupConfigured ? "true" : "false") + ",";
  json += "\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"wifiSsid\":\"" + escapeJsonString(backupWifiSsid) + "\",";
  if (currentDhtError) {
    json += "\"temperature\":null,\"humidity\":null,";
  } else {
    json += "\"temperature\":" + String(currentTemperature, 2) + ",";
    json += "\"humidity\":" + String(currentHumidity, 2) + ",";
  }
  json += "\"soilPercent\":" + String(currentSoilPercent) + ",";
  json += "\"soilRaw\":" + String(currentSoilRaw) + ",";
  json += "\"soilFault\":" + String(currentSoilRaw <= 0 ? "true" : "false") + ",";
  json += "\"rainDetected\":" + String(currentRainDetected ? "true" : "false") + ",";
  json += "\"dhtError\":" + String(currentDhtError ? "true" : "false") + ",";
  json += "\"pumpState\":" + String(pumpState ? "true" : "false") + ",";
  json += "\"manualMode\":" + String(manualMode ? "true" : "false") + ",";
  json += "\"emergencyStop\":" + String(emergencyStopLatched ? "true" : "false") + ",";
  json += "\"stopReason\":\"" + pumpStopReason + "\",";
  json += "\"onThresh\":" + String(pumpOnThreshold) + ",";
  json += "\"offThresh\":" + String(pumpOffThreshold) + ",";
  json += "\"soilDry\":" + String(soilDryValue) + ",";
  json += "\"soilWet\":" + String(soilWetValue) + ",";
  json += "\"rainThreshold\":" + String(rainThreshold) + ",";
  json += "\"cooldown\":" + String(cooldownActive() ? "true" : "false") + ",";
  json += "\"cooldownRemainingSeconds\":" + String(cooldownRemainingSeconds());
  json += "}";
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Connection", "close");
  server.send(200, "application/json", json);
}

void handleWifiScan() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  bool stationConnected = WiFi.status() == WL_CONNECTED;
  // Clear a failed/pending station attempt before setup scans, but never tear
  // down a working Wi-Fi cloud connection just to refresh the network list.
  if (!stationConnected) {
    WiFi.disconnect(false, false);
    delay(150);
  }
  WiFi.scanDelete();
  managePumpSafety();
  int scanStatus = WiFi.scanNetworks(false, true);
  managePumpSafety();

  if (scanStatus < 0) {
    Serial.print("Wi-Fi scan failed, retrying. Code: ");
    Serial.println(scanStatus);
    WiFi.scanDelete();
    delay(250);
    scanStatus = WiFi.scanNetworks(false, true);
    managePumpSafety();
  }

  Serial.print("Wi-Fi scan result count/code: ");
  Serial.println(scanStatus);

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Connection", "close");

  String json = "{\"scanning\":false,\"error\":" + String(scanStatus < 0 ? scanStatus : 0) + ",\"networks\":[";
  int added = 0;
  for (int i = 0; i < scanStatus && added < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;

    bool duplicate = false;
    for (int previous = 0; previous < i; previous++) {
      if (WiFi.SSID(previous) == ssid) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;

    if (added > 0) json += ",";
    json += "{\"ssid\":\"" + escapeJsonString(ssid) + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
    json += "\"secure\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
    added++;
  }
  json += "]}";
  WiFi.scanDelete();
  server.send(200, "application/json", json);
}

void handleSet() {
  String response = "OK";
  int statusCode = 200;

  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    if (mode == "manual") {
      manualMode = true;
      if (server.hasArg("state")) {
        String state = server.arg("state");
        if (state == "on") {
          String reason;
          if (!startPumpSafely("local_manual", reason)) {
            response = "Rejected: " + reason;
            statusCode = 409;
          }
        } else if (state == "off") {
          stopPump("local_manual_off");
        } else {
          response = "Invalid manual state";
          statusCode = 400;
        }
      }
    } else if (mode == "auto") {
      manualMode = false;
      emergencyStopLatched = false;
      dryStartTime = 0; // reset auto hysteresis timers
    } else if (mode == "reset_cd") {
      clearCooldown();
      Serial.println("Cooldown forcefully reset via Captive Portal.");
    } else if (mode == "emergency_off") {
      emergencyStopLatched = true;
      manualMode = true;
      stopPump("emergency_stop");
    } else {
      response = "Invalid mode";
      statusCode = 400;
    }
  }

  if (server.hasArg("on_thresh") || server.hasArg("off_thresh")) {
    if (!server.hasArg("on_thresh") || !server.hasArg("off_thresh")) {
      response = "Both thresholds are required";
      statusCode = 400;
    } else {
      int requestedOn;
      int requestedOff;
      if (!parseUnsignedInt(server.arg("on_thresh"), requestedOn) ||
          !parseUnsignedInt(server.arg("off_thresh"), requestedOff) ||
          !validThresholds(requestedOn, requestedOff)) {
        response = "Thresholds must satisfy 0 <= ON < OFF <= 100";
        statusCode = 400;
      } else {
        pumpOnThreshold = requestedOn;
        pumpOffThreshold = requestedOff;
        saveSettings();
      }
    }
  }

  if (server.hasArg("soil_dry") || server.hasArg("soil_wet") || server.hasArg("rain_threshold")) {
    int requestedDry;
    int requestedWet;
    int requestedRain;
    if (!server.hasArg("soil_dry") || !server.hasArg("soil_wet") ||
        !server.hasArg("rain_threshold") ||
        !parseUnsignedInt(server.arg("soil_dry"), requestedDry) ||
        !parseUnsignedInt(server.arg("soil_wet"), requestedWet) ||
        !parseUnsignedInt(server.arg("rain_threshold"), requestedRain) ||
        !validCalibration(requestedDry, requestedWet, requestedRain)) {
      response = "Calibration requires 0 < soil wet < soil dry <= 4095 and 0 < rain < 4095";
      statusCode = 400;
    } else {
      soilDryValue = requestedDry;
      soilWetValue = requestedWet;
      rainThreshold = requestedRain;
      saveSettings();
    }
  }

  if (server.hasArg("wifi_ssid") || server.hasArg("wifi_password")) {
    String requestedSsid = server.arg("wifi_ssid");
    requestedSsid.trim();
    String requestedPassword = server.arg("wifi_password");
    if (requestedPassword.length() == 0 && requestedSsid == backupWifiSsid) {
      requestedPassword = backupWifiPassword;
    }

    if (!validWifiCredentials(requestedSsid, requestedPassword)) {
      response = "Wi-Fi requires an SSID and either an empty open-network password or 8-63 characters";
      statusCode = 400;
    } else {
      saveWifiCredentials(requestedSsid, requestedPassword);
      wifiBackupActive = false;
      cloudAvailable = false;
      cloudAttempted = false;
      WiFi.disconnect(false, false);
      wifiConnectionInProgress = false;
      lastWifiAttempt = 0;
      connectWifiBackup(true);
      response = "Wi-Fi saved; connection started";
    }
  }
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(statusCode, "text/plain", response);
}

void handleNotFound() {
  if (server.hostHeader() != WiFi.softAPIP().toString()) {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
    return;
  }
  handleRoot();
}



// 5. CELLULAR MODEM
// ============================================================================

// Modem.ino
// Initializes the A7670-class modem and validates cellular network time before using it.

void clearModemUart() {
  while (SerialAT.available()) {
    SerialAT.read();
  }
}

bool waitForModemAT(unsigned long timeoutMs = 60000UL) {
  Serial.print("Waiting for A7670E AT response");
  unsigned long started = millis();

  while (millis() - started < timeoutMs) {
    clearModemUart();
    SerialAT.println("AT");

    unsigned long replyStarted = millis();
    String reply = "";

    while (millis() - replyStarted < 1200UL) {
      servicePortal();
      managePumpSafety();

      while (SerialAT.available()) {
        reply += (char)SerialAT.read();
      }

      if (reply.indexOf("OK") >= 0) {
        Serial.println();
        Serial.println("A7670E is responding.");
        return true;
      }

      delay(5);
    }

    Serial.print(".");
    safeDelay(500);
  }

  Serial.println();
  Serial.println("A7670E did not respond before timeout.");
  return false;
}

void powerCycleModem() {
  Serial.println("Power cycling modem via PWR_EN pin...");
  pinMode(MODEM_PWR_PIN, OUTPUT);

  digitalWrite(MODEM_PWR_PIN, LOW);
  safeDelay(2000);

  digitalWrite(MODEM_PWR_PIN, HIGH);

  SerialAT.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  // Wait for the modem's actual AT response instead of a fixed boot delay.
  if (!waitForModemAT(60000UL)) {
    Serial.println("Warning: modem did not become responsive after power cycle.");
    return;
  }

  // AT can respond before SIM/network/HTTP services are fully initialized.
  Serial.println("Waiting for A7670E internal services...");
  safeDelay(10000);
  clearModemUart();
}

void syncCellularTime() {
  lastCellularTimeAttempt = millis();
  int year3=0, month3=0, day3=0, hour3=0, min3=0, sec3=0;
  float timezone=0;
  Serial.println("Requesting time from Cellular Network...");
  if (modem.getNetworkTime(&year3, &month3, &day3, &hour3, &min3, &sec3, &timezone)) {
    // Some modem firmware returns a two-digit year while other versions return
    // the complete year. Normalize it before constructing struct tm.
    if (year3 >= 0 && year3 < 100) year3 += 2000;

    if (year3 < 2024 || year3 > 2035 || month3 < 1 || month3 > 12 ||
        day3 < 1 || day3 > 31 || hour3 < 0 || hour3 > 23 ||
        min3 < 0 || min3 > 59 || sec3 < 0 || sec3 > 60) {
      Serial.print("Rejected invalid cellular date: ");
      Serial.print(year3);
      Serial.print("-");
      Serial.print(month3);
      Serial.print("-");
      Serial.println(day3);
      clockSyncState = "Cellular clock invalid; waiting for Wi-Fi time";
      return;
    }

    struct tm t = {0};
    t.tm_year = year3 - 1900;
    t.tm_mon = month3 - 1;
    t.tm_mday = day3;
    t.tm_hour = hour3;
    t.tm_min = min3;
    t.tm_sec = sec3;
    time_t timeSinceEpoch = mktime(&t);
    // Network time fields are local to the cellular timezone. ESP32 starts in
    // UTC, so convert that local clock back to UTC before storing Unix time.
    if (timezone >= -14.0f && timezone <= 14.0f) {
      timeSinceEpoch -= (long)(timezone * 3600.0f);
    }
    if (timeSinceEpoch < 1704067200 || timeSinceEpoch > 2082758400) {
      Serial.println("Rejected out-of-range cellular Unix timestamp.");
      clockSyncState = "Cellular clock invalid; waiting for Wi-Fi time";
      return;
    }
    struct timeval tv;
    tv.tv_sec = timeSinceEpoch;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    ntpSyncRequested = false;
    clockSyncState = "Cellular network time synchronized";
    Serial.println("Time synced via Cellular Network.");
  } else {
    clockSyncState = "Cellular time unavailable; waiting for Wi-Fi";
    Serial.println("Failed to get Cellular Time.");
  }
}

void connectNetwork(bool forceRestart) {
  lastCloudAttempt = millis();
  cloudAttempted = true;
  Serial.println("Attempting to connect to Cellular Network (A7670E)...");

  if (forceRestart) {
    // Power cycle the modem to ensure a fresh state
    powerCycleModem();
    SerialAT.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
    delay(1000);
  }

  if (!modem.init()) {
    Serial.println("Failed to initialize modem! Retrying later.");
    cellularActive = false;
    cloudAvailable = false;
    return;
  }

  Serial.print("Waiting for network (up to 60s)...");
  unsigned long start = millis();
  bool netConnected = false;
  while (millis() - start < 60000L) {
    if (modem.isNetworkConnected()) {
      netConnected = true;
      break;
    }
    safeDelay(500);
    Serial.print(".");
  }

  if (!netConnected) {
    Serial.println(" fail. Retrying later.");
    cellularActive = false;
    cloudAvailable = false;
    return;
  }
  Serial.println(" success.");

  int csq = modem.getSignalQuality();
  Serial.print("Signal Quality (CSQ): ");
  Serial.println(csq);

  safeDelay(3000); // Give LTE time to stabilize

  Serial.print("Connecting to APN: ");
  Serial.print(apn);
  Serial.print(" (This can take up to 60 seconds)... ");

  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    Serial.println(" fail. Retrying later.");
    cellularActive = false;
    cloudAvailable = false;
    return;
  }

  Serial.println(" success!");
  cellularActive = true;
  cloudAvailable = false;
  cloudAttempted = false; // Probe Firebase on the next send cycle.

  // Sync RTC from cellular network
  syncCellularTime();
}

void runDiagnostics() {
  Serial.println("\n--- RUNNING STARTUP DIAGNOSTICS ---");

  // 1. Test DHT22
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  Serial.print("[TEST] DHT22 Sensor: ");
  if (isnan(h) || isnan(t)) {
    Serial.println("FAIL (Check wiring)");
  } else {
    Serial.print("PASS (Temp: ");
    Serial.print(t);
    Serial.print("C, Hum: ");
    Serial.print(h);
    Serial.println("%)");
  }

  // 2. Test Soil Moisture
  int soil = analogRead(SOIL_PIN);
  Serial.print("[TEST] Soil Moisture Sensor: ");
  if (soil == 0) {
    Serial.print("WARNING (Raw: ");
    Serial.print(soil);
    Serial.println(", might be shorted to GND)");
  } else if (soil >= 4095) {
    Serial.println("PASS (Raw: 4095 - Completely Dry / Open Air)");
  } else {
    Serial.print("PASS (Raw: ");
    Serial.print(soil);
    Serial.println(")");
  }

  // 3. Test Rain Sensor
  int rain = analogRead(RAIN_PIN);
  Serial.print("[TEST] Rain Sensor: ");
  if (rain == 0) {
    Serial.print("WARNING (Raw: ");
    Serial.print(rain);
    Serial.println(", might be shorted to GND)");
  } else if (rain >= 4095) {
    Serial.println("PASS (Raw: 4095 - Completely Dry / No Rain)");
  } else {
    Serial.print("PASS (Raw: ");
    Serial.print(rain);
    Serial.println(")");
  }

  // 4. Test SIM Module
  Serial.print("[TEST] SIM Module (A7670E): ");
  powerCycleModem();
  SerialAT.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
  safeDelay(3000);
  if (modem.testAT()) {
    String ccid = modem.getSimCCID();
    String imei = modem.getIMEI();
    if (ccid.length() > 0 && ccid != "0" && imei.length() > 0) {
      Serial.println("PASS (SIM Card Detected)");
      Serial.print("       IMEI: ");
      Serial.println(imei);
      Serial.print("       CCID: ");
      Serial.println(ccid);

      int csq = modem.getSignalQuality();
      Serial.print("       Signal Quality (CSQ): ");
      Serial.println(csq);
    } else {
      Serial.println("FAIL (Module responded, but SIM card is MISSING or locked)");
    }
  } else {
    Serial.println("FAIL (Module not responding to AT commands. Check RX/TX/Power)");
  }

  Serial.println("--- DIAGNOSTICS COMPLETE ---\n");
}

// ============================================================================
// 6. FIREBASE TELEMETRY
// ============================================================================

// Cloud.ino
// Uploads one telemetry snapshot over the active route; it never controls the pump.

// ===== SOIL AVERAGE FUNCTION =====
int readSoilAverage() {
  long total = 0;
  int samples = 20;

  for (int i = 0; i < samples; i++) {
    total += analogRead(SOIL_PIN);
    safeDelay(10);
  }

  return total / samples;
}

// ===== A7670E NATIVE HTTP(S) HELPERS =====
String sendA7670AT(const String &command,
                   unsigned long timeoutMs = 3000UL,
                   const String &requiredToken = "") {
  clearModemUart();

  Serial.print("AT CMD: ");
  Serial.println(command);
  SerialAT.println(command);

  String response = "";
  unsigned long started = millis();
  unsigned long lastByte = millis();
  bool receivedAny = false;

  while (millis() - started < timeoutMs) {
    servicePortal();
    managePumpSafety();

    while (SerialAT.available()) {
      char c = (char)SerialAT.read();
      response += c;
      receivedAny = true;
      lastByte = millis();
    }

    if (requiredToken.length() > 0 && response.indexOf(requiredToken) >= 0) {
      // Give the modem a short time to finish the same URC/response.
      if (millis() - lastByte >= 150UL) break;
    }

    if (requiredToken.length() == 0 && receivedAny && millis() - lastByte >= 300UL) {
      if (response.indexOf("OK") >= 0 ||
          response.indexOf("ERROR") >= 0 ||
          response.indexOf("DOWNLOAD") >= 0) {
        break;
      }
    }

    delay(5);
  }

  Serial.println(response);
  return response;
}

bool responseContainsHttpSuccess(const String &response, int methodCode) {
  String ok200 = "+HTTPACTION: " + String(methodCode) + ",200";
  String ok201 = "+HTTPACTION: " + String(methodCode) + ",201";
  String ok204 = "+HTTPACTION: " + String(methodCode) + ",204";

  return response.indexOf(ok200) >= 0 ||
         response.indexOf(ok201) >= 0 ||
         response.indexOf(ok204) >= 0;
}

String extractHttpReadBody(const String &response) {
  int marker = response.indexOf("+HTTPREAD:");
  if (marker < 0) return "";

  int lineEnd = response.indexOf('\n', marker);
  if (lineEnd < 0) return "";

  String body = response.substring(lineEnd + 1);

  int finalOk = body.lastIndexOf("\r\nOK");
  if (finalOk >= 0) body = body.substring(0, finalOk);

  body.trim();
  return body;
}

bool prepareNativeHttps(const String &url, const String &userHeaders = "") {
  // Always terminate a stale HTTP context first. ERROR here is harmless.
  sendA7670AT("AT+HTTPTERM", 2500UL);
  safeDelay(300);

  String response = sendA7670AT("AT+HTTPINIT", 5000UL);
  if (response.indexOf("OK") < 0) {
    Serial.println("A7670E HTTPINIT failed.");
    return false;
  }

  // A7670E/A76XX does not use AT+HTTPSSL=1 for this HTTP service.
  // HTTPS is selected by using an https:// URL. Some firmware supports the
  // optional AT+HTTPPARA="SSLCFG",<id>, but the default context is sufficient
  // for this Firebase test and matches SIMCom's documented HTTPS example.

  String urlCommand = "AT+HTTPPARA=\"URL\",\"" + url + "\"";
  response = sendA7670AT(urlCommand, 7000UL);
  if (response.indexOf("OK") < 0) {
    Serial.println("A7670E URL setup failed.");
    sendA7670AT("AT+HTTPTERM", 2500UL);
    return false;
  }

  response = sendA7670AT(
    "AT+HTTPPARA=\"CONTENT\",\"application/json\"",
    3000UL
  );
  if (response.indexOf("OK") < 0) {
    Serial.println("A7670E Content-Type setup failed.");
    sendA7670AT("AT+HTTPTERM", 2500UL);
    return false;
  }

  if (userHeaders.length() > 0) {
    String headerCommand =
      "AT+HTTPPARA=\"USERDATA\",\"" + userHeaders + "\"";

    response = sendA7670AT(headerCommand, 5000UL);
    if (response.indexOf("OK") < 0) {
      Serial.println("A7670E custom HTTP header setup failed.");
      sendA7670AT("AT+HTTPTERM", 2500UL);
      return false;
    }
  }

  return true;
}

bool nativeFirebasePatch(const String &url,
                         const String &jsonData,
                         String &responseBody) {
  responseBody = "";

  // A7670E HTTPACTION does not expose PATCH directly. Firebase officially
  // supports POST plus X-HTTP-Method-Override: PATCH.
  if (!prepareNativeHttps(url, "X-HTTP-Method-Override: PATCH")) {
    return false;
  }

  String dataCommand =
    "AT+HTTPDATA=" + String(jsonData.length()) + ",15000";

  String response = sendA7670AT(dataCommand, 6000UL, "DOWNLOAD");
  if (response.indexOf("DOWNLOAD") < 0) {
    Serial.println("A7670E HTTPDATA did not enter DOWNLOAD mode.");
    sendA7670AT("AT+HTTPTERM", 2500UL);
    return false;
  }

  Serial.println("Sending Firebase PATCH JSON to A7670E...");
  SerialAT.print(jsonData);

  // Wait for the modem to accept the uploaded request body.
  response = "";
  unsigned long bodyStarted = millis();
  while (millis() - bodyStarted < 5000UL) {
    servicePortal();
    managePumpSafety();

    while (SerialAT.available()) {
      response += (char)SerialAT.read();
    }

    if (response.indexOf("OK") >= 0 || response.indexOf("ERROR") >= 0) break;
    delay(5);
  }
  Serial.println(response);

  if (response.indexOf("ERROR") >= 0) {
    Serial.println("A7670E rejected the HTTP request body.");
    sendA7670AT("AT+HTTPTERM", 2500UL);
    return false;
  }

  // HTTPACTION=1 is POST; Firebase changes it to PATCH from the override header.
  response = sendA7670AT("AT+HTTPACTION=1", 60000UL, "+HTTPACTION:");
  bool success = responseContainsHttpSuccess(response, 1);

  String readResponse = sendA7670AT("AT+HTTPREAD", 15000UL, "+HTTPREAD:");
  responseBody = extractHttpReadBody(readResponse);

  sendA7670AT("AT+HTTPTERM", 3000UL);

  Serial.print("Firebase PATCH body: ");
  Serial.println(responseBody);

  return success;
}

bool nativeFirebaseGet(const String &url, String &responseBody) {
  responseBody = "";
  if (!prepareNativeHttps(url)) return false;

  String response = sendA7670AT("AT+HTTPACTION=0", 60000UL, "+HTTPACTION:");
  bool success = responseContainsHttpSuccess(response, 0);
  String readResponse = sendA7670AT("AT+HTTPREAD", 15000UL, "+HTTPREAD:");
  responseBody = extractHttpReadBody(readResponse);
  sendA7670AT("AT+HTTPTERM", 3000UL);
  return success;
}

bool extractCommandString(const String &json, const String &key, String &value) {
  String marker = "\"" + key + "\"";
  int keyAt = json.indexOf(marker);
  if (keyAt < 0) return false;
  int colon = json.indexOf(':', keyAt + marker.length());
  int quoteStart = json.indexOf('"', colon + 1);
  int quoteEnd = quoteStart >= 0 ? json.indexOf('"', quoteStart + 1) : -1;
  if (colon < 0 || quoteStart < 0 || quoteEnd < 0) return false;
  value = json.substring(quoteStart + 1, quoteEnd);
  return true;
}

bool extractCommandInteger(const String &json, const String &key, int64_t &value) {
  String marker = "\"" + key + "\"";
  int keyAt = json.indexOf(marker);
  if (keyAt < 0) return false;
  int colon = json.indexOf(':', keyAt + marker.length());
  if (colon < 0) return false;
  int start = colon + 1;
  while (start < (int)json.length() && isspace((unsigned char)json[start])) start++;
  char *endPointer = NULL;
  value = strtoll(json.c_str() + start, &endPointer, 10);
  return endPointer != json.c_str() + start;
}

bool validCommandId(const String &commandId) {
  if (commandId.length() < 8 || commandId.length() > 64) return false;
  for (size_t i = 0; i < commandId.length(); i++) {
    char c = commandId[i];
    if (!isAlphaNumeric(c) && c != '-' && c != '_') return false;
  }
  return true;
}

String buildCommandAck(const String &commandId, const String &status,
                       const String &reason) {
  String ack = "{";
  ack += "\"command_id\":\"" + escapeJsonString(commandId) + "\",";
  ack += "\"status\":\"" + status + "\",";
  ack += "\"reason\":\"" + escapeJsonString(reason) + "\",";
  ack += "\"executed_at\":" + String((long long)time(nullptr)) + ",";
  ack += "\"pump_on\":" + String(pumpState ? "true" : "false") + ",";
  ack += "\"emergency_stop\":" + String(emergencyStopLatched ? "true" : "false");
  ack += "}";
  return ack;
}

String applyRemoteCommand(const String &payload) {
  if (payload.length() == 0 || payload == "null") return "";

  String commandId;
  String action;
  int64_t issuedAt = 0;
  int64_t expiresAt = 0;
  if (!extractCommandString(payload, "command_id", commandId) ||
      !extractCommandString(payload, "action", action) ||
      !extractCommandInteger(payload, "issued_at", issuedAt) ||
      !extractCommandInteger(payload, "expires_at", expiresAt) ||
      !validCommandId(commandId)) {
    return buildCommandAck("invalid", "rejected", "malformed_command");
  }

  if (commandId == lastProcessedCommandId) return "";

  String status = "executed";
  String reason = "completed";
  int64_t now = (int64_t)time(nullptr);
  if (!clockIsValid()) {
    status = "rejected";
    reason = "device_clock_not_synchronized";
  } else if (expiresAt <= issuedAt || now > expiresAt || issuedAt > now + 60) {
    status = "rejected";
    reason = "expired_or_invalid_timestamp";
  } else if (issuedAt <= lastProcessedIssuedAt) {
    status = "rejected";
    reason = "replayed_or_older_command";
  }

  // Persist before actuation so a reset cannot replay a pump command.
  lastProcessedCommandId = commandId;
  if (issuedAt > lastProcessedIssuedAt) lastProcessedIssuedAt = issuedAt;
  preferences.putString("lastCommand", lastProcessedCommandId);
  preferences.putLong64("lastIssued", lastProcessedIssuedAt);

  if (status == "executed") {
    if (action == "pump_on") {
      manualMode = true;
      if (!startPumpSafely("remote_dashboard", reason)) status = "rejected";
    } else if (action == "pump_off") {
      manualMode = true;
      stopPump("remote_dashboard_off");
    } else if (action == "auto") {
      manualMode = false;
      manualPumpState = false;
      emergencyStopLatched = false;
      dryStartTime = 0;
    } else if (action == "emergency_off") {
      emergencyStopLatched = true;
      manualMode = true;
      stopPump("remote_emergency_stop");
    } else if (action == "reset_cooldown") {
      clearCooldown();
    } else {
      status = "rejected";
      reason = "unsupported_action";
    }
  }

  Serial.print("Remote dashboard command ");
  Serial.print(commandId);
  Serial.print(": ");
  Serial.print(status);
  Serial.print(" (");
  Serial.print(reason);
  Serial.println(")");
  return buildCommandAck(commandId, status, reason);
}

void pollRemoteControlCellular() {
  String payload;
  if (!nativeFirebaseGet(CONTROL_COMMAND_URL, payload)) return;
  String ack = applyRemoteCommand(payload);
  if (ack.length() == 0) return;
  String responseBody;
  if (!nativeFirebasePatch(CONTROL_ACK_URL, ack, responseBody)) {
    Serial.println("Remote command acknowledgement failed over cellular.");
  }
}

void pollRemoteControlWifi(WiFiClientSecure &client) {
  HTTPClient controlHttp;
  controlHttp.setTimeout(8000);
  controlHttp.setReuse(false);
  if (!controlHttp.begin(client, CONTROL_COMMAND_URL)) return;
  int getCode = controlHttp.GET();
  String payload = getCode == 200 ? controlHttp.getString() : "";
  controlHttp.end();
  if (getCode != 200) return;

  String ack = applyRemoteCommand(payload);
  if (ack.length() == 0) return;
  if (!controlHttp.begin(client, CONTROL_ACK_URL)) return;
  controlHttp.addHeader("Content-Type", "application/json");
  int ackCode = controlHttp.PATCH(ack);
  Serial.print("Remote command Wi-Fi acknowledgement code: ");
  Serial.println(ackCode);
  controlHttp.end();
}

// ===== FIREBASE FUNCTION =====
bool sendToFirebase(float temperature, float humidity, bool dhtError,
                    int soilRaw, int soilPercent,
                    int rainRaw, bool rainDetected,
                    bool pumpState, unsigned long capturedUptime,
                    int64_t capturedUnix) {

  String soilStatus;
  if (soilPercent < 15) {
    soilStatus = "Dry";
  } else if (soilPercent < 55) {
    soilStatus = "Moist";
  } else {
    soilStatus = "Wet";
  }

  String rainStatus = rainDetected ? "Rain Detected" : "No Rain";
  String pumpStatus = pumpState ? "ON" : "OFF";
  bool usingWifi = wifiBackupActive && WiFi.status() == WL_CONNECTED;
  String networkStatus = usingWifi ? "Wi-Fi Connected" :
                         (cellularActive ? "Cellular Connected" : "Disconnected");

  String jsonData = "{";
  jsonData += "\"sensors\":{";
  jsonData += "\"dht22\":{";
  if (!dhtError) {
    jsonData += "\"temperature_celsius\":" + String(temperature, 2) + ",";
    jsonData += "\"humidity_percent\":" + String(humidity, 2) + ",";
  } else {
    jsonData += "\"temperature_celsius\":null,";
    jsonData += "\"humidity_percent\":null,";
  }
  jsonData += "\"error\":" + String(dhtError ? "true" : "false");
  jsonData += "},";

  jsonData += "\"soil\":{";
  jsonData += "\"raw_value\":" + String(soilRaw) + ",";
  jsonData += "\"moisture_percent\":" + String(soilPercent) + ",";
  jsonData += "\"status\":\"" + soilStatus + "\"";
  jsonData += "},";

  jsonData += "\"rain\":{";
  jsonData += "\"raw_value\":" + String(rainRaw) + ",";
  jsonData += "\"detected\":" + String(rainDetected ? "true" : "false") + ",";
  jsonData += "\"status\":\"" + rainStatus + "\"";
  jsonData += "}";
  jsonData += "},";

  jsonData += "\"actuator\":{";
  jsonData += "\"pump\":{";
  jsonData += "\"status\":\"" + pumpStatus + "\",";
  jsonData += "\"is_on\":" + String(pumpState ? "true" : "false") + ",";
  jsonData += "\"cooldown_active\":" + String(cooldownActive() ? "true" : "false") + ",";
  jsonData += "\"cooldown_remaining_seconds\":" + String(cooldownRemainingSeconds()) + ",";
  jsonData += "\"emergency_stop\":" + String(emergencyStopLatched ? "true" : "false") + ",";
  jsonData += "\"manual_mode\":" + String(manualMode ? "true" : "false") + ",";
  jsonData += "\"stop_reason\":\"" + pumpStopReason + "\",";
  jsonData += "\"relay_pin\":23";
  jsonData += "}";
  jsonData += "},";

  jsonData += "\"system\":{";
  jsonData += "\"status\":\"online\",";
  jsonData += "\"network_type\":\"" + String(usingWifi ? "wifi" :
              (cellularActive ? "cellular" : "disconnected")) + "\",";
  jsonData += "\"network_status\":\"" + networkStatus + "\",";
  jsonData += "\"queue_depth\":" + String(telemetryQueueCount) + ",";
  jsonData += "\"captured_uptime_ms\":" + String(capturedUptime) + ",";
  jsonData += "\"captured_unix\":" + String((long long)capturedUnix) + ",";
  jsonData += "\"last_update_server\":{\".sv\":\"timestamp\"},";
  jsonData += "\"last_update_unix\":" + String(time(nullptr));
  jsonData += "}";
  jsonData += "}";

  const String patchUrl = FIREBASE_TELEMETRY_URL;

  lastUploadAttemptMs = millis();
  uploadState = usingWifi ? "Uploading through Wi-Fi" : "Uploading through cellular";

  if (cellularActive && !wifiBackupActive) {
    String patchBody;
    bool patchOK = nativeFirebasePatch(patchUrl, jsonData, patchBody);

    if (patchOK) {
      cloudAvailable = true;
      lastUploadSucceeded = true;
      lastUploadSuccessMs = millis();
      consecutiveUploadFailures = 0;
      uploadState = "Firebase upload successful";
      Serial.println("Firebase cellular PATCH successful.");
      pollRemoteControlCellular();
      return true;
    } else {
      cloudAvailable = false;
      lastUploadSucceeded = false;
      consecutiveUploadFailures++;
      uploadState = "Cellular upload failed";
      Serial.println("Firebase cellular PATCH failed.");

      // If Wi-Fi is available, upload the same snapshot immediately. Returning
      // here used to leave the new route idle until the five-minute backoff.
      if (connectWifiBackup(false) || WiFi.status() == WL_CONNECTED) {
        Serial.println("Wi-Fi backup ready. Retrying this upload now.");
        preferWifiAndSuspendCellular();
      } else {
        if (!modem.isNetworkConnected()) cellularActive = false;
        return false;
      }
    }
  }

  // Wi-Fi fallback retained from the original greenhouse project.
  HTTPClient httpWiFi;
  httpWiFi.setTimeout(15000);
  httpWiFi.setReuse(false);

  WiFiClientSecure wifiClient;
  wifiClient.setInsecure();
  wifiClient.setTimeout(15000);

  if (!httpWiFi.begin(wifiClient, patchUrl)) {
    cloudAvailable = false;
    lastUploadSucceeded = false;
    consecutiveUploadFailures++;
    uploadState = "Wi-Fi upload setup failed";
    Serial.println("Firebase Error: Wi-Fi http.begin() failed.");
    return false;
  }

  httpWiFi.addHeader("Content-Type", "application/json");
  int httpResponseCode = httpWiFi.PATCH(jsonData);

  Serial.print("Firebase Wi-Fi PATCH code: ");
  Serial.println(httpResponseCode);

  if (httpResponseCode >= 200 && httpResponseCode < 300) {
    cloudAvailable = true;
    wifiBackupActive = true;
    lastUploadSucceeded = true;
    lastUploadSuccessMs = millis();
    consecutiveUploadFailures = 0;
    uploadState = "Firebase upload successful";
    Serial.println("Firebase Wi-Fi backup PATCH successful.");
    Serial.println(httpWiFi.getString());
  } else if (httpResponseCode > 0) {
    cloudAvailable = false;
    lastUploadSucceeded = false;
    consecutiveUploadFailures++;
    uploadState = "Firebase rejected the upload";
    Serial.println(httpWiFi.getString());
  } else {
    cloudAvailable = false;
    lastUploadSucceeded = false;
    consecutiveUploadFailures++;
    uploadState = "Wi-Fi upload failed";
    Serial.print("Firebase Wi-Fi error: ");
    Serial.println(httpWiFi.errorToString(httpResponseCode));
  }
  httpWiFi.end();

  if (cloudAvailable) pollRemoteControlWifi(wifiClient);

  wifiClient.stop();
  return cloudAvailable;
}

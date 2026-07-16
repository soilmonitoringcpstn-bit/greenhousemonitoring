#define TINY_GSM_MODEM_A7672X
#include <TinyGsmClient.h>

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "time.h"

WebServer server(80);
DNSServer dnsServer;

const byte DNS_PORT = 53;

// ===== CELLULAR SETTINGS =====
#define SerialAT Serial2
#define RX_PIN 16
#define TX_PIN 17
const char apn[]  = "internet"; // Default APN
const char gprsUser[] = "";
const char gprsPass[] = "";

TinyGsm modem(SerialAT);
TinyGsmClientSecure gsmClient(modem);

bool cellularActive = false;

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

// ===== FIREBASE RTDB URL =====
String firebaseURL = "https://soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app/greenhouse.json";

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
int soilDryValue = 1600;
int soilWetValue = 500;

// ===== RAIN CALIBRATION =====
int rainThreshold = 3800;

// ===== PUMP THRESHOLDS =====
int pumpOnThreshold = 15;    // Pump ON only below 15%
int pumpOffThreshold = 55;   // Pump OFF at 55% and above

bool pumpState = false;

// ===== NTP TIME SETTINGS =====
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 8 * 3600; // UTC+8
const int   daylightOffset_sec = 0;

// ===== AUTO RUN LIMIT & COOLDOWN =====
unsigned long pumpOnTime = 0;
const unsigned long maxRunTime = 120000;       // 2 minutes in ms
const unsigned long cooldownTime = 18000000;   // 5 hours in ms
unsigned long lastAutoRunTime = -cooldownTime; // Prevents cooldown on boot

// ===== DRY DELAY PROTECTION =====
unsigned long dryStartTime = 0;
const unsigned long dryDelay = 10000;

// ===== SENSOR READ INTERVAL =====
unsigned long lastReadTime = 0;
const unsigned long readInterval = 2000;

// ===== FIREBASE SEND INTERVAL =====
unsigned long lastSend = 0;
unsigned long sendInterval = 10000; // send every 10 seconds

// ===== HELPER FUNCTIONS TO KEEP PORTAL ALIVE =====
void servicePortal() {
  dnsServer.processNextRequest();
  server.handleClient();
}

void managePumpSafety() {
  if (pumpState && pumpOnTime > 0) {
    if ((millis() - pumpOnTime) >= maxRunTime) {
      pumpState = false;
      pumpOnTime = 0;
      lastAutoRunTime = millis();
      digitalWrite(RELAY_PIN, RELAY_OFF);
      Serial.println("Safety: 2-min limit reached during blocking delay. Pump OFF.");
    }
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
  String html = "<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">";
  html += "<title>Smart Greenhouse Settings</title>";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', Roboto, Helvetica, Arial, sans-serif; margin: 0; padding: 20px; background: linear-gradient(135deg, #e0f2f1, #b2dfdb); color: #263238; min-height: 100vh; }";
  html += "h2 { color: #004d40; text-align: center; margin-bottom: 30px; font-weight: 800; letter-spacing: 0.5px; }";
  html += "h3 { color: #00695c; margin-top: 30px; font-weight: 700; border-bottom: 2px solid #b2dfdb; padding-bottom: 5px; }";
  html += ".card { background: rgba(255, 255, 255, 0.85); border: 1px solid rgba(255,255,255,0.4); padding: 18px; margin-bottom: 12px; border-radius: 16px; box-shadow: 0 8px 32px rgba(0,0,0,0.05); backdrop-filter: blur(10px); display: flex; justify-content: space-between; align-items: center; }";
  html += ".val { font-weight: 700; color: #00796b; font-size: 1.1em; }";
  html += ".btn { flex: 1; padding: 14px; margin: 5px; border-radius: 12px; border: none; color: white; cursor: pointer; font-weight: bold; font-size: 14px; transition: transform 0.2s, box-shadow 0.2s; box-shadow: 0 4px 6px rgba(0,0,0,0.1); }";
  html += ".btn:active { transform: translateY(2px); }";
  html += ".btn-auto { background: linear-gradient(135deg, #1976d2, #1565c0); }";
  html += ".btn-on { background: linear-gradient(135deg, #388e3c, #2e7d32); }";
  html += ".btn-off { background: linear-gradient(135deg, #d32f2f, #c62828); }";
  html += ".btn-reset { background: linear-gradient(135deg, #f59e0b, #d97706); }";
  html += ".btn-group { display: flex; flex-wrap: wrap; justify-content: space-between; gap: 10px; }";
  html += "input[type='number'] { padding: 8px; border: 2px solid #b2dfdb; border-radius: 8px; width: 70px; font-weight: bold; color: #004d40; outline: none; }";
  html += "</style></head><body>";
  html += "<h2>Greenhouse Monitor</h2>";
  html += "<div class=\"card\"><strong>Network:</strong> <span class=\"val\" id=\"netw\">--</span></div>";
  html += "<div class=\"card\"><strong>Temperature:</strong> <span class=\"val\" id=\"temp\">--</span></div>";
  html += "<div class=\"card\"><strong>Humidity:</strong> <span class=\"val\" id=\"hum\">--</span></div>";
  html += "<div class=\"card\"><strong>Soil Moisture:</strong> <span class=\"val\" id=\"soil\">--</span></div>";
  html += "<div class=\"card\"><strong>Rain Detected:</strong> <span class=\"val\" id=\"rain\">--</span></div>";
  html += "<div class=\"card\"><strong>Pump Relay:</strong> <span class=\"val\" id=\"pump\">--</span></div>";
  html += "<div class=\"card\"><strong>System Mode:</strong> <span class=\"val\" id=\"mode\">--</span></div>";
  html += "<div class=\"card\"><strong>Cooldown Active:</strong> <span class=\"val\" id=\"cd\">--</span></div>";
  
  html += "<h3>Pump Control</h3>";
  html += "<div class=\"btn-group\">";
  html += "<button class=\"btn btn-auto\" onclick=\"setMode('auto','')\">🤖 Auto</button>";
  html += "<button class=\"btn btn-on\" onclick=\"setMode('manual','on')\">💧 ON</button>";
  html += "<button class=\"btn btn-off\" onclick=\"setMode('manual','off')\">🛑 OFF</button>";
  html += "<button class=\"btn btn-reset\" onclick=\"setMode('reset_cd','')\">🔄 Reset CD</button>";
  html += "</div>";
  html += "<h3>Auto Thresholds</h3>";
  html += "<div class=\"card\" style=\"flex-direction:column; gap:10px; align-items:flex-start;\">";
  html += "<div><label>Turn ON below (%): </label><input type=\"number\" id=\"inpOn\"></div>";
  html += "<div><label>Turn OFF above (%): </label><input type=\"number\" id=\"inpOff\"></div>";
  html += "<button class=\"btn btn-auto\" style=\"width:100%; margin-top:10px;\" onclick=\"setThresh()\">Update Thresholds</button>";
  html += "</div>";

  html += "<script>";
  html += "function fetchData() {";
  html += "  fetch('/data').then(r=>r.json()).then(d=>{";
  html += "    document.getElementById('netw').innerHTML = d.network;";
  html += "    document.getElementById('temp').innerHTML = d.dhtError ? 'Error' : d.temperature + ' &deg;C';";
  html += "    document.getElementById('hum').innerHTML = d.dhtError ? 'Error' : d.humidity + ' %';";
  html += "    document.getElementById('soil').innerHTML = d.soilPercent + ' % (Raw: ' + d.soilRaw + ')';";
  html += "    document.getElementById('rain').innerHTML = d.rainDetected ? 'Yes' : 'No';";
  html += "    document.getElementById('pump').innerHTML = d.pumpState ? 'ON' : 'OFF';";
  html += "    document.getElementById('mode').innerHTML = d.manualMode ? 'MANUAL' : 'AUTO';";
  html += "    document.getElementById('cd').innerHTML = (d.cooldown === 'true' || d.cooldown === true) ? 'YES' : 'NO';";
  html += "    if(document.activeElement.id !== 'inpOn') document.getElementById('inpOn').value = d.onThresh;";
  html += "    if(document.activeElement.id !== 'inpOff') document.getElementById('inpOff').value = d.offThresh;";
  html += "  });";
  html += "}";
  html += "setInterval(fetchData, 1500); fetchData();";
  html += "function setMode(mode, state) {";
  html += "  fetch('/set?mode=' + mode + '&state=' + state).then(fetchData);";
  html += "}";
  html += "function setThresh() {";
  html += "  let onT = document.getElementById('inpOn').value;";
  html += "  let offT = document.getElementById('inpOff').value;";
  html += "  fetch('/set?on_thresh=' + onT + '&off_thresh=' + offT).then(fetchData);";
  html += "}";
  html += "</script>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleData() {
  String json = "{";
  json += "\"network\":\"" + String(cellularActive ? "Cellular" : "Disconnected") + "\",";
  json += "\"temperature\":" + String(currentTemperature, 2) + ",";
  json += "\"humidity\":" + String(currentHumidity, 2) + ",";
  json += "\"soilPercent\":" + String(currentSoilPercent) + ",";
  json += "\"soilRaw\":" + String(currentSoilRaw) + ",";
  json += "\"rainDetected\":" + String(currentRainDetected ? "true" : "false") + ",";
  json += "\"dhtError\":" + String(currentDhtError ? "true" : "false") + ",";
  json += "\"pumpState\":" + String(pumpState ? "true" : "false") + ",";
  json += "\"manualMode\":" + String(manualMode ? "true" : "false") + ",";
  json += "\"onThresh\":" + String(pumpOnThreshold) + ",";
  json += "\"offThresh\":" + String(pumpOffThreshold) + ",";
  json += "\"cooldown\":" + String(((millis() - lastAutoRunTime) < cooldownTime) ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleSet() {
  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    if (mode == "manual") {
      manualMode = true;
      if (server.hasArg("state")) {
        String state = server.arg("state");
        if (state == "on") manualPumpState = true;
        else if (state == "off") manualPumpState = false;
      }
    } else if (mode == "auto") {
      manualMode = false;
      dryStartTime = 0; // reset auto hysteresis timers
    } else if (mode == "reset_cd") {
      lastAutoRunTime = -cooldownTime;
      Serial.println("Cooldown forcefully reset via Captive Portal.");
    }
  }
  if (server.hasArg("on_thresh")) {
    pumpOnThreshold = server.arg("on_thresh").toInt();
  }
  if (server.hasArg("off_thresh")) {
    pumpOffThreshold = server.arg("off_thresh").toInt();
  }
  server.send(200, "text/plain", "OK");
}

void handleNotFound() {
  if (server.hostHeader() != WiFi.softAPIP().toString()) {
    server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
    return;
  }
  handleRoot();
}

void syncCellularTime() {
  int year3=0, month3=0, day3=0, hour3=0, min3=0, sec3=0;
  float timezone=0;
  Serial.println("Requesting time from Cellular Network...");
  if (modem.getNetworkTime(&year3, &month3, &day3, &hour3, &min3, &sec3, &timezone)) {
    struct tm t = {0};
    t.tm_year = year3 - 1900;
    t.tm_mon = month3 - 1;
    t.tm_mday = day3;
    t.tm_hour = hour3;
    t.tm_min = min3;
    t.tm_sec = sec3;
    time_t timeSinceEpoch = mktime(&t);
    struct timeval tv;
    tv.tv_sec = timeSinceEpoch;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    Serial.println("Time synced via Cellular Network.");
  } else {
    Serial.println("Failed to get Cellular Time.");
  }
}

void connectNetwork() {
  Serial.println("Attempting to connect to Cellular Network (A760E)...");
  
  if (!modem.init()) {
    Serial.println("Failed to initialize modem! Retrying later.");
    cellularActive = false;
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
    return;
  }
  Serial.println(" success.");

  Serial.print("Connecting to APN: ");
  Serial.print(apn);
  if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
    Serial.println(" fail. Retrying later.");
    cellularActive = false;
    return;
  }
  
  Serial.println(" success!");
  cellularActive = true;
  
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
  if (soil == 0 || soil >= 4095) {
    Serial.print("WARNING (Raw: ");
    Serial.print(soil);
    Serial.println(", might be disconnected or shorted)");
  } else {
    Serial.print("PASS (Raw: ");
    Serial.print(soil);
    Serial.println(")");
  }

  // 3. Test Rain Sensor
  int rain = analogRead(RAIN_PIN);
  Serial.print("[TEST] Rain Sensor: ");
  if (rain == 0 || rain >= 4095) {
    Serial.print("WARNING (Raw: ");
    Serial.print(rain);
    Serial.println(", might be disconnected or shorted)");
  } else {
    Serial.print("PASS (Raw: ");
    Serial.print(rain);
    Serial.println(")");
  }

  // 4. Test SIM Module
  Serial.print("[TEST] SIM Module (A760E): ");
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

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("SMART GREENHOUSE SYSTEM STARTING...");
  Serial.println("DHT22 + Soil + Rain + Relay + Cellular/WiFi + Firebase");
  Serial.println("---------------------------------------------");

  // 1. START AP AND SERVER FIRST (so it's never blocked)
  WiFi.mode(WIFI_AP);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP("Greenhouse_Portal");
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  dnsServer.start(DNS_PORT, "*", apIP);
  
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/set", handleSet);

  // Common captive portal detection URLs
  server.on("/generate_204", handleRoot);
  server.on("/gen_204", handleRoot);
  server.on("/hotspot-detect.html", handleRoot);
  server.on("/connecttest.txt", handleRoot);
  server.on("/ncsi.txt", handleRoot);

  server.onNotFound(handleNotFound); // Redirect captive portal requests
  server.begin();
  Serial.println("Captive Portal started. Connect to 'Greenhouse_Portal' now!");

  // 2. INIT SENSORS
  dht.begin();
  pinMode(SOIL_PIN, INPUT);
  pinMode(RAIN_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);
  Serial.println("Pump relay OFF at startup.");

  // 3. RUN DIAGNOSTICS
  runDiagnostics();

  // 4. Connect Cellular
  connectNetwork();
}

void loop() {
  servicePortal();

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
    struct tm timeinfo;
    bool isScheduledTime = false;
    if (getLocalTime(&timeinfo, 10)) { // 10ms non-blocking
      if ((timeinfo.tm_hour == 6 || timeinfo.tm_hour == 18) && timeinfo.tm_min < 2) {
        isScheduledTime = true;
      }
    }

    // ===== PUMP LOGIC =====
    if (manualMode) {
      if (pumpState != manualPumpState) {
        pumpState = manualPumpState;
        Serial.print("Manual Mode: Pump ");
        Serial.println(pumpState ? "ON" : "OFF");
      }
      dryStartTime = 0;
      pumpOnTime = 0;
    } else if (isScheduledTime) {
      // Scheduled 6 AM/PM override (runs for 2 minutes)
      if (!pumpState) {
        pumpState = true;
        Serial.println("Scheduled Time (6 AM/PM). Pump forced ON.");
      }
      dryStartTime = 0;
      pumpOnTime = 0;
    } else {
      // ===== AUTO MODE (WITH COOLDOWN & 2-MIN LIMIT) =====
      bool coolingDown = (millis() - lastAutoRunTime) < cooldownTime;
      
      if (pumpState) {
        // Pump is currently ON in Auto Mode
        if ((millis() - pumpOnTime) >= maxRunTime) {
          // Run limit reached (2 minutes)
          pumpState = false;
          pumpOnTime = 0;
          lastAutoRunTime = millis(); // Start 5-hour cooldown
          Serial.println("Auto Mode: 2-minute limit reached. Pump OFF. Cooldown started.");
        } else if (rainDetected) {
          // Stop immediately if it rains
          pumpState = false;
          pumpOnTime = 0;
          lastAutoRunTime = millis();
          Serial.println("Auto Mode: Rain detected. Pump OFF. Cooldown started.");
        }
      } else {
        // Pump is currently OFF
        if (soilPercent < pumpOnThreshold && !rainDetected && !coolingDown) {
          if (dryStartTime == 0) {
            dryStartTime = millis();
            Serial.println("Soil dry & cooldown finished. Dry timer started...");
          }
          
          // Wait 10 seconds before triggering
          if (millis() - dryStartTime >= dryDelay) {
            pumpState = true;
            pumpOnTime = millis(); // Record start time for 2-min limit
            Serial.println("Soil stayed dry. Pump ON for 2 minutes.");
            dryStartTime = 0;
          }
        } else {
          dryStartTime = 0;
        }
      }
    }

    digitalWrite(RELAY_PIN, pumpState ? RELAY_ON : RELAY_OFF);

    // ===== SERIAL MONITOR DISPLAY =====
    Serial.println();
    Serial.println("========== GREENHOUSE DATA ==========");
    Serial.print("Network: ");
    Serial.println(cellularActive ? "Cellular (A760E)" : "Disconnected");

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

    bool isCooling = (millis() - lastAutoRunTime) < cooldownTime;
    Serial.print("Cooldown: ");
    if (isCooling) {
      unsigned long remaining = cooldownTime - (millis() - lastAutoRunTime);
      Serial.print("ACTIVE (");
      Serial.print(remaining / 1000 / 60);
      Serial.println(" mins left)");
    } else {
      Serial.println("INACTIVE");
    }



    if (dryStartTime > 0 && !pumpState) {
      Serial.print("Dry timer: ");
      Serial.print((millis() - dryStartTime) / 1000);
      Serial.println(" seconds");
    }

    // ===== SEND TO FIREBASE =====
    if (millis() - lastSend >= sendInterval) {
      lastSend = millis();

      if (!cellularActive || !modem.isNetworkConnected()) {
        Serial.println("Cellular disconnected. Reconnecting...");
        connectNetwork();
      }

      if (cellularActive) {
        sendToFirebase(
          temperature,
          humidity,
          dhtError,
          soilRaw,
          soilPercent,
          rainRaw,
          rainDetected,
          pumpState
        );
      }
    }

    Serial.println("=====================================");

  } // End of readInterval block
}

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

// ===== FIREBASE FUNCTION =====
void sendToFirebase(float temperature, float humidity, bool dhtError,
                    int soilRaw, int soilPercent,
                    int rainRaw, bool rainDetected,
                    bool pumpState) {

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
  String networkStatus = cellularActive ? "Cellular Connected" : "Disconnected";

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
  jsonData += "\"cooldown_active\":" + String(((millis() - lastAutoRunTime) < cooldownTime) ? "true" : "false") + ",";
  jsonData += "\"relay_pin\":23";
  jsonData += "}";
  jsonData += "},";

  jsonData += "\"system\":{";
  jsonData += "\"status\":\"online\",";
  jsonData += "\"network_type\":\"" + String(cellularActive ? "cellular" : "disconnected") + "\",";
  jsonData += "\"network_status\":\"" + networkStatus + "\",";
  jsonData += "\"last_update_unix\":" + String(time(nullptr));
  jsonData += "}";
  jsonData += "}";

  String serverName = "soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app";
  String patchPath = "/greenhouse.json";
  String getPath = "/control.json";

  Serial.println("Sending data to Firebase using PATCH...");

  if (cellularActive) {
    if (gsmClient.connect(serverName.c_str(), 443)) {
      gsmClient.print(String("PATCH ") + patchPath + " HTTP/1.1\r\n" + 
                      "Host: " + serverName + "\r\n" + 
                      "Connection: close\r\n" + 
                      "Content-Type: application/json\r\n" + 
                      "Content-Length: " + String(jsonData.length()) + "\r\n\r\n" + 
                      jsonData);
      unsigned long timeout = millis();
      while (gsmClient.connected() && millis() - timeout < 10000L) {
        while (gsmClient.available()) {
          gsmClient.read(); // Consume response
          timeout = millis();
        }
      }
      gsmClient.stop();
      Serial.println("Firebase: Cellular PATCH completed.");
    } else {
      Serial.println("Firebase Cellular Error: Connection failed.");
    }

    // FETCH REMOTE COMMANDS
    if (gsmClient.connect(serverName.c_str(), 443)) {
      gsmClient.print(String("GET ") + getPath + " HTTP/1.1\r\n" + 
                      "Host: " + serverName + "\r\n" + 
                      "Connection: close\r\n\r\n");
      String payload = "";
      bool isBody = false;
      unsigned long timeout = millis();
      while (gsmClient.connected() && millis() - timeout < 10000L) {
        while (gsmClient.available()) {
          String line = gsmClient.readStringUntil('\n');
          timeout = millis();
          if (line == "\r") {
            isBody = true;
          } else if (isBody) {
            payload += line;
          }
        }
      }
      gsmClient.stop();
      
      if (payload != "null" && payload.length() > 0) {
        Serial.print("Firebase Control Payload: ");
        Serial.println(payload);
        if (payload.indexOf("\"mode\":\"manual\"") != -1) {
          manualMode = true;
          if (payload.indexOf("\"state\":true") != -1) {
            manualPumpState = true;
          } else if (payload.indexOf("\"state\":false") != -1) {
            manualPumpState = false;
          }
        } else if (payload.indexOf("\"mode\":\"auto\"") != -1) {
          manualMode = false;
        } else if (payload.indexOf("\"mode\":\"reset_cd\"") != -1) {
          lastAutoRunTime = -cooldownTime;
          Serial.println("Cooldown forcefully reset via Web Dashboard.");
        }
      }
    }
  } else {
    HTTPClient httpWiFi;
    httpWiFi.setTimeout(15000);
    httpWiFi.setReuse(false);
    
    WiFiClientSecure wifiClient;
    wifiClient.setInsecure();
    wifiClient.setTimeout(15000);
    
    String fullUrl = "https://" + serverName + patchPath;
    if (!httpWiFi.begin(wifiClient, fullUrl)) {
      Serial.println("Firebase Error: http.begin() failed.");
      return;
    }

    httpWiFi.addHeader("Content-Type", "application/json");
    int httpResponseCode = httpWiFi.PATCH(jsonData);

    Serial.print("Firebase Response Code: ");
    Serial.println(httpResponseCode);

    if (httpResponseCode == 200) {
      Serial.println("Firebase: Data sent successfully.");
    } else if (httpResponseCode > 0) {
      Serial.print("Firebase response: ");
      Serial.println(httpWiFi.getString());
    } else {
      Serial.print("Firebase Error: ");
      Serial.println(httpWiFi.errorToString(httpResponseCode));
    }
    httpWiFi.end();

    // FETCH REMOTE COMMANDS
    String controlUrl = "https://" + serverName + getPath;
    if (httpWiFi.begin(wifiClient, controlUrl)) {
      int getResponseCode = httpWiFi.GET();
      if (getResponseCode == 200) {
        String payload = httpWiFi.getString();
        if (payload != "null") {
          Serial.print("Firebase Control Payload: ");
          Serial.println(payload);
          if (payload.indexOf("\"mode\":\"manual\"") != -1) {
            manualMode = true;
            if (payload.indexOf("\"state\":true") != -1) {
              manualPumpState = true;
            } else if (payload.indexOf("\"state\":false") != -1) {
              manualPumpState = false;
            }
          } else if (payload.indexOf("\"mode\":\"auto\"") != -1) {
            manualMode = false;
          } else if (payload.indexOf("\"mode\":\"reset_cd\"") != -1) {
            lastAutoRunTime = -cooldownTime;
            Serial.println("Cooldown forcefully reset via Web Dashboard.");
          }
        }
      }
      httpWiFi.end();
    }
    wifiClient.stop();
  }
}
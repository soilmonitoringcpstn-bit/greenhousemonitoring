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

// ===== WIFI SETTINGS =====
#define WIFI_SSID "Simonne"
#define WIFI_PASSWORD "9bA4GYd"

// ===== FIREBASE RTDB URL =====
// .json is required
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
// LOW = ON, HIGH = OFF
#define RELAY_ON LOW
#define RELAY_OFF HIGH

// ===== SOIL CALIBRATION =====
// Higher raw value = drier soil
// Lower raw value = wetter soil
// Wider range to avoid sudden percentage drop
int soilDryValue = 1600;
int soilWetValue = 500;

// ===== RAIN CALIBRATION =====
// Below this value = rain detected
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
unsigned long lastAutoRunTime = 0;
const unsigned long maxRunTime = 120000;       // 2 minutes in ms
const unsigned long cooldownTime = 18000000;   // 5 hours in ms

// ===== DRY DELAY PROTECTION =====
// Soil must stay dry for 10 seconds before pump turns ON
unsigned long dryStartTime = 0;
const unsigned long dryDelay = 10000;

// ===== SENSOR READ INTERVAL =====
unsigned long lastReadTime = 0;
const unsigned long readInterval = 2000;

// ===== FIREBASE SEND INTERVAL =====
unsigned long lastSend = 0;
unsigned long sendInterval = 10000; // send every 10 seconds

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
  html += ".btn-group { display: flex; justify-content: space-between; gap: 10px; }";
  html += "input[type='number'] { padding: 8px; border: 2px solid #b2dfdb; border-radius: 8px; width: 70px; font-weight: bold; color: #004d40; outline: none; }";
  html += "</style></head><body>";
  html += "<h2>Greenhouse Monitor</h2>";
  html += "<div class=\"card\"><strong>Temperature:</strong> <span class=\"val\" id=\"temp\">--</span></div>";
  html += "<div class=\"card\"><strong>Humidity:</strong> <span class=\"val\" id=\"hum\">--</span></div>";
  html += "<div class=\"card\"><strong>Soil Moisture:</strong> <span class=\"val\" id=\"soil\">--</span></div>";
  html += "<div class=\"card\"><strong>Rain Detected:</strong> <span class=\"val\" id=\"rain\">--</span></div>";
  html += "<div class=\"card\"><strong>Pump Relay:</strong> <span class=\"val\" id=\"pump\">--</span></div>";
  html += "<div class=\"card\"><strong>System Mode:</strong> <span class=\"val\" id=\"mode\">--</span></div>";
  
  html += "<h3>Pump Control</h3>";
  html += "<div class=\"btn-group\">";
  html += "<button class=\"btn btn-auto\" onclick=\"setMode('auto','')\">🤖 Auto</button>";
  html += "<button class=\"btn btn-on\" onclick=\"setMode('manual','on')\">💧 ON</button>";
  html += "<button class=\"btn btn-off\" onclick=\"setMode('manual','off')\">🛑 OFF</button>";
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
  html += "    document.getElementById('temp').innerHTML = d.dhtError ? 'Error' : d.temperature + ' &deg;C';";
  html += "    document.getElementById('hum').innerHTML = d.dhtError ? 'Error' : d.humidity + ' %';";
  html += "    document.getElementById('soil').innerHTML = d.soilPercent + ' % (Raw: ' + d.soilRaw + ')';";
  html += "    document.getElementById('rain').innerHTML = d.rainDetected ? 'Yes' : 'No';";
  html += "    document.getElementById('pump').innerHTML = d.pumpState ? 'ON' : 'OFF';";
  html += "    document.getElementById('mode').innerHTML = d.manualMode ? 'MANUAL' : 'AUTO';";
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
  json += "\"temperature\":" + String(currentTemperature, 2) + ",";
  json += "\"humidity\":" + String(currentHumidity, 2) + ",";
  json += "\"soilPercent\":" + String(currentSoilPercent) + ",";
  json += "\"soilRaw\":" + String(currentSoilRaw) + ",";
  json += "\"rainDetected\":" + String(currentRainDetected ? "true" : "false") + ",";
  json += "\"dhtError\":" + String(currentDhtError ? "true" : "false") + ",";
  json += "\"pumpState\":" + String(pumpState ? "true" : "false") + ",";
  json += "\"manualMode\":" + String(manualMode ? "true" : "false") + ",";
  json += "\"onThresh\":" + String(pumpOnThreshold) + ",";
  json += "\"offThresh\":" + String(pumpOffThreshold);
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

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("SMART GREENHOUSE SYSTEM STARTING...");
  Serial.println("DHT22 + Soil + Rain + Relay + WiFi + Firebase");
  Serial.println("---------------------------------------------");

  dht.begin();

  pinMode(SOIL_PIN, INPUT);
  pinMode(RAIN_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);

  digitalWrite(RELAY_PIN, RELAY_OFF);
  Serial.println("Pump relay OFF at startup.");

  connectWiFi();

  // Configure NTP
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Time configured via NTP.");

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/set", handleSet);
  server.onNotFound(handleNotFound); // Redirect captive portal requests
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  dnsServer.processNextRequest(); // Handle captive portal DNS requests
  server.handleClient();

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
        lastAutoRunTime = millis(); // Start 5-hour cooldown
        Serial.println("Auto Mode: 2-minute limit reached. Pump OFF. Cooldown started.");
      } else if (rainDetected) {
        // Stop immediately if it rains
        pumpState = false;
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

  Serial.print("WiFi Status: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");

  if (dryStartTime > 0 && !pumpState) {
    Serial.print("Dry timer: ");
    Serial.print((millis() - dryStartTime) / 1000);
    Serial.println(" seconds");
  }

  // ===== SEND TO FIREBASE =====
  if (millis() - lastSend >= sendInterval) {
    lastSend = millis();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected. Reconnecting...");
      connectWiFi();
    }

    if (WiFi.status() == WL_CONNECTED) {
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

// ===== WIFI FUNCTION =====
void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_AP_STA); // Dual mode: Station + Access Point
  WiFi.disconnect(true);
  delay(1000);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int tries = 0;

  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
    Serial.print("ESP32 IP Address: ");
    Serial.println(WiFi.localIP());
    Serial.print("WiFi RSSI: ");
    Serial.println(WiFi.RSSI());
  } else {
    Serial.println("WiFi failed. Check SSID, password, or 2.4GHz hotspot.");
  }

  // Set up AP mode
  WiFi.softAP("Greenhouse_Portal");
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  // Start DNS Server
  dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
  Serial.println("DNS Server started for Captive Portal");
}

// ===== SOIL AVERAGE FUNCTION =====
int readSoilAverage() {
  long total = 0;
  int samples = 20;

  for (int i = 0; i < samples; i++) {
    total += analogRead(SOIL_PIN);
    delay(10);
  }

  return total / samples;
}

// ===== FIREBASE FUNCTION =====
void sendToFirebase(float temperature, float humidity, bool dhtError,
                    int soilRaw, int soilPercent,
                    int rainRaw, bool rainDetected,
                    bool pumpState) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Firebase cancelled: WiFi not connected.");
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15000);

  HTTPClient http;
  http.setTimeout(15000);
  http.setReuse(false);

  bool beginOK = http.begin(client, firebaseURL);

  if (!beginOK) {
    Serial.println("Firebase Error: http.begin() failed. Check Firebase URL.");
    return;
  }

  http.addHeader("Content-Type", "application/json");

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
  String wifiStatus = WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected";

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
  jsonData += "\"relay_pin\":23";
  jsonData += "}";
  jsonData += "},";

  jsonData += "\"system\":{";
  jsonData += "\"status\":\"online\",";
  jsonData += "\"wifi_status\":\"" + wifiStatus + "\",";
  jsonData += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  jsonData += "\"last_update_unix\":" + String(time(nullptr));
  jsonData += "}";

  jsonData += "}";

  Serial.println("Sending data to Firebase using PATCH...");
  Serial.println(jsonData);

  int httpResponseCode = http.PATCH(jsonData);

  Serial.print("Firebase Response Code: ");
  Serial.println(httpResponseCode);

  if (httpResponseCode == 200) {
    Serial.println("Firebase: Data sent successfully.");
  } else if (httpResponseCode > 0) {
    Serial.print("Firebase response: ");
    Serial.println(http.getString());
  } else {
    Serial.print("Firebase Error: ");
    Serial.println(http.errorToString(httpResponseCode));
  }

  http.end();
  client.stop(); // Stop the previous connection completely before starting a new one
  
  // FETCH REMOTE COMMANDS
  String controlURL = firebaseURL;
  controlURL.replace(".json", "/control.json");
  if (http.begin(client, controlURL)) {
    int getResponseCode = http.GET();
    if (getResponseCode == 200) {
      String payload = http.getString();
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
        }
      }
    }
    http.end();
  }

  client.stop();
}
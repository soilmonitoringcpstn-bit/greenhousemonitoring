#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <WebServer.h>
#include <DNSServer.h>

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
int soilDryValue = 4095;
int soilWetValue = 1200;

// ===== RAIN CALIBRATION =====
// Below this value = rain detected
int rainThreshold = 3800;

// ===== PUMP THRESHOLDS =====
int pumpOnThreshold = 15;    // Pump ON only below 15%
int pumpOffThreshold = 55;   // Pump OFF at 55% and above

bool pumpState = false;

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
  html += "<title>ESP32 Greenhouse Monitor</title>";
  html += "<style>body{font-family:Arial,sans-serif; margin:20px; background:#f4f7f6; color:#333;}";
  html += "h2{color:#2e7d32; text-align:center;}";
  html += ".card{background:#fff; border:1px solid #ddd; padding:15px; margin-bottom:10px; border-radius:8px; box-shadow:0 2px 4px rgba(0,0,0,0.05);}";
  html += ".val{font-weight:bold; color:#1565c0; float:right;}";
  html += ".btn{display:inline-block; padding:10px 15px; margin:5px; border-radius:5px; border:none; color:white; cursor:pointer; font-weight:bold;}";
  html += ".btn-auto{background:#1565c0;} .btn-on{background:#2e7d32;} .btn-off{background:#d32f2f;}";
  html += "</style></head><body>";
  html += "<h2>Greenhouse Monitor</h2>";
  html += "<div class=\"card\"><strong>Temperature:</strong> <span class=\"val\" id=\"temp\">--</span></div>";
  html += "<div class=\"card\"><strong>Humidity:</strong> <span class=\"val\" id=\"hum\">--</span></div>";
  html += "<div class=\"card\"><strong>Soil Moisture:</strong> <span class=\"val\" id=\"soil\">--</span></div>";
  html += "<div class=\"card\"><strong>Rain Detected:</strong> <span class=\"val\" id=\"rain\">--</span></div>";
  html += "<div class=\"card\"><strong>Pump Relay:</strong> <span class=\"val\" id=\"pump\">--</span></div>";
  html += "<div class=\"card\"><strong>System Mode:</strong> <span class=\"val\" id=\"mode\">--</span></div>";
  
  html += "<h3>Pump Control</h3>";
  html += "<button class=\"btn btn-auto\" onclick=\"setMode('auto','')\">Auto Mode</button>";
  html += "<button class=\"btn btn-on\" onclick=\"setMode('manual','on')\">Manual ON</button>";
  html += "<button class=\"btn btn-off\" onclick=\"setMode('manual','off')\">Manual OFF</button>";
  html += "<h3>Auto Thresholds</h3>";
  html += "<div class=\"card\">";
  html += "<label>Turn ON below (%): <input type=\"number\" id=\"inpOn\" style=\"width:60px\"></label><br><br>";
  html += "<label>Turn OFF above (%): <input type=\"number\" id=\"inpOff\" style=\"width:60px\"></label><br><br>";
  html += "<button class=\"btn btn-auto\" onclick=\"setThresh()\">Update Thresholds</button>";
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

  // ===== PUMP LOGIC =====
  if (manualMode) {
    if (pumpState != manualPumpState) {
      pumpState = manualPumpState;
      Serial.print("Manual Mode: Pump ");
      Serial.println(pumpState ? "ON" : "OFF");
    }
    dryStartTime = 0;
  } else {
    // ===== PUMP LOGIC WITH DELAY + HYSTERESIS =====
    // If soil is below 15% and no rain, start counting
    if (soilPercent < pumpOnThreshold && !rainDetected) {
      if (dryStartTime == 0) {
        dryStartTime = millis();
        Serial.println("Soil below 15%. Dry timer started...");
      }

      // Pump ON only if soil stays dry for 10 seconds
      if (!pumpState && millis() - dryStartTime >= dryDelay) {
        pumpState = true;
        Serial.println("Soil stayed dry for 10 seconds. Pump ON.");
      }
    } else {
      // Reset timer if soil is not dry anymore
      dryStartTime = 0;
    }

    // Pump OFF when soil reaches 55% or rain is detected
    if (pumpState && (soilPercent >= pumpOffThreshold || rainDetected)) {
      pumpState = false;
      dryStartTime = 0;
      Serial.println("Pump OFF: Soil is moist enough or rain detected.");
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
  jsonData += "\"last_update_ms\":" + String(millis());
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
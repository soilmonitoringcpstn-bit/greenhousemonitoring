#define TINY_GSM_MODEM_A7672X
#define TINY_GSM_DEBUG Serial
#include <TinyGsmClient.h>

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <DHT.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
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
const char apn[]  = "internet.globe.com.ph"; // Globe APN
const char gprsUser[] = "";
const char gprsPass[] = "";

TinyGsm modem(SerialAT);

bool cellularActive = false;
bool cloudAvailable = false;
bool cloudAttempted = false;
unsigned long lastCloudAttempt = 0;
const unsigned long cloudRetryInterval = 300000UL; // 5 minutes
String backupWifiSsid = "";
String backupWifiPassword = "";
bool wifiBackupConfigured = false;
bool wifiBackupActive = false;
unsigned long lastWifiAttempt = 0;
const unsigned long wifiRetryInterval = 60000UL;

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
String lastProcessedCommandId = "";
int64_t lastProcessedIssuedAt = 0;

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
String pumpStopReason = "startup";

// ===== NTP TIME SETTINGS =====
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 8 * 3600; // UTC+8
const int   daylightOffset_sec = 0;

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

// ===== FIREBASE SEND INTERVAL =====
unsigned long lastSend = 0;
unsigned long sendInterval = 10000; // send every 10 seconds

const char* SETTINGS_NAMESPACE = "greenhouse";
const char* CONTROL_COMMAND_URL =
  "https://soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app/control/command.json";
const char* CONTROL_ACK_URL =
  "https://soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app/control/ack.json";

bool sensorFaultActive() {
  return currentDhtError || currentSoilRaw <= 0 || currentSoilRaw >= 4095;
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
    if (!clockIsValid()) return true;
    if (cooldownUntilUnix < 0) {
      persistCooldown();
      return true;
    }
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
  if (cooldownUntilUnix < 0) return cooldownTime / 1000UL;
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
  return wetValue > 0 && dryValue < 4095 && wetValue < dryValue &&
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
  lastProcessedCommandId = preferences.getString("lastCommand", "");
  lastProcessedIssuedAt = preferences.getLong64("lastIssued", 0);
  cooldownUntilUnix = preferences.getLong64("cooldownUntil", 0);
  if (preferences.getBool("pumpWasOn", false)) {
    // A reset or power loss interrupted a pump run. Require a full cooldown
    // after the clock is restored, and ensure the relay remains off.
    cooldownUntilUnix = -1;
    preferences.putLong64("cooldownUntil", cooldownUntilUnix);
    preferences.putBool("pumpWasOn", false);
  }
  backupWifiSsid = preferences.getString("wifiSsid", "");
  backupWifiPassword = preferences.getString("wifiPass", "");
  wifiBackupConfigured = backupWifiSsid.length() > 0;

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
  if (!force && lastWifiAttempt != 0 && millis() - lastWifiAttempt < wifiRetryInterval) return false;

  lastWifiAttempt = millis();
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(backupWifiSsid.c_str(), backupWifiPassword.c_str());
  Serial.print("Connecting Wi-Fi backup: ");
  Serial.println(backupWifiSsid);

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000UL) {
    servicePortal();
    managePumpSafety();
    delay(10);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi backup connected: ");
    Serial.println(WiFi.localIP());
    return true;
  }

  Serial.println("Wi-Fi backup connection failed; captive portal remains active.");
  return false;
}

// ===== HELPER FUNCTIONS TO KEEP PORTAL ALIVE =====
void servicePortal() {
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

const char PORTAL_HTML[] PROGMEM = R"PORTAL(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="theme-color" content="#063c32"><title>Greenhouse Control</title><style>
:root{color-scheme:dark;--bg:#071b18;--panel:#0d2924;--line:#245046;--mint:#4ade80;--text:#eefbf5;--muted:#9fc5b7;--red:#ef4444;--amber:#f59e0b;--blue:#3b82f6}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at top,#12463b 0,var(--bg) 45%);color:var(--text);font:15px system-ui,sans-serif;min-height:100vh}.wrap{width:min(920px,100%);margin:auto;padding:24px 16px 48px}header{display:flex;justify-content:space-between;gap:16px;align-items:center;margin-bottom:18px}h1{font-size:clamp(22px,5vw,34px);margin:0}h2{font-size:17px;margin:28px 0 12px}.eyebrow{color:var(--mint);font-size:12px;font-weight:800;letter-spacing:.12em;text-transform:uppercase}.online{padding:8px 12px;border:1px solid var(--line);border-radius:999px;color:var(--muted)}.online.ok{color:var(--mint)}.online.bad{color:#fca5a5}.alert{display:none;padding:14px;border-radius:12px;background:#4b1616;border:1px solid #8e2d2d;margin-bottom:14px}.alert.show{display:block}.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}.card,.panel{background:var(--panel);background:color-mix(in srgb,var(--panel) 94%,white 6%);border:1px solid var(--line);border-radius:16px;box-shadow:0 12px 30px #0004}.card{padding:16px}.label{display:block;color:var(--muted);font-size:12px;margin-bottom:8px}.value{font-weight:800;font-size:20px}.panel{padding:18px}.buttons{display:grid;grid-template-columns:repeat(5,1fr);gap:9px}button{border:0;border-radius:12px;padding:13px 10px;color:white;background:#215f51;font-weight:750;cursor:pointer}button:disabled{opacity:.5;cursor:wait}.on{background:#16733c}.off,.emergency{background:#a52a2a}.emergency{outline:2px solid #f87171}.reset{background:#9a5b08}.form{display:grid;grid-template-columns:repeat(3,1fr);gap:12px}.field label{display:block;color:var(--muted);font-size:12px;margin-bottom:6px}.field input{width:100%;border:1px solid var(--line);border-radius:10px;background:#071b18;color:var(--text);padding:11px;font:inherit}.save{margin-top:14px;width:100%;background:#176b5a}.message{min-height:22px;margin:12px 0 0;color:var(--muted)}.message.error{color:#fca5a5}.hint{color:var(--muted);font-size:12px;line-height:1.5}@media(max-width:700px){.grid{grid-template-columns:repeat(2,1fr)}.buttons{grid-template-columns:repeat(2,1fr)}.emergency{grid-column:1/-1}.form{grid-template-columns:1fr}header{align-items:flex-start;flex-direction:column}}
</style></head><body><main class="wrap"><header><div><div class="eyebrow">ESP32 local access</div><h1>Greenhouse Control</h1></div><div id="connection" class="online">Connecting...</div></header>
<div id="safetyAlert" class="alert" role="alert"></div><section class="grid" aria-label="Live readings">
<div class="card"><span class="label">Soil moisture</span><span id="soil" class="value">--</span></div><div class="card"><span class="label">Temperature</span><span id="temp" class="value">--</span></div><div class="card"><span class="label">Humidity</span><span id="hum" class="value">--</span></div><div class="card"><span class="label">Rain</span><span id="rain" class="value">--</span></div><div class="card"><span class="label">Pump</span><span id="pump" class="value">--</span></div><div class="card"><span class="label">Mode</span><span id="mode" class="value">--</span></div><div class="card"><span class="label">Cooldown</span><span id="cooldown" class="value">--</span></div><div class="card"><span class="label">Cloud route</span><span id="network" class="value">--</span></div></section>
<h2>Pump control</h2><section class="panel"><div class="buttons"><button data-mode="auto">Auto / Resume</button><button class="on" data-mode="manual" data-state="on">Pump ON</button><button class="off" data-mode="manual" data-state="off">Pump OFF</button><button class="reset" data-mode="reset_cd">Reset cooldown</button><button class="emergency" data-mode="emergency_off">Emergency OFF</button></div><p id="message" class="message" aria-live="polite"></p><p class="hint">All starts are still subject to rain, sensor, cooldown and two-minute safety protection. Auto / Resume clears a latched emergency stop.</p></section>
<h2>Automation thresholds</h2><section class="panel"><div class="form"><div class="field"><label for="onThreshold">Turn pump on below (%)</label><input id="onThreshold" type="number" min="0" max="99"></div><div class="field"><label for="offThreshold">Turn pump off at (%)</label><input id="offThreshold" type="number" min="1" max="100"></div></div><button id="saveThresholds" class="save">Save thresholds</button><p class="hint">Required: 0 <= ON threshold < OFF threshold <= 100.</p></section>
<h2>Sensor calibration</h2><section class="panel"><div class="form"><div class="field"><label for="soilWet">Soil wet raw value</label><input id="soilWet" type="number" min="1" max="4093"></div><div class="field"><label for="soilDry">Soil dry raw value</label><input id="soilDry" type="number" min="2" max="4094"></div><div class="field"><label for="rainThreshold">Rain threshold</label><input id="rainThreshold" type="number" min="1" max="4094"></div></div><button id="saveCalibration" class="save">Save calibration</button><p class="hint">Required: soil wet < soil dry. Changes are saved in ESP32 nonvolatile storage.</p></section></main>
<h2>Wi-Fi cloud backup</h2><section class="panel"><div class="form"><div class="field"><label for="wifiSsid">Wi-Fi name (SSID)</label><input id="wifiSsid" maxlength="32" autocomplete="off"></div><div class="field"><label for="wifiPassword">Wi-Fi password</label><input id="wifiPassword" type="password" minlength="8" maxlength="63" autocomplete="new-password" placeholder="Leave blank to keep saved password"></div></div><button id="saveWifi" class="save">Save and connect Wi-Fi</button><p id="wifiStatus" class="hint">Not configured</p><p class="hint">The Greenhouse_Portal access point stays available while the ESP32 connects to this router.</p></section></main>
<script>
const LOCAL_ORIGIN='http://192.168.4.1';const $=id=>document.getElementById(id),editable=id=>document.activeElement!==$(id);let busy=false;
function text(id,value){$(id).textContent=value}function setMessage(value,error=false){text('message',value);$('message').classList.toggle('error',error)}function setBusy(value){busy=value;document.querySelectorAll('button').forEach(button=>button.disabled=value)}
async function request(path,options={}){const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),4000);try{const response=await fetch(LOCAL_ORIGIN+path,{cache:'no-store',signal:controller.signal,...options});const body=await response.text();if(!response.ok)throw new Error(body||('HTTP '+response.status));return body}finally{clearTimeout(timer)}}
async function refresh(){try{const d=JSON.parse(await request('/data')),remaining=Math.max(0,d.cooldownRemainingSeconds||0),hours=Math.floor(remaining/3600),minutes=Math.ceil((remaining%3600)/60);text('soil',d.soilPercent+'% ('+d.soilRaw+')');text('temp',d.dhtError?'Sensor error':d.temperature+' C');text('hum',d.dhtError?'Sensor error':d.humidity+'%');text('rain',d.rainDetected?'Detected':'Clear');text('pump',d.pumpState?'ON':'OFF');text('mode',d.emergencyStop?'EMERGENCY':(d.manualMode?'MANUAL':'AUTO'));text('cooldown',d.cooldown?(hours+'h '+minutes+'m'):'Ready');text('network',d.network);text('wifiStatus',d.wifiConnected?'Connected to '+d.wifiSsid:(d.wifiConfigured?'Saved: '+d.wifiSsid+' (not connected)':'Not configured'));$('connection').textContent='Live';$('connection').className='online ok';const fault=d.emergencyStop||d.rainDetected||d.dhtError||d.soilFault;$('safetyAlert').classList.toggle('show',fault);$('safetyAlert').textContent=d.emergencyStop?'Emergency stop is latched.':d.rainDetected?'Rain detected; pump starts are blocked.':d.dhtError?'DHT22 fault; pump starts are blocked.':d.soilFault?'Soil sensor fault; pump starts are blocked.':'';if(editable('onThreshold'))$('onThreshold').value=d.onThresh;if(editable('offThreshold'))$('offThreshold').value=d.offThresh;if(editable('soilWet'))$('soilWet').value=d.soilWet;if(editable('soilDry'))$('soilDry').value=d.soilDry;if(editable('rainThreshold'))$('rainThreshold').value=d.rainThreshold;if(editable('wifiSsid'))$('wifiSsid').value=d.wifiSsid||''}catch(error){$('connection').textContent='Offline';$('connection').className='online bad';setMessage(error.message,true)}}
async function update(params,confirmText=''){if(busy)return;if(confirmText&&!confirm(confirmText))return;setBusy(true);setMessage('Applying...');try{await request('/set',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(params)});setMessage('Saved and applied by ESP32.');await refresh()}catch(error){setMessage(error.message,true)}finally{setBusy(false)}}
document.querySelectorAll('[data-mode]').forEach(button=>button.onclick=()=>update({mode:button.dataset.mode,state:button.dataset.state||''},button.dataset.mode==='emergency_off'?'Stop the pump and latch emergency mode?':button.dataset.mode==='reset_cd'?'Reset the five-hour safety cooldown?':'');
$('saveThresholds').onclick=()=>{const on=+$('onThreshold').value,off=+$('offThreshold').value;if(!Number.isInteger(on)||!Number.isInteger(off)||on<0||off>100||on>=off)return setMessage('Thresholds must satisfy 0 <= ON < OFF <= 100.',true);update({on_thresh:on,off_thresh:off})};
$('saveCalibration').onclick=()=>{const wet=+$('soilWet').value,dry=+$('soilDry').value,rain=+$('rainThreshold').value;if(!Number.isInteger(wet)||!Number.isInteger(dry)||!Number.isInteger(rain)||wet<=0||wet>=dry||dry>=4095||rain<=0||rain>=4095)return setMessage('Calibration values are invalid.',true);update({soil_wet:wet,soil_dry:dry,rain_threshold:rain})};
$('saveWifi').onclick=()=>{const ssid=$('wifiSsid').value.trim(),password=$('wifiPassword').value;if(!ssid||ssid.length>32)return setMessage('Enter a valid Wi-Fi name.',true);if(password&&password.length<8)return setMessage('Wi-Fi password must be at least 8 characters.',true);update({wifi_ssid:ssid,wifi_password:password});$('wifiPassword').value=''};
refresh();setInterval(refresh,2000);
</script></body></html>
)PORTAL";

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", PORTAL_HTML);
}

void handleData() {
  String json = "{";
  String localNetworkStatus = wifiBackupActive && WiFi.status() == WL_CONNECTED ?
                              "Wi-Fi backup" : (!cellularActive ? "Cellular disconnected" :
                              (cloudAvailable ? "Cellular + cloud" : "Cellular; cloud unavailable"));
  json += "\"network\":\"" + localNetworkStatus + "\",";
  json += "\"cloudAvailable\":" + String(cloudAvailable ? "true" : "false") + ",";
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
  json += "\"soilFault\":" + String((currentSoilRaw <= 0 || currentSoilRaw >= 4095) ? "true" : "false") + ",";
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
      response = "Calibration requires 0 < soil wet < soil dry < 4095 and 0 < rain < 4095";
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
      WiFi.mode(WIFI_AP_STA);
      WiFi.begin(backupWifiSsid.c_str(), backupWifiPassword.c_str());
      lastWifiAttempt = millis();
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

void setup() {
  Serial.begin(115200);
  delay(1000);

  preferences.begin(SETTINGS_NAMESPACE, false);
  loadSettings();

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

    // ===== SEND TO FIREBASE =====
    if (millis() - lastSend >= sendInterval) {
      lastSend = millis();

      bool cloudRetryDue = !cloudAttempted ||
                           (millis() - lastCloudAttempt >= cloudRetryInterval);
      bool modemConnected = cellularActive && modem.isNetworkConnected();
      bool wifiConnected = WiFi.status() == WL_CONNECTED;

      if (!cloudAvailable && wifiBackupConfigured && !wifiConnected) {
        wifiConnected = connectWifiBackup(false);
      }
      if (!cloudAvailable && wifiConnected) wifiBackupActive = true;

      if (!modemConnected && cloudRetryDue) {
        Serial.println("Cellular disconnected. Retrying after backoff...");
        connectNetwork(true);
        modemConnected = cellularActive && modem.isNetworkConnected();
        cloudRetryDue = !cloudAttempted ||
                        (millis() - lastCloudAttempt >= cloudRetryInterval);
      }

      if ((wifiBackupActive && wifiConnected && (cloudAvailable || cloudRetryDue)) ||
          (!wifiBackupActive && modemConnected && (cloudAvailable || cloudRetryDue))) {
        lastCloudAttempt = millis();
        cloudAttempted = true;
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
      } else if (!cloudAvailable) {
        unsigned long waitMs = cloudRetryInterval - (millis() - lastCloudAttempt);
        Serial.print("Cloud unavailable; local portal remains active. Retry in ");
        Serial.print(waitMs / 1000UL);
        Serial.println(" seconds.");
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

  if (!prepareNativeHttps(url)) {
    return false;
  }

  String response = sendA7670AT(
    "AT+HTTPACTION=0",
    60000UL,
    "+HTTPACTION:"
  );

  bool success = responseContainsHttpSuccess(response, 0);

  String readResponse = sendA7670AT(
    "AT+HTTPREAD",
    15000UL,
    "+HTTPREAD:"
  );

  responseBody = extractHttpReadBody(readResponse);
  sendA7670AT("AT+HTTPTERM", 3000UL);

  Serial.print("Firebase GET body: ");
  Serial.println(responseBody);

  return success;
}

String buildCommandAck(const String &commandId, const String &status,
                       const String &reason) {
  StaticJsonDocument<384> ack;
  ack["command_id"] = commandId;
  ack["status"] = status;
  ack["reason"] = reason;
  ack["executed_at"] = (long long)time(nullptr);
  ack["pump_on"] = pumpState;
  ack["emergency_stop"] = emergencyStopLatched;
  String result;
  serializeJson(ack, result);
  return result;
}

String applyRemoteControlPayload(String payload) {
  payload.trim();

  if (payload.length() == 0 || payload == "null") return "";

  Serial.print("Firebase Control Payload: ");
  Serial.println(payload);

  StaticJsonDocument<768> command;
  DeserializationError jsonError = deserializeJson(command, payload);
  if (jsonError) return buildCommandAck("unknown", "rejected", "malformed_json");

  const char* commandIdValue = command["command_id"];
  const char* modeValue = command["mode"];
  if (!commandIdValue || !modeValue) {
    return buildCommandAck(commandIdValue ? commandIdValue : "unknown",
                           "rejected", "missing_required_fields");
  }

  String commandId = commandIdValue;
  String mode = modeValue;
  if (commandId.length() < 8 || commandId.length() > 80) {
    return buildCommandAck(commandId, "rejected", "invalid_command_id");
  }

  if (!command["issued_at"].is<long long>() ||
      !command["expires_at"].is<long long>()) {
    return buildCommandAck(commandId, "rejected", "missing_or_invalid_timestamp");
  }

  time_t now = time(nullptr);
  long long issuedAt = command["issued_at"].as<long long>();
  long long expiresAt = command["expires_at"].as<long long>();
  if (now < 1609459200) {
    return buildCommandAck(commandId, "rejected", "device_time_not_synchronized");
  }
  if (expiresAt <= issuedAt || now > expiresAt || issuedAt > (long long)now + 60) {
    return buildCommandAck(commandId, "rejected", "expired_or_invalid_timestamp");
  }

  if (commandId == lastProcessedCommandId) {
    return buildCommandAck(commandId, "executed", "already_processed");
  }
  if (issuedAt <= lastProcessedIssuedAt) {
    return buildCommandAck(commandId, "rejected", "replayed_or_superseded_command");
  }

  String validationError = "";
  if (mode == "manual" && !command["state"].is<bool>()) {
    validationError = "manual_state_must_be_boolean";
  } else if (mode == "settings") {
    if (!command["pump_on_threshold"].is<int>() ||
        !command["pump_off_threshold"].is<int>() ||
        !validThresholds(command["pump_on_threshold"].as<int>(),
                         command["pump_off_threshold"].as<int>())) {
      validationError = "thresholds_must_satisfy_0_le_on_lt_off_le_100";
    }
  } else if (mode != "manual" && mode != "auto" &&
             mode != "reset_cd" && mode != "emergency_off") {
    validationError = "unsupported_mode";
  }

  if (validationError.length() > 0) {
    return buildCommandAck(commandId, "rejected", validationError);
  }

  // Persist before execution. If power fails mid-command, a pump command is
  // never replayed after reboot.
  lastProcessedCommandId = commandId;
  lastProcessedIssuedAt = issuedAt;
  preferences.putString("lastCommand", lastProcessedCommandId);
  preferences.putLong64("lastIssued", lastProcessedIssuedAt);

  String executionReason = "completed";
  String status = "executed";
  if (mode == "manual") {
    manualMode = true;
    manualPumpState = command["state"].as<bool>();
    if (manualPumpState) {
      if (!startPumpSafely("remote_manual", executionReason)) status = "rejected";
    } else {
      stopPump("remote_manual_off");
    }
  } else if (mode == "auto") {
    manualMode = false;
    manualPumpState = false;
    emergencyStopLatched = false;
    dryStartTime = 0;
  } else if (mode == "reset_cd") {
    clearCooldown();
    Serial.println("Cooldown forcefully reset via Web Dashboard.");
  } else if (mode == "emergency_off") {
    emergencyStopLatched = true;
    manualMode = true;
    stopPump("emergency_stop");
  } else if (mode == "settings") {
    pumpOnThreshold = command["pump_on_threshold"].as<int>();
    pumpOffThreshold = command["pump_off_threshold"].as<int>();
    saveSettings();
  }

  return buildCommandAck(commandId, status, executionReason);
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
  jsonData += "\"cooldown_active\":" + String(cooldownActive() ? "true" : "false") + ",";
  jsonData += "\"cooldown_remaining_seconds\":" + String(cooldownRemainingSeconds()) + ",";
  jsonData += "\"emergency_stop\":" + String(emergencyStopLatched ? "true" : "false") + ",";
  jsonData += "\"stop_reason\":\"" + pumpStopReason + "\",";
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

  const String serverName =
    "soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app";
  const String patchUrl = "https://" + serverName + "/greenhouse.json";
  const String controlUrl = CONTROL_COMMAND_URL;
  const String ackUrl = CONTROL_ACK_URL;

  Serial.println("Sending data with A7670E native HTTPS PATCH...");

  if (cellularActive && !wifiBackupActive) {
    String patchBody;
    bool patchOK = nativeFirebasePatch(patchUrl, jsonData, patchBody);

    if (patchOK) {
      cloudAvailable = true;
      Serial.println("Firebase cellular PATCH successful.");
    } else {
      cloudAvailable = false;
      Serial.println("Firebase cellular PATCH failed.");

      if (connectWifiBackup(false)) {
        wifiBackupActive = true;
        Serial.println("Wi-Fi backup ready; Firebase will retry over Wi-Fi.");
      }

      // Recheck registration/data on the next send cycle.
      if (!modem.isNetworkConnected()) {
        cellularActive = false;
      }
    }

    // Native HTTPTERM fully releases the A7670E HTTP/TLS service, so a
    // separate GET can be performed without restarting the modem.
    String controlPayload;
    bool getOK = nativeFirebaseGet(controlUrl, controlPayload);

    if (getOK) {
      String ackJson = applyRemoteControlPayload(controlPayload);
      if (ackJson.length() > 0) {
        String ackResponse;
        if (!nativeFirebasePatch(ackUrl, ackJson, ackResponse)) {
          Serial.println("Firebase command acknowledgement failed.");
        }
      }
    } else {
      Serial.println("Firebase cellular GET control failed.");
      if (!modem.isNetworkConnected()) {
        cellularActive = false;
      }
    }

    return;
  }

  // Wi-Fi fallback retained from the original greenhouse project.
  HTTPClient httpWiFi;
  httpWiFi.setTimeout(15000);
  httpWiFi.setReuse(false);

  WiFiClientSecure wifiClient;
  wifiClient.setInsecure();
  wifiClient.setTimeout(15000);

  if (!httpWiFi.begin(wifiClient, patchUrl)) {
    Serial.println("Firebase Error: Wi-Fi http.begin() failed.");
    return;
  }

  httpWiFi.addHeader("Content-Type", "application/json");
  int httpResponseCode = httpWiFi.PATCH(jsonData);

  Serial.print("Firebase Wi-Fi PATCH code: ");
  Serial.println(httpResponseCode);

  if (httpResponseCode >= 200 && httpResponseCode < 300) {
    cloudAvailable = true;
    wifiBackupActive = true;
    Serial.println("Firebase Wi-Fi backup PATCH successful.");
    Serial.println(httpWiFi.getString());
  } else if (httpResponseCode > 0) {
    cloudAvailable = false;
    Serial.println(httpWiFi.getString());
  } else {
    cloudAvailable = false;
    Serial.print("Firebase Wi-Fi error: ");
    Serial.println(httpWiFi.errorToString(httpResponseCode));
  }
  httpWiFi.end();

  if (httpWiFi.begin(wifiClient, controlUrl)) {
    int getResponseCode = httpWiFi.GET();
    if (getResponseCode == 200) {
      String ackJson = applyRemoteControlPayload(httpWiFi.getString());
      httpWiFi.end();
      if (ackJson.length() > 0 && httpWiFi.begin(wifiClient, ackUrl)) {
        httpWiFi.addHeader("Content-Type", "application/json");
        int ackCode = httpWiFi.PATCH(ackJson);
        Serial.print("Firebase acknowledgement code: ");
        Serial.println(ackCode);
      }
    }
    httpWiFi.end();
  }

  wifiClient.stop();
}

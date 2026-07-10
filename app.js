const FIREBASE_URL =
  "https://soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app/greenhouse.json";

const STALE_AFTER_MS = 3 * 60 * 1000;
const SIGNATURE_STORAGE_KEY = "greenhouseLatestSignature";
const CHANGED_AT_STORAGE_KEY = "greenhouseLastDataChangedAt";
const AUTH_STORAGE_KEY = "greenhouseDashboardLoggedIn";
const LOGIN_USERNAME = "admin";
const LOGIN_PASSWORD = "greenhouse";

let latestData = null;
let latestSignature = localStorage.getItem(SIGNATURE_STORAGE_KEY) || "";
let lastDataChangedAt = Number(localStorage.getItem(CHANGED_AT_STORAGE_KEY)) || 0;
let realtimeStarted = false;

const elements = {
  loginScreen: document.querySelector("#loginScreen"),
  loginForm: document.querySelector("#loginForm"),
  usernameInput: document.querySelector("#usernameInput"),
  passwordInput: document.querySelector("#passwordInput"),
  loginError: document.querySelector("#loginError"),
  appShell: document.querySelector("#appShell"),
  logoutButton: document.querySelector("#logoutButton"),
  connectionText: document.querySelector("#connectionText"),
  soilPercent: document.querySelector("#soilPercent"),
  soilRaw: document.querySelector("#soilRaw"),
  soilStatus: document.querySelector("#soilStatus"),
  temperatureValue: document.querySelector("#temperatureValue"),
  temperatureError: document.querySelector("#temperatureError"),
  humidityValue: document.querySelector("#humidityValue"),
  humidityError: document.querySelector("#humidityError"),
  pumpValue: document.querySelector("#pumpValue"),
  rainValue: document.querySelector("#rainValue"),
  rainRaw: document.querySelector("#rainRaw"),
  systemStatus: document.querySelector("#systemStatus"),
  wifiStatus: document.querySelector("#wifiStatus"),
  wifiRssi: document.querySelector("#wifiRssi"),
  firebaseStatus: document.querySelector("#firebaseStatus"),
  lastUpdate: document.querySelector("#lastUpdate"),
  latestRecord: document.querySelector("#latestRecord"),
  errorPanel: document.querySelector("#errorPanel"),
  errorMessage: document.querySelector("#errorMessage"),
  lastSync: document.querySelector("#lastSync"),
};

function getByPath(source, path) {
  return path.split(".").reduce((value, key) => {
    if (value === null || value === undefined) return undefined;
    return value[key];
  }, source);
}

function getFirstValue(source, paths) {
  for (const path of paths) {
    const value = getByPath(source, path);
    if (value !== undefined && value !== null) return value;
  }
  return undefined;
}

function formatNumber(value, digits = 1) {
  const number = Number(value);
  if (!Number.isFinite(number)) return "--";
  return Number.isInteger(number) ? String(number) : number.toFixed(digits);
}

function formatBooleanStatus(value, trueText = "ON", falseText = "OFF") {
  if (typeof value === "boolean") return value ? trueText : falseText;
  if (value === undefined || value === null || value === "") return "--";
  return String(value);
}

function formatRainStatus(value) {
  if (typeof value === "boolean") return value ? "Rain Detected" : "No Rain";
  return value ? String(value) : "--";
}

function formatTime(value) {
  const number = Number(value);
  if (!Number.isFinite(number)) return "--";
  if (number > 10000000000) return new Date(number).toLocaleString();
  return `${number} ms`;
}

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#039;");
}

function flattenRecord(source, prefix = "") {
  return Object.entries(source || {}).flatMap(([key, value]) => {
    const label = prefix ? `${prefix} ${key}` : key;
    if (value && typeof value === "object" && !Array.isArray(value)) {
      return flattenRecord(value, label);
    }
    return [[label, value]];
  });
}

function formatLabel(value) {
  return value
    .replace(/_/g, " ")
    .replace(/\b\w/g, (letter) => letter.toUpperCase());
}

function setConnectionStatus(text, type = "neutral") {
  elements.connectionText.textContent = text;
  elements.connectionText.className = `status-badge ${type}`;
}

function getFreshnessType() {
  if (!lastDataChangedAt) return "neutral";
  return Date.now() - lastDataChangedAt > STALE_AFTER_MS ? "error" : "success";
}

function getDataSignature(data) {
  return JSON.stringify(data || {});
}

function getReadings(data) {
  const dhtError = getFirstValue(data, ["sensors.dht22.error", "dht_error"]);
  return {
    soilRaw: getFirstValue(data, ["sensors.soil.raw_value", "soil_raw"]),
    soilPercent: getFirstValue(data, ["sensors.soil.moisture_percent", "soil_moisture_percent"]),
    soilStatus: getFirstValue(data, ["sensors.soil.status", "soil_status"]),
    temperature: getFirstValue(data, ["sensors.dht22.temperature_celsius", "temperature"]),
    humidity: getFirstValue(data, ["sensors.dht22.humidity_percent", "humidity"]),
    dhtError,
    rainRaw: getFirstValue(data, ["sensors.rain.raw_value", "rain_raw"]),
    rainStatus: getFirstValue(data, ["sensors.rain.status", "sensors.rain.detected", "rain_status", "rain_detected"]),
    pumpStatus: getFirstValue(data, ["actuator.pump.status", "actuator.pump.is_on", "pump_on"]),
    systemStatus: getFirstValue(data, ["system.status", "system_status"]),
    wifiStatus: getFirstValue(data, ["system.wifi_status", "wifi_status"]),
    wifiRssi: getFirstValue(data, ["system.wifi_rssi", "wifi_rssi"]),
    lastUpdate: getFirstValue(data, ["system.last_update_ms", "last_update_ms"]),
  };
}

function renderLatestRecord(data) {
  elements.latestRecord.innerHTML = flattenRecord(data)
    .map(
      ([key, value]) => `
        <div>
          <dt>${escapeHtml(formatLabel(key))}</dt>
          <dd>${escapeHtml(typeof value === "boolean" ? String(value) : value ?? "--")}</dd>
        </div>
      `
    )
    .join("");
}

function renderDashboard(data) {
  if (!data || typeof data !== "object") return;
  latestData = data;

  const signature = getDataSignature(data);
  if (signature !== latestSignature) {
    latestSignature = signature;
    lastDataChangedAt = Date.now();
    localStorage.setItem(SIGNATURE_STORAGE_KEY, latestSignature);
    localStorage.setItem(CHANGED_AT_STORAGE_KEY, String(lastDataChangedAt));
  }

  const readings = getReadings(data);
  const dhtHasError = readings.dhtError === true || String(readings.dhtError).toLowerCase() === "true";

  elements.soilPercent.textContent = `${formatNumber(readings.soilPercent, 0)}%`;
  elements.soilRaw.textContent = readings.soilRaw ?? "--";
  elements.soilStatus.textContent = readings.soilStatus || "--";

  elements.temperatureValue.textContent = dhtHasError ? "Error" : `${formatNumber(readings.temperature)} deg C`;
  elements.temperatureError.textContent = dhtHasError ? "Sensor error" : "Normal";
  elements.humidityValue.textContent = dhtHasError ? "Error" : `${formatNumber(readings.humidity)}%`;
  elements.humidityError.textContent = dhtHasError ? "Sensor error" : "Normal";

  elements.pumpValue.textContent = formatBooleanStatus(readings.pumpStatus);
  elements.rainValue.textContent = formatRainStatus(readings.rainStatus);
  elements.rainRaw.textContent = readings.rainRaw ?? "--";

  elements.systemStatus.textContent = readings.systemStatus || "--";
  elements.wifiStatus.textContent = readings.wifiStatus || "--";
  elements.wifiRssi.textContent = readings.wifiRssi !== undefined ? `${readings.wifiRssi} dBm` : "--";
  elements.firebaseStatus.textContent = "Connected";
  elements.lastUpdate.textContent = formatTime(readings.lastUpdate);
  elements.lastSync.textContent = new Date().toLocaleString();

  renderLatestRecord(data);
  elements.errorPanel.hidden = true;
  setConnectionStatus(getFreshnessType() === "error" ? "Offline" : "Connected", getFreshnessType());
}

function renderError(error) {
  setConnectionStatus("Error", "error");
  elements.firebaseStatus.textContent = "Error";
  elements.errorMessage.textContent = error.message || "Unable to read Firebase data.";
  elements.errorPanel.hidden = false;
  elements.latestRecord.innerHTML = `
    <div>
      <dt>Firebase Error</dt>
      <dd>${escapeHtml(error.message || "Unable to read Firebase data.")}</dd>
    </div>
  `;
}

async function loadData() {
  try {
    setConnectionStatus("Connecting", "neutral");
    const response = await fetch(FIREBASE_URL, { cache: "no-store" });
    if (!response.ok) throw new Error(`Firebase returned ${response.status}`);
    renderDashboard(await response.json());
  } catch (error) {
    renderError(error);
  }
}

function setDataAtPath(target, path, value, isPatch = false) {
  if (path === "/") {
    if (isPatch && target && typeof target === "object") {
      return { ...target, ...value };
    }
    return value;
  }

  const next = target && typeof target === "object" ? { ...target } : {};
  const parts = path.split("/").filter(Boolean);
  let cursor = next;

  parts.forEach((part, index) => {
    if (index === parts.length - 1) {
      cursor[part] =
        isPatch && cursor[part] && typeof cursor[part] === "object"
          ? { ...cursor[part], ...value }
          : value;
      return;
    }

    cursor[part] = cursor[part] && typeof cursor[part] === "object" ? { ...cursor[part] } : {};
    cursor = cursor[part];
  });

  return next;
}

function handleStreamMessage(event, isPatch = false) {
  try {
    const message = JSON.parse(event.data);
    renderDashboard(setDataAtPath(latestData, message.path, message.data, isPatch));
  } catch (error) {
    renderError(error);
  }
}

function checkForStaleData() {
  if (!lastDataChangedAt) return;
  if (Date.now() - lastDataChangedAt > STALE_AFTER_MS) {
    setConnectionStatus("Error", "error");
  }
}

function startRealtimeUpdates() {
  if (realtimeStarted) return;
  realtimeStarted = true;

  if (!window.EventSource) {
    loadData();
    setInterval(loadData, 30000);
    return;
  }

  const stream = new EventSource(FIREBASE_URL);
  stream.addEventListener("put", (event) => handleStreamMessage(event));
  stream.addEventListener("patch", (event) => handleStreamMessage(event, true));
  stream.addEventListener("error", () => setConnectionStatus("Reconnecting", "warning"));

  setInterval(checkForStaleData, 5000);
  setInterval(loadData, 300000);
}

function showDashboard() {
  elements.loginScreen.hidden = true;
  elements.appShell.hidden = false;
  loadData();
  startRealtimeUpdates();
}

function showLogin() {
  elements.loginScreen.hidden = false;
  elements.appShell.hidden = true;
  elements.passwordInput.value = "";
  elements.usernameInput.focus();
}

elements.loginForm.addEventListener("submit", (event) => {
  event.preventDefault();
  const username = elements.usernameInput.value.trim();
  const password = elements.passwordInput.value;

  if (username === LOGIN_USERNAME && password === LOGIN_PASSWORD) {
    sessionStorage.setItem(AUTH_STORAGE_KEY, "true");
    elements.loginError.textContent = "";
    showDashboard();
    return;
  }

  elements.loginError.textContent = "Invalid username or password.";
});

elements.logoutButton.addEventListener("click", () => {
  sessionStorage.removeItem(AUTH_STORAGE_KEY);
  showLogin();
});

if (sessionStorage.getItem(AUTH_STORAGE_KEY) === "true") {
  showDashboard();
} else {
  showLogin();
}

// Remote Control Logic
const CONTROL_URL = FIREBASE_URL.replace("greenhouse.json", "greenhouse_control.json");

async function sendRemoteCommand(mode, state) {
  const statusText = document.getElementById("remoteStatusText");
  if (!statusText) return;
  
  statusText.textContent = "Sending command to Firebase...";
  try {
    const response = await fetch(CONTROL_URL, {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mode: mode, state: state, timestamp: Date.now() })
    });
    if (!response.ok) throw new Error(`Firebase returned ${response.status}`);
    statusText.textContent = `Command sent successfully! (Mode: ${mode})`;
    setTimeout(() => { statusText.textContent = ""; }, 3000);
  } catch (error) {
    statusText.textContent = "Error sending command: " + error.message;
  }
}

document.getElementById("remoteAutoBtn")?.addEventListener("click", () => sendRemoteCommand("auto", null));
document.getElementById("remoteOnBtn")?.addEventListener("click", () => sendRemoteCommand("manual", true));
document.getElementById("remoteOffBtn")?.addEventListener("click", () => sendRemoteCommand("manual", false));

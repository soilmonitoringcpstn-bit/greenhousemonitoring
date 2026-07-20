const FIREBASE_URL = window.GREENHOUSE_CONFIG?.firebaseUrl;

if (!FIREBASE_URL) {
  throw new Error("Missing GREENHOUSE_CONFIG.firebaseUrl in app-config.js");
}

const STALE_AFTER_MS = 3 * 60 * 1000;
const HIDE_READINGS_AFTER_MS = 15 * 60 * 1000;
const SIGNATURE_STORAGE_KEY = "greenhouseLatestSignature";
const CHANGED_AT_STORAGE_KEY = "greenhouseLastDataChangedAt";
const AUTH_STORAGE_KEY = "greenhouseDashboardLoggedIn";

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
  errorPanel: document.querySelector("#errorPanel"),
  errorMessage: document.querySelector("#errorMessage"),
  deviceLastUpdate: document.querySelector("#deviceLastUpdate"),
  lastSync: document.querySelector("#lastSync"),
  soilWaterLevel: document.querySelector("#soilWaterLevel"),
  tempMercury: document.querySelector("#tempMercury"),
  humidityDrops: document.querySelector("#humidityDrops"),
  pumpAnimIcon: document.querySelector("#pumpAnimIcon"),
  weatherIcon: document.querySelector("#weatherIcon"),
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
    pumpCooldown: getFirstValue(data, ["actuator.pump.cooldown_active"]),
    systemStatus: getFirstValue(data, ["system.status", "system_status"]),
    wifiStatus: getFirstValue(data, ["system.wifi_status", "wifi_status"]),
    wifiRssi: getFirstValue(data, ["system.wifi_rssi", "wifi_rssi"]),
    lastUpdate: getFirstValue(data, ["system.last_update_ms", "last_update_ms"]),
    lastUpdateServer: getFirstValue(data, ["system.last_update_server", "last_update_server"]),
    lastUpdateUnix: getFirstValue(data, ["system.last_update_unix", "last_update_unix"]),
  };
}

function getDeviceLastUpdateMs(data) {
  const readings = getReadings(data || {});
  const serverTimestamp = Number(readings.lastUpdateServer);
  if (Number.isFinite(serverTimestamp) && serverTimestamp > 0) {
    return serverTimestamp > 100000000000 ? serverTimestamp : serverTimestamp * 1000;
  }

  const deviceTimestamp = Number(readings.lastUpdateUnix);
  return Number.isFinite(deviceTimestamp) && deviceTimestamp > 0
    ? deviceTimestamp * 1000
    : NaN;
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

function hideStaleReadings() {
  elements.soilPercent.textContent = "--";
  elements.soilRaw.textContent = "--";
  elements.soilStatus.textContent = "Stale data";
  elements.temperatureValue.textContent = "--";
  elements.temperatureError.textContent = "Stale data";
  elements.humidityValue.textContent = "--";
  elements.humidityError.textContent = "Stale data";
  elements.pumpValue.textContent = "--";
  elements.rainValue.textContent = "--";
  elements.rainRaw.textContent = "--";
  elements.soilWaterLevel?.style.setProperty("--soil-level", "0%");
  elements.tempMercury?.style.setProperty("--temp-level", "0%");
  elements.humidityDrops?.style.setProperty("--humidity-opacity", "0");
  elements.pumpAnimIcon?.classList.remove("spinning");
  const cooldown = document.getElementById("pumpCooldownValue");
  if (cooldown) cooldown.textContent = "--";
  document.body.classList.add("system-offline");
  setConnectionStatus("Offline", "error");
}

function renderDashboard(data) {
  if (!data || typeof data !== "object") return;
  latestData = data;
  updateOfflineStatus(data);

  const signature = getDataSignature(data);
  if (signature !== latestSignature) {
    latestSignature = signature;
    lastDataChangedAt = Date.now();
    localStorage.setItem(SIGNATURE_STORAGE_KEY, latestSignature);
    localStorage.setItem(CHANGED_AT_STORAGE_KEY, String(lastDataChangedAt));
  }

  const readings = getReadings(data);
  const dhtHasError = readings.dhtError === true || String(readings.dhtError).toLowerCase() === "true";

  const soilP = readings.soilPercent !== undefined ? readings.soilPercent : 0;
  elements.soilPercent.textContent = `${formatNumber(soilP, 0)}%`;
  elements.soilRaw.textContent = readings.soilRaw ?? "--";
  elements.soilStatus.textContent = readings.soilStatus || "--";
  if (elements.soilWaterLevel) {
    elements.soilWaterLevel.style.setProperty('--soil-level', `${soilP}%`);
  }

  const tempP = readings.temperature !== undefined ? readings.temperature : 0;
  elements.temperatureValue.textContent = dhtHasError ? "Error" : `${formatNumber(tempP)}°C`;
  elements.temperatureError.textContent = dhtHasError ? "Sensor error" : "Normal";
  if (elements.tempMercury) {
    const minTemp = 0;
    const maxTemp = 50;
    let tempPercent = ((tempP - minTemp) / (maxTemp - minTemp)) * 100;
    tempPercent = Math.max(0, Math.min(100, tempPercent));
    elements.tempMercury.style.setProperty('--temp-level', `${tempPercent}%`);
    
    // Color scaling: Blue (<20) to Green (20-30) to Red (>30)
    let color = '#ef4444'; // default red
    if (tempP < 20) color = '#3b82f6'; // blue
    else if (tempP <= 30) color = '#10b981'; // green
    elements.tempMercury.style.setProperty('--temp-color', color);
  }

  const humP = readings.humidity !== undefined ? readings.humidity : 0;
  elements.humidityValue.textContent = dhtHasError ? "Error" : `${formatNumber(humP)}%`;
  elements.humidityError.textContent = dhtHasError ? "Sensor error" : "Normal";
  if (elements.humidityDrops) {
    elements.humidityDrops.style.setProperty('--humidity-opacity', (humP / 100).toFixed(2));
  }

  const isPumpOn = readings.pumpStatus === true || String(readings.pumpStatus).toLowerCase() === "true" || String(readings.pumpStatus).toLowerCase() === "on";
  elements.pumpValue.textContent = isPumpOn ? "ON" : "OFF";
  if (elements.pumpAnimIcon) {
    if (isPumpOn) {
      elements.pumpAnimIcon.classList.add("spinning");
    } else {
      elements.pumpAnimIcon.classList.remove("spinning");
    }
  }
  
  const pumpCooldownEl = document.getElementById("pumpCooldownValue");
  if (pumpCooldownEl) {
    const isCoolingDown = readings.pumpCooldown === true || String(readings.pumpCooldown).toLowerCase() === "true";
    pumpCooldownEl.textContent = isCoolingDown ? "Active (5hr)" : "Inactive";
    pumpCooldownEl.style.color = isCoolingDown ? "#f59e0b" : "inherit";
  }

  const isRaining = readings.rainStatus === true || String(readings.rainStatus).toLowerCase() === "true" || String(readings.rainStatus).toLowerCase() === "raining" || String(readings.rainStatus).toLowerCase() === "rain detected";
  elements.rainValue.textContent = isRaining ? "Raining" : "No Rain";
  elements.rainRaw.textContent = readings.rainRaw ?? "--";
  if (elements.weatherIcon) {
    if (isRaining) {
      elements.weatherIcon.textContent = "🌧️";
      elements.weatherIcon.classList.add("raining");
    } else {
      elements.weatherIcon.textContent = "☀️";
      elements.weatherIcon.classList.remove("raining");
    }
  }

  const deviceTimeMs = getDeviceLastUpdateMs(data);
  const deviceTimeIsValid = Number.isFinite(deviceTimeMs) && deviceTimeMs > 0 &&
    deviceTimeMs <= Date.now() + 5 * 60 * 1000 &&
    deviceTimeMs >= Date.UTC(2024, 0, 1);
  elements.deviceLastUpdate.textContent = deviceTimeIsValid
    ? new Date(deviceTimeMs).toLocaleString()
    : "Invalid device clock";
  elements.lastSync.textContent = new Date().toLocaleString();

  elements.errorPanel.hidden = true;
  setConnectionStatus(getFreshnessType() === "error" ? "Offline" : "Connected", getFreshnessType());

  if (!deviceTimeIsValid || Date.now() - deviceTimeMs > HIDE_READINGS_AFTER_MS) {
    hideStaleReadings();
  }
}

function renderError(error) {
  setConnectionStatus("Error", "error");
  elements.errorMessage.textContent = error.message || "Unable to read Firebase data.";
  elements.errorPanel.hidden = false;
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
  const configuredLogin = window.GREENHOUSE_CONFIG?.dashboardLogin;
  const username = elements.usernameInput.value.trim();
  const password = elements.passwordInput.value;

  if (configuredLogin && username === configuredLogin.username && password === configuredLogin.password) {
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

// ===== OFFLINE DETECTION =====
let latestUnixTimestamp = 0;

function updateOfflineStatus(data) {
  if (data && typeof data === "object") {
    const timestampMs = getDeviceLastUpdateMs(data);
    const currentTimeMs = Date.now();
    latestUnixTimestamp = Number.isFinite(timestampMs) &&
      timestampMs >= Date.UTC(2024, 0, 1) && timestampMs <= currentTimeMs + 5 * 60 * 1000
      ? Math.floor(timestampMs / 1000)
      : -1;
  }
}

setInterval(() => {
  if (latestUnixTimestamp === 0) return; // Wait until first payload arrives

  // Date.now() is in ms. Firebase timestamp from ESP32 time(nullptr) is in seconds.
  const currentUnix = Math.floor(Date.now() / 1000);
  const diffSeconds = currentUnix - latestUnixTimestamp;

  if (latestUnixTimestamp < 0 || diffSeconds > HIDE_READINGS_AFTER_MS / 1000) {
    hideStaleReadings();
  }

  if (diffSeconds > STALE_AFTER_MS / 1000) {
    document.body.classList.add('system-offline');
  } else {
    document.body.classList.remove('system-offline');
  }
}, 5000); // Check every 5 seconds


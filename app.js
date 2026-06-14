const FIREBASE_URL =
  "https://soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app/greenhouse.json";

const STALE_AFTER_MS = 3 * 60 * 1000;
const HISTORY_WINDOW_MS = 24 * 60 * 60 * 1000;
const HISTORY_KEY = "greenhouseMonitoringHistory";
const SIGNATURE_STORAGE_KEY = "greenhouseLatestSignature";
const CHANGED_AT_STORAGE_KEY = "greenhouseLastDataChangedAt";
const ERROR_COUNT_KEY = "greenhouseErrorCountDate";

let latestData = null;
let latestSignature = localStorage.getItem(SIGNATURE_STORAGE_KEY) || "";
let lastDataChangedAt = Number(localStorage.getItem(CHANGED_AT_STORAGE_KEY)) || 0;
let history = loadHistory();

const elements = {
  connectionText: document.querySelector("#connectionText"),
  wifiStatus: document.querySelector("#wifiStatus"),
  firebaseStatus: document.querySelector("#firebaseStatus"),
  espStatus: document.querySelector("#espStatus"),
  lastReceived: document.querySelector("#lastReceived"),
  soilGauge: document.querySelector("#soilGauge"),
  soilMoistureValue: document.querySelector("#soilMoistureValue"),
  soilCondition: document.querySelector("#soilCondition"),
  temperatureValue: document.querySelector("#temperatureValue"),
  tempHigh: document.querySelector("#tempHigh"),
  tempLow: document.querySelector("#tempLow"),
  humidityValue: document.querySelector("#humidityValue"),
  humidityTrend: document.querySelector("#humidityTrend"),
  rainValue: document.querySelector("#rainValue"),
  lastRainEvent: document.querySelector("#lastRainEvent"),
  pumpValue: document.querySelector("#pumpValue"),
  pumpLastActive: document.querySelector("#pumpLastActive"),
  pumpActivations: document.querySelector("#pumpActivations"),
  healthGauge: document.querySelector("#healthGauge"),
  healthScore: document.querySelector("#healthScore"),
  healthLabel: document.querySelector("#healthLabel"),
  healthDescription: document.querySelector("#healthDescription"),
  systemHealthList: document.querySelector("#systemHealthList"),
  soilChart: document.querySelector("#soilChart"),
  climateChart: document.querySelector("#climateChart"),
  rainTimeline: document.querySelector("#rainTimeline"),
  pumpTimeline: document.querySelector("#pumpTimeline"),
  activityFeed: document.querySelector("#activityFeed"),
  latestError: document.querySelector("#latestError"),
  errorCode: document.querySelector("#errorCode"),
  errorSeverity: document.querySelector("#errorSeverity"),
  errorTimestamp: document.querySelector("#errorTimestamp"),
  errorCount: document.querySelector("#errorCount"),
  firmwareVersion: document.querySelector("#firmwareVersion"),
  databaseStatus: document.querySelector("#databaseStatus"),
  lastUpdated: document.querySelector("#lastUpdated"),
  systemUptime: document.querySelector("#systemUptime"),
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

function toNumber(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function formatNumber(value, digits = 1) {
  const number = toNumber(value);
  if (number === null) return "--";
  return Number.isInteger(number) ? String(number) : number.toFixed(digits);
}

function formatTime(timestamp) {
  if (!timestamp) return "--";
  return new Intl.DateTimeFormat(undefined, {
    hour: "numeric",
    minute: "2-digit",
    second: "2-digit",
  }).format(new Date(timestamp));
}

function formatDateTime(timestamp) {
  if (!timestamp) return "--";
  return new Date(timestamp).toLocaleString();
}

function formatDuration(ms) {
  const value = toNumber(ms);
  if (value === null) return "--";
  const seconds = Math.floor(value / 1000);
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (hours > 0) return `${hours}h ${minutes}m`;
  return `${minutes}m ${seconds % 60}s`;
}

function isRainDetected(value) {
  if (typeof value === "boolean") return value;
  return String(value || "").toLowerCase().includes("rain") &&
    !String(value || "").toLowerCase().includes("no rain");
}

function isPumpOn(value) {
  if (typeof value === "boolean") return value;
  return String(value || "").toLowerCase() === "on";
}

function getReadings(data) {
  return {
    soil: toNumber(getFirstValue(data, ["sensors.soil.moisture_percent", "soil_moisture_percent"])),
    temperature: toNumber(getFirstValue(data, ["sensors.dht22.temperature_celsius", "temperature"])),
    humidity: toNumber(getFirstValue(data, ["sensors.dht22.humidity_percent", "humidity"])),
    rain: getFirstValue(data, ["sensors.rain.status", "sensors.rain.detected", "rain_status", "rain_detected"]),
    pump: getFirstValue(data, ["actuator.pump.is_on", "actuator.pump.status", "pump_on"]),
    wifi: getFirstValue(data, ["system.wifi_status", "wifi_status"]),
    rssi: getFirstValue(data, ["system.wifi_rssi", "wifi_rssi"]),
    espStatus: getFirstValue(data, ["system.status", "system_status"]),
    uptime: getFirstValue(data, ["system.last_update_ms", "last_update_ms"]),
    firmware: getFirstValue(data, ["system.firmware_version", "firmware_version"]),
    battery: getFirstValue(data, ["solar.battery_voltage", "system.battery_voltage", "battery_voltage"]),
    charging: getFirstValue(data, ["solar.charging_status", "solar.is_charging", "charging_status"]),
    powerSource: getFirstValue(data, ["solar.power_source", "power_source"]),
  };
}

function loadHistory() {
  try {
    const parsed = JSON.parse(localStorage.getItem(HISTORY_KEY) || "[]");
    return Array.isArray(parsed) ? parsed : [];
  } catch {
    return [];
  }
}

function saveHistory() {
  localStorage.setItem(HISTORY_KEY, JSON.stringify(history.slice(-600)));
}

function addHistorySample(readings) {
  const now = Date.now();
  const sample = {
    timestamp: now,
    soil: readings.soil,
    temperature: readings.temperature,
    humidity: readings.humidity,
    rain: isRainDetected(readings.rain),
    pump: isPumpOn(readings.pump),
  };
  const last = history[history.length - 1];
  if (!last || JSON.stringify({ ...last, timestamp: 0 }) !== JSON.stringify({ ...sample, timestamp: 0 })) {
    history.push(sample);
  }
  history = history.filter((item) => now - item.timestamp <= HISTORY_WINDOW_MS);
  saveHistory();
}

function setBadge(element, text, status = "neutral") {
  element.textContent = text;
  element.className = `status-badge ${status}`;
}

function getFreshnessStatus() {
  if (!lastDataChangedAt) return "neutral";
  return Date.now() - lastDataChangedAt > STALE_AFTER_MS ? "danger" : "success";
}

function getSoilCondition(soil) {
  if (soil === null) return "Waiting";
  if (soil < 35) return "Dry";
  if (soil < 65) return "Moderate";
  return "Optimal";
}

function calculateHealthScore(readings) {
  const scores = [];
  if (readings.soil !== null) scores.push(Math.max(0, 100 - Math.abs(readings.soil - 70) * 1.4));
  if (readings.temperature !== null) scores.push(Math.max(0, 100 - Math.abs(readings.temperature - 28) * 5));
  if (readings.humidity !== null) scores.push(Math.max(0, 100 - Math.abs(readings.humidity - 65) * 2));
  scores.push(isRainDetected(readings.rain) ? 78 : 92);
  return Math.round(scores.reduce((sum, value) => sum + value, 0) / scores.length);
}

function renderMetrics(readings) {
  const soil = readings.soil;
  elements.soilMoistureValue.textContent = soil === null ? "--" : `${formatNumber(soil, 0)}%`;
  elements.soilGauge.style.setProperty("--value", soil || 0);
  elements.soilCondition.textContent = getSoilCondition(soil);

  elements.temperatureValue.textContent =
    readings.temperature === null ? "--" : `${formatNumber(readings.temperature)}°C`;
  elements.humidityValue.textContent =
    readings.humidity === null ? "--" : `${formatNumber(readings.humidity)}%`;
  elements.rainValue.textContent = isRainDetected(readings.rain) ? "Rain Detected" : "No Rain";
  elements.pumpValue.textContent = isPumpOn(readings.pump) ? "On" : "Off";

  const today = history.filter((item) => new Date(item.timestamp).toDateString() === new Date().toDateString());
  const temps = today.map((item) => item.temperature).filter((value) => value !== null);
  elements.tempHigh.textContent = temps.length ? `${formatNumber(Math.max(...temps))}°C` : "--";
  elements.tempLow.textContent = temps.length ? `${formatNumber(Math.min(...temps))}°C` : "--";

  const previousHumidity = history.length > 1 ? history[history.length - 2].humidity : null;
  if (previousHumidity === null || readings.humidity === null) {
    elements.humidityTrend.textContent = "Trend unavailable";
  } else {
    const delta = readings.humidity - previousHumidity;
    elements.humidityTrend.innerHTML = `${delta >= 0 ? "↗" : "↘"} ${Math.abs(delta).toFixed(1)}% from last reading`;
  }

  const rainEvent = [...history].reverse().find((item) => item.rain);
  elements.lastRainEvent.textContent = rainEvent ? formatTime(rainEvent.timestamp) : "No recent rain";

  const activations = countPumpActivations(today);
  const lastActivation = findLastPumpActivation();
  elements.pumpActivations.textContent = String(activations);
  elements.pumpLastActive.textContent = lastActivation ? formatTime(lastActivation) : "--";
}

function countPumpActivations(items) {
  return items.reduce((count, item, index) => {
    const previous = index > 0 ? items[index - 1] : null;
    return count + (item.pump && !previous?.pump ? 1 : 0);
  }, 0);
}

function findLastPumpActivation() {
  for (let index = history.length - 1; index >= 0; index -= 1) {
    if (history[index].pump) return history[index].timestamp;
  }
  return null;
}

function renderHealth(readings) {
  const score = calculateHealthScore(readings);
  const label = score >= 85 ? "Excellent Growing Conditions" : score >= 70 ? "Stable Growing Conditions" : "Needs Attention";
  elements.healthScore.textContent = `${score}%`;
  elements.healthGauge.style.setProperty("--score", score);
  elements.healthLabel.textContent = label;
  elements.healthDescription.textContent =
    "Score is derived from soil moisture, temperature, humidity, and rain conditions.";
}

function renderStatuses(readings) {
  const freshStatus = getFreshnessStatus();
  const isFresh = freshStatus === "success";
  setBadge(elements.connectionText, isFresh ? "Live" : "Offline", freshStatus);
  setBadge(elements.firebaseStatus, "Firebase Connected", "success");
  setBadge(
    elements.wifiStatus,
    readings.wifi ? `WiFi ${readings.wifi}` : "WiFi Unknown",
    readings.wifi && String(readings.wifi).toLowerCase().includes("connected") ? "success" : "warning"
  );
  setBadge(
    elements.espStatus,
    readings.espStatus ? `ESP32 ${readings.espStatus}` : "ESP32 Unknown",
    isFresh ? "success" : "danger"
  );
  elements.lastReceived.textContent = `Last data: ${formatDateTime(lastDataChangedAt)}`;
  elements.databaseStatus.textContent = "Connected";
  elements.lastUpdated.textContent = formatTime(Date.now());
  elements.systemUptime.textContent = formatDuration(readings.uptime);
  elements.firmwareVersion.textContent = readings.firmware || "N/A";
}

function statusItem(label, value, status) {
  return `
    <div class="health-item">
      <span>${label}</span>
      <strong class="dot-status ${status}">${value}</strong>
    </div>
  `;
}

function renderSystemHealth(readings) {
  const fresh = getFreshnessStatus() === "success";
  elements.systemHealthList.innerHTML = [
    statusItem("Soil Moisture Sensor", readings.soil !== null ? "Online" : "Warning", readings.soil !== null ? "online" : "warning"),
    statusItem("DHT22 Sensor", readings.temperature !== null && readings.humidity !== null ? "Online" : "Warning", readings.temperature !== null && readings.humidity !== null ? "online" : "warning"),
    statusItem("Rain Sensor", readings.rain !== undefined ? "Online" : "Warning", readings.rain !== undefined ? "online" : "warning"),
    statusItem("WiFi", readings.wifi || "Unknown", String(readings.wifi || "").toLowerCase().includes("connected") ? "online" : "warning"),
    statusItem("Firebase", "Connected", "online"),
    statusItem("Internet Connection", fresh ? "Online" : "Offline", fresh ? "online" : "offline"),
    statusItem("Battery Voltage", readings.battery ? `${readings.battery}V` : "N/A", readings.battery ? "online" : "warning"),
    statusItem("Solar Charging", readings.charging ?? "N/A", readings.charging ? "online" : "warning"),
    statusItem("Power Source", readings.powerSource || "Solar / N/A", "online"),
  ].join("");
}

function renderLineChart(container, series, options = {}) {
  const width = 640;
  const height = 220;
  const pad = 26;
  const validSeries = series.map((item) => ({
    ...item,
    points: history
      .map((sample) => ({ x: sample.timestamp, y: sample[item.key] }))
      .filter((point) => point.y !== null && point.y !== undefined),
  }));
  const allPoints = validSeries.flatMap((item) => item.points);
  if (allPoints.length < 2) {
    container.innerHTML = '<div class="empty-chart">Collecting chart data...</div>';
    return;
  }
  const minX = Math.min(...allPoints.map((point) => point.x));
  const maxX = Math.max(...allPoints.map((point) => point.x));
  const minY = options.min ?? Math.min(...allPoints.map((point) => point.y));
  const maxY = options.max ?? Math.max(...allPoints.map((point) => point.y));
  const rangeX = maxX - minX || 1;
  const rangeY = maxY - minY || 1;

  const lines = validSeries
    .map((item) => {
      const points = item.points
        .map((point) => {
          const x = pad + ((point.x - minX) / rangeX) * (width - pad * 2);
          const y = height - pad - ((point.y - minY) / rangeY) * (height - pad * 2);
          return `${x.toFixed(1)},${y.toFixed(1)}`;
        })
        .join(" ");
      return `<polyline fill="none" stroke="${item.color}" stroke-width="4" stroke-linecap="round" stroke-linejoin="round" points="${points}" />`;
    })
    .join("");

  const legends = validSeries
    .map(
      (item, index) =>
        `<text x="${pad + index * 150}" y="20" fill="${item.color}" font-size="13" font-weight="700">${item.label}</text>`
    )
    .join("");

  container.innerHTML = `
    <svg viewBox="0 0 ${width} ${height}" role="img" aria-label="Historical trend chart">
      <line x1="${pad}" y1="${height - pad}" x2="${width - pad}" y2="${height - pad}" stroke="#cbd5e1" />
      <line x1="${pad}" y1="${pad}" x2="${pad}" y2="${height - pad}" stroke="#cbd5e1" />
      ${legends}
      ${lines}
    </svg>
  `;
}

function renderEventTimeline(container, key, activeColor) {
  const width = 640;
  const height = 220;
  const events = history.filter((item) => item[key]);
  if (!history.length) {
    container.innerHTML = '<div class="empty-chart">Collecting event history...</div>';
    return;
  }
  const minX = Math.min(...history.map((item) => item.timestamp));
  const maxX = Math.max(...history.map((item) => item.timestamp));
  const rangeX = maxX - minX || 1;
  const markers = events
    .map((item) => {
      const x = 28 + ((item.timestamp - minX) / rangeX) * (width - 56);
      return `<circle cx="${x.toFixed(1)}" cy="110" r="8" fill="${activeColor}" />`;
    })
    .join("");
  container.innerHTML = `
    <svg viewBox="0 0 ${width} ${height}" role="img" aria-label="Event timeline">
      <line x1="28" y1="110" x2="${width - 28}" y2="110" stroke="#cbd5e1" stroke-width="4" stroke-linecap="round" />
      ${markers || `<text x="50%" y="114" text-anchor="middle" fill="#64748b" font-size="16" font-weight="700">No events recorded</text>`}
    </svg>
  `;
}

function renderCharts() {
  renderLineChart(elements.soilChart, [{ key: "soil", label: "Soil Moisture", color: "#10B981" }], {
    min: 0,
    max: 100,
  });
  renderLineChart(
    elements.climateChart,
    [
      { key: "temperature", label: "Temperature", color: "#0EA5E9" },
      { key: "humidity", label: "Humidity", color: "#14B8A6" },
    ],
    { min: 0, max: 100 }
  );
  renderEventTimeline(elements.rainTimeline, "rain", "#0EA5E9");
  renderEventTimeline(elements.pumpTimeline, "pump", "#10B981");
}

function renderActivity(readings) {
  const time = formatTime(Date.now());
  const items = [
    ["🟢", "Soil Moisture Updated", time],
    ["🟢", "Temperature Updated", time],
    ["🔵", "Data Uploaded to Firebase", time],
    [getFreshnessStatus() === "success" ? "🟢" : "🔴", `ESP32 ${readings.espStatus || "status unknown"}`, time],
  ];
  elements.activityFeed.innerHTML = items
    .map(
      ([icon, label, itemTime]) => `
        <div class="activity-item">
          <span class="activity-icon">${icon}</span>
          <div>
            <strong>${label}</strong>
            <time>${itemTime}</time>
          </div>
        </div>
      `
    )
    .join("");
}

function todayKey() {
  return new Date().toISOString().slice(0, 10);
}

function getErrorCount() {
  const stored = JSON.parse(localStorage.getItem(ERROR_COUNT_KEY) || "{}");
  return stored.date === todayKey() ? stored.count : 0;
}

function incrementErrorCount() {
  const count = getErrorCount() + 1;
  localStorage.setItem(ERROR_COUNT_KEY, JSON.stringify({ date: todayKey(), count }));
  return count;
}

function renderNoError() {
  elements.latestError.textContent = "None detected";
  elements.errorCode.textContent = "--";
  elements.errorSeverity.textContent = "Normal";
  elements.errorTimestamp.textContent = "--";
  elements.errorCount.textContent = String(getErrorCount());
}

function renderError(error) {
  const count = incrementErrorCount();
  const message = error.message || "Unknown error";
  elements.latestError.textContent = message.includes("401") ? "Firebase Authentication Failed" : message;
  elements.errorCode.textContent = message.includes("401") ? "401 Unauthorized" : "Client Error";
  elements.errorSeverity.textContent = message.includes("401") ? "High" : "Warning";
  elements.errorTimestamp.textContent = formatTime(Date.now());
  elements.errorCount.textContent = String(count);
  elements.databaseStatus.textContent = "Error";
}

function getDataSignature(data) {
  return JSON.stringify(data || {});
}

function renderDashboard(data) {
  if (!data || typeof data !== "object") return;
  latestData = data;
  const readings = getReadings(data);
  const signature = getDataSignature(data);
  if (signature !== latestSignature) {
    latestSignature = signature;
    lastDataChangedAt = Date.now();
    localStorage.setItem(SIGNATURE_STORAGE_KEY, latestSignature);
    localStorage.setItem(CHANGED_AT_STORAGE_KEY, String(lastDataChangedAt));
    addHistorySample(readings);
  }
  renderMetrics(readings);
  renderHealth(readings);
  renderStatuses(readings);
  renderSystemHealth(readings);
  renderCharts();
  renderActivity(readings);
  renderNoError();
}

async function loadData() {
  try {
    setBadge(elements.connectionText, "Connecting", "neutral");
    const response = await fetch(FIREBASE_URL, { cache: "no-store" });
    if (!response.ok) throw new Error(`Firebase returned ${response.status}`);
    const data = await response.json();
    renderDashboard(data);
  } catch (error) {
    setBadge(elements.connectionText, "Offline", "danger");
    setBadge(elements.firebaseStatus, "Firebase Error", "danger");
    setBadge(elements.espStatus, "ESP32 Unknown", "neutral");
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
    setBadge(elements.connectionText, "Offline", "danger");
    setBadge(elements.espStatus, "ESP32 Offline", "danger");
  }
}

function startRealtimeUpdates() {
  if (!window.EventSource) {
    loadData();
    setInterval(loadData, 30000);
    return;
  }
  const stream = new EventSource(FIREBASE_URL);
  stream.addEventListener("put", (event) => handleStreamMessage(event));
  stream.addEventListener("patch", (event) => handleStreamMessage(event, true));
  stream.addEventListener("error", () => {
    setBadge(elements.connectionText, "Reconnecting", "warning");
  });
  setInterval(checkForStaleData, 5000);
  setInterval(loadData, 300000);
}

renderCharts();
loadData();
startRealtimeUpdates();

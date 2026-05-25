const FIREBASE_URL =
  "https://soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app/.json";

let latestData = null;
let latestSignature = "";
let lastDataChangedAt = 0;
const STALE_AFTER_MS = 3 * 60 * 1000;

const fields = {
  soilMoistureValue: {
    paths: ["sensors.soil.moisture_percent", "soil_moisture_percent"],
    unit: "%",
  },
  temperatureValue: {
    paths: ["sensors.dht22.temperature_celsius", "temperature"],
    unit: " deg C",
  },
  humidityValue: {
    paths: ["sensors.dht22.humidity_percent", "humidity"],
    unit: "%",
  },
  pumpValue: {
    paths: ["actuator.pump.is_on", "pump_on"],
    unit: "",
  },
  rainValue: {
    paths: ["sensors.rain.status", "sensors.rain.detected", "rain_status", "rain_detected"],
    unit: "",
  },
};

const connectionText = document.querySelector("#connectionText");
const latestDetails = document.querySelector("#latestDetails");
const lastUpdated = document.querySelector("#lastUpdated");
const hiddenDetailKeys = new Set([
  "actuator pump relay_pin",
  "sensors dht22 error",
  "sensors rain raw_value",
  "sensors soil raw_value",
  "system last_update_ms",
  "system status",
  "system wifi_rssi",
  "system wifi_status",
]);

function formatValue(value, unit = "") {
  if (value === null || value === undefined || value === "") return "--";
  if (typeof value === "boolean") return value ? "On" : "Off";
  const number = Number(value);
  if (Number.isFinite(number)) {
    return `${Number.isInteger(number) ? number : number.toFixed(1)}${unit}`;
  }
  return String(value);
}

function formatLabel(key) {
  return key
    .replace(/_/g, " ")
    .replace(/\b\w/g, (letter) => letter.toUpperCase());
}

function escapeHtml(value) {
  return String(value)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#039;");
}

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

function flattenDetails(source, prefix = "") {
  return Object.entries(source || {}).flatMap(([key, value]) => {
    const label = prefix ? `${prefix} ${key}` : key;
    if (value && typeof value === "object" && !Array.isArray(value)) {
      return flattenDetails(value, label);
    }
    return [[label, value]];
  });
}

function renderData(greenhouse) {
  if (!greenhouse || typeof greenhouse !== "object") return;

  Object.entries(fields).forEach(([elementId, config]) => {
    document.querySelector(`#${elementId}`).textContent = formatValue(
      getFirstValue(greenhouse, config.paths),
      config.unit
    );
  });

  latestDetails.innerHTML = flattenDetails(greenhouse)
    .filter(([key]) => !hiddenDetailKeys.has(key))
    .map(
      ([key, value]) => `
        <div>
          <dt>${escapeHtml(formatLabel(key))}</dt>
          <dd>${escapeHtml(formatValue(value))}</dd>
        </div>
      `
    )
    .join("");
}

function getGreenhouseData(data) {
  return data.greenhouse || data;
}

function updateLastUpdated() {
  lastUpdated.textContent = `Last updated: ${new Date().toLocaleString()}`;
}

function getDataSignature(greenhouse) {
  return JSON.stringify(greenhouse || {});
}

function setConnectionStatus(status) {
  connectionText.textContent = status;
}

function renderAndTrackData(data) {
  latestData = data;
  const greenhouse = getGreenhouseData(latestData);
  const signature = getDataSignature(greenhouse);

  if (signature !== latestSignature) {
    latestSignature = signature;
    lastDataChangedAt = Date.now();
    updateLastUpdated();
  }

  renderData(greenhouse);
  setConnectionStatus(Date.now() - lastDataChangedAt > STALE_AFTER_MS ? "Offline" : "Live");
}

function checkForStaleData() {
  if (!lastDataChangedAt) return;
  if (Date.now() - lastDataChangedAt > STALE_AFTER_MS) {
    setConnectionStatus("Offline");
  }
}

async function loadData() {
  try {
    setConnectionStatus("Connecting");
    const response = await fetch(FIREBASE_URL, { cache: "no-store" });
    if (!response.ok) throw new Error(`Firebase returned ${response.status}`);

    const data = await response.json();
    renderAndTrackData(data);
  } catch (error) {
    setConnectionStatus("Offline");
    latestDetails.innerHTML = `
      <div>
        <dt>Error</dt>
        <dd>${escapeHtml(error.message)}</dd>
      </div>
    `;
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
  const message = JSON.parse(event.data);
  renderAndTrackData(setDataAtPath(latestData, message.path, message.data, isPatch));
}

function startRealtimeUpdates() {
  if (!window.EventSource) {
    loadData();
    setInterval(loadData, 30000);
    return;
  }

  setConnectionStatus("Connecting");
  const stream = new EventSource(FIREBASE_URL);

  stream.addEventListener("put", (event) => handleStreamMessage(event));
  stream.addEventListener("patch", (event) => handleStreamMessage(event, true));
  stream.addEventListener("open", () => {
    checkForStaleData();
  });
  stream.addEventListener("error", () => {
    setConnectionStatus("Reconnecting");
  });

  setInterval(checkForStaleData, 5000);
  setInterval(loadData, 300000);
}

loadData();
startRealtimeUpdates();

const FIREBASE_URL =
  "https://soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app/.json";

const fields = {
  soilMoistureValue: { key: "soil_moisture_percent", unit: "%" },
  temperatureValue: { key: "temperature", unit: " deg C" },
  humidityValue: { key: "humidity", unit: "%" },
  pumpValue: { key: "pump_on", unit: "" },
};

const connectionText = document.querySelector("#connectionText");
const latestDetails = document.querySelector("#latestDetails");
const lastUpdated = document.querySelector("#lastUpdated");

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

function renderData(greenhouse) {
  Object.entries(fields).forEach(([elementId, config]) => {
    document.querySelector(`#${elementId}`).textContent = formatValue(
      greenhouse[config.key],
      config.unit
    );
  });

  latestDetails.innerHTML = Object.entries(greenhouse)
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

async function loadData() {
  try {
    connectionText.textContent = "Connecting";
    const response = await fetch(FIREBASE_URL, { cache: "no-store" });
    if (!response.ok) throw new Error(`Firebase returned ${response.status}`);

    const data = await response.json();
    const greenhouse = data.greenhouse || data;
    renderData(greenhouse);
    connectionText.textContent = "Live";
    lastUpdated.textContent = `Last updated: ${new Date().toLocaleString()}`;
  } catch (error) {
    connectionText.textContent = "Offline";
    latestDetails.innerHTML = `
      <div>
        <dt>Error</dt>
        <dd>${escapeHtml(error.message)}</dd>
      </div>
    `;
  }
}

loadData();
setInterval(loadData, 30000);

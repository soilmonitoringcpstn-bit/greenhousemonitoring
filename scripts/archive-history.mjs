import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

const HISTORY_SAMPLE_INTERVAL_MS = 15 * 60 * 1000;
const HISTORY_RETENTION_MS = 30 * 24 * 60 * 60 * 1000;
const MAX_TELEMETRY_AGE_MS = 15 * 60 * 1000;
const MAX_FUTURE_SKEW_MS = 5 * 60 * 1000;
const MIN_VALID_TIMESTAMP_MS = Date.UTC(2024, 0, 1);

function valueAtPath(source, path) {
  return path.split(".").reduce((value, key) => value?.[key], source);
}

function firstValue(source, paths) {
  for (const path of paths) {
    const value = valueAtPath(source, path);
    if (value !== undefined && value !== null) return value;
  }
  return null;
}

function numericOrNull(value) {
  if (value === null || value === undefined || value === "") return null;
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function booleanValue(value, positiveText = "") {
  if (value === true || value === 1) return true;
  const normalized = String(value ?? "").trim().toLowerCase();
  return normalized === "true" || normalized === "1" || normalized === "on" ||
    normalized === positiveText;
}

function telemetryTimestamp(data) {
  const serverValue = Number(firstValue(data, [
    "system.last_update_server",
    "last_update_server",
  ]));
  if (Number.isFinite(serverValue) && serverValue > 0) {
    return serverValue > 100000000000 ? serverValue : serverValue * 1000;
  }

  const deviceValue = Number(firstValue(data, [
    "system.last_update_unix",
    "last_update_unix",
  ]));
  return Number.isFinite(deviceValue) && deviceValue > 0
    ? deviceValue * 1000
    : NaN;
}

export function historyKey(timestamp) {
  const bucket = Math.floor(timestamp / HISTORY_SAMPLE_INTERVAL_MS);
  return `s${String(bucket).padStart(10, "0")}`;
}

export function createHistoryUpdate(data, now = Date.now()) {
  if (!data || typeof data !== "object") {
    throw new Error("Firebase did not return a greenhouse telemetry object.");
  }

  const timestamp = telemetryTimestamp(data);
  if (!Number.isFinite(timestamp) || timestamp < MIN_VALID_TIMESTAMP_MS) {
    throw new Error("Greenhouse telemetry has no valid timestamp.");
  }
  if (timestamp > now + MAX_FUTURE_SKEW_MS) {
    throw new Error("Greenhouse telemetry timestamp is unexpectedly in the future.");
  }
  if (now - timestamp > MAX_TELEMETRY_AGE_MS) {
    return {
      skipped: true,
      reason: "The ESP32 telemetry is stale, so no duplicate last-known snapshot was saved.",
    };
  }

  const dhtError = booleanValue(firstValue(data, [
    "sensors.dht22.error",
    "dht_error",
  ]));
  const soilPercent = numericOrNull(firstValue(data, [
    "sensors.soil.moisture_percent",
    "soil_moisture_percent",
  ]));
  const soilRaw = numericOrNull(firstValue(data, [
    "sensors.soil.raw_value",
    "soil_raw",
  ]));
  const temperature = numericOrNull(firstValue(data, [
    "sensors.dht22.temperature_celsius",
    "temperature",
  ]));
  const humidity = numericOrNull(firstValue(data, [
    "sensors.dht22.humidity_percent",
    "humidity",
  ]));
  const pumpStatus = firstValue(data, [
    "actuator.pump.status",
    "actuator.pump.is_on",
    "pump_on",
  ]);
  const rainStatus = firstValue(data, [
    "sensors.rain.detected",
    "sensors.rain.status",
    "rain_detected",
    "rain_status",
  ]);
  const configuredSoilStatus = firstValue(data, [
    "sensors.soil.status",
    "soil_status",
  ]);
  const soilStatus = configuredSoilStatus == null
    ? soilPercent === null ? "" : soilPercent < 15 ? "Dry" : soilPercent < 55 ? "Moist" : "Wet"
    : String(configuredSoilStatus);

  const key = historyKey(timestamp);
  const expiredKey = historyKey(timestamp - HISTORY_RETENTION_MS);
  const record = {
    timestamp: Math.round(timestamp),
    soilPercent,
    soilRaw,
    temperature: dhtError ? null : temperature,
    humidity: dhtError ? null : humidity,
    pumpOn: booleanValue(pumpStatus, "on"),
    rain: booleanValue(rainStatus, "rain detected") ||
      String(rainStatus ?? "").trim().toLowerCase() === "raining",
    soilStatus,
  };

  return {
    skipped: false,
    key,
    expiredKey,
    record,
    patch: { [key]: record, [expiredKey]: null },
  };
}

function authenticatedUrl(rawUrl, token) {
  const url = new URL(rawUrl);
  if (token) url.searchParams.set("auth", token);
  return url;
}

async function firebaseRequest(url, options = {}) {
  const response = await fetch(url, {
    ...options,
    signal: AbortSignal.timeout(20_000),
  });
  if (!response.ok) {
    const body = await response.text();
    throw new Error(`Firebase returned ${response.status}: ${body.slice(0, 300)}`);
  }
  return response;
}

export async function archiveLatestReading({
  telemetryUrl,
  historyUrl,
  authToken = "",
  now = Date.now(),
}) {
  if (!telemetryUrl || !historyUrl) {
    throw new Error("FIREBASE_TELEMETRY_URL and FIREBASE_HISTORY_URL are required.");
  }

  const telemetryResponse = await firebaseRequest(
    authenticatedUrl(telemetryUrl, authToken),
    { headers: { Accept: "application/json" } },
  );
  const update = createHistoryUpdate(await telemetryResponse.json(), now);
  if (update.skipped) return update;

  await firebaseRequest(authenticatedUrl(historyUrl, authToken), {
    method: "PATCH",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(update.patch),
  });
  return update;
}

async function main() {
  const result = await archiveLatestReading({
    telemetryUrl: process.env.FIREBASE_TELEMETRY_URL,
    historyUrl: process.env.FIREBASE_HISTORY_URL,
    authToken: process.env.FIREBASE_AUTH_TOKEN,
  });

  if (result.skipped) {
    console.log(result.reason);
  } else {
    console.log(`Archived fresh greenhouse telemetry as ${result.key}.`);
  }
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  main().catch((error) => {
    console.error(error.message);
    process.exitCode = 1;
  });
}

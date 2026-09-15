import assert from "node:assert/strict";
import test from "node:test";

import { createHistoryUpdate, historyKey } from "./archive-history.mjs";

const NOW = Date.UTC(2026, 8, 15, 3, 7, 0);
const UPDATED_AT = NOW - 10_000;

function telemetry(overrides = {}) {
  return {
    sensors: {
      dht22: { temperature_celsius: 29.25, humidity_percent: 73.5, error: false },
      soil: { raw_value: 1300, moisture_percent: 42, status: "Moist" },
      rain: { raw_value: 4095, detected: false, status: "No Rain" },
    },
    actuator: { pump: { status: "OFF", is_on: false } },
    system: { last_update_server: UPDATED_AT },
    ...overrides,
  };
}

test("creates the history shape consumed by the dashboard", () => {
  const result = createHistoryUpdate(telemetry(), NOW);

  assert.equal(result.skipped, false);
  assert.equal(result.key, historyKey(UPDATED_AT));
  assert.deepEqual(result.patch[result.key], {
    timestamp: UPDATED_AT,
    soilPercent: 42,
    soilRaw: 1300,
    temperature: 29.25,
    humidity: 73.5,
    pumpOn: false,
    rain: false,
    soilStatus: "Moist",
  });
  assert.equal(result.patch[result.expiredKey], null);
});

test("uses null temperature and humidity when the DHT reports an error", () => {
  const data = telemetry();
  data.sensors.dht22.error = true;
  const result = createHistoryUpdate(data, NOW);

  assert.equal(result.record.temperature, null);
  assert.equal(result.record.humidity, null);
});

test("does not manufacture new history from stale telemetry", () => {
  const data = telemetry();
  data.system.last_update_server = NOW - 16 * 60 * 1000;

  const result = createHistoryUpdate(data, NOW);
  assert.equal(result.skipped, true);
});

test("rejects an invalid telemetry timestamp", () => {
  const data = telemetry();
  data.system = {};

  assert.throws(
    () => createHistoryUpdate(data, NOW),
    /no valid timestamp/i,
  );
});

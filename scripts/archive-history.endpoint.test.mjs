import assert from "node:assert/strict";
import test from "node:test";

import { GET } from "../api/archive-history.mjs";

test("archive endpoint rejects requests when no secret is configured", async () => {
  const previousSecret = process.env.CRON_SECRET;
  delete process.env.CRON_SECRET;

  try {
    const response = await GET(new Request("https://example.test/api/archive-history"));
    assert.equal(response.status, 401);
    assert.equal(response.headers.get("cache-control"), "no-store");
    assert.equal((await response.json()).ok, false);
  } finally {
    if (previousSecret === undefined) delete process.env.CRON_SECRET;
    else process.env.CRON_SECRET = previousSecret;
  }
});

test("archive endpoint rejects a missing or wrong bearer token", async () => {
  const previousSecret = process.env.CRON_SECRET;
  process.env.CRON_SECRET = "test-only-secret";

  try {
    for (const authorization of [undefined, "Bearer wrong-secret"]) {
      const response = await GET(new Request("https://example.test/api/archive-history", {
        headers: authorization ? { authorization } : {},
      }));
      assert.equal(response.status, 401);
    }
  } finally {
    if (previousSecret === undefined) delete process.env.CRON_SECRET;
    else process.env.CRON_SECRET = previousSecret;
  }
});

test("authorized request archives fresh telemetry", async () => {
  const previousSecret = process.env.CRON_SECRET;
  const previousFetch = globalThis.fetch;
  process.env.CRON_SECRET = "test-only-secret";
  const requests = [];

  globalThis.fetch = async (input, options = {}) => {
    requests.push({ url: String(input), method: options.method || "GET", body: options.body });
    if (requests.length === 1) {
      return Response.json({
        sensors: {
          dht22: { temperature_celsius: 25.4, humidity_percent: 91.5, error: false },
          soil: { raw_value: 1250, moisture_percent: 73, status: "Wet" },
          rain: { detected: false },
        },
        actuator: { pump: { status: "OFF" } },
        system: { last_update_server: Date.now() - 1000 },
      });
    }
    return Response.json({});
  };

  try {
    const response = await GET(new Request("https://example.test/api/archive-history", {
      headers: { authorization: "Bearer test-only-secret" },
    }));
    const result = await response.json();
    assert.equal(response.status, 200);
    assert.equal(response.headers.get("cache-control"), "no-store");
    assert.equal(result.ok, true);
    assert.equal(result.skipped, false);
    assert.match(result.key, /^s\d{10}$/);
    assert.equal(requests.length, 2);
    assert.equal(requests[0].method, "GET");
    assert.equal(requests[1].method, "PATCH");
    assert.equal(JSON.parse(requests[1].body)[result.key].soilPercent, 73);
  } finally {
    globalThis.fetch = previousFetch;
    if (previousSecret === undefined) delete process.env.CRON_SECRET;
    else process.env.CRON_SECRET = previousSecret;
  }
});

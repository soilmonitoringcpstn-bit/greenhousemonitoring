import { archiveLatestReading } from "../scripts/archive-history.mjs";

const FIREBASE_TELEMETRY_URL =
  "https://soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app/greenhouse.json";
const FIREBASE_HISTORY_URL =
  "https://soil-monitoring-system-e2d60-default-rtdb.asia-southeast1.firebasedatabase.app/control/history.json";

export async function GET(request) {
  const secret = process.env.CRON_SECRET;
  if (!secret || request.headers.get("authorization") !== `Bearer ${secret}`) {
    return Response.json({ ok: false, error: "Unauthorized" }, { status: 401 });
  }

  try {
    const result = await archiveLatestReading({
      telemetryUrl: FIREBASE_TELEMETRY_URL,
      historyUrl: FIREBASE_HISTORY_URL,
      authToken: process.env.FIREBASE_AUTH_TOKEN,
    });

    return Response.json(result.skipped
      ? { ok: true, skipped: true, reason: result.reason }
      : { ok: true, skipped: false, key: result.key });
  } catch (error) {
    console.error("Unable to archive greenhouse history:", error);
    return Response.json(
      { ok: false, error: "Unable to archive the latest reading" },
      { status: 502 },
    );
  }
}

# Smart Solar Greenhouse Monitoring

Static greenhouse monitoring dashboard for:

SMART SOLAR DRIVEN AUTOMATED GREENHOUSE WITH CLOUD MONITORING FOR SUSTAINABLE TOMATO FARMING

The site reads live data from Firebase Realtime Database and displays the current soil moisture, temperature, humidity, pump status, and latest record fields.

The hosted dashboard includes acknowledged pump controls using
`/control/command` and `/control/ack`. Set its Firebase Realtime Database
endpoints in `app-config.js`. The browser login is not a security boundary;
protect Firebase using Authentication and database rules before public use.

## Firmware requirements

Install the ESP32 Arduino core plus TinyGSM and the Adafruit DHT sensor library.
`Preferences` is included with the ESP32 core.

Hosted freshness checks use Firebase's `system.last_update_server` server
timestamp. The ESP32 `system.last_update_unix` value remains as a compatibility
fallback, so a bad device clock cannot normally make stale readings look current.

History snapshots are stored under `/control/history`. The browser saves a
snapshot when it is open, and the scheduled GitHub Actions workflow is a backup.
GitHub's schedule can be delayed or skipped, so use the protected Vercel endpoint
with an external scheduler for reliable 15-minute history on Vercel Hobby. No
ESP32 firmware change is needed.

### Set up 15-minute history on Vercel Hobby

1. In the Vercel project, add a **Production** environment variable named
   `CRON_SECRET` with a long random value (at least 32 characters). Redeploy
   the site so the Vercel Function receives it. Do not put this value in the
   repository, `app-config.js`, or the browser.
2. Create a free job at [cron-job.org](https://cron-job.org/) with the URL
   `https://YOUR-VERCEL-DOMAIN/api/archive-history`, method `GET`, and an
   execution interval of every 15 minutes. Add a custom header named
   `Authorization` whose value is `Bearer YOUR_CRON_SECRET` (the same value
   configured in Vercel).
3. Run the job once from cron-job.org and check for a JSON response containing
   `"ok":true`. `"skipped":true` means the ESP32 has not uploaded a fresh
   reading in the past 15 minutes. A `401` means the secret does not match;
   a `502` means the Firebase read or write failed. The job should then keep
   running without visits to the dashboard.

The endpoint saves at most one key per 15-minute bucket and prunes the matching
bucket past 30 days. The GitHub workflow may still fill occasional buckets, but
it must not be relied on as the primary timer.

The local `Greenhouse_Portal` stores a router SSID and password. When Wi-Fi
connects, it becomes the primary cloud route and cellular packet data is
suspended. Local and acknowledged remote commands share the same pump safety
checks.

## Complete documentation

Start with [`docs/START_HERE.md`](docs/START_HERE.md). The `docs` directory
includes:

- Architecture and data flow
- Arduino IDE setup and upload instructions
- Pin tables, wiring diagrams, and electrical safety notes
- Firmware behavior and configuration
- Function-by-function commented source-code explanation
- Captive portal and Wi-Fi setup
- Firebase schema, timestamps, and security guidance
- Hosted dashboard setup and deployment
- Commissioning tests, troubleshooting, and maintenance checklists

The complete handoff ZIP also includes the original Markdown guides and
print-ready PDF versions under `docs/pdf/`.

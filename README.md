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

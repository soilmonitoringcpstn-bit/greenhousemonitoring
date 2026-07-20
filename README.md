# Smart Solar Greenhouse Monitoring

Static greenhouse monitoring dashboard for:

SMART SOLAR DRIVEN AUTOMATED GREENHOUSE WITH CLOUD MONITORING FOR SUSTAINABLE TOMATO FARMING

The site reads live data from Firebase Realtime Database and displays the current soil moisture, temperature, humidity, pump status, and latest record fields.

The hosted dashboard is read-only. Set its Firebase Realtime Database endpoint in
`app-config.js`; application logic does not contain project-specific URLs or
hardcoded browser credentials. Pump and safety controls are available only from
the ESP32 local captive portal.

## Firmware requirements

Install the ESP32 Arduino core plus TinyGSM, DHT sensor library, and ArduinoJson.
`Preferences` is included with the ESP32 core.

Remote commands use `/control/command` and acknowledgements use `/control/ack`.
Commands require a unique `command_id`, `issued_at`, and `expires_at` Unix timestamp.
The dashboard reports success only after the ESP32 acknowledges execution.

The local `Greenhouse_Portal` can also store a backup router SSID and password.
When cellular Firebase access is unavailable, the ESP32 keeps its captive portal
active and sends cloud traffic through the configured Wi-Fi connection.

# Google Apps Script Backend

[Code.gs](Code.gs) is a self-contained Google Apps Script that turns a Google Sheet into a small JSON API: it pulls together package tracking, stock prices, calendar events, MakerWorld stats, flight status, and weather, and serves them all from one URL your ESP32 (or anything else) can `GET`. It handles caching and rate-limiting for you so you don't hammer paid/rate-limited APIs.

This is an optional, alternative data backend — a single endpoint covering several sources at once, as opposed to wiring each API directly into the sketch as shown in [API_INTEGRATION_GUIDE.md](../API_INTEGRATION_GUIDE.md).

> **Note:** the original version of this script had a `getLayoutData()` function that dumped the entire config sheet (see below) — including API keys — straight into the API response. That's been removed here. The config sheet is only ever read server-side by the script itself and is never returned to callers.

## 1. Prerequisites

- A Google account.
- A Google Sheet named **exactly** `ESP32Dashboard` somewhere in your Drive (the script finds it by name, not by ID).
- Free/paid API keys for whichever features you want (see the table below) — everything else works with no key.

## 2. Create the Apps Script project

1. Open the `ESP32Dashboard` sheet, then **Extensions → Apps Script** (or go to [script.google.com](https://script.google.com) → New project).
2. Delete the default boilerplate in `Code.gs` and paste in the full contents of this repo's [Code.gs](Code.gs).
3. Save the project (give it a name, e.g. "ESP32 Dashboard Backend").

## 3. Set up the spreadsheet tabs

Create these tabs in the `ESP32Dashboard` sheet. Column order doesn't matter for most tabs — the script looks columns up by header name — except **Layout**, where columns must be in the order below (it's read by position for a couple of fields).

### `Layout` — private config/secrets, never exposed via the API

Row 1 = headers: `ID`, `Extra1`, `Extra2`, `Extra3`, ... up to `Extra13`. Add one row per feature you're using:

| ID  | Extra1 | Extra2 | Extra3 | ... | Extra12 | Extra13 | Feature |
|-----|--------|--------|--------|-----|---------|---------|---------|
| 8   | latitude | longitude | forecast days (1–7) | | | | Weather (Open-Meteo) |
| 16  | pkge.net API key | | | | | | Package tracking |
| 64  | | | | | MakerWorld design ID | result limit | MakerWorld stats |
| 512 | AviationStack API key | daily call limit (default 4, max 24) | | | | | Flight tracking |

Only add rows for the features you actually want — a missing/empty row just makes that feature return `null` instead of erroring.

### `stocks`

One row per position, starting at row 2. Columns A–K:

| Col | Field | Notes |
|-----|-------|-------|
| A | Symbol | e.g. `NASDAQ:AAPL` — only the part after `:` is returned |
| B | Current price | e.g. `=GOOGLEFINANCE(A2,"price")` |
| C | Day high | |
| D | Day low | |
| E | 52-week high | |
| F | 52-week low | |
| G | Previous close | |
| H | Opening price | |
| I | Your average cost | used to compute `priceChangePer` |
| J | (unused) | |
| K | Total value | |

`=GOOGLEFINANCE()` formulas work well for columns B–H.

### `Tracking`

Row 1 = headers (any order): `Carrier`, `CarrierID`, `TrackingNumber`, `LastStatus`, `DeliveryStatus`, `LastChecked`, `Status`, `Inserted`. Add a row per package with `Carrier`, `CarrierID` (pkge.net courier ID, or `-1` for auto-detect), and `TrackingNumber` filled in — the script inserts it into pkge.net and keeps the rest updated automatically.

### `SkyTrack`

One row per flight you want tracked, starting at row 2:

| Col | Field |
|-----|-------|
| A | Flight date/time (departure) |
| B | Flight IATA code, e.g. `AC1234` |
| C | Enabled — `TRUE`/blank to enable, `FALSE` to skip |
| D–K | Written by the script (status, route, times, delay, airline, last updated) — leave blank |

The script always tracks whichever row is the soonest upcoming (or most recent past) flight.

### Calendar

No sheet needed — `getCalendar()` reads the default calendar of whichever Google account the script runs as.

## 4. Authorize and deploy

1. In the Apps Script editor, select the `getAllInfo` function in the toolbar dropdown and click **Run** once. Approve the permission prompts (Drive, Sheets, Calendar, external requests) — this is required before the web app will work.
2. **Deploy → New deployment → Select type: Web app.**
3. Set **Execute as: Me**, **Who has access: Anyone** (or **Anyone within [your org]** if you're on Workspace and want it restricted).
4. Click **Deploy**, authorize again if prompted, and copy the **Web app URL** (`https://script.google.com/macros/s/XXXXX/exec`).

Any time you edit `Code.gs` afterward, go to **Deploy → Manage deployments → ✏️ → New version** — editing the script alone does **not** update the live `/exec` URL.

## 5. Using the endpoint

The web app responds to `GET` requests and always returns JSON.

**Get everything** (shared cache, refreshed every 15 min):
```
GET https://script.google.com/macros/s/XXXXX/exec
```

**Get a subset**, from that same 15-min cache:
```
GET https://script.google.com/macros/s/XXXXX/exec?data=stocks,weather
```

**Per-source fetch/cache** — add `fn=1` to fetch (and cache) each requested source independently, with its own TTL, instead of one shared 15-min bundle:
```
GET https://script.google.com/macros/s/XXXXX/exec?fn=1&data=flight
```

Available keys:

| Key | Contains | TTL (per-source mode) |
|-----|----------|------------------------|
| `calEvents` | Next 24h events, days with events this month, and per-day event titles/times (`eventsByDay`, keyed by day-of-month) | 5 min |
| `stocks` | Your `stocks` sheet, with computed % change | 5 min |
| `tracking` | Package tracking status | 5 min |
| `makerworld` | Aggregated like/download/print/collection/boost counts | 10 min |
| `flight` | Current/next tracked flight status | 15 min |
| `weather` | Daily forecast (sunrise/sunset/temps/weather code) | 15 min |

## 6. Wiring it into the ESP32 sketch

Same pattern as the other integrations in [API_INTEGRATION_GUIDE.md](../API_INTEGRATION_GUIDE.md) — `HTTPClient` + `ArduinoJson`:

```c
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define DASHBOARD_API_URL "https://script.google.com/macros/s/XXXXX/exec"

void climate_update() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(DASHBOARD_API_URL) + "?data=weather");
  int httpCode = http.GET();

  if (httpCode == 200) {
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, http.getString());

    JsonObject today = doc["weather"][0];
    climate_data.temperature = today["temp_max"];
    climate_data.timestamp = millis();
  }
  http.end();
}
```

Google's redirect from `/exec` to the actual response can add latency (typically under a second, occasionally a couple of seconds) — keep the ESP32's HTTP timeout generous (5–10s) for this endpoint specifically.

## 7. Notes

- **Keep the spreadsheet private.** The `Layout` tab stores your API keys in plain text; the script only ever reads it server-side, but that protection disappears if you share the sheet itself.
- **`Execute as: Me`** means the script runs with your Google permissions no matter who calls the URL. Anyone with the `/exec` link can read your data — restrict "Who has access" if that matters, or add your own shared-secret check (e.g. compare `e.parameter.token` at the top of `doGet`) before deploying somewhere semi-public.
- AviationStack's free tier is limited, which is why the script self-throttles to a configurable number of calls per day (`Layout` row `512`, `Extra2`) using `PropertiesService`.
- `CacheService` entries cap out at 6 hours per `put()` call — that's the ceiling for any TTL in this script, not just the ones listed above.

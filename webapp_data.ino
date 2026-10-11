// ==================== Shared Dashboard Data API ====================
// Single Google Apps Script web app that pre-aggregates stocks, calendar
// events, and MakerWorld stats server-side and hands back one compact JSON
// response (a few KB, regardless of how much raw data went into it) -
// replacing three separate direct-to-API fetches this dashboard used to
// make: Yahoo Finance for stocks, a Google Calendar ICS feed, and
// MakerWorld's raw design-service endpoint, the last of which alone
// routinely ran 500KB-900KB+ per fetch (see calendar.ino/stocks.ino/
// makerworld.ino's header comments). Doing the heavy fetch/parse/sum
// server-side, where memory is free, is a much better fit for this than
// cramming three separate parsers onto the ESP32.
//
// settings.dashboard_script_id is just the deployment ID from the Apps
// Script web app URL (.../macros/s/<ID>/exec) - set on the Wi-Fi setup
// page. The response shape (only the fields this reads are shown):
//   {
//     "stocks": [{"name":"AAPL","price":..,"prevClose":..}, ...],
//     "calEvents": {
//       "daysWithEvents": [19,20,22,...],
//       "eventsByDay": {"19": [{"title":"...","time":"14:30","allDay":false}, ...], ...}
//     },
//     "makerworld": {"total": {"likes":..,"downloads":..,"prints":..,
//                               "collections":..,"boosts":..}}
//   }
//
// This is an Arduino multi-tab sketch: all .ino files in this folder are
// concatenated into one translation unit at build time, so no #include of
// this file is needed. This file is named to sort alphabetically after
// calendar.ino/makerworld.ino/stocks.ino specifically so the arrays/structs
// it writes into (calendar_event_days, stocks[], makerworld_stats, ...)
// are already declared by the time this compiles - Arduino auto-prototypes
// functions across every tab regardless of order, but plain variables
// still have to be declared before their first use in the concatenated
// file. last_dashboard_fetch_ms/DASHBOARD_FETCH_INTERVAL_MS live in
// esp32_dashboard.ino's top globals block instead of here, since
// network_task() there reads/writes them directly and that file is always
// compiled first - see its comment.

// Blocks for a few seconds during the HTTPS GET + JSON parse, same
// trade-off network_task() already makes for fetch_weather(). The combined
// response is small enough (a few KB) that buffering it whole with
// getString() is safe here, unlike the old per-source fetchers this
// replaced.
void fetch_dashboard_data()
{
  last_dashboard_fetch_ms = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  if (settings.dashboard_script_id[0] == '\0') return;

  makerworld_fetch_attempted = true;

  String url = "https://script.google.com/macros/s/";
  url += settings.dashboard_script_id;
  url += "/exec";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS); // Apps Script exec URLs redirect once to the actual output
  if (!http.begin(client, url))
  {
    Serial.println("[DashboardAPI] begin() failed");
    stocks_needs_ui_refresh = true;
    calendar_needs_ui_refresh = true;
    makerworld_needs_ui_refresh = true;
    return;
  }

  // Default 5s (HTTPCLIENT_DEFAULT_TCP_TIMEOUT) is too short for this one -
  // Apps Script web apps can take several seconds to execute (cold start +
  // whatever the script itself pulls together), on top of the redirect this
  // request already follows to script.googleusercontent.com. Was surfacing
  // as GET failing with -11 (HTTPC_ERROR_READ_TIMEOUT).
  http.setConnectTimeout(8000);
  http.setTimeout(15000);

  int code = http.GET();
  if (code != HTTP_CODE_OK)
  {
    Serial.printf("[DashboardAPI] GET failed: %d\n", code);
    http.end();
    stocks_needs_ui_refresh = true;
    calendar_needs_ui_refresh = true;
    makerworld_needs_ui_refresh = true;
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err)
  {
    Serial.printf("[DashboardAPI] JSON parse failed: %s\n", err.c_str());
    stocks_needs_ui_refresh = true;
    calendar_needs_ui_refresh = true;
    makerworld_needs_ui_refresh = true;
    return;
  }

  // ---- MakerWorld ----
  JsonObject mw = doc["makerworld"]["total"];
  if (!mw.isNull())
  {
    makerworld_stats.likes = mw["likes"] | 0L;
    makerworld_stats.downloads = mw["downloads"] | 0L;
    makerworld_stats.prints = mw["prints"] | 0L;
    makerworld_stats.collections = mw["collections"] | 0L;
    makerworld_stats.boosts = mw["boosts"] | 0L;
    makerworld_stats.valid = true;
  }
  makerworld_needs_ui_refresh = true;

  // ---- Stocks ----
  JsonArray stockArr = doc["stocks"];
  int stock_n = 0;
  if (!stockArr.isNull())
  {
    for (JsonObject item : stockArr)
    {
      if (stock_n >= STOCK_COUNT) break;
      const char *sym = item["name"] | "";
      strncpy(stocks[stock_n].symbol, sym, sizeof(stocks[stock_n].symbol) - 1);
      stocks[stock_n].symbol[sizeof(stocks[stock_n].symbol) - 1] = '\0';
      float price = item["price"] | 0.0f;
      float prevClose = item["prevClose"] | 0.0f;
      stocks[stock_n].price = price;
      stocks[stock_n].change_pct = (prevClose != 0) ? (price - prevClose) / prevClose * 100.0f : 0.0f;
      stocks[stock_n].valid = (price > 0);
      stock_n++;
    }
  }
  for (int i = stock_n; i < STOCK_COUNT; i++) stocks[i].symbol[0] = '\0';
  stocks_needs_ui_refresh = true;

  // ---- Calendar ----
  JsonObject calObj = doc["calEvents"];
  for (int d = 1; d <= CALENDAR_MAX_DAY; d++)
  {
    calendar_event_days[d] = false;
    calendar_event_count_per_day[d] = 0;
  }
  int day_count = 0;
  JsonArray days = calObj["daysWithEvents"];
  if (!days.isNull())
  {
    for (JsonVariant v : days)
    {
      int d = v.as<int>();
      if (d >= 1 && d <= CALENDAR_MAX_DAY)
      {
        calendar_event_days[d] = true;
        day_count++;
      }
    }
  }
  calendar_event_count = day_count;

  // Per-day event titles/times for the day-tap detail screen (see
  // calendar.ino) - keyed by day-of-month, each entry a {title, time,
  // allDay} object with time already formatted server-side (see
  // Code.gs/getCalendar()'s comment on why - no timezone math needed here).
  JsonObject eventsByDay = calObj["eventsByDay"];
  if (!eventsByDay.isNull())
  {
    for (JsonPair kv : eventsByDay)
    {
      int d = atoi(kv.key().c_str());
      if (d < 1 || d > CALENDAR_MAX_DAY) continue;

      JsonArray dayEvents = kv.value().as<JsonArray>();
      if (dayEvents.isNull()) continue;

      int n = 0;
      for (JsonObject ev : dayEvents)
      {
        if (n >= CALENDAR_MAX_EVENTS_PER_DAY) break;
        const char *title = ev["title"] | "";
        const char *time = ev["time"] | "";
        strncpy(calendar_events[d][n].title, title, sizeof(calendar_events[d][n].title) - 1);
        calendar_events[d][n].title[sizeof(calendar_events[d][n].title) - 1] = '\0';
        strncpy(calendar_events[d][n].time, time, sizeof(calendar_events[d][n].time) - 1);
        calendar_events[d][n].time[sizeof(calendar_events[d][n].time) - 1] = '\0';
        calendar_events[d][n].all_day = ev["allDay"] | false;
        n++;
      }
      calendar_event_count_per_day[d] = n;
    }
  }

  calendar_have_data = true;
  calendar_needs_ui_refresh = true;

  Serial.printf("[DashboardAPI] Synced - makerworld valid=%d, %d stock(s), %d calendar day(s)\n",
                makerworld_stats.valid, stock_n, day_count);
}

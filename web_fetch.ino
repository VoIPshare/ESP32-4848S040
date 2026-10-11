// ==================== Direct data fetches ====================
// Schedules the fetches in direct_fetch.cpp (stocks, calendar, MakerWorld,
// package tracking, live flight status) and copies their results into the
// screens' data. Each service is called directly from the ESP32 - this
// replaces the Google Apps Script web app the dashboard used to read
// everything through, the same way the e-ink dashboard
// (ESP32-eInk-Dashboard) does it.
//
// Everything here runs on network_task, never on the LVGL thread: results
// are copied in under data_lock() and a *_needs_ui_refresh flag tells
// loop() to redraw. Weather and Proxmox keep their own fetchers
// (fetch_weather() in esp32_dashboard.ino, proxmox_fetch() in proxmox.ino).
//
// This file is named to sort alphabetically after calendar.ino/
// makerworld.ino/stocks.ino/tracking.ino, so the variables it writes into
// (calendar, stocks[], makerworld_stats, tracking[], ...) are already
// declared by the time this compiles - Arduino auto-prototypes functions
// across every tab regardless of order, but plain variables still have to
// be declared before their first use in the concatenated file.

#define STOCKS_FETCH_INTERVAL_MS (10UL * 60UL * 1000UL)
#define CALENDAR_FETCH_INTERVAL_MS (30UL * 60UL * 1000UL)
#define MAKERWORLD_FETCH_INTERVAL_MS (60UL * 60UL * 1000UL) // ~1 MB download, so not too often
#define TRACKING_FETCH_INTERVAL_MS (2UL * 60UL * 60UL * 1000UL)
#define TRACKING_EXPIRE_CHECK_MS (10UL * 60UL * 1000UL)
#define FLIGHT_CHECK_INTERVAL_MS (60UL * 1000UL)
#define CLOCK_RETRY_FETCH_MS (15UL * 1000UL) // calendar retry while the clock isn't set yet

#define FLIGHT_LIVE_BEFORE_S (6 * 3600)     // live flight status starts 6 h before departure
#define FLIGHT_LIVE_REFRESH_S (30 * 60)     // and is refreshed every 30 minutes until landing,
#define FLIGHT_LIVE_FAST_S (10 * 60)        // every 10 minutes within an hour of departure or landing
#define FLIGHT_LIVE_AFTER_MAX_S (3 * 3600)  // safety: never follow a flight more than 3 h past its expected landing
#define FLIGHT_LIVE_MAX_REQUESTS 30         // safety: at most this many AeroDataBox requests per flight

static uint32_t last_stocks_fetch_ms = 0;
static uint32_t last_calendar_fetch_ms = 0;
static uint32_t last_makerworld_fetch_ms = 0;
static uint32_t last_tracking_fetch_ms = 0;
static uint32_t last_tracking_expire_ms = 0;
static uint32_t last_flight_check_ms = 0;

static bool clock_is_set()
{
  return time(nullptr) > 1700000000;
}

void fetch_stocks()
{
  last_stocks_fetch_ms = millis();
  static DfQuote quotes[STOCK_COUNT];
  int n = 0, failed = 0;
  bool ok = df_fetch_stocks(settings.stock_symbols, quotes, STOCK_COUNT, &n, &failed);
  // Every symbol failed: keep the previous quotes on screen.
  if (!ok && settings.stock_symbols[0]) return;

  data_lock();
  for (int i = 0; i < STOCK_COUNT; i++)
  {
    StockQuote &s = stocks[i];
    if (i < n)
    {
      snprintf(s.symbol, sizeof(s.symbol), "%s", quotes[i].symbol);
      s.price = quotes[i].price;
      s.change_pct = quotes[i].prev_close != 0 ? (quotes[i].price - quotes[i].prev_close) / quotes[i].prev_close * 100.0f : 0.0f;
      s.valid = quotes[i].price > 0;
    }
    else
    {
      s.symbol[0] = '\0';
      s.valid = false;
    }
  }
  data_unlock();
  stocks_needs_ui_refresh = true;
  if (failed) Serial.printf("[Stocks] %d symbol(s) failed\n", failed);
}

void fetch_calendar()
{
  if (!calendar_has_feeds())
  {
    last_calendar_fetch_ms = millis();
    data_lock();
    calendar_have_data = false;
    data_unlock();
    calendar_needs_ui_refresh = true;
    return;
  }
  // Recurrence and day boundaries need the local date: retry soon until NTP lands.
  if (!clock_is_set())
  {
    last_calendar_fetch_ms = millis() - CALENDAR_FETCH_INTERVAL_MS + CLOCK_RETRY_FETCH_MS;
    return;
  }
  last_calendar_fetch_ms = millis();

  static CalendarData *staging = NULL;
  if (!staging) staging = (CalendarData *)psram_calloc(sizeof(CalendarData));
  if (!staging) return;

  int failed = 0;
  bool ok = df_fetch_calendar(settings.ics_urls, ICS_MAX_FEEDS, staging, &failed);

  data_lock();
  CalendarData *c = calendar_data();
  if (ok && c)
  {
    memcpy(c, staging, sizeof(CalendarData));
    calendar_have_data = true;
  }
  calendar_fetch_failed = !ok;
  data_unlock();
  calendar_needs_ui_refresh = true;
  if (failed) Serial.printf("[ICS] %d feed(s) failed\n", failed);
}

void fetch_makerworld()
{
  last_makerworld_fetch_ms = millis();
  if (!settings.makerworld_uid[0])
  {
    makerworld_needs_ui_refresh = true;
    return;
  }

  DfMakerWorld mw;
  bool ok = df_fetch_makerworld(settings.makerworld_uid, &mw);

  data_lock();
  makerworld_fetch_attempted = true;
  // A failed fetch keeps the previous totals on screen.
  if (ok)
  {
    makerworld_stats.likes = mw.likes;
    makerworld_stats.downloads = mw.downloads;
    makerworld_stats.prints = mw.prints;
    makerworld_stats.collections = mw.collections;
    makerworld_stats.boosts = mw.boosts;
    makerworld_stats.valid = true;
  }
  data_unlock();
  makerworld_needs_ui_refresh = true;
}

void fetch_tracking()
{
  last_tracking_fetch_ms = millis();
  if (!settings.pkge_api_key[0])
  {
    data_lock();
    tracking_count = 0;
    data_unlock();
    tracking_needs_ui_refresh = true;
    return;
  }

  TrackingItem found[TRACKING_MAX];
  int n = 0;
  bool ok = df_fetch_tracking(settings.pkge_api_key, found, TRACKING_MAX, &n);

  data_lock();
  tracking_fetch_attempted = true;
  tracking_fetch_ok = ok;
  // A failed fetch keeps the previous list on screen.
  if (ok)
  {
    memcpy(tracking, found, sizeof(TrackingItem) * n);
    tracking_count = n;
  }
  data_unlock();
  tracking_needs_ui_refresh = true;
}

// Delivered parcels stay for a day; drop them without a download.
void tracking_expire()
{
  last_tracking_expire_ms = millis();
  data_lock();
  int before = tracking_count;
  tracking_count = df_tracking_drop_expired(tracking, tracking_count);
  bool changed = tracking_count != before;
  data_unlock();
  if (changed) tracking_needs_ui_refresh = true;
}

// Follows the next flight from the calendar from 6 hours before departure
// until it lands (AeroDataBox), with request caps so a missing "landed"
// status can't burn through the API quota. Returns true if it fetched.
bool flight_maintain()
{
  last_flight_check_ms = millis();
  if (!clock_is_set()) return false;
  time_t now = time(nullptr);

  UpcomingEvent next = {};
  LiveFlight f;
  data_lock();
  if (calendar && calendar_have_data) next = calendar->next_flight;
  f = live_flight;
  data_unlock();

  bool changed = false;
  if (next.start && next.start != f.cal_start && next.start - now < FLIGHT_LIVE_BEFORE_S)
  {
    f = {};
    f.cal_start = next.start;
    f.cal_end = next.end;
    df_extract_flight_number(next.title, f.number, sizeof(f.number));
    changed = true;
  }

  // Stop 1 h after the actual landing, and in any case at most
  // FLIGHT_LIVE_AFTER_MAX_S after the expected landing (scheduled arrival,
  // else the calendar end), even if "landed" is never reported.
  time_t expected_landing = f.arr_scheduled ? f.arr_scheduled : (f.cal_end > f.cal_start ? f.cal_end : f.cal_start);
  time_t live_limit = expected_landing + FLIGHT_LIVE_AFTER_MAX_S;
  if (f.arr_actual && f.arr_actual + 3600 < live_limit) live_limit = f.arr_actual + 3600;
  bool live_window = f.cal_start && now >= f.cal_start - FLIGHT_LIVE_BEFORE_S && now < live_limit;
  bool finished = f.valid && (!strcmp(f.status, "Arrived") || !strcmp(f.status, "Canceled") || !strcmp(f.status, "Diverted"));
  time_t dep = f.dep_actual ? f.dep_actual : (f.dep_scheduled ? f.dep_scheduled : f.cal_start);
  time_t arr = f.arr_actual ? f.arr_actual : f.arr_scheduled;
  bool near_movement = (now > dep - 3600 && now < dep + 1800) || (arr && now > arr - 3600 && now < arr + 1800);
  time_t refresh = near_movement ? FLIGHT_LIVE_FAST_S : FLIGHT_LIVE_REFRESH_S;
  bool due = live_window && !finished && f.number[0] && settings.aerodatabox_key[0] &&
             f.requests < FLIGHT_LIVE_MAX_REQUESTS && (f.last_fetch == 0 || now - f.last_fetch >= refresh);

  if (due)
  {
    df_fetch_flight(settings.aerodatabox_key, &f);
    f.last_fetch = time(nullptr); // also after a failure: retried at the next interval
    f.requests++;
    changed = true;
  }

  if (changed)
  {
    data_lock();
    live_flight = f;
    data_unlock();
    calendar_needs_ui_refresh = true; // redraws the Home strip
  }
  return due;
}

// Everything at once - right after Wi-Fi connects. MakerWorld goes last
// since it's by far the slowest download.
void direct_fetch_all()
{
  fetch_stocks();
  fetch_calendar();
  fetch_tracking();
  flight_maintain();
  fetch_makerworld();
}

// Called on every network_task pass while connected. Runs at most one
// fetch per call, so MQTT and the web portal get serviced in between.
void direct_fetch_maintain()
{
  uint32_t now_ms = millis();

  // Right after midnight on the 1st the per-day data still describes last
  // month: refetch as soon as that's noticed.
  bool month_changed = false;
  if (clock_is_set())
  {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    data_lock();
    month_changed = calendar && calendar_have_data && (calendar->month != lt.tm_mon || calendar->year != lt.tm_year + 1900);
    data_unlock();
  }

  if (now_ms - last_calendar_fetch_ms > CALENDAR_FETCH_INTERVAL_MS ||
      (month_changed && now_ms - last_calendar_fetch_ms > 60000UL))
  {
    fetch_calendar();
    return;
  }
  if (now_ms - last_flight_check_ms > FLIGHT_CHECK_INTERVAL_MS && flight_maintain()) return;
  if (now_ms - last_stocks_fetch_ms > STOCKS_FETCH_INTERVAL_MS) { fetch_stocks(); return; }
  if (now_ms - last_tracking_fetch_ms > TRACKING_FETCH_INTERVAL_MS) { fetch_tracking(); return; }
  if (now_ms - last_tracking_expire_ms > TRACKING_EXPIRE_CHECK_MS) tracking_expire();
  if (now_ms - last_makerworld_fetch_ms > MAKERWORLD_FETCH_INTERVAL_MS) { fetch_makerworld(); return; }
}

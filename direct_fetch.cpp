// Direct-to-service fetches - see direct_fetch.h. Ported from the e-ink
// dashboard's directFetch.cpp, adapted to fill this dashboard's structs.
#include "direct_fetch.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ctype.h>
#include <new>

#define DF_USER_AGENT "Mozilla/5.0 (ESP32 Dashboard)"

// ----------------------- Helpers -----------------------

static void url_encode(const char *in, char *out, size_t cap)
{
  static const char hex[] = "0123456789ABCDEF";
  size_t o = 0;
  for (; *in && o + 4 < cap; in++)
  {
    unsigned char c = (unsigned char)*in;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      out[o++] = (char)c;
    }
    else
    {
      out[o++] = '%';
      out[o++] = hex[c >> 4];
      out[o++] = hex[c & 0x0F];
    }
  }
  out[o] = '\0';
}

// Small answers only: the whole body is buffered with getString() (the
// stream deserializer mis-detects EOF on chunked HTTPS answers).
static bool http_get_json(const char *url, JsonDocument &doc, const JsonDocument *filter = nullptr)
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return false;
  http.setUserAgent(DF_USER_AGENT);

  int code = http.GET();
  if (code != 200)
  {
    Serial.printf("[Direct] HTTP %d for %s\n", code, url);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  DeserializationError err = filter
                                 ? deserializeJson(doc, body, DeserializationOption::Filter(*filter))
                                 : deserializeJson(doc, body);
  if (err)
  {
    Serial.printf("[Direct] JSON error %s for %s\n", err.c_str(), url);
    return false;
  }
  return true;
}

// Reader for ArduinoJson that buffers the TLS stream and tolerates pauses in
// a large download (Stream::read() returning -1 mid-transfer would otherwise
// end the JSON as IncompleteInput). Used with useHTTP10(true), so the raw
// stream is the body (no chunked encoding).
class PatientStreamReader
{
public:
  explicit PatientStreamReader(WiFiClient *s) : stream(s) {}

  int read()
  {
    if (pos >= len && !fill()) return -1;
    return buf[pos++];
  }

  size_t readBytes(char *out, size_t n)
  {
    size_t done = 0;
    while (done < n)
    {
      if (pos >= len && !fill()) break;
      size_t chunk = min(n - done, len - pos);
      memcpy(out + done, buf + pos, chunk);
      pos += chunk;
      done += chunk;
    }
    return done;
  }

  size_t total = 0;

private:
  WiFiClient *stream;
  uint8_t buf[1024];
  size_t len = 0, pos = 0;

  bool fill()
  {
    unsigned long start = millis();
    while (millis() - start < 10000)
    {
      int avail = stream->available();
      if (avail > 0)
      {
        len = stream->read(buf, min((size_t)avail, sizeof(buf)));
        pos = 0;
        if (len > 0) { total += len; return true; }
      }
      else if (!stream->connected())
      {
        return false;
      }
      delay(2);
    }
    return false;
  }
};

// Days since 1970-01-01 for a civil date (proleptic Gregorian).
static long days_from_civil(int y, int m, int d)
{
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

static int days_in_month(int y, int m /* 1-12 */)
{
  static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
  return (m == 2 && leap) ? 29 : dim[(m - 1) % 12];
}

static time_t make_local(int y, int mon, int d, int hh, int mm, int ss)
{
  struct tm t = {};
  t.tm_year = y - 1900;
  t.tm_mon = mon - 1;
  t.tm_mday = d;
  t.tm_hour = hh;
  t.tm_min = mm;
  t.tm_sec = ss;
  t.tm_isdst = -1;
  return mktime(&t);
}

// "2021-07-06T13:46:25+03:00", "2026-10-15 18:05Z", "...Z" or "2021-07-06"
// -> UTC epoch seconds (0 if invalid). Seconds, fractions and the offset are optional.
static time_t parse_iso_utc(const char *s)
{
  int y, mo, d, hh = 0, mm = 0, ss = 0, n = 0;
  if (!s || sscanf(s, "%4d-%2d-%2d", &y, &mo, &d) != 3) return 0;
  time_t t = (time_t)days_from_civil(y, mo, d) * 86400;
  if (s[10] != 'T' && s[10] != ' ') return t;
  const char *p = s + 11;
  if (sscanf(p, "%2d:%2d%n", &hh, &mm, &n) < 2) return t;
  p += n;
  if (*p == ':') { ss = atoi(p + 1); p += 3; }
  while (*p == '.' || isdigit((unsigned char)*p)) p++; // fractional seconds
  t += hh * 3600 + mm * 60 + ss;
  if (*p == '+' || *p == '-')
  {
    int oh = 0, om = 0;
    sscanf(p + 1, "%2d:%2d", &oh, &om);
    t -= (oh * 3600 + om * 60) * (*p == '+' ? 1 : -1);
  }
  return t;
}

// ----------------------- Weather location (Open-Meteo geocoding) -----------------------

static char cached_location[65] = "";
static float cached_lat = 0, cached_lon = 0;

bool df_resolve_location(const char *location, float *lat, float *lon)
{
  if (!location || location[0] == '\0') return false;

  if (sscanf(location, "%f , %f", lat, lon) == 2) return true;

  if (strcmp(cached_location, location) == 0)
  {
    *lat = cached_lat;
    *lon = cached_lon;
    return true;
  }

  // "City" or "City, CC" where CC is a 2-letter country code.
  char city[65];
  char country[3] = "";
  snprintf(city, sizeof(city), "%s", location);
  char *comma = strchr(city, ',');
  if (comma)
  {
    *comma = '\0';
    const char *cc = comma + 1;
    while (*cc == ' ') cc++;
    if (strlen(cc) == 2)
    {
      country[0] = (char)toupper((unsigned char)cc[0]);
      country[1] = (char)toupper((unsigned char)cc[1]);
      country[2] = '\0';
    }
  }
  for (int i = (int)strlen(city) - 1; i >= 0 && city[i] == ' '; i--) city[i] = '\0';

  char encoded[65 * 3];
  url_encode(city, encoded, sizeof(encoded));

  char url[320];
  snprintf(url, sizeof(url),
           "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&format=json%s%s",
           encoded, country[0] ? "&countryCode=" : "", country);

  JsonDocument doc;
  if (!http_get_json(url, doc)) return false;

  JsonObject r = doc["results"][0];
  if (r.isNull())
  {
    Serial.printf("[Weather] Location not found: %s\n", location);
    return false;
  }
  *lat = r["latitude"] | 0.0f;
  *lon = r["longitude"] | 0.0f;

  cached_lat = *lat;
  cached_lon = *lon;
  snprintf(cached_location, sizeof(cached_location), "%s", location);
  Serial.printf("[Weather] %s -> %.4f,%.4f\n", location, *lat, *lon);
  return true;
}

// ----------------------- Stocks (Yahoo Finance) -----------------------

bool df_fetch_stocks(const char *symbols, DfQuote *out, int max, int *count, int *failed)
{
  int n = 0, failures = 0;
  if (symbols && symbols[0])
  {
    JsonDocument filter;
    JsonObject meta = filter["chart"]["result"][0]["meta"].to<JsonObject>();
    meta["regularMarketPrice"] = true;
    meta["chartPreviousClose"] = true;
    meta["previousClose"] = true;
    filter["chart"]["result"][0]["indicators"]["quote"][0]["close"] = true;

    char list[96];
    snprintf(list, sizeof(list), "%s", symbols);

    char *save = nullptr;
    for (char *sym = strtok_r(list, ", ", &save); sym && n < max; sym = strtok_r(nullptr, ", ", &save))
    {
      char encoded[48];
      url_encode(sym, encoded, sizeof(encoded));

      char url[160];
      snprintf(url, sizeof(url), "https://query1.finance.yahoo.com/v8/finance/chart/%s?range=1d&interval=15m", encoded);

      JsonDocument doc;
      if (!http_get_json(url, doc, &filter)) { failures++; continue; }

      JsonObject m = doc["chart"]["result"][0]["meta"];
      if (m.isNull() || m["regularMarketPrice"].isNull())
      {
        Serial.printf("[Stocks] No quote for %s\n", sym);
        failures++;
        continue;
      }

      DfQuote &q = out[n++];
      // Drop the index prefix for display, e.g. "^IXIC" -> "IXIC".
      snprintf(q.symbol, sizeof(q.symbol), "%s", sym[0] == '^' ? sym + 1 : sym);
      q.price = m["regularMarketPrice"] | 0.0f;
      q.prev_close = m["chartPreviousClose"] | (m["previousClose"] | 0.0f);
      // Today's 15-minute closes for the sparkline (gaps come as null).
      q.spark_n = 0;
      for (JsonVariant c : doc["chart"]["result"][0]["indicators"]["quote"][0]["close"].as<JsonArray>())
        if (!c.isNull() && q.spark_n < STOCK_SPARK_MAX) q.spark[q.spark_n++] = c.as<float>();
      Serial.printf("[Stocks] %s %.2f (prev %.2f)\n", q.symbol, q.price, q.prev_close);
    }
  }
  if (count) *count = n;
  if (failed) *failed = failures;
  return n > 0;
}

// ----------------------- MakerWorld (Bambu API) -----------------------

// Copies a UTF-8 string, cut at a character boundary so a multi-byte
// character is never split.
static void utf8_copy(char *dst, const char *src, size_t n)
{
  size_t len = strlen(src);
  if (len >= n)
  {
    len = n - 1;
    while (len > 0 && (src[len] & 0xC0) == 0x80) len--; // back to the start of that character
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
  while (len > 0 && dst[len - 1] == ' ') dst[--len] = '\0';
}

bool df_fetch_makerworld(const char *uid, DfMakerWorld *out)
{
  if (!uid || uid[0] == '\0') return false;

  // makerworld.com is behind a browser check; Bambu's API host serves the same data directly.
  char url[160];
  snprintf(url, sizeof(url), "https://api.bambulab.com/v1/design-service/published/%s/design?limit=100", uid);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true); // no chunked encoding, so the ~1 MB answer can be parsed as a stream
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;
  http.setUserAgent(DF_USER_AGENT);

  int code = http.GET();
  if (code != 200)
  {
    Serial.printf("[MakerWorld] HTTP %d\n", code);
    http.end();
    return false;
  }

  // Keep only the counters of each design; everything else (covers, titles, ...) is skipped.
  JsonDocument filter;
  JsonObject f = filter["hits"][0].to<JsonObject>();
  f["likeCount"] = true;
  f["downloadCount"] = true;
  f["printCount"] = true;
  f["collectionCount"] = true;
  f["boostCnt"] = true;
  f["title"] = true;

  PatientStreamReader *reader = new (std::nothrow) PatientStreamReader(http.getStreamPtr());
  if (!reader) { http.end(); return false; }

  unsigned long t0 = millis();
  JsonDocument doc;
  // The skipped parts of each design are nested deeper than the default limit of 10.
  DeserializationError err = deserializeJson(doc, *reader,
                                             DeserializationOption::Filter(filter),
                                             DeserializationOption::NestingLimit(40));
  size_t total = reader->total;
  delete reader;
  http.end();
  if (err)
  {
    Serial.printf("[MakerWorld] JSON error %s after %u bytes\n", err.c_str(), (unsigned)total);
    return false;
  }

  JsonArray hits = doc["hits"];
  if (hits.isNull()) return false;

  *out = {};
  for (JsonObject h : hits)
  {
    long downloads = h["downloadCount"] | 0L;
    long likes = h["likeCount"] | 0L;
    out->likes += likes;
    out->downloads += downloads;
    out->prints += h["printCount"] | 0L;
    out->collections += h["collectionCount"] | 0L;
    out->boosts += h["boostCnt"] | 0L;
    out->designs++;

    // Insertion into the top list, most downloaded first.
    int pos = out->top_count;
    while (pos > 0 && out->top_downloads[pos - 1] < downloads) pos--;
    if (pos >= MW_TOP_DESIGNS) continue;
    int last = out->top_count < MW_TOP_DESIGNS ? out->top_count : MW_TOP_DESIGNS - 1;
    for (int i = last; i > pos; i--)
    {
      memcpy(out->top_title[i], out->top_title[i - 1], MW_TITLE_MAX);
      out->top_downloads[i] = out->top_downloads[i - 1];
      out->top_likes[i] = out->top_likes[i - 1];
    }
    utf8_copy(out->top_title[pos], h["title"] | "", MW_TITLE_MAX);
    out->top_downloads[pos] = downloads;
    out->top_likes[pos] = likes;
    if (out->top_count < MW_TOP_DESIGNS) out->top_count++;
  }
  Serial.printf("[MakerWorld] %u designs in %lu ms: likes %ld downloads %ld prints %ld boosts %ld\n",
                (unsigned)hits.size(), millis() - t0, out->likes, out->downloads, out->prints, out->boosts);
  return true;
}

bool df_fetch_makerworld_profile(const char *uid, DfMakerWorldProfile *out)
{
  if (!uid || uid[0] == '\0') return false;
  char url[128];
  snprintf(url, sizeof(url), "https://api.bambulab.com/v1/user-service/user/profile/%s", uid);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;
  http.setUserAgent(DF_USER_AGENT);
  int code = http.GET();
  if (code != 200)
  {
    Serial.printf("[MakerWorld] profile HTTP %d\n", code);
    http.end();
    return false;
  }

  JsonDocument filter;
  filter["fanCount"] = true;
  filter["personal"]["userLevel"]["level"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) return false;

  out->followers = doc["fanCount"] | 0L;
  out->level = doc["personal"]["userLevel"]["level"] | 0;
  return true;
}

// ----------------------- Calendar (.ics) -----------------------

#define ICS_MAX_EXDATES 16
#define ICS_MAX_BYDAY 7
#define ICS_MAX_STEPS 1500           // safety limit per recurring event
#define ICS_LOOKAHEAD_S (7 * 86400)  // upcoming events and flights: up to a week ahead

struct IcsEvent
{
  char summary[CALENDAR_TITLE_MAX];
  time_t start;
  time_t end;
  bool has_end;
  bool all_day;
  bool cancelled;

  // RRULE subset: FREQ, INTERVAL, UNTIL, COUNT, BYDAY
  char freq; // 0 = none, 'D', 'W', 'M', 'Y'
  int interval;
  time_t until; // 0 = none
  int count;    // 0 = none
  int by_day_count;
  int8_t by_day_wd[ICS_MAX_BYDAY]; // 0 = Monday ... 6 = Sunday
  int8_t by_day_n[ICS_MAX_BYDAY];  // 0 = every, 2 = second, -1 = last (MONTHLY only)

  time_t exdates[ICS_MAX_EXDATES];
  int exdate_count;
};

struct IcsWindow
{
  time_t now;
  time_t from;        // where expanding starts: the month's 1st, or now if that's earlier
  time_t month_start; // local midnight on the 1st of the month the day lists are for
  time_t to;          // max(now + 7 days, first day of next month)
  time_t month_end;
  CalendarData *out;
  uint8_t feed; // the feed being read - kept on every event it adds
};

// Parses "20261010", "20261010T140000" (local / TZID) or "20261010T140000Z" (UTC).
static time_t parse_ics_value(const char *v, bool *all_day)
{
  int y = 0, mon = 0, d = 0, hh = 0, mm = 0, ss = 0;
  if (sscanf(v, "%4d%2d%2d", &y, &mon, &d) != 3) return 0;
  if (v[8] != 'T')
  {
    if (all_day) *all_day = true;
    return make_local(y, mon, d, 0, 0, 0);
  }
  if (all_day) *all_day = false;
  sscanf(v + 9, "%2d%2d%2d", &hh, &mm, &ss);
  if (v[15] == 'Z') return (time_t)days_from_civil(y, mon, d) * 86400 + hh * 3600 + mm * 60 + ss;
  // TZID times are treated as the dashboard's own timezone.
  return make_local(y, mon, d, hh, mm, ss);
}

static int weekday_index(const char *s)
{
  static const char *names[] = {"MO", "TU", "WE", "TH", "FR", "SA", "SU"};
  for (int i = 0; i < 7; i++)
    if (s[0] == names[i][0] && s[1] == names[i][1]) return i;
  return -1;
}

static void parse_rrule(const char *value, IcsEvent &ev)
{
  char buf[256];
  snprintf(buf, sizeof(buf), "%s", value);
  char *save = nullptr;
  for (char *part = strtok_r(buf, ";", &save); part; part = strtok_r(nullptr, ";", &save))
  {
    if (!strncmp(part, "FREQ=", 5))
    {
      const char *f = part + 5;
      if (!strcmp(f, "DAILY")) ev.freq = 'D';
      else if (!strcmp(f, "WEEKLY")) ev.freq = 'W';
      else if (!strcmp(f, "MONTHLY")) ev.freq = 'M';
      else if (!strcmp(f, "YEARLY")) ev.freq = 'Y';
    }
    else if (!strncmp(part, "INTERVAL=", 9))
    {
      ev.interval = max(1, atoi(part + 9));
    }
    else if (!strncmp(part, "UNTIL=", 6))
    {
      ev.until = parse_ics_value(part + 6, nullptr);
    }
    else if (!strncmp(part, "COUNT=", 6))
    {
      ev.count = atoi(part + 6);
    }
    else if (!strncmp(part, "BYDAY=", 6))
    {
      char *save2 = nullptr;
      for (char *day = strtok_r(part + 6, ",", &save2); day && ev.by_day_count < ICS_MAX_BYDAY; day = strtok_r(nullptr, ",", &save2))
      {
        int n = atoi(day); // "2TU" -> 2, "-1FR" -> -1, "MO" -> 0
        const char *name = day;
        while (*name == '-' || *name == '+' || isdigit((unsigned char)*name)) name++;
        int wd = weekday_index(name);
        if (wd < 0) continue;
        ev.by_day_wd[ev.by_day_count] = (int8_t)wd;
        ev.by_day_n[ev.by_day_count] = (int8_t)n;
        ev.by_day_count++;
      }
    }
  }
}

// Undo iCalendar TEXT escaping: "\," "\;" "\\" and "\n".
static void unescape_text(char *s)
{
  char *out = s;
  for (char *in = s; *in; in++)
  {
    if (*in == '\\' && in[1])
    {
      in++;
      *out++ = (*in == 'n' || *in == 'N') ? ' ' : *in;
    }
    else
    {
      *out++ = *in;
    }
  }
  *out = '\0';
}

static void add_upcoming(CalendarData *out, const IcsEvent &ev, time_t s, time_t e, uint8_t feed)
{
  const int cap = CALENDAR_UPCOMING_MAX;
  int n = out->upcoming_count;
  int pos = n;
  while (pos > 0 && out->upcoming[pos - 1].start > s) pos--;
  if (pos >= cap) return;

  int last = (n < cap) ? n : cap - 1;
  for (int i = last; i > pos; i--) out->upcoming[i] = out->upcoming[i - 1];

  UpcomingEvent &item = out->upcoming[pos];
  snprintf(item.title, sizeof(item.title), "%s", ev.summary);
  item.start = s;
  item.end = e;
  item.all_day = ev.all_day;
  item.feed = feed;
  if (n < cap) out->upcoming_count++;
}

// Inserts one entry in a day's list, kept sorted by start time.
static void add_to_day(CalendarData *out, int day, const IcsEvent &ev, time_t sort_key, time_t start, time_t end, bool all_day, uint8_t feed)
{
  out->has_event[day] = true;
  CalendarEvent *list = out->days[day];
  int n = out->day_count[day];
  int pos = n;
  while (pos > 0 && list[pos - 1].start > sort_key) pos--;
  if (pos >= CALENDAR_MAX_EVENTS_PER_DAY) return;

  int last = (n < CALENDAR_MAX_EVENTS_PER_DAY) ? n : CALENDAR_MAX_EVENTS_PER_DAY - 1;
  for (int i = last; i > pos; i--) list[i] = list[i - 1];

  CalendarEvent &item = list[pos];
  snprintf(item.title, sizeof(item.title), "%s", ev.summary);
  item.all_day = all_day;
  item.start = sort_key;
  item.end = end;
  item.feed = feed;
  item.time[0] = '\0';
  if (!all_day)
  {
    struct tm lt;
    localtime_r(&start, &lt);
    strftime(item.time, sizeof(item.time), "%H:%M", &lt);
  }
  if (n < CALENDAR_MAX_EVENTS_PER_DAY) out->day_count[day]++;
}

// Adds an occurrence to every day of the month it touches. Days after the
// first are listed as all-day (the event is already running at midnight).
static void add_to_month(IcsWindow &w, const IcsEvent &ev, time_t s, time_t e)
{
  w.out->month_event_count++;
  time_t cur = max(s, w.month_start);
  for (int guard = 0; guard < 32 && cur < w.month_end && cur < e; guard++)
  {
    struct tm lt;
    localtime_r(&cur, &lt);
    if (lt.tm_mon == w.out->month && lt.tm_mday >= 1 && lt.tm_mday <= CALENDAR_MAX_DAY)
    {
      bool first_day = (cur == s);
      add_to_day(w.out, lt.tm_mday, ev, cur, s, e, ev.all_day || !first_day, w.feed);
    }

    lt.tm_mday += 1;
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_isdst = -1;
    cur = mktime(&lt);
  }
}

// Returns false once occurrences are past the window (stop expanding).
static bool emit_occurrence(const IcsEvent &ev, IcsWindow &w, time_t s)
{
  if (s >= w.to) return false;
  time_t dur = ev.end - ev.start;
  time_t e = s + dur;
  if (e <= w.from && !(dur == 0 && s >= w.from)) return true;

  for (int i = 0; i < ev.exdate_count; i++)
    if (ev.exdates[i] == s) return true;

  if (e > w.now && s < w.now + ICS_LOOKAHEAD_S) add_upcoming(w.out, ev, s, e, w.feed);
  if (s < w.month_end && (e > w.month_start || (dur == 0 && s >= w.month_start))) add_to_month(w, ev, s, e > s ? e : s + 1);

  // Remember the earliest flight not yet over within the next week: still to
  // depart (shown with a countdown) or departed but before its calendar end,
  // so a delayed flight keeps its live status.
  UpcomingEvent &fl = w.out->next_flight;
  if (e > w.now && s < w.now + ICS_LOOKAHEAD_S && (fl.start == 0 || s < fl.start) && df_is_flight_title(ev.summary))
  {
    snprintf(fl.title, sizeof(fl.title), "%s", ev.summary);
    fl.start = s;
    fl.end = e;
    fl.all_day = ev.all_day;
  }
  return true;
}

static time_t nth_weekday_of_month(int y, int mon, int wd, int n, const struct tm &base)
{
  int dim = days_in_month(y, mon);
  struct tm first = {};
  first.tm_year = y - 1900;
  first.tm_mon = mon - 1;
  first.tm_mday = 1;
  first.tm_hour = 12;
  first.tm_isdst = -1;
  mktime(&first);
  int first_wd = (first.tm_wday + 6) % 7; // Monday = 0
  int day;
  if (n > 0)
  {
    day = 1 + (wd - first_wd + 7) % 7 + (n - 1) * 7;
  }
  else
  {
    int last_wd = (first_wd + dim - 1) % 7;
    day = dim - (last_wd - wd + 7) % 7 + (n + 1) * 7;
  }
  if (day < 1 || day > dim) return 0;
  return make_local(y, mon, day, base.tm_hour, base.tm_min, base.tm_sec);
}

static void expand_event(const IcsEvent &ev, IcsWindow &w)
{
  if (ev.cancelled || ev.start == 0) return;

  if (!ev.freq)
  {
    emit_occurrence(ev, w, ev.start);
    return;
  }

  struct tm base;
  localtime_r(&ev.start, &base);
  const int by = base.tm_year + 1900, bm = base.tm_mon + 1, bd = base.tm_mday;
  const int interval = ev.interval > 0 ? ev.interval : 1;
  const time_t dur = ev.end - ev.start;
  int emitted = 0;

  auto accept = [&](time_t t) -> bool { // false = stop
    if (t < ev.start) return true;
    if (ev.until && t > ev.until) return false;
    if (ev.count && emitted >= ev.count) return false;
    emitted++;
    return emit_occurrence(ev, w, t);
  };

  // Without COUNT, jump close to the window instead of walking from the first occurrence.
  long skip_seconds = ev.count ? 0 : (long)(w.from - dur - ev.start);

  if (ev.freq == 'D')
  {
    long k = skip_seconds > 0 ? skip_seconds / (86400L * interval) - 1 : 0;
    for (int step = 0; step < ICS_MAX_STEPS; step++, k++)
    {
      if (k < 0) continue;
      if (!accept(make_local(by, bm, bd + (int)(k * interval), base.tm_hour, base.tm_min, base.tm_sec))) return;
    }
  }
  else if (ev.freq == 'W')
  {
    int base_wd = (base.tm_wday + 6) % 7;
    int days[ICS_MAX_BYDAY];
    int n_days = 0;
    for (int d = 0; d < 7; d++) // weekdays in order Monday..Sunday
    {
      bool use = ev.by_day_count == 0 ? d == base_wd : false;
      for (int i = 0; i < ev.by_day_count; i++)
        if (ev.by_day_wd[i] == d) use = true;
      if (use) days[n_days++] = d;
    }
    long k = skip_seconds > 0 ? skip_seconds / (7L * 86400L * interval) - 1 : 0;
    for (int step = 0; step < ICS_MAX_STEPS; step++, k++)
    {
      if (k < 0) continue;
      for (int i = 0; i < n_days; i++)
      {
        int mday = bd - base_wd + (int)(k * 7 * interval) + days[i];
        if (!accept(make_local(by, bm, mday, base.tm_hour, base.tm_min, base.tm_sec))) return;
      }
    }
  }
  else if (ev.freq == 'M')
  {
    long k = skip_seconds > 0 ? skip_seconds / (31L * 86400L * interval) - 1 : 0;
    for (int step = 0; step < ICS_MAX_STEPS; step++, k++)
    {
      if (k < 0) continue;
      int months = bm - 1 + (int)(k * interval);
      int y = by + months / 12, mon = months % 12 + 1;
      if (ev.by_day_count > 0 && ev.by_day_n[0] != 0)
      {
        time_t t = nth_weekday_of_month(y, mon, ev.by_day_wd[0], ev.by_day_n[0], base);
        if (t && !accept(t)) return;
        if (!t && make_local(y, mon, 1, 0, 0, 0) >= w.to) return;
      }
      else if (bd <= days_in_month(y, mon))
      {
        if (!accept(make_local(y, mon, bd, base.tm_hour, base.tm_min, base.tm_sec))) return;
      }
    }
  }
  else if (ev.freq == 'Y')
  {
    long k = skip_seconds > 0 ? skip_seconds / (366L * 86400L * interval) - 1 : 0;
    for (int step = 0; step < ICS_MAX_STEPS; step++, k++)
    {
      if (k < 0) continue;
      int y = by + (int)(k * interval);
      if (bd > days_in_month(y, bm)) continue; // Feb 29 in non-leap years
      if (!accept(make_local(y, bm, bd, base.tm_hour, base.tm_min, base.tm_sec))) return;
    }
  }
}

// Buffered line reader over the HTTP stream, with RFC 5545 line unfolding.
class IcsLineReader
{
public:
  explicit IcsLineReader(WiFiClient *s) : stream(s) {}

  // Returns false at end of stream.
  bool next(char *out, size_t cap)
  {
    if (!have_pending && !read_physical(pending, sizeof(pending))) return false;
    have_pending = false;
    snprintf(out, cap, "%s", pending);

    // Continuation lines start with a space or tab.
    while (read_physical(pending, sizeof(pending)))
    {
      if (pending[0] == ' ' || pending[0] == '\t')
      {
        size_t len = strlen(out);
        snprintf(out + len, cap - len, "%s", pending + 1);
      }
      else
      {
        have_pending = true;
        break;
      }
    }
    return true;
  }

private:
  WiFiClient *stream;
  uint8_t buf[1024];
  size_t buf_len = 0, buf_pos = 0;
  char pending[320];
  bool have_pending = false;

  int read_byte()
  {
    if (buf_pos < buf_len) return buf[buf_pos++];
    unsigned long start = millis();
    while (millis() - start < 10000)
    {
      int avail = stream->available();
      if (avail > 0)
      {
        buf_len = stream->read(buf, min((size_t)avail, sizeof(buf)));
        buf_pos = 0;
        if (buf_len > 0) return buf[buf_pos++];
      }
      else if (!stream->connected())
      {
        return -1;
      }
      delay(2);
    }
    return -1;
  }

  bool read_physical(char *out, size_t cap)
  {
    size_t n = 0;
    int c;
    bool any = false;
    while ((c = read_byte()) >= 0)
    {
      any = true;
      if (c == '\n') break;
      if (c == '\r') continue;
      if (n + 1 < cap) out[n++] = (char)c;
    }
    out[n] = '\0';
    return any;
  }
};

// Downloads one feed and adds its occurrences to the window.
static bool read_ics_feed(const char *url, IcsWindow &w)
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true); // no chunked encoding, so the raw stream is the file
  http.setTimeout(15000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != 200)
  {
    Serial.printf("[ICS] HTTP %d\n", code); // the URL is secret, so it's not logged
    http.end();
    return false;
  }

  IcsLineReader *reader = new (std::nothrow) IcsLineReader(http.getStreamPtr());
  if (!reader) { http.end(); return false; }

  static char line[640];
  static IcsEvent ev;
  bool in_event = false;
  int events_seen = 0;
  unsigned long t0 = millis();

  while (reader->next(line, sizeof(line)))
  {
    if (!in_event)
    {
      if (!strcmp(line, "BEGIN:VEVENT"))
      {
        memset(&ev, 0, sizeof(ev));
        ev.interval = 1;
        in_event = true;
      }
      continue;
    }

    if (!strcmp(line, "END:VEVENT"))
    {
      in_event = false;
      events_seen++;
      if (!ev.has_end) ev.end = ev.all_day ? ev.start + 86400 : ev.start;
      expand_event(ev, w);
      continue;
    }

    char *colon = strchr(line, ':');
    if (!colon) continue;
    *colon = '\0';
    const char *name = line;
    char *value = colon + 1;

    if (!strncmp(name, "DTSTART", 7))
    {
      ev.start = parse_ics_value(value, &ev.all_day);
    }
    else if (!strncmp(name, "DTEND", 5))
    {
      ev.end = parse_ics_value(value, nullptr);
      ev.has_end = true;
    }
    else if (!strncmp(name, "SUMMARY", 7))
    {
      unescape_text(value);
      snprintf(ev.summary, sizeof(ev.summary), "%s", value);
    }
    else if (!strcmp(name, "RRULE"))
    {
      parse_rrule(value, ev);
    }
    else if (!strncmp(name, "EXDATE", 6))
    {
      char *save = nullptr;
      for (char *v = strtok_r(value, ",", &save); v && ev.exdate_count < ICS_MAX_EXDATES; v = strtok_r(nullptr, ",", &save))
        ev.exdates[ev.exdate_count++] = parse_ics_value(v, nullptr);
    }
    else if (!strcmp(name, "STATUS"))
    {
      ev.cancelled = !strcmp(value, "CANCELLED");
    }
  }
  delete reader;
  http.end();

  Serial.printf("[ICS] %d events parsed in %lu ms\n", events_seen, millis() - t0);
  return true;
}

bool df_fetch_calendar(const char urls[][ICS_URL_MAX], int n, CalendarData *out, int *failed, int year, int month)
{
  if (failed) *failed = 0;
  if (!out) return false;

  struct tm now_tm;
  if (!getLocalTime(&now_tm, 5000))
  {
    Serial.println("[ICS] Time not synced");
    return false;
  }

  memset(out, 0, sizeof(*out));
  out->year = year > 0 ? year : now_tm.tm_year + 1900;
  out->month = year > 0 ? month : now_tm.tm_mon;

  IcsWindow w = {};
  w.now = time(nullptr);
  w.from = make_local(out->year, out->month + 1, 1, 0, 0, 0);
  w.month_end = make_local(out->year, out->month + 2, 1, 0, 0, 0);
  w.month_start = w.from;
  w.to = max(w.now + (time_t)ICS_LOOKAHEAD_S, w.month_end);
  // Browsing a later month: expand from now, so the upcoming list (from now
  // on) is still complete.
  if (w.now < w.from) w.from = w.now;
  w.out = out;

  // All feeds go into the same window, so the lists are merged.
  bool any_ok = false;
  int failures = 0;
  for (int i = 0; i < n; i++)
  {
    if (urls[i][0] == '\0') continue;
    w.feed = i;
    if (read_ics_feed(urls[i], w)) any_ok = true;
    else failures++;
  }
  if (failed) *failed = failures;
  if (!any_ok) return false;

  if (out->next_flight.start) Serial.printf("[ICS] Next flight: %s\n", out->next_flight.title);
  Serial.printf("[ICS] %d this month, %d upcoming\n", out->month_event_count, out->upcoming_count);
  return true;
}

// ----------------------- Package tracking (PKGE) -----------------------

#define TRACKING_KEEP_DELIVERED_S 86400 // delivered parcels stay visible for 1 day

static bool tracking_expired(const TrackingItem &it, time_t now)
{
  return it.status == 5 && now - it.last_date > TRACKING_KEEP_DELIVERED_S;
}

// Sort order: parcels still on their way first, then by most recent checkpoint.
static bool tracking_before(const TrackingItem &a, const TrackingItem &b)
{
  bool a_done = a.status == 5, b_done = b.status == 5;
  if (a_done != b_done) return !a_done;
  return a.last_date > b.last_date;
}

int df_tracking_drop_expired(TrackingItem *items, int count)
{
  time_t now = time(nullptr);
  if (now < 1700000000) return count; // clock not set yet
  int kept = 0;
  for (int i = 0; i < count; i++)
    if (!tracking_expired(items[i], now)) items[kept++] = items[i];
  return kept;
}

bool df_fetch_tracking(const char *api_key, TrackingItem *out, int max, int *count)
{
  if (!api_key || api_key[0] == '\0') return false;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true); // no chunked encoding, so the answer can be parsed as a stream
  http.setTimeout(15000);
  if (!http.begin(client, "https://api.pkge.net/v1/packages/list?per-page=100")) return false;
  http.addHeader("X-Api-Key", api_key);

  int code = http.GET();
  if (code != 200)
  {
    Serial.printf("[Tracking] HTTP %d (401 = wrong API key)\n", code);
    http.end();
    return false;
  }

  // Keep only what the screen shows; checkpoints and addresses are skipped.
  JsonDocument filter;
  JsonObject f = filter["payload"][0].to<JsonObject>();
  f["track_number"] = true;
  f["status"] = true;
  f["last_status_date"] = true;
  f["est_delivery_date_from"] = true;
  f["est_delivery_date_to"] = true;

  PatientStreamReader *reader = new (std::nothrow) PatientStreamReader(http.getStreamPtr());
  if (!reader) { http.end(); return false; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, *reader,
                                             DeserializationOption::Filter(filter),
                                             DeserializationOption::NestingLimit(40));
  delete reader;
  http.end();
  if (err)
  {
    Serial.printf("[Tracking] JSON error %s\n", err.c_str());
    return false;
  }

  JsonArray list = doc["payload"];
  if (list.isNull())
  {
    Serial.println("[Tracking] Unexpected answer (no payload list)");
    return false;
  }

  int n = 0, seen = 0;
  time_t now = time(nullptr);
  for (JsonObject p : list)
  {
    seen++;
    TrackingItem it = {};
    snprintf(it.track_number, sizeof(it.track_number), "%s", p["track_number"] | "");
    it.status = p["status"] | 0;
    it.last_date = parse_iso_utc(p["last_status_date"] | "");
    it.est_delivery = parse_iso_utc(p["est_delivery_date_to"] | (p["est_delivery_date_from"] | ""));
    if (tracking_expired(it, now)) continue;

    // Insert in sorted position, keeping only the first `max`.
    int pos = n;
    while (pos > 0 && tracking_before(it, out[pos - 1])) pos--;
    if (pos >= max) continue;
    int last = n < max ? n : max - 1;
    for (int i = last; i > pos; i--) out[i] = out[i - 1];
    out[pos] = it;
    if (n < max) n++;
  }
  if (count) *count = n;

  Serial.printf("[Tracking] %d packages in the account, showing %d\n", seen, n);
  return true;
}

// ----------------------- Live flight status (AeroDataBox) -----------------------

// Actual/estimated gate time (what airlines and flight apps show): the
// revised time, else the runway (take-off / touchdown) time; 0 when neither is known.
static time_t actual_time(JsonObject movement)
{
  time_t t = parse_iso_utc(movement["revisedTime"]["utc"] | "");
  return t ? t : parse_iso_utc(movement["runwayTime"]["utc"] | "");
}

bool df_fetch_flight(const char *api_key, LiveFlight *f)
{
  if (!api_key || api_key[0] == '\0' || f->number[0] == '\0') return false;

  // The API wants the local departure date (the dashboard's timezone).
  struct tm lt;
  localtime_r(&f->cal_start, &lt);
  char date[11];
  strftime(date, sizeof(date), "%Y-%m-%d", &lt);

  char url[200];
  snprintf(url, sizeof(url),
           "https://aerodatabox.p.rapidapi.com/flights/Number/%s/%s"
           "?dateLocalRole=Departure&withAircraftImage=false&withLocation=false",
           f->number, date);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;
  http.addHeader("X-RapidAPI-Key", api_key);
  http.addHeader("X-RapidAPI-Host", "aerodatabox.p.rapidapi.com");

  int code = http.GET();
  if (code != 200)
  {
    Serial.printf("[Flight] HTTP %d for %s on %s (204/404 = flight not found, 401/403 = key or plan)\n",
                  code, f->number, date);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument filter;
  JsonObject fl = filter[0].to<JsonObject>();
  fl["status"] = true;
  for (const char *dir : {"departure", "arrival"})
  {
    for (const char *t : {"scheduledTime", "revisedTime", "runwayTime"})
    {
      fl[dir][t]["utc"] = true;
      fl[dir][t]["local"] = true;
    }
    fl[dir]["gate"] = true;
    fl[dir]["terminal"] = true;
    fl[dir]["airport"]["municipalityName"] = true;
    fl[dir]["airport"]["location"] = true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err)
  {
    Serial.printf("[Flight] JSON error %s\n", err.c_str());
    return false;
  }

  // A flight number can fly more than once a day: take the leg closest to the calendar time.
  JsonObject best;
  time_t best_diff = 0;
  for (JsonObject leg : doc.as<JsonArray>())
  {
    time_t dep = parse_iso_utc(leg["departure"]["scheduledTime"]["utc"] | "");
    time_t diff = dep > f->cal_start ? dep - f->cal_start : f->cal_start - dep;
    if (best.isNull() || diff < best_diff) { best = leg; best_diff = diff; }
  }
  if (best.isNull())
  {
    Serial.printf("[Flight] No %s flight found on %s\n", f->number, date);
    return false;
  }

  JsonObject dep = best["departure"], arr = best["arrival"];
  snprintf(f->status, sizeof(f->status), "%s", best["status"] | "Unknown");
  f->dep_scheduled = parse_iso_utc(dep["scheduledTime"]["utc"] | "");
  f->dep_actual = actual_time(dep);
  f->arr_scheduled = parse_iso_utc(arr["scheduledTime"]["utc"] | "");
  f->arr_actual = actual_time(arr);
  snprintf(f->dep_gate, sizeof(f->dep_gate), "%s", dep["gate"] | "");
  snprintf(f->dep_terminal, sizeof(f->dep_terminal), "%s", dep["terminal"] | "");

  // Airport-local "2026-10-12 14:05-04:00" -> "2026-10-12 14:05".
  auto local = [](JsonObject movement, char *out, size_t n, bool actual) {
    const char *s = actual ? (movement["revisedTime"]["local"] | (movement["runwayTime"]["local"] | ""))
                           : (const char *)nullptr;
    if (!s || !s[0]) s = movement["scheduledTime"]["local"] | "";
    snprintf(out, n, "%.16s", s);
  };
  local(dep, f->dep_sched_local, sizeof(f->dep_sched_local), false);
  local(dep, f->dep_local, sizeof(f->dep_local), true);
  local(arr, f->arr_local, sizeof(f->arr_local), true);
  snprintf(f->dep_city, sizeof(f->dep_city), "%s", dep["airport"]["municipalityName"] | "");
  snprintf(f->arr_city, sizeof(f->arr_city), "%s", arr["airport"]["municipalityName"] | "");
  JsonObject loc = arr["airport"]["location"];
  f->arr_has_location = !loc.isNull() && !loc["lat"].isNull();
  if (f->arr_has_location)
  {
    f->arr_lat = loc["lat"] | 0.0f;
    f->arr_lon = loc["lon"] | 0.0f;
  }
  f->valid = true;

  Serial.printf("[Flight] %s %s, dep %s (%s) arr %s (%s), gate %s\n", f->number, f->status,
                f->dep_local, f->dep_city, f->arr_local, f->arr_city, f->dep_gate[0] ? f->dep_gate : "-");
  return true;
}

bool df_fetch_current_weather(float lat, float lon, float *temp, int *wcode)
{
  char url[200];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code",
           lat, lon);
  JsonDocument doc;
  if (!http_get_json(url, doc)) return false;
  JsonObject cur = doc["current"];
  if (cur.isNull() || cur["temperature_2m"].isNull()) return false;
  *temp = cur["temperature_2m"] | 0.0f;
  *wcode = cur["weather_code"] | 0;
  return true;
}

// ----------------------- Flights in the calendar -----------------------

static bool is_upper_word_at(const char *p, int len)
{
  for (int i = 0; i < len; i++)
    if (!isupper((unsigned char)p[i])) return false;
  return !isalnum((unsigned char)p[len]);
}

// "TS399", "AC 871": 2-character airline code (letters/digits, not both
// digits), 1-4 digit number. When `out` is given, the number is copied
// there without the space.
static bool find_flight_number(const char *t, char *out, size_t out_size)
{
  for (const char *p = t; *p; p++)
  {
    if (p != t && isalnum((unsigned char)p[-1])) continue; // must start a word
    unsigned char a = p[0], b = a ? p[1] : 0;
    if (!(isupper(a) || isdigit(a)) || !(isupper(b) || isdigit(b))) continue;
    if (isdigit(a) && isdigit(b)) continue;
    const char *q = p + 2;
    if (*q == ' ') q++;
    int digits = 0;
    while (isdigit((unsigned char)q[digits])) digits++;
    if (digits >= 1 && digits <= 4 && !isalnum((unsigned char)q[digits]))
    {
      if (out && out_size > 0) snprintf(out, out_size, "%c%c%.*s", a, b, digits, q);
      return true;
    }
  }
  return false;
}

bool df_extract_flight_number(const char *title, char *out, size_t out_size)
{
  return title && find_flight_number(title, out, out_size);
}

// "PUJ->YUL", "YUL - YYZ", "YUL→YYZ", "YUL to YYZ", "YUL/YYZ": two airport
// codes with a separator. Copies them to from/to (4 bytes each) when given.
static bool find_route(const char *t, char *from, char *to)
{
  for (const char *p = t; *p; p++)
  {
    if (p != t && isalnum((unsigned char)p[-1])) continue;
    if (!is_upper_word_at(p, 3)) continue;
    const char *q = p + 3;
    bool separator = false;
    for (;;)
    {
      if (*q == ' ' || *q == '-' || *q == '>' || *q == '/') { q++; separator = true; }
      else if ((unsigned char)q[0] == 0xE2 && (unsigned char)q[1] == 0x86 && (unsigned char)q[2] == 0x92) { q += 3; separator = true; } // →
      else if (q[0] == 't' && q[1] == 'o' && q[2] == ' ') { q += 3; separator = true; }
      else break;
    }
    if (separator && is_upper_word_at(q, 3))
    {
      if (from) snprintf(from, 4, "%.3s", p);
      if (to) snprintf(to, 4, "%.3s", q);
      return true;
    }
  }
  return false;
}

static bool has_route(const char *t) { return find_route(t, nullptr, nullptr); }

bool df_extract_route(const char *title, char *from, char *to)
{
  return title && find_route(title, from, to);
}

bool df_is_flight_title(const char *t)
{
  if (!t || !*t) return false;
  if (strcasestr(t, "flight") || strstr(t, "\xE2\x9C\x88")) return true; // "flight" or ✈
  return find_flight_number(t, nullptr, 0) && has_route(t);
}

void df_format_countdown(time_t secs, char *buf, size_t n)
{
  if (secs < 3600) snprintf(buf, n, "in %ldm", (long)(secs / 60));
  else if (secs < 86400) snprintf(buf, n, "in %ldh%02ldm", (long)(secs / 3600), (long)((secs % 3600) / 60));
  else snprintf(buf, n, "in %ldd %ldh", (long)(secs / 86400), (long)((secs % 86400) / 3600));
}

static void format_clock(time_t t, char *buf, size_t n)
{
  struct tm lt;
  localtime_r(&t, &lt);
  strftime(buf, n, "%H:%M", &lt);
}

void df_format_live_flight(const LiveFlight &f, char *buf, size_t n)
{
  char arr[8];
  if (strlen(f.arr_local) >= 16) snprintf(arr, sizeof(arr), "%.5s", f.arr_local + 11); // airport-local time
  else format_clock(f.arr_actual ? f.arr_actual : f.arr_scheduled, arr, sizeof(arr));
  long delay_min = (f.dep_actual && f.dep_scheduled) ? (long)(f.dep_actual - f.dep_scheduled) / 60 : 0;

  if (!strcmp(f.status, "Canceled")) { snprintf(buf, n, "CANCELED"); return; }
  if (!strcmp(f.status, "CanceledUncertain")) { snprintf(buf, n, "Possibly canceled"); return; }
  if (!strcmp(f.status, "Diverted")) { snprintf(buf, n, "Diverted"); return; }
  if (!strcmp(f.status, "Arrived")) { snprintf(buf, n, "Landed %s", arr); return; }
  if (!strcmp(f.status, "Departed") || !strcmp(f.status, "EnRoute") || !strcmp(f.status, "Approaching"))
  {
    snprintf(buf, n, "In the air, lands %s", arr);
    return;
  }

  const char *label = "Scheduled";
  if (!strcmp(f.status, "Expected")) label = "On time";
  if (!strcmp(f.status, "CheckIn")) label = "Check-in";
  if (!strcmp(f.status, "Boarding")) label = "Boarding";
  if (!strcmp(f.status, "GateClosed")) label = "Gate closed";
  if (!strcmp(f.status, "Delayed") || delay_min >= 5) label = "Delayed";

  int len = snprintf(buf, n, "%s", label);
  if (delay_min >= 5 && len < (int)n) len += snprintf(buf + len, n - len, " +%ldm", delay_min);
  if (f.dep_gate[0] && len < (int)n) len += snprintf(buf + len, n - len, ", gate %s", f.dep_gate);
  if (f.dep_terminal[0] && len < (int)n)
  {
    bool numeric = true;
    for (const char *c = f.dep_terminal; *c; c++)
      if (!isdigit((unsigned char)*c)) numeric = false;
    snprintf(buf + len, n - len, numeric ? ", T%s" : ", Terminal %s", f.dep_terminal);
  }
}

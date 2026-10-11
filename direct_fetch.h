#ifndef __DIRECT_FETCH_H__
#define __DIRECT_FETCH_H__

// Widget data fetched directly by the ESP32 from each service - no Google
// Apps Script or other server in between. Ported from the e-ink dashboard
// (ESP32-eInk-Dashboard's directFetch.cpp).
//
// A plain .cpp (not another .ino tab) so its internal structs don't trip
// Arduino's hoisted auto-prototypes. Every function only fills the output
// it's given and never touches LVGL: they all run on network_task, and
// web_fetch.ino copies the results into the screens' data and flags them
// for loop() to apply.

#include <Arduino.h>
#include "calendar_types.h"

#define ICS_MAX_FEEDS 3
#define ICS_URL_MAX 257
#define TRACKING_MAX 5

struct DfQuote
{
  char symbol[12];
  float price;
  float prev_close;
};

struct DfMakerWorld
{
  long likes;
  long downloads;
  long prints;
  long collections;
  long boosts;
};

// One parcel from the PKGE account.
struct TrackingItem
{
  char track_number[40];
  int status;          // PKGE status code: 3 in transit, 4 at pickup point, 5 delivered, ...
  time_t last_date;    // date of the latest checkpoint
  time_t est_delivery; // estimated delivery date, 0 if unknown
};

// "City", "City, CC" or "lat,lon" -> coordinates. A city is looked up once
// with Open-Meteo's geocoding and cached for later calls.
bool df_resolve_location(const char *location, float *lat, float *lon);

// Yahoo Finance quotes for comma separated symbols (indexes start with ^).
// Returns true if at least one quote was read.
bool df_fetch_stocks(const char *symbols, DfQuote *out, int max, int *count, int *failed);

// Sum of likes/downloads/prints/collections/boosts over every published
// design of a MakerWorld user. Streams the ~1 MB answer through a filter.
bool df_fetch_makerworld(const char *uid, DfMakerWorld *out);

// Up to ICS_MAX_FEEDS iCal (.ics) feeds merged into one month. Times are
// interpreted in the current TZ (set by configTzTime()). `out` is a staging
// buffer: returns true if at least one feed was read, false if every feed
// failed or the clock isn't set yet (then `out` holds nothing useful).
bool df_fetch_calendar(const char urls[][ICS_URL_MAX], int n, CalendarData *out, int *failed);

// Parcels in the PKGE account: active ones and those delivered in the last
// day, still-moving first, newest update first.
bool df_fetch_tracking(const char *api_key, TrackingItem *out, int max, int *count);
// Removes parcels delivered more than a day ago. Returns the new count.
int df_tracking_drop_expired(TrackingItem *items, int count);

// Schedule and live status of `f->number` (departing on the local date of
// f->cal_start) from AeroDataBox via RapidAPI: times (UTC and airport-local),
// gate, terminal, cities and the arrival airport's location. Sets f->valid
// on success.
bool df_fetch_flight(const char *api_key, LiveFlight *f);

// Current temperature and WMO weather code at a location (Open-Meteo).
bool df_fetch_current_weather(float lat, float lon, float *temp, int *wcode);

// True for calendar titles that look like a flight: "TS399 PUJ->YUL",
// "AC 871 YUL to YYZ", "Flight to Toronto (AC 871)".
bool df_is_flight_title(const char *title);
// Copies the flight number ("TS399", spaces removed) from a title.
bool df_extract_flight_number(const char *title, char *out, size_t out_size);

// Copies the airport codes of a route ("YUL->YYZ" -> "YUL", "YYZ"); each
// buffer needs 4 bytes. False if the title has no route.
bool df_extract_route(const char *title, char *from, char *to);

// "in 45m", "in 3h05m", "in 2d 4h".
void df_format_countdown(time_t secs, char *buf, size_t n);
// "Delayed +45m, gate B7, T1", "In the air, lands 19:15", "Landed 19:12", ...
void df_format_live_flight(const LiveFlight &f, char *buf, size_t n);

#endif // __DIRECT_FETCH_H__

#ifndef __CALENDAR_TYPES_H__
#define __CALENDAR_TYPES_H__

#include <time.h>
#include <stdint.h>

// These types live in a header (rather than in calendar.ino, where they're
// used) because Arduino's auto-generated function prototypes are hoisted
// above every .ino tab's content - if a struct is only defined later in a
// tab, a hoisted prototype that mentions it references it before the
// compiler has seen the definition ("'CalendarEvent' does not name a type").
// A type pulled in via #include is visible at that hoisted point since
// #include is resolved by the compiler itself, not by Arduino's tab
// concatenation. direct_fetch.cpp fills them, so they're shared with it too.

#define CALENDAR_MAX_DAY 31
#define CALENDAR_MAX_EVENTS_PER_DAY 6
#define CALENDAR_UPCOMING_MAX 8
#define CALENDAR_TITLE_MAX 64

// One entry on the Calendar day-detail screen.
struct CalendarEvent
{
  char title[CALENDAR_TITLE_MAX];
  char time[6]; // "14:30", empty for all-day events
  bool all_day;
  time_t start; // keeps each day's list sorted
};

// One occurrence in the next few days, for the Home screen's next-event strip.
struct UpcomingEvent
{
  char title[CALENDAR_TITLE_MAX];
  time_t start;
  time_t end;
  bool all_day;
};

// Everything the iCal feeds produce for one month. Built by
// df_fetch_calendar() and copied into calendar.ino's copy in one go.
struct CalendarData
{
  int year;  // e.g. 2026
  int month; // 0-11 (tm_mon), the month days[] refers to
  bool has_event[CALENDAR_MAX_DAY + 1];
  CalendarEvent days[CALENDAR_MAX_DAY + 1][CALENDAR_MAX_EVENTS_PER_DAY];
  uint8_t day_count[CALENDAR_MAX_DAY + 1];
  int month_event_count; // occurrences touching the month
  UpcomingEvent upcoming[CALENDAR_UPCOMING_MAX]; // not yet over, soonest first
  int upcoming_count;
  UpcomingEvent next_flight; // earliest flight not yet over within 7 days; start == 0: none
};

// Live status of the next flight from the calendar (AeroDataBox).
struct LiveFlight
{
  char number[12];   // "TS399", taken from the calendar title
  time_t cal_start;  // departure time in the calendar (identifies the flight)
  time_t cal_end;    // end of the calendar event (expected landing if no live data)
  time_t last_fetch;
  uint8_t requests;  // AeroDataBox requests made for this flight (capped)
  bool valid;        // live data received
  char status[20];   // Expected, CheckIn, Boarding, Departed, Delayed, Arrived, ...
  time_t dep_scheduled, dep_actual; // UTC; "actual" = revised/runway time when known, else 0
  time_t arr_scheduled, arr_actual;
  char dep_gate[8];
  char dep_terminal[8];
  // Airport-local times as AeroDataBox gives them, "2026-10-12 14:05" (empty if unknown):
  char dep_sched_local[17];
  char dep_local[17]; // actual/estimated departure, else scheduled
  char arr_local[17]; // actual/estimated arrival, else scheduled
  char dep_city[24];
  char arr_city[24];
  bool arr_has_location;
  float arr_lat, arr_lon;
  // Current weather at the destination (Open-Meteo), refreshed hourly.
  bool dest_valid;
  float dest_temp;
  int dest_wcode;
  time_t dest_fetch;
};

#endif // __CALENDAR_TYPES_H__

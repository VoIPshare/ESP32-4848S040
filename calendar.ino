// ==================== Screen 4: Calendar ====================
// Split out of ESP32-4848S040.ino. Renders the real current month (via the
// same NTP sync that drives the Home screen clock - see clock_year/mon/mday
// in ESP32-4848S040.ino). Today is a filled circle; days with events get up
// to three small dots, coloured by which iCal feed they came from; past days
// are dimmed. Arrows next to the title browse other months (fetched on
// demand - see fetch_calendar_view()), and "Up next" lists the next events.
//
// The events come from up to 3 iCal (.ics) feeds the ESP32 downloads and
// parses itself (recurrence included) - see df_fetch_calendar() in
// direct_fetch.cpp and web_fetch.ino, which copies the result into
// `calendar` below under data_lock(). This file only renders it.
//
// Tapping a day opens a separate detail screen (cal_day_screen, not part of
// screens[]/the swipeable set - same "own screen outside the main array"
// pattern as Settings) listing that day's events, with a top-left back
// button. Day-of-month is tracked per grid cell in cal_cell_day[] (kept in
// sync by calendar_apply_to_ui() every month change) rather than baked into
// each cell's click-event user_data at creation time, so a single set of
// event callbacks registered once at screen-build time stays correct
// across month changes without re-registering anything.
//
// This is an Arduino multi-tab sketch: all .ino files in this folder are
// concatenated into one translation unit at build time, so no #include of
// this file is needed - create_calendar_screen() is called directly from
// setup() in ESP32-4848S040.ino, and calendar_apply_to_ui() is called from
// loop() there whenever calendar_needs_ui_refresh is set (by either a fresh
// NTP sync or a fresh calendar fetch).

#define CALENDAR_GRID_CELLS 42 // 6 rows x 7 cols - enough for any month's layout

#define CAL_ROW_H 38
#define CAL_UP_ROWS 3
#define CAL_BROWSE_MONTHS 12 // how far the arrows go, each way

static lv_obj_t *cal_title_lab = NULL;
static lv_obj_t *cal_status_lab = NULL; // right of "UP NEXT": loading, no events, ...
static lv_obj_t *cal_arrow[2] = {NULL, NULL};
static lv_obj_t *cal_cell_today[CALENDAR_GRID_CELLS] = {NULL};
static lv_obj_t *cal_cell_label[CALENDAR_GRID_CELLS] = {NULL};
static lv_obj_t *cal_cell_dot[CALENDAR_GRID_CELLS][3];
static int cal_cell_day[CALENDAR_GRID_CELLS]; // day-of-month for each cell, -1 if blank
static lv_obj_t *cal_up_row[CAL_UP_ROWS], *cal_up_bar[CAL_UP_ROWS], *cal_up_when[CAL_UP_ROWS], *cal_up_title[CAL_UP_ROWS],
    *cal_up_rel[CAL_UP_ROWS];
static time_t cal_up_start[CAL_UP_ROWS]; // tap: that day

// Latest calendar data (per-day lists for the day-detail screen, upcoming
// events and the next flight for the Home strip). ~13 KB, so it lives in
// PSRAM - see calendar_data(). CalendarData itself is defined in
// calendar_types.h, not here - see that header's comment for why.
static CalendarData *calendar = NULL;
static bool calendar_have_data = false; // true once a fetch has completed at least once
static bool calendar_fetch_failed = false;

// Live status of calendar->next_flight (AeroDataBox) - see web_fetch.ino.
static LiveFlight live_flight = {};

// Another month browsed with the arrows: cal_view_offset months from this
// one. Its data is fetched on request into calendar_view (PSRAM) by
// fetch_calendar_view() in web_fetch.ino.
static int cal_view_offset = 0;
static CalendarData *calendar_view = NULL;
static bool cal_view_loaded = false, cal_view_failed = false;
static volatile bool cal_view_fetch_req = false;
static int cal_view_req_year = 0, cal_view_req_month = 0;

// One colour per iCal feed.
static lv_color_t cal_feed_color(uint8_t feed)
{
  static const uint32_t C[3] = {0x00BCD4, 0xB388FF, 0x66BB6A};
  return lv_color_hex(C[feed % 3]);
}

// The month shown: year and month 0-11.
static void cal_view_ym(int *y, int *m)
{
  int idx = clock_year * 12 + clock_mon + cal_view_offset;
  *y = idx / 12;
  *m = idx % 12;
}

// PSRAM if available (internal RAM is what Wi-Fi/TLS run short of), zeroed.
void *psram_calloc(size_t size)
{
  void *p = heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : calloc(1, size);
}

CalendarData *calendar_data()
{
  if (!calendar) calendar = (CalendarData *)psram_calloc(sizeof(CalendarData));
  return calendar;
}

bool cal_is_leap_year(int y)
{
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int cal_days_in_month(int y, int m /* 1-12 */)
{
  static const int dim[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && cal_is_leap_year(y)) return 29;
  return dim[m - 1];
}

// Sakamoto's algorithm. Returns 0=Sunday..6=Saturday.
int cal_day_of_week_sun0(int y, int m, int d)
{
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

// Converts to Mon=0..Sun=6, matching the grid's day_letters/start_col convention.
int cal_day_of_week_mon0(int y, int m, int d)
{
  int w = cal_day_of_week_sun0(y, m, d);
  return (w + 6) % 7;
}

bool calendar_has_feeds()
{
  for (int i = 0; i < ICS_MAX_FEEDS; i++)
    if (settings.ics_urls[i][0]) return true;
  return false;
}

// True when the fetched per-day data is for the month on screen - right
// after midnight on the 1st it still describes last month until the next
// fetch (web_fetch.ino refetches as soon as it notices the month changed).
bool calendar_data_is_current()
{
  return calendar && calendar_have_data && calendar->year == clock_year && calendar->month == clock_mon;
}

// The per-day data for year/month (0-11) if it's loaded, else NULL.
// Call with data_lock() held.
static CalendarData *cal_data_for(int y, int m)
{
  if (calendar_data_is_current() && calendar->year == y && calendar->month == m) return calendar;
  if (calendar_view && cal_view_loaded && calendar_view->year == y && calendar_view->month == m) return calendar_view;
  return NULL;
}

// Asks network_task for year/month (0-11) unless it's loaded or on its way.
// Call with data_lock() held.
static void cal_request_month(int y, int m)
{
  if (cal_data_for(y, m)) return;
  if (cal_view_fetch_req && cal_view_req_year == y && cal_view_req_month == m) return;
  if (cal_view_failed && cal_view_req_year == y && cal_view_req_month == m) return; // shown as failed; arrows retry
  cal_view_req_year = y;
  cal_view_req_month = m;
  cal_view_failed = false;
  cal_view_fetch_req = true; // network_task: fetch_calendar_view()
}

// Back to this month (Home after idle, and setup()).
void calendar_view_reset()
{
  if (cal_view_offset == 0) return;
  cal_view_offset = 0;
  calendar_needs_ui_refresh = true;
}

// Whole days from now's date to t's date (0 today, 1 tomorrow, -1 yesterday).
static int cal_day_diff(time_t t, time_t now)
{
  struct tm a, b;
  localtime_r(&now, &a);
  localtime_r(&t, &b);
  a.tm_hour = b.tm_hour = 12;
  a.tm_min = b.tm_min = a.tm_sec = b.tm_sec = 0;
  a.tm_isdst = b.tm_isdst = -1;
  return (int)lround(difftime(mktime(&b), mktime(&a)) / 86400.0);
}

// "in 40 min" / "in 3 h" / "in 3 h 20 min" (exact) / "Tomorrow" / "in 3 days".
static void cal_relative(time_t t, time_t now, bool exact, char *buf, size_t n)
{
  int dd = cal_day_diff(t, now);
  long mins = (long)((t - now + 59) / 60);
  if (dd == 0 || mins < 60)
  {
    if (mins < 60) snprintf(buf, n, tr(S_IN_MIN), (int)(mins > 0 ? mins : 0));
    else if (exact && mins % 60) snprintf(buf, n, tr(S_IN_H_MIN), (int)(mins / 60), (int)(mins % 60));
    else snprintf(buf, n, tr(S_IN_H), (int)((mins + 30) / 60));
  }
  else if (dd == 1) snprintf(buf, n, "%s", tr(S_TOMORROW));
  else snprintf(buf, n, tr(S_IN_DAYS), dd);
}

// First three letters of a weekday ("Fri", "Ven"); wday 0 = Sunday.
static void cal_wday_short(int wday, char *buf, size_t n)
{
  snprintf(buf, n, "%.3s", WEEKDAY_NAMES[wday % 7]);
}

// Refreshes the title, grid and Up next from clock_year/clock_mon/clock_mday
// (set by sync_clock_ntp()), the browsed month and the fetched calendar data.
// Called at screen creation and whenever calendar_needs_ui_refresh is set.
void calendar_apply_to_ui()
{
  int vy, vm;
  cal_view_ym(&vy, &vm);
  int tdim = cal_days_in_month(vy, vm + 1);
  int start_col = cal_day_of_week_mon0(vy, vm + 1, 1);
  bool this_month = cal_view_offset == 0;

  data_lock();
  CalendarData *d = cal_data_for(vy, vm);
  if (!this_month && !d && calendar_has_feeds()) cal_request_month(vy, vm); // e.g. another month was loaded meanwhile

  if (cal_title_lab) lv_label_set_text_fmt(cal_title_lab, "%s %d", MONTH_NAMES[vm % 12], vy);
  if (cal_arrow[0]) (cal_view_offset > -CAL_BROWSE_MONTHS ? lv_obj_clear_flag : lv_obj_add_flag)(cal_arrow[0], LV_OBJ_FLAG_HIDDEN);
  if (cal_arrow[1]) (cal_view_offset < CAL_BROWSE_MONTHS ? lv_obj_clear_flag : lv_obj_add_flag)(cal_arrow[1], LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < CALENDAR_GRID_CELLS && cal_cell_label[i]; i++)
  {
    int day = i - start_col + 1;
    bool valid = (day >= 1 && day <= tdim);
    cal_cell_day[i] = valid ? day : -1;
    (valid ? lv_obj_clear_flag : lv_obj_add_flag)(cal_cell_label[i], LV_OBJ_FLAG_HIDDEN);
    bool is_today = valid && this_month && day == clock_mday;
    (is_today ? lv_obj_clear_flag : lv_obj_add_flag)(cal_cell_today[i], LV_OBJ_FLAG_HIDDEN);
    int dots = valid && d ? min((int)d->day_count[day], 3) : 0;
    for (int k = 0; k < 3; k++) (k < dots ? lv_obj_clear_flag : lv_obj_add_flag)(cal_cell_dot[i][k], LV_OBJ_FLAG_HIDDEN);
    if (!valid) continue;

    bool past = cal_view_offset < 0 || (this_month && day < clock_mday);
    bool weekend = i % 7 >= 5;
    lv_label_set_text_fmt(cal_cell_label[i], "%d", day);
    uint32_t col = is_today ? 0x062A30 : past ? (weekend ? 0x3A444D : 0x4A555F) : weekend ? 0x9AA4AD : 0xE0E0E0;
    lv_obj_set_style_text_color(cal_cell_label[i], lv_color_hex(col), LV_PART_MAIN);

    // Dots centered under the number, coloured by feed, dimmed in the past.
    lv_coord_t cx = 16 + (i % 7) * 64 + 32, cy = 80 + (i / 7) * CAL_ROW_H + CAL_ROW_H / 2;
    for (int k = 0; k < dots; k++)
    {
      lv_obj_set_pos(cal_cell_dot[i][k], cx - (dots * 8 - 3) / 2 + k * 8, cy + 12);
      lv_color_t c = cal_feed_color(d->days[day][k].feed);
      lv_obj_set_style_bg_color(cal_cell_dot[i][k], past ? lv_color_mix(c, COLOR_BG, 90) : c, LV_PART_MAIN);
    }
  }

  // Status, right of "UP NEXT".
  const char *status = "";
  if (!calendar_has_feeds()) status = "";
  else if (!this_month && !d) status = cal_view_failed ? tr(S_LOAD_FAILED) : tr(S_LOADING);
  else if (this_month && !calendar_have_data) status = calendar_fetch_failed ? tr(S_CAL_FAILED) : tr(S_CAL_SYNC);
  else if (d && d->month_event_count == 0) status = tr(S_NO_EVENTS_MONTH);
  if (cal_status_lab) lv_label_set_text(cal_status_lab, status);

  // Up next: the next events from now on, whatever month is shown.
  time_t now = time(nullptr);
  int shown = 0;
  if (calendar && calendar_have_data && now > 1700000000)
  {
    for (int i = 0; i < calendar->upcoming_count && shown < CAL_UP_ROWS; i++)
    {
      UpcomingEvent &ev = calendar->upcoming[i];
      if (ev.end <= now) continue;
      struct tm s;
      localtime_r(&ev.start, &s);
      int dd = cal_day_diff(ev.start, now);
      char when[40], wd[8];
      if (dd <= 0) snprintf(wd, sizeof(wd), "%s", tr(S_TODAY));
      else cal_wday_short(s.tm_wday, wd, sizeof(wd));
      if (ev.all_day) snprintf(when, sizeof(when), "%s \xC2\xB7 %s", wd, tr(S_ALL_DAY));
      else snprintf(when, sizeof(when), "%s %02d:%02d", wd, s.tm_hour, s.tm_min);
      char rel[32];
      if (ev.start <= now) snprintf(rel, sizeof(rel), "%s", tr(S_NOW));
      else cal_relative(ev.start, now, false, rel, sizeof(rel));
      lv_obj_clear_flag(cal_up_row[shown], LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(cal_up_bar[shown], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_color(cal_up_bar[shown], cal_feed_color(ev.feed), LV_PART_MAIN);
      lv_label_set_text(cal_up_when[shown], when);
      lv_label_set_text(cal_up_title[shown], ev.title[0] ? ev.title : tr(S_UNTITLED));
      lv_obj_set_style_text_color(cal_up_title[shown], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
      lv_label_set_text(cal_up_rel[shown], rel);
      cal_up_start[shown] = ev.start;
      shown++;
    }
  }
  if (shown == 0 && cal_up_row[0])
  {
    // One muted line instead: nothing coming, or no calendar set up.
    lv_obj_clear_flag(cal_up_row[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cal_up_bar[0], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(cal_up_when[0], "");
    lv_label_set_text(cal_up_title[0], calendar_has_feeds() ? tr(S_NOTHING) : tr(S_NO_CAL));
    lv_obj_set_style_text_color(cal_up_title[0], COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_label_set_text(cal_up_rel[0], "");
    cal_up_start[0] = 0;
    shown = 1;
  }
  for (int i = shown; i < CAL_UP_ROWS; i++)
    if (cal_up_row[i]) lv_obj_add_flag(cal_up_row[i], LV_OBJ_FLAG_HIDDEN);

  update_home_next_event();
  data_unlock();
  flight_refresh_screen();
  // The day view is open on a month that just arrived: fill it in.
  if (in_subscreen && lv_scr_act() == cal_day_screen) cal_day_fill();
}

// The flight line pinned on the Home strip: the next flight from 24 hours
// before departure, and with live data (AeroDataBox) until shortly after it
// lands. Returns false when no flight should be pinned right now.
bool format_home_flight(time_t now, char *buf, size_t n)
{
  const UpcomingEvent &fl = calendar->next_flight;
  if (!fl.start) return false;

  bool live = live_flight.valid && live_flight.cal_start == fl.start;
  time_t dep = fl.start;
  if (live) dep = live_flight.dep_actual ? live_flight.dep_actual : (live_flight.dep_scheduled ? live_flight.dep_scheduled : fl.start);

  if (now < dep)
  {
    if (dep - now > 86400) return false;
    char countdown[24];
    df_format_countdown(dep - now, countdown, sizeof(countdown));
    int len = snprintf(buf, n, "%s - %s", fl.title, countdown);
    if (live && len > 0 && len < (int)n)
    {
      char status[64];
      df_format_live_flight(live_flight, status, sizeof(status));
      snprintf(buf + len, n - len, " - %s", status);
    }
    return true;
  }

  // Departed: only worth pinning while live data says something useful.
  if (!live) return false;
  time_t landed = live_flight.arr_actual;
  if (!strcmp(live_flight.status, "Arrived") && landed && now > landed + 3600) return false;
  char status[64];
  df_format_live_flight(live_flight, status, sizeof(status));
  snprintf(buf, n, "%s - %s", fl.title, status);
  return true;
}

// Refreshes the Home screen's "Next: ..." strip. Called from
// calendar_apply_to_ui() above and once a minute from clock_tick_cb(), so
// passed events and flight countdowns move on without a refetch.
void update_home_next_event()
{
  if (!home_next_event_lab) return;

  data_lock();
  time_t now = time(nullptr);
  char line[200];
  bool flight = calendar && calendar_have_data && now > 1700000000 && format_home_flight(now, line, sizeof(line));

  if (!flight)
  {
    // First event not started yet (an all-day event that already began
    // today would otherwise hide the rest of the day).
    const UpcomingEvent *ev = NULL;
    if (calendar && calendar_have_data)
      for (int i = 0; i < calendar->upcoming_count && !ev; i++)
        if (calendar->upcoming[i].start > now) ev = &calendar->upcoming[i];

    if (!calendar_has_feeds())
      snprintf(line, sizeof(line), "%s", tr(S_NO_CAL));
    else if (!ev)
      snprintf(line, sizeof(line), "%s", calendar_have_data ? tr(S_NOTHING) : tr(S_CAL_SYNC));
    else
    {
      struct tm st, nt;
      localtime_r(&ev->start, &st);
      localtime_r(&now, &nt);
      // Whole days between the two local dates (rounded, so a DST change in
      // between doesn't shift it).
      struct tm a = nt, b = st;
      a.tm_hour = a.tm_min = a.tm_sec = 0;
      b.tm_hour = b.tm_min = b.tm_sec = 0;
      a.tm_isdst = b.tm_isdst = -1;
      int day_diff = (int)((mktime(&b) - mktime(&a) + 43200) / 86400);

      char when[24];
      if (day_diff == 0) snprintf(when, sizeof(when), "%s", tr(S_TODAY));
      else if (day_diff == 1) snprintf(when, sizeof(when), "%s", tr(S_TOMORROW));
      else snprintf(when, sizeof(when), "%s", WEEKDAY_NAMES[st.tm_wday % 7]);
      if (!ev->all_day)
      {
        size_t len = strlen(when);
        strftime(when + len, sizeof(when) - len, " %H:%M", &st);
      }

      int len = snprintf(line, sizeof(line), "%s - %s", when, ev->title);
      if (df_is_flight_title(ev->title) && len > 0 && len < (int)sizeof(line))
      {
        char countdown[24];
        df_format_countdown(ev->start - now, countdown, sizeof(countdown));
        snprintf(line + len, sizeof(line) - len, " - %s", countdown);
      }
    }
  }

  lv_label_set_text(home_next_event_lab, line);
  if (home_next_accent) lv_obj_set_style_bg_color(home_next_accent, flight ? COLOR_SECONDARY : COLOR_PRIMARY, LV_PART_MAIN);
  data_unlock();
}

// ==================== Day detail screen ====================
// Not part of screens[]/the swipeable set - reached only by tapping a day
// on the Calendar screen, exited only via its own back button. Same
// "separate screen outside the main array" shape as Settings (see
// open_settings_cb()/close_settings_screen() in ESP32-4848S040.ino).
// cal_day_screen itself is declared in ESP32-4848S040.ino alongside
// settings_screen, not here - setup() (which assigns to it) lives in that
// file, concatenated before this one, and a plain variable (unlike a
// function) has to already be declared at its first use in the
// translation unit.
static lv_obj_t *cal_day_title_lab = NULL;
static lv_obj_t *cal_day_list = NULL;
static lv_obj_t *cal_day_btn[2] = {NULL, NULL}, *cal_day_btn_lab[2] = {NULL, NULL}; // previous / next day
static int cal_day_y = 0, cal_day_m = 0, cal_day_d = 0; // the day shown (month 0-11)

static void cal_label(lv_obj_t *parent, lv_obj_t **out, const lv_font_t *font, lv_color_t color, lv_coord_t x, lv_coord_t y,
                      const char *text, lv_coord_t width = 0)
{
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
  if (width)
  {
    lv_obj_set_width(l, width);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  }
  lv_obj_set_pos(l, x, y);
  if (out) *out = l;
}

// A plain box that lets taps and drags through (to the list or the screen).
static lv_obj_t *cal_box(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, lv_color_t bg, lv_coord_t radius)
{
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, bg, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

static void cal_day_event_row(const CalendarEvent &ev, bool today, time_t now)
{
  bool done = today && ev.end <= now;
  bool running = today && ev.start <= now && ev.end > now;
  lv_obj_t *row = cal_box(cal_day_list, 440, 56, lv_color_hex(0x1E1E1E), 12);
  if (running)
  {
    lv_obj_set_style_border_width(row, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(row, COLOR_PRIMARY, LV_PART_MAIN);
  }
  lv_obj_t *bar = cal_box(row, 5, 56, done ? lv_color_mix(cal_feed_color(ev.feed), COLOR_BG, 90) : cal_feed_color(ev.feed), 0);
  lv_obj_set_pos(bar, 0, 0);

  char end[16] = "";
  struct tm s, e;
  localtime_r(&ev.start, &s);
  localtime_r(&ev.end, &e);
  if (ev.end > ev.start)
  {
    if (e.tm_yday == s.tm_yday) snprintf(end, sizeof(end), "%02d:%02d", e.tm_hour, e.tm_min);
    else cal_wday_short(e.tm_wday, end, sizeof(end));
  }
  lv_color_t muted = COLOR_TEXT_MUTED, dim = lv_color_hex(0x4A555F);
  cal_label(row, NULL, &lv_font_ui_16, done ? dim : COLOR_PRIMARY, 16, 8, ev.time);
  cal_label(row, NULL, &lv_font_ui_12, done ? dim : muted, 16, 32, end);
  cal_label(row, NULL, &lv_font_ui_16, done ? dim : lv_color_hex(0xFFFFFF), 108, 8, ev.title[0] ? ev.title : tr(S_UNTITLED), 316);

  char sub[48] = "";
  if (done) snprintf(sub, sizeof(sub), "%s", tr(S_DONE));
  else if (running)
  {
    char left[24];
    snprintf(left, sizeof(left), tr(S_MIN_LEFT), (int)((ev.end - now + 59) / 60));
    snprintf(sub, sizeof(sub), "%s \xC2\xB7 %s", tr(S_NOW), left);
  }
  else if (today) cal_relative(ev.start, now, true, sub, sizeof(sub));
  else if (ev.end > ev.start)
  {
    long mins = (long)((ev.end - ev.start) / 60);
    if (mins >= 60 && mins % 60) snprintf(sub, sizeof(sub), "%ld h %ld min", mins / 60, mins % 60);
    else if (mins >= 60) snprintf(sub, sizeof(sub), "%ld h", mins / 60);
    else snprintf(sub, sizeof(sub), "%ld min", mins);
  }
  cal_label(row, NULL, &lv_font_ui_12, done ? dim : muted, 108, 32, sub, 316);
}

static void cal_day_now_line(time_t now)
{
  lv_obj_t *line = cal_box(cal_day_list, 440, 16, COLOR_BG, 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_TRANSP, LV_PART_MAIN);
  struct tm t;
  localtime_r(&now, &t);
  char txt[24];
  snprintf(txt, sizeof(txt), "%s %02d:%02d", tr(S_NOW), t.tm_hour, t.tm_min);
  lv_obj_t *lab;
  cal_label(line, &lab, &lv_font_ui_12, COLOR_SECONDARY, 0, 0, txt);
  lv_obj_update_layout(lab);
  lv_coord_t x = lv_obj_get_width(lab) + 8;
  lv_obj_t *rule = cal_box(line, 440 - x, 2, COLOR_SECONDARY, 1);
  lv_obj_set_pos(rule, x, 7);
}

// Fills the day view for cal_day_y/m/d from its month's data.
static void cal_day_fill()
{
  lv_obj_clean(cal_day_list);
  int dim = cal_days_in_month(cal_day_y, cal_day_m + 1);
  int wday = cal_day_of_week_sun0(cal_day_y, cal_day_m + 1, cal_day_d);
  char t[48];
  format_long_date(t, sizeof(t), wday, cal_day_m, cal_day_d);
  lv_label_set_text(cal_day_title_lab, t);

  data_lock();
  CalendarData *d = cal_data_for(cal_day_y, cal_day_m);
  if (!d && calendar_has_feeds() && !(cal_day_y == clock_year && cal_day_m == clock_mon)) cal_request_month(cal_day_y, cal_day_m);
  time_t now = time(nullptr);
  bool today = cal_day_y == clock_year && cal_day_m == clock_mon && cal_day_d == clock_mday && now > 1700000000;
  int n = d ? min((int)d->day_count[cal_day_d], CALENDAR_MAX_EVENTS_PER_DAY) : 0;

  // All-day events as pills on top.
  lv_obj_t *chips = NULL;
  for (int i = 0; i < n; i++)
  {
    CalendarEvent &ev = d->days[cal_day_d][i];
    if (!ev.all_day) continue;
    if (!chips)
    {
      chips = cal_box(cal_day_list, 440, LV_SIZE_CONTENT, COLOR_BG, 0);
      lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, LV_PART_MAIN);
      lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
      lv_obj_set_style_pad_column(chips, 8, LV_PART_MAIN);
      lv_obj_set_style_pad_row(chips, 6, LV_PART_MAIN);
    }
    lv_obj_t *chip = cal_box(chips, LV_SIZE_CONTENT, LV_SIZE_CONTENT, lv_color_mix(cal_feed_color(ev.feed), COLOR_BG, 60), 8);
    lv_obj_set_style_pad_hor(chip, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(chip, 5, LV_PART_MAIN);
    lv_obj_set_style_border_side(chip, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 4, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip, cal_feed_color(ev.feed), LV_PART_MAIN);
    lv_obj_t *lab = lv_label_create(chip);
    lv_label_set_text(lab, ev.title[0] ? ev.title : tr(S_UNTITLED));
    lv_obj_set_style_text_font(lab, &lv_font_ui_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  }

  // Timed events, with today's "now" line before the first one not over.
  bool now_drawn = !today, any_timed = false;
  for (int i = 0; i < n; i++)
  {
    CalendarEvent &ev = d->days[cal_day_d][i];
    if (ev.all_day) continue;
    any_timed = true;
    if (!now_drawn && ev.end > now)
    {
      cal_day_now_line(now);
      now_drawn = true;
    }
    cal_day_event_row(ev, today, now);
  }
  if (!now_drawn && any_timed) cal_day_now_line(now);
  if (n == 0)
  {
    lv_obj_t *lab;
    cal_label(cal_day_list, &lab, &lv_font_ui_16, COLOR_TEXT_MUTED, 0, 0, d ? tr(S_NO_EVENTS) : tr(S_LOADING));
    lv_obj_set_style_pad_top(lab, 120, LV_PART_MAIN);
    lv_obj_set_width(lab, 440);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  }
  data_unlock();
  lv_obj_scroll_to_y(cal_day_list, 0, LV_ANIM_OFF);

  // Previous / next day buttons, within the month's data.
  for (int k = 0; k < 2; k++)
  {
    int nd = cal_day_d + (k ? 1 : -1);
    bool ok = nd >= 1 && nd <= dim;
    (ok ? lv_obj_clear_flag : lv_obj_add_flag)(cal_day_btn[k], LV_OBJ_FLAG_HIDDEN);
    if (!ok) continue;
    char wd[8];
    cal_wday_short(cal_day_of_week_sun0(cal_day_y, cal_day_m + 1, nd), wd, sizeof(wd));
    if (k) lv_label_set_text_fmt(cal_day_btn_lab[k], "%s %d  " LV_SYMBOL_RIGHT, wd, nd);
    else lv_label_set_text_fmt(cal_day_btn_lab[k], LV_SYMBOL_LEFT "  %s %d", wd, nd);
  }
}

// Opens the day view for y/m (0-11)/day.
void open_calendar_day(int y, int m, int day)
{
  if (day < 1 || day > cal_days_in_month(y, m + 1) || !cal_day_list) return;
  cal_day_y = y;
  cal_day_m = m;
  cal_day_d = day;
  cal_day_fill();
  if (!in_subscreen) open_subscreen(cal_day_screen);
}

// A day of the month on screen (also used for the screenshot page "calday").
void open_calendar_day_screen(int day)
{
  int vy, vm;
  cal_view_ym(&vy, &vm);
  open_calendar_day(vy, vm, day);
}

static void cal_day_step_cb(lv_event_t *e)
{
  cal_day_d += (int)(intptr_t)lv_event_get_user_data(e);
  cal_day_fill();
}

// user_data is &cal_cell_day[i] (not the day value itself) so this stays
// correct after a month change updates cal_cell_day - see this file's
// header comment.
void calendar_cell_clicked_cb(lv_event_t *e)
{
  int *day_ptr = (int *)lv_event_get_user_data(e);
  if (!day_ptr || *day_ptr < 1) return;
  open_calendar_day_screen(*day_ptr);
}

static void cal_month_arrow_cb(lv_event_t *e)
{
  int dir = (int)(intptr_t)lv_event_get_user_data(e);
  cal_view_offset = constrain(cal_view_offset + dir, -CAL_BROWSE_MONTHS, CAL_BROWSE_MONTHS);
  if (cal_view_offset != 0)
  {
    int vy, vm;
    cal_view_ym(&vy, &vm);
    data_lock();
    if (cal_view_failed && cal_view_req_year == vy && cal_view_req_month == vm) cal_view_failed = false; // try again
    cal_request_month(vy, vm);
    data_unlock();
  }
  calendar_apply_to_ui();
}

static void cal_up_clicked_cb(lv_event_t *e)
{
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (!cal_up_start[i]) return;
  struct tm s;
  localtime_r(&cal_up_start[i], &s);
  open_calendar_day(s.tm_year + 1900, s.tm_mon, s.tm_mday);
}

lv_obj_t *create_calendar_day_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Top-left is the back button (the corner tap goes back too). The other
  // corners are hotspots (Settings, previous/next screen), so the day
  // buttons sit at the bottom center.
  make_back_button(scr);

  cal_day_title_lab = lv_label_create(scr);
  lv_obj_set_style_text_color(cal_day_title_lab, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(cal_day_title_lab, &lv_font_ui_24, LV_PART_MAIN);
  lv_obj_align(cal_day_title_lab, LV_ALIGN_TOP_MID, 20, 24);

  cal_day_list = lv_obj_create(scr);
  lv_obj_set_pos(cal_day_list, 20, 76);
  lv_obj_set_size(cal_day_list, 440, 334);
  lv_obj_set_style_bg_opa(cal_day_list, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(cal_day_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(cal_day_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(cal_day_list, 8, LV_PART_MAIN);
  lv_obj_set_flex_flow(cal_day_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(cal_day_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(cal_day_list, LV_SCROLLBAR_MODE_ACTIVE);

  for (int k = 0; k < 2; k++)
  {
    lv_obj_t *b = lv_btn_create(scr);
    lv_obj_set_size(b, 136, 44);
    lv_obj_set_pos(b, k ? 244 : 100, 420);
    lv_obj_set_style_bg_color(b, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(b, cal_day_step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(k ? 1 : -1));
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_ui_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, COLOR_PRIMARY, LV_PART_MAIN);
    lv_obj_center(l);
    cal_day_btn[k] = b;
    cal_day_btn_lab[k] = l;
  }
  return scr;
}

lv_obj_t *create_calendar_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Title between the two arrows - both clear of the corner hotspots
  // (jump menu top-left, Settings top-right).
  cal_title_lab = lv_label_create(scr);
  lv_obj_set_style_text_color(cal_title_lab, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(cal_title_lab, &lv_font_ui_24, LV_PART_MAIN);
  lv_obj_set_width(cal_title_lab, 216);
  lv_obj_set_style_text_align(cal_title_lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_pos(cal_title_lab, 132, 22);
  for (int k = 0; k < 2; k++)
  {
    lv_obj_t *b = lv_btn_create(scr);
    lv_obj_set_size(b, 40, 40);
    lv_obj_set_pos(b, k ? 348 : 92, 16);
    lv_obj_set_style_bg_color(b, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_radius(b, 12, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b, cal_month_arrow_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(k ? 1 : -1));
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, k ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(l, COLOR_PRIMARY, LV_PART_MAIN);
    lv_obj_center(l);
    cal_arrow[k] = b;
  }

  const char *letters = tr(S_DAY_LETTERS); // Monday first
  const lv_coord_t grid_x = 16, grid_top = 80, cell_w = 64;
  for (int c = 0; c < 7; c++)
  {
    lv_obj_t *h = lv_label_create(scr);
    char one[2] = {letters[c], '\0'};
    lv_label_set_text(h, one);
    lv_obj_set_style_text_color(h, c >= 5 ? lv_color_hex(0x5F6B75) : COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(h, &lv_font_ui_12, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(h, 1, LV_PART_MAIN);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(h, cell_w);
    lv_obj_set_pos(h, grid_x + c * cell_w, 62);
  }

  // Cell positions are fixed for the lifetime of the screen - only content
  // changes on refresh. 6 rows x CAL_ROW_H: the number (today in a filled
  // circle) and up to three event dots under it.
  for (int i = 0; i < CALENDAR_GRID_CELLS; i++)
  {
    int row = i / 7, col = i % 7;
    lv_coord_t cx = grid_x + col * cell_w + cell_w / 2;
    lv_coord_t cy = grid_top + row * CAL_ROW_H + CAL_ROW_H / 2;

    lv_obj_t *today = cal_box(scr, 28, 28, COLOR_PRIMARY, LV_RADIUS_CIRCLE);
    lv_obj_set_pos(today, cx - 14, cy - 17);
    lv_obj_add_flag(today, LV_OBJ_FLAG_HIDDEN);
    cal_cell_today[i] = today;

    lv_obj_t *lab = lv_label_create(scr);
    lv_obj_set_style_text_font(lab, &lv_font_ui_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(lab, 34);
    lv_obj_add_flag(lab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(lab, cx - 17, cy - 3 - lv_font_ui_16.line_height / 2);
    cal_cell_label[i] = lab;

    for (int k = 0; k < 3; k++)
    {
      lv_obj_t *dot = cal_box(scr, 5, 5, COLOR_PRIMARY, LV_RADIUS_CIRCLE);
      lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
      cal_cell_dot[i][k] = dot;
    }

    // Invisible full-cell touch target, on top so it catches the tap.
    // calendar_cell_clicked_cb() no-ops on blank cells (cal_cell_day == -1).
    lv_obj_t *touch = lv_obj_create(scr);
    lv_obj_set_size(touch, cell_w, CAL_ROW_H);
    lv_obj_set_pos(touch, grid_x + col * cell_w, cy - CAL_ROW_H / 2);
    lv_obj_set_style_bg_opa(touch, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(touch, 0, LV_PART_MAIN);
    lv_obj_clear_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch, calendar_cell_clicked_cb, LV_EVENT_CLICKED, &cal_cell_day[i]);
  }

  // Up next: the next three events; a tap opens that day.
  lv_obj_t *cap;
  cal_label(scr, &cap, &lv_font_ui_12, COLOR_TEXT_MUTED, 22, 316, tr(S_UP_NEXT));
  lv_obj_set_style_text_letter_space(cap, 1, LV_PART_MAIN);
  cal_label(scr, &cal_status_lab, &lv_font_ui_12, COLOR_TEXT_MUTED, 0, 316, "");
  lv_obj_set_width(cal_status_lab, 300);
  lv_obj_set_style_text_align(cal_status_lab, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_obj_set_x(cal_status_lab, 158);
  for (int i = 0; i < CAL_UP_ROWS; i++)
  {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_pos(row, 22, 334 + i * 38);
    lv_obj_set_size(row, 436, 32);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x1E1E1E), LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(row, true, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(row, cal_up_clicked_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    cal_up_row[i] = row;
    cal_up_bar[i] = cal_box(row, 4, 32, COLOR_PRIMARY, 0);
    cal_label(row, &cal_up_when[i], &lv_font_ui_14, COLOR_PRIMARY, 14, 7, "", 124);
    cal_label(row, &cal_up_title[i], &lv_font_ui_14, lv_color_hex(0xFFFFFF), 142, 7, "", 204);
    cal_label(row, &cal_up_rel[i], &lv_font_ui_12, COLOR_TEXT_MUTED, 0, 9, "", 80);
    lv_obj_set_style_text_align(cal_up_rel[i], LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    lv_obj_set_x(cal_up_rel[i], 344);
  }

  calendar_apply_to_ui();
  return scr;
}

// ==================== Agenda page ====================
// The next 7 days as one list, empty days left out - opened by tapping the
// "Next" card on Home. Days of this month come from the month data (so
// today's finished events show too, dimmed); later ones from the upcoming
// list (a week ahead).
static lv_obj_t *agenda_screen = NULL;
static lv_obj_t *agenda_list = NULL;

static void agenda_header(const char *text)
{
  lv_obj_t *lab = lv_label_create(agenda_list);
  lv_label_set_text(lab, text);
  lv_obj_set_style_text_color(lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(lab, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(lab, 1, LV_PART_MAIN);
  lv_obj_set_style_pad_top(lab, 6, LV_PART_MAIN);
}

static void agenda_row(const char *when, const char *title, bool past, int feed = -1)
{
  lv_obj_t *row = lv_obj_create(agenda_list);
  lv_obj_set_size(row, 440, 44);
  lv_obj_set_style_bg_color(row, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE); // drags scroll the list

  if (feed >= 0)
  {
    lv_obj_t *bar = cal_box(row, 4, 44, past ? lv_color_mix(cal_feed_color(feed), COLOR_BG, 90) : cal_feed_color(feed), 0);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_clip_corner(row, true, LV_PART_MAIN);
  }

  lv_obj_t *t = lv_label_create(row);
  lv_label_set_text(t, when);
  lv_obj_set_style_text_color(t, past ? COLOR_TEXT_MUTED : COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(t, &lv_font_ui_16, LV_PART_MAIN);
  lv_obj_align(t, LV_ALIGN_LEFT_MID, 14, 0);

  lv_obj_t *n = lv_label_create(row);
  lv_label_set_text(n, title[0] ? title : tr(S_UNTITLED));
  lv_obj_set_style_text_color(n, past ? COLOR_TEXT_MUTED : lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(n, &lv_font_ui_16, LV_PART_MAIN);
  lv_obj_set_width(n, 330);
  lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
  lv_obj_align(n, LV_ALIGN_LEFT_MID, 96, 0);
}

// Adds day `which` (0 today ... 6) to the list; returns false (and adds
// nothing) when it has no events.
static bool agenda_add_day(int which, time_t now)
{
  struct tm d;
  time_t t = now + which * 86400;
  localtime_r(&t, &d);
  bool same_month = calendar_data_is_current() && d.tm_mon == clock_mon && d.tm_year + 1900 == clock_year;
  int count = 0;
  if (same_month) count = min((int)calendar->day_count[d.tm_mday], CALENDAR_MAX_EVENTS_PER_DAY);
  else if (calendar && calendar_have_data)
  {
    for (int i = 0; i < calendar->upcoming_count; i++)
    {
      struct tm s;
      localtime_r(&calendar->upcoming[i].start, &s);
      if (s.tm_yday == d.tm_yday && s.tm_year == d.tm_year) count++;
    }
  }
  if (!count) return false;

  char hdr[64], dm[32];
  format_day_month(dm, sizeof(dm), d.tm_mon, d.tm_mday);
  if (which < 2) snprintf(hdr, sizeof(hdr), "%s  -  %s", which ? tr(S_TOMORROW_UP) : tr(S_TODAY_UP), dm);
  else format_long_date(hdr, sizeof(hdr), d.tm_wday, d.tm_mon, d.tm_mday);
  agenda_header(hdr);

  if (same_month)
  {
    for (int i = 0; i < count; i++)
    {
      CalendarEvent &ev = calendar->days[d.tm_mday][i];
      agenda_row(ev.all_day ? tr(S_ALL_DAY) : ev.time, ev.title, which == 0 && !ev.all_day && ev.end <= now, ev.feed);
    }
  }
  else
  {
    for (int i = 0; i < calendar->upcoming_count; i++)
    {
      UpcomingEvent &ev = calendar->upcoming[i];
      struct tm s;
      localtime_r(&ev.start, &s);
      if (s.tm_yday != d.tm_yday || s.tm_year != d.tm_year) continue;
      char when[8];
      strftime(when, sizeof(when), "%H:%M", &s);
      agenda_row(ev.all_day ? tr(S_ALL_DAY) : when, ev.title, false, ev.feed);
    }
  }
  return true;
}

void open_agenda_screen()
{
  if (!agenda_screen || screen_pinned) return;
  lv_obj_clean(agenda_list);
  time_t now = time(nullptr);
  data_lock();
  if (now > 1700000000)
  {
    bool any = false;
    for (int i = 0; i < 7; i++) any |= agenda_add_day(i, now);
    if (!any) agenda_row("", calendar_has_feeds() ? tr(S_NOTHING_WEEK) : tr(S_NO_CAL), true);
  }
  else agenda_header(tr(S_WAIT_CLOCK));
  data_unlock();
  lv_obj_scroll_to_y(agenda_list, 0, LV_ANIM_OFF);
  open_subscreen(agenda_screen);
}

void home_next_clicked_cb(lv_event_t *e)
{
  open_agenda_screen();
}

lv_obj_t *create_agenda_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  agenda_screen = scr;

  make_back_button(scr);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, tr(S_AGENDA));
  lv_obj_set_style_text_color(title, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_ui_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

  // Scrolls vertically when there are many events.
  agenda_list = lv_obj_create(scr);
  lv_obj_set_size(agenda_list, 456, 396);
  lv_obj_align(agenda_list, LV_ALIGN_TOP_MID, 0, 76);
  lv_obj_set_style_bg_opa(agenda_list, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(agenda_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(agenda_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_left(agenda_list, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(agenda_list, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(agenda_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(agenda_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(agenda_list, LV_SCROLLBAR_MODE_OFF);
  return scr;
}

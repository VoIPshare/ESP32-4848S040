// ==================== Screen 4: Calendar ====================
// Split out of ESP32-4848S040.ino. Renders the real current month (via the
// same NTP sync that drives the Home screen clock - see clock_year/mon/mday
// in ESP32-4848S040.ino) and overlays which days have events. Filled dot =
// event day, ringed dot = today, ringed+filled = today with an event.
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

static lv_obj_t *cal_title_lab = NULL;
static lv_obj_t *cal_legend_lab = NULL;
static lv_obj_t *cal_cell_marker[CALENDAR_GRID_CELLS] = {NULL};
static lv_obj_t *cal_cell_label[CALENDAR_GRID_CELLS] = {NULL};
static int cal_cell_day[CALENDAR_GRID_CELLS]; // day-of-month for each cell, -1 if blank

// Latest calendar data (per-day lists for the day-detail screen, upcoming
// events and the next flight for the Home strip). ~13 KB, so it lives in
// PSRAM - see calendar_data(). CalendarData itself is defined in
// calendar_types.h, not here - see that header's comment for why.
static CalendarData *calendar = NULL;
static bool calendar_have_data = false; // true once a fetch has completed at least once
static bool calendar_fetch_failed = false;

// Live status of calendar->next_flight (AeroDataBox) - see web_fetch.ino.
static LiveFlight live_flight = {};

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

// Refreshes the title/grid/legend from clock_year/clock_mon/clock_mday (set by
// sync_clock_ntp()) and the fetched calendar data. Called once at screen
// creation and again whenever calendar_needs_ui_refresh is set (a fresh NTP
// sync and/or a fresh calendar fetch).
void calendar_apply_to_ui()
{
  int ty = clock_year, tmon = clock_mon + 1; // 1-based month
  int tdim = cal_days_in_month(ty, tmon);
  int start_col = cal_day_of_week_mon0(ty, tmon, 1);

  data_lock();
  bool current = calendar_data_is_current();

  if (cal_title_lab) lv_label_set_text_fmt(cal_title_lab, "%s %d", MONTH_NAMES[clock_mon % 12], ty);

  for (int i = 0; i < CALENDAR_GRID_CELLS; i++)
  {
    int day = i - start_col + 1;
    bool valid = (day >= 1 && day <= tdim);
    lv_obj_t *marker = cal_cell_marker[i];
    lv_obj_t *lab = cal_cell_label[i];

    cal_cell_day[i] = valid ? day : -1;

    if (!valid)
    {
      if (marker) lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
      if (lab) lv_obj_add_flag(lab, LV_OBJ_FLAG_HIDDEN);
      continue;
    }

    bool is_today = (day == clock_mday);
    bool has_event = current && calendar->has_event[day];

    if (marker)
    {
      if (has_event)
      {
        lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_bg_color(marker, COLOR_PRIMARY, LV_PART_MAIN);
        lv_obj_set_style_border_width(marker, is_today ? 2 : 0, LV_PART_MAIN);
        lv_obj_set_style_border_color(marker, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
      }
      else if (is_today)
      {
        lv_obj_clear_flag(marker, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(marker, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_border_width(marker, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(marker, COLOR_PRIMARY, LV_PART_MAIN);
      }
      else
      {
        lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
      }
    }

    if (lab)
    {
      lv_obj_clear_flag(lab, LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text_fmt(lab, "%d", day);
      lv_obj_set_style_text_color(lab, has_event ? lv_color_hex(0xFFFFFF) : lv_color_hex(0xE0E0E0), LV_PART_MAIN);
    }
  }

  if (cal_legend_lab)
  {
    int count = current ? calendar->month_event_count : 0;
    if (!calendar_has_feeds())
      lv_label_set_text(cal_legend_lab, tr(S_NO_CAL_LONG));
    else if (!calendar_have_data)
      lv_label_set_text(cal_legend_lab, calendar_fetch_failed ? tr(S_CAL_FAILED) : tr(S_CAL_SYNC));
    else if (!current)
      lv_label_set_text(cal_legend_lab, tr(S_CAL_SYNC));
    else if (count == 0)
      lv_label_set_text(cal_legend_lab, "No events this month");
    else
      lv_label_set_text_fmt(cal_legend_lab, "%d event%s this month", count, count == 1 ? "" : "s");
  }

  update_home_next_event();
  data_unlock();
  flight_refresh_screen();
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
static lv_obj_t *cal_day_empty_lab = NULL;
static lv_obj_t *cal_day_row[CALENDAR_MAX_EVENTS_PER_DAY] = {NULL};
static lv_obj_t *cal_day_row_time[CALENDAR_MAX_EVENTS_PER_DAY] = {NULL};
static lv_obj_t *cal_day_row_title[CALENDAR_MAX_EVENTS_PER_DAY] = {NULL};

void open_calendar_day_screen(int day)
{
  int ty = clock_year, tmon = clock_mon + 1;
  if (day < 1 || day > cal_days_in_month(ty, tmon)) return;

  data_lock();
  if (!calendar_data_is_current() || calendar->day_count[day] == 0)
  {
    data_unlock();
    return; // nothing to show - stay on the Calendar screen
  }

  int wday = cal_day_of_week_sun0(ty, tmon, day); // 0=Sunday, matches WEEKDAY_NAMES
  if (cal_day_title_lab)
  {
    char d[48];
    format_long_date(d, sizeof(d), wday, clock_mon, day);
    lv_label_set_text(cal_day_title_lab, d);
  }

  int n = calendar->day_count[day];
  if (n > CALENDAR_MAX_EVENTS_PER_DAY) n = CALENDAR_MAX_EVENTS_PER_DAY;

  for (int i = 0; i < CALENDAR_MAX_EVENTS_PER_DAY; i++)
  {
    if (!cal_day_row[i]) continue;
    if (i < n)
    {
      lv_obj_clear_flag(cal_day_row[i], LV_OBJ_FLAG_HIDDEN);
      CalendarEvent &ev = calendar->days[day][i];
      if (cal_day_row_time[i]) lv_label_set_text(cal_day_row_time[i], ev.all_day ? tr(S_ALL_DAY) : ev.time);
      if (cal_day_row_title[i]) lv_label_set_text(cal_day_row_title[i], ev.title[0] ? ev.title : tr(S_UNTITLED));
    }
    else
    {
      lv_obj_add_flag(cal_day_row[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (cal_day_empty_lab)
  {
    if (n == 0) lv_obj_clear_flag(cal_day_empty_lab, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(cal_day_empty_lab, LV_OBJ_FLAG_HIDDEN);
  }
  data_unlock();

  open_subscreen(cal_day_screen);
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

lv_obj_t *create_calendar_day_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Top-left is clear here: the Settings hotspot is top-right and the pin
  // hotspot is bottom-left (both 90x90, on the always-on-top LVGL layer,
  // above every screen's content - see create_settings_hotspot()/
  // create_pin_hotspot()). A button placed under either would never
  // receive the tap.
  make_back_button(scr);

  cal_day_title_lab = lv_label_create(scr);
  lv_obj_set_style_text_color(cal_day_title_lab, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(cal_day_title_lab, &lv_font_ui_28, LV_PART_MAIN);
  lv_obj_align(cal_day_title_lab, LV_ALIGN_TOP_MID, 0, 26);

  cal_day_empty_lab = lv_label_create(scr);
  lv_label_set_text(cal_day_empty_lab, tr(S_NO_EVENTS));
  lv_obj_set_style_text_color(cal_day_empty_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(cal_day_empty_lab, &lv_font_ui_16, LV_PART_MAIN);
  lv_obj_align(cal_day_empty_lab, LV_ALIGN_TOP_MID, 0, 200);
  lv_obj_add_flag(cal_day_empty_lab, LV_OBJ_FLAG_HIDDEN);

  const lv_coord_t row_h = 54;
  const lv_coord_t row_gap = 8;
  const lv_coord_t list_top = 90;

  for (int i = 0; i < CALENDAR_MAX_EVENTS_PER_DAY; i++)
  {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_size(row, 440, row_h);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, list_top + i * (row_h + row_gap));
    lv_obj_set_style_bg_color(row, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    cal_day_row[i] = row;

    lv_obj_t *time_lab = lv_label_create(row);
    lv_label_set_text(time_lab, "--:--");
    lv_obj_set_style_text_color(time_lab, COLOR_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(time_lab, &lv_font_ui_16, LV_PART_MAIN);
    lv_obj_set_width(time_lab, 70);
    lv_obj_align(time_lab, LV_ALIGN_LEFT_MID, 16, 0);
    cal_day_row_time[i] = time_lab;

    lv_obj_t *title_lab = lv_label_create(row);
    lv_label_set_text(title_lab, "");
    lv_obj_set_style_text_color(title_lab, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title_lab, &lv_font_ui_16, LV_PART_MAIN);
    lv_obj_set_width(title_lab, 320);
    lv_label_set_long_mode(title_lab, LV_LABEL_LONG_DOT);
    lv_obj_align(title_lab, LV_ALIGN_LEFT_MID, 96, 0);
    cal_day_row_title[i] = title_lab;
  }

  return scr;
}

lv_obj_t *create_calendar_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  cal_title_lab = lv_label_create(scr);
  lv_obj_set_style_text_color(cal_title_lab, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(cal_title_lab, &lv_font_ui_28, LV_PART_MAIN);
  lv_obj_align(cal_title_lab, LV_ALIGN_TOP_MID, 0, 20);

  const char *letters = tr(S_DAY_LETTERS); // Monday first
  const lv_coord_t grid_x = 16;
  const lv_coord_t grid_top = 70;
  const lv_coord_t cell_w = 64;
  const lv_coord_t cell_h = 56;
  const lv_coord_t first_row_cy = grid_top + 44; // ~10px clear of the M T W T F S S row

  for (int c = 0; c < 7; c++)
  {
    lv_obj_t *h = lv_label_create(scr);
    char one[2] = {letters[c], '\0'};
    lv_label_set_text(h, one);
    lv_obj_set_style_text_color(h, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(h, &lv_font_ui_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(h, cell_w);
    lv_obj_set_pos(h, grid_x + c * cell_w, grid_top);
  }

  // Cell positions are fixed for the lifetime of the screen - only content
  // (day number, marker style, visibility) changes on refresh, so both
  // pools are built once here rather than recreated per month.
  for (int i = 0; i < CALENDAR_GRID_CELLS; i++)
  {
    int row = i / 7, col = i % 7;
    lv_coord_t cx = grid_x + col * cell_w + cell_w / 2;
    lv_coord_t cy = first_row_cy + row * cell_h;

    lv_obj_t *marker = lv_obj_create(scr);
    lv_obj_set_size(marker, 34, 34);
    lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_clear_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(marker, cx - 17, cy - 17);
    cal_cell_marker[i] = marker;

    // Same width as the marker with centered text, so 1- and 2-digit days
    // both sit in the middle of the circle (a content-sized label at a fixed
    // offset left single digits off-center).
    lv_obj_t *lab = lv_label_create(scr);
    lv_obj_set_style_text_font(lab, &lv_font_ui_16, LV_PART_MAIN);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(lab, 34);
    lv_obj_add_flag(lab, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(lab, cx - 17, cy - lv_font_ui_16.line_height / 2);
    cal_cell_label[i] = lab;

    // Invisible full-cell touch target, on top so it - not the small dot -
    // is what actually catches the tap. Stays clickable even for blank/
    // out-of-month cells; calendar_cell_clicked_cb() just no-ops on those
    // (cal_cell_day[i] == -1) rather than needing this toggled per month.
    lv_obj_t *touch = lv_obj_create(scr);
    lv_obj_set_size(touch, cell_w, cell_h);
    lv_obj_set_pos(touch, grid_x + col * cell_w, cy - cell_h / 2);
    lv_obj_set_style_bg_opa(touch, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(touch, 0, LV_PART_MAIN);
    lv_obj_clear_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch, calendar_cell_clicked_cb, LV_EVENT_CLICKED, &cal_cell_day[i]);
  }

  cal_legend_lab = lv_label_create(scr);
  lv_obj_set_style_text_color(cal_legend_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(cal_legend_lab, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_align(cal_legend_lab, LV_ALIGN_BOTTOM_MID, 0, -46);

  calendar_apply_to_ui();

  return scr;
}

// ==================== Agenda page ====================
// Today's and tomorrow's events as one list - opened by tapping the "Next"
// card on Home. Today comes from the month data (so finished events show
// too, dimmed); tomorrow from the month data or, when tomorrow is already
// next month, from the upcoming list.
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

static void agenda_row(const char *when, const char *title, bool past)
{
  lv_obj_t *row = lv_obj_create(agenda_list);
  lv_obj_set_size(row, 440, 44);
  lv_obj_set_style_bg_color(row, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE); // drags scroll the list

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

static void agenda_add_day(int which /* 0 today, 1 tomorrow */, time_t now)
{
  struct tm d;
  time_t t = now + which * 86400;
  localtime_r(&t, &d);
  char hdr[64], dm[32];
  format_day_month(dm, sizeof(dm), d.tm_mon, d.tm_mday);
  snprintf(hdr, sizeof(hdr), "%s  -  %s", which ? tr(S_TOMORROW_UP) : tr(S_TODAY_UP), dm);
  agenda_header(hdr);

  int shown = 0;
  bool same_month = calendar_data_is_current() && d.tm_mon == clock_mon && d.tm_year + 1900 == clock_year;
  if (same_month)
  {
    for (int i = 0; i < calendar->day_count[d.tm_mday] && i < CALENDAR_MAX_EVENTS_PER_DAY; i++)
    {
      CalendarEvent &ev = calendar->days[d.tm_mday][i];
      agenda_row(ev.all_day ? tr(S_ALL_DAY) : ev.time, ev.title, which == 0 && !ev.all_day && ev.start < now);
      shown++;
    }
  }
  else if (calendar && calendar_have_data)
  {
    for (int i = 0; i < calendar->upcoming_count; i++)
    {
      UpcomingEvent &ev = calendar->upcoming[i];
      struct tm s;
      localtime_r(&ev.start, &s);
      if (s.tm_yday != d.tm_yday || s.tm_year != d.tm_year) continue;
      char when[8];
      strftime(when, sizeof(when), "%H:%M", &s);
      agenda_row(ev.all_day ? tr(S_ALL_DAY) : when, ev.title, false);
      shown++;
    }
  }
  if (!shown) agenda_row("", calendar_has_feeds() ? tr(S_NOTHING) : tr(S_NO_CAL), true);
}

void open_agenda_screen()
{
  if (!agenda_screen || screen_pinned) return;
  lv_obj_clean(agenda_list);
  time_t now = time(nullptr);
  data_lock();
  if (now > 1700000000)
  {
    agenda_add_day(0, now);
    agenda_add_day(1, now);
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

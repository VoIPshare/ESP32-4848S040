// ==================== Flight ====================
// Details of the next flight found in the calendar (calendar->next_flight,
// see df_is_flight_title()): route, countdown, departure/arrival times,
// gate and terminal. With an AeroDataBox key the times include delays and
// the status follows the flight until it lands (live_flight, refreshed by
// web_fetch.ino's flight_maintain()).
//
// The screen is only part of the swipe sequence while a flight is coming
// up or in progress - flight_screen_active() - so screen navigation skips
// it (and hides its page dot) the rest of the time. Like the other tabs,
// this is concatenated into the main sketch, so no #include is needed.

// FLIGHT_SCREEN_INDEX is defined next to SCREEN_COUNT in esp32_dashboard.ino.

static lv_obj_t *flight_number_lab = NULL;
static lv_obj_t *flight_route_lab = NULL;
static lv_obj_t *flight_date_lab = NULL;
static lv_obj_t *flight_countdown_lab = NULL;
static lv_obj_t *flight_pill = NULL;
static lv_obj_t *flight_pill_lab = NULL;
static lv_obj_t *flight_dep_val = NULL;
static lv_obj_t *flight_dep_sched_lab = NULL;
static lv_obj_t *flight_arr_val = NULL;
static lv_obj_t *flight_gate_val = NULL;
static lv_obj_t *flight_terminal_val = NULL;
static lv_obj_t *flight_title_lab = NULL;
static lv_obj_t *flight_source_lab = NULL;

static bool flight_live_for(const UpcomingEvent &fl)
{
  return live_flight.valid && live_flight.cal_start == fl.start;
}

// Shown from the moment the flight is in the calendar data (up to 7 days
// ahead) until it's over: 1 hour after the reported landing with live data,
// else the end of the calendar event.
bool flight_screen_active()
{
  data_lock();
  bool active = false;
  time_t now = time(nullptr);
  if (calendar && calendar_have_data && calendar->next_flight.start && now > 1700000000)
  {
    const UpcomingEvent &fl = calendar->next_flight;
    time_t over = fl.end > fl.start ? fl.end : fl.start + 3600;
    if (flight_live_for(fl))
    {
      if (live_flight.arr_actual) over = live_flight.arr_actual + 3600;
      else if (live_flight.arr_scheduled > over) over = live_flight.arr_scheduled + 3600;
    }
    active = now < over;
  }
  data_unlock();
  return active;
}

static void flight_format_clock(time_t t, char *buf, size_t n)
{
  if (!t) { snprintf(buf, n, "--"); return; }
  struct tm lt;
  localtime_r(&t, &lt);
  strftime(buf, n, "%H:%M", &lt);
}

void flight_apply_to_ui()
{
  if (!flight_number_lab) return;

  data_lock();
  if (!calendar || !calendar->next_flight.start)
  {
    data_unlock();
    return;
  }
  const UpcomingEvent fl = calendar->next_flight;
  const bool live = flight_live_for(fl);
  const LiveFlight f = live_flight;
  data_unlock();

  time_t now = time(nullptr);

  char number[12] = "";
  if (!df_extract_flight_number(fl.title, number, sizeof(number))) snprintf(number, sizeof(number), "Flight");
  lv_label_set_text(flight_number_lab, number);

  char from[4], to[4];
  if (df_extract_route(fl.title, from, to)) lv_label_set_text_fmt(flight_route_lab, "%s  " LV_SYMBOL_RIGHT "  %s", from, to);
  else lv_label_set_text(flight_route_lab, "");

  time_t dep_sched = live && f.dep_scheduled ? f.dep_scheduled : fl.start;
  time_t dep = live && f.dep_actual ? f.dep_actual : dep_sched;
  time_t arr = live ? (f.arr_actual ? f.arr_actual : f.arr_scheduled) : (fl.end > fl.start ? fl.end : 0);

  struct tm dt;
  localtime_r(&dep, &dt);
  char date[32];
  strftime(date, sizeof(date), "%A, %B %d", &dt);
  lv_label_set_text(flight_date_lab, date);

  // Status pill + headline: countdown before departure, then the live state.
  const char *pill = live ? "On time" : "Scheduled";
  lv_color_t pill_color = live ? COLOR_SUCCESS : COLOR_TEXT_MUTED;
  long delay_min = (live && f.dep_actual && f.dep_scheduled) ? (long)(f.dep_actual - f.dep_scheduled) / 60 : 0;
  char headline[48];
  if (now < dep)
  {
    df_format_countdown(dep - now, headline, sizeof(headline));
    headline[0] = 'I'; // "in 3h20m" -> "In 3h20m"
  }
  else
  {
    snprintf(headline, sizeof(headline), "Departed");
  }

  if (live)
  {
    char arr_clock[8];
    flight_format_clock(arr, arr_clock, sizeof(arr_clock));
    if (!strcmp(f.status, "Canceled") || !strcmp(f.status, "CanceledUncertain"))
    {
      pill = "Canceled";
      pill_color = lv_color_hex(0xEF5350);
      snprintf(headline, sizeof(headline), "Canceled");
    }
    else if (!strcmp(f.status, "Diverted"))
    {
      pill = "Diverted";
      pill_color = lv_color_hex(0xEF5350);
    }
    else if (!strcmp(f.status, "Arrived"))
    {
      pill = "Landed";
      pill_color = COLOR_SUCCESS;
      snprintf(headline, sizeof(headline), "Landed %s", arr_clock);
    }
    else if (!strcmp(f.status, "Departed") || !strcmp(f.status, "EnRoute") || !strcmp(f.status, "Approaching"))
    {
      pill = "In the air";
      pill_color = COLOR_PRIMARY;
      snprintf(headline, sizeof(headline), "Lands %s", arr_clock);
    }
    else if (!strcmp(f.status, "Boarding")) { pill = "Boarding"; pill_color = COLOR_SECONDARY; }
    else if (!strcmp(f.status, "GateClosed")) { pill = "Gate closed"; pill_color = COLOR_SECONDARY; }
    else if (!strcmp(f.status, "CheckIn")) { pill = "Check-in"; pill_color = COLOR_PRIMARY; }

    if (delay_min >= 5 && strcmp(pill, "Canceled") && strcmp(pill, "Landed") && strcmp(pill, "In the air"))
    {
      static char delayed[24];
      snprintf(delayed, sizeof(delayed), "Delayed +%ldm", delay_min);
      pill = delayed;
      pill_color = COLOR_WARNING;
    }
  }
  lv_label_set_text(flight_pill_lab, pill);
  lv_obj_set_style_bg_color(flight_pill, pill_color, LV_PART_MAIN);
  lv_label_set_text(flight_countdown_lab, headline);

  char buf[16];
  flight_format_clock(dep, buf, sizeof(buf));
  lv_label_set_text(flight_dep_val, buf);
  lv_obj_set_style_text_color(flight_dep_val, delay_min >= 5 ? COLOR_WARNING : COLOR_PRIMARY, LV_PART_MAIN);
  if (delay_min >= 5)
  {
    char sched[8];
    flight_format_clock(dep_sched, sched, sizeof(sched));
    lv_label_set_text_fmt(flight_dep_sched_lab, "was %s", sched);
  }
  else
  {
    lv_label_set_text(flight_dep_sched_lab, "");
  }
  flight_format_clock(arr, buf, sizeof(buf));
  lv_label_set_text(flight_arr_val, buf);
  lv_label_set_text(flight_gate_val, live && f.dep_gate[0] ? f.dep_gate : "--");
  lv_label_set_text(flight_terminal_val, live && f.dep_terminal[0] ? f.dep_terminal : "--");

  lv_label_set_text(flight_title_lab, fl.title);
  if (!settings.aerodatabox_key[0])
    lv_label_set_text(flight_source_lab, "Times from your calendar - add an AeroDataBox key for live status");
  else if (live)
  {
    char upd[8];
    flight_format_clock(f.last_fetch, upd, sizeof(upd));
    lv_label_set_text_fmt(flight_source_lab, "Live status by AeroDataBox, updated %s", upd);
  }
  else if (fl.start - now > 6 * 3600) // FLIGHT_LIVE_BEFORE_S in web_fetch.ino, which compiles after this tab
    lv_label_set_text(flight_source_lab, "Live status starts 6 hours before departure");
  else
    lv_label_set_text(flight_source_lab, "Waiting for live status...");
}

// Redraws the screen and keeps the page dots in sync with whether the
// flight screen is in the sequence. If the flight is over while it's
// showing, goes back to Home. Called once a minute (clock_tick_cb()) and
// after every calendar/flight refresh (calendar_apply_to_ui()).
void flight_refresh_screen()
{
  bool active = flight_screen_active();
  if (active) flight_apply_to_ui();
  if (!active && current_screen == FLIGHT_SCREEN_INDEX && !in_settings) go_to_screen_blackout(0);
  else if (page_dots[0]) update_page_indicator(current_screen);
}

// ==================== Screen 8: Flight ====================
lv_obj_t *create_flight_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  flight_number_lab = lv_label_create(scr);
  lv_label_set_text(flight_number_lab, "");
  lv_obj_set_style_text_color(flight_number_lab, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(flight_number_lab, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_align(flight_number_lab, LV_ALIGN_TOP_MID, 0, 20);

  flight_route_lab = lv_label_create(scr);
  lv_label_set_text(flight_route_lab, "");
  lv_obj_set_style_text_color(flight_route_lab, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(flight_route_lab, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(flight_route_lab, 2, LV_PART_MAIN);
  lv_obj_align(flight_route_lab, LV_ALIGN_TOP_MID, 0, 62);

  flight_date_lab = lv_label_create(scr);
  lv_label_set_text(flight_date_lab, "");
  lv_obj_set_style_text_color(flight_date_lab, lv_color_hex(0xB0BEC5), LV_PART_MAIN);
  lv_obj_set_style_text_font(flight_date_lab, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(flight_date_lab, LV_ALIGN_TOP_MID, 0, 104);

  flight_pill = make_pill(scr, "", COLOR_TEXT_MUTED, lv_color_hex(0x10181F), &flight_pill_lab);
  lv_obj_align(flight_pill, LV_ALIGN_TOP_MID, 0, 138);

  flight_countdown_lab = lv_label_create(scr);
  lv_label_set_text(flight_countdown_lab, "");
  lv_obj_set_style_text_color(flight_countdown_lab, COLOR_SECONDARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(flight_countdown_lab, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_align(flight_countdown_lab, LV_ALIGN_TOP_MID, 0, 180);

  // Departs / Arrives / Gate / Terminal - same chip row as the Home weather stats.
  lv_obj_t *row = lv_obj_create(scr);
  lv_obj_set_size(row, 440, 80);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 232);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  make_stat_chip(row, "--", "DEPARTS", COLOR_PRIMARY, &flight_dep_val, 104);
  make_stat_chip(row, "--", "ARRIVES", COLOR_PRIMARY, &flight_arr_val, 104);
  make_stat_chip(row, "--", "GATE", COLOR_SECONDARY, &flight_gate_val, 104);
  make_stat_chip(row, "--", "TERMINAL", COLOR_SECONDARY, &flight_terminal_val, 104);

  // "was 14:05" under the chips when the departure is delayed.
  flight_dep_sched_lab = lv_label_create(scr);
  lv_label_set_text(flight_dep_sched_lab, "");
  lv_obj_set_style_text_color(flight_dep_sched_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(flight_dep_sched_lab, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_width(flight_dep_sched_lab, 104);
  lv_obj_set_style_text_align(flight_dep_sched_lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(flight_dep_sched_lab, LV_ALIGN_TOP_LEFT, 20, 318);

  flight_title_lab = lv_label_create(scr);
  lv_label_set_text(flight_title_lab, "");
  lv_obj_set_style_text_color(flight_title_lab, lv_color_hex(0xE8EDF2), LV_PART_MAIN);
  lv_obj_set_style_text_font(flight_title_lab, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_width(flight_title_lab, 420);
  lv_obj_set_style_text_align(flight_title_lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(flight_title_lab, LV_LABEL_LONG_DOT);
  lv_obj_align(flight_title_lab, LV_ALIGN_TOP_MID, 0, 350);

  flight_source_lab = lv_label_create(scr);
  lv_label_set_text(flight_source_lab, "");
  lv_obj_set_style_text_color(flight_source_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(flight_source_lab, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_width(flight_source_lab, 420);
  lv_obj_set_style_text_align(flight_source_lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(flight_source_lab, LV_LABEL_LONG_WRAP);
  lv_obj_align(flight_source_lab, LV_ALIGN_TOP_MID, 0, 380);

  return scr;
}

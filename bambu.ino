// ==================== Bambu Lab printer (local MQTT) ====================
// Split out of ESP32-4848S040.ino: everything specific to the Bambu Lab
// printer widget - the local-MQTT client, HMS warning decoding, the
// print-finish flash/badge-flash effects, and the Bambu screen itself.
//
// This is an Arduino multi-tab sketch: all .ino files in this folder are
// concatenated into one translation unit at build time (main sketch first,
// then the rest alphabetically), so no #include of this file is needed and
// functions/globals defined here are visible from ESP32-4848S040.ino and
// vice versa. A few pieces of Bambu state that ESP32-4848S040.ino's
// loop()/network_task()/Wi-Fi portal touch directly (not just via a function
// call) - bambu_pname_lab, bambu_live_product_name, bambu_needs_ui_refresh,
// bambu_fail_count, bambu_gave_up - stay declared in ESP32-4848S040.ino's
// top globals block so they're declared before those earlier use points.
// Set to 0 to silence every Serial log this file produces (MQTT connect
// status, HMS/field-parsing traces, etc.) - independent of the sketch-wide
// DEBUG flag in ESP32-4848S040.ino, so Bambu's especially chatty MQTT
// traffic can be quieted at compile time without touching logging for
// weather/stocks/calendar/etc.
#define BAMBU_LOG_ENABLED 0
#if BAMBU_LOG_ENABLED
#define BAMBU_PRINT(...) Serial.print(__VA_ARGS__)
#define BAMBU_PRINTLN(...) Serial.println(__VA_ARGS__)
#define BAMBU_PRINTF(...) Serial.printf(__VA_ARGS__)
#else
#define BAMBU_PRINT(...)
#define BAMBU_PRINTLN(...)
#define BAMBU_PRINTF(...)
#endif

#define BAMBU_MAX_AMS 4
#define BAMBU_MAX_TRAYS 4
#define BAMBU_MAX_WARNINGS 4

struct BambuTray
{
  int remain = -1;
  char tray_type[16] = "";
  char tray_color[10] = ""; // hex RRGGBB or RRGGBBAA as reported by the printer
};
struct BambuAms
{
  float temp = 0;
  int humidity = 0;
  int trayCount = 0;
  BambuTray trays[BAMBU_MAX_TRAYS];
};
struct BambuStatus
{
  char job_name[48] = "";
  char gcode_state[16] = "";
  int percent = 0;
  int remain_time_min = 0;
  float bed_temp = 0;
  float nozzle_temp = 0;
  int layer_num = 0;
  int total_layer_num = 0;
  int amsCount = 0;
  BambuAms ams[BAMBU_MAX_AMS];
  int cooling_fan_speed = -1; // -1 = no data yet, vs. 0 = confirmed off
  int big_fan1_speed = -1;    // aux fan
  int big_fan2_speed = -1;    // exhaust fan
  float chamber_temp = 0;
  float nozzle_target = 0, bed_target = 0;
  int tray_now = -1; // active AMS tray as ams * 4 + slot; -1 none (254/255 = external / none)
  bool has_warning = false;
  char warning_list[BAMBU_MAX_WARNINGS][64] = {{0}};
  int warning_count = 0;
};
static BambuStatus bambu;
static int bambu_warning_display_idx = 0;

WiFiClientSecure bambu_tls;
PubSubClient bambu_mqtt(bambu_tls);
static uint32_t bambu_last_attempt_ms = 0;
#define BAMBU_RECONNECT_INTERVAL_MS 5000
#define BAMBU_MAX_FAILURES 5

// Bambu screen widgets updated live from bambu_apply_to_ui() - populated by
// create_bambu_screen(). (bambu_pname_lab and bambu_live_product_name are
// declared in ESP32-4848S040.ino - see the file header comment above.)
static lv_obj_t *bambu_status_pill = NULL, *bambu_status_pill_lab = NULL;
static lv_obj_t *bambu_arc = NULL, *bambu_pct_lab = NULL, *bambu_layer_lab = NULL;
static lv_obj_t *bambu_job_lab = NULL;
static lv_obj_t *bambu_time_lab = NULL, *bambu_time_sub = NULL, *bambu_state_word = NULL; // "1h 05m" "left" / "Finished"
static lv_obj_t *bambu_done_row = NULL, *bambu_done_val = NULL, *bambu_start_row = NULL, *bambu_start_val = NULL;
static lv_obj_t *bambu_temp_val[3] = {NULL, NULL, NULL}; // nozzle, bed, chamber
static lv_obj_t *bambu_ams_page[BAMBU_MAX_AMS], *bambu_ams_cap[BAMBU_MAX_AMS], *bambu_ams_env[BAMBU_MAX_AMS];
static lv_obj_t *bambu_tray_tile[BAMBU_MAX_AMS][BAMBU_MAX_TRAYS], *bambu_tray_sw[BAMBU_MAX_AMS][BAMBU_MAX_TRAYS],
    *bambu_tray_mat[BAMBU_MAX_AMS][BAMBU_MAX_TRAYS], *bambu_tray_rem[BAMBU_MAX_AMS][BAMBU_MAX_TRAYS],
    *bambu_tray_bar[BAMBU_MAX_AMS][BAMBU_MAX_TRAYS];
static lv_obj_t *bambu_bottom_lab = NULL; // fans, or the printer's warnings
static time_t bambu_print_started = 0;   // seen starting since boot; 0 = unknown

const char *bambu_state_label(const char *state)
{
  if (strcmp(state, "RUNNING") == 0) return "PRINTING";
  if (strcmp(state, "PREPARE") == 0) return "PREPARING";
  if (strcmp(state, "PAUSE") == 0) return "PAUSED";
  if (strcmp(state, "FINISH") == 0) return "FINISHED";
  if (state[0] == '\0') return "IDLE";
  return state;
}

// HMS (Health Management System) code -> human-readable meaning, per your
// table. Bambu reports each hms entry as separate "attr"/"code" integers;
// the code displayed in Bambu Studio/the app is the two concatenated as hex
// (16 hex digits), though some shorter (8-digit) codes appear to be the
// "code" field alone - hms_lookup() below tries both.
struct HmsCode { const char *code; const char *meaning; };
static const HmsCode HMS_TABLE[] = {
  {"0501040000030002", "Threaded rods need lubrication"},
  {"0501040000030003", "Linear rods need cleaning and lubrication"},
  {"0502040000030002", "Threaded rods need lubrication"},
  {"0300100000020001", "X-axis resonance low - belt may be loose"},
  {"0300110000020001", "Y-axis resonance low - belt may be loose"},
  {"0300120000020001", "Toolhead front cover fell off"},
  {"03000D0000010008", "Build plate not placed properly"},
  {"03000D0000010009", "Build plate not placed properly"},
  {"03000D000001000A", "Build plate not placed properly"},
  {"03000D0000020001", "Heatbed homing abnormal"},
  {"0300120000020001", "Paused - toolhead front cover fell off"},
  {"0C0003000002000C", "Build plate marker not found / wrong plate"},
  {"0300090000020001", "The extrusion motor is overloaded."},
  {"0300801C", "Clogged nozzle"},
  {"03004057", "Layer shift"},
  {"03004068", "Layer shift"},
  {"0300800C", "Layer shift"},
  {"03008004", "Filament runout"},
  {"05004001", "Failed to connect to Bambu Cloud"},
  {"05004002", "Unable to parse print job"},
  {"05004003", "Problem parsing gcode.3mf"},
  {"05004006", "Not enough storage space"},
  {"0C00030000020012", "Camera needs calibration"},
  {"0C00030000020014", "Foreign-object detection accuracy decreased"},
  {"0C00030000020015", "Foreign-object detection not working"},
  {"0C0003000003001A", "Foreign-object detection failed"},
  {"0300910000010008", "Chamber heater failed to reach target"},
};
#define HMS_TABLE_COUNT (sizeof(HMS_TABLE) / sizeof(HMS_TABLE[0]))

// AMS "slot filament ran out" codes all follow one pattern:
// 07 <ams unit: 00-03> 2 <slot: 0-3> 0000020001 (16 hex digits) - one rule
// here replaces what would otherwise be 16 near-identical HMS_TABLE rows
// (4 AMS units x 4 slots each). Returns NULL if code doesn't match.
const char *hms_ams_runout_meaning(const char *code)
{
  if (strlen(code) != 16) return NULL;
  if (strncasecmp(code, "07", 2) != 0) return NULL;
  if (code[4] != '2') return NULL;
  if (strncasecmp(code + 6, "0000020001", 10) != 0) return NULL;

  char ams_hex[3] = {code[2], code[3], '\0'};
  char slot_hex[2] = {code[5], '\0'};
  int ams_idx = (int)strtol(ams_hex, NULL, 16);
  int slot_idx = (int)strtol(slot_hex, NULL, 16);
  if (ams_idx < 0 || ams_idx > 3 || slot_idx < 0 || slot_idx > 3) return NULL;

  static char buf[64];
  snprintf(buf, sizeof(buf), "AMS %c Slot %d filament has run out. Please insert a new filament.",
           (char)('A' + ams_idx), slot_idx + 1);
  return buf;
}

const char *hms_lookup(const char *code)
{
  const char *ams_runout = hms_ams_runout_meaning(code);
  if (ams_runout) return ams_runout;
  for (size_t i = 0; i < HMS_TABLE_COUNT; i++)
    if (strcasecmp(HMS_TABLE[i].code, code) == 0) return HMS_TABLE[i].meaning;
  return NULL;
}

// HMS codes confirmed benign/false-positive for this printer and excluded
// from bambu.warning_list entirely (not shown, not counted toward
// has_warning) - as opposed to HMS_TABLE above, which still surfaces a
// code, just with a friendlier label. Unlike HMS_TABLE this doesn't claim
// to know what the code means, just that it isn't worth alerting on here.
static const char *HMS_IGNORE_LIST[] = {
  "0500060000020070", // reported harmless by user, 2026-08-16
};
#define HMS_IGNORE_COUNT (sizeof(HMS_IGNORE_LIST) / sizeof(HMS_IGNORE_LIST[0]))

bool hms_is_ignored(const char *code)
{
  for (size_t i = 0; i < HMS_IGNORE_COUNT; i++)
    if (strcasecmp(HMS_IGNORE_LIST[i], code) == 0) return true;
  return false;
}

// Bambu reports tray_color as hex "RRGGBB" or "RRGGBBAA". Returns false (no
// color applied, caller keeps its placeholder) if the string is missing/short.
bool parse_hex_color(const char *hex, lv_color_t *out)
{
  if (!hex || strlen(hex) < 6) return false;
  char byte_buf[3] = {0, 0, 0};
  byte_buf[0] = hex[0]; byte_buf[1] = hex[1];
  uint8_t r = (uint8_t)strtol(byte_buf, NULL, 16);
  byte_buf[0] = hex[2]; byte_buf[1] = hex[3];
  uint8_t g = (uint8_t)strtol(byte_buf, NULL, 16);
  byte_buf[0] = hex[4]; byte_buf[1] = hex[5];
  uint8_t b = (uint8_t)strtol(byte_buf, NULL, 16);
  *out = lv_color_make(r, g, b);
  return true;
}

// Bambu reports fan speed on a 0-15 raw scale - same rounding-to-10% used
// in the original e-paper project's drawFanStat().
int fan_percent(int raw)
{
  if (raw < 0) return -1; // no data yet
  return (int)(ceil(raw * 10.0f / 15.0f) * 10);
}

// This printer's JSON has already turned out to mix numeric and string
// representations for the same kind of field (product_name nested
// unexpectedly, non-standard print.3D.layer_num) - the plain `| default`
// idiom silently keeps the default instead of parsing a numeric STRING
// value, so these explicitly try both before falling back.
int json_as_int(JsonVariantConst v, int fallback)
{
  if (v.isNull()) return fallback;
  if (v.is<const char *>())
  {
    const char *s = v.as<const char *>();
    return (s && s[0]) ? atoi(s) : fallback;
  }
  return v.as<int>();
}

float json_as_float(JsonVariantConst v, float fallback)
{
  if (v.isNull()) return fallback;
  if (v.is<const char *>())
  {
    const char *s = v.as<const char *>();
    return (s && s[0]) ? atof(s) : fallback;
  }
  return v.as<float>();
}

// Shows/hides/re-colors the Home screen badge based on current print state
// and the finished-badge countdown. Called both from bambu_apply_to_ui()
// (whenever new MQTT data is pushed) and clock_tick_cb()'s 1s tick (purely
// to notice when the 10-minute finished window has elapsed - a local
// countdown check, not a re-fetch from the printer).
void update_home_bambu_badge()
{
  bool printing = strcmp(bambu.gcode_state, "RUNNING") == 0 || strcmp(bambu.gcode_state, "PREPARE") == 0;
  if (printing)
  {
    if (home_bambu_badge_lab)
    {
      // Progress and time left, e.g. "PRINTING 42% - 1h 05m".
      if (strcmp(bambu.gcode_state, "RUNNING") == 0 && bambu.remain_time_min > 0)
        lv_label_set_text_fmt(home_bambu_badge_lab, "%s %d%%  -  %dh %02dm", tr(S_PRINTING), bambu.percent,
                              bambu.remain_time_min / 60, bambu.remain_time_min % 60);
      else if (strcmp(bambu.gcode_state, "RUNNING") == 0)
        lv_label_set_text_fmt(home_bambu_badge_lab, "%s %d%%", tr(S_PRINTING), bambu.percent);
      else
        lv_label_set_text(home_bambu_badge_lab, tr(S_PREPARING));
      lv_obj_set_style_text_color(home_bambu_badge_lab, lv_color_hex(0x2B1600), LV_PART_MAIN);
    }
    if (home_bambu_badge)
    {
      lv_obj_set_style_bg_color(home_bambu_badge, COLOR_SECONDARY, LV_PART_MAIN);
      lv_obj_clear_flag(home_bambu_badge, LV_OBJ_FLAG_HIDDEN);
    }
  }
  else if (bambu_finish_badge_until_ms != 0 && millis() < bambu_finish_badge_until_ms)
  {
    if (home_bambu_badge_lab)
    {
      lv_label_set_text(home_bambu_badge_lab, tr(S_PRINT_FINISHED));
      lv_obj_set_style_text_color(home_bambu_badge_lab, lv_color_hex(0x0B2E13), LV_PART_MAIN);
    }
    if (home_bambu_badge)
    {
      lv_obj_set_style_bg_color(home_bambu_badge, COLOR_SUCCESS, LV_PART_MAIN);
      lv_obj_clear_flag(home_bambu_badge, LV_OBJ_FLAG_HIDDEN);
    }
  }
  else
  {
    bambu_finish_badge_until_ms = 0;
    if (home_bambu_badge) lv_obj_add_flag(home_bambu_badge, LV_OBJ_FLAG_HIDDEN);
  }
}

// Pushes the current `bambu` struct onto the on-screen widgets. Must only be
// called from the LVGL-owning thread (loop(), via bambu_needs_ui_refresh) -
// bambu_mqtt_callback() and bambu_mqtt_maintain() run on network_task and
// are not allowed to touch LVGL objects directly.
void bambu_apply_to_ui()
{
  if (bambu_gave_up)
  {
    if (bambu_job_lab) lv_label_set_text(bambu_job_lab, "Connection failed - check IP/serial/access code in Settings");
    if (bambu_status_pill_lab) lv_label_set_text(bambu_status_pill_lab, "OFFLINE");
    if (bambu_status_pill) lv_obj_set_style_bg_color(bambu_status_pill, lv_color_hex(0xEF5350), LV_PART_MAIN);
    if (home_bambu_badge_lab)
    {
      lv_label_set_text(home_bambu_badge_lab, "BAMBU ERROR");
      lv_obj_set_style_text_color(home_bambu_badge_lab, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    }
    if (home_bambu_badge) lv_obj_set_style_bg_color(home_bambu_badge, lv_color_hex(0xEF5350), LV_PART_MAIN);
    start_bambu_badge_flash();
    return;
  }

  // Start the 10-min "finished" badge window + a green full-screen flash
  // only on a genuinely-observed RUNNING/PREPARE/PAUSE -> FINISH transition -
  // not just any message that happens to say FINISH. Bambu's printer keeps
  // reporting its last real state (including a stale FINISH from hours ago)
  // regardless of our own connection history, so right after a reboot the
  // very first message we see can already say FINISH with no way to tell
  // it apart from a fresh one - unless we require that we already saw it
  // actively printing first. bambu_prev_gcode_state starts empty on boot,
  // so this naturally suppresses the false trigger there.
  bool was_printing_ish = strcmp(bambu_prev_gcode_state, "RUNNING") == 0 ||
                           strcmp(bambu_prev_gcode_state, "PREPARE") == 0 ||
                           strcmp(bambu_prev_gcode_state, "PAUSE") == 0;
  ph_note_state(bambu_prev_gcode_state, bambu.gcode_state);
  if (strcmp(bambu.gcode_state, "FAILED") == 0 && was_printing_ish)
  {
    char msg[64];
    snprintf(msg, sizeof(msg), LV_SYMBOL_WARNING "  %s", tr(S_PRINT_FAILED));
    alert_show(msg, lv_color_hex(0xC62828), 60000UL);
    ph_record(bambu.job_name, false);
  }
  if (strcmp(bambu.gcode_state, "FINISH") == 0 && was_printing_ish)
  {
    ph_record(bambu.job_name, true);
    bambu_finish_badge_until_ms = millis() + BAMBU_FINISH_BADGE_MS;
    start_finish_screen_flash();
    // The Bambu screen for BAMBU_AUTO_SHOW_MS at the end too (setting-gated),
    // then Home.
    if (settings.auto_show_bambu_on_start && !in_settings && !screen_pinned)
    {
      if (current_screen != 1) go_to_screen_blackout(1);
      bambu_screen_auto_opened = true;
      bambu_auto_return_home_at_ms = millis() + BAMBU_AUTO_SHOW_MS;
    }
  }

  // Auto-open the Bambu screen for BAMBU_AUTO_SHOW_MS on a genuine print
  // start (setting-gated), then back to Home - Home's printing card
  // (weather_slot.ino) shows the progress from there.
  // Same reboot guard as the finish badge above: requiring !was_printing_ish
  // AND percent < 1 rules out "we just reconnected mid-print" (which would
  // otherwise look identical to a fresh start on the very first message
  // after a reboot, since bambu_prev_gcode_state starts empty either way).
  // Suppressed while screen_pinned, same as every other auto-navigate.
  bool now_printing_ish = strcmp(bambu.gcode_state, "RUNNING") == 0 || strcmp(bambu.gcode_state, "PREPARE") == 0;
  if (!was_printing_ish && now_printing_ish && bambu.percent < 1) bambu_print_started = time(nullptr);
  if (settings.auto_show_bambu_on_start && !was_printing_ish && now_printing_ish && bambu.percent < 1 &&
      !in_settings && current_screen != 1 && !screen_pinned)
  {
    go_to_screen_blackout(1);
    bambu_screen_auto_opened = true;
    bambu_auto_return_home_at_ms = millis() + BAMBU_AUTO_SHOW_MS;
  }

  strncpy(bambu_prev_gcode_state, bambu.gcode_state, sizeof(bambu_prev_gcode_state) - 1);
  bambu_prev_gcode_state[sizeof(bambu_prev_gcode_state) - 1] = '\0';
  update_home_bambu_badge();
  home_print_apply(); // weather_slot.ino

  if (bambu_pname_lab)
    lv_label_set_text_fmt(bambu_pname_lab, "%s  " LV_SYMBOL_LIST, bambu_live_product_name[0] ? bambu_live_product_name : settings.printer_name);
  if (!bambu_arc) return;

  const char *st = bambu.gcode_state;
  bool running = !strcmp(st, "RUNNING"), preparing = !strcmp(st, "PREPARE"), paused = !strcmp(st, "PAUSE");
  bool active = running || preparing || paused;

  // Status pill: orange while printing, green when finished, red on failure.
  lv_color_t pill = active ? COLOR_SECONDARY : !strcmp(st, "FINISH") ? COLOR_SUCCESS : !strcmp(st, "FAILED") ? lv_color_hex(0xEF5350) : lv_color_hex(0x4A555F);
  lv_obj_set_style_bg_color(bambu_status_pill, pill, LV_PART_MAIN);
  lv_obj_set_style_text_color(bambu_status_pill_lab, active ? lv_color_hex(0x2B1600) : lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_label_set_text(bambu_status_pill_lab, bambu_state_label(st));

  // Ring: percent and layer.
  int pct = constrain(bambu.percent, 0, 100);
  lv_arc_set_value(bambu_arc, pct);
  lv_label_set_text_fmt(bambu_pct_lab, "%d%%", pct);
  if (bambu.total_layer_num > 0) lv_label_set_text_fmt(bambu_layer_lab, "Layer %d / %d", bambu.layer_num, bambu.total_layer_num);
  else lv_label_set_text(bambu_layer_lab, "");

  lv_label_set_text(bambu_job_lab, bambu.job_name[0] ? bambu.job_name : "No active job");

  // Time left, or the state in words.
  bool show_time = (running || paused) && bambu.remain_time_min > 0;
  if (show_time)
  {
    int h = bambu.remain_time_min / 60, m = bambu.remain_time_min % 60;
    if (h) lv_label_set_text_fmt(bambu_time_lab, "%dh %02dm", h, m);
    else lv_label_set_text_fmt(bambu_time_lab, "%dm", m);
    lv_label_set_text(bambu_time_sub, paused ? "left, paused" : "left");
    lv_obj_align_to(bambu_time_sub, bambu_time_lab, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -6);
  }
  else
  {
    lv_label_set_text(bambu_state_word, preparing ? "Preparing" : paused ? "Paused" : !strcmp(st, "FINISH") ? "Finished"
                                        : !strcmp(st, "FAILED") ? "Failed" : "Idle");
  }
  (show_time ? lv_obj_clear_flag : lv_obj_add_flag)(bambu_time_lab, LV_OBJ_FLAG_HIDDEN);
  (show_time ? lv_obj_clear_flag : lv_obj_add_flag)(bambu_time_sub, LV_OBJ_FLAG_HIDDEN);
  (show_time ? lv_obj_add_flag : lv_obj_clear_flag)(bambu_state_word, LV_OBJ_FLAG_HIDDEN);

  // Done at / Started, as clock times (when they're known).
  time_t now = time(nullptr);
  struct tm t, today;
  bool clock_ok = now > 1700000000 && localtime_r(&now, &today);
  bool done_known = running && bambu.remain_time_min > 0 && clock_ok;
  if (done_known)
  {
    time_t done = now + bambu.remain_time_min * 60;
    localtime_r(&done, &t);
    if (t.tm_yday == today.tm_yday) lv_label_set_text_fmt(bambu_done_val, "%02d:%02d", t.tm_hour, t.tm_min);
    else lv_label_set_text_fmt(bambu_done_val, "tomorrow %02d:%02d", t.tm_hour, t.tm_min);
  }
  (done_known ? lv_obj_clear_flag : lv_obj_add_flag)(bambu_done_row, LV_OBJ_FLAG_HIDDEN);
  bool start_known = active && bambu_print_started > 1700000000 && clock_ok;
  if (start_known)
  {
    localtime_r(&bambu_print_started, &t);
    lv_label_set_text_fmt(bambu_start_val, "%02d:%02d", t.tm_hour, t.tm_min);
  }
  (start_known ? lv_obj_clear_flag : lv_obj_add_flag)(bambu_start_row, LV_OBJ_FLAG_HIDDEN);

  // Temperatures: current, and the target in grey while one is set (orange).
  float cur[2] = {bambu.nozzle_temp, bambu.bed_temp}, tgt[2] = {bambu.nozzle_target, bambu.bed_target};
  for (int i = 0; i < 2; i++)
  {
    if (tgt[i] > 0) lv_label_set_text_fmt(bambu_temp_val[i], "%.0f\xC2\xB0 #8A97A0 / %.0f#", cur[i], tgt[i]);
    else lv_label_set_text_fmt(bambu_temp_val[i], "%.0f\xC2\xB0", cur[i]);
    lv_obj_set_style_text_color(bambu_temp_val[i], tgt[i] > 0 ? COLOR_SECONDARY : lv_color_hex(0xCFD8DC), LV_PART_MAIN);
  }
  lv_label_set_text_fmt(bambu_temp_val[2], "%.0f\xC2\xB0", bambu.chamber_temp);

  // AMS: one page per unit (at least one), four tiles each; the tray in use
  // is outlined.
  int units = bambu.amsCount > 0 ? bambu.amsCount : 1;
  for (int u = 0; u < BAMBU_MAX_AMS; u++)
  {
    (u < units ? lv_obj_clear_flag : lv_obj_add_flag)(bambu_ams_page[u], LV_OBJ_FLAG_HIDDEN);
    if (u >= units) continue;
    if (units > 1) lv_label_set_text_fmt(bambu_ams_cap[u], "AMS %d OF %d", u + 1, units);
    else lv_label_set_text(bambu_ams_cap[u], "AMS");
    bool have = u < bambu.amsCount;
    BambuAms &a = bambu.ams[u];
    if (have)
    {
      // Filament starts to suffer above ~40% - orange, then red.
      lv_label_set_text_fmt(bambu_ams_env[u], "%d%% RH \xC2\xB7 %.0f\xC2\xB0" "C", a.humidity, a.temp);
      lv_obj_set_style_text_color(bambu_ams_env[u], a.humidity >= 50 ? lv_color_hex(0xEF5350) : a.humidity >= 40 ? COLOR_SECONDARY : COLOR_TEXT_MUTED, LV_PART_MAIN);
    }
    else lv_label_set_text(bambu_ams_env[u], "no AMS data");
    for (int i = 0; i < BAMBU_MAX_TRAYS; i++)
    {
      bool tray = have && i < a.trayCount;
      BambuTray *tr = tray ? &a.trays[i] : NULL;
      bool loaded = tray && (tr->tray_type[0] || tr->remain >= 0);
      bool in_use = bambu.tray_now == u * BAMBU_MAX_TRAYS + i;
      lv_color_t col;
      bool has_col = loaded && parse_hex_color(tr->tray_color, &col);
      lv_obj_set_style_bg_opa(bambu_tray_sw[u][i], has_col ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
      if (has_col) lv_obj_set_style_bg_color(bambu_tray_sw[u][i], col, LV_PART_MAIN);
      lv_label_set_text(bambu_tray_mat[u][i], loaded ? (tr->tray_type[0] ? tr->tray_type : "?") : "Empty");
      lv_obj_set_style_text_color(bambu_tray_mat[u][i], loaded ? lv_color_hex(0xFFFFFF) : COLOR_TEXT_MUTED, LV_PART_MAIN);
      int rem = loaded ? tr->remain : -1;
      if (rem >= 0) lv_label_set_text_fmt(bambu_tray_rem[u][i], in_use ? "%d%% \xC2\xB7 in use" : "%d%%", rem);
      else lv_label_set_text(bambu_tray_rem[u][i], in_use ? "in use" : loaded ? "--" : "");
      lv_bar_set_value(bambu_tray_bar[u][i], rem > 0 ? rem : 0, LV_ANIM_OFF);
      // Light filament stays visible on the dark track; very dark gets grey.
      lv_color_t bar_col = has_col ? col : COLOR_TEXT_MUTED;
      if (has_col && lv_color_brightness(col) < 40) bar_col = lv_color_hex(0x9AA4AD);
      lv_obj_set_style_bg_color(bambu_tray_bar[u][i], bar_col, LV_PART_INDICATOR);
      lv_obj_set_style_border_width(bambu_tray_tile[u][i], in_use ? 2 : 0, LV_PART_MAIN);
    }
  }

  // Bottom line: the printer's warnings (cycled by bambu_warning_cycle_cb),
  // otherwise the fans.
  if (bambu.has_warning)
  {
    if (bambu_warning_display_idx >= bambu.warning_count) bambu_warning_display_idx = 0;
    const char *w = bambu.warning_count > 0 ? bambu.warning_list[bambu_warning_display_idx] : "";
    lv_label_set_text_fmt(bambu_bottom_lab, LV_SYMBOL_WARNING "  %s", w[0] ? w : "Printer reports an issue");
    lv_obj_set_style_text_color(bambu_bottom_lab, COLOR_WARNING, LV_PART_MAIN);
  }
  else
  {
    int f[3] = {fan_percent(bambu.cooling_fan_speed), fan_percent(bambu.big_fan1_speed), fan_percent(bambu.big_fan2_speed)};
    char v[3][8];
    for (int i = 0; i < 3; i++)
      if (f[i] < 0) strcpy(v[i], "--");
      else snprintf(v[i], sizeof(v[i]), "%d%%", f[i]);
    lv_label_set_text_fmt(bambu_bottom_lab, "Part fan #CFD8DC %s#     Aux #CFD8DC %s#     Exhaust #CFD8DC %s#", v[0], v[1], v[2]);
    lv_obj_set_style_text_color(bambu_bottom_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  }
}

void bambu_mqtt_callback(char *topic, byte *payload, unsigned int length)
{
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error)
  {
    BAMBU_PRINTLN("[Bambu] JSON parse failed");
    return;
  }

  // DIAGNOSTIC: full raw message.
  if (BAMBU_LOG_ENABLED)
  {
    String fullJson;
    serializeJson(doc, fullJson);
    BAMBU_PRINTF("[Bambu] full json: %s\n", fullJson.c_str());
  }

  JsonObject info = doc["info"];
  if (!info.isNull())
  {
    if (BAMBU_LOG_ENABLED)
    {
      BAMBU_PRINT("[Bambu] info keys:");
      for (JsonPair kv : info) { BAMBU_PRINT(" "); BAMBU_PRINT(kv.key().c_str()); }
      BAMBU_PRINTLN();
      String infoStr;
      serializeJson(info, infoStr);
      BAMBU_PRINTF("[Bambu] info raw: %s\n", infoStr.c_str());
    }

    // Model name lives at info.module[i].product_name (e.g. "ota" module) -
    // take the first module that reports one.
    JsonArray modules = info["module"];
    if (!modules.isNull())
    {
      for (JsonObject mod : modules)
      {
        const char *pname = mod["product_name"] | (const char *)NULL;
        if (pname && pname[0])
        {
          strncpy(bambu_live_product_name, pname, sizeof(bambu_live_product_name) - 1);
          bambu_live_product_name[sizeof(bambu_live_product_name) - 1] = '\0';
          bambu_needs_ui_refresh = true;
          break;
        }
      }
    }
  }
  if (doc["dev_name"].is<const char *>()) BAMBU_PRINTF("[Bambu] dev_name (top-level): %s\n", doc["dev_name"].as<const char *>());
  if (doc["dev_type"].is<const char *>()) BAMBU_PRINTF("[Bambu] dev_type (top-level): %s\n", doc["dev_type"].as<const char *>());

  JsonObject print = doc["print"];
  if (print.isNull()) return;

  if (print["dev_name"].is<const char *>()) BAMBU_PRINTF("[Bambu] dev_name (print): %s\n", print["dev_name"].as<const char *>());
  if (print["dev_type"].is<const char *>()) BAMBU_PRINTF("[Bambu] dev_type (print): %s\n", print["dev_type"].as<const char *>());

  bambu.layer_num = print["3D"]["layer_num"] | bambu.layer_num;
  bambu.total_layer_num = print["3D"]["total_layer_num"] | bambu.total_layer_num;
  bambu.percent = print["percent"] | bambu.percent;
  bambu.remain_time_min = print["mc_remaining_time"] | bambu.remain_time_min;
  bambu.bed_temp = print["bed_temper"] | bambu.bed_temp;
  bambu.nozzle_temp = print["nozzle_temper"] | bambu.nozzle_temp;
  bambu.nozzle_target = json_as_float(print["nozzle_target_temper"], bambu.nozzle_target);
  bambu.bed_target = json_as_float(print["bed_target_temper"], bambu.bed_target);
  bambu.cooling_fan_speed = json_as_int(print["cooling_fan_speed"], bambu.cooling_fan_speed);
  bambu.big_fan1_speed = json_as_int(print["big_fan1_speed"], bambu.big_fan1_speed);
  bambu.big_fan2_speed = json_as_int(print["big_fan2_speed"], bambu.big_fan2_speed);
  if (DEBUG && (!print["cooling_fan_speed"].isNull() || !print["big_fan1_speed"].isNull() || !print["big_fan2_speed"].isNull()))
  {
    BAMBU_PRINTF("[Bambu] fans raw: cooling=%s(%s) big_fan1=%s(%s) big_fan2=%s(%s) -> parsed %d/%d/%d\n",
                  print["cooling_fan_speed"].as<String>().c_str(), print["cooling_fan_speed"].is<const char *>() ? "str" : "num",
                  print["big_fan1_speed"].as<String>().c_str(), print["big_fan1_speed"].is<const char *>() ? "str" : "num",
                  print["big_fan2_speed"].as<String>().c_str(), print["big_fan2_speed"].is<const char *>() ? "str" : "num",
                  bambu.cooling_fan_speed, bambu.big_fan1_speed, bambu.big_fan2_speed);
  }

  // Chamber temp: this printer reports it at print.info.temp (confirmed
  // against the printer's own display), with print.device.ctc.info.temp and
  // the more commonly-documented print.chamber_temper as fallbacks for
  // other printer models/firmware.
  JsonVariant chamberVar = print["info"]["temp"];
  if (chamberVar.isNull()) chamberVar = print["device"]["ctc"]["info"]["temp"];
  if (chamberVar.isNull()) chamberVar = print["chamber_temper"];
  if (!chamberVar.isNull())
  {
    float prev_chamber = bambu.chamber_temp;
    bambu.chamber_temp = json_as_float(chamberVar, bambu.chamber_temp);
    BAMBU_PRINTF("[Bambu] chamber temp raw=%s(%s) -> parsed %.1f (prev %.1f)\n",
                  chamberVar.as<String>().c_str(),
                  chamberVar.is<const char *>() ? "str" : "num",
                  bambu.chamber_temp, prev_chamber);
  }

  JsonArray hms = print["hms"];
  if (!hms.isNull())
  {
    // Bambu re-sends the same unresolved hms array on most push reports
    // while printing. Decode into a scratch list first and only replace
    // bambu.warning_list (and reset the 5s display cycle) if the actual
    // set of warnings changed - otherwise the cycle timer never gets to
    // run its full course between messages.
    char new_list[BAMBU_MAX_WARNINGS][64] = {{0}};
    int new_count = 0;
    for (JsonObject entry : hms)
    {
      if (new_count >= BAMBU_MAX_WARNINGS) break;
      int attr = json_as_int(entry["attr"], 0);
      int code = json_as_int(entry["code"], 0);
      char combined16[17], code_only[9];
      snprintf(combined16, sizeof(combined16), "%08X%08X", attr, code);
      snprintf(code_only, sizeof(code_only), "%08X", code);

      if (hms_is_ignored(combined16)) continue;

      const char *meaning = hms_lookup(combined16);
      if (!meaning) meaning = hms_lookup(code_only);

      char *slot = new_list[new_count];
      if (meaning)
      {
        strncpy(slot, meaning, 63);
      }
      else
      {
        // Not in our small curated table - show the full code in Bambu's
        // own dash-grouped format (XXXX-XXXX-XXXX-XXXX) so it can be looked
        // up directly against the printer's QR code / wiki.bambulab.com.
        snprintf(slot, 64, "Code %.4s-%.4s-%.4s-%.4s",
                 combined16, combined16 + 4, combined16 + 8, combined16 + 12);
      }
      slot[63] = '\0';
      new_count++;
    }

    bool changed = (new_count != bambu.warning_count);
    if (!changed)
      for (int i = 0; i < new_count; i++)
        if (strcmp(new_list[i], bambu.warning_list[i]) != 0) { changed = true; break; }

    if (changed)
    {
      bambu.warning_count = new_count;
      for (int i = 0; i < new_count; i++) strncpy(bambu.warning_list[i], new_list[i], 64);
      bambu_warning_display_idx = 0;
    }
    bambu.has_warning = bambu.warning_count > 0;
  }
  if (!print["print_error"].isNull())
  {
    int print_error = json_as_int(print["print_error"], 0);
    if (print_error != 0)
    {
      bambu.has_warning = true;
      if (bambu.warning_count == 0)
      {
        snprintf(bambu.warning_list[bambu.warning_count], 64, "Print error %d", print_error);
        bambu.warning_count++;
      }
    }
  }

  const char *name = print["subtask_name"] | "";
  if (name[0])
  {
    strncpy(bambu.job_name, name, sizeof(bambu.job_name) - 1);
    bambu.job_name[sizeof(bambu.job_name) - 1] = '\0';
  }

  const char *state = print["gcode_state"] | "";
  if (state[0])
  {
    strncpy(bambu.gcode_state, state, sizeof(bambu.gcode_state) - 1);
    bambu.gcode_state[sizeof(bambu.gcode_state) - 1] = '\0';
  }

  // Bambu sends partial/delta reports - only overwrite AMS state on messages
  // that actually include it, so we don't blank it out on unrelated updates.
  if (!print["ams"]["tray_now"].isNull())
  {
    int t = json_as_int(print["ams"]["tray_now"], -1);
    bambu.tray_now = t >= 0 && t < BAMBU_MAX_AMS * BAMBU_MAX_TRAYS ? t : -1;
  }
  JsonArray amsArray = print["ams"]["ams"];
  if (!amsArray.isNull())
  {
    bambu.amsCount = 0;
    for (JsonObject amsObj : amsArray)
    {
      if (bambu.amsCount >= BAMBU_MAX_AMS) break;
      BambuAms &a = bambu.ams[bambu.amsCount++];
      a.temp = json_as_float(amsObj["temp"], a.temp);
      a.humidity = json_as_int(amsObj["humidity_raw"], a.humidity);
      a.trayCount = 0;

      JsonArray trayArray = amsObj["tray"];
      if (!trayArray.isNull())
      {
        for (JsonObject trayObj : trayArray)
        {
          if (a.trayCount >= BAMBU_MAX_TRAYS) break;
          BambuTray &t = a.trays[a.trayCount++];
          t.remain = trayObj["remain"] | -1;
          const char *ttype = trayObj["tray_type"] | "";
          strncpy(t.tray_type, ttype, sizeof(t.tray_type) - 1);
          t.tray_type[sizeof(t.tray_type) - 1] = '\0';
          const char *tcolor = trayObj["tray_color"] | "";
          strncpy(t.tray_color, tcolor, sizeof(t.tray_color) - 1);
          t.tray_color[sizeof(t.tray_color) - 1] = '\0';
        }
      }
    }
  }

  bambu_needs_ui_refresh = true;
}

#define BAMBU_REFRESH_INTERVAL_MS (60UL * 1000UL)
static uint32_t bambu_last_refresh_ms = 0;

// Bambu's own periodic reports are often partial/delta updates - explicitly
// requesting a full "pushall" on a timer keeps everything (AMS, temps, job
// info) genuinely fresh rather than relying on whatever the printer decides
// to push on its own.
void bambu_request_pushall()
{
  char topic[64];
  snprintf(topic, sizeof(topic), "device/%s/request", settings.mqtt_sn);
  bambu_mqtt.publish(topic, "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\"}}");
  BAMBU_PRINTLN("[Bambu] Requested full refresh (pushall)");
}

// Standard local-MQTT request for device/module info - the reply (an "info"
// key on the report topic) is where the printer model name is most likely
// to actually be, per the diagnostic dump in bambu_mqtt_callback().
void bambu_request_info()
{
  char topic[64];
  snprintf(topic, sizeof(topic), "device/%s/request", settings.mqtt_sn);
  bambu_mqtt.publish(topic, "{\"info\":{\"sequence_id\":\"0\",\"command\":\"get_version\"}}");
  BAMBU_PRINTLN("[Bambu] Requested device info (get_version)");
}

// Non-blocking except for the TLS handshake on an actual connect attempt
// (gated to once per BAMBU_RECONNECT_INTERVAL_MS); called every loop().
void bambu_mqtt_maintain()
{
  if (!wifi_connected) return;
  if (settings.printer_ip[0] == '\0' || settings.mqtt_sn[0] == '\0' || settings.mqtt_pass[0] == '\0') return;
  if (bambu_gave_up) return;

  if (bambu_mqtt.connected())
  {
    bambu_mqtt.loop();
    if (millis() - bambu_last_refresh_ms > BAMBU_REFRESH_INTERVAL_MS)
    {
      bambu_last_refresh_ms = millis();
      bambu_request_pushall();
    }
    return;
  }

  uint32_t now = millis();
  if (now - bambu_last_attempt_ms < BAMBU_RECONNECT_INTERVAL_MS) return;
  bambu_last_attempt_ms = now;

  bambu_tls.setInsecure(); // Bambu's local MQTT broker uses a self-signed cert
  // NetworkClientSecure defaults to a 120s TLS handshake timeout - if the
  // printer is unreachable/wrong IP, connect() would block the whole loop()
  // (and therefore all LVGL rendering/touch handling) for up to 2 minutes.
  // setHandshakeTimeout() takes SECONDS, not ms.
  bambu_tls.setHandshakeTimeout(5);
  bambu_mqtt.setBufferSize(20000);
  bambu_mqtt.setServer(settings.printer_ip, settings.printer_port);
  bambu_mqtt.setCallback(bambu_mqtt_callback);

  BAMBU_PRINTLN("[Bambu] Connecting to MQTT...");
  if (bambu_mqtt.connect("ESP32Dashboard", "bblp", settings.mqtt_pass))
  {
    char topic[64];
    snprintf(topic, sizeof(topic), "device/%s/report", settings.mqtt_sn);
    bambu_mqtt.subscribe(topic);
    BAMBU_PRINTLN("[Bambu] MQTT connected");
    bambu_fail_count = 0;
    bambu_last_refresh_ms = millis();
    bambu_request_pushall();
    bambu_request_info();
  }
  else
  {
    BAMBU_PRINTF("[Bambu] MQTT connect failed, rc=%d\n", bambu_mqtt.state());
    bambu_fail_count++;
    if (bambu_fail_count >= BAMBU_MAX_FAILURES)
    {
      bambu_gave_up = true;
      bambu_needs_ui_refresh = true;
      BAMBU_PRINTLN("[Bambu] Giving up after 5 failed attempts");
    }
  }
}

// ==================== Screen 2: Bambu printer ====================
// A plain, non-clickable box (cards, tiles, swatches): taps and swipes go
// through to the screen.
static lv_obj_t *bambu_box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, uint32_t bg, lv_coord_t radius)
{
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(bg), LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

static lv_obj_t *bambu_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, lv_coord_t x, lv_coord_t y, const char *text = "")
{
  lv_obj_t *l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
  lv_obj_set_pos(l, x, y);
  return l;
}

// Layout (480x480, page dots below y 450):
//   title + status pill
//   progress ring (percent, layer)  |  job, time left, done at, started
//   nozzle / bed / chamber chips
//   AMS: one page per unit, scrolled vertically, four tiles each
//   fans - or the printer's warnings
lv_obj_t *create_bambu_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // ---- Title (tap: print history) and status ----
  lv_obj_t *pname = bambu_label(scr, &lv_font_ui_20, lv_color_hex(0xFFFFFF), 22, 24);
  lv_label_set_text_fmt(pname, "%s  " LV_SYMBOL_LIST, settings.printer_name);
  lv_obj_add_flag(pname, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(pname, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_ext_click_area(pname, 14);
  lv_obj_add_event_cb(pname, open_print_history, LV_EVENT_SHORT_CLICKED, NULL);
  bambu_pname_lab = pname;

  bambu_status_pill = make_pill(scr, "IDLE", lv_color_hex(0x4A555F), lv_color_hex(0xFFFFFF), &bambu_status_pill_lab);
  lv_obj_align(bambu_status_pill, LV_ALIGN_TOP_RIGHT, -22, 22);

  // ---- Progress ring ----
  lv_obj_t *arc = lv_arc_create(scr);
  lv_obj_set_size(arc, 150, 150);
  lv_obj_set_pos(arc, 22, 68);
  lv_arc_set_rotation(arc, 270);
  lv_arc_set_bg_angles(arc, 0, 360);
  lv_arc_set_range(arc, 0, 100);
  lv_arc_set_value(arc, 0);
  lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, lv_color_hex(0x2E3540), LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, COLOR_SECONDARY, LV_PART_INDICATOR);
  bambu_arc = arc;
  bambu_pct_lab = lv_label_create(arc);
  lv_label_set_text(bambu_pct_lab, "0%");
  lv_obj_set_style_text_font(bambu_pct_lab, &lv_font_roboto_medium_44, LV_PART_MAIN);
  lv_obj_set_style_text_color(bambu_pct_lab, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_align(bambu_pct_lab, LV_ALIGN_CENTER, 0, -8);
  bambu_layer_lab = lv_label_create(arc);
  lv_label_set_text(bambu_layer_lab, "");
  lv_obj_set_style_text_font(bambu_layer_lab, &lv_font_ui_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(bambu_layer_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_align(bambu_layer_lab, LV_ALIGN_CENTER, 0, 24);

  // ---- Job, time left, done at, started ----
  bambu_job_lab = bambu_label(scr, &lv_font_ui_20, lv_color_hex(0xFFFFFF), 192, 72, "No active job");
  lv_obj_set_size(bambu_job_lab, 266, 50); // two lines at most
  lv_label_set_long_mode(bambu_job_lab, LV_LABEL_LONG_DOT);
  bambu_time_lab = bambu_label(scr, &lv_font_roboto_medium_44, lv_color_hex(0xFFFFFF), 192, 122, "");
  bambu_time_sub = bambu_label(scr, &lv_font_ui_14, COLOR_TEXT_MUTED, 0, 0, "left");
  bambu_state_word = bambu_label(scr, &lv_font_ui_28, lv_color_hex(0xFFFFFF), 192, 126, "Idle");
  lv_obj_add_flag(bambu_time_lab, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(bambu_time_sub, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t **rows[2][2] = {{&bambu_done_row, &bambu_done_val}, {&bambu_start_row, &bambu_start_val}};
  static const char *ROW_NAMES[2] = {"Done at", "Started"};
  for (int i = 0; i < 2; i++)
  {
    lv_obj_t *row = bambu_box(scr, 192, 172 + i * 24, 266, 22, 0x000000, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
    bambu_label(row, &lv_font_ui_14, COLOR_TEXT_MUTED, 0, 2, ROW_NAMES[i]);
    lv_obj_t *v = bambu_label(row, &lv_font_ui_16, lv_color_hex(0xFFFFFF), 0, 0, "");
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    *rows[i][0] = row;
    *rows[i][1] = v;
  }

  // ---- Temperatures ----
  static const char *TEMP_NAMES[3] = {"NOZZLE", "BED", "CHAMBER"};
  for (int i = 0; i < 3; i++)
  {
    lv_obj_t *chip = bambu_box(scr, 22 + i * 148, 230, 140, 60, 0x1E1E1E, 14);
    bambu_temp_val[i] = bambu_label(chip, &lv_font_ui_24, i == 2 ? COLOR_PRIMARY : lv_color_hex(0xCFD8DC), 12, 6, "--\xC2\xB0");
    lv_label_set_recolor(bambu_temp_val[i], true);
    lv_obj_t *cap = bambu_label(chip, &lv_font_ui_12, COLOR_TEXT_MUTED, 12, 38, TEMP_NAMES[i]);
    lv_obj_set_style_text_letter_space(cap, 1, LV_PART_MAIN);
  }

  // ---- AMS: one 108 px page per unit, scrolled vertically (a horizontal
  // swipe still changes screens) and snapped page by page ----
  lv_obj_t *ams = lv_obj_create(scr);
  lv_obj_set_pos(ams, 22, 298);
  lv_obj_set_size(ams, 436, 108);
  lv_obj_set_style_bg_opa(ams, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ams, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ams, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(ams, 0, LV_PART_MAIN);
  lv_obj_set_scroll_dir(ams, LV_DIR_VER);
  lv_obj_set_scroll_snap_y(ams, LV_SCROLL_SNAP_START);
  lv_obj_set_scrollbar_mode(ams, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_add_flag(ams, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(ams, LV_OBJ_FLAG_SCROLL_ONE);
  for (int u = 0; u < BAMBU_MAX_AMS; u++)
  {
    lv_obj_t *page = bambu_box(ams, 0, u * 108, 436, 108, 0x000000, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_add_flag(page, LV_OBJ_FLAG_SNAPPABLE);
    bambu_ams_page[u] = page;
    bambu_ams_cap[u] = bambu_label(page, &lv_font_ui_12, COLOR_TEXT_MUTED, 0, 0, "AMS");
    lv_obj_set_style_text_letter_space(bambu_ams_cap[u], 1, LV_PART_MAIN);
    bambu_ams_env[u] = bambu_label(page, &lv_font_ui_12, COLOR_TEXT_MUTED, 0, 0, "");
    lv_obj_align(bambu_ams_env[u], LV_ALIGN_TOP_RIGHT, 0, 0);
    for (int i = 0; i < BAMBU_MAX_TRAYS; i++)
    {
      lv_obj_t *tile = bambu_box(page, i * 111, 20, 103, 86, 0x1E1E1E, 14);
      lv_obj_set_style_border_color(tile, COLOR_SECONDARY, LV_PART_MAIN);
      bambu_tray_tile[u][i] = tile;
      lv_obj_t *sw = bambu_box(tile, 10, 8, 22, 22, 0x000000, LV_RADIUS_CIRCLE);
      lv_obj_set_style_border_width(sw, 2, LV_PART_MAIN);
      lv_obj_set_style_border_color(sw, lv_color_hex(0x3A4652), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_MAIN);
      bambu_tray_sw[u][i] = sw;
      bambu_tray_mat[u][i] = bambu_label(tile, &lv_font_ui_14, COLOR_TEXT_MUTED, 10, 34, "Empty");
      bambu_tray_rem[u][i] = bambu_label(tile, &lv_font_ui_12, COLOR_TEXT_MUTED, 10, 52, "");
      lv_obj_set_width(bambu_tray_rem[u][i], 85);
      lv_label_set_long_mode(bambu_tray_rem[u][i], LV_LABEL_LONG_DOT);
      lv_obj_t *bar = lv_bar_create(tile);
      lv_obj_set_pos(bar, 10, 72);
      lv_obj_set_size(bar, 83, 4);
      lv_bar_set_range(bar, 0, 100);
      lv_obj_set_style_bg_color(bar, lv_color_hex(0x2E3540), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_radius(bar, 2, LV_PART_MAIN);
      lv_obj_set_style_radius(bar, 2, LV_PART_INDICATOR);
      lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
      bambu_tray_bar[u][i] = bar;
    }
    if (u > 0) lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
  }

  // ---- Fans, or the printer's warnings ----
  bambu_bottom_lab = bambu_label(scr, &lv_font_ui_12, COLOR_TEXT_MUTED, 22, 420, "");
  lv_obj_set_width(bambu_bottom_lab, 436);
  lv_label_set_long_mode(bambu_bottom_lab, LV_LABEL_LONG_DOT);
  lv_label_set_recolor(bambu_bottom_lab, true);

  return scr;
}

// Same construction as create_blackout_overlay(): opaque, never animated -
// see ESP32-4848S040.ino's screen_gesture_cb() for why. Green, and created
// after the blackout overlay so it draws on top if both were ever visible.
void create_finish_flash_overlay()
{
  lv_obj_t *layer = lv_disp_get_layer_top(NULL);
  finish_flash_overlay = lv_obj_create(layer);
  lv_obj_set_size(finish_flash_overlay, 480, 480);
  lv_obj_set_pos(finish_flash_overlay, 0, 0);
  lv_obj_set_style_bg_color(finish_flash_overlay, COLOR_SUCCESS, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(finish_flash_overlay, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(finish_flash_overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(finish_flash_overlay, 0, LV_PART_MAIN);
  lv_obj_clear_flag(finish_flash_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(finish_flash_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(finish_flash_overlay, LV_OBJ_FLAG_HIDDEN);
}

// 3 blinks (6 toggles) of the full screen, green, when a print finishes.
void finish_flash_timer_cb(lv_timer_t *timer)
{
  finish_flash_count++;
  if (finish_flash_count >= 6)
  {
    lv_obj_add_flag(finish_flash_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(timer);
    finish_flash_timer = NULL;
    return;
  }
  if (finish_flash_count % 2 == 1) lv_obj_clear_flag(finish_flash_overlay, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(finish_flash_overlay, LV_OBJ_FLAG_HIDDEN);
}

void start_finish_screen_flash()
{
  if (finish_flash_timer || !finish_flash_overlay) return;
  finish_flash_count = 0;
  finish_flash_timer = lv_timer_create(finish_flash_timer_cb, 250, NULL);
}

// When the printer reports more than one HMS warning at once, rotate the
// Bambu job label to show them one at a time, 5 seconds apart.
void bambu_warning_cycle_cb(lv_timer_t *timer)
{
  if (!bambu.has_warning || bambu.warning_count <= 1 || !bambu_bottom_lab) return;
  bambu_warning_display_idx = (bambu_warning_display_idx + 1) % bambu.warning_count;
  const char *w = bambu.warning_list[bambu_warning_display_idx];
  lv_label_set_text_fmt(bambu_bottom_lab, LV_SYMBOL_WARNING "  %s", w[0] ? w : "Printer reports an issue");
}

// 3 blinks of the Home badge to draw attention to a Bambu connection error;
// ends with the badge left visible in its (red) error state.
void bambu_badge_flash_cb(lv_timer_t *timer)
{
  bambu_badge_flash_count++;
  if (bambu_badge_flash_count >= 6)
  {
    if (home_bambu_badge) lv_obj_clear_flag(home_bambu_badge, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(timer);
    bambu_badge_flash_timer = NULL;
    return;
  }
  if (!home_bambu_badge) return;
  if (bambu_badge_flash_count % 2 == 1) lv_obj_clear_flag(home_bambu_badge, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(home_bambu_badge, LV_OBJ_FLAG_HIDDEN);
}

void start_bambu_badge_flash()
{
  if (bambu_badge_flash_timer || !home_bambu_badge) return;
  bambu_badge_flash_count = 0;
  bambu_badge_flash_timer = lv_timer_create(bambu_badge_flash_cb, 250, NULL);
}

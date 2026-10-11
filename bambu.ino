// ==================== Bambu Lab printer (local MQTT) ====================
// Split out of esp32_dashboard.ino: everything specific to the Bambu Lab
// printer widget - the local-MQTT client, HMS warning decoding, the
// print-finish flash/badge-flash effects, and the Bambu screen itself.
//
// This is an Arduino multi-tab sketch: all .ino files in this folder are
// concatenated into one translation unit at build time (main sketch first,
// then the rest alphabetically), so no #include of this file is needed and
// functions/globals defined here are visible from esp32_dashboard.ino and
// vice versa. A few pieces of Bambu state that esp32_dashboard.ino's
// loop()/network_task()/Wi-Fi portal touch directly (not just via a function
// call) - bambu_pname_lab, bambu_live_product_name, bambu_needs_ui_refresh,
// bambu_fail_count, bambu_gave_up - stay declared in esp32_dashboard.ino's
// top globals block so they're declared before those earlier use points.
// Set to 0 to silence every Serial log this file produces (MQTT connect
// status, HMS/field-parsing traces, etc.) - independent of the sketch-wide
// DEBUG flag in esp32_dashboard.ino, so Bambu's especially chatty MQTT
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
// declared in esp32_dashboard.ino - see the file header comment above.)
static lv_obj_t *bambu_ams_temp_val = NULL;
static lv_obj_t *bambu_ams_hum_val = NULL;
static lv_obj_t *bambu_job_lab = NULL;
static lv_obj_t *bambu_status_pill = NULL;
static lv_obj_t *bambu_status_pill_lab = NULL;
static lv_obj_t *bambu_bar = NULL;
static lv_obj_t *bambu_prog_lab = NULL;
static lv_obj_t *bambu_nozzle_val = NULL;
static lv_obj_t *bambu_bed_val = NULL;    // "55°C" - line1 of the BED/CHAMBER chip
static lv_obj_t *bambu_chamber_val = NULL; // "35°C" - line2 of the BED/CHAMBER chip
static lv_obj_t *bambu_layer_cur_val = NULL;
static lv_obj_t *bambu_layer_tot_val = NULL;
static lv_obj_t *bambu_slot_bar[BAMBU_MAX_TRAYS] = {NULL, NULL, NULL, NULL};
static lv_obj_t *bambu_slot_pct_lab[BAMBU_MAX_TRAYS] = {NULL, NULL, NULL, NULL};

// Fan status: [0]=cooling (part), [1]=aux (big_fan1), [2]=exhaust (big_fan2).
// Each is a hand-drawn 3-blade fan (hub + lv_line blades) "spinning" via a
// 3-frame swap (bambu_fan_spin_cb, a plain lv_timer show/hide cycle) rather
// than the transform/layer-compositing style path that broke full-screen
// objects on this board - a tiny icon's layer buffer would likely be safe
// too, but this reuses the pattern already proven to work (weather icons,
// blackout overlay) instead of risking that failure mode again.
static lv_obj_t *bambu_fan_hub[3] = {NULL, NULL, NULL};
static lv_obj_t *bambu_fan_blade[3][3] = {{NULL}}; // [fan][frame 0-2]
static bool bambu_fan_active[3] = {false, false, false};
static uint8_t bambu_fan_frame[3] = {0, 0, 0}; // per-fan - independent speeds need independent frames
static uint32_t bambu_fan_period_ms[3] = {200, 200, 200};
static uint32_t bambu_fan_accum_ms[3] = {0, 0, 0};
static lv_obj_t *bambu_fan_pct_lab[3] = {NULL, NULL, NULL};

// 6 visual speed tiers (100% gets an extra kick above the 81-99% tier) -
// costs nothing extra at runtime (the 50ms base timer tick is unchanged
// either way; this only affects a lookup done once per incoming MQTT update,
// not per tick).
uint32_t fan_spin_period_ms(int pct)
{
  if (pct <= 20) return 500;
  if (pct <= 40) return 350;
  if (pct <= 60) return 250;
  if (pct <= 80) return 180;
  if (pct < 100) return 120;
  return 80;
}

// 3 rotation frames (0, 40, 80) of a symmetric 3-blade fan - a 3-fold
// symmetric shape repeats every 120°, so 3 frames 40° apart is enough to
// simulate a full smooth rotation when cycled. Each frame is one polyline:
// center -> tip -> center -> tip -> center -> tip -> center (24x24 icon,
// center at 12,12). Shared across all 3 fan icons - identical geometry.
static lv_point_t fan_frame_pts[3][7] = {
    {{12, 12}, {12, 3}, {12, 12}, {20, 17}, {12, 12}, {4, 17}, {12, 12}},
    {{12, 12}, {18, 5}, {12, 12}, {15, 20}, {12, 12}, {3, 10}, {12, 12}},
    {{12, 12}, {21, 10}, {12, 12}, {9, 20}, {12, 12}, {6, 5}, {12, 12}},
};

// Runs on a fixed 50ms base tick (registered in setup()); each fan only
// actually advances its own blade once its own accumulated time reaches its
// own bambu_fan_period_ms - that's what lets fans spin at different visual
// speeds (see bambu_apply_to_ui(), which sets the period from the reported
// fan %) despite sharing one timer.
void bambu_fan_spin_cb(lv_timer_t *timer)
{
  const uint32_t tick_ms = 50;
  for (int f = 0; f < 3; f++)
  {
    if (!bambu_fan_active[f]) continue;
    bambu_fan_accum_ms[f] += tick_ms;
    if (bambu_fan_accum_ms[f] < bambu_fan_period_ms[f]) continue;
    bambu_fan_accum_ms[f] = 0;
    uint8_t next = (bambu_fan_frame[f] + 1) % 3;
    if (bambu_fan_blade[f][bambu_fan_frame[f]]) lv_obj_add_flag(bambu_fan_blade[f][bambu_fan_frame[f]], LV_OBJ_FLAG_HIDDEN);
    if (bambu_fan_blade[f][next]) lv_obj_clear_flag(bambu_fan_blade[f][next], LV_OBJ_FLAG_HIDDEN);
    bambu_fan_frame[f] = next;
  }
}

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
      lv_label_set_text(home_bambu_badge_lab, "PRINTING");
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
      lv_label_set_text(home_bambu_badge_lab, "PRINT FINISHED");
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
  if (strcmp(bambu.gcode_state, "FINISH") == 0 && was_printing_ish)
  {
    bambu_finish_badge_until_ms = millis() + BAMBU_FINISH_BADGE_MS;
    start_finish_screen_flash();
    // If we're showing the Bambu screen because this print auto-opened it,
    // don't wait on inactivity to leave - return to Home 5 min after finish.
    if (bambu_screen_auto_opened) bambu_auto_return_home_at_ms = millis() + BAMBU_AUTO_FINISH_RETURN_MS;
  }

  // Auto-open the Bambu screen on a genuine print start (setting-gated).
  // Same reboot guard as the finish badge above: requiring !was_printing_ish
  // AND percent < 1 rules out "we just reconnected mid-print" (which would
  // otherwise look identical to a fresh start on the very first message
  // after a reboot, since bambu_prev_gcode_state starts empty either way).
  // Suppressed while screen_locked, same as every other auto-navigate.
  bool now_printing_ish = strcmp(bambu.gcode_state, "RUNNING") == 0 || strcmp(bambu.gcode_state, "PREPARE") == 0;
  if (settings.auto_show_bambu_on_start && !was_printing_ish && now_printing_ish && bambu.percent < 1 &&
      !in_settings && current_screen != 1 && !screen_locked)
  {
    go_to_screen_blackout(1);
    bambu_screen_auto_opened = true;
    bambu_auto_return_home_at_ms = 0;
  }

  strncpy(bambu_prev_gcode_state, bambu.gcode_state, sizeof(bambu_prev_gcode_state) - 1);
  bambu_prev_gcode_state[sizeof(bambu_prev_gcode_state) - 1] = '\0';
  update_home_bambu_badge();

  if (bambu_pname_lab) lv_label_set_text(bambu_pname_lab, bambu_live_product_name[0] ? bambu_live_product_name : settings.printer_name);
  if (bambu_job_lab)
  {
    if (bambu.has_warning)
    {
      if (bambu_warning_display_idx >= bambu.warning_count) bambu_warning_display_idx = 0;
      const char *w = bambu.warning_count > 0 ? bambu.warning_list[bambu_warning_display_idx] : "printer reports an issue";
      lv_label_set_text_fmt(bambu_job_lab, "WARNING: %s", w[0] ? w : "printer reports an issue");
      lv_obj_set_style_text_color(bambu_job_lab, COLOR_WARNING, LV_PART_MAIN);
    }
    else
    {
      lv_label_set_text(bambu_job_lab, bambu.job_name[0] ? bambu.job_name : "No active job");
      lv_obj_set_style_text_color(bambu_job_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
    }
  }

  bool printing = strcmp(bambu.gcode_state, "RUNNING") == 0 || strcmp(bambu.gcode_state, "PREPARE") == 0;
  if (bambu_status_pill) lv_obj_set_style_bg_color(bambu_status_pill, printing ? COLOR_SUCCESS : COLOR_TEXT_MUTED, LV_PART_MAIN);
  if (bambu_status_pill_lab) lv_label_set_text(bambu_status_pill_lab, bambu_state_label(bambu.gcode_state));

  if (bambu_bar) lv_bar_set_value(bambu_bar, bambu.percent, LV_ANIM_OFF);
  if (bambu_prog_lab)
  {
    // Chamber temp now has its own home in the BED/CHAMBER chip below (see
    // bambu_bed_val), so it's no longer repeated here.
    lv_label_set_text_fmt(bambu_prog_lab, "%d%% complete  -  %dh %dm left",
                           bambu.percent, bambu.remain_time_min / 60, bambu.remain_time_min % 60);
  }
  if (bambu_nozzle_val) lv_label_set_text_fmt(bambu_nozzle_val, "%.0f\xC2\xB0" "C", bambu.nozzle_temp);
  if (bambu_bed_val) lv_label_set_text_fmt(bambu_bed_val, "%.0f\xC2\xB0" "C", bambu.bed_temp);
  if (bambu_chamber_val) lv_label_set_text_fmt(bambu_chamber_val, "%.0f\xC2\xB0" "C", bambu.chamber_temp);
  if (bambu_layer_cur_val) lv_label_set_text_fmt(bambu_layer_cur_val, "%d", bambu.layer_num);
  if (bambu_layer_tot_val) lv_label_set_text_fmt(bambu_layer_tot_val, "%d", bambu.total_layer_num);

  int fan_raw[3] = {bambu.cooling_fan_speed, bambu.big_fan1_speed, bambu.big_fan2_speed};
  for (int i = 0; i < 3; i++)
  {
    int pct = fan_percent(fan_raw[i]);
    bool on = pct > 0;
    bambu_fan_period_ms[i] = fan_spin_period_ms(pct);
    if (on != bambu_fan_active[i])
    {
      bambu_fan_active[i] = on;
      // Blades stay visible either way (a fan should still look like a fan
      // when stopped) - only the color changes; bambu_fan_spin_cb already
      // only advances the frame for active fans, so an inactive one just
      // freezes on whichever frame it's currently showing.
      lv_color_t fan_color = on ? COLOR_PRIMARY : COLOR_TEXT_MUTED;
      for (int fr = 0; fr < 3; fr++)
      {
        if (!bambu_fan_blade[i][fr]) continue;
        bool show = (fr == bambu_fan_frame[i]);
        (show ? lv_obj_clear_flag : lv_obj_add_flag)(bambu_fan_blade[i][fr], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_line_color(bambu_fan_blade[i][fr], fan_color, LV_PART_MAIN);
      }
      if (bambu_fan_hub[i]) lv_obj_set_style_bg_color(bambu_fan_hub[i], on ? COLOR_PRIMARY : COLOR_TEXT_MUTED, LV_PART_MAIN);
    }
    if (bambu_fan_pct_lab[i])
    {
      if (pct < 0) lv_label_set_text(bambu_fan_pct_lab[i], "--");
      else lv_label_set_text_fmt(bambu_fan_pct_lab[i], "%d%%", pct);
    }
  }

  if (bambu.amsCount > 0)
  {
    if (bambu_ams_temp_val) lv_label_set_text_fmt(bambu_ams_temp_val, "%.0f\xC2\xB0" "C", bambu.ams[0].temp);
    if (bambu_ams_hum_val) lv_label_set_text_fmt(bambu_ams_hum_val, "%d%%", bambu.ams[0].humidity);
  }
  else
  {
    if (bambu_ams_temp_val) lv_label_set_text(bambu_ams_temp_val, "--\xC2\xB0" "C");
    if (bambu_ams_hum_val) lv_label_set_text(bambu_ams_hum_val, "--%");
  }

  int trayCount = bambu.amsCount > 0 ? bambu.ams[0].trayCount : 0;
  for (int i = 0; i < BAMBU_MAX_TRAYS; i++)
  {
    if (i >= trayCount) continue; // no live data for this slot yet - leave placeholder as-is
    BambuTray &t = bambu.ams[0].trays[i];
    int pct = t.remain >= 0 ? t.remain : 0;
    if (bambu_slot_bar[i])
    {
      lv_bar_set_value(bambu_slot_bar[i], pct, LV_ANIM_OFF);
      lv_color_t tray_col;
      // Real filament color from the printer, not the fixed placeholder set
      // at screen creation - falls back to the placeholder if this tray
      // hasn't reported a color yet (e.g. empty slot).
      if (parse_hex_color(t.tray_color, &tray_col)) lv_obj_set_style_bg_color(bambu_slot_bar[i], tray_col, LV_PART_INDICATOR);
    }
    if (bambu_slot_pct_lab[i])
    {
      if (t.remain >= 0) lv_label_set_text_fmt(bambu_slot_pct_lab[i], "%d%%", t.remain);
      else lv_label_set_text(bambu_slot_pct_lab[i], "--");
    }
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
lv_obj_t *create_bambu_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // ---------------- Bambu printer card ----------------
  lv_obj_t *pcard = lv_obj_create(scr);
  // 472, not the full 480: keeps a thin 4px margin so the card doesn't sit
  // flush against the physical screen edge, while still maximizing the
  // width available to the chip row below.
  lv_obj_set_size(pcard, 472, 436);
  lv_obj_align(pcard, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_opa(pcard, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(pcard, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(pcard, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_all(pcard, 18, LV_PART_MAIN);
  lv_obj_clear_flag(pcard, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *pname = lv_label_create(pcard);
  lv_label_set_text(pname, settings.printer_name);
  lv_obj_set_style_text_color(pname, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(pname, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_align(pname, LV_ALIGN_TOP_LEFT, 0, 2);
  bambu_pname_lab = pname;

  lv_obj_t *pstatus_pill_lab = NULL;
  lv_obj_t *pstatus_pill = make_pill(pcard, "PRINTING", COLOR_SUCCESS, lv_color_hex(0x0B2E13), &pstatus_pill_lab);
  lv_obj_align(pstatus_pill, LV_ALIGN_TOP_RIGHT, 0, 0);
  bambu_status_pill = pstatus_pill;
  bambu_status_pill_lab = pstatus_pill_lab;

  lv_obj_t *job = lv_label_create(pcard);
  lv_label_set_text(job, "Benchy_v2.gcode");
  lv_obj_set_style_text_color(job, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(job, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(job, LV_ALIGN_TOP_LEFT, 0, 40);
  bambu_job_lab = job;

  lv_obj_t *bar = lv_bar_create(pcard);
  lv_obj_set_size(bar, 436, 24);
  lv_bar_set_range(bar, 0, 100);
  lv_bar_set_value(bar, 75, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_radius(bar, 12, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, COLOR_SECONDARY, LV_PART_INDICATOR);
  lv_obj_set_style_radius(bar, 12, LV_PART_INDICATOR);
  lv_obj_align(bar, LV_ALIGN_TOP_LEFT, 0, 80);
  bambu_bar = bar;

  lv_obj_t *prog = lv_label_create(pcard);
  lv_label_set_text(prog, "75% complete  -  1h 24m left");
  lv_obj_set_style_text_color(prog, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(prog, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(prog, LV_ALIGN_TOP_LEFT, 0, 110);
  bambu_prog_lab = prog;

  lv_obj_t *temps = lv_obj_create(pcard);
  lv_obj_set_size(temps, 436, 74);
  lv_obj_align(temps, LV_ALIGN_TOP_LEFT, 0, 142);
  lv_obj_set_style_bg_opa(temps, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(temps, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(temps, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(temps, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(temps, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(temps, LV_OBJ_FLAG_SCROLLABLE);

  // Chip widths are sized to each caption's actual rendered width (the
  // widest text in that chip) rather than split evenly - "BED/CHAMBER" at
  // font 12 needs ~102px on its own, while the others need well under 100.
  make_stat_chip(temps, "230°C", "NOZZLE", COLOR_SECONDARY, &bambu_nozzle_val, 80, &lv_font_montserrat_22, LV_FLEX_ALIGN_END);
  make_stat_chip2(temps, "55°C", "35°C", "BED/CHAMBER", COLOR_SECONDARY, &bambu_bed_val, &bambu_chamber_val, 140);
  make_stat_chip2(temps, "142", "320", "LAYER", COLOR_PRIMARY, &bambu_layer_cur_val, &bambu_layer_tot_val, 80);
  make_stat_chip2(temps, "--\xC2\xB0" "C", "--%", "AMS", COLOR_PRIMARY, &bambu_ams_temp_val, &bambu_ams_hum_val, 90);

  // ---- Fans: cooling / aux / exhaust, each a hand-drawn 3-blade fan icon
  // (hub + 3 blades in 3 cycled frames - see bambu_fan_spin_cb) + a percentage
  // label. Blades start hidden; bambu_apply_to_ui() reveals frame 0 for
  // whichever fans are actually reporting > 0.
  static const char *fan_names[3] = {"COOLING", "AUX", "EXHAUST"};
  for (int i = 0; i < 3; i++)
  {
    lv_coord_t fan_x = i * 140;

    lv_obj_t *fan_icon = lv_obj_create(pcard);
    lv_obj_set_size(fan_icon, 24, 24);
    lv_obj_set_pos(fan_icon, fan_x, 222);
    lv_obj_set_style_bg_opa(fan_icon, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(fan_icon, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(fan_icon, 0, LV_PART_MAIN);
    lv_obj_clear_flag(fan_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(fan_icon, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hub = lv_obj_create(fan_icon);
    lv_obj_set_size(hub, 6, 6);
    lv_obj_set_pos(hub, 9, 9);
    lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(hub, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_border_width(hub, 0, LV_PART_MAIN);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(hub, LV_OBJ_FLAG_CLICKABLE);
    bambu_fan_hub[i] = hub;

    for (int fr = 0; fr < 3; fr++)
    {
      lv_obj_t *blade = lv_line_create(fan_icon);
      lv_line_set_points(blade, fan_frame_pts[fr], 7);
      // Gray to match the hub's default "off" look - apply logic recolors
      // both together once real data arrives.
      lv_obj_set_style_line_color(blade, COLOR_TEXT_MUTED, LV_PART_MAIN);
      lv_obj_set_style_line_width(blade, 2, LV_PART_MAIN);
      lv_obj_set_style_line_rounded(blade, true, LV_PART_MAIN);
      lv_obj_add_flag(blade, LV_OBJ_FLAG_HIDDEN);
      bambu_fan_blade[i][fr] = blade;
    }
    // Always show one static frame (even before we know the fan's state) -
    // a fan should always look like a fan, just motionless when off.
    lv_obj_clear_flag(bambu_fan_blade[i][0], LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *fname = lv_label_create(pcard);
    lv_label_set_text(fname, fan_names[i]);
    lv_obj_set_style_text_color(fname, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(fname, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(fname, fan_x + 30, 224);

    lv_obj_t *fpct = lv_label_create(pcard);
    lv_label_set_text(fpct, "--");
    lv_obj_set_style_text_color(fpct, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(fpct, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_pos(fpct, fan_x + 30, 240);
    bambu_fan_pct_lab[i] = fpct;
  }

  lv_obj_t *fdiv = lv_obj_create(pcard);
  lv_obj_set_size(fdiv, 436, 1);
  lv_obj_set_style_bg_color(fdiv, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_border_width(fdiv, 0, LV_PART_MAIN);
  lv_obj_align(fdiv, LV_ALIGN_TOP_LEFT, 0, 262);
  lv_obj_clear_flag(fdiv, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *ams_lab = lv_label_create(pcard);
  lv_label_set_text(ams_lab, "AMS");
  lv_obj_set_style_text_color(ams_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(ams_lab, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(ams_lab, 1, LV_PART_MAIN);
  lv_obj_align(ams_lab, LV_ALIGN_TOP_LEFT, 0, 274);

  // Four AMS filament slots, each a two-color progress line: filled portion
  // in the filament's color (real color once live data arrives - see
  // parse_hex_color()), remainder in a neutral track with a white outline.
  lv_color_t spool_colors[4] = {
      lv_color_hex(0xFF7A00), // PLA Basic - Orange
      lv_color_hex(0xECEFF1), // PETG Basic - White
      lv_color_hex(0x2979FF), // TPU 95A - Blue
      lv_color_hex(0x9C27B0), // PLA Basic - Purple
  };
  const int spool_percent[4] = {68, 15, 90, 42};

  for (int i = 0; i < 4; i++)
  {
    lv_coord_t row_y = 296 + i * 24;

    lv_obj_t *slot_bar = lv_bar_create(pcard);
    lv_obj_set_size(slot_bar, 368, 8);
    lv_obj_set_pos(slot_bar, 0, row_y);
    lv_bar_set_range(slot_bar, 0, 100);
    lv_bar_set_value(slot_bar, spool_percent[i], LV_ANIM_OFF);
    // White track (not dark gray) so a dark/black filament's fill still
    // shows up against the empty portion - a dark fill on a dark track was
    // effectively invisible.
    lv_obj_set_style_bg_color(slot_bar, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_radius(slot_bar, 4, LV_PART_MAIN);
    // Outer outline in a neutral gray (not white, since the track itself is
    // now white and a white border on white would vanish).
    lv_obj_set_style_border_width(slot_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(slot_bar, COLOR_TEXT_MUTED, LV_PART_MAIN);
    // Inset the fill so it doesn't draw over the border - without this the
    // indicator extends edge-to-edge and hides the outline entirely.
    lv_obj_set_style_pad_all(slot_bar, 1, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slot_bar, spool_colors[i], LV_PART_INDICATOR);
    lv_obj_set_style_radius(slot_bar, 3, LV_PART_INDICATOR);
    // Dark outline on the fill itself too - insurance for the opposite case
    // (a white/light filament would otherwise blend into the white track).
    lv_obj_set_style_border_width(slot_bar, 1, LV_PART_INDICATOR);
    lv_obj_set_style_border_color(slot_bar, lv_color_hex(0x000000), LV_PART_INDICATOR);

    lv_obj_t *pct = lv_label_create(pcard);
    lv_label_set_text_fmt(pct, "%d%%", spool_percent[i]);
    lv_obj_set_style_text_color(pct, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(pct, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(pct, 382, row_y - 3);

    bambu_slot_bar[i] = slot_bar;
    bambu_slot_pct_lab[i] = pct;
  }

  return scr;
}

// Same construction as create_blackout_overlay(): opaque, never animated -
// see esp32_dashboard.ino's screen_gesture_cb() for why. Green, and created
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
  if (!bambu.has_warning || bambu.warning_count <= 1 || !bambu_job_lab) return;
  bambu_warning_display_idx = (bambu_warning_display_idx + 1) % bambu.warning_count;
  const char *w = bambu.warning_list[bambu_warning_display_idx];
  lv_label_set_text_fmt(bambu_job_lab, "WARNING: %s", w[0] ? w : "printer reports an issue");
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

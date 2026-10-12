// ==================== Alerts ====================
// A colored banner across the top of every screen for things worth noticing
// right away: a package delivered (tracking.ino), a Proxmox node going
// offline (proxmox.ino), a failed print (bambu.ino). It blinks a few times,
// stays for a minute, or as long as a /notify asks (a tap dismisses it) and lights the screen up -
// except at night (see night_now()), when it waits quietly for a touch.
// Opaque and only ever shown/hidden, never faded (see blackout_overlay).

#define ALERT_BLINKS 6

static lv_obj_t *alert_banner = NULL;
static lv_obj_t *alert_lab = NULL;
static uint32_t alert_until_ms = 0;
static uint8_t alert_blinks_left = 0;

void alert_dismiss_cb(lv_event_t *e)
{
  alert_until_ms = 0;
  alert_blinks_left = 0;
  lv_obj_add_flag(alert_banner, LV_OBJ_FLAG_HIDDEN);
}

void create_alert_banner()
{
  alert_banner = lv_obj_create(lv_disp_get_layer_top(NULL));
  lv_obj_set_size(alert_banner, 440, 56);
  lv_obj_align(alert_banner, LV_ALIGN_TOP_MID, 0, 56); // below the corner hotspots' top strip
  lv_obj_set_style_border_width(alert_banner, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(alert_banner, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(alert_banner, 16, LV_PART_MAIN);
  lv_obj_clear_flag(alert_banner, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(alert_banner, alert_dismiss_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(alert_banner, LV_OBJ_FLAG_HIDDEN);

  alert_lab = lv_label_create(alert_banner);
  lv_obj_set_width(alert_lab, 408);
  lv_label_set_long_mode(alert_lab, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(alert_lab, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(alert_lab, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_center(alert_lab);
}

// LVGL thread only (the *_apply_to_ui() functions call it).
void alert_show(const char *text, lv_color_t color, uint32_t show_ms)
{
  if (!alert_banner) return;
  Serial.printf("[Alert] %s\n", text);
  lv_label_set_text(alert_lab, text);
  lv_obj_set_style_bg_color(alert_banner, color, LV_PART_MAIN);
  lv_obj_clear_flag(alert_banner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(alert_banner);
  alert_until_ms = millis() + show_ms;
  alert_blinks_left = ALERT_BLINKS;
  if (!night_now())
  {
    last_activity_ms = millis(); // wakes a dimmed screen / screensaver
    backlight_update();
    screensaver_update();
  }
}

// Every second from clock_tick_cb(): blink, then expire.
void alert_tick()
{
  if (!alert_until_ms) return;
  if (alert_blinks_left)
  {
    alert_blinks_left--;
    (alert_blinks_left % 2 ? lv_obj_add_flag : lv_obj_clear_flag)(alert_banner, LV_OBJ_FLAG_HIDDEN);
  }
  if ((int32_t)(alert_until_ms - millis()) <= 0) alert_dismiss_cb(NULL);
}

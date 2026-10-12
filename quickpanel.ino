// ==================== Quick panel ====================
// Swipe down from the top edge (see my_touchpad_read()): brightness, pin,
// "dim now", Settings, and the Wi-Fi details. Closes on a tap outside it
// or after QP_SHOW_MS. Opaque, only shown/hidden (no fades - see
// blackout_overlay).

#define QP_SHOW_MS (15UL * 1000UL)

static lv_obj_t *qp_backdrop = NULL;
static lv_obj_t *qp_slider = NULL;
static lv_obj_t *qp_level_lab = NULL;
static lv_obj_t *qp_pin_lab = NULL;
static lv_obj_t *qp_wifi_lab = NULL;
static uint32_t qp_until_ms = 0;
static bool qp_open_pending = false;

void quick_panel_hide()
{
  qp_until_ms = 0;
  if (qp_backdrop) lv_obj_add_flag(qp_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void qp_refresh()
{
  lv_slider_set_value(qp_slider, settings.day_level, LV_ANIM_OFF);
  lv_label_set_text_fmt(qp_level_lab, "%d%%", settings.day_level);
  lv_label_set_text(qp_pin_lab, screen_pinned ? tr(S_UNPIN) : tr(S_PIN));
  if (wifi_signal_ui_connected)
    lv_label_set_text_fmt(qp_wifi_lab, LV_SYMBOL_WIFI "  %s  \xE2\x80\xA2  %s  \xE2\x80\xA2  %d dBm", WiFi.SSID().c_str(),
                          WiFi.localIP().toString().c_str(), (int)wifi_signal_rssi_dbm);
  else
    lv_label_set_text_fmt(qp_wifi_lab, LV_SYMBOL_WIFI "  %s", tr(S_NOT_CONNECTED));
}

// From the touch handler: opened on the next clock tick, after the finger is up.
void quick_panel_request()
{
  if (in_settings || ota_in_progress) return;
  qp_open_pending = true;
}

// Every second from clock_tick_cb().
void quick_panel_tick()
{
  if (qp_open_pending)
  {
    qp_open_pending = false;
    nav_menu_hide();
    qp_refresh();
    lv_obj_clear_flag(qp_backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(qp_backdrop);
    qp_until_ms = millis() + QP_SHOW_MS;
  }
  else if (qp_until_ms && (int32_t)(qp_until_ms - millis()) <= 0) quick_panel_hide();
}

static void qp_keep_open() { qp_until_ms = millis() + QP_SHOW_MS; }

void qp_backdrop_cb(lv_event_t *e)
{
  if (lv_event_get_target(e) == qp_backdrop) quick_panel_hide(); // a tap outside the card
}

void qp_slider_cb(lv_event_t *e)
{
  qp_keep_open();
  settings.day_level = (uint8_t)lv_slider_get_value(qp_slider);
  lv_label_set_text_fmt(qp_level_lab, "%d%%", settings.day_level);
  backlight_set(settings.day_level);
  if (lv_event_get_code(e) == LV_EVENT_RELEASED) save_settings();
}

void qp_pin_cb(lv_event_t *e)
{
  pin_set(!screen_pinned);
  quick_panel_hide();
}

void qp_dim_cb(lv_event_t *e)
{
  quick_panel_hide();
  dim_forced = true;
  dim_forced_at = millis();
  backlight_update();
}

void qp_settings_cb(lv_event_t *e)
{
  quick_panel_hide();
  open_settings_cb(NULL);
}

static lv_obj_t *qp_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, lv_obj_t **label_out)
{
  lv_obj_t *btn = lv_btn_create(parent);
  lv_obj_set_size(btn, 128, 52);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x232D38), LV_PART_MAIN);
  lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(btn, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *lab = lv_label_create(btn);
  lv_label_set_text(lab, text);
  lv_obj_set_style_text_font(lab, &lv_font_ui_16, LV_PART_MAIN);
  lv_obj_center(lab);
  if (label_out) *label_out = lab;
  return btn;
}

void create_quick_panel()
{
  qp_backdrop = lv_obj_create(lv_disp_get_layer_top(NULL));
  lv_obj_set_size(qp_backdrop, 480, 480);
  lv_obj_set_pos(qp_backdrop, 0, 0);
  lv_obj_set_style_bg_opa(qp_backdrop, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(qp_backdrop, 0, LV_PART_MAIN);
  lv_obj_clear_flag(qp_backdrop, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(qp_backdrop, qp_backdrop_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(qp_backdrop, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *card = lv_obj_create(qp_backdrop);
  lv_obj_set_size(card, 448, 236);
  lv_obj_align(card, LV_ALIGN_TOP_MID, 0, 8);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x10181F), LV_PART_MAIN);
  lv_obj_set_style_border_color(card, lv_color_hex(0x34424F), LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  // Brightness.
  lv_obj_t *sun = lv_label_create(card);
  lv_label_set_text(sun, LV_SYMBOL_EYE_OPEN);
  lv_obj_set_style_text_color(sun, COLOR_WARNING, LV_PART_MAIN);
  lv_obj_align(sun, LV_ALIGN_TOP_LEFT, 0, 6);

  qp_slider = lv_slider_create(card);
  lv_obj_set_size(qp_slider, 290, 14);
  lv_obj_align(qp_slider, LV_ALIGN_TOP_LEFT, 40, 8);
  lv_slider_set_range(qp_slider, 5, 100);
  lv_obj_set_style_bg_color(qp_slider, COLOR_PRIMARY, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(qp_slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_ext_click_area(qp_slider, 16);
  lv_obj_add_event_cb(qp_slider, qp_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(qp_slider, qp_slider_cb, LV_EVENT_RELEASED, NULL);

  qp_level_lab = lv_label_create(card);
  lv_obj_set_style_text_color(qp_level_lab, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(qp_level_lab, &lv_font_ui_16, LV_PART_MAIN);
  lv_obj_align(qp_level_lab, LV_ALIGN_TOP_RIGHT, 0, 4);

  // Buttons.
  lv_obj_t *pin_btn = qp_button(card, tr(S_PIN), qp_pin_cb, &qp_pin_lab);
  lv_obj_align(pin_btn, LV_ALIGN_TOP_LEFT, 0, 54);
  lv_obj_t *dim_btn = qp_button(card, tr(S_DIM_NOW), qp_dim_cb, NULL);
  lv_obj_align(dim_btn, LV_ALIGN_TOP_MID, 0, 54);
  lv_obj_t *set_btn = qp_button(card, tr(S_SETTINGS), qp_settings_cb, NULL);
  lv_obj_align(set_btn, LV_ALIGN_TOP_RIGHT, 0, 54);

  // Wi-Fi details.
  qp_wifi_lab = lv_label_create(card);
  lv_obj_set_width(qp_wifi_lab, 410);
  lv_label_set_long_mode(qp_wifi_lab, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_color(qp_wifi_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(qp_wifi_lab, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_align(qp_wifi_lab, LV_ALIGN_TOP_LEFT, 0, 132);

  lv_obj_t *hint = lv_label_create(card);
  lv_label_set_text(hint, tr(S_SWIPE_HINT));
  lv_obj_set_style_text_color(hint, lv_color_hex(0x55616B), LV_PART_MAIN);
  lv_obj_set_style_text_font(hint, &lv_font_ui_12, LV_PART_MAIN);
  lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, 0);
}

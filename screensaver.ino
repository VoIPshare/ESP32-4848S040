// ==================== Night clock screensaver ====================
// While the screen is dimmed at night (see backlight_update()) and
// settings.night_clock is on, a big dim clock on black replaces the
// dashboard - readable from bed without lighting up the room. It shifts a
// little every minute. The backlight never goes below SAVER_MIN_LEVEL while
// it shows (see backlight_update()). The first touch only wakes the
// dashboard (it's swallowed in my_touchpad_read(), like a touch on a
// switched-off screen).

static lv_obj_t *saver = NULL;
static lv_obj_t *saver_time = NULL;
static lv_obj_t *saver_date = NULL;
static uint8_t saver_shown_min = 255;

void create_screensaver()
{
  saver = lv_obj_create(lv_disp_get_layer_top(NULL));
  lv_obj_set_size(saver, 480, 480);
  lv_obj_set_pos(saver, 0, 0);
  lv_obj_set_style_bg_color(saver, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(saver, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(saver, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(saver, 0, LV_PART_MAIN);
  lv_obj_clear_flag(saver, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(saver, LV_OBJ_FLAG_HIDDEN);

  saver_time = lv_label_create(saver);
  lv_label_set_text(saver_time, "");
  lv_obj_set_style_text_font(saver_time, &lv_font_roboto_bold_136, LV_PART_MAIN);
  // Full-strength colors: the low night backlight already makes it dim.
  lv_obj_set_style_text_color(saver_time, lv_color_hex(0xFFFFFF), LV_PART_MAIN);

  saver_date = lv_label_create(saver);
  lv_label_set_text(saver_date, "");
  lv_obj_set_style_text_font(saver_date, &lv_font_montserrat_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(saver_date, lv_color_hex(0xB0BEC5), LV_PART_MAIN);
}

bool screensaver_visible()
{
  return saver && !lv_obj_has_flag(saver, LV_OBJ_FLAG_HIDDEN);
}

static void screensaver_draw()
{
  saver_shown_min = clock_min;
  lv_label_set_text_fmt(saver_time, "%02d:%02d", clock_hour, clock_min);
  lv_label_set_text_fmt(saver_date, "%s, %s %d", WEEKDAY_NAMES[clock_wday % 7], MONTH_NAMES[clock_mon % 12], clock_mday);
  // A small, different offset each minute.
  lv_coord_t dx = (lv_coord_t)((clock_min * 37) % 61) - 30;
  lv_coord_t dy = (lv_coord_t)((clock_min * 53) % 81) - 40;
  lv_obj_align(saver_time, LV_ALIGN_CENTER, dx, dy - 20);
  lv_obj_align_to(saver_date, saver_time, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);
}

// From backlight_update() (every second, and on a touch or alert).
void screensaver_update()
{
  if (!saver) return;
  bool want = settings.night_clock && settings.night_level > 0 && night_now() &&
              millis() - last_activity_ms > NIGHT_DIM_IDLE_MS && !ota_in_progress;
  if (want && !screensaver_visible())
  {
    screensaver_draw();
    lv_obj_clear_flag(saver, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(saver);
  }
  else if (!want && screensaver_visible())
    lv_obj_add_flag(saver, LV_OBJ_FLAG_HIDDEN);
  else if (want && saver_shown_min != clock_min)
    screensaver_draw();
}

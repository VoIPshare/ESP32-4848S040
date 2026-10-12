// ==================== Forecast page ====================
// Detail page opened by tapping the weather on Home: the next 12 hours
// (every 2nd hour) and the next 5 days, from the same Open-Meteo call as
// the Home weather (fetch_weather() fills weather_data.hourly/daily).
// Back button / top-left corner return to Home - see open_subscreen().

static lv_obj_t *forecast_screen = NULL;
static lv_obj_t *fc_hour_time[6], *fc_hour_icon[6], *fc_hour_temp[6], *fc_hour_pop[6];
static lv_obj_t *fc_day_row[5], *fc_day_name[5], *fc_day_icon[5], *fc_day_pop[5], *fc_day_temps[5];
static lv_obj_t *fc_empty_lab = NULL;

// MDI weather icon for a WMO weather code.
const lv_img_dsc_t *wx_icon_for(int code, bool is_day)
{
  if (code <= 1) return is_day ? &wx_icon_sunny : &wx_icon_night;
  if (code == 2) return &wx_icon_partly;
  if (code == 3) return &wx_icon_cloudy;
  if (code == 45 || code == 48) return &wx_icon_fog;
  if (code == 65 || code == 67 || code == 82) return &wx_icon_pouring;
  if ((code >= 71 && code <= 77) || code == 85 || code == 86) return &wx_icon_snow;
  if (code >= 95) return &wx_icon_storm;
  if (code >= 51) return &wx_icon_rain;
  return &wx_icon_cloudy;
}

static lv_color_t wx_icon_color(int code)
{
  if (code <= 2) return COLOR_WARNING;              // sun
  if (code >= 95) return COLOR_SECONDARY;           // storm
  if (code >= 51) return COLOR_PRIMARY;             // rain / snow
  return lv_color_hex(0xB0BEC5);                    // clouds, fog
}

static lv_obj_t *fc_icon(lv_obj_t *parent)
{
  lv_obj_t *img = lv_img_create(parent);
  lv_img_set_src(img, &wx_icon_cloudy);
  lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_img_recolor(img, lv_color_hex(0xB0BEC5), LV_PART_MAIN);
  return img;
}

static lv_obj_t *fc_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color)
{
  lv_obj_t *lab = lv_label_create(parent);
  lv_label_set_text(lab, "");
  lv_obj_set_style_text_font(lab, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(lab, color, LV_PART_MAIN);
  return lab;
}

void forecast_apply_to_ui()
{
  if (!forecast_screen) return;
  data_lock();
  int nh = weather_data.hourly_count, nd = weather_data.daily_count;
  if (fc_empty_lab) (nh || nd ? lv_obj_add_flag : lv_obj_clear_flag)(fc_empty_lab, LV_OBJ_FLAG_HIDDEN);

  for (int c = 0; c < 6; c++)
  {
    int i = c * 2; // every 2nd hour
    bool have = i < nh;
    if (!have)
    {
      lv_label_set_text(fc_hour_time[c], "");
      lv_label_set_text(fc_hour_temp[c], "");
      lv_label_set_text(fc_hour_pop[c], "");
      lv_obj_add_flag(fc_hour_icon[c], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    auto &h = weather_data.hourly[i];
    lv_label_set_text_fmt(fc_hour_time[c], "%02d:00", h.hour);
    lv_obj_clear_flag(fc_hour_icon[c], LV_OBJ_FLAG_HIDDEN);
    lv_img_set_src(fc_hour_icon[c], wx_icon_for(h.wcode, h.is_day));
    lv_obj_set_style_img_recolor(fc_hour_icon[c], wx_icon_color(h.wcode), LV_PART_MAIN);
    lv_label_set_text_fmt(fc_hour_temp[c], "%.0f\xC2\xB0", h.temp);
    if (h.pop >= 10) lv_label_set_text_fmt(fc_hour_pop[c], "%d%%", h.pop);
    else lv_label_set_text(fc_hour_pop[c], "");
  }

  // Days: skip today (daily[0]) - Home already shows its high/low.
  for (int r = 0; r < 5; r++)
  {
    int i = r + 1;
    if (i >= nd)
    {
      lv_obj_add_flag(fc_day_row[r], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(fc_day_row[r], LV_OBJ_FLAG_HIDDEN);
    auto &d = weather_data.daily[i];
    lv_label_set_text(fc_day_name[r], r == 0 ? "Tomorrow" : WEEKDAY_NAMES[d.wday % 7]);
    lv_img_set_src(fc_day_icon[r], wx_icon_for(d.wcode, true));
    lv_obj_set_style_img_recolor(fc_day_icon[r], wx_icon_color(d.wcode), LV_PART_MAIN);
    if (d.pop >= 10) lv_label_set_text_fmt(fc_day_pop[r], "%d%%", d.pop);
    else lv_label_set_text(fc_day_pop[r], "");
    lv_label_set_text_fmt(fc_day_temps[r], "%.0f\xC2\xB0  /  %.0f\xC2\xB0", d.tmax, d.tmin);
  }
  data_unlock();
}

void open_forecast_screen()
{
  if (!forecast_screen || screen_pinned) return;
  forecast_apply_to_ui();
  open_subscreen(forecast_screen);
}

void home_weather_clicked_cb(lv_event_t *e)
{
  open_forecast_screen();
}

lv_obj_t *create_forecast_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  forecast_screen = scr;

  make_back_button(scr);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Forecast");
  lv_obj_set_style_text_color(title, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

  fc_empty_lab = fc_label(scr, &lv_font_montserrat_16, COLOR_TEXT_MUTED);
  lv_label_set_text(fc_empty_lab, "No forecast yet");
  lv_obj_align(fc_empty_lab, LV_ALIGN_CENTER, 0, 0);

  // Next hours: one card, 6 columns.
  lv_obj_t *hours = lv_obj_create(scr);
  lv_obj_set_size(hours, 448, 132);
  lv_obj_align(hours, LV_ALIGN_TOP_MID, 0, 76);
  lv_obj_set_style_bg_color(hours, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(hours, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(hours, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(hours, 0, LV_PART_MAIN);
  lv_obj_clear_flag(hours, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(hours, LV_OBJ_FLAG_GESTURE_BUBBLE);
  for (int c = 0; c < 6; c++)
  {
    lv_coord_t cx = 8 + c * 72 + 36; // column center
    fc_hour_time[c] = fc_label(hours, &lv_font_montserrat_14, COLOR_TEXT_MUTED);
    lv_obj_align(fc_hour_time[c], LV_ALIGN_TOP_MID, cx - 224, 10);
    fc_hour_icon[c] = fc_icon(hours);
    lv_obj_align(fc_hour_icon[c], LV_ALIGN_TOP_MID, cx - 224, 34);
    fc_hour_temp[c] = fc_label(hours, &lv_font_montserrat_20, lv_color_hex(0xFFFFFF));
    lv_obj_align(fc_hour_temp[c], LV_ALIGN_TOP_MID, cx - 224, 70);
    fc_hour_pop[c] = fc_label(hours, &lv_font_montserrat_12, COLOR_PRIMARY);
    lv_obj_align(fc_hour_pop[c], LV_ALIGN_TOP_MID, cx - 224, 100);
  }

  // Next days.
  for (int r = 0; r < 5; r++)
  {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_size(row, 448, 44);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 220 + r * 50);
    lv_obj_set_style_bg_color(row, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    fc_day_row[r] = row;

    fc_day_name[r] = fc_label(row, &lv_font_montserrat_16, lv_color_hex(0xFFFFFF));
    lv_obj_align(fc_day_name[r], LV_ALIGN_LEFT_MID, 16, 0);
    fc_day_icon[r] = fc_icon(row);
    lv_obj_align(fc_day_icon[r], LV_ALIGN_LEFT_MID, 150, 0);
    fc_day_pop[r] = fc_label(row, &lv_font_montserrat_14, COLOR_PRIMARY);
    lv_obj_align(fc_day_pop[r], LV_ALIGN_LEFT_MID, 196, 0);
    fc_day_temps[r] = fc_label(row, &lv_font_montserrat_18, lv_color_hex(0xFFFFFF));
    lv_obj_align(fc_day_temps[r], LV_ALIGN_RIGHT_MID, -16, 0);
  }

  forecast_apply_to_ui();
  return scr;
}

// ==================== Package tracking ====================
// Parcels from a PKGE Business account (business.pkge.net), one row each:
// the last 8 characters of the tracking number, a colored status pill, and
// the estimated delivery date when the carrier gives one. Packages are
// added on the PKGE side - nothing is typed on the device. Delivered
// parcels stay for 1 day, then drop off (see df_tracking_drop_expired()).
//
// tracking[] is filled by web_fetch.ino (fetch on network_task, copied in
// under data_lock()), and tracking_apply_to_ui() is called from loop()
// whenever tracking_needs_ui_refresh is set. Like the other tabs, this is
// concatenated into the main sketch, so no #include is needed.

static TrackingItem tracking[TRACKING_MAX];
static int tracking_count = 0;
static bool tracking_fetch_attempted = false;
static bool tracking_fetch_ok = false;

static lv_obj_t *tracking_status_lab = NULL;
static lv_obj_t *tracking_row[TRACKING_MAX] = {NULL};
static lv_obj_t *tracking_number_lab[TRACKING_MAX] = {NULL};
static lv_obj_t *tracking_eta_lab[TRACKING_MAX] = {NULL};
static lv_obj_t *tracking_pill[TRACKING_MAX] = {NULL};
static lv_obj_t *tracking_pill_lab[TRACKING_MAX] = {NULL};

static const lv_coord_t TRACKING_ROW_H = 62;
static const lv_coord_t TRACKING_ROW_GAP = 8;
static const lv_coord_t TRACKING_LIST_TOP = 70;

const char *tracking_status_label(int status)
{
  switch (status)
  {
    case -1: case 0: case 1: return "Checking";
    case 2: return "No info yet";
    case 3: return "In transit";
    case 4: return "At pickup point";
    case 5: return "Delivered";
    case 6: return "Not delivered";
    case 7: return "Delivery error";
    case 8: return "Preparing";
    case 9: return "Route ended";
    default: return "Unknown";
  }
}

lv_color_t tracking_status_color(int status)
{
  if (status == 5) return COLOR_SUCCESS;
  if (status == 3 || status == 8) return COLOR_PRIMARY;
  if (status == 4) return COLOR_SECONDARY;
  if (status == 6 || status == 7) return lv_color_hex(0xEF5350);
  return COLOR_TEXT_MUTED;
}

void tracking_apply_to_ui()
{
  data_lock();

  if (tracking_status_lab)
  {
    if (!settings.pkge_api_key[0])
      lv_label_set_text(tracking_status_lab, "No PKGE API key - set it on the Wi-Fi setup page");
    else if (!tracking_fetch_attempted)
      lv_label_set_text(tracking_status_lab, "Fetching packages...");
    else if (!tracking_fetch_ok && tracking_count == 0)
      lv_label_set_text(tracking_status_lab, "Fetch failed - check the API key and connection");
    else if (tracking_count == 0)
      lv_label_set_text(tracking_status_lab, "No packages being tracked");
    else
      lv_label_set_text(tracking_status_lab, "");
  }

  for (int i = 0; i < TRACKING_MAX; i++)
  {
    if (!tracking_row[i]) continue;
    if (i >= tracking_count)
    {
      lv_obj_add_flag(tracking_row[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(tracking_row[i], LV_OBJ_FLAG_HIDDEN);
    const TrackingItem &t = tracking[i];

    // The last 8 characters are enough to recognize a parcel.
    size_t len = strlen(t.track_number);
    lv_label_set_text(tracking_number_lab[i], len > 8 ? t.track_number + len - 8 : t.track_number);

    char eta[40] = "";
    if (t.status != 5 && t.est_delivery > 0)
    {
      struct tm et;
      localtime_r(&t.est_delivery, &et);
      strftime(eta, sizeof(eta), "ETA %a %b %d", &et);
    }
    else if (t.last_date > 0)
    {
      struct tm lt;
      localtime_r(&t.last_date, &lt);
      strftime(eta, sizeof(eta), "Updated %b %d, %H:%M", &lt);
    }
    lv_label_set_text(tracking_eta_lab[i], eta);

    lv_label_set_text(tracking_pill_lab[i], tracking_status_label(t.status));
    lv_obj_set_style_bg_color(tracking_pill[i], tracking_status_color(t.status), LV_PART_MAIN);
  }

  data_unlock();
}

// ==================== Screen 7: Packages ====================
lv_obj_t *create_tracking_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Packages");
  lv_obj_set_style_text_color(title, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  tracking_status_lab = lv_label_create(scr);
  lv_label_set_text(tracking_status_lab, "");
  lv_obj_set_style_text_color(tracking_status_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(tracking_status_lab, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_width(tracking_status_lab, 420);
  lv_obj_set_style_text_align(tracking_status_lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(tracking_status_lab, LV_ALIGN_TOP_MID, 0, 200);

  for (int i = 0; i < TRACKING_MAX; i++)
  {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_size(row, 440, TRACKING_ROW_H);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, TRACKING_LIST_TOP + i * (TRACKING_ROW_H + TRACKING_ROW_GAP));
    lv_obj_set_style_bg_color(row, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    tracking_row[i] = row;

    lv_obj_t *num = lv_label_create(row);
    lv_label_set_text(num, "");
    lv_obj_set_style_text_color(num, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(num, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_align(num, LV_ALIGN_TOP_LEFT, 16, 9);
    tracking_number_lab[i] = num;

    lv_obj_t *eta = lv_label_create(row);
    lv_label_set_text(eta, "");
    lv_obj_set_style_text_color(eta, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(eta, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_align(eta, LV_ALIGN_BOTTOM_LEFT, 16, -9);
    tracking_eta_lab[i] = eta;

    lv_obj_t *pill_lab = NULL;
    lv_obj_t *pill = make_pill(row, "", COLOR_TEXT_MUTED, lv_color_hex(0x10181F), &pill_lab);
    lv_obj_align(pill, LV_ALIGN_RIGHT_MID, -14, 0);
    tracking_pill[i] = pill;
    tracking_pill_lab[i] = pill_lab;
  }

  tracking_apply_to_ui();

  return scr;
}

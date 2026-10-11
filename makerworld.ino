// ==================== MakerWorld design stats ====================
// Renders aggregate engagement stats (likes/downloads/prints/collections/
// boosts) for a MakerWorld design. This used to fetch MakerWorld's raw
// design-service API directly on-device - gone now, since that endpoint
// bundles full per-instance metadata alongside the numbers we actually
// want and routinely runs 500KB-900KB+ per fetch, far more than fits in
// ESP32 heap. makerworld_stats is instead populated by webapp_data.ino's
// single shared fetch of a Google Apps Script endpoint that already sums
// the totals server-side. This file only knows how to render
// makerworld_stats, not fetch it.
//
// This is an Arduino multi-tab sketch: all .ino files in this folder are
// concatenated into one translation unit at build time, so no #include of
// this file is needed. makerworld_needs_ui_refresh lives in
// esp32_dashboard.ino's top globals block instead of here, since
// network_task()/loop() there read/write it directly and that file is
// always compiled first - see its comment.
struct MakerWorldStats
{
  long likes = 0;
  long downloads = 0;
  long prints = 0;
  long collections = 0;
  long boosts = 0;
  bool valid = false;
};
static MakerWorldStats makerworld_stats;

static lv_obj_t *makerworld_status_lab = NULL;
static lv_obj_t *makerworld_likes_val = NULL;
static lv_obj_t *makerworld_downloads_val = NULL;
static lv_obj_t *makerworld_prints_val = NULL;
static lv_obj_t *makerworld_collections_val = NULL;
static lv_obj_t *makerworld_boosts_val = NULL;
// Distinguishes "haven't tried fetching yet" from "tried and it failed" -
// without this, the status label had no way to tell those apart and a
// design ID that was actually saved could sit stuck on the create-time
// placeholder text ("No design ID set...") until a fetch happened to succeed.
static bool makerworld_fetch_attempted = false;

// Abbreviates large counts (12345 -> "12.3K", 2500000 -> "2.5M") so they fit
// make_stat_chip's ~132px-wide value line.
void format_stat_count(long n, char *buf, size_t buflen)
{
  if (n >= 1000000) snprintf(buf, buflen, "%.1fM", n / 1000000.0f);
  else if (n >= 1000) snprintf(buf, buflen, "%.1fK", n / 1000.0f);
  else snprintf(buf, buflen, "%ld", n);
}

void makerworld_apply_to_ui()
{
  if (makerworld_status_lab)
  {
    if (!settings.dashboard_script_id[0])
      lv_label_set_text(makerworld_status_lab, "No Dashboard Data API configured - set it on the Wi-Fi setup page");
    else if (!makerworld_fetch_attempted)
      lv_label_set_text(makerworld_status_lab, "Fetching MakerWorld stats...");
    else if (!makerworld_stats.valid)
      lv_label_set_text(makerworld_status_lab, "Fetch failed - check connection");
    else
      lv_label_set_text(makerworld_status_lab, "");
  }

  if (!makerworld_stats.valid) return;

  char buf[16];
  format_stat_count(makerworld_stats.likes, buf, sizeof(buf));
  if (makerworld_likes_val) lv_label_set_text(makerworld_likes_val, buf);
  format_stat_count(makerworld_stats.downloads, buf, sizeof(buf));
  if (makerworld_downloads_val) lv_label_set_text(makerworld_downloads_val, buf);
  format_stat_count(makerworld_stats.prints, buf, sizeof(buf));
  if (makerworld_prints_val) lv_label_set_text(makerworld_prints_val, buf);
  format_stat_count(makerworld_stats.collections, buf, sizeof(buf));
  if (makerworld_collections_val) lv_label_set_text(makerworld_collections_val, buf);
  format_stat_count(makerworld_stats.boosts, buf, sizeof(buf));
  if (makerworld_boosts_val) lv_label_set_text(makerworld_boosts_val, buf);
}

// ==================== Screen 7: MakerWorld ====================
lv_obj_t *create_makerworld_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "MakerWorld");
  lv_obj_set_style_text_color(title, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  makerworld_status_lab = lv_label_create(scr);
  lv_label_set_text(makerworld_status_lab, "No Dashboard Data API configured - set it on the Wi-Fi setup page");
  lv_obj_set_style_text_color(makerworld_status_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(makerworld_status_lab, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_width(makerworld_status_lab, 420);
  lv_obj_set_style_text_align(makerworld_status_lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(makerworld_status_lab, LV_ALIGN_TOP_MID, 0, 60);

  // Row 1: Likes / Downloads / Prints - same 440px space-between row used
  // for the Home screen's weather stats.
  lv_obj_t *row1 = lv_obj_create(scr);
  lv_obj_set_size(row1, 440, 90);
  lv_obj_align(row1, LV_ALIGN_TOP_MID, 0, 140);
  lv_obj_set_style_bg_opa(row1, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row1, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row1, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(row1, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row1, LV_OBJ_FLAG_SCROLLABLE);

  make_stat_chip(row1, "--", "LIKES", COLOR_PRIMARY, &makerworld_likes_val);
  make_stat_chip(row1, "--", "DOWNLOADS", COLOR_PRIMARY, &makerworld_downloads_val);
  make_stat_chip(row1, "--", "PRINTS", COLOR_PRIMARY, &makerworld_prints_val);

  // Row 2: Collections / Boosts - only 2 chips, so centered with a fixed gap
  // rather than space-between (which would stretch too wide with just 2).
  lv_obj_t *row2 = lv_obj_create(scr);
  lv_obj_set_size(row2, 440, 90);
  lv_obj_align(row2, LV_ALIGN_TOP_MID, 0, 250);
  lv_obj_set_style_bg_opa(row2, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row2, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row2, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(row2, 40, LV_PART_MAIN);
  lv_obj_set_flex_flow(row2, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row2, LV_OBJ_FLAG_SCROLLABLE);

  make_stat_chip(row2, "--", "COLLECTIONS", COLOR_SECONDARY, &makerworld_collections_val, 150);
  make_stat_chip(row2, "--", "BOOSTS", COLOR_SECONDARY, &makerworld_boosts_val, 150);

  // Syncs the status label with whatever's actually in settings right now -
  // without this the label stayed on its hardcoded creation-time text
  // ("No design ID set...") until a fetch happened to complete, even if a
  // design ID had already been loaded from saved settings.
  makerworld_apply_to_ui();

  return scr;
}

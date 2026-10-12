// ==================== MakerWorld design stats ====================
// Renders aggregate engagement stats (likes/downloads/prints/collections/
// boosts) summed over every published design of a MakerWorld user, how much
// each grew today, the profile's followers/level and the most downloaded
// designs. makerworld_stats is filled by web_fetch.ino: df_fetch_makerworld()
// in direct_fetch.cpp streams Bambu's design-service answer (~1 MB) through a
// JSON filter that keeps only the counters and titles, so it never has to fit
// in RAM. This file only knows how to render makerworld_stats, not fetch it.
//
// This is an Arduino multi-tab sketch: all .ino files in this folder are
// concatenated into one translation unit at build time, so no #include of
// this file is needed. makerworld_needs_ui_refresh lives in
// ESP32-4848S040.ino's top globals block instead of here, since
// network_task()/loop() there read/write it directly and that file is
// always compiled first - see its comment.
enum { MW_LIKES, MW_DOWNLOADS, MW_PRINTS, MW_COLLECTIONS, MW_BOOSTS, MW_COUNTER_COUNT };

struct MakerWorldStats
{
  long counter[MW_COUNTER_COUNT] = {0, 0, 0, 0, 0};
  long today[MW_COUNTER_COUNT] = {0, 0, 0, 0, 0}; // growth since local midnight
  bool today_valid = false;                       // false until the clock is set
  int designs = 0;
  long followers = -1; // -1: profile not read (yet)
  int level = 0;
  int top_count = 0;
  char top_title[MW_TOP_DESIGNS][MW_TITLE_MAX];
  long top_downloads[MW_TOP_DESIGNS];
  long top_likes[MW_TOP_DESIGNS];
  char updated[6] = ""; // "HH:MM" of the last good fetch
  bool valid = false;
};
static MakerWorldStats makerworld_stats;

static lv_obj_t *makerworld_status_lab = NULL;
static lv_obj_t *makerworld_profile_lab = NULL;
static lv_obj_t *makerworld_val[MW_COUNTER_COUNT] = {NULL};
static lv_obj_t *makerworld_delta[MW_COUNTER_COUNT] = {NULL};
static lv_obj_t *makerworld_top_row[MW_TOP_DESIGNS] = {NULL};
static lv_obj_t *makerworld_top_title[MW_TOP_DESIGNS] = {NULL};
static lv_obj_t *makerworld_top_count[MW_TOP_DESIGNS] = {NULL};
// Distinguishes "haven't tried fetching yet" from "tried and it failed" -
// without this, the status label had no way to tell those apart and a
// user ID that was actually saved could sit stuck on the create-time
// placeholder text until a fetch happened to succeed.
static bool makerworld_fetch_attempted = false;

// Abbreviates large counts (12345 -> "12.3K", 2500000 -> "2.5M") so they fit
// make_stat_chip's ~132px-wide value line.
void format_stat_count(long n, char *buf, size_t buflen)
{
  if (n >= 1000000) snprintf(buf, buflen, "%.1fM", n / 1000000.0f);
  else if (n >= 1000) snprintf(buf, buflen, "%.1fK", n / 1000.0f);
  else snprintf(buf, buflen, "%ld", n);
}

// Today's growth: the first good fetch of each local day becomes the
// baseline (kept in NVS, so a reboot doesn't reset it), and every later
// fetch shows the difference. Runs on network_task (under data_lock(),
// after the new totals are in makerworld_stats) - no LVGL here.
void makerworld_update_today()
{
  MakerWorldStats *s = &makerworld_stats;
  struct tm now;
  time_t t = time(nullptr);
  localtime_r(&t, &now);
  if (now.tm_year < 120) { s->today_valid = false; return; } // clock not set yet
  snprintf(s->updated, sizeof(s->updated), "%02d:%02d", now.tm_hour, now.tm_min);

  static long base[MW_COUNTER_COUNT];
  static int32_t base_day = -1; // year * 1000 + day of year
  int32_t day = (now.tm_year + 1900) * 1000 + now.tm_yday;

  Preferences p;
  if (base_day < 0 && p.begin("mwday", true))
  {
    base_day = p.getInt("day", -1);
    p.getBytes("base", base, sizeof(base));
    p.end();
  }
  if (base_day != day)
  {
    base_day = day;
    memcpy(base, s->counter, sizeof(base));
    if (p.begin("mwday", false))
    {
      p.putInt("day", day);
      p.putBytes("base", base, sizeof(base));
      p.end();
    }
  }
  for (int i = 0; i < MW_COUNTER_COUNT; i++)
    s->today[i] = s->counter[i] > base[i] ? s->counter[i] - base[i] : 0;
  s->today_valid = true;
}

void makerworld_apply_to_ui()
{
  if (makerworld_status_lab)
  {
    if (!settings.makerworld_uid[0])
      lv_label_set_text(makerworld_status_lab, "No MakerWorld user ID - set it on the Wi-Fi setup page");
    else if (!makerworld_fetch_attempted)
      lv_label_set_text(makerworld_status_lab, "Fetching MakerWorld stats...");
    else if (!makerworld_stats.valid)
      lv_label_set_text(makerworld_status_lab, "Fetch failed - check connection");
    else if (makerworld_stats.updated[0])
      lv_label_set_text_fmt(makerworld_status_lab, "Updated %s", makerworld_stats.updated);
    else
      lv_label_set_text(makerworld_status_lab, "");
  }

  if (!makerworld_stats.valid) return;

  char buf[24];
  for (int i = 0; i < MW_COUNTER_COUNT; i++)
  {
    format_stat_count(makerworld_stats.counter[i], buf, sizeof(buf));
    if (makerworld_val[i]) lv_label_set_text(makerworld_val[i], buf);
    if (!makerworld_delta[i]) continue;
    long d = makerworld_stats.today[i];
    if (!makerworld_stats.today_valid)
      lv_label_set_text(makerworld_delta[i], "");
    else
    {
      format_stat_count(d, buf, sizeof(buf));
      lv_label_set_text_fmt(makerworld_delta[i], d > 0 ? "+%s today" : "%s today", buf);
    }
    lv_obj_set_style_text_color(makerworld_delta[i], d > 0 ? COLOR_SUCCESS : COLOR_TEXT_MUTED, LV_PART_MAIN);
  }

  if (makerworld_profile_lab)
  {
    char line[96];
    int len = snprintf(line, sizeof(line), "%d designs", makerworld_stats.designs);
    if (makerworld_stats.followers >= 0)
    {
      format_stat_count(makerworld_stats.followers, buf, sizeof(buf));
      len += snprintf(line + len, sizeof(line) - len, "  \xE2\x80\xA2  %s followers", buf);
    }
    if (makerworld_stats.level > 0)
      snprintf(line + len, sizeof(line) - len, "  \xE2\x80\xA2  Level %d", makerworld_stats.level);
    lv_label_set_text(makerworld_profile_lab, line);
  }

  for (int i = 0; i < MW_TOP_DESIGNS; i++)
  {
    if (!makerworld_top_row[i]) continue;
    if (i >= makerworld_stats.top_count)
    {
      lv_obj_add_flag(makerworld_top_row[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(makerworld_top_row[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(makerworld_top_title[i], makerworld_stats.top_title[i]);
    format_stat_count(makerworld_stats.top_downloads[i], buf, sizeof(buf));
    lv_label_set_text_fmt(makerworld_top_count[i], LV_SYMBOL_DOWNLOAD " %s", buf);
  }
}

// One counter card: big value, caption, and today's growth under it.
static void mw_make_counter(lv_obj_t *parent, const char *caption, lv_color_t color, int idx, lv_coord_t w)
{
  lv_obj_t *chip = lv_obj_create(parent);
  lv_obj_set_size(chip, w, 92);
  lv_obj_set_style_bg_color(chip, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(chip, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(chip, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(chip, 2, LV_PART_MAIN);
  lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *val = lv_label_create(chip);
  lv_label_set_text(val, "--");
  lv_obj_set_style_text_color(val, color, LV_PART_MAIN);
  lv_obj_set_style_text_font(val, &lv_font_ui_26, LV_PART_MAIN);
  makerworld_val[idx] = val;

  lv_obj_t *lab = lv_label_create(chip);
  lv_label_set_text(lab, caption);
  lv_obj_set_style_text_color(lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(lab, &lv_font_ui_12, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(lab, 1, LV_PART_MAIN);

  lv_obj_t *delta = lv_label_create(chip);
  lv_label_set_text(delta, "");
  lv_obj_set_style_text_color(delta, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(delta, &lv_font_ui_12, LV_PART_MAIN);
  makerworld_delta[idx] = delta;
}

static lv_obj_t *mw_make_row(lv_obj_t *scr, lv_coord_t y, lv_coord_t h)
{
  lv_obj_t *row = lv_obj_create(scr);
  lv_obj_set_size(row, 440, h);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, y);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  return row;
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
  lv_obj_set_style_text_font(title, &lv_font_ui_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

  makerworld_profile_lab = lv_label_create(scr);
  lv_label_set_text(makerworld_profile_lab, "");
  lv_obj_set_style_text_color(makerworld_profile_lab, lv_color_hex(0xB0BEC5), LV_PART_MAIN);
  lv_obj_set_style_text_font(makerworld_profile_lab, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_align(makerworld_profile_lab, LV_ALIGN_TOP_MID, 0, 54);

  makerworld_status_lab = lv_label_create(scr);
  lv_label_set_text(makerworld_status_lab, "");
  lv_obj_set_style_text_color(makerworld_status_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(makerworld_status_lab, &lv_font_ui_12, LV_PART_MAIN);
  lv_obj_set_width(makerworld_status_lab, 420);
  lv_obj_set_style_text_align(makerworld_status_lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(makerworld_status_lab, LV_ALIGN_TOP_MID, 0, 74);

  // Row 1: Likes / Downloads / Prints. Row 2: Collections / Boosts.
  lv_obj_t *row1 = mw_make_row(scr, 98, 92);
  mw_make_counter(row1, "LIKES", COLOR_PRIMARY, MW_LIKES, 140);
  mw_make_counter(row1, "DOWNLOADS", COLOR_PRIMARY, MW_DOWNLOADS, 140);
  mw_make_counter(row1, "PRINTS", COLOR_PRIMARY, MW_PRINTS, 140);

  lv_obj_t *row2 = mw_make_row(scr, 198, 92);
  mw_make_counter(row2, "COLLECTIONS", COLOR_SECONDARY, MW_COLLECTIONS, 216);
  mw_make_counter(row2, "BOOSTS", COLOR_SECONDARY, MW_BOOSTS, 216);

  // Most downloaded designs.
  lv_obj_t *top_hdr = lv_label_create(scr);
  lv_label_set_text(top_hdr, "TOP DESIGNS");
  lv_obj_set_style_text_color(top_hdr, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(top_hdr, &lv_font_ui_12, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(top_hdr, 1, LV_PART_MAIN);
  lv_obj_set_pos(top_hdr, 24, 304);

  static const uint32_t rank_colors[MW_TOP_DESIGNS] = {0xFFC107, 0xB0BEC5, 0xCD7F32}; // gold, silver, bronze
  for (int i = 0; i < MW_TOP_DESIGNS; i++)
  {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_size(row, 440, 36);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 324 + i * 40);
    lv_obj_set_style_bg_color(row, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN); // shown once there's data
    makerworld_top_row[i] = row;

    lv_obj_t *rank = lv_label_create(row);
    lv_label_set_text_fmt(rank, "%d", i + 1);
    lv_obj_set_style_text_color(rank, lv_color_hex(rank_colors[i]), LV_PART_MAIN);
    lv_obj_set_style_text_font(rank, &lv_font_ui_18, LV_PART_MAIN);
    lv_obj_align(rank, LV_ALIGN_LEFT_MID, 14, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, "");
    lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(name, &lv_font_ui_14, LV_PART_MAIN);
    lv_obj_set_width(name, 300);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 40, 0);
    makerworld_top_title[i] = name;

    lv_obj_t *count = lv_label_create(row);
    lv_label_set_text(count, "");
    lv_obj_set_style_text_color(count, COLOR_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_text_font(count, &lv_font_ui_14, LV_PART_MAIN);
    lv_obj_align(count, LV_ALIGN_RIGHT_MID, -14, 0);
    makerworld_top_count[i] = count;
  }

  // Syncs the status label with whatever's actually in settings right now -
  // without this the label stayed on its hardcoded creation-time text
  // until a fetch happened to complete, even if a user ID had already been
  // loaded from saved settings.
  makerworld_apply_to_ui();

  return scr;
}

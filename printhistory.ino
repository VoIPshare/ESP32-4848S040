// ==================== Bambu print history ====================
// The last PH_MAX prints (name, finished or failed, when, how long), kept
// in NVS so they survive restarts. Recorded by bambu_apply_to_ui() on a
// real RUNNING -> FINISH/FAILED transition; shown on a detail page opened
// by tapping the printer name on the Bambu screen.

#define PH_MAX 6

struct PrintRecord
{
  char name[40];
  uint32_t end;     // time_t, local clock
  uint16_t minutes; // 0: unknown (we didn't see it start)
  uint8_t ok;
  uint8_t used;
};
static PrintRecord ph[PH_MAX];
static bool ph_loaded = false;
static time_t ph_start = 0; // when the current print started, if we saw it

static lv_obj_t *ph_screen = NULL;
static lv_obj_t *ph_list = NULL;

static void ph_load()
{
  if (ph_loaded) return;
  ph_loaded = true;
  memset(ph, 0, sizeof(ph));
  Preferences p;
  if (p.begin("printhist", true))
  {
    if (p.getBytesLength("list") == sizeof(ph)) p.getBytes("list", ph, sizeof(ph));
    p.end();
  }
}

// Every printer report: notes when a print starts.
void ph_note_state(const char *prev, const char *now)
{
  bool was = !strcmp(prev, "RUNNING") || !strcmp(prev, "PREPARE") || !strcmp(prev, "PAUSE");
  bool is = !strcmp(now, "RUNNING") || !strcmp(now, "PREPARE");
  if (!was && is && prev[0]) ph_start = time(nullptr); // prev empty = just booted mid-print
}

void ph_record(const char *name, bool ok)
{
  ph_load();
  memmove(&ph[1], &ph[0], sizeof(PrintRecord) * (PH_MAX - 1));
  PrintRecord &r = ph[0];
  memset(&r, 0, sizeof(r));
  strlcpy(r.name, name[0] ? name : "Print", sizeof(r.name));
  time_t now = time(nullptr);
  r.end = (uint32_t)now;
  r.minutes = ph_start > 0 && now > ph_start ? (uint16_t)min<time_t>((now - ph_start) / 60, 65535) : 0;
  r.ok = ok;
  r.used = 1;
  ph_start = 0;
  Preferences p;
  if (p.begin("printhist", false))
  {
    p.putBytes("list", ph, sizeof(ph));
    p.end();
  }
}

static void ph_fill()
{
  ph_load();
  lv_obj_clean(ph_list);
  int shown = 0;
  for (int i = 0; i < PH_MAX; i++)
  {
    if (!ph[i].used) continue;
    shown++;
    lv_obj_t *row = lv_obj_create(ph_list);
    lv_obj_set_size(row, 440, 58);
    lv_obj_set_style_bg_color(row, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *mark = lv_label_create(row);
    lv_label_set_text(mark, ph[i].ok ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(mark, ph[i].ok ? COLOR_SUCCESS : lv_color_hex(0xEF5350), LV_PART_MAIN);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 14, 0);

    lv_obj_t *name = lv_label_create(row);
    lv_label_set_text(name, ph[i].name);
    lv_obj_set_width(name, 380);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(name, &lv_font_ui_16, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 44, 8);

    char when[48];
    time_t t = (time_t)ph[i].end;
    struct tm lt;
    localtime_r(&t, &lt);
    char dm[24];
    format_day_month(dm, sizeof(dm), lt.tm_mon, lt.tm_mday);
    int n = snprintf(when, sizeof(when), "%s, %02d:%02d", dm, lt.tm_hour, lt.tm_min);
    if (ph[i].minutes)
      snprintf(when + n, sizeof(when) - n, "  \xE2\x80\xA2  %dh %02dm", ph[i].minutes / 60, ph[i].minutes % 60);
    if (!ph[i].ok)
    {
      strlcat(when, "  \xE2\x80\xA2  ", sizeof(when));
      strlcat(when, tr(S_FAILED), sizeof(when));
    }
    lv_obj_t *sub = lv_label_create(row);
    lv_label_set_text(sub, when);
    lv_obj_set_style_text_color(sub, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(sub, &lv_font_ui_14, LV_PART_MAIN);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 44, -8);
  }
  if (!shown)
  {
    lv_obj_t *lab = lv_label_create(ph_list);
    lv_label_set_text(lab, tr(S_NO_PRINTS));
    lv_obj_set_style_text_color(lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(lab, &lv_font_ui_16, LV_PART_MAIN);
  }
}

void open_print_history(lv_event_t *e)
{
  if (!ph_screen || screen_pinned) return;
  ph_fill();
  open_subscreen(ph_screen);
}

lv_obj_t *create_print_history_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  ph_screen = scr;

  make_back_button(scr);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, tr(S_PRINT_HISTORY));
  lv_obj_set_style_text_color(title, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_ui_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

  ph_list = lv_obj_create(scr);
  lv_obj_set_size(ph_list, 456, 396);
  lv_obj_align(ph_list, LV_ALIGN_TOP_MID, 0, 76);
  lv_obj_set_style_bg_opa(ph_list, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(ph_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ph_list, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_left(ph_list, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(ph_list, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(ph_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_scroll_dir(ph_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(ph_list, LV_SCROLLBAR_MODE_OFF);
  return scr;
}

// ==================== Home: the humidity / wind / UV slot ====================
// The stats row under the weather shares its place with two cards:
//  - an incoming call (asterisk.ino), for CALL_SHOW_MS, ahead of everything;
//  - what's playing on Jellyfin (jellyfin.ino) and new voicemails
//    (asterisk.ino), taking turns with the stats every SLOT_SWAP_MS while
//    there's something to show.
// Only one of the three shows at a time; weather_slot_tick() picks it.
// With "Big caller ID" (setup page) a ringing call covers the whole
// weather panel instead: the number as big as the clock, the name above it.
// (Named weather_slot so it's merged after asterisk.ino and jellyfin.ino.)

#define CALL_SHOW_MS (15UL * 1000UL)
#define SLOT_SWAP_MS (20UL * 1000UL)

static lv_obj_t *home_call_card = NULL, *home_call_name = NULL, *home_call_sub = NULL;
static lv_obj_t *home_media_card = NULL, *home_media_icon = NULL, *home_media_title = NULL, *home_media_who = NULL,
                *home_media_pct = NULL, *home_media_bar = NULL;
static lv_obj_t *home_vm_card = NULL, *home_vm_count = NULL, *home_vm_from = NULL;
static lv_obj_t *home_call_big = NULL, *home_call_big_name = NULL, *home_call_big_num = NULL;
static uint32_t call_shown_ms = 0;
static bool call_showing = false;
static bool media_playing = false;
static bool vm_waiting = false;

// A rounded card in the stats row's spot (stats: 80 px tall, 56 px above
// the panel's bottom; the card is centered on the same line).
static lv_obj_t *slot_card(lv_obj_t *parent, lv_color_t bg)
{
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_size(card, 440, 66);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -63);
  lv_obj_set_style_bg_color(card, bg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
  return card;
}

static lv_obj_t *slot_label(lv_obj_t *card, const lv_font_t *font, lv_color_t color, lv_coord_t x, lv_coord_t y, lv_coord_t w)
{
  lv_obj_t *lab = lv_label_create(card);
  lv_label_set_text(lab, "");
  lv_obj_set_style_text_font(lab, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(lab, color, LV_PART_MAIN);
  lv_obj_set_width(lab, w);
  lv_label_set_long_mode(lab, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(lab, x, y);
  return lab;
}

static void weather_slot_tick(lv_timer_t *t);

// From create_home_screen(), right after the stats row.
void build_home_slot_cards(lv_obj_t *parent)
{
  // Incoming call: phone icon, the name (or number) big, the number under it.
  home_call_card = slot_card(parent, lv_color_hex(0x1B3A20));
  lv_obj_t *phone = lv_label_create(home_call_card);
  lv_label_set_text(phone, LV_SYMBOL_CALL);
  lv_obj_set_style_text_font(phone, &lv_font_ui_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(phone, COLOR_SUCCESS, LV_PART_MAIN);
  lv_obj_align(phone, LV_ALIGN_LEFT_MID, 18, 0);
  home_call_name = slot_label(home_call_card, &lv_font_ui_24, lv_color_hex(0xFFFFFF), 58, 6, 366);
  home_call_sub = slot_label(home_call_card, &lv_font_ui_14, lv_color_hex(0xA5D6A7), 58, 38, 366);

  // Jellyfin: play/pause icon, title, "user - device", progress.
  home_media_card = slot_card(parent, lv_color_hex(0x232D38));
  home_media_icon = lv_label_create(home_media_card);
  lv_obj_set_style_text_font(home_media_icon, &lv_font_ui_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(home_media_icon, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_align(home_media_icon, LV_ALIGN_LEFT_MID, 18, -2);
  home_media_title = slot_label(home_media_card, &lv_font_ui_20, lv_color_hex(0xFFFFFF), 52, 7, 372);
  home_media_who = slot_label(home_media_card, &lv_font_ui_14, COLOR_TEXT_MUTED, 52, 35, 310);
  home_media_pct = lv_label_create(home_media_card);
  lv_obj_set_style_text_font(home_media_pct, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(home_media_pct, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_align(home_media_pct, LV_ALIGN_TOP_RIGHT, -16, 35);
  home_media_bar = lv_bar_create(home_media_card);
  lv_obj_set_size(home_media_bar, 408, 3);
  lv_obj_align(home_media_bar, LV_ALIGN_BOTTOM_MID, 0, -5);
  lv_bar_set_range(home_media_bar, 0, 100);
  lv_obj_set_style_bg_color(home_media_bar, lv_color_hex(0x3A4652), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(home_media_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(home_media_bar, COLOR_PRIMARY, LV_PART_INDICATOR);
  lv_obj_set_style_radius(home_media_bar, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(home_media_bar, 2, LV_PART_INDICATOR);

  // Voicemail: envelope, "2 new voicemails", who left the last one and when.
  home_vm_card = slot_card(parent, lv_color_hex(0x3A2A12));
  lv_obj_t *env = lv_label_create(home_vm_card);
  lv_label_set_text(env, LV_SYMBOL_ENVELOPE);
  lv_obj_set_style_text_font(env, &lv_font_ui_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(env, COLOR_SECONDARY, LV_PART_MAIN);
  lv_obj_align(env, LV_ALIGN_LEFT_MID, 18, 0);
  home_vm_count = slot_label(home_vm_card, &lv_font_ui_22, lv_color_hex(0xFFFFFF), 58, 7, 366);
  home_vm_from = slot_label(home_vm_card, &lv_font_ui_14, lv_color_hex(0xFFCC80), 58, 38, 366);

  // Big caller ID: the whole weather panel, everything centered.
  home_call_big = lv_obj_create(parent);
  lv_obj_set_size(home_call_big, 480, 300);
  lv_obj_set_pos(home_call_big, 0, 0);
  lv_obj_set_style_bg_color(home_call_big, lv_color_hex(0x1A222B), LV_PART_MAIN); // Home's background
  lv_obj_set_style_bg_opa(home_call_big, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(home_call_big, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(home_call_big, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(home_call_big, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(home_call_big, 2, LV_PART_MAIN);
  lv_obj_clear_flag(home_call_big, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(home_call_big, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(home_call_big, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_add_flag(home_call_big, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *head = lv_label_create(home_call_big);
  lv_label_set_text_fmt(head, LV_SYMBOL_CALL "  %s", tr(S_INCOMING_CALL));
  lv_obj_set_style_text_font(head, &lv_font_ui_18, LV_PART_MAIN);
  lv_obj_set_style_text_color(head, COLOR_SUCCESS, LV_PART_MAIN);
  home_call_big_name = lv_label_create(home_call_big);
  lv_obj_set_style_text_font(home_call_big_name, &lv_font_ui_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(home_call_big_name, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_width(home_call_big_name, 450);
  lv_obj_set_style_text_align(home_call_big_name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(home_call_big_name, LV_LABEL_LONG_DOT);
  home_call_big_num = lv_label_create(home_call_big);
  lv_obj_set_style_text_color(home_call_big_num, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_width(home_call_big_num, 470);
  lv_obj_set_style_text_align(home_call_big_num, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(home_call_big_num, LV_LABEL_LONG_WRAP);

  lv_timer_create(weather_slot_tick, 500, NULL);
}

// Shows exactly one of the stats row, the call card and the media card.
static void weather_slot_tick(lv_timer_t *t)
{
  if (call_showing && millis() - call_shown_ms > CALL_SHOW_MS) call_showing = false;
  lv_obj_t *want = home_weather_stats;
  if (call_showing) want = home_call_card;
  else
  {
    // The stats, then whichever of media / voicemail has something, in turn.
    lv_obj_t *turns[3] = {home_weather_stats};
    int n = 1;
    if (media_playing) turns[n++] = home_media_card;
    if (vm_waiting) turns[n++] = home_vm_card;
    want = turns[(millis() / SLOT_SWAP_MS) % n];
  }
  if (call_showing && settings.call_big)
  {
    if (lv_obj_has_flag(home_call_big, LV_OBJ_FLAG_HIDDEN))
    {
      lv_obj_move_foreground(home_call_big);
      lv_obj_clear_flag(home_call_big, LV_OBJ_FLAG_HIDDEN);
    }
    want = home_weather_stats; // underneath, for when it goes
  }
  else if (home_call_big && !lv_obj_has_flag(home_call_big, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(home_call_big, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *all[] = {home_weather_stats, home_call_card, home_media_card, home_vm_card};
  for (lv_obj_t *o : all)
  {
    if (!o) continue;
    bool hidden = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
    if (o == want && hidden) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    else if (o != want && !hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }
}

// loop(): a call just started ringing.
void home_call_apply()
{
  if (!home_call_card) return;
  data_lock();
  char num[40];
  format_phone(call_number, num, sizeof(num));
  bool named = call_name[0] && strcmp(call_name, call_number);
  if (named)
  {
    lv_label_set_text(home_call_name, call_name);
    lv_label_set_text_fmt(home_call_sub, "%s  \xE2\x80\xA2  %s", tr(S_INCOMING_CALL), call_number[0] ? num : tr(S_UNKNOWN));
  }
  else
  {
    lv_label_set_text(home_call_name, call_number[0] ? num : tr(S_UNKNOWN));
    lv_label_set_text(home_call_sub, tr(S_INCOMING_CALL));
  }
  // Big view: the name in its own size above the number. The number font
  // only has digits ( ) - + and space: anything else (no number, letters)
  // goes in the name line instead. "514 555-1234" splits after the area
  // code to fit at 96 px (one line would need 72 px); long international
  // numbers drop to 56 px.
  bool digits_only = call_number[0] != '\0';
  for (const char *p = num; *p; p++)
    if (!strchr("0123456789()-+ ", *p)) digits_only = false;
  lv_label_set_text(home_call_big_name, named ? call_name : digits_only ? "" : call_number[0] ? call_number : tr(S_UNKNOWN));
  (lv_label_get_text(home_call_big_name)[0] ? lv_obj_clear_flag : lv_obj_add_flag)(home_call_big_name, LV_OBJ_FLAG_HIDDEN);
  if (digits_only)
  {
    char big[48];
    strlcpy(big, num, sizeof(big));
    int len = strlen(big);
    bool na = len == 12 && big[3] == ' ' && big[7] == '-';
    if (na) big[3] = '\n'; // "514\n555-1234"
    bool large = na || len <= 8;
    lv_obj_set_style_text_font(home_call_big_num, large ? &lv_font_roboto_bold_96 : &lv_font_roboto_bold_56, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(home_call_big_num, large ? -24 : 0, LV_PART_MAIN);
    lv_label_set_text(home_call_big_num, big);
    lv_obj_clear_flag(home_call_big_num, LV_OBJ_FLAG_HIDDEN);
  }
  else lv_obj_add_flag(home_call_big_num, LV_OBJ_FLAG_HIDDEN);
  data_unlock();
  call_shown_ms = millis();
  call_showing = true;
  // Wake a dimmed screen (night dimming / night clock), like a touch would.
  last_activity_ms = millis();
  dim_forced = false;
  backlight_update();
  weather_slot_tick(NULL);
}

// loop(): a fresh Jellyfin answer.
void home_media_apply()
{
  if (!home_media_card) return;
  data_lock();
  media_playing = jf.count > 0;
  if (media_playing)
  {
    JfSession &s = jf.s[0]; // a playing one comes first
    uint8_t pct = s.run_s ? (uint8_t)(s.pos_s * 100ULL / s.run_s) : 0;
    lv_label_set_text(home_media_icon, s.paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    lv_label_set_text(home_media_title, s.title);
    if (jf.count > 1) lv_label_set_text_fmt(home_media_who, "%s  +%d", s.who, jf.count - 1);
    else lv_label_set_text(home_media_who, s.who);
    lv_label_set_text_fmt(home_media_pct, "%u%%", pct);
    lv_bar_set_value(home_media_bar, pct, LV_ANIM_OFF);
  }
  data_unlock();
  weather_slot_tick(NULL);
}

// loop(): the voicemail count changed.
void home_vm_apply()
{
  if (!home_vm_card) return;
  data_lock();
  vm_waiting = vm_new_total > 0;
  if (vm_waiting)
  {
    if (vm_new_total == 1) lv_label_set_text(home_vm_count, tr(S_VM_ONE));
    else lv_label_set_text_fmt(home_vm_count, tr(S_VM_MANY), vm_new_total);
    if (vm_last_number[0])
    {
      char who[96], when[32] = "";
      char num[40];
      format_phone(vm_last_number, num, sizeof(num));
      if (vm_last_name[0] && strcmp(vm_last_name, vm_last_number)) snprintf(who, sizeof(who), "%s %s", vm_last_name, num);
      else strlcpy(who, num, sizeof(who));
      struct tm t, today;
      time_t now = time(nullptr);
      if (vm_last_time > 1700000000 && localtime_r(&vm_last_time, &t) && localtime_r(&now, &today))
      {
        if (t.tm_yday == today.tm_yday && t.tm_year == today.tm_year) snprintf(when, sizeof(when), "  \xE2\x80\xA2  %02d:%02d", t.tm_hour, t.tm_min);
        else
        {
          char day[24];
          format_day_month(day, sizeof(day), t.tm_mon, t.tm_mday);
          snprintf(when, sizeof(when), "  \xE2\x80\xA2  %s %02d:%02d", day, t.tm_hour, t.tm_min);
        }
      }
      lv_label_set_text_fmt(home_vm_from, "%s %s%s", tr(S_VM_FROM), who, when);
    }
    else lv_label_set_text(home_vm_from, ""); // counted at boot: who left them isn't known
  }
  data_unlock();
  weather_slot_tick(NULL);
}

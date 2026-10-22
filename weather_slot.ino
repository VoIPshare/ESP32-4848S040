// ==================== Home: the humidity / wind / UV slot ====================
// The stats row under the weather shares its place with two cards:
//  - an incoming call (asterisk.ino), ahead of everything: while it rings,
//    and CALL_SHOW_AFTER_MS after (CALL_SHOW_MAX_MS at most; a tap hides it);
//  - Proxmox nodes offline (proxmox.ino), what's playing on Jellyfin
//    (jellyfin.ino) and the printer (bambu.ino: printing, finished for 10
//    min, or can't connect), taking turns with the stats every SLOT_SWAP_MS while
//    there's something to show.
// Only one shows at a time; weather_slot_tick() picks it.
// Voicemail has its own envelope on every screen (top-right), not a card here.
// With "Big caller ID" (setup page) a ringing call covers the whole
// weather panel instead: the number in big digits, the name above it.
// (Named weather_slot so it's merged after asterisk.ino and jellyfin.ino.)

#define CALL_SHOW_AFTER_MS (30UL * 1000UL)
#define CALL_SHOW_MAX_MS (2UL * 60UL * 1000UL)
#define SLOT_SWAP_MS (20UL * 1000UL)

static lv_obj_t *home_call_card = NULL, *home_call_name = NULL, *home_call_sub = NULL;
static lv_obj_t *home_media_card = NULL, *home_media_icon = NULL, *home_media_title = NULL, *home_media_who = NULL,
                *home_media_pct = NULL, *home_media_bar = NULL, *home_media_btn = NULL, *home_media_btn_lab = NULL;
static lv_obj_t *home_print_card = NULL, *home_print_icon = NULL, *home_print_name = NULL, *home_print_sub = NULL, *home_print_pct = NULL,
                *home_print_bar = NULL;
static bool print_running = false;
static lv_obj_t *home_px_card = NULL, *home_px_sub = NULL, *home_px_count = NULL;
static bool px_offline = false; // the Proxmox card has something to show // the printing card has something to show (printing, finished, error)
static lv_obj_t *home_call_big = NULL, *home_call_big_name = NULL, *home_call_big_num = NULL;
static lv_obj_t *vm_icon = NULL, *vm_icon_lab = NULL; // every screen, next to the Wi-Fi icon
static uint32_t call_shown_ms = 0, call_ringing_ms = 0; // when it started / was last seen ringing
static bool call_showing = false;
static bool media_playing = false;

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

// Called with data_lock() held or before network_task runs.
static void vm_icon_update()
{
  if (vm_icon && vm_new_total > 0) lv_label_set_text_fmt(vm_icon_lab, LV_SYMBOL_ENVELOPE " %d", vm_new_total);
  // Shown or hidden by weather_slot_tick().
}

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
  home_media_title = slot_label(home_media_card, &lv_font_ui_20, lv_color_hex(0xFFFFFF), 52, 7, 320);
  home_media_who = slot_label(home_media_card, &lv_font_ui_14, COLOR_TEXT_MUTED, 52, 35, 262);
  home_media_pct = lv_label_create(home_media_card);
  lv_obj_set_style_text_font(home_media_pct, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(home_media_pct, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_align(home_media_pct, LV_ALIGN_TOP_RIGHT, -66, 35);
  // Play/pause of that stream; the rest of the card opens the Jellyfin screen.
  home_media_btn = lv_obj_create(home_media_card);
  lv_obj_set_size(home_media_btn, 44, 44);
  lv_obj_align(home_media_btn, LV_ALIGN_RIGHT_MID, -10, -3);
  lv_obj_set_style_radius(home_media_btn, 12, LV_PART_MAIN);
  lv_obj_set_style_bg_color(home_media_btn, lv_color_hex(0x34424F), LV_PART_MAIN);
  lv_obj_set_style_bg_color(home_media_btn, lv_color_hex(0x46596A), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_border_width(home_media_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(home_media_btn, 0, LV_PART_MAIN);
  lv_obj_clear_flag(home_media_btn, LV_OBJ_FLAG_SCROLLABLE);
  home_media_btn_lab = lv_label_create(home_media_btn);
  lv_label_set_text(home_media_btn_lab, LV_SYMBOL_PAUSE);
  lv_obj_set_style_text_font(home_media_btn_lab, &lv_font_ui_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(home_media_btn_lab, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(home_media_btn_lab);
  home_media_bar = lv_bar_create(home_media_card);
  lv_obj_set_size(home_media_bar, 366, 3);
  lv_obj_align(home_media_bar, LV_ALIGN_BOTTOM_LEFT, 16, -5);
  lv_bar_set_range(home_media_bar, 0, 100);
  lv_obj_set_style_bg_color(home_media_bar, lv_color_hex(0x3A4652), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(home_media_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(home_media_bar, COLOR_PRIMARY, LV_PART_INDICATOR);
  lv_obj_set_style_radius(home_media_bar, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(home_media_bar, 2, LV_PART_INDICATOR);

  // Printing: printer icon, printer name, job and time left, percent, progress.
  home_print_card = slot_card(parent, lv_color_hex(0x2E2418));
  lv_obj_t *pimg = home_print_icon = lv_img_create(home_print_card);
  lv_img_set_src(pimg, &home_icon_printer);
  lv_obj_set_style_img_recolor(pimg, COLOR_SECONDARY, LV_PART_MAIN);
  lv_obj_set_style_img_recolor_opa(pimg, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(pimg, LV_ALIGN_LEFT_MID, 12, -2);
  home_print_name = slot_label(home_print_card, &lv_font_ui_20, lv_color_hex(0xFFFFFF), 52, 7, 290);
  home_print_sub = slot_label(home_print_card, &lv_font_ui_14, COLOR_TEXT_MUTED, 52, 35, 290);
  home_print_pct = lv_label_create(home_print_card);
  lv_obj_set_style_text_font(home_print_pct, &lv_font_ui_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(home_print_pct, COLOR_SECONDARY, LV_PART_MAIN);
  lv_obj_align(home_print_pct, LV_ALIGN_RIGHT_MID, -16, -3);
  home_print_bar = lv_bar_create(home_print_card);
  lv_obj_set_size(home_print_bar, 408, 3);
  lv_obj_align(home_print_bar, LV_ALIGN_BOTTOM_MID, 0, -5);
  lv_bar_set_range(home_print_bar, 0, 100);
  lv_obj_set_style_bg_color(home_print_bar, lv_color_hex(0x4A3A28), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(home_print_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(home_print_bar, COLOR_SECONDARY, LV_PART_INDICATOR);
  lv_obj_set_style_radius(home_print_bar, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(home_print_bar, 2, LV_PART_INDICATOR);

  // Proxmox: nodes offline, named; a tap opens the Proxmox screen.
  home_px_card = slot_card(parent, lv_color_hex(0x3A1A1A));
  lv_obj_t *srv = lv_img_create(home_px_card);
  lv_img_set_src(srv, &home_icon_server);
  lv_obj_set_style_img_recolor(srv, lv_color_hex(0xEF5350), LV_PART_MAIN);
  lv_obj_set_style_img_recolor_opa(srv, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(srv, LV_ALIGN_LEFT_MID, 12, 0);
  slot_label(home_px_card, &lv_font_ui_20, lv_color_hex(0xFFFFFF), 52, 7, 290);
  lv_label_set_text(lv_obj_get_child(home_px_card, -1), "Proxmox");
  home_px_sub = slot_label(home_px_card, &lv_font_ui_14, lv_color_hex(0xFFAB91), 52, 35, 300);
  home_px_count = lv_label_create(home_px_card);
  lv_obj_set_style_text_font(home_px_count, &lv_font_ui_28, LV_PART_MAIN);
  lv_obj_set_style_text_color(home_px_count, lv_color_hex(0xEF5350), LV_PART_MAIN);
  lv_obj_align(home_px_count, LV_ALIGN_RIGHT_MID, -18, 0);

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

  // Voicemail: an orange envelope with the count of new messages on the
  // top layer, top-right where the Wi-Fi icon is (or next to it), on every
  // screen - only while there are new messages.
  vm_icon = make_pill(lv_layer_top(), "", COLOR_SECONDARY, lv_color_hex(0x2B1600), &vm_icon_lab);
  lv_obj_set_pos(vm_icon, 400, 8); // placed by weather_slot_tick()
  lv_obj_clear_flag(vm_icon, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(vm_icon, LV_OBJ_FLAG_HIDDEN);
  vm_icon_update();

  lv_timer_create(weather_slot_tick, 500, NULL);
}

// Shows exactly one of the stats row, the call card and the media card.
static void weather_slot_tick(lv_timer_t *t)
{
  // Top-right: in the Wi-Fi icon's place while that one is hidden (it
  // auto-hides), just left of it while it shows. Same middle line (y 21).
  if (vm_icon)
  {
    bool wifi_shown = wifi_signal_box && !lv_obj_has_flag(wifi_signal_box, LV_OBJ_FLAG_HIDDEN);
    lv_coord_t w = lv_obj_get_width(vm_icon), h = lv_obj_get_height(vm_icon);
    lv_coord_t x = 480 - 10 - w - (wifi_shown ? 34 + 8 : 0), y = 21 - h / 2;
    if (lv_obj_get_x(vm_icon) != x || lv_obj_get_y(vm_icon) != y) lv_obj_set_pos(vm_icon, x, y);
    // Not over the Settings screen's own top-right status text.
    bool hide = !ami_configured() || in_settings || vm_new_total <= 0;
    if (hide != lv_obj_has_flag(vm_icon, LV_OBJ_FLAG_HIDDEN)) (hide ? lv_obj_add_flag : lv_obj_clear_flag)(vm_icon, LV_OBJ_FLAG_HIDDEN);
  }
  if (call_showing)
  {
    data_lock();
    bool ringing = call_log_count && call_log[0].status == CALL_RINGING;
    data_unlock();
    if (ringing) call_ringing_ms = millis();
    if (millis() - call_ringing_ms > CALL_SHOW_AFTER_MS || millis() - call_shown_ms > CALL_SHOW_MAX_MS) call_showing = false;
  }
  lv_obj_t *want = home_weather_stats;
  if (call_showing) want = home_call_card;
  else
  {
    // The stats, then whichever of print / media has something, in turn.
    lv_obj_t *turns[4] = {home_weather_stats};
    int n = 1;
    if (px_offline) turns[n++] = home_px_card;
    if (print_running) turns[n++] = home_print_card;
    if (media_playing) turns[n++] = home_media_card;
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
  lv_obj_t *all[] = {home_weather_stats, home_call_card, home_media_card, home_print_card, home_px_card};
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
  // Big view: the name in its own size above the number. The number fonts
  // only have digits - + and space: anything else (no number, letters,
  // brackets) goes in the name line instead. "514 555-1234" splits after
  // the area code to fit at 90 px; long international numbers drop to 56 px.
  bool digits_only = call_number[0] != '\0';
  for (const char *p = num; *p; p++)
    if (!strchr("0123456789-+ ", *p)) digits_only = false;
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
    lv_obj_set_style_text_font(home_call_big_num, large ? &lv_font_roboto_medium_90 : &lv_font_roboto_regular_56, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(home_call_big_num, large ? 20 : 0, LV_PART_MAIN);
    lv_label_set_text(home_call_big_num, big);
    lv_obj_clear_flag(home_call_big_num, LV_OBJ_FLAG_HIDDEN);
  }
  else lv_obj_add_flag(home_call_big_num, LV_OBJ_FLAG_HIDDEN);
  data_unlock();
  call_shown_ms = call_ringing_ms = millis();
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
    lv_label_set_text(home_media_btn_lab, s.paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
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
  data_lock();
  vm_icon_update();
  data_unlock();
}

static void slot_open_screen_cb(lv_event_t *e)
{
  jump_to_screen((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

// A tap on the big caller ID hides it.
static void slot_call_dismiss_cb(lv_event_t *e)
{
  call_showing = false;
  weather_slot_tick(NULL);
}

static void slot_play_pause_cb(lv_event_t *e)
{
  lv_label_set_text(home_media_btn_lab, "...");
  jf_play_pause_first();
}

// From create_home_screen(), after it made the whole weather panel
// click-through (a tap there opens the Forecast): taps on these cards
// open their own screen instead, and the play/pause button works.
void home_slot_cards_clickable()
{
  struct { lv_obj_t *card; uint8_t screen; } cards[] = {
      {home_media_card, JF_SCREEN_INDEX}, {home_call_card, PHONE_SCREEN_INDEX}, {home_print_card, 1}, {home_px_card, 3}};
  for (auto &c : cards)
  {
    lv_obj_add_flag(c.card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(c.card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(c.card, slot_open_screen_cb, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)c.screen);
  }
  lv_obj_add_flag(home_call_big, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(home_call_big, slot_call_dismiss_cb, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_add_flag(home_media_btn, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(home_media_btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(home_media_btn, slot_play_pause_cb, LV_EVENT_SHORT_CLICKED, NULL);
}

// From bambu_apply_to_ui() (loop()): the printer sent new data.
void home_print_apply()
{
  if (!home_print_card) return;
  const char *st = bambu.gcode_state;
  bool printing = !strcmp(st, "RUNNING") || !strcmp(st, "PREPARE") || !strcmp(st, "PAUSE");
  bool finished = !printing && bambu_finish_badge_until_ms && millis() < bambu_finish_badge_until_ms;
  // The card takes its turn while printing, for 10 min after a print
  // finishes, and while the printer can't be reached.
  print_running = printing || finished || bambu_gave_up;
  if (!print_running)
  {
    weather_slot_tick(NULL);
    return;
  }
  lv_label_set_text(home_print_name, bambu_live_product_name[0] ? bambu_live_product_name : settings.printer_name);
  char sub[96];
  lv_color_t accent = COLOR_SECONDARY;
  int bar = constrain(bambu.percent, 0, 100);
  if (bambu_gave_up)
  {
    accent = lv_color_hex(0xEF5350);
    snprintf(sub, sizeof(sub), "Can't connect - check IP, serial and access code");
    lv_label_set_text(home_print_pct, "!");
    bar = 0;
  }
  else if (finished)
  {
    accent = COLOR_SUCCESS;
    snprintf(sub, sizeof(sub), "%s \xE2\x80\xA2 %s", tr(S_PRINT_FINISHED), bambu.job_name);
    lv_label_set_text(home_print_pct, "100%");
    bar = 100;
  }
  else
  {
    if (!strcmp(st, "PREPARE")) snprintf(sub, sizeof(sub), "%s", tr(S_PREPARING));
    else if (!strcmp(st, "PAUSE")) snprintf(sub, sizeof(sub), "Paused \xE2\x80\xA2 %s", bambu.job_name);
    else if (bambu.remain_time_min > 0)
      snprintf(sub, sizeof(sub), "%dh %02dm left \xE2\x80\xA2 %s", bambu.remain_time_min / 60, bambu.remain_time_min % 60, bambu.job_name);
    else snprintf(sub, sizeof(sub), "%s", bambu.job_name);
    lv_label_set_text_fmt(home_print_pct, "%d%%", bambu.percent);
  }
  lv_label_set_text(home_print_sub, sub);
  lv_obj_set_style_text_color(home_print_pct, accent, LV_PART_MAIN);
  lv_obj_set_style_bg_color(home_print_bar, accent, LV_PART_INDICATOR);
  lv_obj_set_style_img_recolor(home_print_icon, accent, LV_PART_MAIN);
  lv_bar_set_value(home_print_bar, bar, LV_ANIM_OFF);
  weather_slot_tick(NULL);
}

// From update_home_proxmox() (proxmox.ino, loop()): the red Proxmox card
// takes its turn while nodes are offline - their names, and how many.
void home_proxmox_apply(int offline)
{
  if (!home_px_card) return;
  px_offline = offline > 0;
  if (px_offline)
  {
    char names[96] = "";
    for (int i = 0; i < PROXMOX_MAX_NODES; i++)
    {
      if (!proxmox.nodes[i].valid || !strcmp(proxmox.nodes[i].status, "online")) continue;
      if (names[0]) strlcat(names, ", ", sizeof(names));
      strlcat(names, proxmox.nodes[i].name, sizeof(names));
    }
    lv_label_set_text_fmt(home_px_sub, "%s %s", names, tr(S_OFFLINE));
    lv_label_set_text_fmt(home_px_count, "%d", offline);
  }
  weather_slot_tick(NULL);
}

// ==================== Jellyfin: now playing, controls, library ====================
// Asks the Jellyfin server for its sessions (every 5 s while the Jellyfin
// screen shows, 20 s otherwise); while something is playing, Home
// alternates humidity / wind / UV with it (weather_slot.ino). The Jellyfin
// screen shows up to JF_SHOWN streams with previous / play-pause / next /
// stop (always shown: servers report SupportsRemoteControl false for some
// players that do obey, e.g. the Fire TV app; a refused command shows on
// the screen), and the library counts. Needs the server address and an API key
// (Dashboard -> API Keys).
//
// Fetching and commands run on network_task (jf_maintain()); the result is
// copied into jf under data_lock() and shown from loop().
//
// Pop-ups (jf_notify()): calls (asterisk.ino) and print news (bambu.ino)
// are shown on every Jellyfin app used in the last 15 minutes, through the
// DisplayMessage general command (POST /Sessions/{id}/Command - the Fire TV
// app shows these) - queued from any task, sent by network_task.

#define JF_FETCH_VISIBLE_MS (5UL * 1000UL)
#define JF_FETCH_HIDDEN_MS (20UL * 1000UL)
#define JF_COUNTS_MS (30UL * 60UL * 1000UL)
#define JF_MAX 4   // sessions kept
#define JF_SHOWN 2 // with controls on the screen

struct JfSession
{
  char id[40];
  char title[96]; // "Series - Episode", "Artist - Song", "Movie (2019)"
  char who[64];   // "user - device"
  uint32_t pos_s, run_s;
  bool paused;
};
struct JfState
{
  int count = 0; // sessions playing something; the playing ones come first
  JfSession s[JF_MAX];
  int http = 0; // last answer: 200, an HTTP error, or < 0 (no connection)
  bool counts_ok = false;
  int movies = 0, series = 0, episodes = 0, songs = 0;
};
static JfState jf;
// Declared here so Arduino's own prototypes aren't put above the structs.
static void jf_store(const JfState &now);
static void jf_fill_session(JfSession &out, JsonObject s);
static volatile bool jf_ui_refresh = false;
static uint32_t last_jf_fetch_ms = 0, last_jf_counts_ms = 0;
static volatile int jf_cmd_session = -1; // set by a control tap, done by network_task
static const char *volatile jf_cmd = NULL;
// Last command's answer (204 = done), shown on the screen for JF_CMD_NOTE_MS.
#define JF_CMD_NOTE_MS (10UL * 1000UL)
static int jf_cmd_code = 0;
static uint32_t jf_cmd_ms = 0;
static char jf_cmd_note[112] = ""; // "Sent PlayPause to user - device (204)"

bool jf_configured()
{
  return settings.jf_url[0] && settings.jf_key[0];
}

static int jf_request(const char *path, bool post, JsonDocument *doc, JsonDocument *filter, const char *body = NULL); // below

// Device names seen in the session lists (for the setup page's per-device
// pop-up switches), under data_lock().
#define JF_DEVICE_MAX 8
static char jf_devices[JF_DEVICE_MAX][40];
static int jf_device_count = 0;

static void jf_remember_device(const char *name)
{
  if (!name || !name[0]) return;
  data_lock();
  bool known = false;
  for (int i = 0; i < jf_device_count && !known; i++) known = !strcmp(jf_devices[i], name);
  if (!known && jf_device_count < JF_DEVICE_MAX) strlcpy(jf_devices[jf_device_count++], name, sizeof(jf_devices[0]));
  data_unlock();
}

// True when settings.jf_mute ("TV|Phone") lists this device.
bool jf_device_muted(const char *name)
{
  const char *p = settings.jf_mute;
  size_t n = strlen(name);
  while (*p)
  {
    const char *end = strchr(p, '|');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == n && !strncmp(p, name, n)) return true;
    if (!end) break;
    p = end + 1;
  }
  return false;
}

// For the setup page: the devices seen plus the muted ones. Returns how many.
int jf_device_list(char (*out)[40], int max)
{
  int n = 0;
  data_lock();
  for (int i = 0; i < jf_device_count && n < max; i++) strlcpy(out[n++], jf_devices[i], 40);
  data_unlock();
  const char *p = settings.jf_mute;
  while (*p && n < max)
  {
    const char *end = strchr(p, '|');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    char name[40];
    strlcpy(name, p, min(len + 1, sizeof(name)));
    bool dup = false;
    for (int i = 0; i < n && !dup; i++) dup = !strcmp(out[i], name);
    if (!dup && name[0]) strlcpy(out[n++], name, 40);
    if (!end) break;
    p = end + 1;
  }
  return n;
}

#define JF_MSG_MAX 4
#define JF_MSG_SHOW_MS 15000
static struct { char header[48]; char text[112]; } jf_msg_q[JF_MSG_MAX];
static volatile int jf_msg_count = 0;

// Queues a pop-up for the Jellyfin apps (any task). Dropped when Jellyfin
// isn't set up, pop-ups are off, or a few are already waiting.
void jf_notify(const char *header, const char *text)
{
  if (!jf_configured() || !settings.jf_popups) return;
  data_lock();
  if (jf_msg_count < JF_MSG_MAX)
  {
    strlcpy(jf_msg_q[jf_msg_count].header, header, sizeof(jf_msg_q[0].header));
    strlcpy(jf_msg_q[jf_msg_count].text, text, sizeof(jf_msg_q[0].text));
    jf_msg_count++;
  }
  data_unlock();
}

// network_task: sends the queued pop-ups to every recently used session.
static void jf_send_messages()
{
  struct { char header[48]; char text[112]; } q[JF_MSG_MAX];
  data_lock();
  int n = jf_msg_count;
  memcpy(q, jf_msg_q, sizeof(q));
  jf_msg_count = 0;
  data_unlock();

  JsonDocument filter;
  filter[0]["Id"] = true;
  filter[0]["DeviceName"] = true;
  JsonDocument sessions;
  if (jf_request("/Sessions?ActiveWithinSeconds=900", false, &sessions, &filter) != 200) return;
  for (int i = 0; i < n; i++)
  {
    JsonDocument msg;
    msg["Name"] = "DisplayMessage";
    JsonObject args = msg["Arguments"].to<JsonObject>();
    args["Header"] = q[i].header;
    args["Text"] = q[i].text;
    args["TimeoutMs"] = String(JF_MSG_SHOW_MS); // a string, as the clients expect
    String body;
    serializeJson(msg, body);
    for (JsonObject s : sessions.as<JsonArray>())
    {
      const char *id = s["Id"] | "";
      const char *dev = s["DeviceName"] | "";
      jf_remember_device(dev);
      if (!id[0] || jf_device_muted(dev)) continue;
      char path[96];
      snprintf(path, sizeof(path), "/Sessions/%s/Command", id);
      int code = jf_request(path, true, NULL, NULL, body.c_str());
      Serial.printf("[Jellyfin] pop-up \"%s\" to %s: HTTP %d\n", q[i].header, (const char *)(s["DeviceName"] | "?"), code);
    }
  }
}

// GET or POST against the Jellyfin API. Plain http:// or https:// (a LAN
// server with a self-signed certificate is accepted, like Proxmox). On 200,
// a GET's answer is parsed into doc (through filter, if given).
static int jf_request(const char *path, bool post, JsonDocument *doc, JsonDocument *filter, const char *body)
{
  String url = String(settings.jf_url);
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += path;
  bool https = url.startsWith("https://");
  WiFiClient plain;
  WiFiClientSecure secure;
  if (https) secure.setInsecure();
  HTTPClient http;
  http.setTimeout(6000);
  // GETs as HTTP/1.0: no chunked encoding, so the answer can be parsed as a
  // stream. Not for POSTs: HTTPClient sends no Content-Length for an empty
  // body, and an HTTP/1.0 POST without one has a body "until the connection
  // closes" - Jellyfin waited for it and the command timed out (curl, on
  // HTTP/1.1, worked).
  http.useHTTP10(!post);
  if (!http.begin(https ? (WiFiClient &)secure : plain, url)) return -1;
  http.addHeader("Authorization", String("MediaBrowser Token=\"") + settings.jf_key + "\"");
  if (body) http.addHeader("Content-Type", "application/json");
  else if (post) http.addHeader("Content-Length", "0");
  int code = post ? http.POST((uint8_t *)(body ? body : ""), body ? strlen(body) : 0) : http.GET();
  if (code == 200 && doc)
  {
    DeserializationError err = filter ? deserializeJson(*doc, http.getStream(), DeserializationOption::Filter(*filter))
                                      : deserializeJson(*doc, http.getStream());
    if (err)
    {
      Serial.printf("[Jellyfin] bad answer from %s: %s\n", path, err.c_str());
      code = -2;
    }
  }
  else if (code != 200 && code != 204) Serial.printf("[Jellyfin] %s: HTTP %d\n", path, code);
  http.end();
  return code;
}

static void jf_store(const JfState &now)
{
  data_lock();
  jf = now;
  data_unlock();
  jf_ui_refresh = true;
}

static void jf_fill_session(JfSession &out, JsonObject s)
{
  JsonObject item = s["NowPlayingItem"];
  const char *name = item["Name"] | "";
  const char *type = item["Type"] | "";
  const char *series = item["SeriesName"] | "";
  const char *artist = item["AlbumArtist"] | "";
  int year = item["ProductionYear"] | 0;
  if (!strcmp(type, "Episode") && series[0]) snprintf(out.title, sizeof(out.title), "%s - %s", series, name);
  else if (!strcmp(type, "Audio") && artist[0]) snprintf(out.title, sizeof(out.title), "%s - %s", artist, name);
  else if (!strcmp(type, "Movie") && year) snprintf(out.title, sizeof(out.title), "%s (%d)", name, year);
  else strlcpy(out.title, name, sizeof(out.title));
  snprintf(out.who, sizeof(out.who), "%s \xE2\x80\xA2 %s", (const char *)(s["UserName"] | "?"), (const char *)(s["DeviceName"] | "?"));
  strlcpy(out.id, s["Id"] | "", sizeof(out.id));
  out.run_s = (uint32_t)((item["RunTimeTicks"] | 0.0) / 1e7);
  out.pos_s = (uint32_t)((s["PlayState"]["PositionTicks"] | 0.0) / 1e7);
  out.paused = s["PlayState"]["IsPaused"] | false;
}

static void jf_fetch()
{
  last_jf_fetch_ms = millis();
  JfState now;
  data_lock();
  now.counts_ok = jf.counts_ok; // kept between the (rarer) counts fetches
  now.movies = jf.movies, now.series = jf.series, now.episodes = jf.episodes, now.songs = jf.songs;
  data_unlock();

  // The sessions carry a lot more (capabilities, media streams, ...): keep only this.
  JsonDocument filter;
  JsonObject f = filter[0].to<JsonObject>();
  f["Id"] = true;
  f["UserName"] = true;
  f["DeviceName"] = true;
  f["NowPlayingItem"]["Name"] = true;
  f["NowPlayingItem"]["Type"] = true;
  f["NowPlayingItem"]["SeriesName"] = true;
  f["NowPlayingItem"]["AlbumArtist"] = true;
  f["NowPlayingItem"]["ProductionYear"] = true;
  f["NowPlayingItem"]["RunTimeTicks"] = true;
  f["PlayState"]["PositionTicks"] = true;
  f["PlayState"]["IsPaused"] = true;
  JsonDocument doc;
  now.http = jf_request("/Sessions?ActiveWithinSeconds=900", false, &doc, &filter);
  if (now.http == 200)
  {
    for (JsonObject s : doc.as<JsonArray>()) jf_remember_device(s["DeviceName"] | "");
    // Playing ones first, then paused.
    for (int pass = 0; pass < 2; pass++)
      for (JsonObject s : doc.as<JsonArray>())
      {
        if (!s["NowPlayingItem"].is<JsonObject>()) continue;
        bool paused = s["PlayState"]["IsPaused"] | false;
        if (paused != (pass == 1) || now.count >= JF_MAX) continue;
        jf_fill_session(now.s[now.count++], s);
      }
  }

  if (now.http == 200 && (last_jf_counts_ms == 0 || millis() - last_jf_counts_ms > JF_COUNTS_MS))
  {
    last_jf_counts_ms = millis();
    JsonDocument counts;
    if (jf_request("/Items/Counts", false, &counts, NULL) == 200)
    {
      now.counts_ok = true;
      now.movies = counts["MovieCount"] | 0;
      now.series = counts["SeriesCount"] | 0;
      now.episodes = counts["EpisodeCount"] | 0;
      now.songs = counts["SongCount"] | 0;
    }
  }
  jf_store(now);
}

// network_task, every pass.
void jf_maintain()
{
  if (!jf_configured()) return;
  if (jf_msg_count)
  {
    jf_send_messages();
    return;
  }
  int i = jf_cmd_session;
  const char *cmd = jf_cmd;
  if (i >= 0 && cmd)
  {
    jf_cmd_session = -1;
    char path[96], who[64];
    data_lock();
    snprintf(path, sizeof(path), "/Sessions/%s/Playing/%s", i < jf.count ? jf.s[i].id : "-", cmd);
    strlcpy(who, i < jf.count ? jf.s[i].who : "?", sizeof(who));
    data_unlock();
    int code = jf_request(path, true, NULL, NULL);
    Serial.printf("[Jellyfin] %s %s: HTTP %d\n", path, who, code);
    data_lock();
    snprintf(jf_cmd_note, sizeof(jf_cmd_note), "Sent %s to %s (%d)", cmd, who, code);
    data_unlock();
    jf_cmd_code = code;
    jf_cmd_ms = millis();
    delay(400); // let the client act before reading it back
    jf_fetch();
    return;
  }
  uint32_t every = (current_screen == JF_SCREEN_INDEX && !in_subscreen) ? JF_FETCH_VISIBLE_MS : JF_FETCH_HIDDEN_MS;
  if (last_jf_fetch_ms == 0 || millis() - last_jf_fetch_ms > every) jf_fetch();
}

bool jf_take_ui_refresh()
{
  if (!jf_ui_refresh) return false;
  jf_ui_refresh = false;
  return true;
}

// ==================== Screen 11: Jellyfin ====================

static lv_obj_t *jf_status_lab = NULL, *jf_empty_lab = NULL;
static lv_obj_t *jf_card[JF_SHOWN], *jf_card_title[JF_SHOWN], *jf_card_who[JF_SHOWN], *jf_card_bar[JF_SHOWN],
    *jf_card_time[JF_SHOWN], *jf_card_ctrl[JF_SHOWN], *jf_card_play[JF_SHOWN];
static lv_obj_t *jf_count_val[4];

// "1:02:03" or "45:10".
static void jf_format_time(uint32_t s, char *buf, size_t n)
{
  if (s >= 3600) snprintf(buf, n, "%lu:%02lu:%02lu", (unsigned long)(s / 3600), (unsigned long)(s / 60 % 60), (unsigned long)(s % 60));
  else snprintf(buf, n, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

void jf_apply_to_ui()
{
  if (!jf_status_lab) return;
  data_lock();
  if (!jf_configured()) lv_label_set_text(jf_status_lab, "Set the Jellyfin address and API key on the setup page");
  else if (jf.http == 0) lv_label_set_text(jf_status_lab, "Connecting...");
  else if (jf.http == 401) lv_label_set_text(jf_status_lab, "API key refused (HTTP 401)");
  else if (jf.http != 200) lv_label_set_text_fmt(jf_status_lab, "Can't reach the server (%d)", jf.http);
  else if (jf_cmd_ms && millis() - jf_cmd_ms < JF_CMD_NOTE_MS) lv_label_set_text(jf_status_lab, jf_cmd_note); // the last tap's result
  else if (jf.count > JF_SHOWN) lv_label_set_text_fmt(jf_status_lab, "%d streams  \xE2\x80\xA2  %d more not shown", jf.count, jf.count - JF_SHOWN);
  else if (jf.count) lv_label_set_text_fmt(jf_status_lab, "%d stream%s", jf.count, jf.count == 1 ? "" : "s");
  else lv_label_set_text(jf_status_lab, "Connected");
  bool cmd_failed = jf_cmd_ms && millis() - jf_cmd_ms < JF_CMD_NOTE_MS && jf_cmd_code != 200 && jf_cmd_code != 204;
  lv_obj_set_style_text_color(jf_status_lab, (jf.http == 200 || jf.http == 0) && !cmd_failed ? COLOR_TEXT_MUTED : lv_color_hex(0xEF5350),
                              LV_PART_MAIN);

  for (int i = 0; i < JF_SHOWN; i++)
  {
    if (i >= jf.count)
    {
      lv_obj_add_flag(jf_card[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    JfSession &s = jf.s[i];
    lv_obj_clear_flag(jf_card[i], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(jf_card_title[i], s.title);
    lv_label_set_text(jf_card_who[i], s.who);
    char pos[16], run[16];
    jf_format_time(s.pos_s, pos, sizeof(pos));
    jf_format_time(s.run_s, run, sizeof(run));
    if (s.run_s) lv_label_set_text_fmt(jf_card_time[i], "%s%s / %s", s.paused ? "Paused  \xE2\x80\xA2  " : "", pos, run);
    else lv_label_set_text_fmt(jf_card_time[i], "%s%s", s.paused ? "Paused  \xE2\x80\xA2  " : "", pos);
    lv_bar_set_value(jf_card_bar[i], s.run_s ? (int)(s.pos_s * 100ULL / s.run_s) : 0, LV_ANIM_OFF);
    lv_label_set_text(jf_card_play[i], s.paused ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
  }
  (jf.count || jf.http != 200 ? lv_obj_add_flag : lv_obj_clear_flag)(jf_empty_lab, LV_OBJ_FLAG_HIDDEN);

  int vals[4] = {jf.movies, jf.series, jf.episodes, jf.songs};
  for (int k = 0; k < 4; k++)
  {
    if (jf.counts_ok) lv_label_set_text_fmt(jf_count_val[k], "%d", vals[k]);
    else lv_label_set_text(jf_count_val[k], "-");
  }
  data_unlock();
}

// Play/pause of the first stream (Home's card).
void jf_play_pause_first()
{
  jf_cmd = "PlayPause";
  jf_cmd_session = 0;
}

// user_data: session index * 4 + control (0 previous, 1 play/pause, 2 next, 3 stop).
static void jf_ctrl_cb(lv_event_t *e)
{
  static const char *const CMD[] = {"PreviousTrack", "PlayPause", "NextTrack", "Stop"};
  int v = (int)(intptr_t)lv_event_get_user_data(e);
  jf_cmd = CMD[v % 4];
  jf_cmd_session = v / 4;
  if (jf_status_lab)
  {
    lv_label_set_text_fmt(jf_status_lab, "Sending %s...", CMD[v % 4]); // the tap registered
    lv_obj_set_style_text_color(jf_status_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  }
}

// A control button; returns its label (the play/pause one changes symbol).
static lv_obj_t *jf_ctrl_btn(lv_obj_t *parent, const char *sym, int user)
{
  lv_obj_t *btn = lv_obj_create(parent);
  lv_obj_set_size(btn, 84, 40);
  lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x34424F), LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x46596A), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(btn, jf_ctrl_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)user);
  lv_obj_t *lab = lv_label_create(btn);
  lv_label_set_text(lab, sym);
  lv_obj_set_style_text_font(lab, &lv_font_ui_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(lab, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(lab);
  return lab;
}

lv_obj_t *create_jf_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Jellyfin");
  lv_obj_set_style_text_color(title, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_ui_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  jf_status_lab = lv_label_create(scr);
  lv_label_set_text(jf_status_lab, "");
  lv_obj_set_style_text_font(jf_status_lab, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_align(jf_status_lab, LV_ALIGN_TOP_MID, 0, 58);

  jf_empty_lab = lv_label_create(scr);
  lv_label_set_text(jf_empty_lab, "Nothing playing");
  lv_obj_set_style_text_font(jf_empty_lab, &lv_font_ui_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(jf_empty_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_align(jf_empty_lab, LV_ALIGN_TOP_MID, 0, 210);

  for (int i = 0; i < JF_SHOWN; i++)
  {
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 440, 146);
    lv_obj_set_pos(card, 20, 84 + i * 154);
    lv_obj_set_style_bg_color(card, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    jf_card[i] = card;

    jf_card_title[i] = lv_label_create(card);
    lv_obj_set_style_text_font(jf_card_title[i], &lv_font_ui_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(jf_card_title[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_width(jf_card_title[i], 408);
    lv_label_set_long_mode(jf_card_title[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(jf_card_title[i], 16, 10);

    jf_card_who[i] = lv_label_create(card);
    lv_obj_set_style_text_font(jf_card_who[i], &lv_font_ui_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(jf_card_who[i], COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_width(jf_card_who[i], 408);
    lv_label_set_long_mode(jf_card_who[i], LV_LABEL_LONG_DOT);
    lv_obj_set_pos(jf_card_who[i], 16, 36);

    jf_card_bar[i] = lv_bar_create(card);
    lv_obj_set_size(jf_card_bar[i], 408, 5);
    lv_obj_set_pos(jf_card_bar[i], 16, 60);
    lv_bar_set_range(jf_card_bar[i], 0, 100);
    lv_obj_set_style_bg_color(jf_card_bar[i], lv_color_hex(0x3A4652), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(jf_card_bar[i], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(jf_card_bar[i], COLOR_PRIMARY, LV_PART_INDICATOR);

    jf_card_time[i] = lv_label_create(card);
    lv_obj_set_style_text_font(jf_card_time[i], &lv_font_ui_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(jf_card_time[i], COLOR_PRIMARY, LV_PART_MAIN);
    lv_obj_set_pos(jf_card_time[i], 16, 70);

    lv_obj_t *ctrl = lv_obj_create(card);
    lv_obj_set_size(ctrl, 408, 44);
    lv_obj_set_pos(ctrl, 16, 94);
    lv_obj_set_style_bg_opa(ctrl, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctrl, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctrl, 0, LV_PART_MAIN);
    lv_obj_clear_flag(ctrl, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ctrl, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    jf_card_ctrl[i] = ctrl;
    jf_ctrl_btn(ctrl, LV_SYMBOL_PREV, i * 4 + 0);
    jf_card_play[i] = jf_ctrl_btn(ctrl, LV_SYMBOL_PAUSE, i * 4 + 1);
    jf_ctrl_btn(ctrl, LV_SYMBOL_NEXT, i * 4 + 2);
    jf_ctrl_btn(ctrl, LV_SYMBOL_STOP, i * 4 + 3);
  }

  // Library counts
  lv_obj_t *row = lv_obj_create(scr);
  lv_obj_set_size(row, 440, 58);
  lv_obj_set_pos(row, 20, 392);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  static const char *const CAPS[] = {"MOVIES", "SERIES", "EPISODES", "SONGS"};
  for (int k = 0; k < 4; k++)
  {
    lv_obj_t *chip = make_stat_chip(row, "-", CAPS[k], COLOR_PRIMARY, &jf_count_val[k], 104, &lv_font_ui_20, LV_FLEX_ALIGN_CENTER, 54);
    lv_obj_set_style_bg_color(chip, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_GESTURE_BUBBLE);
  }

  jf_apply_to_ui();
  return scr;
}

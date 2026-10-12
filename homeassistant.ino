// ==================== Home Assistant ====================
// Up to HA_MAX entities from Home Assistant's REST API, as tiles: name and
// state, lit when "on". Tapping a light/switch/fan/input_boolean/
// automation tile toggles it. Needs the HA address, a long-lived access
// token (HA profile -> Security) and the entity IDs, on the setup page.
//
// Fetching and toggling run on network_task (ha_maintain()); the screen is
// refreshed from loop() when ha_take_ui_refresh() says so.

#define HA_MAX 8
#define HA_FETCH_VISIBLE_MS (10UL * 1000UL)  // while the screen shows
#define HA_FETCH_HIDDEN_MS (5UL * 60UL * 1000UL)

struct HaEntity
{
  char id[48];
  char name[32];
  char state[24];
  bool ok;
};
static HaEntity ha[HA_MAX];
static int ha_count = 0;
static bool ha_fetched = false;
static volatile bool ha_ui_refresh = false;
static volatile int ha_toggle_index = -1; // set by a tap, done by network_task
static uint32_t last_ha_fetch_ms = 0;

static lv_obj_t *ha_status_lab = NULL;
static lv_obj_t *ha_tile[HA_MAX], *ha_tile_name[HA_MAX], *ha_tile_state[HA_MAX];

bool ha_configured()
{
  return settings.ha_url[0] && settings.ha_token[0] && settings.ha_entities[0];
}

// Lights, switches & co. can be toggled with homeassistant.toggle.
static bool ha_toggleable(const char *id)
{
  static const char *DOMAINS[] = {"light.", "switch.", "fan.", "input_boolean.", "automation.", "script."};
  for (const char *d : DOMAINS)
    if (strncmp(id, d, strlen(d)) == 0) return true;
  return false;
}

// Splits settings.ha_entities ("light.desk, switch.fan") into ha[].
static void ha_parse_entities()
{
  int n = 0;
  const char *p = settings.ha_entities;
  while (*p && n < HA_MAX)
  {
    while (*p == ' ' || *p == ',') p++;
    const char *start = p;
    while (*p && *p != ',') p++;
    int len = p - start;
    while (len > 0 && start[len - 1] == ' ') len--;
    if (len <= 0) continue;
    if (len >= (int)sizeof(ha[n].id)) len = sizeof(ha[n].id) - 1;
    if (strncmp(ha[n].id, start, len) != 0 || ha[n].id[len] != '\0')
    {
      memcpy(ha[n].id, start, len);
      ha[n].id[len] = '\0';
      ha[n].name[0] = ha[n].state[0] = '\0';
      ha[n].ok = false;
    }
    n++;
  }
  ha_count = n;
}

// GET or POST against the HA API. Plain http:// or https:// (a LAN HA with
// a self-signed certificate is accepted, like Proxmox).
static int ha_request(const char *path, const char *body, String *out)
{
  String url = String(settings.ha_url);
  while (url.endsWith("/")) url.remove(url.length() - 1);
  url += path;
  bool https = url.startsWith("https://");
  WiFiClient plain;
  WiFiClientSecure secure;
  if (https) secure.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(https ? (WiFiClient &)secure : plain, url)) return -1;
  http.addHeader("Authorization", String("Bearer ") + settings.ha_token);
  http.addHeader("Content-Type", "application/json");
  int code = body ? http.POST((uint8_t *)body, strlen(body)) : http.GET();
  if (out && code == 200) *out = http.getString();
  http.end();
  return code;
}

static void ha_fetch_all()
{
  last_ha_fetch_ms = millis();
  data_lock();
  ha_parse_entities();
  int n = ha_count;
  data_unlock();

  for (int i = 0; i < n; i++)
  {
    char path[80];
    snprintf(path, sizeof(path), "/api/states/%s", ha[i].id);
    String body;
    int code = ha_request(path, NULL, &body);
    JsonDocument doc;
    bool ok = code == 200 && !deserializeJson(doc, body);
    data_lock();
    ha[i].ok = ok;
    if (ok)
    {
      strlcpy(ha[i].name, doc["attributes"]["friendly_name"] | ha[i].id, sizeof(ha[i].name));
      const char *unit = doc["attributes"]["unit_of_measurement"] | "";
      snprintf(ha[i].state, sizeof(ha[i].state), "%s%s%s", (const char *)(doc["state"] | "?"), unit[0] ? " " : "", unit);
    }
    else
    {
      strlcpy(ha[i].name, ha[i].id, sizeof(ha[i].name));
      snprintf(ha[i].state, sizeof(ha[i].state), code == 401 ? "bad token" : code == 404 ? "not found" : "error %d", code);
    }
    data_unlock();
  }
  ha_fetched = true;
  ha_ui_refresh = true;
}

// network_task, every pass.
void ha_maintain()
{
  if (!ha_configured()) return;
  int t = ha_toggle_index;
  if (t >= 0 && t < ha_count)
  {
    ha_toggle_index = -1;
    char body[80];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", ha[t].id);
    int code = ha_request("/api/services/homeassistant/toggle", body, NULL);
    Serial.printf("[HA] toggle %s: HTTP %d\n", ha[t].id, code);
    delay(300); // let HA apply it before reading it back
    ha_fetch_all();
    return;
  }
  uint32_t every = (current_screen == HA_SCREEN_INDEX && !in_subscreen) ? HA_FETCH_VISIBLE_MS : HA_FETCH_HIDDEN_MS;
  if (last_ha_fetch_ms == 0 || millis() - last_ha_fetch_ms > every) ha_fetch_all();
}

bool ha_take_ui_refresh()
{
  if (!ha_ui_refresh) return false;
  ha_ui_refresh = false;
  return true;
}

void ha_apply_to_ui()
{
  if (!ha_status_lab) return;
  data_lock();
  if (!ha_configured()) lv_label_set_text(ha_status_lab, "Set the Home Assistant address, token and entities on the setup page");
  else if (!ha_fetched) lv_label_set_text(ha_status_lab, "Connecting to Home Assistant...");
  else lv_label_set_text(ha_status_lab, "");

  for (int i = 0; i < HA_MAX; i++)
  {
    if (i >= ha_count || !ha_fetched)
    {
      lv_obj_add_flag(ha_tile[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(ha_tile[i], LV_OBJ_FLAG_HIDDEN);
    bool on = strcmp(ha[i].state, "on") == 0 || strcmp(ha[i].state, "open") == 0 || strcmp(ha[i].state, "home") == 0 ||
              strcmp(ha[i].state, "playing") == 0;
    lv_label_set_text(ha_tile_name[i], ha[i].name);
    // States like "on"/"off" read better capitalized.
    char st[24];
    strlcpy(st, ha[i].state, sizeof(st));
    if (st[0] >= 'a' && st[0] <= 'z') st[0] -= 32;
    lv_label_set_text(ha_tile_state[i], st);
    lv_obj_set_style_bg_color(ha_tile[i], on ? lv_color_hex(0x3A3220) : COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_width(ha_tile[i], on ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_style_text_color(ha_tile_state[i], !ha[i].ok ? lv_color_hex(0xEF5350) : on ? COLOR_WARNING : lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  }
  data_unlock();
}

void ha_tile_clicked_cb(lv_event_t *e)
{
  int i = (int)(intptr_t)lv_event_get_user_data(e);
  if (i >= ha_count || !ha_toggleable(ha[i].id)) return;
  lv_label_set_text(ha_tile_state[i], "...");
  ha_toggle_index = i;
}

// ==================== Screen 9: Home Assistant ====================
lv_obj_t *create_ha_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Home");
  lv_obj_set_style_text_color(title, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_ui_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  ha_status_lab = lv_label_create(scr);
  lv_label_set_text(ha_status_lab, "");
  lv_obj_set_style_text_color(ha_status_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(ha_status_lab, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_set_width(ha_status_lab, 400);
  lv_obj_set_style_text_align(ha_status_lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(ha_status_lab, LV_ALIGN_CENTER, 0, 0);

  // 2 columns x 4 rows.
  for (int i = 0; i < HA_MAX; i++)
  {
    lv_obj_t *tile = lv_obj_create(scr);
    lv_obj_set_size(tile, 212, 86);
    lv_obj_set_pos(tile, 20 + (i % 2) * 228, 72 + (i / 2) * 94);
    lv_obj_set_style_bg_color(tile, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_color(tile, COLOR_WARNING, LV_PART_MAIN);
    lv_obj_set_style_border_width(tile, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(tile, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tile, 12, LV_PART_MAIN);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(tile, ha_tile_clicked_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_HIDDEN);
    ha_tile[i] = tile;

    ha_tile_name[i] = lv_label_create(tile);
    lv_label_set_text(ha_tile_name[i], "");
    lv_obj_set_width(ha_tile_name[i], 188);
    lv_label_set_long_mode(ha_tile_name[i], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(ha_tile_name[i], COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(ha_tile_name[i], &lv_font_ui_14, LV_PART_MAIN);
    lv_obj_align(ha_tile_name[i], LV_ALIGN_TOP_LEFT, 0, 0);

    ha_tile_state[i] = lv_label_create(tile);
    lv_label_set_text(ha_tile_state[i], "");
    lv_obj_set_width(ha_tile_state[i], 188);
    lv_label_set_long_mode(ha_tile_state[i], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(ha_tile_state[i], &lv_font_ui_24, LV_PART_MAIN);
    lv_obj_align(ha_tile_state[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
  }

  ha_apply_to_ui();
  return scr;
}

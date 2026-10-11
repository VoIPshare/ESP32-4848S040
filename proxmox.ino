// ==================== Screen 5: Proxmox ====================
// Split out of esp32_dashboard.ino. Talks to a real Proxmox VE cluster over
// its REST API (https://<host>/api2/json/cluster/resources) using an API
// token from settings.proxmox_host/proxmox_token_id/proxmox_token_secret -
// see esp32_dashboard.ino's build_settings_page() for where those are set.
//
// cluster/resources is used instead of /nodes (or one call per node) because
// it returns nodes, VMs (qemu) and containers (lxc) for the whole cluster in
// a single response - one fetch is enough for both the per-node cards and
// the cluster-wide guest counts below.
//
// This is an Arduino multi-tab sketch: all .ino files in this folder are
// concatenated into one translation unit at build time, so no #include of
// this file is needed - create_proxmox_screen() is called directly from
// setup() in esp32_dashboard.ino. proxmox_needs_ui_refresh/last_proxmox_fetch_ms
// stay declared in esp32_dashboard.ino's top globals block since
// network_task()/loop() there read/write them directly - see that file's
// comment.
// 3, not more: a 3-node cluster is the most common home/lab size (odd count
// for quorum), and it's what fits this screen's fixed 480px height
// comfortably with roomy per-node cards. Extra nodes beyond this simply
// aren't shown - node_count still reflects the real total in the summary
// line above the cards.
#define PROXMOX_MAX_NODES 3

struct ProxmoxNode
{
  char name[32] = "";
  char status[12] = ""; // "online" / "offline" / whatever Proxmox reports
  float cpu_pct = 0;    // 0-100 (cluster/resources reports a 0-1 ratio)
  float mem_pct = 0;    // 0-100, used/total
  float disk_pct = 0;   // 0-100, used/total (node rootfs)
  uint32_t uptime_sec = 0;
  bool valid = false;
};

struct ProxmoxCluster
{
  ProxmoxNode nodes[PROXMOX_MAX_NODES];
  int node_count = 0;
  int vm_running = 0;
  int vm_total = 0;
  int ct_running = 0;
  int ct_total = 0;
  bool valid = false; // at least one successful fetch/parse
};
static ProxmoxCluster proxmox;
// Distinguishes "haven't tried fetching yet" from "tried and it failed" -
// same reasoning as makerworld_fetch_attempted in makerworld.ino.
static bool proxmox_fetch_attempted = false;

static lv_obj_t *proxmox_status_lab = NULL;
static lv_obj_t *proxmox_node_card[PROXMOX_MAX_NODES] = {NULL};
static lv_obj_t *proxmox_node_name_lab[PROXMOX_MAX_NODES] = {NULL};
static lv_obj_t *proxmox_node_pill[PROXMOX_MAX_NODES] = {NULL};
static lv_obj_t *proxmox_node_pill_lab[PROXMOX_MAX_NODES] = {NULL};
static lv_obj_t *proxmox_cpu_bar[PROXMOX_MAX_NODES] = {NULL};
static lv_obj_t *proxmox_cpu_lab[PROXMOX_MAX_NODES] = {NULL};
static lv_obj_t *proxmox_mem_bar[PROXMOX_MAX_NODES] = {NULL};
static lv_obj_t *proxmox_mem_lab[PROXMOX_MAX_NODES] = {NULL};
static lv_obj_t *proxmox_info_lab[PROXMOX_MAX_NODES] = {NULL}; // "Up 12d 4h . Disk 42%"

// "5d 3h" / "3h 12m" / "12m" - whichever two units are most relevant, same
// idea as bambu.ino's remain_time_min formatting.
void proxmox_format_uptime(uint32_t sec, char *buf, size_t buflen)
{
  uint32_t days = sec / 86400UL;
  uint32_t hours = (sec % 86400UL) / 3600UL;
  uint32_t mins = (sec % 3600UL) / 60UL;
  if (days > 0) snprintf(buf, buflen, "%lud %luh", (unsigned long)days, (unsigned long)hours);
  else if (hours > 0) snprintf(buf, buflen, "%luh %lum", (unsigned long)hours, (unsigned long)mins);
  else snprintf(buf, buflen, "%lum", (unsigned long)mins);
}

// Runs on network_task, same blocking-HTTPS-fetch trade-off as
// fetch_weather()/the web_fetch.ino fetches - this is a local LAN call though,
// so it's normally much faster than either of those.
void proxmox_fetch()
{
  last_proxmox_fetch_ms = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  if (settings.proxmox_host[0] == '\0' || settings.proxmox_token_id[0] == '\0' || settings.proxmox_token_secret[0] == '\0') return;

  proxmox_fetch_attempted = true;

  String url = "https://";
  url += settings.proxmox_host;
  url += "/api2/json/cluster/resources";

  // Proxmox's token auth header: "PVEAPIToken=<user@realm!tokenname>=<secret>".
  String auth = "PVEAPIToken=";
  auth += settings.proxmox_token_id;
  auth += "=";
  auth += settings.proxmox_token_secret;

  WiFiClientSecure client;
  client.setInsecure(); // Proxmox's default web cert is self-signed
  HTTPClient http;
  if (!http.begin(client, url))
  {
    Serial.println("[Proxmox] begin() failed");
    proxmox.valid = false;
    proxmox_needs_ui_refresh = true;
    return;
  }
  http.addHeader("Authorization", auth);

  int code = http.GET();
  if (code != HTTP_CODE_OK)
  {
    Serial.printf("[Proxmox] GET failed: %d\n", code);
    http.end();
    proxmox.valid = false;
    proxmox_needs_ui_refresh = true;
    return;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err)
  {
    Serial.printf("[Proxmox] JSON parse failed: %s\n", err.c_str());
    proxmox.valid = false;
    proxmox_needs_ui_refresh = true;
    return;
  }

  JsonArray data = doc["data"];
  if (data.isNull())
  {
    proxmox.valid = false;
    proxmox_needs_ui_refresh = true;
    return;
  }

  int node_n = 0;       // slots actually filled (capped at PROXMOX_MAX_NODES)
  int total_nodes = 0;  // real cluster size, for the summary line even if it exceeds the card slots
  int vm_running = 0, vm_total = 0, ct_running = 0, ct_total = 0;

  for (JsonObject item : data)
  {
    const char *type = item["type"] | "";

    if (strcmp(type, "node") == 0)
    {
      total_nodes++;
      if (node_n >= PROXMOX_MAX_NODES) continue;
      ProxmoxNode &n = proxmox.nodes[node_n];

      const char *name = item["node"] | "";
      strncpy(n.name, name, sizeof(n.name) - 1);
      n.name[sizeof(n.name) - 1] = '\0';

      const char *status = item["status"] | "unknown";
      strncpy(n.status, status, sizeof(n.status) - 1);
      n.status[sizeof(n.status) - 1] = '\0';

      double cpu = item["cpu"] | 0.0;
      n.cpu_pct = (float)(cpu * 100.0);

      double mem = item["mem"] | 0.0;
      double maxmem = item["maxmem"] | 0.0;
      n.mem_pct = (maxmem > 0) ? (float)(mem / maxmem * 100.0) : 0.0f;

      double disk = item["disk"] | 0.0;
      double maxdisk = item["maxdisk"] | 0.0;
      n.disk_pct = (maxdisk > 0) ? (float)(disk / maxdisk * 100.0) : 0.0f;

      n.uptime_sec = item["uptime"] | 0UL;
      n.valid = true;
      node_n++;
    }
    else if (strcmp(type, "qemu") == 0 || strcmp(type, "lxc") == 0)
    {
      if ((item["template"] | 0) == 1) continue; // templates aren't real guests
      bool running = strcmp(item["status"] | "", "running") == 0;
      if (strcmp(type, "qemu") == 0) { vm_total++; if (running) vm_running++; }
      else { ct_total++; if (running) ct_running++; }
    }
  }

  for (int i = node_n; i < PROXMOX_MAX_NODES; i++) proxmox.nodes[i].valid = false;
  proxmox.node_count = total_nodes;
  proxmox.vm_running = vm_running;
  proxmox.vm_total = vm_total;
  proxmox.ct_running = ct_running;
  proxmox.ct_total = ct_total;
  proxmox.valid = true;
  proxmox_needs_ui_refresh = true;

  Serial.printf("[Proxmox] Synced - %d node(s), %d/%d VMs, %d/%d CTs running\n",
                total_nodes, vm_running, vm_total, ct_running, ct_total);
}

// Nodes currently shown in proxmox.nodes[] (up to PROXMOX_MAX_NODES) that
// aren't reporting "online" - the same array proxmox_apply_to_ui() reads.
int proxmox_offline_count()
{
  int n = 0;
  for (int i = 0; i < PROXMOX_MAX_NODES; i++)
  {
    if (!proxmox.nodes[i].valid) continue;
    if (strcmp(proxmox.nodes[i].status, "online") != 0) n++;
  }
  return n;
}

// A few blinks of the Home badge to draw the eye whenever a node newly
// needs attention - same "toggle HIDDEN, never animate opacity" pattern as
// bambu.ino's bambu_badge_flash_cb() (animated/partial opacity on a
// full-screen object breaks on this board - see that file's comment).
void proxmox_badge_flash_cb(lv_timer_t *timer)
{
  proxmox_badge_flash_count++;
  if (proxmox_badge_flash_count >= 6)
  {
    if (home_proxmox_badge) lv_obj_clear_flag(home_proxmox_badge, LV_OBJ_FLAG_HIDDEN);
    lv_timer_del(timer);
    proxmox_badge_flash_timer = NULL;
    return;
  }
  if (!home_proxmox_badge) return;
  if (proxmox_badge_flash_count % 2 == 1) lv_obj_clear_flag(home_proxmox_badge, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(home_proxmox_badge, LV_OBJ_FLAG_HIDDEN);
}

void start_proxmox_badge_flash()
{
  if (proxmox_badge_flash_timer || !home_proxmox_badge) return;
  proxmox_badge_flash_count = 0;
  proxmox_badge_flash_timer = lv_timer_create(proxmox_badge_flash_cb, 250, NULL);
}

// Shows/hides/labels the Home screen's "node offline" badge from the most
// recent fetch. Only fires on a confirmed offline node, never on a failed
// fetch - a single dropped LAN request isn't worth alarming over, same
// reasoning as the Bambu badge only appearing after BAMBU_MAX_FAILURES
// rather than on the first failed connect attempt.
void update_home_proxmox_badge()
{
  if (!home_proxmox_badge) return;

  int offline = proxmox.valid ? proxmox_offline_count() : 0;
  if (offline == 0)
  {
    lv_obj_add_flag(home_proxmox_badge, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  if (home_proxmox_badge_lab)
  {
    if (offline == 1)
    {
      // Name the one offline node when there's just one - more useful than
      // a bare count on a 1- or 2-node box.
      const char *name = "";
      for (int i = 0; i < PROXMOX_MAX_NODES; i++)
      {
        if (proxmox.nodes[i].valid && strcmp(proxmox.nodes[i].status, "online") != 0) { name = proxmox.nodes[i].name; break; }
      }
      lv_label_set_text_fmt(home_proxmox_badge_lab, "%s OFFLINE", name);
    }
    else
    {
      lv_label_set_text_fmt(home_proxmox_badge_lab, "%d NODES OFFLINE", offline);
    }
  }
  lv_obj_clear_flag(home_proxmox_badge, LV_OBJ_FLAG_HIDDEN);
  start_proxmox_badge_flash();
}

void proxmox_apply_to_ui()
{
  if (proxmox_status_lab)
  {
    if (!settings.proxmox_host[0] || !settings.proxmox_token_id[0] || !settings.proxmox_token_secret[0])
      lv_label_set_text(proxmox_status_lab, "No Proxmox API configured - set it on the Wi-Fi setup page");
    else if (!proxmox_fetch_attempted)
      lv_label_set_text(proxmox_status_lab, "Fetching cluster status...");
    else if (!proxmox.valid)
      lv_label_set_text(proxmox_status_lab, "Fetch failed - check host/token in Settings");
    else
      lv_label_set_text_fmt(proxmox_status_lab, "%d node%s  .  %d/%d VMs  .  %d/%d CTs running",
                             proxmox.node_count, proxmox.node_count == 1 ? "" : "s",
                             proxmox.vm_running, proxmox.vm_total,
                             proxmox.ct_running, proxmox.ct_total);
  }

  for (int i = 0; i < PROXMOX_MAX_NODES; i++)
  {
    if (!proxmox_node_card[i]) continue;
    ProxmoxNode &n = proxmox.nodes[i];

    if (!n.valid)
    {
      lv_obj_add_flag(proxmox_node_card[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(proxmox_node_card[i], LV_OBJ_FLAG_HIDDEN);

    if (proxmox_node_name_lab[i]) lv_label_set_text(proxmox_node_name_lab[i], n.name);

    bool online = strcmp(n.status, "online") == 0;
    lv_color_t status_color = online ? COLOR_SUCCESS : lv_color_hex(0xEF5350);
    if (proxmox_node_pill[i]) lv_obj_set_style_bg_color(proxmox_node_pill[i], status_color, LV_PART_MAIN);
    if (proxmox_node_pill_lab[i]) lv_label_set_text(proxmox_node_pill_lab[i], online ? "ONLINE" : "OFFLINE");

    if (proxmox_cpu_bar[i]) lv_bar_set_value(proxmox_cpu_bar[i], (int)n.cpu_pct, LV_ANIM_OFF);
    if (proxmox_cpu_lab[i]) lv_label_set_text_fmt(proxmox_cpu_lab[i], "CPU %.0f%%", n.cpu_pct);

    if (proxmox_mem_bar[i]) lv_bar_set_value(proxmox_mem_bar[i], (int)n.mem_pct, LV_ANIM_OFF);
    if (proxmox_mem_lab[i]) lv_label_set_text_fmt(proxmox_mem_lab[i], "RAM %.0f%%", n.mem_pct);

    if (proxmox_info_lab[i])
    {
      char uptime_buf[16];
      proxmox_format_uptime(n.uptime_sec, uptime_buf, sizeof(uptime_buf));
      lv_label_set_text_fmt(proxmox_info_lab[i], "Up %s  .  Disk %.0f%%", uptime_buf, n.disk_pct);
    }
  }

  update_home_proxmox_badge();
}

lv_obj_t *create_proxmox_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Proxmox Cluster");
  lv_obj_set_style_text_color(title, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  lv_obj_t *status = lv_label_create(scr);
  lv_label_set_text(status, "No Proxmox API configured - set it on the Wi-Fi setup page");
  lv_obj_set_style_text_color(status, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(status, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_width(status, 440);
  lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 56);
  proxmox_status_lab = status;

  // Per-node cards: name + status pill on top, then CPU/RAM bars side by
  // side, then a muted uptime/disk line - the same "important at a glance"
  // set most Proxmox dashboards (and `pvesh get /cluster/resources`) lead
  // with. Each card starts hidden and proxmox_apply_to_ui() reveals only as
  // many as actually reported data, so a 1- or 2-node box doesn't leave
  // empty cards on screen.
  const lv_coord_t card_w = 440;
  const lv_coord_t card_h = 100;
  const lv_coord_t card_gap = 10;
  const lv_coord_t list_top = 86;
  const lv_coord_t bar_w = 198;

  for (int i = 0; i < PROXMOX_MAX_NODES; i++)
  {
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, card_w, card_h);
    lv_obj_align(card, LV_ALIGN_TOP_MID, 0, list_top + i * (card_h + card_gap));
    lv_obj_set_style_bg_color(card, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 14, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 12, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN); // shown once real node data arrives
    proxmox_node_card[i] = card;

    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, "pve-node");
    lv_obj_set_style_text_color(name, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 0, 0);
    proxmox_node_name_lab[i] = name;

    lv_obj_t *pill_lab = NULL;
    lv_obj_t *pill = make_pill(card, "ONLINE", COLOR_SUCCESS, lv_color_hex(0x0B2E13), &pill_lab);
    lv_obj_align(pill, LV_ALIGN_TOP_RIGHT, 0, -2);
    proxmox_node_pill[i] = pill;
    proxmox_node_pill_lab[i] = pill_lab;

    lv_obj_t *cpu_lab = lv_label_create(card);
    lv_label_set_text(cpu_lab, "CPU --%");
    lv_obj_set_style_text_color(cpu_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(cpu_lab, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(cpu_lab, 0, 26);
    proxmox_cpu_lab[i] = cpu_lab;

    lv_obj_t *cpu_bar = lv_bar_create(card);
    lv_obj_set_size(cpu_bar, bar_w, 8);
    lv_obj_set_pos(cpu_bar, 0, 42);
    lv_bar_set_range(cpu_bar, 0, 100);
    lv_bar_set_value(cpu_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(cpu_bar, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_radius(cpu_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cpu_bar, COLOR_PRIMARY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(cpu_bar, 4, LV_PART_INDICATOR);
    proxmox_cpu_bar[i] = cpu_bar;

    lv_obj_t *mem_lab = lv_label_create(card);
    lv_label_set_text(mem_lab, "RAM --%");
    lv_obj_set_style_text_color(mem_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(mem_lab, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(mem_lab, card_w - 24 - bar_w, 26);
    proxmox_mem_lab[i] = mem_lab;

    lv_obj_t *mem_bar = lv_bar_create(card);
    lv_obj_set_size(mem_bar, bar_w, 8);
    lv_obj_set_pos(mem_bar, card_w - 24 - bar_w, 42);
    lv_bar_set_range(mem_bar, 0, 100);
    lv_bar_set_value(mem_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(mem_bar, COLOR_BG, LV_PART_MAIN);
    lv_obj_set_style_radius(mem_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(mem_bar, COLOR_SECONDARY, LV_PART_INDICATOR);
    lv_obj_set_style_radius(mem_bar, 4, LV_PART_INDICATOR);
    proxmox_mem_bar[i] = mem_bar;

    lv_obj_t *info = lv_label_create(card);
    lv_label_set_text(info, "Up --  .  Disk --%");
    lv_obj_set_style_text_color(info, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(info, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_pos(info, 0, 60);
    proxmox_info_lab[i] = info;
  }

  // Syncs the status label with whatever's actually in settings right now -
  // same reasoning as makerworld.ino's create_makerworld_screen() calling
  // makerworld_apply_to_ui() at the end.
  proxmox_apply_to_ui();

  return scr;
}

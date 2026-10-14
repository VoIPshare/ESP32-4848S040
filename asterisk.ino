// ==================== Asterisk (AMI): caller ID, voicemail, Phone screen ====================
// Keeps a TCP connection open to the Asterisk Manager Interface and
// watches DialBegin events: when a phone starts ringing (only the
// extensions listed on the setup page, if any), Home shows the caller for
// 15 s in place of humidity / wind / UV (weather_slot.ino). DialEnd says
// how it ended, for the recent calls on the Phone screen.
//
// Voicemail: MessageWaiting events keep the message counts of the watched
// mailboxes (the extensions, in context "default" unless written
// "101@other"); when a message is left, the event comes on the caller's
// channel, so it also says who left it. MailboxCount after the login
// fills in the counts from before the boot.
//
// Call back (Phone screen): Originate rings the first watched extension
// through Local/<ext>@<context>, then dials the number in <context>.
//
// Needs a manager user in /etc/asterisk/manager.conf, e.g.
//   [dashboard]
//   secret = something-long
//   deny = 0.0.0.0/0.0.0.0
//   permit = 192.168.1.0/255.255.255.0
//   read = call
//   write = reporting,originate   ; MailboxCount, and Originate for call back
//
// The connection runs on network_task (ami_maintain()); everything the UI
// reads is copied under data_lock() and shown from loop().

#define AMI_DEFAULT_PORT 5038
#define AMI_RETRY_MS (30UL * 1000UL)
#define AMI_AUTH_RETRY_MS (5UL * 60UL * 1000UL) // wrong user/secret: don't hammer (fail2ban)
#define AMI_PING_MS (60UL * 1000UL)
#define AMI_SILENT_MS (3 * AMI_PING_MS) // nothing back for this long: reconnect
#define AMI_NOTE_MS (10UL * 1000UL)     // "Calling back..." on the Phone screen
#define CALL_LOG_MAX 6

static WiFiClient ami;
static bool ami_logged_in = false;
static uint32_t ami_next_try_ms = 0, ami_last_ping_ms = 0, ami_last_rx_ms = 0;
static char ami_line[200];
static int ami_line_len = 0;

// The event being read: AMI sends "Key: value" lines, a blank line ends it.
static struct
{
  char event[24], response[16], message[64], action_id[8];
  char uid[40], cid_num[32], cid_name[48];
  char dest_chan[64], dest_num[32], dest_exten[32], dial_status[16];
  char mailbox[48], new_msgs[8], old_msgs[8];
} ami_ev;

// Connection state, for the Phone screen.
enum { AMI_ST_CONNECTING, AMI_ST_UNREACHABLE, AMI_ST_REFUSED, AMI_ST_ONLINE };
static uint8_t ami_status = AMI_ST_CONNECTING;
static char ami_status_msg[64] = "";
static char ami_note[64] = ""; // call back result, shown for AMI_NOTE_MS
static uint32_t ami_note_ms = 0;
static volatile bool ami_status_refresh = false;

// The caller ringing right now, for Home.
static char call_number[32] = "", call_name[48] = "";
static volatile bool call_ui_refresh = false;

// Recent calls to the watched extensions, newest first. One per calling
// channel, so a ring group ringing several phones is still one call.
enum { CALL_RINGING, CALL_ANSWERED, CALL_MISSED, CALL_BUSY };
struct CallLogEntry
{
  char uid[40], number[32], name[48];
  time_t when;
  uint8_t status;
};
static CallLogEntry call_log[CALL_LOG_MAX];
static int call_log_count = 0;
static volatile int ami_callback_req = -1; // index into call_log, set by a tap

// Voicemail (under data_lock()).
#define VM_MAX_BOXES 6
static struct { char box[48]; int new_count, old_count; } vm_box[VM_MAX_BOXES];
static int vm_box_count = 0;
static int vm_new_total = 0, vm_old_total = 0;
static char vm_last_number[32] = "", vm_last_name[48] = "";
static time_t vm_last_time = 0;
static volatile bool vm_ui_refresh = false;

bool ami_configured()
{
  return settings.ami_host[0] && settings.ami_user[0] && settings.ami_secret[0];
}

// North American numbers as "514 555-1234" ("+1 514 555 1234", "(514) 555-1234",
// "15145551234" and "5145551234" alike) and "555-1234"; anything else
// (extensions, international numbers) as it came.
void format_phone(const char *in, char *out, size_t n)
{
  char d[16];
  int nd = 0;
  bool other = false;
  for (const char *p = in; *p; p++)
  {
    if (*p >= '0' && *p <= '9')
    {
      if (nd < (int)sizeof(d) - 1) d[nd++] = *p;
    }
    else if (!strchr(" +-.()", *p)) other = true;
  }
  d[nd] = '\0';
  const char *x = d;
  if (nd == 11 && d[0] == '1') x++, nd--;
  bool plus_other = in[0] == '+' && !(in[1] == '1');
  if (!other && !plus_other && nd == 10 && x[0] >= '2' && x[3] >= '2')
    snprintf(out, n, "%.3s %.3s-%.4s", x, x + 3, x + 6);
  else if (!other && !plus_other && nd == 7 && x[0] >= '2' && in[0] != '+')
    snprintf(out, n, "%.3s-%.4s", x, x + 3);
  else strlcpy(out, in, n);
}

static void ami_set_status(uint8_t st, const char *msg)
{
  data_lock();
  ami_status = st;
  strlcpy(ami_status_msg, msg, sizeof(ami_status_msg));
  data_unlock();
  ami_status_refresh = true;
}

static void ami_set_note(const char *msg)
{
  data_lock();
  strlcpy(ami_note, msg, sizeof(ami_note));
  ami_note_ms = millis();
  data_unlock();
  ami_status_refresh = true;
}

// Next entry of settings.ami_ext ("101, PJSIP/102, 103@office"), trimmed.
static bool ami_next_ext(const char *&p, char *out, size_t n)
{
  while (*p)
  {
    while (*p == ' ' || *p == ',') p++;
    const char *start = p;
    while (*p && *p != ',') p++;
    int len = p - start;
    while (len > 0 && start[len - 1] == ' ') len--;
    if (len <= 0) continue;
    if (len >= (int)n) len = n - 1;
    memcpy(out, start, len);
    out[len] = '\0';
    return true;
  }
  return false;
}

// Bare extension number of an entry: "PJSIP/101" or "101@office" -> "101".
static void ami_ext_number(const char *ext, char *out, size_t n)
{
  const char *slash = strchr(ext, '/');
  strlcpy(out, slash ? slash + 1 : ext, n);
  char *at = strchr(out, '@');
  if (at) *at = '\0';
}

// The extension call back rings: the first one listed ("" if none).
static void ami_callback_ext(char *out, size_t n)
{
  const char *p = settings.ami_ext;
  char ext[48];
  out[0] = '\0';
  if (ami_next_ext(p, ext, sizeof(ext))) ami_ext_number(ext, out, n);
}

// True when the ringing side is one of the watched extensions (or none set).
// An entry matches the extension number or the channel's device: "101" or
// "PJSIP/101" both match DestChannel "PJSIP/101-0000002a".
static bool ami_ext_match()
{
  const char *p = settings.ami_ext;
  if (!*p) return true;
  char ext[48];
  while (ami_next_ext(p, ext, sizeof(ext)))
  {
    char *at = strchr(ext, '@'); // "101@office" is for the mailbox
    if (at) *at = '\0';
    int len = strlen(ext);
    if (!strcmp(ext, ami_ev.dest_num) || !strcmp(ext, ami_ev.dest_exten)) return true;
    // Device part of the channel: "PJSIP/101" from "PJSIP/101-0000002a".
    const char *chan = ami_ev.dest_chan;
    const char *dash = strrchr(chan, '-');
    int dev_len = dash ? dash - chan : strlen(chan);
    const char *slash = strchr(chan, '/');
    if (dev_len == len && !strncmp(chan, ext, len)) return true;
    if (slash && slash < chan + dev_len && dev_len - (slash + 1 - chan) == len && !strncmp(slash + 1, ext, len)) return true;
  }
  return false;
}

// Mailbox of an extension entry: "PJSIP/101" -> "101@default", "101@office" as is.
static void ami_ext_mailbox(const char *ext, char *out, size_t n)
{
  const char *slash = strchr(ext, '/');
  if (slash) ext = slash + 1;
  if (strchr(ext, '@')) strlcpy(out, ext, n);
  else snprintf(out, n, "%s@default", ext);
}

// Every mailbox when no extensions are set.
static bool ami_mailbox_watched(const char *box)
{
  const char *p = settings.ami_ext;
  if (!*p) return true;
  char ext[48], mb[48];
  while (ami_next_ext(p, ext, sizeof(ext)))
  {
    ami_ext_mailbox(ext, mb, sizeof(mb));
    if (!strcasecmp(mb, box)) return true;
  }
  return false;
}

// Asks for the watched mailboxes' counts (right after the login).
static void ami_ask_mailboxes()
{
  const char *p = settings.ami_ext;
  char ext[48], mb[48];
  while (ami_next_ext(p, ext, sizeof(ext)))
  {
    ami_ext_mailbox(ext, mb, sizeof(mb));
    ami.printf("Action: MailboxCount\r\nActionID: mbx\r\nMailbox: %s\r\n\r\n", mb);
  }
}

// A mailbox's counts changed; from_* is who just left a message (if known).
static void vm_update(const char *box, int new_count, int old_count, const char *from_num, const char *from_name)
{
  data_lock();
  int i = 0;
  while (i < vm_box_count && strcasecmp(vm_box[i].box, box)) i++;
  if (i == vm_box_count)
  {
    if (vm_box_count == VM_MAX_BOXES)
    {
      data_unlock();
      return;
    }
    strlcpy(vm_box[i].box, box, sizeof(vm_box[i].box));
    vm_box[i].new_count = vm_box[i].old_count = 0;
    vm_box_count++;
  }
  if (new_count > vm_box[i].new_count && from_num)
  {
    strlcpy(vm_last_number, from_num, sizeof(vm_last_number));
    strlcpy(vm_last_name, from_name ? from_name : "", sizeof(vm_last_name));
    vm_last_time = time(nullptr);
  }
  vm_box[i].new_count = new_count;
  vm_box[i].old_count = old_count;
  vm_new_total = vm_old_total = 0;
  for (int k = 0; k < vm_box_count; k++)
  {
    vm_new_total += vm_box[k].new_count;
    vm_old_total += vm_box[k].old_count;
  }
  if (vm_new_total == 0)
  {
    vm_last_number[0] = vm_last_name[0] = '\0';
    vm_last_time = 0;
  }
  data_unlock();
  vm_ui_refresh = true;
}

// Asterisk's "no value" placeholders.
static bool ami_blank(const char *v)
{
  return !v[0] || !strcmp(v, "<unknown>") || !strcmp(v, "unknown") || !strcmp(v, "<Unknown>");
}

static int call_log_find(const char *uid)
{
  for (int i = 0; i < call_log_count; i++)
    if (!strcmp(call_log[i].uid, uid)) return i;
  return -1;
}

static void ami_dial_begin()
{
  data_lock();
  int i = call_log_find(ami_ev.uid);
  bool fresh = i < 0;
  if (fresh)
  {
    if (call_log_count < CALL_LOG_MAX) call_log_count++;
    memmove(&call_log[1], &call_log[0], (call_log_count - 1) * sizeof(CallLogEntry));
    i = 0;
    CallLogEntry &c = call_log[0];
    strlcpy(c.uid, ami_ev.uid, sizeof(c.uid));
    strlcpy(c.number, ami_blank(ami_ev.cid_num) ? "" : ami_ev.cid_num, sizeof(c.number));
    strlcpy(c.name, ami_blank(ami_ev.cid_name) ? "" : ami_ev.cid_name, sizeof(c.name));
    c.when = time(nullptr);
    strlcpy(call_number, c.number, sizeof(call_number));
    strlcpy(call_name, c.name, sizeof(call_name));
  }
  if (call_log[i].status != CALL_ANSWERED) call_log[i].status = CALL_RINGING; // next phone of a hunt group
  data_unlock();
  if (fresh)
  {
    Serial.printf("[AMI] call for %s from %s\n", ami_ev.dest_chan, ami_ev.cid_num);
    call_ui_refresh = true; // Home shows it
  }
  ami_status_refresh = true;
}

static void ami_dial_end()
{
  data_lock();
  int i = call_log_find(ami_ev.uid);
  if (i >= 0 && call_log[i].status != CALL_ANSWERED)
  {
    const char *st = ami_ev.dial_status;
    call_log[i].status = !strcmp(st, "ANSWER") ? CALL_ANSWERED : !strcmp(st, "BUSY") ? CALL_BUSY : CALL_MISSED;
    ami_status_refresh = true;
  }
  data_unlock();
}

static void ami_response_done()
{
  bool ok = !strcmp(ami_ev.response, "Success");
  if (!strcmp(ami_ev.action_id, "login"))
  {
    if (ok)
    {
      ami_logged_in = true;
      Serial.println("[AMI] logged in");
      char msg[64];
      snprintf(msg, sizeof(msg), "Connected to %s", settings.ami_host);
      ami_set_status(AMI_ST_ONLINE, msg);
      ami_ask_mailboxes();
    }
    else
    {
      Serial.printf("[AMI] login refused: %s\n", ami_ev.message);
      char msg[64];
      snprintf(msg, sizeof(msg), "Login refused: %s", ami_ev.message);
      ami_set_status(AMI_ST_REFUSED, msg);
      ami.stop();
      ami_next_try_ms = millis() + AMI_AUTH_RETRY_MS;
    }
  }
  else if (!strcmp(ami_ev.action_id, "mbx"))
  {
    if (ok) vm_update(ami_ev.mailbox, atoi(ami_ev.new_msgs), atoi(ami_ev.old_msgs), NULL, NULL);
    else Serial.printf("[AMI] MailboxCount: %s (write = reporting?)\n", ami_ev.message);
  }
  else if (!strcmp(ami_ev.action_id, "cb"))
  {
    char msg[64];
    if (ok) snprintf(msg, sizeof(msg), "Calling back - pick up your phone");
    else snprintf(msg, sizeof(msg), "Call back failed: %s", ami_ev.message); // write = originate?
    Serial.printf("[AMI] %s\n", msg);
    ami_set_note(msg);
  }
}

static void ami_event_done()
{
  if (ami_ev.response[0]) ami_response_done();
  else if (!strcmp(ami_ev.event, "MessageWaiting") && ami_mailbox_watched(ami_ev.mailbox))
  {
    Serial.printf("[AMI] voicemail %s: %s new\n", ami_ev.mailbox, ami_ev.new_msgs);
    bool from = !ami_blank(ami_ev.cid_num);
    vm_update(ami_ev.mailbox, atoi(ami_ev.new_msgs), atoi(ami_ev.old_msgs), from ? ami_ev.cid_num : NULL,
              from && !ami_blank(ami_ev.cid_name) ? ami_ev.cid_name : NULL);
  }
  else if (!strcmp(ami_ev.event, "DialBegin") && ami_ext_match()) ami_dial_begin();
  else if (!strcmp(ami_ev.event, "DialEnd")) ami_dial_end();
  memset(&ami_ev, 0, sizeof(ami_ev));
}

static void ami_line_done()
{
  if (ami_line_len == 0)
  {
    ami_event_done();
    return;
  }
  char *colon = strstr(ami_line, ": ");
  if (!colon) return; // the "Asterisk Call Manager/x.y" banner
  *colon = '\0';
  const char *key = ami_line, *val = colon + 2;
  struct { const char *key; char *dst; size_t n; } fields[] = {
      {"Event", ami_ev.event, sizeof(ami_ev.event)},
      {"Response", ami_ev.response, sizeof(ami_ev.response)},
      {"Message", ami_ev.message, sizeof(ami_ev.message)},
      {"ActionID", ami_ev.action_id, sizeof(ami_ev.action_id)},
      {"Uniqueid", ami_ev.uid, sizeof(ami_ev.uid)},
      {"CallerIDNum", ami_ev.cid_num, sizeof(ami_ev.cid_num)},
      {"CallerIDName", ami_ev.cid_name, sizeof(ami_ev.cid_name)},
      {"DestChannel", ami_ev.dest_chan, sizeof(ami_ev.dest_chan)},
      {"DestCallerIDNum", ami_ev.dest_num, sizeof(ami_ev.dest_num)},
      {"DestExten", ami_ev.dest_exten, sizeof(ami_ev.dest_exten)},
      {"DialStatus", ami_ev.dial_status, sizeof(ami_ev.dial_status)},
      {"Mailbox", ami_ev.mailbox, sizeof(ami_ev.mailbox)},
      {"New", ami_ev.new_msgs, sizeof(ami_ev.new_msgs)},         // MessageWaiting
      {"Old", ami_ev.old_msgs, sizeof(ami_ev.old_msgs)},
      {"NewMessages", ami_ev.new_msgs, sizeof(ami_ev.new_msgs)}, // MailboxCount
      {"OldMessages", ami_ev.old_msgs, sizeof(ami_ev.old_msgs)},
  };
  for (auto &f : fields)
    if (!strcmp(key, f.key)) strlcpy(f.dst, val, f.n);
}

static void ami_connect()
{
  char host[64];
  strlcpy(host, settings.ami_host, sizeof(host));
  uint16_t port = AMI_DEFAULT_PORT;
  char *colon = strchr(host, ':');
  if (colon)
  {
    *colon = '\0';
    port = atoi(colon + 1);
  }
  ami_next_try_ms = millis() + AMI_RETRY_MS;
  ami_logged_in = false;
  ami_line_len = 0;
  memset(&ami_ev, 0, sizeof(ami_ev));
  if (!ami.connect(host, port, 3000))
  {
    Serial.printf("[AMI] can't reach %s:%u\n", host, port);
    char msg[64];
    snprintf(msg, sizeof(msg), "Can't reach %s:%u - is AMI enabled?", host, port);
    ami_set_status(AMI_ST_UNREACHABLE, msg);
    return;
  }
  ami.setNoDelay(true);
  ami.printf("Action: Login\r\nActionID: login\r\nUsername: %s\r\nSecret: %s\r\nEvents: call\r\n\r\n", settings.ami_user,
             settings.ami_secret);
  ami_last_rx_ms = ami_last_ping_ms = millis();
}

// Rings the first watched extension, then dials call_log[i]'s number.
static void ami_call_back(int i)
{
  char ext[32], number[32];
  ami_callback_ext(ext, sizeof(ext));
  data_lock();
  strlcpy(number, i < call_log_count ? call_log[i].number : "", sizeof(number));
  data_unlock();
  if (!ext[0] || !number[0]) return;
  const char *ctx = settings.ami_context[0] ? settings.ami_context : "from-internal";
  ami.printf("Action: Originate\r\nActionID: cb\r\nChannel: Local/%s@%s\r\nContext: %s\r\nExten: %s\r\nPriority: 1\r\n"
             "CallerID: \"Call back\" <%s>\r\nTimeout: 30000\r\nAsync: true\r\n\r\n",
             ext, ctx, ctx, number, number);
  char msg[64];
  snprintf(msg, sizeof(msg), "Ringing %s to call %s...", ext, number);
  ami_set_note(msg);
}

// network_task, every pass.
void ami_maintain()
{
  if (!ami_configured()) return;
  if (!ami.connected())
  {
    if (ami_logged_in)
    {
      ami_logged_in = false;
      ami_set_status(AMI_ST_CONNECTING, "Connection lost - reconnecting...");
    }
    if ((int32_t)(millis() - ami_next_try_ms) >= 0) ami_connect();
    return;
  }
  // A bounded chunk per pass, so a burst of events doesn't hold up the rest.
  for (int n = 0; n < 2048 && ami.available(); n++)
  {
    char c = ami.read();
    ami_last_rx_ms = millis();
    if (c == '\r') continue;
    if (c == '\n')
    {
      ami_line[ami_line_len] = '\0';
      ami_line_done();
      ami_line_len = 0;
    }
    else if (ami_line_len < (int)sizeof(ami_line) - 1) ami_line[ami_line_len++] = c;
  }
  if (!ami.connected()) return;
  int cb = ami_callback_req;
  if (cb >= 0)
  {
    ami_callback_req = -1;
    if (ami_logged_in) ami_call_back(cb);
  }
  if (millis() - ami_last_rx_ms > AMI_SILENT_MS)
  {
    Serial.println("[AMI] no answer, reconnecting");
    ami.stop();
    ami_next_try_ms = millis();
  }
  else if (ami_logged_in && millis() - ami_last_ping_ms > AMI_PING_MS)
  {
    ami_last_ping_ms = millis();
    ami.print("Action: Ping\r\nActionID: ping\r\n\r\n");
  }
}

bool call_take_ui_refresh()
{
  if (!call_ui_refresh) return false;
  call_ui_refresh = false;
  return true;
}

bool vm_take_ui_refresh()
{
  if (!vm_ui_refresh) return false;
  vm_ui_refresh = false;
  return true;
}

bool ami_take_status_refresh()
{
  if (!ami_status_refresh) return false;
  ami_status_refresh = false;
  return true;
}

// ==================== Screen 10: Phone ====================
// Connection state, voicemail, and the last CALL_LOG_MAX calls with a
// call-back button each.

static lv_obj_t *phone_status_lab = NULL;
static lv_obj_t *phone_vm_card = NULL, *phone_vm_count = NULL, *phone_vm_from = NULL;
static lv_obj_t *phone_empty_lab = NULL;
static lv_obj_t *phone_row[CALL_LOG_MAX], *phone_row_time[CALL_LOG_MAX], *phone_row_who[CALL_LOG_MAX],
    *phone_row_status[CALL_LOG_MAX], *phone_row_btn[CALL_LOG_MAX];
static bool phone_note_shown = false;

// "14:32" today, "12/10 14:32" (day/month) before.
static void phone_format_when(time_t when, char *buf, size_t n)
{
  struct tm t, today;
  time_t now = time(nullptr);
  buf[0] = '\0';
  if (when < 1700000000 || !localtime_r(&when, &t) || !localtime_r(&now, &today)) return;
  if (t.tm_yday == today.tm_yday && t.tm_year == today.tm_year) snprintf(buf, n, "%02d:%02d", t.tm_hour, t.tm_min);
  else snprintf(buf, n, "%d/%d %02d:%02d", t.tm_mday, t.tm_mon + 1, t.tm_hour, t.tm_min);
}

void phone_apply_to_ui()
{
  if (!phone_status_lab) return;
  data_lock();
  phone_note_shown = ami_note[0] && millis() - ami_note_ms < AMI_NOTE_MS;
  if (!ami_configured()) lv_label_set_text(phone_status_lab, "Set the Asterisk host, user and secret on the setup page");
  else if (phone_note_shown) lv_label_set_text(phone_status_lab, ami_note);
  else lv_label_set_text(phone_status_lab, ami_status_msg[0] ? ami_status_msg : "Connecting...");
  lv_obj_set_style_text_color(phone_status_lab,
                              phone_note_shown ? COLOR_PRIMARY
                              : ami_status == AMI_ST_ONLINE ? COLOR_SUCCESS
                              : ami_status == AMI_ST_CONNECTING ? COLOR_TEXT_MUTED
                                                                : lv_color_hex(0xEF5350),
                              LV_PART_MAIN);

  // Voicemail
  if (vm_new_total > 0)
  {
    lv_label_set_text_fmt(phone_vm_count, "%d new  \xE2\x80\xA2  %d old", vm_new_total, vm_old_total);
    char when[24], who[96];
    phone_format_when(vm_last_time, when, sizeof(when));
    if (vm_last_number[0])
    {
      char num[40];
      format_phone(vm_last_number, num, sizeof(num));
      if (vm_last_name[0] && strcmp(vm_last_name, vm_last_number)) snprintf(who, sizeof(who), "%s %s", vm_last_name, num);
      else strlcpy(who, num, sizeof(who));
      lv_label_set_text_fmt(phone_vm_from, "Last from %s%s%s", who, when[0] ? "  \xE2\x80\xA2  " : "", when);
    }
    else lv_label_set_text(phone_vm_from, "Waiting since before the last restart");
  }
  else
  {
    lv_label_set_text(phone_vm_count, vm_old_total ? "No new voicemail" : "No voicemail");
    if (vm_old_total) lv_label_set_text_fmt(phone_vm_from, "%d old message%s", vm_old_total, vm_old_total == 1 ? "" : "s");
    else lv_label_set_text(phone_vm_from, "");
  }
  lv_obj_set_style_bg_color(phone_vm_card, vm_new_total ? lv_color_hex(0x3A2A12) : COLOR_BG_LIGHT, LV_PART_MAIN);

  // Recent calls
  char ext[32];
  ami_callback_ext(ext, sizeof(ext));
  for (int i = 0; i < CALL_LOG_MAX; i++)
  {
    if (i >= call_log_count)
    {
      lv_obj_add_flag(phone_row[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    CallLogEntry &c = call_log[i];
    lv_obj_clear_flag(phone_row[i], LV_OBJ_FLAG_HIDDEN);
    char when[24];
    phone_format_when(c.when, when, sizeof(when));
    lv_label_set_text(phone_row_time[i], when);
    char num[40];
    format_phone(c.number, num, sizeof(num));
    if (c.name[0] && strcmp(c.name, c.number)) lv_label_set_text_fmt(phone_row_who[i], "%s  %s", c.name, num);
    else lv_label_set_text(phone_row_who[i], c.number[0] ? num : "Unknown");
    static const char *const ST[] = {"Ringing", "Answered", "Missed", "Busy"};
    static const uint32_t ST_COLOR[] = {0x00BCD4, 0x4CAF50, 0xEF5350, 0xFF9800};
    lv_label_set_text(phone_row_status[i], ST[c.status & 3]);
    lv_obj_set_style_text_color(phone_row_status[i], lv_color_hex(ST_COLOR[c.status & 3]), LV_PART_MAIN);
    (ext[0] && c.number[0] ? lv_obj_clear_flag : lv_obj_add_flag)(phone_row_btn[i], LV_OBJ_FLAG_HIDDEN);
  }
  (call_log_count ? lv_obj_add_flag : lv_obj_clear_flag)(phone_empty_lab, LV_OBJ_FLAG_HIDDEN);
  data_unlock();
}

static void phone_callback_cb(lv_event_t *e)
{
  ami_callback_req = (int)(intptr_t)lv_event_get_user_data(e);
}

// Drops the call-back note once it's been up for AMI_NOTE_MS.
static void phone_note_timer_cb(lv_timer_t *t)
{
  if (phone_note_shown && millis() - ami_note_ms >= AMI_NOTE_MS) phone_apply_to_ui();
}

lv_obj_t *create_phone_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Phone");
  lv_obj_set_style_text_color(title, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_ui_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

  phone_status_lab = lv_label_create(scr);
  lv_label_set_text(phone_status_lab, "");
  lv_obj_set_style_text_font(phone_status_lab, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_set_width(phone_status_lab, 440);
  lv_label_set_long_mode(phone_status_lab, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(phone_status_lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(phone_status_lab, LV_ALIGN_TOP_MID, 0, 58);

  // Voicemail card
  phone_vm_card = lv_obj_create(scr);
  lv_obj_set_size(phone_vm_card, 440, 72);
  lv_obj_set_pos(phone_vm_card, 20, 84);
  lv_obj_set_style_bg_color(phone_vm_card, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(phone_vm_card, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(phone_vm_card, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_all(phone_vm_card, 0, LV_PART_MAIN);
  lv_obj_clear_flag(phone_vm_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(phone_vm_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_t *env = lv_label_create(phone_vm_card);
  lv_label_set_text(env, LV_SYMBOL_ENVELOPE);
  lv_obj_set_style_text_font(env, &lv_font_ui_24, LV_PART_MAIN);
  lv_obj_set_style_text_color(env, COLOR_SECONDARY, LV_PART_MAIN);
  lv_obj_align(env, LV_ALIGN_LEFT_MID, 18, 0);
  phone_vm_count = lv_label_create(phone_vm_card);
  lv_obj_set_style_text_font(phone_vm_count, &lv_font_ui_22, LV_PART_MAIN);
  lv_obj_set_style_text_color(phone_vm_count, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_pos(phone_vm_count, 58, 10);
  phone_vm_from = lv_label_create(phone_vm_card);
  lv_obj_set_style_text_font(phone_vm_from, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(phone_vm_from, lv_color_hex(0xFFCC80), LV_PART_MAIN);
  lv_obj_set_width(phone_vm_from, 366);
  lv_label_set_long_mode(phone_vm_from, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(phone_vm_from, 58, 42);

  lv_obj_t *cap = lv_label_create(scr);
  lv_label_set_text(cap, "RECENT CALLS");
  lv_obj_set_style_text_font(cap, &lv_font_ui_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(cap, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(cap, 1, LV_PART_MAIN);
  lv_obj_set_pos(cap, 24, 168);

  phone_empty_lab = lv_label_create(scr);
  lv_label_set_text(phone_empty_lab, "No calls since the last restart");
  lv_obj_set_style_text_font(phone_empty_lab, &lv_font_ui_16, LV_PART_MAIN);
  lv_obj_set_style_text_color(phone_empty_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_align(phone_empty_lab, LV_ALIGN_TOP_MID, 0, 270);

  for (int i = 0; i < CALL_LOG_MAX; i++)
  {
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_set_size(row, 440, 40);
    lv_obj_set_pos(row, 20, 188 + i * 43);
    lv_obj_set_style_bg_color(row, COLOR_BG_LIGHT, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
    phone_row[i] = row;

    phone_row_time[i] = lv_label_create(row);
    lv_obj_set_style_text_font(phone_row_time[i], &lv_font_ui_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(phone_row_time[i], COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_align(phone_row_time[i], LV_ALIGN_LEFT_MID, 12, 0);

    phone_row_who[i] = lv_label_create(row);
    lv_obj_set_style_text_font(phone_row_who[i], &lv_font_ui_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(phone_row_who[i], lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_width(phone_row_who[i], 200);
    lv_label_set_long_mode(phone_row_who[i], LV_LABEL_LONG_DOT);
    lv_obj_align(phone_row_who[i], LV_ALIGN_LEFT_MID, 112, 0);

    phone_row_status[i] = lv_label_create(row);
    lv_obj_set_style_text_font(phone_row_status[i], &lv_font_ui_14, LV_PART_MAIN);
    lv_obj_align(phone_row_status[i], LV_ALIGN_LEFT_MID, 318, 0);

    lv_obj_t *btn = lv_obj_create(row);
    lv_obj_set_size(btn, 40, 32);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_radius(btn, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1B3A20), LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2E5E35), LV_PART_MAIN | LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(btn, phone_callback_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *sym = lv_label_create(btn);
    lv_label_set_text(sym, LV_SYMBOL_CALL);
    lv_obj_set_style_text_font(sym, &lv_font_ui_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(sym, COLOR_SUCCESS, LV_PART_MAIN);
    lv_obj_center(sym);
    phone_row_btn[i] = btn;
  }

  lv_timer_create(phone_note_timer_cb, 1000, NULL);
  phone_apply_to_ui();
  return scr;
}

/*******************************************************************************
 * ESP32-4848S040 Multi-Screen Dashboard
 * LVGL 8.3.0 Compatible Version with ST7701S Support
 * 480x480 Display with 7-8 Swipeable Screens
 ******************************************************************************/

// Shown in the Serial log at boot; .github/workflows/compile.yml tags each
// release with it, so bump it with every firmware change.
#define FW_VERSION "2.5"

// The ESP32-4848S040's RGB panel driver and PSRAM layout only exist on the
// ESP32-S3: fail early with a clear message instead of deep in Arduino_GFX.
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
#error "This dashboard only builds for the ESP32-S3 (ESP32-4848S040): select an ESP32-S3 board."
#endif
#include "Arduino_GFX_Library.h"
#include <lvgl.h>
#include <Wire.h>
#include <TAMC_GT911.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include "calendar_types.h"
#include "direct_fetch.h"

// Set to 0 to silence verbose diagnostic Serial output (raw JSON dumps,
// parsed-field traces) - e.g. once a field mapping is confirmed working and
// you no longer need to see it on every MQTT message. Set to 1 while
// diagnosing a new/changed field. Startup and error/status logs (Wi-Fi,
// NTP, MQTT connect attempts) always print regardless of this.
#define DEBUG 1
#if DEBUG
#define DEBUG_PRINTF(...) Serial.printf(__VA_ARGS__)
#define DEBUG_PRINTLN(...) Serial.println(__VA_ARGS__)
#else
#define DEBUG_PRINTF(...)
#define DEBUG_PRINTLN(...)
#endif

// Hand-drawn weather icon: one shared composite built in create_home_screen(),
// with condition-specific parts shown/hidden by set_weather_icon() rather
// than swapping in image assets. Declared here (not near the rest of the
// weather code below) because Arduino's auto-generated function prototypes
// land right after the includes, and would otherwise reference this type
// before it's declared.
enum WeatherIconType { WICON_CLEAR, WICON_PARTLY_CLOUDY, WICON_CLOUDY, WICON_FOG, WICON_DRIZZLE, WICON_RAIN, WICON_SNOW, WICON_THUNDER };

// Custom 86px regular font (Roboto Regular, subset to 0-9 : ° C F) used for
// the clock and the weather temperature - see lv_font_roboto_regular_86.c.
extern "C" {
extern const lv_font_t lv_font_roboto_regular_86;
}

#define GFX_BL 38

/*******************************************************************************
 * GT911 capacitive touch (I2C) — pins per the vendor reference for this exact
 * board (ESP32-4848S040 / 86BOX GUITION).
 ******************************************************************************/
#define TOUCH_GT911_SDA 19
#define TOUCH_GT911_SCL 45
#define TOUCH_GT911_INT -1
#define TOUCH_GT911_RST -1
#define TOUCH_MAP_X1 480
#define TOUCH_MAP_X2 0
#define TOUCH_MAP_Y1 480
#define TOUCH_MAP_Y2 0

// How many 90 deg clockwise steps my_touchpad_read() applies on top of the
// base TOUCH_MAP_x/y mapping to match gfx's rotation setting (see the
// Arduino_RGB_Display() call above) - the touch chip has no idea what
// rotation the display is set to, so this has to be kept in sync by hand
// whenever that rotation parameter changes. Single number to tweak (0-3)
// instead of re-deriving the point-swap formula each time. Was 2, derived
// by assuming the touch mapping needs to rotate the same direction as the
// display did (rotation 3->0 is +1 step, so +1 more on top of the old
// formula's +1 = 2 total) - wrong on-device, touches landed in the wrong
// spot. Trying 0 next (the opposite assumption: the touch mapping needs to
// move the other way, landing back at "no extra rotation" for index 0). If
// this is still off, 3 is the only untested value left.
#define TOUCH_ROTATION_STEPS 0

TAMC_GT911 ts = TAMC_GT911(TOUCH_GT911_SDA, TOUCH_GT911_SCL, TOUCH_GT911_INT, TOUCH_GT911_RST, 480, 480);

Arduino_DataBus *bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED /* DC */, 39 /* CS */,
    48 /* SCK */, 47 /* MOSI */, GFX_NOT_DEFINED /* MISO */);
Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    18 /* DE */, 17 /* VSYNC */, 16 /* HSYNC */, 21 /* PCLK */,
    11 /* R0 */, 12 /* R1 */, 13 /* R2 */, 14 /* R3 */, 0 /* R4 */,
    8 /* G0 */, 20 /* G1 */, 3 /* G2 */, 46 /* G3 */, 9 /* G4 */, 10 /* G5 */,
    4 /* B0 */, 5 /* B1 */, 6 /* B2 */, 7 /* B3 */, 15 /* B4 */,
    1 /* hsync_polarity */, 10 /* hsync_front_porch */, 8 /* hsync_pulse_width */, 50 /* hsync_back_porch */,
    1 /* vsync_polarity */, 10 /* vsync_front_porch */, 8 /* vsync_pulse_width */, 20 /* vsync_back_porch */,
    0 /* pclk_active_neg */, 12000000 /* prefer_speed */, false /* useBigEndian */,
    0 /* de_idle_high */, 0 /* pclk_idle_high */,
    480 * 40 /* bounce_buffer_size_px: reduce tearing on full-screen swaps - bumped from 480*10
                since network activity (WiFi/TLS competing for PSRAM bandwidth) was starving the
                RGB DMA refill badly enough to show as blinking lines / brief freezes */);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    480 /* width */, 480 /* height */, rgbpanel, 0 /* rotation: library default - see my_touchpad_read() for the matching touch mapping */, true /* auto_flush */,
    bus, GFX_NOT_DEFINED /* RST */, st7701_type9_init_operations, sizeof(st7701_type9_init_operations));

/* Change to your screen resolution */
static uint32_t screenWidth;
static uint32_t screenHeight;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_draw_buf;
static lv_disp_drv_t disp_drv;

#define BLACK 0x0000
#define COLOR_PRIMARY lv_color_hex(0x00BCD4)
#define COLOR_SECONDARY lv_color_hex(0xFF9800)
#define COLOR_SUCCESS lv_color_hex(0x4CAF50)
#define COLOR_WARNING lv_color_hex(0xFFC107)
#define COLOR_BG lv_color_hex(0x121212)
#define COLOR_BG_LIGHT lv_color_hex(0x1E1E1E)
#define COLOR_TEXT_MUTED lv_color_hex(0x8A97A0)
#define COLOR_CLOUD lv_color_hex(0xECEFF1)

// Screens: Home, Bambu printer, Calendar, Proxmox, Stocks, MakerWorld,
// Packages, Flight. The Flight screen is only in the swipe sequence while
// a flight is coming up - see screen_available()/flight.ino.
#define SCREEN_COUNT 8
#define FLIGHT_SCREEN_INDEX 7
static lv_obj_t *screens[SCREEN_COUNT];
static uint8_t current_screen = 0;

// Dynamic demo clock on the Home screen (no NTP wired up yet, so this is a
// free-running tick, not wall-clock time) - lets you see it actually working.
static lv_obj_t *home_hour_label = NULL;
static lv_obj_t *home_min_label = NULL;
static lv_obj_t *home_date_label = NULL;
static lv_obj_t *home_next_event_lab = NULL; // "Next: ..." strip - populated by calendar.ino's update_home_next_event()
static lv_obj_t *home_next_accent = NULL;    // its accent bar - orange while a flight is pinned there
static uint8_t demo_hour = 14, demo_min = 32, demo_sec = 0;
// Weekday/month/day-of-month, set from the same NTP sync that corrects
// demo_hour/min (see sync_clock_ntp()) - placeholder values below match the
// date label's old hardcoded text until the first sync lands.
static uint8_t demo_wday = 6, demo_mday = 15, demo_mon = 7; // tm_wday/tm_mon convention: Sun=0, Jan=0
static uint16_t demo_year = 2026; // full year (not tm_year's since-1900 form), also set in sync_clock_ntp()

// Live-updated weather widgets on the Home screen (see fetch_weather()).
static lv_obj_t *weather_temp_lab = NULL;
static lv_obj_t *weather_desc_lab = NULL;
static lv_obj_t *weather_humidity_val = NULL;
static lv_obj_t *weather_wind_val = NULL;
static lv_obj_t *weather_uv_val = NULL;

// Page-position dots (see create_page_indicator()) auto-hide after idle.
static lv_obj_t *page_dots_row = NULL;
static uint32_t last_activity_ms = 0;
static bool in_settings = false;
static bool in_calendar_day = false; // true while the Calendar day-detail screen is showing (see calendar.ino)

// Auto-return-to-Home after N min idle (see clock_tick_cb()) - N is
// settings.idle_return_home_min, set on the on-device Settings screen (see
// IDLE_TIMEOUT_OPTIONS_MIN/create_settings_screen()) - except while showing
// the Bambu screen because a print just auto-opened it - that case instead
// gets its own 5-min-after-finish return, and neither applies while
// screen_locked.
#define BAMBU_AUTO_FINISH_RETURN_MS (5UL * 60UL * 1000UL)
static bool bambu_screen_auto_opened = false;
static uint32_t bambu_auto_return_home_at_ms = 0; // 0 = not scheduled

// Screen lock: hold the bottom-left corner 3s to toggle (see create_lock_hotspot()
// and lock_hotspot_event_cb()). While locked, swipe navigation, the Home
// badge's jump-to-Bambu, the print-start auto-navigate, and both auto-return
// timers above are all suppressed - the visible screen just stays put.
static lv_obj_t *lock_hotspot = NULL;
static lv_obj_t *lock_indicator = NULL;
static uint32_t lock_press_start_ms = 0;
static bool lock_press_fired = false;
static bool screen_locked = false;

// Wi-Fi signal indicator (top-left, always-on-top layer, see
// create_wifi_signal_indicator()) - four ascending bars filled up to the
// current RSSI-derived level, plus a red diagonal strike across all-dim bars
// when not connected. Same "top layer object, visible on every screen"
// pattern as lock_indicator above. Updated by wifi_signal_apply_to_ui(),
// driven off a periodic RSSI sample taken in network_task (see
// WIFI_SIGNAL_SAMPLE_INTERVAL_MS) - WiFi.RSSI() is a local driver read, not a
// network round-trip, but it's funneled through the same
// sample-in-network_task / apply-in-loop() split as every other bit of
// Wi-Fi state here rather than carving out a one-off exception to the
// LVGL-thread rule.
static lv_obj_t *wifi_bars[4] = {NULL, NULL, NULL, NULL};
static lv_obj_t *wifi_no_signal_line = NULL;

// Home screen print-status badge (see create_home_screen()) + the state
// needed to detect a RUNNING->FINISH transition and time out the "finished"
// badge after 10 minutes. All updated only from bambu_apply_to_ui() (an
// event-driven push, not a poll) and clock_tick_cb()'s existing local 1s
// tick (for the timeout check only - no network involved).
static lv_obj_t *home_bambu_badge = NULL;
static lv_obj_t *home_bambu_badge_lab = NULL;
static char bambu_prev_gcode_state[16] = "";
static uint32_t bambu_finish_badge_until_ms = 0; // 0 = finished-badge not active
#define BAMBU_FINISH_BADGE_MS (10UL * 60UL * 1000UL)

// Full-screen green flash on print finish, and a few blinks of the Home
// badge on connection error - both purely local LVGL-thread visual effects,
// same "toggle HIDDEN, never animate opacity" pattern as blackout_overlay
// (animated/partial opacity on a full 480x480 object breaks on this board -
// see switch_screen()/blackout_overlay history).
static lv_obj_t *finish_flash_overlay = NULL;
static lv_timer_t *finish_flash_timer = NULL;
static uint8_t finish_flash_count = 0;
static lv_timer_t *bambu_badge_flash_timer = NULL;
static uint8_t bambu_badge_flash_count = 0;

// Home screen "node offline" badge (see create_home_screen()) - same
// show/hide-a-pill idea as home_bambu_badge above, just anchored top-right
// instead of top-left so the two can never overlap if both are up at once.
// Updated only from proxmox_apply_to_ui() in proxmox.ino, on the same
// event-driven-push basis as the Bambu badge.
static lv_obj_t *home_proxmox_badge = NULL;
static lv_obj_t *home_proxmox_badge_lab = NULL;
static lv_timer_t *proxmox_badge_flash_timer = NULL;
static uint8_t proxmox_badge_flash_count = 0;

// Bambu/Stocks state touched directly (read/write, not just a function call)
// from code below that stays in this file (loop(), network_task(), the
// Wi-Fi portal's handle_save_post()) - kept here, ahead of every use point,
// rather than in bambu.ino/stocks.ino, since Arduino always compiles this
// main sketch file first and appends the other tabs after it.
static lv_obj_t *bambu_pname_lab = NULL;
static char bambu_live_product_name[32] = ""; // from info.module[].product_name, e.g. "Bambu Lab P2S"
static volatile bool bambu_needs_ui_refresh = false;
static uint8_t bambu_fail_count = 0;
static bool bambu_gave_up = false;
static volatile bool stocks_needs_ui_refresh = false;
static volatile bool calendar_needs_ui_refresh = false;
static volatile bool makerworld_needs_ui_refresh = false;
static volatile bool tracking_needs_ui_refresh = false;
// Stocks/calendar/MakerWorld/packages/flights are each fetched directly from
// their service by web_fetch.ino, which keeps its own per-source cadence.

// Guards the data network_task copies in (calendar, stocks, MakerWorld,
// packages, flight) against the LVGL thread reading it mid-copy. Recursive,
// since calendar_apply_to_ui() calls update_home_next_event(), which locks
// too. Created at the top of setup(), before anything can take it.
static SemaphoreHandle_t data_mutex = NULL;

void data_lock()
{
  if (data_mutex) xSemaphoreTakeRecursive(data_mutex, portMAX_DELAY);
}

void data_unlock()
{
  if (data_mutex) xSemaphoreGiveRecursive(data_mutex);
}

// Proxmox is a local-LAN fetch (no external rate limit to worry about, see
// proxmox.ino), so it refreshes far more often than the Apps Script sources
// above. Also referenced from network_task() below, which stays in this
// file - see the note above.
static volatile bool proxmox_needs_ui_refresh = false;
static uint32_t last_proxmox_fetch_ms = 0;
#define PROXMOX_FETCH_INTERVAL_MS (20UL * 1000UL)

// ==================== Persistent device settings (NVS) ====================
// Declared this early (rather than down by load_settings()/save_settings(),
// where it lived before idle_return_home_min was added) because
// clock_tick_cb() below - which runs long before setup() finishes wiring up
// the rest of the dashboard - now reads settings.idle_return_home_min on
// every tick, and a plain global has to be declared before its first use in
// the same translation unit (unlike functions, which Arduino auto-prototypes
// across every .ino tab regardless of order).
struct DeviceSettings
{
  char tz_name[40] = "Europe/Rome"; // IANA-style zone name - see TZ_TABLE
  // "City", "City, CC" (2-letter country code) or "lat,lon" - a city is
  // looked up once with Open-Meteo's geocoding (see df_resolve_location()).
  char weather_location[65] = "Milan, IT";
  char printer_name[32] = "My Printer"; // no reliable name field in Bambu's local MQTT report - user-entered
  char printer_ip[16] = "192.168.1.100";
  uint16_t printer_port = 8883;
  char mqtt_sn[24] = "";    // Bambu device serial number (MQTT topic: device/<sn>/report)
  char mqtt_pass[16] = "";  // Bambu LAN-mode access code (MQTT password, user "bblp")
  char sta_ssid[33] = "";
  char sta_pass[65] = "";
  bool auto_show_bambu_on_start = false;
  // Fetched directly by web_fetch.ino / direct_fetch.cpp:
  char stock_symbols[96] = "";  // Yahoo Finance symbols, comma separated: "AAPL,NVDA,^IXIC"
  // iCal (.ics) feeds merged into the Calendar screen. A Google Calendar
  // "secret address" works like a password, so the URLs are never logged.
  char ics_urls[ICS_MAX_FEEDS][ICS_URL_MAX] = {"", "", ""};
  char makerworld_uid[24] = ""; // numeric MakerWorld user ID
  char pkge_api_key[96] = "";   // PKGE Business API key (package tracking)
  char aerodatabox_key[96] = ""; // RapidAPI key for AeroDataBox (live flight status, optional)
  uint8_t idle_return_home_min = 5; // one of IDLE_TIMEOUT_OPTIONS_MIN - see create_settings_screen()
  // Proxmox VE cluster - see proxmox.ino. host is "ip-or-hostname:port"
  // (default port 8006); token_id is the full "user@realm!tokenname" from
  // Datacenter -> Permissions -> API Tokens; token_secret is the UUID shown
  // once at token creation time. A read-only PVEAuditor role on the token
  // is enough for everything this screen reads.
  char proxmox_host[64] = "";
  char proxmox_token_id[64] = "";
  char proxmox_token_secret[40] = "";
};
static DeviceSettings settings;

void clock_tick_cb(lv_timer_t *timer)
{
  demo_sec++;
  if (demo_sec >= 60)
  {
    demo_sec = 0;
    demo_min++;
    if (demo_min >= 60) { demo_min = 0; demo_hour++; }
    if (demo_hour >= 24) demo_hour = 0;
    lv_label_set_text_fmt(home_hour_label, "%02d", demo_hour);
    lv_label_set_text_fmt(home_min_label, "%02d", demo_min);
    // Passed events drop off and flight countdowns tick, without a refetch.
    update_home_next_event();
    flight_refresh_screen();
  }

  // Auto-hide the page-position dots after a minute of no touch activity.
  if (page_dots_row && !lv_obj_has_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN) &&
      millis() - last_activity_ms > 60000)
  {
    lv_obj_add_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN);
  }

  // Notice when the 10-min "finished" badge window has elapsed. Purely a
  // local countdown check against millis() - no network call here.
  if (bambu_finish_badge_until_ms != 0 && millis() >= bambu_finish_badge_until_ms)
  {
    update_home_bambu_badge();
  }

  // Auto-return to Home after settings.idle_return_home_min of no touch -
  // except while the Bambu screen is showing because a print just
  // auto-opened it (that case is handled by the finish-triggered return
  // below instead), never while locked, and never while viewing the
  // Calendar day-detail screen (its own back button is the only exit).
  if (!screen_locked && !in_settings && !in_calendar_day && current_screen != 0)
  {
    bool bambu_auto_exempt = (current_screen == 1 && bambu_screen_auto_opened);
    uint32_t idle_return_home_ms = (uint32_t)settings.idle_return_home_min * 60UL * 1000UL;
    if (!bambu_auto_exempt && millis() - last_activity_ms > idle_return_home_ms)
    {
      bambu_screen_auto_opened = false;
      bambu_auto_return_home_at_ms = 0;
      go_to_screen_blackout(0);
    }
  }

  // If the Bambu screen was auto-opened for a print and that print has since
  // finished, return to Home 5 minutes after the finish instead of waiting
  // on the (suppressed) inactivity timeout above.
  if (!screen_locked && bambu_auto_return_home_at_ms != 0 && millis() >= bambu_auto_return_home_at_ms)
  {
    bambu_auto_return_home_at_ms = 0;
    bambu_screen_auto_opened = false;
    if (current_screen == 1 && !in_settings) go_to_screen_blackout(0);
  }
}

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif

  lv_disp_flush_ready(disp);
}

/* Touchpad read */
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
  ts.read();
  if (ts.isTouched)
  {
    last_activity_ms = millis();
    if (page_dots_row && !in_settings && !in_calendar_day) lv_obj_clear_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN);

    lv_coord_t bx = map(ts.points[0].x, TOUCH_MAP_X1, TOUCH_MAP_X2, 0, gfx->width() - 1);
    lv_coord_t by = map(ts.points[0].y, TOUCH_MAP_Y1, TOUCH_MAP_Y2, 0, gfx->height() - 1);

    // See TOUCH_ROTATION_STEPS above - if touch is off after a display
    // rotation change, try bumping that constant (0-3) before touching
    // anything here.
    lv_coord_t tx, ty;
    switch (TOUCH_ROTATION_STEPS % 4)
    {
      case 1: tx = gfx->width() - 1 - by; ty = bx; break;
      case 2: tx = gfx->width() - 1 - bx; ty = gfx->height() - 1 - by; break;
      case 3: tx = by; ty = gfx->height() - 1 - bx; break;
      default: tx = bx; ty = by; break;
    }

    data->state = LV_INDEV_STATE_PR;
    data->point.x = tx;
    data->point.y = ty;
  }
  else
  {
    data->state = LV_INDEV_STATE_REL;
  }
}

// False for screens that are currently left out of the swipe sequence
// (only the Flight screen, when no flight is coming up).
bool screen_available(uint8_t idx)
{
  if (idx == FLIGHT_SCREEN_INDEX) return flight_screen_active();
  return idx < SCREEN_COUNT;
}

// Next (dir = +1) or previous (-1) screen in the swipe sequence, wrapping
// around and skipping unavailable ones.
uint8_t adjacent_screen(uint8_t from, int dir)
{
  uint8_t idx = from;
  for (int i = 0; i < SCREEN_COUNT; i++)
  {
    idx = (uint8_t)((idx + SCREEN_COUNT + dir) % SCREEN_COUNT);
    if (screen_available(idx)) return idx;
  }
  return 0;
}

// Horizontal swipe anywhere on a screen moves to the next/previous screen.
// Vertical drags are left alone so the Tracking screen can still scroll.
void screen_gesture_cb(lv_event_t *e)
{
  if (screen_locked) return;

  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;

  // MOVE_LEFT/RIGHT animate along the wrong physical axis on this panel's
  // rotation, and opacity-based fades (FADE_IN/OUT) flicker because a
  // full 480x480 screen forces LVGL to render into a temporary layer
  // buffer that this board can't reliably allocate. So: cut to black,
  // swap the screen underneath instantly, then cut back - see
  // go_to_screen_blackout().
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_LEFT)
  {
    bambu_screen_auto_opened = false;
    bambu_auto_return_home_at_ms = 0;
    go_to_screen_blackout(adjacent_screen(current_screen, +1));
  }
  else if (dir == LV_DIR_RIGHT)
  {
    bambu_screen_auto_opened = false;
    bambu_auto_return_home_at_ms = 0;
    go_to_screen_blackout(adjacent_screen(current_screen, -1));
  }
}

// Full-screen black cutout on the always-on-top layer, used to mask screen
// swaps. Only ever fully shown or fully hidden (never animated/partial
// opacity) so it never triggers LVGL's temporary-layer compositing path.
static lv_obj_t *blackout_overlay = NULL;
static uint8_t pending_screen = 0;

void blackout_reveal_cb(lv_timer_t *timer)
{
  switch_screen(pending_screen, LV_SCR_LOAD_ANIM_NONE);
  lv_obj_add_flag(blackout_overlay, LV_OBJ_FLAG_HIDDEN);
}

void go_to_screen_blackout(uint8_t target)
{
  pending_screen = target;
  lv_obj_clear_flag(blackout_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_timer_t *t = lv_timer_create(blackout_reveal_cb, 100, NULL);
  lv_timer_set_repeat_count(t, 1);
}

// Short tap on the top-right hotspot (the same one that opens Settings on a
// 5s hold - see create_settings_hotspot()): next screen, like a left swipe.
// SHORT_CLICKED isn't sent after a long press, so opening Settings never
// also flips the page.
void corner_tap_next_screen_cb(lv_event_t *e)
{
  if (screen_locked || in_settings || in_calendar_day) return;
  bambu_screen_auto_opened = false;
  bambu_auto_return_home_at_ms = 0;
  go_to_screen_blackout(adjacent_screen(current_screen, +1));
}

// Tapping the Home screen's print-status badge jumps straight to the Bambu
// screen (index 1 in screens[]).
void bambu_badge_clicked_cb(lv_event_t *e)
{
  if (screen_locked) return;
  bambu_screen_auto_opened = false;
  bambu_auto_return_home_at_ms = 0;
  go_to_screen_blackout(1);
}

// Tapping the Home screen's "node offline" badge jumps straight to the
// Proxmox screen (index 3 in the screens[] array - see setup()).
void proxmox_badge_clicked_cb(lv_event_t *e)
{
  if (screen_locked) return;
  go_to_screen_blackout(3);
}

// Long-press bottom-left corner (3s) to toggle screen_locked - see
// create_lock_hotspot(). Timed manually via PRESSED/PRESSING/RELEASED
// rather than the indev's built-in long_press_time, since that's already
// set to 5s for the Settings hotspot (LVGL only supports one duration per
// indev) and this needs a different, shorter hold.
void lock_hotspot_event_cb(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED)
  {
    lock_press_start_ms = millis();
    lock_press_fired = false;
  }
  else if (code == LV_EVENT_PRESSING)
  {
    if (!lock_press_fired && lock_press_start_ms != 0 && millis() - lock_press_start_ms >= 3000)
    {
      lock_press_fired = true;
      screen_locked = !screen_locked;
      if (lock_indicator)
      {
        if (screen_locked) lv_obj_clear_flag(lock_indicator, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(lock_indicator, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }
  else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
  {
    lock_press_start_ms = 0;
    lock_press_fired = false;
  }
}

#define AP_SSID "ESP32-Dashboard-Setup"
#define AP_PASSWORD "12345678"
#define AP_IP_STR "192.168.4.1"

static Preferences prefs;
static volatile bool wifi_got_ip = false;
static bool wifi_connected = false;

// Wi-Fi signal indicator state - sampled in network_task, applied to the
// bars/strike (wifi_bars/wifi_no_signal_line above) by
// wifi_signal_apply_to_ui() in loop(). rssi/connected are snapshotted
// together at sample time so the UI never shows an RSSI-derived bar count
// alongside a stale "connected" state.
static volatile bool wifi_signal_needs_ui_refresh = false;
static int8_t wifi_signal_rssi_dbm = 0;
static bool wifi_signal_ui_connected = false;
static uint32_t last_wifi_signal_sample_ms = 0;
#define WIFI_SIGNAL_SAMPLE_INTERVAL_MS (2UL * 1000UL)

// ==================== Timezones (IANA name -> POSIX TZ rule) ====================
// A curated subset (not the full ~600-zone IANA database - that's impractical
// to bundle) covering major regions/cities, each with its real DST rule so
// the clock shifts for summer/winter time automatically once set.
struct TzEntry { const char *name; const char *posix; };
static const TzEntry TZ_TABLE[] = {
  {"UTC", "UTC0"},
  {"America/Toronto", "EST5EDT,M3.2.0,M11.1.0"},
  {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Montreal", "EST5EDT,M3.2.0,M11.1.0"},
  {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
  {"America/Winnipeg", "CST6CDT,M3.2.0,M11.1.0"},
  {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
  {"America/Edmonton", "MST7MDT,M3.2.0,M11.1.0"},
  {"America/Phoenix", "MST7"},
  {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
  {"America/Vancouver", "PST8PDT,M3.2.0,M11.1.0"},
  {"America/Anchorage", "AKST9AKDT,M3.2.0,M11.1.0"},
  {"America/Sao_Paulo", "<-03>3"},
  {"America/Mexico_City", "CST6"},
  {"America/Argentina/Buenos_Aires", "<-03>3"},
  {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
  {"Europe/Dublin", "IST-1GMT0,M10.5.0,M3.5.0/1"},
  {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Brussels", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Stockholm", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Europe/Bucharest", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Europe/Kyiv", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Europe/Istanbul", "<+03>-3"},
  {"Europe/Moscow", "MSK-3"},
  {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
  {"Africa/Cairo", "EET-2"},
  {"Africa/Johannesburg", "SAST-2"},
  {"Africa/Lagos", "WAT-1"},
  {"Asia/Dubai", "<+04>-4"},
  {"Asia/Kolkata", "IST-5:30"},
  {"Asia/Karachi", "PKT-5"},
  {"Asia/Dhaka", "<+06>-6"},
  {"Asia/Bangkok", "<+07>-7"},
  {"Asia/Jakarta", "WIB-7"},
  {"Asia/Shanghai", "CST-8"},
  {"Asia/Hong_Kong", "HKT-8"},
  {"Asia/Singapore", "<+08>-8"},
  {"Asia/Tokyo", "JST-9"},
  {"Asia/Seoul", "KST-9"},
  {"Australia/Perth", "AWST-8"},
  {"Australia/Adelaide", "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
  {"Australia/Brisbane", "AEST-10"},
  {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"Australia/Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"Pacific/Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
  {"Pacific/Honolulu", "HST10"},
};
#define TZ_TABLE_COUNT (sizeof(TZ_TABLE) / sizeof(TZ_TABLE[0]))

const char *tz_name_to_posix(const char *name)
{
  for (size_t i = 0; i < TZ_TABLE_COUNT; i++)
    if (strcmp(TZ_TABLE[i].name, name) == 0) return TZ_TABLE[i].posix;
  return "UTC0";
}

// ==================== Weather (Open-Meteo, no API key needed) ====================
static uint32_t last_weather_fetch_ms = 0;
#define WEATHER_FETCH_INTERVAL_MS (10UL * 60UL * 1000UL)

// Weather icon parts (built by build_weather_icon(), all inside one 210x130
// container): each condition shows a different subset. Only circles,
// rounded rects and lines - no images, no shadows (cheap to redraw).
static lv_obj_t *wicon_sun_full = NULL;     // clear day: big sun with 8 rays
static lv_obj_t *wicon_sun_small = NULL;    // partly cloudy day: smaller sun behind the cloud
static lv_obj_t *wicon_moon_full = NULL;    // clear night: crescent + stars
static lv_obj_t *wicon_moon_small = NULL;   // partly cloudy night
static lv_obj_t *wicon_cloud_partly = NULL; // cloud in front of the small sun/moon
static lv_obj_t *wicon_cloud_back = NULL;   // overcast: darker cloud behind...
static lv_obj_t *wicon_cloud_front = NULL;  // ...and a white one in front
static lv_obj_t *wicon_cloud_top = NULL;    // rain/drizzle/snow/storm/fog: cloud with the weather below it
static lv_obj_t *wicon_rain[4] = {NULL};
static lv_obj_t *wicon_snow[6] = {NULL};
static lv_obj_t *wicon_fog_bars[3] = {NULL};
static lv_obj_t *wicon_bolt = NULL;

static void wicon_show(lv_obj_t *obj, bool show)
{
  if (obj) (show ? lv_obj_clear_flag : lv_obj_add_flag)(obj, LV_OBJ_FLAG_HIDDEN);
}

// Recolors every part of a cloud built by make_cloud().
static void wicon_cloud_color(lv_obj_t *cloud, lv_color_t color)
{
  if (!cloud) return;
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(cloud); i++)
    lv_obj_set_style_bg_color(lv_obj_get_child(cloud, i), color, LV_PART_MAIN);
}

void set_weather_icon(WeatherIconType type, bool is_day)
{
  bool precip = (type == WICON_RAIN || type == WICON_DRIZZLE || type == WICON_SNOW || type == WICON_THUNDER || type == WICON_FOG);

  wicon_show(wicon_sun_full, type == WICON_CLEAR && is_day);
  wicon_show(wicon_moon_full, type == WICON_CLEAR && !is_day);
  wicon_show(wicon_sun_small, type == WICON_PARTLY_CLOUDY && is_day);
  wicon_show(wicon_moon_small, type == WICON_PARTLY_CLOUDY && !is_day);
  wicon_show(wicon_cloud_partly, type == WICON_PARTLY_CLOUDY);
  wicon_show(wicon_cloud_back, type == WICON_CLOUDY);
  wicon_show(wicon_cloud_front, type == WICON_CLOUDY);
  wicon_show(wicon_cloud_top, precip);

  lv_color_t top = lv_color_hex(0xB0BEC5);                   // rain, drizzle, fog
  if (type == WICON_SNOW) top = COLOR_CLOUD;                  // snow: white cloud
  else if (type == WICON_THUNDER) top = lv_color_hex(0x78909C); // storm: dark cloud
  wicon_cloud_color(wicon_cloud_top, top);

  for (int i = 0; i < 4; i++)
    wicon_show(wicon_rain[i], type == WICON_RAIN || (type == WICON_DRIZZLE && (i == 1 || i == 2)));
  for (int i = 0; i < 6; i++) wicon_show(wicon_snow[i], type == WICON_SNOW);
  for (int i = 0; i < 3; i++) wicon_show(wicon_fog_bars[i], type == WICON_FOG);
  wicon_show(wicon_bolt, type == WICON_THUNDER);
}

WeatherIconType wmo_weather_icon(int code)
{
  if (code == 0) return WICON_CLEAR;
  if (code == 1 || code == 2) return WICON_PARTLY_CLOUDY;
  if (code == 3) return WICON_CLOUDY;
  if (code == 45 || code == 48) return WICON_FOG;
  if (code >= 51 && code <= 57) return WICON_DRIZZLE;
  if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return WICON_RAIN;
  if ((code >= 71 && code <= 77) || (code >= 85 && code <= 86)) return WICON_SNOW;
  if (code >= 95) return WICON_THUNDER;
  return WICON_PARTLY_CLOUDY;
}

const char *wmo_weather_desc(int code)
{
  if (code == 0) return "Clear Sky";
  if (code == 1 || code == 2) return "Partly Cloudy";
  if (code == 3) return "Overcast";
  if (code == 45 || code == 48) return "Fog";
  if (code >= 51 && code <= 57) return "Drizzle";
  if (code >= 61 && code <= 67) return "Rain";
  if (code >= 71 && code <= 77) return "Snow";
  if (code >= 80 && code <= 82) return "Rain Showers";
  if (code >= 85 && code <= 86) return "Snow Showers";
  if (code >= 95) return "Thunderstorm";
  return "Unknown";
}

// Plain (non-LVGL) result storage: fetch_weather() runs on network_task and
// must never touch LVGL directly (not thread-safe), so it only writes here
// and flags weather_needs_ui_refresh; weather_apply_to_ui() (called from
// loop(), the LVGL-owning thread) does the actual label/icon updates.
struct WeatherData
{
  float temp = 0;
  int humidity = 0;
  float wind = 0;
  float uv = 0;
  int wcode = 0;
  bool is_day = true;
};
static WeatherData weather_data;
static volatile bool weather_needs_ui_refresh = false;

void weather_apply_to_ui()
{
  // NOTE: the custom clock/temperature font only has glyphs for 0-9 : ° C F,
  // so a below-zero reading (no '-' glyph) would render with a missing
  // character - fine for most climates but flag it if you're somewhere cold.
  if (weather_temp_lab) lv_label_set_text_fmt(weather_temp_lab, "%.0f\xC2\xB0" "C", weather_data.temp);
  if (weather_desc_lab) lv_label_set_text(weather_desc_lab, wmo_weather_desc(weather_data.wcode));
  set_weather_icon(wmo_weather_icon(weather_data.wcode), weather_data.is_day);
  if (weather_humidity_val) lv_label_set_text_fmt(weather_humidity_val, "%d%%", weather_data.humidity);
  if (weather_wind_val) lv_label_set_text_fmt(weather_wind_val, "%.0f km/h", weather_data.wind);
  if (weather_uv_val) lv_label_set_text_fmt(weather_uv_val, "%.0f", weather_data.uv);
}

// Applies the latest wifi_signal_rssi_dbm/wifi_signal_ui_connected sample
// (see network_task) to the bars/strike built by
// create_wifi_signal_indicator(). Thresholds are the common phone-style
// RSSI bands (dBm, higher/less-negative = stronger).
void wifi_signal_apply_to_ui()
{
  int level = 0;
  if (wifi_signal_ui_connected)
  {
    if (wifi_signal_rssi_dbm >= -55) level = 4;
    else if (wifi_signal_rssi_dbm >= -65) level = 3;
    else if (wifi_signal_rssi_dbm >= -75) level = 2;
    else level = 1;
  }

  for (int i = 0; i < 4; i++)
  {
    if (!wifi_bars[i]) continue;
    bool filled = i < level;
    lv_obj_set_style_bg_color(wifi_bars[i], filled ? COLOR_PRIMARY : COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(wifi_bars[i], filled ? LV_OPA_COVER : LV_OPA_30, LV_PART_MAIN);
  }

  if (wifi_no_signal_line) (wifi_signal_ui_connected ? lv_obj_add_flag : lv_obj_clear_flag)(wifi_no_signal_line, LV_OBJ_FLAG_HIDDEN);
}

// Blocks for ~1-2s during the HTTPS GET - fine here since this now runs on
// network_task, never on the LVGL-owning thread.
void fetch_weather()
{
  last_weather_fetch_ms = millis();
  if (WiFi.status() != WL_CONNECTED) return;

  float lat, lon;
  if (!df_resolve_location(settings.weather_location, &lat, &lon))
  {
    Serial.printf("[Weather] Can't resolve location \"%s\"\n", settings.weather_location);
    return;
  }

  char url[192];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,wind_speed_10m,uv_index,weather_code,is_day&timezone=auto",
           lat, lon);

  WiFiClientSecure client;
  client.setInsecure(); // read-only public API, no cert pinning needed
  HTTPClient http;
  if (!http.begin(client, url))
  {
    Serial.println("[Weather] begin() failed");
    return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK)
  {
    Serial.printf("[Weather] GET failed: %d\n", code);
    http.end();
    return;
  }

  // getString() (not getStream()) - ArduinoJson's stream deserializer
  // frequently mis-detects EOF against HTTPClient's chunked HTTPS
  // responses on ESP32, surfacing as InvalidInput/IncompleteInput even
  // though the GET itself succeeded.
  String payload = http.getString();
  http.end();
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err)
  {
    Serial.printf("[Weather] JSON parse failed: %s\n", err.c_str());
    return;
  }

  JsonObject current = doc["current"];
  if (current.isNull()) return;

  weather_data.temp = current["temperature_2m"] | 0.0f;
  weather_data.humidity = current["relative_humidity_2m"] | 0;
  weather_data.wind = current["wind_speed_10m"] | 0.0f;
  weather_data.uv = current["uv_index"] | 0.0f;
  weather_data.wcode = current["weather_code"] | 0;
  weather_data.is_day = (current["is_day"] | 1) != 0;
  weather_needs_ui_refresh = true;

  Serial.printf("[Weather] %.1f C, %s\n", weather_data.temp, wmo_weather_desc(weather_data.wcode));
}

// Stocks backend + screen moved to stocks.ino (see that file's header
// comment for why no #include is needed).

// Bambu backend + screen moved to bambu.ino (see that file's header comment
// for why no #include is needed).
// NVS keys for settings.ics_urls[] (max 15 chars each).
static const char *ICS_PREF_KEYS[ICS_MAX_FEEDS] = {"ics0", "ics1", "ics2"};

void load_settings()
{
  prefs.begin("dash", true);
  prefs.getString("tzn", settings.tz_name, sizeof(settings.tz_name));
  if (prefs.isKey("wloc"))
    prefs.getString("wloc", settings.weather_location, sizeof(settings.weather_location));
  else if (prefs.isKey("lat")) // saved by older firmware as two numbers
    snprintf(settings.weather_location, sizeof(settings.weather_location), "%.4f,%.4f",
             prefs.getFloat("lat", 0), prefs.getFloat("lon", 0));
  prefs.getString("pname", settings.printer_name, sizeof(settings.printer_name));
  prefs.getString("pip", settings.printer_ip, sizeof(settings.printer_ip));
  settings.printer_port = prefs.getUShort("pport", settings.printer_port);
  prefs.getString("mqtt_sn", settings.mqtt_sn, sizeof(settings.mqtt_sn));
  prefs.getString("mqtt_pass", settings.mqtt_pass, sizeof(settings.mqtt_pass));
  prefs.getString("ssid", settings.sta_ssid, sizeof(settings.sta_ssid));
  prefs.getString("pass", settings.sta_pass, sizeof(settings.sta_pass));
  settings.auto_show_bambu_on_start = prefs.getBool("abos", settings.auto_show_bambu_on_start);
  prefs.getString("stocks", settings.stock_symbols, sizeof(settings.stock_symbols));
  for (int i = 0; i < ICS_MAX_FEEDS; i++)
    prefs.getString(ICS_PREF_KEYS[i], settings.ics_urls[i], sizeof(settings.ics_urls[i]));
  prefs.getString("mwuid", settings.makerworld_uid, sizeof(settings.makerworld_uid));
  prefs.getString("pkge", settings.pkge_api_key, sizeof(settings.pkge_api_key));
  prefs.getString("adbkey", settings.aerodatabox_key, sizeof(settings.aerodatabox_key));
  settings.idle_return_home_min = prefs.getUChar("idlemin", settings.idle_return_home_min);
  prefs.getString("pxhost", settings.proxmox_host, sizeof(settings.proxmox_host));
  prefs.getString("pxtok", settings.proxmox_token_id, sizeof(settings.proxmox_token_id));
  prefs.getString("pxsec", settings.proxmox_token_secret, sizeof(settings.proxmox_token_secret));
  prefs.end();
}

void save_settings()
{
  prefs.begin("dash", false);
  prefs.putString("tzn", settings.tz_name);
  prefs.putString("wloc", settings.weather_location);
  prefs.putString("pname", settings.printer_name);
  prefs.putString("pip", settings.printer_ip);
  prefs.putUShort("pport", settings.printer_port);
  prefs.putString("mqtt_sn", settings.mqtt_sn);
  prefs.putString("mqtt_pass", settings.mqtt_pass);
  prefs.putString("ssid", settings.sta_ssid);
  prefs.putString("pass", settings.sta_pass);
  prefs.putBool("abos", settings.auto_show_bambu_on_start);
  prefs.putString("stocks", settings.stock_symbols);
  for (int i = 0; i < ICS_MAX_FEEDS; i++)
    prefs.putString(ICS_PREF_KEYS[i], settings.ics_urls[i]);
  prefs.putString("mwuid", settings.makerworld_uid);
  prefs.putString("pkge", settings.pkge_api_key);
  prefs.putString("adbkey", settings.aerodatabox_key);
  prefs.putUChar("idlemin", settings.idle_return_home_min);
  prefs.putString("pxhost", settings.proxmox_host);
  prefs.putString("pxtok", settings.proxmox_token_id);
  prefs.putString("pxsec", settings.proxmox_token_secret);
  prefs.end();
}

// ==================== Wi-Fi setup portal (served while the AP is on) ====================
WebServer config_server(80);
DNSServer dns_server;

String html_escape(const String &s)
{
  String out = s;
  out.replace("&", "&amp;");
  out.replace("\"", "&quot;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  return out;
}

// For embedding a value inside a single-quoted JS string literal (different
// escaping rules than an HTML attribute).
String js_escape(const String &s)
{
  String out = s;
  out.replace("\\", "\\\\");
  out.replace("'", "\\'");
  return out;
}

String build_settings_page()
{
  String page = F(
      "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Dashboard Setup</title><style>"
      "body{background:#121212;color:#fff;font-family:sans-serif;margin:0;padding:24px}"
      "h2{color:#00BCD4;margin-top:0}"
      "fieldset{border:1px solid #1E1E1E;background:#1E1E1E;border-radius:12px;padding:16px;margin-bottom:16px}"
      "legend{color:#00BCD4;padding:0 8px}"
      "label{display:block;font-size:13px;color:#8A97A0;margin-top:10px}"
      "input,select{width:100%;box-sizing:border-box;padding:8px;margin-top:4px;border-radius:8px;border:1px solid #333;background:#121212;color:#fff}"
      "button{width:100%;padding:12px;margin-top:16px;border:0;border-radius:8px;background:#00BCD4;color:#0B2E13;font-size:16px}"
      "</style></head><body>"
      "<h2>Dashboard Setup</h2>"
      "<form action='/save' method='POST'>"
      "<fieldset><legend>Wi-Fi</legend>"
      "<label>Home network</label>"
      "<select name='ssid' id='ssid'>");
  if (settings.sta_ssid[0] != '\0')
  {
    page += "<option value='" + html_escape(settings.sta_ssid) + "' selected>" + html_escape(settings.sta_ssid) + " (saved)</option>";
  }
  else
  {
    page += F("<option value=''>Scanning...</option>");
  }
  page += F(
      "</select>"
      "<label>Password</label><input type='password' name='pass' value='");
  page += html_escape(settings.sta_pass);
  page += F(
      "'>"
      "</fieldset>"
      "<fieldset><legend>Clock</legend>"
      "<label>Time zone</label>"
      "<select name='tz'>");
  for (size_t i = 0; i < TZ_TABLE_COUNT; i++)
  {
    page += "<option value='";
    page += TZ_TABLE[i].name;
    page += "'";
    if (strcmp(TZ_TABLE[i].name, settings.tz_name) == 0) page += " selected";
    page += ">";
    page += TZ_TABLE[i].name;
    page += "</option>";
  }
  page += F(
      "</select>"
      "</fieldset>"
      "<fieldset><legend>Weather location</legend>"
      "<label>City or coordinates</label><input type='text' name='wloc' placeholder='London, GB' value='");
  page += html_escape(settings.weather_location);
  page += F(
      "'>"
      "<label style='margin-top:2px;font-size:11px'>A city, optionally with a 2-letter country code "
      "(<i>London, GB</i>), or <i>latitude,longitude</i> (<i>51.51,-0.13</i>).</label>"
      "</fieldset>"
      "<fieldset><legend>Bambu printer</legend>"
      "<label>Printer name</label><input type='text' name='pname' value='");
  page += html_escape(settings.printer_name);
  page += F(
      "'>"
      "<label>IP address</label><input type='text' name='pip' value='");
  page += html_escape(settings.printer_ip);
  page += F("'>"
            "<label>Port</label><input type='number' min='1' max='65535' name='pport' value='");
  page += String(settings.printer_port);
  page += F(
      "'>"
      "<label>Serial number</label><input type='text' name='mqtt_sn' value='");
  page += html_escape(settings.mqtt_sn);
  page += F(
      "'>"
      "<label>LAN access code</label><input type='text' name='mqtt_pass' value='");
  page += html_escape(settings.mqtt_pass);
  page += F(
      "'>"
      "</fieldset>"
      "<fieldset><legend>Stocks</legend>"
      "<label>Symbols (up to 7, comma separated)</label>"
      "<input type='text' name='stocks' placeholder='AAPL,NVDA,^IXIC' value='");
  page += html_escape(settings.stock_symbols);
  page += F(
      "'>"
      "<label style='margin-top:2px;font-size:11px'>Yahoo Finance symbols. Indexes start with ^.</label>"
      "</fieldset>"
      "<fieldset><legend>Calendar</legend>");
  for (int i = 0; i < ICS_MAX_FEEDS; i++)
  {
    page += "<label>iCal URL " + String(i + 1) + "</label><input type='text' name='";
    page += ICS_PREF_KEYS[i];
    page += "' placeholder='https://calendar.google.com/calendar/ical/.../basic.ics' value='";
    page += html_escape(settings.ics_urls[i]);
    page += "'>";
  }
  page += F(
      "<label style='margin-top:2px;font-size:11px'>Up to 3 .ics links, merged. Google Calendar: Settings -&gt; "
      "your calendar -&gt; Integrate calendar -&gt; Secret address in iCal format (treat it like a password). "
      "Flights in a title like <i>AC871 YUL-&gt;YYZ</i> are pinned on the Home screen with a countdown.</label>"
      "</fieldset>"
      "<fieldset><legend>MakerWorld</legend>"
      "<label>User ID</label><input type='text' name='mwuid' inputmode='numeric' value='");
  page += html_escape(settings.makerworld_uid);
  page += F(
      "'>"
      "<label style='margin-top:2px;font-size:11px'>The number in your profile's published-models address "
      "(/design-service/published/&lt;number&gt;/design).</label>"
      "</fieldset>"
      "<fieldset><legend>Package tracking</legend>"
      "<label>PKGE API key</label><input type='password' name='pkge' autocomplete='off' placeholder='");
  page += settings.pkge_api_key[0] ? F("saved - leave empty to keep") : F("not set");
  page += F(
      "'>"
      "<label style='margin-top:2px;font-size:11px'>From business.pkge.net -&gt; Company profile -&gt; API key. "
      "Add tracking numbers in the PKGE dashboard. Enter - to remove the key.</label>"
      "</fieldset>"
      "<fieldset><legend>Live flight status (optional)</legend>"
      "<label>AeroDataBox API key</label><input type='password' name='adbkey' autocomplete='off' placeholder='");
  page += settings.aerodatabox_key[0] ? F("saved - leave empty to keep") : F("not set");
  page += F(
      "'>"
      "<label style='margin-top:2px;font-size:11px'>Free key: rapidapi.com -&gt; AeroDataBox -&gt; Basic plan. "
      "On the day of a flight in your calendar, shows delays, gate and departed/landed. Enter - to remove the key.</label>"
      "</fieldset>"
      "<fieldset><legend>Proxmox cluster</legend>"
      "<label>Host (ip:port)</label><input type='text' name='pxhost' placeholder='192.168.1.50:8006' value='");
  page += html_escape(settings.proxmox_host);
  page += F(
      "'>"
      "<label>API token ID</label><input type='text' name='pxtok' placeholder='root@pam!dashboard' value='");
  page += html_escape(settings.proxmox_token_id);
  page += F(
      "'>"
      "<label>API token secret</label><input type='password' name='pxsec' value='");
  page += html_escape(settings.proxmox_token_secret);
  page += F(
      "'>"
      "<label style='margin-top:2px;font-size:11px'>Create a token under Datacenter -&gt; Permissions -&gt; API "
      "Tokens. A PVEAuditor-only token (read-only, \"Privilege Separation\" left on) is enough to show cluster "
      "status here.</label>"
      "</fieldset>"
      "<button type='submit'>Save</button>"
      "</form>"
      "<script>"
      "var saved='");
  page += js_escape(settings.sta_ssid);
  page += F(
      "';"
      "fetch('/scan').then(r=>r.json()).then(list=>{"
      "var sel=document.getElementById('ssid');sel.innerHTML='';"
      "var found=false;"
      "list.sort((a,b)=>b.rssi-a.rssi);"
      "list.forEach(n=>{var o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';"
      "if(n.ssid===saved){o.selected=true;found=true;}sel.appendChild(o);});"
      "if(saved && !found){var o=document.createElement('option');o.value=saved;o.textContent=saved+' (saved, not in range)';o.selected=true;sel.insertBefore(o,sel.firstChild);}"
      "});"
      "</script>"
      "</body></html>");
  return page;
}

void handle_root()
{
  config_server.send(200, "text/html", build_settings_page());
}

void handle_scan()
{
  int n = WiFi.scanNetworks();
  String json = "[";
  for (int i = 0; i < n; i++)
  {
    if (i) json += ",";
    json += "{\"ssid\":\"" + html_escape(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
  }
  json += "]";
  config_server.send(200, "application/json", json);
  WiFi.scanDelete();
}

void handle_save_post()
{
  if (config_server.hasArg("tz")) config_server.arg("tz").toCharArray(settings.tz_name, sizeof(settings.tz_name));
  if (config_server.hasArg("wloc"))
  {
    String loc = config_server.arg("wloc");
    loc.trim();
    loc.toCharArray(settings.weather_location, sizeof(settings.weather_location));
  }
  if (config_server.hasArg("pname")) config_server.arg("pname").toCharArray(settings.printer_name, sizeof(settings.printer_name));
  if (config_server.hasArg("pip")) config_server.arg("pip").toCharArray(settings.printer_ip, sizeof(settings.printer_ip));
  if (config_server.hasArg("pport")) settings.printer_port = config_server.arg("pport").toInt();
  if (config_server.hasArg("mqtt_sn")) config_server.arg("mqtt_sn").toCharArray(settings.mqtt_sn, sizeof(settings.mqtt_sn));
  if (config_server.hasArg("mqtt_pass")) config_server.arg("mqtt_pass").toCharArray(settings.mqtt_pass, sizeof(settings.mqtt_pass));
  if (config_server.hasArg("ssid")) config_server.arg("ssid").toCharArray(settings.sta_ssid, sizeof(settings.sta_ssid));
  if (config_server.hasArg("pass")) config_server.arg("pass").toCharArray(settings.sta_pass, sizeof(settings.sta_pass));
  if (config_server.hasArg("stocks")) config_server.arg("stocks").toCharArray(settings.stock_symbols, sizeof(settings.stock_symbols));
  for (int i = 0; i < ICS_MAX_FEEDS; i++)
  {
    if (!config_server.hasArg(ICS_PREF_KEYS[i])) continue;
    String url = config_server.arg(ICS_PREF_KEYS[i]);
    url.trim();
    url.toCharArray(settings.ics_urls[i], sizeof(settings.ics_urls[i]));
  }
  if (config_server.hasArg("mwuid"))
  {
    String uid = config_server.arg("mwuid");
    uid.trim();
    uid.toCharArray(settings.makerworld_uid, sizeof(settings.makerworld_uid));
  }
  // API keys: empty keeps the saved key (the page never echoes it back), "-" removes it.
  if (config_server.hasArg("pkge"))
  {
    String key = config_server.arg("pkge");
    key.trim();
    if (key == "-") settings.pkge_api_key[0] = '\0';
    else if (key.length() > 0) key.toCharArray(settings.pkge_api_key, sizeof(settings.pkge_api_key));
  }
  if (config_server.hasArg("adbkey"))
  {
    String key = config_server.arg("adbkey");
    key.trim();
    if (key == "-") settings.aerodatabox_key[0] = '\0';
    else if (key.length() > 0) key.toCharArray(settings.aerodatabox_key, sizeof(settings.aerodatabox_key));
  }
  if (config_server.hasArg("pxhost")) config_server.arg("pxhost").toCharArray(settings.proxmox_host, sizeof(settings.proxmox_host));
  if (config_server.hasArg("pxtok")) config_server.arg("pxtok").toCharArray(settings.proxmox_token_id, sizeof(settings.proxmox_token_id));
  if (config_server.hasArg("pxsec")) config_server.arg("pxsec").toCharArray(settings.proxmox_token_secret, sizeof(settings.proxmox_token_secret));
  save_settings();

  config_server.send(200, "text/html",
                      "<html><body style='background:#121212;color:#fff;font-family:sans-serif;text-align:center;padding-top:60px'>"
                      "<h2 style='color:#00BCD4'>Saved</h2><p>The device is restarting to apply your settings.</p></body></html>");

  // A full reboot - rather than the previous piecemeal "leave the AP,
  // reconnect Wi-Fi, resync the clock, refetch the calendar" apply-in-place
  // dance - guarantees every setting (Wi-Fi, timezone, MQTT, calendar,
  // MakerWorld ID, API keys, ...) gets applied through the one code path that already
  // knows how (setup()), instead of needing a bespoke live-apply step added
  // here for every new setting. delay() first so the response above
  // actually reaches the browser before the connection drops.
  Serial.println("Settings saved via web form - restarting...");
  delay(1000);
  ESP.restart();
}

void start_config_server()
{
  config_server.on("/", handle_root);
  config_server.on("/scan", handle_scan);
  config_server.on("/save", HTTP_POST, handle_save_post);
  // Catch-all: iOS/Android/Windows each probe a different well-known URL
  // (e.g. captive.apple.com/hotspot-detect.html, connectivitycheck.gstatic.com)
  // right after joining a Wi-Fi network. Serving our page for *any* unmatched
  // path - combined with the DNS server below redirecting every hostname to
  // us - makes those probes fail "as expected" for a captive portal, which is
  // exactly what makes the OS auto-open a browser to this page.
  config_server.onNotFound(handle_root);
  config_server.begin();

  // Redirect all DNS queries from AP clients to our own IP, regardless of
  // hostname - the other half of the captive-portal trick above.
  dns_server.start(53, "*", WiFi.softAPIP());
}

// ==================== Settings (long-press top-right corner) ====================
static bool ap_enabled = false;
static uint8_t screen_before_settings = 0;
static lv_obj_t *settings_screen;
static lv_obj_t *cal_day_screen; // Calendar day-detail screen - see calendar.ino
static lv_obj_t *ap_switch;
static lv_obj_t *auto_bambu_switch;
static lv_obj_t *ap_status_lab;
static lv_obj_t *idle_timeout_dd;
// Selectable idle-return-to-Home timeouts, in minutes - index into this
// array is what the dropdown widget actually stores/reports.
static const uint8_t IDLE_TIMEOUT_OPTIONS_MIN[4] = {1, 2, 5, 10};
static lv_obj_t *page_dots[SCREEN_COUNT];

void update_page_indicator(uint8_t active)
{
  for (int i = 0; i < SCREEN_COUNT; i++)
  {
    lv_obj_set_style_bg_color(page_dots[i], i == active ? COLOR_PRIMARY : COLOR_TEXT_MUTED, LV_PART_MAIN);
    if (screen_available(i) || i == active) lv_obj_clear_flag(page_dots[i], LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(page_dots[i], LV_OBJ_FLAG_HIDDEN);
  }
}

void update_ap_status_label()
{
  if (ap_enabled)
  {
    lv_label_set_text_fmt(ap_status_lab, "On - %s / %s (%s)", AP_SSID, AP_PASSWORD, AP_IP_STR);
  }
  else
  {
    lv_label_set_text(ap_status_lab, "Off");
  }
}

void close_settings_screen()
{
  in_settings = false;
  if (page_dots_row) lv_obj_clear_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN);
  switch_screen(screen_before_settings, LV_SCR_LOAD_ANIM_NONE);
}

// Starts/stops the setup AP + its web portal immediately - no separate Save
// step. Both callers (ap_switch_toggled_cb, settings_close_cb) are on the
// LVGL thread - it's not called from network_task/handle_save_post, since a
// web-form Save now reboots the device instead of applying settings live
// (see handle_save_post()).
void set_ap_enabled(bool on)
{
  if (on == ap_enabled) return;
  ap_enabled = on;

  if (ap_enabled)
  {
    // No WiFi.mode()/softAPConfig() here - softAP() sets the mode itself and
    // the default AP IP is already 192.168.4.1. Matches the known-working
    // pattern from the earlier e-paper project; the extra config calls were
    // one more thing that could go wrong for no benefit.
    bool ok = WiFi.softAP(AP_SSID, AP_PASSWORD);
    start_config_server();
    Serial.printf("AP %s - SSID: %s  IP: %s  free heap: %u\n",
                   ok ? "started" : "FAILED TO START",
                   AP_SSID, WiFi.softAPIP().toString().c_str(), ESP.getFreeHeap());
  }
  else
  {
    config_server.stop();
    dns_server.stop();
    WiFi.softAPdisconnect(true);
    Serial.println("AP stopped");
  }
}

void ap_switch_toggled_cb(lv_event_t *e)
{
  set_ap_enabled(lv_obj_has_state(ap_switch, LV_STATE_CHECKED));
  update_ap_status_label(); // safe here - this callback runs on the LVGL thread
}

void auto_bambu_switch_toggled_cb(lv_event_t *e)
{
  settings.auto_show_bambu_on_start = lv_obj_has_state(auto_bambu_switch, LV_STATE_CHECKED);
  save_settings(); // this on-device toggle applies live immediately - unlike a web-form Save, which reboots (see handle_save_post())
}

// Index into IDLE_TIMEOUT_OPTIONS_MIN closest to a stored minute value -
// used to select the right dropdown entry even if a future firmware ever
// changes the option list (falls back to the array's last entry rather
// than crashing on an unmatched legacy value).
uint16_t idle_timeout_min_to_index(uint8_t minutes)
{
  for (uint16_t i = 0; i < 4; i++)
    if (IDLE_TIMEOUT_OPTIONS_MIN[i] == minutes) return i;
  return 2; // default option (5 min)
}

void idle_timeout_dd_changed_cb(lv_event_t *e)
{
  uint16_t sel = lv_dropdown_get_selected(idle_timeout_dd);
  if (sel < 4) settings.idle_return_home_min = IDLE_TIMEOUT_OPTIONS_MIN[sel];
  save_settings(); // on-device control, applies live immediately - same as auto_bambu_switch_toggled_cb above
}

void restart_btn_clicked_cb(lv_event_t *e)
{
  Serial.println("Restarting (user requested from Settings)...");
  delay(100); // let the serial line flush before the reset
  ESP.restart();
}

// Connects to the home Wi-Fi network saved via the setup portal, if any.
// Called both at boot and whenever Settings is closed (credentials may have
// just changed). Non-blocking - actual connection result comes back via
// on_wifi_event()/wifi_got_ip in loop().
void connect_saved_wifi()
{
  if (settings.sta_ssid[0] == '\0') return;
  WiFi.mode(WIFI_STA);
  // Lower TX power than the default (WIFI_POWER_19_5dBm) - full power draws a
  // current spike on transmit that this board's shared 5V rail doesn't like,
  // showing up as blinking lines / brief freezes on the RGB panel during
  // network activity. The earlier reason-204 handshake drops turned out to
  // be an AP-side issue, not weak signal on this end, so this is safe to
  // re-add - 15dBm is still plenty for typical home Wi-Fi range.
  WiFi.setTxPower(WIFI_POWER_15dBm);
  WiFi.begin(settings.sta_ssid, settings.sta_pass);
  Serial.printf("Connecting to Wi-Fi: %s\n", settings.sta_ssid);
}

// Runs on the Wi-Fi driver's own task, not the LVGL loop - so it only sets a
// flag here and leaves any LVGL/label updates to loop() on the main thread.
void on_wifi_event(WiFiEvent_t event, WiFiEventInfo_t info)
{
  switch (event)
  {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("Wi-Fi connected, IP: %s  GW: %s  Subnet: %s  DNS: %s / %s\n",
                     WiFi.localIP().toString().c_str(),
                     WiFi.gatewayIP().toString().c_str(),
                     WiFi.subnetMask().toString().c_str(),
                     WiFi.dnsIP(0).toString().c_str(),
                     WiFi.dnsIP(1).toString().c_str());
      wifi_got_ip = true;
      wifi_connected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("Wi-Fi disconnected (reason %d)\n", info.wifi_sta_disconnected.reason);
      wifi_connected = false;
      break;
    default:
      break;
  }
}

static volatile bool clock_needs_ui_refresh = false;
static bool clock_synced_once = false;

// Pulls current time over NTP and updates the demo clock's running state.
// Runs on network_task - only touches plain vars + a flag; the Home screen
// labels are updated from loop() (the LVGL-owning thread) when
// clock_needs_ui_refresh is set, via clock_apply_to_ui(). Returns whether it
// succeeded so network_task can retry sooner than the normal hourly cadence
// while it hasn't gotten a first successful sync yet (e.g. right after boot,
// if Wi-Fi/NTP isn't reachable yet on the first attempt).
bool sync_clock_ntp()
{
  const char *posix_tz = tz_name_to_posix(settings.tz_name);
  // configTzTime (not configTime(0,0,...)) - configTime() with numeric
  // offsets internally calls setTimeZone()->setenv("TZ", "UTC0", 1), which
  // was silently clobbering our POSIX TZ string back to plain UTC right
  // after we set it (confirmed in esp32-hal-time.c). configTzTime() applies
  // the TZ string correctly and still runs SNTP the same way.
  configTzTime(posix_tz, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 5000))
  {
    demo_hour = timeinfo.tm_hour;
    demo_min = timeinfo.tm_min;
    demo_sec = timeinfo.tm_sec;
    demo_wday = timeinfo.tm_wday;
    demo_mday = timeinfo.tm_mday;
    demo_mon = timeinfo.tm_mon;
    demo_year = timeinfo.tm_year + 1900;
    clock_needs_ui_refresh = true;
    // The Calendar screen's month grid (today ring, day-of-week layout) is
    // derived from these same fields, so it needs the same refresh - even
    // when no iCal URL is configured, the grid itself should
    // still track the real date instead of staying on its placeholder.
    calendar_needs_ui_refresh = true;
    clock_synced_once = true;
    Serial.printf("Clock synced via NTP: %04d-%02d-%02d %02d:%02d:%02d local (tz_name=%s -> posix=%s, isdst=%d)\n",
                   timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                   timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec,
                   settings.tz_name, posix_tz, timeinfo.tm_isdst);
    return true;
  }
  else
  {
    Serial.printf("NTP sync failed (WiFi.status()=%d, tz_name=%s -> posix=%s)\n",
                   WiFi.status(), settings.tz_name, posix_tz);
    return false;
  }
}

static const char *WEEKDAY_NAMES[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *MONTH_NAMES[12] = {"January", "February", "March", "April", "May", "June",
                                       "July", "August", "September", "October", "November", "December"};

void clock_apply_to_ui()
{
  if (home_hour_label) lv_label_set_text_fmt(home_hour_label, "%02d", demo_hour);
  if (home_min_label) lv_label_set_text_fmt(home_min_label, "%02d", demo_min);
  if (home_date_label) lv_label_set_text_fmt(home_date_label, "%s, %s %d", WEEKDAY_NAMES[demo_wday % 7], MONTH_NAMES[demo_mon % 12], demo_mday);
}

// The only exit from Settings: turns the setup AP off (if on) and connects
// to whichever home Wi-Fi network was configured through the web portal.
void settings_close_cb(lv_event_t *e)
{
  set_ap_enabled(false);
  connect_saved_wifi();

  close_settings_screen();
}

// Fires after the touch driver's long_press_time (set to 5s in setup()).
void open_settings_cb(lv_event_t *e)
{
  if (in_settings) return;
  in_settings = true;
  screen_before_settings = current_screen;

  if (ap_enabled) lv_obj_add_state(ap_switch, LV_STATE_CHECKED);
  else lv_obj_clear_state(ap_switch, LV_STATE_CHECKED);
  update_ap_status_label();

  if (page_dots_row) lv_obj_add_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load_anim(settings_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// ==================== Shared helpers ====================

// Small rounded stat card with a big value on top and a muted caption below.
// Used for weather stats and printer temps/spool so the whole UI reads as one system.
lv_obj_t *make_stat_chip(lv_obj_t *parent, const char *value, const char *label, lv_color_t value_color, lv_obj_t **value_label_out = NULL, lv_coord_t chip_width = 132, const lv_font_t *value_font = &lv_font_montserrat_22, lv_flex_align_t main_place = LV_FLEX_ALIGN_CENTER, lv_coord_t chip_height = 74)
{
  lv_obj_t *chip = lv_obj_create(parent);
  lv_obj_set_size(chip, chip_width, chip_height);
  lv_obj_set_style_bg_color(chip, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_border_width(chip, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(chip, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);
  lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_COLUMN);
  // main_place defaults to CENTER (the original look, used by the weather
  // chips). The Bambu NOZZLE chip passes END instead, since make_stat_chip2's
  // two-value chips next to it are taller - bottom-aligning both is what
  // keeps every chip's caption on the same line across that row.
  lv_obj_set_flex_align(chip, main_place, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *val = lv_label_create(chip);
  lv_label_set_text(val, value);
  lv_obj_set_style_text_color(val, value_color, LV_PART_MAIN);
  lv_obj_set_style_text_font(val, value_font, LV_PART_MAIN);
  if (value_label_out) *value_label_out = val;

  lv_obj_t *lab = lv_label_create(chip);
  lv_label_set_text(lab, label);
  lv_obj_set_style_text_color(lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(lab, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(lab, 1, LV_PART_MAIN);

  return chip;
}

// Same chip shell as make_stat_chip() above, but with two stacked value
// lines instead of one - for chips that pack two related readings into the
// same slot (e.g. current/total layer, AMS temp/humidity). Pass label=NULL
// to skip the muted caption line entirely (when the two value lines already
// carry their own text) rather than sharing one caption below both - three
// stacked lines plus a caption doesn't fit this chip's 74px height
// comfortably, so the caller picks whichever fits.
lv_obj_t *make_stat_chip2(lv_obj_t *parent, const char *line1, const char *line2, const char *label,
                           lv_color_t value_color, lv_obj_t **line1_out = NULL, lv_obj_t **line2_out = NULL,
                           lv_coord_t chip_width = 132, const lv_font_t *value_font = &lv_font_montserrat_16)
{
  lv_obj_t *chip = lv_obj_create(parent);
  lv_obj_set_size(chip, chip_width, 74);
  lv_obj_set_style_bg_color(chip, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_border_width(chip, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(chip, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);
  lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_COLUMN);
  // Bottom-aligned so this chip's last line lands on the same Y as the
  // caption in make_stat_chip's single-value chips, even though this one
  // has an extra value line above it - keeps the row's titles/last lines
  // flush across chips of different content height.
  lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *l1 = lv_label_create(chip);
  lv_label_set_text(l1, line1);
  lv_obj_set_style_text_color(l1, value_color, LV_PART_MAIN);
  lv_obj_set_style_text_font(l1, value_font, LV_PART_MAIN);
  if (line1_out) *line1_out = l1;

  lv_obj_t *l2 = lv_label_create(chip);
  lv_label_set_text(l2, line2);
  lv_obj_set_style_text_color(l2, value_color, LV_PART_MAIN);
  lv_obj_set_style_text_font(l2, value_font, LV_PART_MAIN);
  if (line2_out) *line2_out = l2;

  if (label)
  {
    lv_obj_t *lab = lv_label_create(chip);
    lv_label_set_text(lab, label);
    lv_obj_set_style_text_color(lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(lab, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_letter_space(lab, 1, LV_PART_MAIN);
  }

  return chip;
}

// Small rounded status badge, e.g. "PRINTING" / "IN TRANSIT".
lv_obj_t *make_pill(lv_obj_t *parent, const char *text, lv_color_t bg_color, lv_color_t text_color, lv_obj_t **text_label_out = NULL)
{
  lv_obj_t *pill = lv_obj_create(parent);
  lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(pill, bg_color, LV_PART_MAIN);
  lv_obj_set_style_border_width(pill, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(pill, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(pill, 5, LV_PART_MAIN);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lab = lv_label_create(pill);
  lv_label_set_text(lab, text);
  lv_obj_set_style_text_color(lab, text_color, LV_PART_MAIN);
  lv_obj_set_style_text_font(lab, &lv_font_montserrat_14, LV_PART_MAIN);
  if (text_label_out) *text_label_out = lab;

  return pill;
}

// ==================== Weather icon ====================
// Transparent container for one icon part.
static lv_obj_t *wicon_group(lv_obj_t *parent)
{
  lv_obj_t *g = lv_obj_create(parent);
  lv_obj_set_size(g, 210, 130);
  lv_obj_set_pos(g, 0, 0);
  lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(g, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(g, 0, LV_PART_MAIN);
  lv_obj_clear_flag(g, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
  return g;
}

static lv_obj_t *wicon_circle(lv_obj_t *parent, lv_coord_t cx, lv_coord_t cy, lv_coord_t r, lv_color_t color)
{
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_size(c, 2 * r, 2 * r);
  lv_obj_set_pos(c, cx - r, cy - r);
  lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(c, color, LV_PART_MAIN);
  lv_obj_set_style_border_width(c, 0, LV_PART_MAIN);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
  return c;
}

static lv_obj_t *wicon_rect(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h, lv_coord_t radius, lv_color_t color)
{
  lv_obj_t *r = lv_obj_create(parent);
  lv_obj_set_size(r, w, h);
  lv_obj_set_pos(r, x, y);
  lv_obj_set_style_radius(r, radius, LV_PART_MAIN);
  lv_obj_set_style_bg_color(r, color, LV_PART_MAIN);
  lv_obj_set_style_border_width(r, 0, LV_PART_MAIN);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_CLICKABLE);
  return r;
}

// Straight rounded line from (x1,y1) to (x2,y2). The two points need
// static storage (LVGL keeps the pointer), so they come from a pool.
static lv_obj_t *wicon_line(lv_obj_t *parent, lv_coord_t x1, lv_coord_t y1, lv_coord_t x2, lv_coord_t y2, lv_coord_t width, lv_color_t color)
{
  static lv_point_t pool[32][2];
  static int used = 0;
  if (used >= 32) return NULL;
  lv_point_t *pts = pool[used++];
  pts[0].x = x1; pts[0].y = y1;
  pts[1].x = x2; pts[1].y = y2;
  lv_obj_t *l = lv_line_create(parent);
  lv_line_set_points(l, pts, 2);
  lv_obj_set_style_line_color(l, color, LV_PART_MAIN);
  lv_obj_set_style_line_width(l, width, LV_PART_MAIN);
  lv_obj_set_style_line_rounded(l, true, LV_PART_MAIN);
  return l;
}

// The "classic puffy" cloud: four bumps over a flat base with round ends.
// Designed in a 142x76 box at scale 100 (%), placed with its top-left at (x, y).
static lv_obj_t *make_cloud(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, int scale, lv_color_t color)
{
  lv_obj_t *g = wicon_group(parent);
  auto S = [scale](int v) -> lv_coord_t { return (lv_coord_t)(v * scale / 100); };
  wicon_circle(g, x + S(22), y + S(54), S(22), color);
  wicon_circle(g, x + S(52), y + S(36), S(28), color);
  wicon_circle(g, x + S(88), y + S(32), S(32), color);
  wicon_circle(g, x + S(120), y + S(54), S(22), color);
  wicon_rect(g, x + S(22), y + S(46), S(98), S(30), 0, color);
  return g;
}

// Sun: disc with `rays` rays starting at `first_deg` every 45 degrees.
static lv_obj_t *make_sun(lv_obj_t *parent, lv_coord_t cx, lv_coord_t cy, lv_coord_t r, int first_deg, int rays)
{
  lv_obj_t *g = wicon_group(parent);
  for (int i = 0; i < rays; i++)
  {
    float a = (first_deg + i * 45) * 3.14159265f / 180.0f;
    float c = cosf(a), s = sinf(a);
    wicon_line(g, cx + (lv_coord_t)(c * (r + 10)), cy + (lv_coord_t)(s * (r + 10)),
               cx + (lv_coord_t)(c * (r + 22)), cy + (lv_coord_t)(s * (r + 22)), 6, lv_color_hex(0xFFC947));
  }
  lv_obj_t *disc = wicon_circle(g, cx, cy, r, lv_color_hex(0xFFC947));
  lv_obj_set_style_bg_grad_color(disc, COLOR_SECONDARY, LV_PART_MAIN);
  lv_obj_set_style_bg_grad_dir(disc, LV_GRAD_DIR_VER, LV_PART_MAIN);
  return g;
}

// Crescent moon: a disc with a background-colored disc over its upper right.
static lv_obj_t *make_moon(lv_obj_t *parent, lv_coord_t cx, lv_coord_t cy, lv_coord_t r, bool stars)
{
  lv_obj_t *g = wicon_group(parent);
  wicon_circle(g, cx, cy, r, lv_color_hex(0xFFF3C4));
  wicon_circle(g, cx + r * 52 / 100, cy - r * 40 / 100, r * 90 / 100, lv_color_hex(0x1A222B)); // Home screen background
  if (stars)
  {
    wicon_circle(g, cx + 55, cy - 35, 3, lv_color_hex(0xFFF3C4));
    wicon_circle(g, cx + 67, cy + 7, 3, lv_color_hex(0xFFF3C4));
    wicon_circle(g, cx - 47, cy - 37, 3, lv_color_hex(0xFFF3C4));
  }
  return g;
}

// Builds every part of the weather icon in one 210x130 container; the
// current condition is chosen by set_weather_icon(). Coordinates match the
// design preview (sun/moon top-left, cloud lower right, weather below).
void build_weather_icon(lv_obj_t *weather_panel)
{
  lv_obj_t *icon = lv_obj_create(weather_panel);
  lv_obj_set_size(icon, 210, 130);
  lv_obj_set_pos(icon, 8, 18);
  lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(icon, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(icon, 0, LV_PART_MAIN);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

  // Clear: big sun (8 rays) / moon with stars, centered.
  wicon_sun_full = make_sun(icon, 105, 65, 32, 0, 8);
  wicon_moon_full = make_moon(icon, 105, 65, 38, true);

  // Partly cloudy: small sun (rays only where the cloud doesn't cover it) or moon, cloud in front.
  wicon_sun_small = make_sun(icon, 66, 52, 26, 135, 4);
  wicon_moon_small = make_moon(icon, 70, 50, 28, false);
  wicon_cloud_partly = make_cloud(icon, 56, 44, 100, COLOR_CLOUD);

  // Overcast: darker cloud behind, white cloud in front.
  wicon_cloud_back = make_cloud(icon, 92, 6, 70, lv_color_hex(0x90A4AE));
  wicon_cloud_front = make_cloud(icon, 34, 40, 92, COLOR_CLOUD);

  // Rain/drizzle/snow/storm/fog: cloud at the top, weather underneath.
  wicon_cloud_top = make_cloud(icon, 38, 16, 92, lv_color_hex(0xB0BEC5));

  static const lv_coord_t rain_x[4] = {70, 94, 118, 142};
  for (int i = 0; i < 4; i++)
  {
    wicon_rain[i] = wicon_line(icon, rain_x[i], 100, rain_x[i] - 6, 120, 6, lv_color_hex(0x4FC3F7));
    wicon_show(wicon_rain[i], false);
  }

  static const lv_coord_t snow_xy[6][2] = {{70, 102}, {100, 102}, {130, 102}, {85, 118}, {115, 118}, {145, 118}};
  for (int i = 0; i < 6; i++)
  {
    wicon_snow[i] = wicon_circle(icon, snow_xy[i][0], snow_xy[i][1], 5, lv_color_hex(0xFFFFFF));
    wicon_show(wicon_snow[i], false);
  }

  for (int i = 0; i < 3; i++)
  {
    wicon_fog_bars[i] = wicon_rect(icon, i == 1 ? 58 : 44, 94 + i * 12, 120, 8, 4, COLOR_CLOUD);
    lv_obj_set_style_bg_opa(wicon_fog_bars[i], i == 0 ? LV_OPA_80 : (i == 1 ? LV_OPA_60 : LV_OPA_40), LV_PART_MAIN);
    wicon_show(wicon_fog_bars[i], false);
  }

  // Lightning: thick zigzag under the storm cloud.
  static lv_point_t bolt_pts[4] = {{114, 92}, {98, 110}, {112, 110}, {100, 128}};
  wicon_bolt = lv_line_create(icon);
  lv_line_set_points(wicon_bolt, bolt_pts, 4);
  lv_obj_set_style_line_color(wicon_bolt, COLOR_WARNING, LV_PART_MAIN);
  lv_obj_set_style_line_width(wicon_bolt, 7, LV_PART_MAIN);
  lv_obj_set_style_line_rounded(wicon_bolt, true, LV_PART_MAIN);
  wicon_show(wicon_bolt, false);

  set_weather_icon(WICON_PARTLY_CLOUDY, true); // placeholder until the first weather fetch
}

// ==================== Screen 1: Home (date/time + weather) ====================
lv_obj_t *create_home_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x1A222B), LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // ---- Clock panel: top 40% of the 480px screen ----
  lv_obj_t *clock_panel = lv_obj_create(scr);
  // Trimmed from 192 - shifts the (CENTER-aligned) time/date up a bit and
  // hands the freed strip to weather_panel below, for the next-event card.
  lv_obj_set_size(clock_panel, 480, 180);
  lv_obj_set_pos(clock_panel, 0, 0);
  lv_obj_set_style_bg_opa(clock_panel, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(clock_panel, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(clock_panel, 0, LV_PART_MAIN);
  lv_obj_clear_flag(clock_panel, LV_OBJ_FLAG_SCROLLABLE);

  // Hour/minute each get a fixed-width slot so the digits never shift the
  // layout when they change between one and two visually-wide characters.
  // Slot width/row height/offsets below are scaled ~0.9x from their old
  // lv_font_roboto_bold_94 values, matching that font's narrower, shorter
  // lv_font_roboto_regular_86 replacement (adv_w 773 vs 863, line_height 63
  // vs 70 - see lv_font_roboto_regular_86.c / lv_font_roboto_bold_94.c).
  lv_obj_t *time_row = lv_obj_create(clock_panel);
  lv_obj_set_size(time_row, 420, 90);
  lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(time_row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(time_row, 0, LV_PART_MAIN);
  lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(time_row, 0, LV_PART_MAIN);
  lv_obj_align(time_row, LV_ALIGN_CENTER, 0, -13);

  lv_obj_t *hour_val = lv_label_create(time_row);
  lv_obj_set_width(hour_val, 117);
  lv_obj_set_style_text_align(hour_val, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(hour_val, "14");
  lv_obj_set_style_text_color(hour_val, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(hour_val, &lv_font_roboto_regular_86, LV_PART_MAIN);

  lv_obj_t *colon_val = lv_label_create(time_row);
  lv_label_set_text(colon_val, ":");
  lv_obj_set_style_text_color(colon_val, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(colon_val, &lv_font_roboto_regular_86, LV_PART_MAIN);

  lv_obj_t *min_val = lv_label_create(time_row);
  lv_obj_set_width(min_val, 117);
  lv_obj_set_style_text_align(min_val, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(min_val, "32");
  lv_obj_set_style_text_color(min_val, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(min_val, &lv_font_roboto_regular_86, LV_PART_MAIN);

  home_hour_label = hour_val;
  home_min_label = min_val;

  lv_obj_t *divider = lv_obj_create(clock_panel);
  lv_obj_set_size(divider, 50, 3);
  lv_obj_set_style_bg_color(divider, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_border_width(divider, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(divider, 2, LV_PART_MAIN);
  lv_obj_align(divider, LV_ALIGN_CENTER, 0, 43);
  lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *date_val = lv_label_create(clock_panel);
  lv_label_set_text(date_val, "Saturday, August 15"); // placeholder until clock_apply_to_ui() runs post-NTP-sync
  lv_obj_set_style_text_color(date_val, lv_color_hex(0xB0BEC5), LV_PART_MAIN);
  lv_obj_set_style_text_font(date_val, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_align(date_val, LV_ALIGN_CENTER, 0, 65);
  home_date_label = date_val;

  // ---- Weather panel: bottom 60% of the screen ----
  lv_obj_t *weather_panel = lv_obj_create(scr);
  lv_obj_set_size(weather_panel, 480, 300);
  lv_obj_set_pos(weather_panel, 0, 180);
  lv_obj_set_style_bg_opa(weather_panel, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(weather_panel, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(weather_panel, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(weather_panel, 0, LV_PART_MAIN);
  lv_obj_clear_flag(weather_panel, LV_OBJ_FLAG_SCROLLABLE);

  // Bambu print-status badge - hidden by default, shown/updated by
  // update_home_bambu_badge() (see bambu_apply_to_ui()). Placed in the empty
  // strip above the weather icon (which starts at y=30 within this panel),
  // clear of the clock above and the icon/temperature below.
  lv_obj_t *bambu_badge_lab = NULL;
  home_bambu_badge = make_pill(weather_panel, "PRINTING", COLOR_SECONDARY, lv_color_hex(0x2B1600), &bambu_badge_lab);
  lv_obj_align(home_bambu_badge, LV_ALIGN_TOP_LEFT, 20, 4);
  home_bambu_badge_lab = bambu_badge_lab;
  lv_obj_add_flag(home_bambu_badge, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(home_bambu_badge, bambu_badge_clicked_cb, LV_EVENT_CLICKED, NULL);

  // Proxmox "node offline" badge - hidden by default, shown/updated by
  // update_home_proxmox_badge() (see proxmox_apply_to_ui() in proxmox.ino).
  // Top-right instead of the Bambu badge's top-left so a print running at
  // the same time a node drops doesn't stack the two badges on top of
  // each other.
  lv_obj_t *proxmox_badge_lab = NULL;
  home_proxmox_badge = make_pill(weather_panel, "NODE OFFLINE", lv_color_hex(0xEF5350), lv_color_hex(0xFFFFFF), &proxmox_badge_lab);
  lv_obj_align(home_proxmox_badge, LV_ALIGN_TOP_RIGHT, -20, 4);
  home_proxmox_badge_lab = proxmox_badge_lab;
  lv_obj_add_flag(home_proxmox_badge, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(home_proxmox_badge, proxmox_badge_clicked_cb, LV_EVENT_CLICKED, NULL);

  build_weather_icon(weather_panel);

  lv_obj_t *temp = lv_label_create(weather_panel);
  lv_label_set_text(temp, "24°C");
  lv_obj_set_style_text_color(temp, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(temp, &lv_font_roboto_regular_86, LV_PART_MAIN);
  lv_obj_align(temp, LV_ALIGN_TOP_RIGHT, -24, 24);
  weather_temp_lab = temp;

  // y pulled up from 96 to 90 to keep the same tight ~2px gap under temp,
  // now that lv_font_roboto_regular_86's shorter line_height (63 vs the old
  // lv_font_roboto_bold_94's 70) leaves temp's own bottom edge higher up.
  lv_obj_t *desc = lv_label_create(weather_panel);
  lv_label_set_text(desc, "Partly Cloudy");
  lv_obj_set_style_text_color(desc, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(desc, &lv_font_montserrat_18, LV_PART_MAIN);
  lv_obj_align(desc, LV_ALIGN_TOP_RIGHT, -24, 90);
  weather_desc_lab = desc;

  // Stats row
  lv_obj_t *stats = lv_obj_create(weather_panel);
  lv_obj_set_size(stats, 440, 80);
  // Shifted up to follow the icon's move and the panel growing taller
  // (clock_panel gave up 12px) - keeps a clean 16px gap under the icon.
  lv_obj_align(stats, LV_ALIGN_BOTTOM_MID, 0, -56);
  lv_obj_set_style_bg_opa(stats, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(stats, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(stats, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(stats, LV_OBJ_FLAG_SCROLLABLE);

  // Slightly smaller than make_stat_chip()'s default 132x74 - just for
  // this row, via the trailing chip_height param (appended after
  // main_place rather than inserted earlier, so bambu.ino's fully-
  // positional NOZZLE chip call doesn't shift and break).
  make_stat_chip(stats, "58%", "HUMIDITY", COLOR_PRIMARY, &weather_humidity_val, 110, &lv_font_montserrat_22, LV_FLEX_ALIGN_CENTER, 62);
  make_stat_chip(stats, "12 km/h", "WIND", COLOR_PRIMARY, &weather_wind_val, 110, &lv_font_montserrat_22, LV_FLEX_ALIGN_CENTER, 62);
  make_stat_chip(stats, "4", "UV INDEX", COLOR_PRIMARY, &weather_uv_val, 110, &lv_font_montserrat_22, LV_FLEX_ALIGN_CENTER, 62);

  // Next-event card: the only free space left is the strip below the stats
  // row. A subtle rounded card with a colored accent bar reads better than
  // plain centered text. Text is set by calendar.ino's
  // update_home_next_event(), which runs on the same triggers as
  // calendar_apply_to_ui() (NTP sync, fresh calendar fetch) and once a
  // minute from clock_tick_cb(). A flight in the next 24 hours is pinned
  // here with a countdown (and live status with an AeroDataBox key).
  lv_obj_t *next_card = lv_obj_create(weather_panel);
  lv_obj_set_size(next_card, 440, 32);
  lv_obj_set_style_bg_color(next_card, lv_color_hex(0x232D38), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(next_card, LV_OPA_70, LV_PART_MAIN);
  lv_obj_set_style_border_width(next_card, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(next_card, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_all(next_card, 0, LV_PART_MAIN);
  lv_obj_clear_flag(next_card, LV_OBJ_FLAG_SCROLLABLE);
  // Pulled up with the rest of the cluster, staying a tight 4px under the
  // stats row's bottom edge; leaves generous clearance below it now.
  lv_obj_align(next_card, LV_ALIGN_BOTTOM_MID, 0, -20);

  lv_obj_t *next_accent = lv_obj_create(next_card);
  lv_obj_set_size(next_accent, 4, 18);
  lv_obj_set_style_bg_color(next_accent, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_border_width(next_accent, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(next_accent, 2, LV_PART_MAIN);
  lv_obj_clear_flag(next_accent, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(next_accent, LV_ALIGN_LEFT_MID, 12, 0);
  home_next_accent = next_accent;

  lv_obj_t *next_event_lab = lv_label_create(next_card);
  lv_label_set_text(next_event_lab, "");
  lv_obj_set_style_text_color(next_event_lab, lv_color_hex(0xE8EDF2), LV_PART_MAIN);
  lv_obj_set_style_text_font(next_event_lab, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_width(next_event_lab, 396);
  lv_obj_set_style_text_align(next_event_lab, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_label_set_long_mode(next_event_lab, LV_LABEL_LONG_DOT);
  lv_obj_align(next_event_lab, LV_ALIGN_LEFT_MID, 26, 0);
  home_next_event_lab = next_event_lab;

  return scr;
}

// Screen 1 (Bambu printer) moved to bambu.ino.
// Screen 2 (Calendar) moved to calendar.ino.
// Screen 3 (Proxmox) moved to proxmox.ino.
// Screen 4 (Stocks) moved to stocks.ino.
// Screen 5 (MakerWorld) moved to makerworld.ino.
// Screen 6 (Packages) lives in tracking.ino.

// ==================== Settings screen ====================
lv_obj_t *create_settings_screen()
{
  lv_obj_t *scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_pad_all(scr, 24, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(scr);
  lv_label_set_text(title, "Settings");
  lv_obj_set_style_text_color(title, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  // ---- Setup Access Point ----
  lv_obj_t *ap_row = lv_obj_create(scr);
  lv_obj_set_size(ap_row, 432, 90);
  lv_obj_align(ap_row, LV_ALIGN_TOP_LEFT, 0, 60);
  lv_obj_set_style_bg_color(ap_row, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(ap_row, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(ap_row, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ap_row, 16, LV_PART_MAIN);
  lv_obj_clear_flag(ap_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *ap_title = lv_label_create(ap_row);
  lv_label_set_text(ap_title, "Setup Access Point");
  lv_obj_set_style_text_color(ap_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(ap_title, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(ap_title, LV_ALIGN_TOP_LEFT, 0, 0);

  ap_switch = lv_switch_create(ap_row);
  lv_obj_set_style_bg_color(ap_switch, COLOR_PRIMARY, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_align(ap_switch, LV_ALIGN_TOP_RIGHT, 0, -4);
  lv_obj_add_event_cb(ap_switch, ap_switch_toggled_cb, LV_EVENT_VALUE_CHANGED, NULL);

  ap_status_lab = lv_label_create(ap_row);
  lv_label_set_text(ap_status_lab, "Off");
  lv_obj_set_style_text_color(ap_status_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(ap_status_lab, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(ap_status_lab, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  // ---- Auto-open Bambu screen when a print starts ----
  lv_obj_t *abos_row = lv_obj_create(scr);
  lv_obj_set_size(abos_row, 432, 70);
  lv_obj_align(abos_row, LV_ALIGN_TOP_LEFT, 0, 160);
  lv_obj_set_style_bg_color(abos_row, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(abos_row, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(abos_row, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(abos_row, 16, LV_PART_MAIN);
  lv_obj_clear_flag(abos_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *abos_title = lv_label_create(abos_row);
  lv_label_set_text(abos_title, "Show Bambu screen on print start");
  lv_obj_set_style_text_color(abos_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(abos_title, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(abos_title, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_width(abos_title, 340);

  auto_bambu_switch = lv_switch_create(abos_row);
  lv_obj_set_style_bg_color(auto_bambu_switch, COLOR_PRIMARY, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_align(auto_bambu_switch, LV_ALIGN_TOP_RIGHT, 0, -4);
  if (settings.auto_show_bambu_on_start) lv_obj_add_state(auto_bambu_switch, LV_STATE_CHECKED);
  lv_obj_add_event_cb(auto_bambu_switch, auto_bambu_switch_toggled_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // ---- Idle return-to-Home timeout ----
  lv_obj_t *idle_row = lv_obj_create(scr);
  lv_obj_set_size(idle_row, 432, 70);
  lv_obj_align(idle_row, LV_ALIGN_TOP_LEFT, 0, 240);
  lv_obj_set_style_bg_color(idle_row, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(idle_row, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(idle_row, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(idle_row, 16, LV_PART_MAIN);
  lv_obj_clear_flag(idle_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *idle_title = lv_label_create(idle_row);
  lv_label_set_text(idle_title, "Return to Home after");
  lv_obj_set_style_text_color(idle_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(idle_title, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(idle_title, LV_ALIGN_TOP_LEFT, 0, 0);

  idle_timeout_dd = lv_dropdown_create(idle_row);
  lv_dropdown_set_options(idle_timeout_dd, "1 min\n2 min\n5 min\n10 min");
  lv_obj_set_width(idle_timeout_dd, 120);
  lv_obj_align(idle_timeout_dd, LV_ALIGN_TOP_RIGHT, 0, -6);
  lv_dropdown_set_selected(idle_timeout_dd, idle_timeout_min_to_index(settings.idle_return_home_min));
  lv_obj_add_event_cb(idle_timeout_dd, idle_timeout_dd_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // ---- Restart device ----
  lv_obj_t *restart_btn = lv_btn_create(scr);
  lv_obj_set_size(restart_btn, 432, 50);
  lv_obj_align(restart_btn, LV_ALIGN_TOP_LEFT, 0, 320);
  lv_obj_set_style_bg_color(restart_btn, lv_color_hex(0xEF5350), LV_PART_MAIN);
  lv_obj_set_style_radius(restart_btn, 12, LV_PART_MAIN);
  lv_obj_add_event_cb(restart_btn, restart_btn_clicked_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *restart_lab = lv_label_create(restart_btn);
  lv_label_set_text(restart_lab, "Restart Device");
  lv_obj_center(restart_lab);

  // ---- Close: the AP toggle above already applies live, so this is the
  // only exit - it turns the AP off and connects to the configured Wi-Fi.
  lv_obj_t *close_btn = lv_btn_create(scr);
  lv_obj_set_size(close_btn, 432, 50);
  lv_obj_align(close_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(close_btn, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_radius(close_btn, 12, LV_PART_MAIN);
  lv_obj_add_event_cb(close_btn, settings_close_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *close_lab = lv_label_create(close_btn);
  lv_label_set_text(close_lab, "Close");
  lv_obj_center(close_lab);

  return scr;
}

// Small dot row on the always-on-top layer so it survives screen swaps.
// One dot per dashboard screen; the active page's dot is highlighted.
void create_page_indicator()
{
  lv_obj_t *layer = lv_disp_get_layer_top(NULL);
  lv_obj_t *row = lv_obj_create(layer);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  for (int i = 0; i < SCREEN_COUNT; i++)
  {
    lv_obj_t *dot = lv_obj_create(row);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    page_dots[i] = dot;
  }

  page_dots_row = row;
}

// Invisible top-right hotspot on the always-on-top layer. Long-pressing it
// (5s, configured on the touch indev in setup()) opens the settings screen;
// a short tap moves to the next screen.
void create_settings_hotspot()
{
  lv_obj_t *layer = lv_disp_get_layer_top(NULL);
  lv_obj_t *hotspot = lv_obj_create(layer);
  lv_obj_set_size(hotspot, 90, 90);
  lv_obj_align(hotspot, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_bg_opa(hotspot, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(hotspot, 0, LV_PART_MAIN);
  lv_obj_clear_flag(hotspot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(hotspot, open_settings_cb, LV_EVENT_LONG_PRESSED, NULL);
  lv_obj_add_event_cb(hotspot, corner_tap_next_screen_cb, LV_EVENT_SHORT_CLICKED, NULL);
}

// Invisible bottom-left hotspot on the always-on-top layer, mirroring
// create_settings_hotspot() above (moved off top-left so it no longer
// covers the Calendar day-detail screen's back button there - see
// calendar.ino's create_calendar_day_screen()). Holding it 3s toggles
// screen_locked (see lock_hotspot_event_cb() for why this is timed
// manually instead of via long_press_time). Also builds the small red
// "LOCKED" pill shown top-center while locked.
void create_lock_hotspot()
{
  lv_obj_t *layer = lv_disp_get_layer_top(NULL);
  lock_hotspot = lv_obj_create(layer);
  lv_obj_set_size(lock_hotspot, 90, 90);
  lv_obj_align(lock_hotspot, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_opa(lock_hotspot, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(lock_hotspot, 0, LV_PART_MAIN);
  lv_obj_clear_flag(lock_hotspot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(lock_hotspot, lock_hotspot_event_cb, LV_EVENT_ALL, NULL);

  lock_indicator = lv_label_create(layer);
  lv_label_set_text(lock_indicator, "LOCKED");
  lv_obj_set_style_text_color(lock_indicator, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_bg_color(lock_indicator, lv_color_hex(0xEF5350), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(lock_indicator, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(lock_indicator, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(lock_indicator, 4, LV_PART_MAIN);
  lv_obj_set_style_radius(lock_indicator, 10, LV_PART_MAIN);
  lv_obj_clear_flag(lock_indicator, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(lock_indicator, LV_ALIGN_TOP_MID, 0, 6);
  lv_obj_add_flag(lock_indicator, LV_OBJ_FLAG_HIDDEN);
}

// Persistent Wi-Fi signal indicator, top-right corner of the always-on-top
// layer (visible over every screen, like create_page_indicator()'s dots).
// Top-left was the first choice but the Calendar day-detail screen has a
// real, visible back button right there (calendar.ino's back_btn at 12,12) -
// top-right only has the invisible settings-hotspot touch target under it
// (no visible content on any screen sits this close to that corner), so it
// doesn't collide with anything. Four ascending bars sit in a 34x22 box;
// wifi_signal_apply_to_ui() fills them left-to-right up to the current RSSI
// level and colors the rest dim, and reveals a diagonal red strike across
// them when not connected at all - deliberately distinct from "weak signal"
// (all bars dim but no strike).
void create_wifi_signal_indicator()
{
  lv_obj_t *layer = lv_disp_get_layer_top(NULL);

  lv_obj_t *box = lv_obj_create(layer);
  lv_obj_set_size(box, 34, 22);
  lv_obj_align(box, LV_ALIGN_TOP_RIGHT, -10, 10);
  lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

  static const lv_coord_t bar_h[4] = {6, 11, 16, 21};
  for (int i = 0; i < 4; i++)
  {
    lv_obj_t *bar = lv_obj_create(box);
    lv_obj_set_size(bar, 5, bar_h[i]);
    lv_obj_set_pos(bar, i * 8, 21 - bar_h[i]);
    lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_border_width(bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, COLOR_TEXT_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_30, LV_PART_MAIN);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    wifi_bars[i] = bar;
  }

  // Absolute top-layer coordinates matching the box's TOP_RIGHT placement
  // above (screen is 480 wide: box spans x:[436,470] y:[10,32]) - the line
  // is a separate top-layer child, not parented to box, so it needs its own
  // coordinates rather than inheriting the align.
  static lv_point_t strike_pts[2] = {{436, 10}, {470, 32}};
  lv_obj_t *strike = lv_line_create(layer);
  lv_line_set_points(strike, strike_pts, 2);
  lv_obj_set_style_line_color(strike, lv_color_hex(0xEF5350), LV_PART_MAIN);
  lv_obj_set_style_line_width(strike, 3, LV_PART_MAIN);
  lv_obj_set_style_line_rounded(strike, true, LV_PART_MAIN);
  lv_obj_clear_flag(strike, LV_OBJ_FLAG_CLICKABLE);
  wifi_no_signal_line = strike;
}

// See blackout_overlay above screen_gesture_cb: opaque, never animated.
void create_blackout_overlay()
{
  lv_obj_t *layer = lv_disp_get_layer_top(NULL);
  blackout_overlay = lv_obj_create(layer);
  lv_obj_set_size(blackout_overlay, 480, 480);
  lv_obj_set_pos(blackout_overlay, 0, 0);
  lv_obj_set_style_bg_color(blackout_overlay, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(blackout_overlay, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(blackout_overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(blackout_overlay, 0, LV_PART_MAIN);
  lv_obj_clear_flag(blackout_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(blackout_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(blackout_overlay, LV_OBJ_FLAG_HIDDEN);
}

// create_finish_flash_overlay(), finish_flash_timer_cb(),
// start_finish_screen_flash(), bambu_warning_cycle_cb(),
// bambu_badge_flash_cb(), and start_bambu_badge_flash() moved to bambu.ino.

void switch_screen(uint8_t num, lv_scr_load_anim_t anim)
{
  if (num >= SCREEN_COUNT) return;
  // time=0, delay=0 is the only combination lv_scr_load_anim() treats as an
  // immediate, synchronous swap (see lv_disp.c) - anything else, even with
  // anim=NONE, schedules the swap via its async animation machinery and
  // keeps showing the old screen until that "animation" completes.
  lv_scr_load_anim(screens[num], anim, 0, 0, false);
  current_screen = num;
  update_page_indicator(num);
}


// ==================== Network task (runs on core 0, separate from LVGL) ====================
// Everything here can block for a while (HTTPS GETs, TLS handshakes, DNS/HTTP
// servers) without affecting rendering or touch responsiveness, because it's
// pinned to the opposite core from loop()/lv_timer_handler(). The one rule
// this task must never break: no LVGL calls from here, ever - LVGL is not
// thread-safe. Anything that needs to reach the screen sets a
// *_needs_ui_refresh flag (or, for the demo clock, updates the plain
// demo_hour/min/sec vars) and lets loop() apply it.
static uint32_t last_clock_sync_ms = 0;
#define CLOCK_SYNC_INTERVAL_MS (60UL * 60UL * 1000UL)
#define CLOCK_RETRY_INTERVAL_MS (15UL * 1000UL)

void network_task(void *param)
{
  for (;;)
  {
    if (ap_enabled)
    {
      config_server.handleClient();
      dns_server.processNextRequest();
    }
    if (wifi_got_ip)
    {
      wifi_got_ip = false;
      last_clock_sync_ms = millis();
      sync_clock_ntp();
      // Diagnostic: every WiFiClientSecure handshake below needs a sizeable
      // contiguous chunk of free internal RAM (mbedTLS session buffers), and
      // this board runs tight on that right after boot (see disp_draw_buf's
      // PSRAM comment in setup() for the earlier ~24KB crunch). If these
      // GETs fail with -1 (connection refused/handshake never completed)
      // right when free/largest_block print low here, that's heap
      // exhaustion, not a Proxmox/Bambu/script config problem.
      Serial.printf("[Heap] free=%u  largest_block=%u  min_free_ever=%u\n",
                     ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
      fetch_weather();
      proxmox_fetch();
      direct_fetch_all();
    }
    // Retry every 15s until the first sync succeeds (e.g. NTP wasn't
    // reachable yet right after boot); once synced, settle into the normal
    // hourly cadence (DST transitions, clock drift).
    uint32_t clock_sync_interval = clock_synced_once ? CLOCK_SYNC_INTERVAL_MS : CLOCK_RETRY_INTERVAL_MS;
    if (wifi_connected && millis() - last_clock_sync_ms > clock_sync_interval)
    {
      last_clock_sync_ms = millis();
      sync_clock_ntp();
    }
    if (wifi_connected && millis() - last_weather_fetch_ms > WEATHER_FETCH_INTERVAL_MS) fetch_weather();
    if (wifi_connected) direct_fetch_maintain();
    if (wifi_connected && millis() - last_proxmox_fetch_ms > PROXMOX_FETCH_INTERVAL_MS) proxmox_fetch();
    if (millis() - last_wifi_signal_sample_ms > WIFI_SIGNAL_SAMPLE_INTERVAL_MS)
    {
      last_wifi_signal_sample_ms = millis();
      wifi_signal_ui_connected = wifi_connected;
      wifi_signal_rssi_dbm = wifi_connected ? (int8_t)WiFi.RSSI() : 0;
      wifi_signal_needs_ui_refresh = true;
    }
    bambu_mqtt_maintain();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== Dashboard starting (firmware " FW_VERSION ") ===");

  data_mutex = xSemaphoreCreateRecursiveMutex();

  load_settings();

  WiFi.onEvent(on_wifi_event);
  connect_saved_wifi();

  Serial.println("Initializing touch...");
  ts.begin();
  ts.setRotation(ROTATION_NORMAL);
  Serial.println("Touch init complete");

  Serial.println("Initializing display...");
  gfx->begin(16000000);
  Serial.println("Display begin() complete");

  delay(500);
  gfx->fillScreen(BLACK);
  Serial.println("Screen filled black");

#ifdef GFX_BL
  pinMode(GFX_BL, OUTPUT);
  digitalWrite(GFX_BL, HIGH);
#endif

  lv_init();

  screenWidth = gfx->width();
  screenHeight = gfx->height();

#ifdef ESP32
  // PSRAM, not internal RAM: this is just a staging buffer copied out by
  // my_disp_flush() every frame, not DMA target memory, and at 480*200*2
  // bytes (~190KB) it was starving WiFi's internal-RAM RX buffer allocation
  // (free heap was down to ~24KB, causing AP start failures).
  disp_draw_buf = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * screenWidth * 200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  disp_draw_buf = (lv_color_t *)malloc(sizeof(lv_color_t) * screenWidth * 200);
#endif

  if (!disp_draw_buf) {
    Serial.println("LVGL disp_draw_buf allocate failed!");
  }
  else
  {
    lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, screenWidth * 200);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    indev_drv.long_press_time = 5000; // hold top-right corner 5s to open Settings
    lv_indev_drv_register(&indev_drv);

    screens[0] = create_home_screen();
    screens[1] = create_bambu_screen();
    screens[2] = create_calendar_screen();
    screens[3] = create_proxmox_screen();
    screens[4] = create_stocks_screen();
    screens[5] = create_makerworld_screen();
    screens[6] = create_tracking_screen();
    screens[FLIGHT_SCREEN_INDEX] = create_flight_screen();
    settings_screen = create_settings_screen();
    cal_day_screen = create_calendar_day_screen();

    for (int i = 0; i < SCREEN_COUNT; i++) {
      lv_obj_add_event_cb(screens[i], screen_gesture_cb, LV_EVENT_GESTURE, NULL);
    }

    create_page_indicator();
    create_settings_hotspot();
    create_lock_hotspot();
    create_wifi_signal_indicator();
    create_blackout_overlay();
    create_finish_flash_overlay();
    last_activity_ms = millis();
    lv_timer_create(clock_tick_cb, 1000, NULL);
    lv_timer_create(bambu_fan_spin_cb, 50, NULL);
    lv_timer_create(bambu_warning_cycle_cb, 5000, NULL);

    switch_screen(0, LV_SCR_LOAD_ANIM_NONE);

    // Network I/O (Wi-Fi/AP servers, HTTPS fetches, MQTT) runs on core 0 so
    // it can never stall LVGL rendering/touch on this core - see
    // network_task() above for why and the thread-safety rules it follows.
    // 12 KB stack: the iCal/MakerWorld/PKGE parsers keep their buffers on
    // the heap, but the TLS client + JSON filters still need some headroom.
    xTaskCreatePinnedToCore(network_task, "network", 12288, NULL, 1, NULL, 0);

    Serial.println("Setup done");
  }
}

void loop()
{
  lv_timer_handler();

  // Anything network_task flagged as ready gets applied to LVGL here, on
  // the one thread that's allowed to touch it.
  if (clock_needs_ui_refresh) { clock_needs_ui_refresh = false; clock_apply_to_ui(); }
  if (weather_needs_ui_refresh) { weather_needs_ui_refresh = false; weather_apply_to_ui(); }
  if (stocks_needs_ui_refresh) { stocks_needs_ui_refresh = false; stocks_apply_to_ui(); }
  if (calendar_needs_ui_refresh) { calendar_needs_ui_refresh = false; calendar_apply_to_ui(); }
  if (makerworld_needs_ui_refresh) { makerworld_needs_ui_refresh = false; makerworld_apply_to_ui(); }
  if (tracking_needs_ui_refresh) { tracking_needs_ui_refresh = false; tracking_apply_to_ui(); }
  if (bambu_needs_ui_refresh) { bambu_needs_ui_refresh = false; bambu_apply_to_ui(); }
  if (proxmox_needs_ui_refresh) { proxmox_needs_ui_refresh = false; proxmox_apply_to_ui(); }
  if (wifi_signal_needs_ui_refresh) { wifi_signal_needs_ui_refresh = false; wifi_signal_apply_to_ui(); }

  delay(5);
}

/*******************************************************************************
 * ESP32-4848S040 Multi-Screen Dashboard
 * LVGL 8.4 (8.x API) with ST7701S Support
 * 480x480 Display with 7-8 Swipeable Screens
 ******************************************************************************/

// Shown in the Serial log at boot; .github/workflows/compile.yml tags each
// release with it, so bump it with every firmware change.
#define FW_VERSION "3.48"

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
#include <Update.h>
#include "bootloader_random.h"
#include "nvs.h"
#include "esp_task_wdt.h"
#include "esp_ota_ops.h"
#include "esp_image_format.h"
#include "mbedtls/platform.h"
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <esp32s3/rom/cache.h> // Cache_WriteBack_Addr() - see my_disp_flush()
#include "calendar_types.h"
#include "direct_fetch.h"
#include "gh_update.h"
#include "lang.h"

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

// Big numbers: Roboto Medium 90 px (0-9 : ° C F - + space) for the weather
// temperature and the Big caller ID number, Roboto Regular 56 px (0-9 - +
// space) for long international numbers. The clocks are Roboto Bold (0-9 :).
extern "C" {
extern const lv_font_t lv_font_roboto_medium_90, lv_font_roboto_regular_56;
extern const lv_font_t lv_font_roboto_medium_44; // 0-9 % h m - space: the Bambu screen's percent and time left
// UI text: LVGL's built-in Montserrat (same file, same symbols) plus Latin-1
// accents and typographic quotes, for Italian/French and accented titles.
// Made with lv_font_conv - see the README.
LV_FONT_DECLARE(lv_font_ui_12);
LV_FONT_DECLARE(lv_font_ui_14);
LV_FONT_DECLARE(lv_font_ui_16);
LV_FONT_DECLARE(lv_font_ui_18);
LV_FONT_DECLARE(lv_font_ui_20);
LV_FONT_DECLARE(lv_font_ui_22);
LV_FONT_DECLARE(lv_font_ui_24);
LV_FONT_DECLARE(lv_font_ui_26);
LV_FONT_DECLARE(lv_font_ui_28);

// Bigger, bold digits-and-colon-only Roboto for the Home clock.
extern const lv_font_t lv_font_roboto_bold_136, lv_font_roboto_bold_126; // clock: night clock / Home
// Home screen shortcut icons (Material Design Icons) - see home_icons.c /
// tools/make_home_icons.py.
extern const lv_img_dsc_t home_icon_home, home_icon_printer, home_icon_calendar, home_icon_server, home_icon_stocks,
    home_icon_makerworld, home_icon_package, home_icon_flight, home_icon_pin, home_icon_ha, home_icon_phone, home_icon_jellyfin,
    wx_icon_sunny, wx_icon_night, wx_icon_partly, wx_icon_cloudy, wx_icon_fog, wx_icon_rain, wx_icon_pouring,
    wx_icon_snow, wx_icon_storm;
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
    0 /* pclk_active_neg */, 10000000 /* prefer_speed: 10 MHz (~35 fps) - was 12 MHz; less PSRAM bandwidth per
                                            frame leaves more headroom for Wi-Fi/TLS, see rgb_resync.cpp */, false /* useBigEndian */,
    0 /* de_idle_high */, 0 /* pclk_idle_high */,
    480 * 40 /* bounce_buffer_size_px: reduce tearing on full-screen swaps - bumped from 480*10
                since network activity (WiFi/TLS competing for PSRAM bandwidth) was starving the
                RGB DMA refill badly enough to show as blinking lines / brief freezes */);
Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    480 /* width */, 480 /* height */, rgbpanel, 0 /* rotation: library default - see my_touchpad_read() for the matching touch mapping */, true /* auto_flush */,
    bus, GFX_NOT_DEFINED /* RST */, st7701_type9_init_operations, sizeof(st7701_type9_init_operations));

// Re-aligns the panel after a bounce-buffer underrun (see rgb_resync.cpp).
void rgb_panel_resync(Arduino_ESP32RGBPanel *panel);
bool rgb_panel_full_reset(Arduino_ESP32RGBPanel *panel);
bool rgb_panel_diag_start(Arduino_ESP32RGBPanel *panel);
void rgb_panel_diag(Arduino_ESP32RGBPanel *panel, char *out, size_t n);
void rgb_panel_resync_if_needed(Arduino_ESP32RGBPanel *panel);
void rgb_panel_signal_pause(int pclk_gpio, uint32_t ms);
void rgb_panel_signals(bool on, int de, int vsync, int hsync, int pclk);
static volatile bool panel_boot_request = false; // setup page -> loop(): panel_boot_sequence()
#define RGB_PCLK_GPIO 21
static volatile bool panel_pause_request = false; // setup page -> loop(): rgb_panel_signal_pause()
// Set by the setup page, done by loop() (not mid-draw): rgb_panel_full_reset().
static volatile bool panel_reset_request = false;
#define RGB_RESYNC_INTERVAL_MS 1000

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
// Packages, Flight, Home Assistant, Phone (Asterisk), Jellyfin. Flight is
// only in the swipe sequence while a flight is coming up, the last three
// only once they're set up - see screen_available().
#define SCREEN_COUNT 11
#define HA_SCREEN_INDEX 8
#define PHONE_SCREEN_INDEX 9
#define JF_SCREEN_INDEX 10
#define FLIGHT_SCREEN_INDEX 7
static lv_obj_t *screens[SCREEN_COUNT];
static uint8_t current_screen = 0;

// Home screen clock and date labels (see clock_tick_cb()/clock_apply_to_ui()).
static lv_obj_t *home_hour_label = NULL;
static lv_obj_t *home_min_label = NULL;
static lv_obj_t *home_date_label = NULL;
static lv_obj_t *home_next_event_lab = NULL; // "Next: ..." strip - populated by calendar.ino's update_home_next_event()
static lv_obj_t *home_next_accent = NULL;    // its accent bar - orange while a flight is pinned there
static lv_obj_t *home_flight_btn = NULL;     // shortcut to the Flight screen, only while it's available
static lv_obj_t *home_shortcuts_row = NULL;   // icons on Home itself ("always on Home" mode)
static lv_obj_t *home_time_row = NULL;        // the clock - moves down a bit to clear that icon row
// Jump menu: a tap on the top-left corner of any screen opens a card with
// one icon per screen (see create_nav_menu()); it closes after 10 s, on a
// choice, or on a tap outside it.
static lv_obj_t *nav_menu_hotspot = NULL;
static lv_obj_t *nav_menu_backdrop = NULL;
static lv_obj_t *nav_menu_card = NULL;
static lv_obj_t *nav_menu_btn[SCREEN_COUNT] = {NULL};
static uint32_t nav_menu_until_ms = 0;
#define NAV_MENU_SHOW_MS (10UL * 1000UL)
enum { SHORTCUTS_ALWAYS = 0, SHORTCUTS_ON_TAP = 1, SHORTCUTS_OFF = 2 };
enum { UPDATE_NOTIFY = 0, UPDATE_AUTO = 1, UPDATE_OFF = 2 };
// Local time shown on Home and used by the Calendar. Once NTP has synced,
// clock_tick_cb() reads the system time every second (so midnight, DST and
// a busy UI never leave it behind); before that it just counts ticks from
// these placeholders. Only written on the LVGL thread.
static uint8_t clock_hour = 14, clock_min = 32, clock_sec = 0;
static uint8_t clock_wday = 6, clock_mday = 15, clock_mon = 7; // tm_wday/tm_mon convention: Sun=0, Jan=0
static uint16_t clock_year = 2026; // full year (not tm_year's since-1900 form)
static volatile bool clock_needs_ui_refresh = false; // set by sync_clock_ntp() on network_task
static volatile bool clock_synced_once = false;

// Live-updated weather widgets on the Home screen (see fetch_weather()).
static lv_obj_t *weather_temp_lab = NULL;
static lv_obj_t *weather_desc_lab = NULL;
static lv_obj_t *weather_extra_lab = NULL; // "Feels like 15° - H 19° L 9°"
static lv_obj_t *weather_humidity_val = NULL;
static lv_obj_t *weather_wind_val = NULL;
static lv_obj_t *weather_uv_val = NULL;
static lv_obj_t *home_weather_stats = NULL; // humidity / wind / UV row - swapped out by weather_slot.ino

// Page-position dots (see create_page_indicator()) auto-hide after idle.
static lv_obj_t *page_dots_row = NULL;
static uint32_t last_activity_ms = 0;
static bool in_settings = false;
// Detail pages outside screens[] (Calendar day, Forecast, Agenda) - see
// open_subscreen(). current_screen keeps the page they were opened from.
static bool in_subscreen = false;
// Firmware update running (setup page upload or GitHub) - see ota_apply_to_ui().
static volatile bool ota_in_progress = false;
static volatile uint32_t ota_started_ms = 0;
// Writing flash keeps the panel from reading its picture, so it flickers
// whatever is on it: after showing the card for a moment the backlight goes
// off until the restart (see backlight_update()).
#define OTA_DARK_AFTER_MS 4000

// Why the board last restarted, when it wasn't planned (see restart_reason_init()).
static const char *restart_reason = "";
static uint16_t crash_count = 0;

// Auto-return-to-Home after N min idle (see clock_tick_cb()) - N is
// settings.idle_return_home_min, set on the on-device Settings screen (see
// IDLE_TIMEOUT_OPTIONS_MIN/create_settings_screen()) - except while showing
// the Bambu screen because a print just auto-opened it - that case instead
// gets its own 5-min-after-finish return, and neither applies while
// screen_pinned.
#define BAMBU_AUTO_SHOW_MS (10UL * 1000UL) // the Bambu screen at a print's start and end, then Home
static bool bambu_screen_auto_opened = false;
static uint32_t bambu_auto_return_home_at_ms = 0; // 0 = not scheduled

// Screen pin: hold the top-left corner 3s to toggle (see pin_hold_event(),
// pin icon from create_pin_hotspot()). While pinned, swipe navigation, the Home
// badge's jump-to-Bambu, the print-start auto-navigate, and both auto-return
// timers above are all suppressed - the visible screen just stays put.
static lv_obj_t *pin_hotspot = NULL;
static lv_obj_t *pin_indicator = NULL;
static uint32_t pin_press_start_ms = 0;
static bool pin_press_fired = false;
static bool screen_pinned = false;

// Wi-Fi signal indicator (top-right, always-on-top layer, see
// create_wifi_signal_indicator()) - four ascending bars filled up to the
// current RSSI-derived level, plus a red diagonal strike across all-dim bars
// when not connected. Same "top layer object, visible on every screen"
// pattern as pin_indicator above. Updated by wifi_signal_apply_to_ui(),
// driven off a periodic RSSI sample taken in network_task (see
// WIFI_SIGNAL_SAMPLE_INTERVAL_MS) - WiFi.RSSI() is a local driver read, not a
// network round-trip, but it's funneled through the same
// sample-in-network_task / apply-in-loop() split as every other bit of
// Wi-Fi state here rather than carving out a one-off exception to the
// LVGL-thread rule.
static lv_obj_t *wifi_bars[4] = {NULL, NULL, NULL, NULL};
static lv_obj_t *wifi_no_signal_line = NULL;
static lv_obj_t *wifi_signal_box = NULL;
// Auto-hide (settings.wifi_icon_autohide): the icon shows only on Home, for
// WIFI_ICON_SHOW_MS after arriving there (or after the connection state
// changes); while disconnected it stays up on Home. See
// wifi_indicator_update_visibility().
#define WIFI_ICON_SHOW_MS (20UL * 1000UL)
static uint32_t wifi_icon_until_ms = 0;
void wifi_indicator_update_visibility();

// Home screen print-status badge (see create_home_screen()) + the state
// needed to detect a RUNNING->FINISH transition and time out the "finished"
// badge after 10 minutes. All updated only from bambu_apply_to_ui() (an
// event-driven push, not a poll) and clock_tick_cb()'s existing local 1s
// tick (for the timeout check only - no network involved).
static char bambu_prev_gcode_state[16] = "";
static uint32_t bambu_finish_badge_until_ms = 0; // 0 = not showing "Print finished" on Home's printing card
#define BAMBU_FINISH_BADGE_MS (10UL * 60UL * 1000UL)

// Full-screen green flash on print finish - a purely local LVGL-thread visual effect,
// same "toggle HIDDEN, never animate opacity" pattern as blackout_overlay
// (animated/partial opacity on a full 480x480 object breaks on this board -
// see switch_screen()/blackout_overlay history).
static lv_obj_t *finish_flash_overlay = NULL;
static lv_timer_t *finish_flash_timer = NULL;
static uint8_t finish_flash_count = 0;

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

// Flight change alert: when a flight refresh changes the times, gate,
// terminal or status, network_task sets flight_change_pending and loop()
// shows the Flight screen for FLIGHT_ALERT_MS (if enabled in Settings),
// with the changes highlighted - see flight_alert_start() in flight.ino.
#define FLIGHT_CHG_DEP 0x01
#define FLIGHT_CHG_ARR 0x02
#define FLIGHT_CHG_GATE 0x04
#define FLIGHT_CHG_TERMINAL 0x08
#define FLIGHT_CHG_STATUS 0x10
#define FLIGHT_ALERT_MS (2UL * 60UL * 1000UL)
static volatile bool flight_change_pending = false;
static volatile uint8_t flight_change_mask = 0;
static uint32_t flight_alert_until_ms = 0; // 0 = no alert showing
static uint8_t flight_alert_prev_screen = 0;
static uint8_t flight_alert_mask = 0;
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
  // Empty: the city of the time zone is used (see weather_location_effective()).
  char weather_location[65] = "";
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
  // Home Assistant (homeassistant.ino): address, long-lived token, entity IDs.
  char ha_url[96] = "";
  char ha_token[300] = "";
  char ha_entities[400] = ""; // "light.desk, switch.fan, sensor.outdoor_temperature"
  uint8_t idle_return_home_min = 5; // one of IDLE_TIMEOUT_OPTIONS_MIN - see create_settings_screen()
  bool flight_alert = true; // show the Flight screen for 2 min when the flight's data changes
  bool wifi_icon_autohide = true; // Wi-Fi icon only on Home, for 20 s - see wifi_indicator_update_visibility()
  // Night dimming (see backlight_update()): from night_start to night_end
  // (local hours, may wrap past midnight) the backlight drops to
  // night_level % after 30 s without a touch; 0 = off. A touch wakes it.
  bool night_dim = false;
  uint8_t night_start = 23, night_start_min = 0; // hour, minute
  uint8_t night_end = 7, night_end_min = 0;
  uint8_t night_level = 10;
  uint8_t day_level = 100; // normal brightness, %
  bool night_clock = true;  // dimmed at night: big clock instead of the dashboard (screensaver.ino)
  // GitHub release updates (see gh_update_maintain()): UPDATE_NOTIFY shows
  // an icon on Home to accept/skip, UPDATE_AUTO installs by itself,
  // UPDATE_OFF never checks on its own (the Settings button still works).
  uint8_t update_mode = UPDATE_NOTIFY;
  uint8_t lang = LANG_EN; // LANG_EN / LANG_IT / LANG_FR - see lang.h
  bool shot_open = false; // /screenshot without the login - see handle_screenshot()
  char update_skip[16] = ""; // version the user chose to skip
  uint8_t home_shortcuts = SHORTCUTS_ON_TAP; // SHORTCUTS_ALWAYS (icons on Home) / _ON_TAP (top-left menu) / _OFF
  // Proxmox VE cluster - see proxmox.ino. host is "ip-or-hostname:port"
  // (default port 8006); token_id is the full "user@realm!tokenname" from
  // Datacenter -> Permissions -> API Tokens; token_secret is the UUID shown
  // once at token creation time. A read-only PVEAuditor role on the token
  // is enough for everything this screen reads.
  char proxmox_host[64] = "";
  char proxmox_token_id[64] = "";
  char proxmox_token_secret[40] = "";
  // Asterisk caller ID (asterisk.ino): manager host ("ip" or "ip:port",
  // default 5038), a manager.conf user with read = call, and optionally
  // the extensions to watch ("101,102"; empty = every call).
  char ami_host[64] = "";
  char ami_user[32] = "";
  char ami_secret[64] = "";
  char ami_ext[48] = "";
  char ami_context[32] = "from-internal"; // dialplan context for "call back" (Originate)
  bool call_big = false; // a ringing call covers the whole weather panel, number in big digits
  // Jellyfin (jellyfin.ino): server address and an API key (Dashboard -> API Keys).
  char jf_url[96] = "";
  char jf_key[48] = "";
  bool jf_popups = true; // calls and print news as pop-ups on the Jellyfin clients
  char jf_mute[200] = "";  // Jellyfin devices that get no pop-ups, "|"-separated
};
static DeviceSettings settings;

// A stream shifted by an underrun (40 lines rolled up) recovers within a
// second: every second, on an esp_timer (not the LVGL loop, so it also
// runs while loop() is busy), the stream's timing is compared with the one
// measured right after boot, and only when it's out of step is the stream
// restarted. It used to restart blindly every 1-2 s; each restart briefly
// disturbs the signal, which is a suspect for the panel occasionally
// showing the picture from the wrong line (only a reboot cleared that).
static void rgb_resync_timer_cb(void *arg)
{
  rgb_panel_resync_if_needed(rgbpanel);
}

void rgb_resync_timer_start()
{
  static esp_timer_handle_t t = NULL;
  if (t) return;
  esp_timer_create_args_t args = {};
  args.callback = rgb_resync_timer_cb;
  args.name = "rgb_resync";
  if (esp_timer_create(&args, &t) == ESP_OK) esp_timer_start_periodic(t, RGB_RESYNC_INTERVAL_MS * 1000ULL);
}

void clock_tick_cb(lv_timer_t *timer)
{

  uint8_t prev_min = clock_min, prev_mday = clock_mday;
  if (!clock_read_local())
  {
    // Not synced yet: free-running placeholder.
    if (++clock_sec >= 60)
    {
      clock_sec = 0;
      if (++clock_min >= 60) { clock_min = 0; if (++clock_hour >= 24) clock_hour = 0; }
    }
  }
  if (clock_min != prev_min)
  {
    lv_label_set_text_fmt(home_hour_label, "%02d", clock_hour);
    lv_label_set_text_fmt(home_min_label, "%02d", clock_min);
    // Passed events drop off and flight countdowns tick, without a refetch.
    update_home_next_event();
    flight_refresh_screen();
  }
  if (clock_mday != prev_mday)
  {
    clock_apply_to_ui();     // the date line
    calendar_apply_to_ui();  // the month grid's "today" ring (and the month itself on the 1st)
  }

  wifi_indicator_update_visibility();
  backlight_update();
  alert_tick();
  quick_panel_tick();

  // Auto-hide the page-position dots after 10 s of no touch activity (a
  // touch brings them back - see my_touchpad_read()).
  if (page_dots_row && !lv_obj_has_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN) &&
      millis() - last_activity_ms > 10000)
  {
    lv_obj_add_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN);
  }

  // The 10-min "Print finished" on Home's printing card has elapsed. Purely
  // a local countdown check against millis() - no network call here.
  if (bambu_finish_badge_until_ms != 0 && millis() >= bambu_finish_badge_until_ms)
  {
    bambu_finish_badge_until_ms = 0;
    home_print_apply(); // weather_slot.ino
  }

  // Auto-return to Home after settings.idle_return_home_min of no touch -
  // except while the Bambu screen is showing because a print just
  // auto-opened it (that case is handled by the finish-triggered return
  // below instead) and never while pinned. Detail pages close too.
  // Jump menu left open: close it after 10 s.
  if (nav_menu_until_ms != 0 && millis() >= nav_menu_until_ms) nav_menu_hide();

  // Flight change alert over: back to whatever was showing before.
  if (flight_alert_until_ms != 0 && millis() >= flight_alert_until_ms) flight_alert_end(false);

  if (!screen_pinned && !in_settings && (current_screen != 0 || in_subscreen) && flight_alert_until_ms == 0)
  {
    bool bambu_auto_exempt = (current_screen == 1 && bambu_screen_auto_opened);
    uint32_t idle_return_home_ms = (uint32_t)settings.idle_return_home_min * 60UL * 1000UL;
    if (!bambu_auto_exempt && millis() - last_activity_ms > idle_return_home_ms)
    {
      bambu_screen_auto_opened = false;
      bambu_auto_return_home_at_ms = 0;
      if (in_subscreen)
      {
        in_subscreen = false;
        if (page_dots_row) lv_obj_clear_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN);
      }
      calendar_view_reset(); // calendar.ino: back to this month
      go_to_screen_blackout(0);
    }
  }

  // The Bambu screen was auto-opened at a print's start or end: back to Home
  // after BAMBU_AUTO_SHOW_MS instead of the (suppressed) inactivity timeout.
  if (!screen_pinned && bambu_auto_return_home_at_ms != 0 && millis() >= bambu_auto_return_home_at_ms)
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

  // The panel's DMA reads the frame buffer straight from PSRAM, so what the
  // CPU just wrote must leave the data cache. Arduino_GFX writes back from
  // the area's exact first pixel, which is rarely cache-line aligned, and
  // the first line of an area could stay stale - short dark dashes (16 px =
  // one 32-byte line) at the left edge of freshly drawn objects. Write back
  // the whole rows, rounded out to 64-byte lines.
  uint16_t *fb = gfx->getFramebuffer();
  if (fb)
  {
    uintptr_t start = (uintptr_t)(fb + (uint32_t)area->y1 * screenWidth) & ~(uintptr_t)63;
    uintptr_t end = ((uintptr_t)(fb + (uint32_t)(area->y2 + 1) * screenWidth) + 63) & ~(uintptr_t)63;
    Cache_WriteBack_Addr((uint32_t)start, (uint32_t)(end - start));
  }

  lv_disp_flush_ready(disp);
}

// ==================== Backlight / night dimming ====================
// 1 kHz: this board's backlight driver goes black on a 5 kHz PWM (tested
// 2026-10-12: 1 kHz and 200 Hz dim fine, 5 kHz is off at anything below 100%).
#define BACKLIGHT_PWM_HZ 1000
#define NIGHT_DIM_IDLE_MS (30UL * 1000UL)
#define SAVER_MIN_LEVEL 15 // %: the night clock's floor - lower, this panel is practically black
static uint8_t backlight_level = 100; // %, as last written
static bool touch_swallow = false;    // a wake-up touch on a dark screen: ignored until released

void backlight_set(uint8_t pct)
{
#ifdef GFX_BL
  backlight_level = pct;
  ledcWrite(GFX_BL, (uint32_t)pct * 255 / 100);
#endif
}

// "Dim now" from the quick panel: dims (and shows the night clock) right
// away, day or night, until the next touch - see my_touchpad_read().
static bool dim_forced = false;
static uint32_t dim_forced_at = 0;

bool night_now()
{
  if (!settings.night_dim || !clock_synced_once) return false;
  // Minutes since midnight.
  int s = settings.night_start * 60 + settings.night_start_min, e = settings.night_end * 60 + settings.night_end_min;
  int t = clock_hour * 60 + clock_min;
  if (s == e) return false;
  return s < e ? (t >= s && t < e) : (t >= s || t < e); // e.g. 23:30 -> 7:00 wraps past midnight
}

// Once a second from clock_tick_cb(), and right away on a touch.
void backlight_update()
{
  bool dim = dim_forced || (night_now() && millis() - last_activity_ms > NIGHT_DIM_IDLE_MS);
  static bool was_dim = false;
  if (dim != was_dim)
  {
    was_dim = dim;
    Serial.printf("[Night] %s (%02d:%02d, night %02d:%02d-%02d:%02d, level %d%%, idle %lu s)\n", dim ? "dimmed" : "awake", clock_hour,
                  clock_min, settings.night_start, settings.night_start_min, settings.night_end, settings.night_end_min,
                  dim ? settings.night_level : settings.day_level,
                  (unsigned long)((millis() - last_activity_ms) / 1000));
  }
  uint8_t want = dim ? settings.night_level : settings.day_level;
  if (ota_in_progress && millis() - ota_started_ms > OTA_DARK_AFTER_MS) want = 0; // updating: dark, no flicker
  screensaver_update();
  if (screensaver_visible() && want < SAVER_MIN_LEVEL) want = SAVER_MIN_LEVEL;
  if (want != backlight_level) backlight_set(want);
}

/* Touchpad read */
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
  ts.read();
  if (ts.isTouched && (touch_swallow || backlight_level == 0 || screensaver_visible()))
  {
    // Screen off or night clock showing: this touch only wakes the
    // dashboard - nothing underneath gets pressed.
    touch_swallow = true;
    last_activity_ms = millis();
    dim_forced = false;
    backlight_update();
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  touch_swallow = false;
  static bool was_touched = false;
  static lv_coord_t start_x, start_y, last_x, last_y;
  if (!ts.isTouched && was_touched)
  {
    // Released: a swipe down from the top edge opens the quick panel.
    was_touched = false;
    if (start_y < 50 && start_x > 80 && start_x < 400 && last_y - start_y > 90 && abs(last_x - start_x) < 90)
      quick_panel_request();
  }
  if (ts.isTouched)
  {
    last_activity_ms = millis();
    if (backlight_level != settings.day_level) backlight_update();
    if (page_dots_row && !in_settings && !in_subscreen) lv_obj_clear_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN);

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
    if (!was_touched) { start_x = tx; start_y = ty; }
    if (!was_touched && dim_forced && millis() - dim_forced_at > 1500) dim_forced = false; // (only if not swallowed above)
    was_touched = true;
    last_x = tx;
    last_y = ty;
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
  if (idx == HA_SCREEN_INDEX) return ha_configured();
  if (idx == PHONE_SCREEN_INDEX) return ami_configured();
  if (idx == JF_SCREEN_INDEX) return jf_configured();
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
  if (screen_pinned) return;

  lv_indev_t *indev = lv_indev_get_act();
  if (!indev) return;

  // MOVE_LEFT/RIGHT animate along the wrong physical axis on this panel's
  // rotation, and opacity-based fades (FADE_IN/OUT) flicker because a
  // full 480x480 screen forces LVGL to render into a temporary layer
  // buffer that this board can't reliably allocate. So: cut to black,
  // swap the screen underneath instantly, then cut back - see
  // go_to_screen_blackout().
  lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) flight_alert_cancel();
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
  rgb_panel_resync_if_needed(rgbpanel); // while the screen is black anyway; only when out of step
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

// Tap on the bottom-right hotspot (see create_next_hotspot()): next screen,
// like a left swipe. The top-right corner only opens Settings (5s hold).
void corner_tap_next_screen_cb(lv_event_t *e)
{
  if (screen_pinned || in_settings || in_subscreen) return;
  nav_menu_hide();
  flight_alert_cancel();
  bambu_screen_auto_opened = false;
  bambu_auto_return_home_at_ms = 0;
  go_to_screen_blackout(adjacent_screen(current_screen, +1));
}

// Jumps to a screen from the Home icons or the jump menu.
void jump_to_screen(uint8_t target)
{
  if (screen_pinned || !screen_available(target)) return;
  nav_menu_hide();
  flight_alert_cancel();
  bambu_screen_auto_opened = false;
  bambu_auto_return_home_at_ms = 0;
  if (target != current_screen) go_to_screen_blackout(target);
}

void home_shortcut_clicked_cb(lv_event_t *e)
{
  jump_to_screen((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

// Shows/hides the Home icon row and the top-left menu corner per
// settings.home_shortcuts. The menu corner works on every screen (Settings
// excepted); the Home row starts right of it so both can be used.
void nav_shortcuts_apply_mode()
{
  bool always = settings.home_shortcuts == SHORTCUTS_ALWAYS;
  if (home_shortcuts_row) (always ? lv_obj_clear_flag : lv_obj_add_flag)(home_shortcuts_row, LV_OBJ_FLAG_HIDDEN);
  // Clock center at y=82 (y=94 under the icon row); the divider is at y=150.
  if (home_time_row) lv_obj_align(home_time_row, LV_ALIGN_CENTER, 0, always ? 10 : -2);
  // Kept with shortcuts off too: the same corner toggles the screen pin.
  if (nav_menu_hotspot) (in_settings ? lv_obj_add_flag : lv_obj_clear_flag)(nav_menu_hotspot, LV_OBJ_FLAG_HIDDEN);
}

void nav_menu_hide()
{
  nav_menu_until_ms = 0;
  if (nav_menu_card) lv_obj_add_flag(nav_menu_card, LV_OBJ_FLAG_HIDDEN);
  if (nav_menu_backdrop) lv_obj_add_flag(nav_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
}

void nav_menu_show()
{
  if (!nav_menu_card) return;
  for (int i = 0; i < SCREEN_COUNT; i++)
  {
    if (!nav_menu_btn[i]) continue;
    (screen_available(i) ? lv_obj_clear_flag : lv_obj_add_flag)(nav_menu_btn[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_border_width(nav_menu_btn[i], i == current_screen ? 2 : 0, LV_PART_MAIN);
  }
  lv_obj_clear_flag(nav_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(nav_menu_card, LV_OBJ_FLAG_HIDDEN);
  nav_menu_until_ms = millis() + NAV_MENU_SHOW_MS;
}

// Top-left corner: a 3 s hold toggles the screen pin (see
// pin_hold_event()), a tap opens the jump menu. On the Calendar day view
// that corner is its back button, so a tap keeps working as one.
void nav_menu_hotspot_cb(lv_event_t *e)
{
  if (pin_hold_event(e)) return;
  if (lv_event_get_code(e) != LV_EVENT_SHORT_CLICKED) return;
  if (in_settings) return;
  if (in_subscreen) { close_subscreen(NULL); return; }
  if (screen_pinned || settings.home_shortcuts == SHORTCUTS_OFF) return;
  if (nav_menu_until_ms) nav_menu_hide();
  else nav_menu_show();
}

void nav_menu_btn_cb(lv_event_t *e)
{
  jump_to_screen((uint8_t)(uintptr_t)lv_event_get_user_data(e));
}

void nav_menu_backdrop_cb(lv_event_t *e)
{
  nav_menu_hide();
}

// One shortcut button: rounded square with a recolored icon.
lv_obj_t *make_shortcut_btn(lv_obj_t *parent, const lv_img_dsc_t *icon, lv_color_t color, lv_event_cb_t cb, uint8_t screen, lv_coord_t size = 42)
{
  lv_obj_t *btn = lv_obj_create(parent);
  lv_obj_set_size(btn, size, size);
  lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x232D38), LV_PART_MAIN);
  lv_obj_set_style_bg_color(btn, lv_color_hex(0x34424F), LV_PART_MAIN | LV_STATE_PRESSED);
  lv_obj_set_style_border_color(btn, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_border_width(btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(btn, 0, LV_PART_MAIN);
  lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_GESTURE_BUBBLE); // swiping over the icons still changes screen
  lv_obj_add_event_cb(btn, cb, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)screen);

  lv_obj_t *img = lv_img_create(btn);
  lv_img_set_src(img, icon);
  lv_obj_set_style_img_recolor(img, color, LV_PART_MAIN);
  lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, LV_PART_MAIN);
  if (size < 36) lv_img_set_zoom(img, 256 * (size - 6) / 30); // the icons are 30 px, made for 36 px buttons
  lv_obj_center(img);
  lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
  return btn;
}

struct ShortcutDef { const lv_img_dsc_t *icon; uint8_t screen; };
static const ShortcutDef SHORTCUTS[] = {
    {&home_icon_home, 0}, {&home_icon_printer, 1}, {&home_icon_calendar, 2}, {&home_icon_server, 3},
    {&home_icon_stocks, 4}, {&home_icon_makerworld, 5}, {&home_icon_package, 6}, {&home_icon_flight, FLIGHT_SCREEN_INDEX},
    {&home_icon_ha, HA_SCREEN_INDEX}, {&home_icon_phone, PHONE_SCREEN_INDEX}, {&home_icon_jellyfin, JF_SCREEN_INDEX}};
#define SHORTCUT_COUNT (sizeof(SHORTCUTS) / sizeof(SHORTCUTS[0]))

// Jump menu on the always-on-top layer: an invisible top-left hotspot, a
// backdrop that closes the menu when tapped outside, and the icon card.
void create_nav_menu()
{
  lv_obj_t *layer = lv_disp_get_layer_top(NULL);

  nav_menu_backdrop = lv_obj_create(layer);
  lv_obj_set_size(nav_menu_backdrop, 480, 480);
  lv_obj_set_pos(nav_menu_backdrop, 0, 0);
  lv_obj_set_style_bg_color(nav_menu_backdrop, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(nav_menu_backdrop, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(nav_menu_backdrop, 0, LV_PART_MAIN);
  lv_obj_clear_flag(nav_menu_backdrop, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(nav_menu_backdrop, nav_menu_backdrop_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_add_flag(nav_menu_backdrop, LV_OBJ_FLAG_HIDDEN);

  nav_menu_card = lv_obj_create(layer);
  // Up to 5 icons per row (42 px + 6 px gaps + 8 px padding + border),
  // wrapping onto a second row - a single row got too wide with 9 screens.
  lv_obj_set_size(nav_menu_card, 5 * 42 + 4 * 6 + 2 * 8 + 2, LV_SIZE_CONTENT);
  lv_obj_align(nav_menu_card, LV_ALIGN_TOP_MID, 0, 10); // stays centered as the Flight icon comes and goes
  lv_obj_set_style_bg_color(nav_menu_card, lv_color_hex(0x10181F), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(nav_menu_card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(nav_menu_card, lv_color_hex(0x34424F), LV_PART_MAIN);
  lv_obj_set_style_border_width(nav_menu_card, 1, LV_PART_MAIN);
  lv_obj_set_style_radius(nav_menu_card, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(nav_menu_card, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_column(nav_menu_card, 6, LV_PART_MAIN);
  lv_obj_set_style_pad_row(nav_menu_card, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(nav_menu_card, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(nav_menu_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(nav_menu_card, LV_OBJ_FLAG_SCROLLABLE);
  for (size_t i = 0; i < SHORTCUT_COUNT; i++)
  {
    uint8_t scr = SHORTCUTS[i].screen;
    nav_menu_btn[scr] = make_shortcut_btn(nav_menu_card, SHORTCUTS[i].icon,
                                          scr == FLIGHT_SCREEN_INDEX ? COLOR_SECONDARY : COLOR_PRIMARY, nav_menu_btn_cb, scr);
  }
  lv_obj_add_flag(nav_menu_card, LV_OBJ_FLAG_HIDDEN);

  nav_menu_hotspot = lv_obj_create(layer);
  lv_obj_set_size(nav_menu_hotspot, 80, 70);
  lv_obj_align(nav_menu_hotspot, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_style_bg_opa(nav_menu_hotspot, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(nav_menu_hotspot, 0, LV_PART_MAIN);
  lv_obj_clear_flag(nav_menu_hotspot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(nav_menu_hotspot, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(nav_menu_hotspot, nav_menu_hotspot_cb, LV_EVENT_ALL, NULL);

  nav_shortcuts_apply_mode();
}

// Holding the top-left corner 3 s toggles screen_pinned (called from
// nav_menu_hotspot_cb()). Timed manually via PRESSED/PRESSING/RELEASED
// rather than the indev's built-in long_press_time, since that's already
// set to 5s for the Settings hotspot (LVGL only supports one duration per
// indev) and this needs a different, shorter hold. Returns true when the
// event was part of the hold, so the caller ignores it - including the
// SHORT_CLICKED that can follow a completed hold.
void pin_set(bool on)
{
  screen_pinned = on;
  if (pin_indicator) (on ? lv_obj_clear_flag : lv_obj_add_flag)(pin_indicator, LV_OBJ_FLAG_HIDDEN);
}

bool pin_hold_event(lv_event_t *e)
{
  lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED)
  {
    pin_press_start_ms = millis();
    pin_press_fired = false;
  }
  else if (code == LV_EVENT_PRESSING)
  {
    if (!pin_press_fired && pin_press_start_ms != 0 && millis() - pin_press_start_ms >= 3000)
    {
      pin_press_fired = true;
      pin_set(!screen_pinned);
    }
  }
  else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
  {
    pin_press_start_ms = 0; // pin_press_fired stays set until the next press, for SHORT_CLICKED below
  }
  else if (code == LV_EVENT_SHORT_CLICKED)
  {
    return pin_press_fired;
  }
  return false;
}

// Bottom-left corner tap: previous screen.
void prev_hotspot_cb(lv_event_t *e)
{
  corner_tap_prev_screen();
}

void corner_tap_prev_screen()
{
  if (screen_pinned || in_settings || in_subscreen) return;
  nav_menu_hide();
  flight_alert_cancel();
  bambu_screen_auto_opened = false;
  bambu_auto_return_home_at_ms = 0;
  go_to_screen_blackout(adjacent_screen(current_screen, -1));
}

void corner_tap_next_screen_bottom_cb(lv_event_t *e)
{
  corner_tap_next_screen_cb(e);
}

// Invisible bottom-right hotspot on the always-on-top layer: a tap moves to
// the next screen (mirrors the bottom-left one, which goes back). Kept a bit
// smaller than the others so it doesn't cover the Calendar's last row.
void create_next_hotspot()
{
  lv_obj_t *layer = lv_disp_get_layer_top(NULL);
  lv_obj_t *hotspot = lv_obj_create(layer);
  lv_obj_set_size(hotspot, 70, 64);
  lv_obj_align(hotspot, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_style_bg_opa(hotspot, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(hotspot, 0, LV_PART_MAIN);
  lv_obj_clear_flag(hotspot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(hotspot, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(hotspot, corner_tap_next_screen_bottom_cb, LV_EVENT_SHORT_CLICKED, NULL);
}

#define AP_SSID "ESP32-Dashboard-Setup"
// Setup AP password: random, made once per device and kept in NVS (see
// ap_password_init()), shown on the Settings screen while the AP is on -
// so joining it needs someone standing at the display.
static char ap_password[9] = "";
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
static uint32_t weather_ok_ms = 0; // last fetch that worked - for the setup page
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

// In the screen language - see lang.h.
const char *wmo_weather_desc(int code)
{
  return wmo_desc_lang(code);
}

// Plain (non-LVGL) result storage: fetch_weather() runs on network_task and
// must never touch LVGL directly (not thread-safe), so it only writes here
// and flags weather_needs_ui_refresh; weather_apply_to_ui() (called from
// loop(), the LVGL-owning thread) does the actual label/icon updates.
#define WEATHER_HOURS 12 // forecast page: every hour, shown every 2nd
#define WEATHER_DAYS 6   // today + 5

struct WeatherData
{
  float temp = 0;
  int humidity = 0;
  float wind = 0;
  float uv = 0;
  int wcode = 0;
  bool is_day = true;
  float feels = 0;
  float tmax = 0, tmin = 0;
  bool has_daily = false;
  // Forecast page (forecast.ino): the next hours and the next days.
  int hourly_count = 0;
  struct { uint8_t hour; float temp; int wcode; uint8_t pop; bool is_day; } hourly[WEATHER_HOURS];
  int daily_count = 0;
  struct { uint8_t wday; float tmax, tmin; int wcode; uint8_t pop; } daily[WEATHER_DAYS];
};
static WeatherData weather_data;
static volatile bool weather_needs_ui_refresh = false;

void weather_apply_to_ui()
{
  // lv_font_roboto_medium_90 has 0-9 : ° C F and '-' (below zero).
  if (weather_temp_lab) lv_label_set_text_fmt(weather_temp_lab, "%.0f\xC2\xB0" "C", weather_data.temp);
  if (weather_desc_lab) lv_label_set_text(weather_desc_lab, wmo_weather_desc(weather_data.wcode));
  if (weather_extra_lab)
  {
    if (weather_data.has_daily)
      lv_label_set_text_fmt(weather_extra_lab, "%s %.0f\xC2\xB0  \xE2\x80\xA2  %s %.0f\xC2\xB0  %s %.0f\xC2\xB0", tr(S_FEELS),
                            weather_data.feels, tr(S_HIGH), weather_data.tmax, tr(S_LOW), weather_data.tmin);
    else
      lv_label_set_text_fmt(weather_extra_lab, "%s %.0f\xC2\xB0", tr(S_FEELS), weather_data.feels);
  }
  set_weather_icon(wmo_weather_icon(weather_data.wcode), weather_data.is_day);
  if (weather_humidity_val) lv_label_set_text_fmt(weather_humidity_val, "%d%%", weather_data.humidity);
  if (weather_wind_val) lv_label_set_text_fmt(weather_wind_val, "%.0f km/h", weather_data.wind);
  if (weather_uv_val) lv_label_set_text_fmt(weather_uv_val, "%.0f", weather_data.uv);
  forecast_apply_to_ui();
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

  // Connection came or went: show the icon again for a while (on Home).
  static bool prev_connected = false;
  if (wifi_signal_ui_connected != prev_connected)
  {
    prev_connected = wifi_signal_ui_connected;
    wifi_icon_until_ms = millis() + WIFI_ICON_SHOW_MS;
  }
  wifi_indicator_update_visibility();
  if (in_settings) settings_info_update();
}

// Always visible with auto-hide off. With it on: only on Home, and there
// only for WIFI_ICON_SHOW_MS after arriving (switch_screen()) or after the
// connection changed - or for as long as Wi-Fi is down. Runs every second
// from clock_tick_cb(), so it only touches the flags when they change.
void wifi_indicator_update_visibility()
{
  if (!wifi_signal_box) return;
  bool show = !settings.wifi_icon_autohide ||
              (current_screen == 0 && !in_settings && !in_subscreen &&
               (!wifi_signal_ui_connected || (wifi_icon_until_ms != 0 && (int32_t)(wifi_icon_until_ms - millis()) > 0)));
  if (show == lv_obj_has_flag(wifi_signal_box, LV_OBJ_FLAG_HIDDEN))
    (show ? lv_obj_clear_flag : lv_obj_add_flag)(wifi_signal_box, LV_OBJ_FLAG_HIDDEN);
  bool strike = show && !wifi_signal_ui_connected;
  if (wifi_no_signal_line && strike == lv_obj_has_flag(wifi_no_signal_line, LV_OBJ_FLAG_HIDDEN))
    (strike ? lv_obj_clear_flag : lv_obj_add_flag)(wifi_no_signal_line, LV_OBJ_FLAG_HIDDEN);
  update_icon_refresh(); // same "only on Home" rule
}

// The weather location from the setup page, or - when it's empty - the
// city of the time zone ("America/Toronto" -> "Toronto"). False if neither.
bool weather_location_effective(char *buf, size_t n)
{
  if (settings.weather_location[0])
  {
    snprintf(buf, n, "%s", settings.weather_location);
    return true;
  }
  const char *slash = strrchr(settings.tz_name, '/');
  if (!slash || !slash[1]) return false;
  snprintf(buf, n, "%s", slash + 1);
  for (char *c = buf; *c; c++)
    if (*c == '_') *c = ' ';
  return true;
}

// Blocks for ~1-2s during the HTTPS GET - fine here since this now runs on
// network_task, never on the LVGL-owning thread.
void fetch_weather()
{
  last_weather_fetch_ms = millis();
  if (WiFi.status() != WL_CONNECTED) return;

  float lat, lon;
  char location[65];
  if (!weather_location_effective(location, sizeof(location)))
  {
    Serial.println("[Weather] No location set");
    return;
  }
  if (!df_resolve_location(location, &lat, &lon))
  {
    Serial.printf("[Weather] Can't resolve location \"%s\"\n", location);
    return;
  }
  Serial.printf("[Weather] Location \"%s\" (%.4f,%.4f)\n", location, lat, lon);

  char url[520];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,wind_speed_10m,uv_index,weather_code,is_day,apparent_temperature"
           "&hourly=temperature_2m,weather_code,precipitation_probability,is_day&forecast_hours=%d"
           "&daily=temperature_2m_max,temperature_2m_min,weather_code,precipitation_probability_max"
           "&forecast_days=%d&timezone=auto",
           lat, lon, WEATHER_HOURS + 1, WEATHER_DAYS);

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
  weather_data.feels = current["apparent_temperature"] | weather_data.temp;
  JsonArray tmax = doc["daily"]["temperature_2m_max"], tmin = doc["daily"]["temperature_2m_min"];
  weather_data.has_daily = !tmax.isNull() && !tmin.isNull() && tmax.size() > 0;
  if (weather_data.has_daily)
  {
    weather_data.tmax = tmax[0] | 0.0f;
    weather_data.tmin = tmin[0] | 0.0f;
  }

  // Forecast page. Times are local ("2026-10-12T14:00") thanks to timezone=auto.
  data_lock();
  JsonObject hourly = doc["hourly"];
  JsonArray htime = hourly["time"];
  int nh = 0;
  for (size_t i = 0; i < htime.size() && nh < WEATHER_HOURS; i++)
  {
    const char *t = htime[i] | "";
    if (strlen(t) < 13) continue;
    // forecast_hours starts at the current hour: skip it, the page starts at the next one.
    if (i == 0 && htime.size() > WEATHER_HOURS) continue;
    weather_data.hourly[nh].hour = (uint8_t)atoi(t + 11);
    weather_data.hourly[nh].temp = hourly["temperature_2m"][i] | 0.0f;
    weather_data.hourly[nh].wcode = hourly["weather_code"][i] | 0;
    weather_data.hourly[nh].pop = hourly["precipitation_probability"][i] | 0;
    weather_data.hourly[nh].is_day = (hourly["is_day"][i] | 1) != 0;
    nh++;
  }
  weather_data.hourly_count = nh;
  JsonObject daily = doc["daily"];
  JsonArray dtime = daily["time"];
  int nd = 0;
  for (size_t i = 0; i < dtime.size() && nd < WEATHER_DAYS; i++)
  {
    const char *t = dtime[i] | "";
    struct tm d = {};
    if (sscanf(t, "%d-%d-%d", &d.tm_year, &d.tm_mon, &d.tm_mday) != 3) continue;
    d.tm_year -= 1900;
    d.tm_mon -= 1;
    d.tm_hour = 12;
    mktime(&d); // fills tm_wday
    weather_data.daily[nd].wday = (uint8_t)d.tm_wday;
    weather_data.daily[nd].tmax = daily["temperature_2m_max"][i] | 0.0f;
    weather_data.daily[nd].tmin = daily["temperature_2m_min"][i] | 0.0f;
    weather_data.daily[nd].wcode = daily["weather_code"][i] | 0;
    weather_data.daily[nd].pop = daily["precipitation_probability_max"][i] | 0;
    nd++;
  }
  weather_data.daily_count = nd;
  data_unlock();
  weather_ok_ms = millis();
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
  {
    float lat = prefs.getFloat("lat", 0), lon = prefs.getFloat("lon", 0);
    // Older firmware saved its Milan default even if never changed: ignore it.
    bool old_default = fabsf(lat - 45.4642f) < 0.001f && fabsf(lon - 9.19f) < 0.001f;
    if (!old_default)
      snprintf(settings.weather_location, sizeof(settings.weather_location), "%.4f,%.4f", lat, lon);
  }
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
  prefs.getString("haurl", settings.ha_url, sizeof(settings.ha_url));
  prefs.getString("hatok", settings.ha_token, sizeof(settings.ha_token));
  prefs.getString("haent", settings.ha_entities, sizeof(settings.ha_entities));
  settings.idle_return_home_min = prefs.getUChar("idlemin", settings.idle_return_home_min);
  settings.flight_alert = prefs.getBool("fltalert", settings.flight_alert);
  settings.wifi_icon_autohide = prefs.getBool("wifihide", settings.wifi_icon_autohide);
  settings.night_dim = prefs.getBool("ndim", settings.night_dim);
  settings.night_start = prefs.getUChar("nstart", settings.night_start);
  settings.night_end = prefs.getUChar("nend", settings.night_end);
  settings.night_start_min = prefs.getUChar("nstartm", settings.night_start_min);
  settings.night_end_min = prefs.getUChar("nendm", settings.night_end_min);
  settings.night_level = prefs.getUChar("nlevel", settings.night_level);
  settings.day_level = prefs.getUChar("dlevel", settings.day_level);
  settings.night_clock = prefs.getBool("nclock", settings.night_clock);
  settings.update_mode = prefs.getUChar("updmode", settings.update_mode);
  settings.lang = prefs.getUChar("lang", settings.lang);
  settings.shot_open = prefs.getBool("shotopen", settings.shot_open);
  ui_lang = settings.lang < LANG_COUNT ? settings.lang : LANG_EN;
  prefs.getString("updskip", settings.update_skip, sizeof(settings.update_skip));
  settings.home_shortcuts = prefs.getUChar("shortcuts", settings.home_shortcuts);
  prefs.getString("pxhost", settings.proxmox_host, sizeof(settings.proxmox_host));
  prefs.getString("pxtok", settings.proxmox_token_id, sizeof(settings.proxmox_token_id));
  prefs.getString("pxsec", settings.proxmox_token_secret, sizeof(settings.proxmox_token_secret));
  prefs.getString("amihost", settings.ami_host, sizeof(settings.ami_host));
  prefs.getString("amiuser", settings.ami_user, sizeof(settings.ami_user));
  prefs.getString("amisec", settings.ami_secret, sizeof(settings.ami_secret));
  prefs.getString("amiext", settings.ami_ext, sizeof(settings.ami_ext));
  prefs.getString("amictx", settings.ami_context, sizeof(settings.ami_context));
  settings.call_big = prefs.getBool("callbig", settings.call_big);
  prefs.getString("jfurl", settings.jf_url, sizeof(settings.jf_url));
  prefs.getString("jfkey", settings.jf_key, sizeof(settings.jf_key));
  settings.jf_popups = prefs.getBool("jfpop", settings.jf_popups);
  prefs.getString("jfmute", settings.jf_mute, sizeof(settings.jf_mute));
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
  prefs.putString("haurl", settings.ha_url);
  prefs.putString("hatok", settings.ha_token);
  prefs.putString("haent", settings.ha_entities);
  prefs.putUChar("idlemin", settings.idle_return_home_min);
  prefs.putBool("fltalert", settings.flight_alert);
  prefs.putBool("wifihide", settings.wifi_icon_autohide);
  prefs.putBool("ndim", settings.night_dim);
  prefs.putUChar("nstart", settings.night_start);
  prefs.putUChar("nend", settings.night_end);
  prefs.putUChar("nstartm", settings.night_start_min);
  prefs.putUChar("nendm", settings.night_end_min);
  prefs.putUChar("nlevel", settings.night_level);
  prefs.putUChar("dlevel", settings.day_level);
  prefs.putBool("nclock", settings.night_clock);
  prefs.putUChar("updmode", settings.update_mode);
  prefs.putUChar("lang", settings.lang);
  prefs.putBool("shotopen", settings.shot_open);
  prefs.putString("updskip", settings.update_skip);
  prefs.putUChar("shortcuts", settings.home_shortcuts);
  prefs.putString("pxhost", settings.proxmox_host);
  prefs.putString("pxtok", settings.proxmox_token_id);
  prefs.putString("pxsec", settings.proxmox_token_secret);
  prefs.putString("amihost", settings.ami_host);
  prefs.putString("amiuser", settings.ami_user);
  prefs.putString("amisec", settings.ami_secret);
  prefs.putString("amiext", settings.ami_ext);
  prefs.putString("amictx", settings.ami_context);
  prefs.putBool("callbig", settings.call_big);
  prefs.putString("jfurl", settings.jf_url);
  prefs.putString("jfkey", settings.jf_key);
  prefs.putBool("jfpop", settings.jf_popups);
  prefs.putString("jfmute", settings.jf_mute);
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
  out.replace("'", "&#39;"); // the page quotes attributes with '
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

// Service list for the setup page's status chips and section headers -
// web_service_status() in web_status.ino fills them in.
enum { WS_OFF, WS_OK, WS_ERR };
enum { SVC_WEATHER, SVC_CALENDAR, SVC_BAMBU, SVC_PROXMOX, SVC_STOCKS, SVC_MAKERWORLD, SVC_PACKAGES, SVC_HA, SVC_PHONE, SVC_JELLYFIN, SVC_COUNT };
static const char *const SVC_NAMES[SVC_COUNT] = {"Weather", "Calendar", "Bambu", "Proxmox", "Stocks",
                                                 "MakerWorld", "Packages", "Home Assistant", "Asterisk", "Jellyfin"};

// The setup page is sent in pieces as it's built (chunked), instead of as
// one String of 30+ KB: internal RAM is tight on this board.
static String page_buf;
static void pg_flush()
{
  if (page_buf.length()) config_server.sendContent(page_buf);
  page_buf = "";
}
static void pg(const String &s) { page_buf += s; if (page_buf.length() > 1800) pg_flush(); }
static void pg(const char *s) { page_buf += s; if (page_buf.length() > 1800) pg_flush(); }
static void pg(const __FlashStringHelper *s) { page_buf += s; if (page_buf.length() > 1800) pg_flush(); }

static const char SETUP_CSS[] PROGMEM =
    ":root{--bg:#121212;--card:#1C2228;--line:#2A333C;--muted:#8A97A0;--pri:#00BCD4;--ok:#4CAF50;--err:#EF5350}"
    "*{box-sizing:border-box}"
    "body{background:var(--bg);color:#fff;font:15px/1.4 system-ui,sans-serif;margin:0;padding:16px 16px 96px}"
    "main{max-width:720px;margin:auto}"
    "h1{font-size:22px;color:var(--pri);margin:4px 0 12px}"
    "h3{font-size:12px;letter-spacing:1px;color:var(--muted);margin:22px 4px 8px;font-weight:600}"
    ".card{background:var(--card);border-radius:14px;padding:14px 16px;margin-bottom:10px}"
    ".status{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:4px 12px;font-size:13px;color:var(--muted)}"
    ".status b{color:#fff;font-weight:500}"
    ".chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:12px}"
    ".chip{font-size:12px;padding:4px 10px;border-radius:99px;background:#232B33;color:#cfd8dc}"
    ".chip i,summary i{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:6px;flex:none}"
    ".s1{background:var(--ok)}.s2{background:var(--err)}.s0{background:#4a555f}"
    "details{background:var(--card);border-radius:14px;margin-bottom:8px}"
    "summary{list-style:none;cursor:pointer;padding:14px 16px;display:flex;align-items:center}"
    "summary::-webkit-details-marker{display:none}"
    "summary b{font-weight:500;white-space:nowrap}"
    "summary span{margin-left:auto;padding-left:10px;font-size:12px;color:var(--muted);min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}"
    "summary:after{content:'\\203A';margin-left:10px;color:var(--muted);transition:transform .2s}"
    "details[open] summary:after{transform:rotate(90deg)}"
    ".body{padding:0 16px 14px}"
    "label{display:block;font-size:13px;color:var(--muted);margin:10px 0 4px}"
    "input[type=text],input[type=password],input[type=number],input[type=file],input[type=time],select{width:100%;padding:9px 10px;border-radius:9px;"
    "border:1px solid var(--line);background:var(--bg);color:#fff;font-size:15px}"
    "input:focus,select:focus{outline:none;border-color:var(--pri)}"
    ".hint{font-size:12px;color:var(--muted);margin:6px 0 0}"
    ".hint i,.hint code{font-style:normal;background:#232B33;padding:1px 5px;border-radius:5px}"
    ".row{display:grid;grid-template-columns:1fr 1fr;gap:0 10px}.row>*{min-width:0}"
    ".sw{display:flex;align-items:center;justify-content:space-between;gap:12px;margin-top:12px;font-size:14px;color:#fff}"
    ".sw input{appearance:none;-webkit-appearance:none;width:42px;height:24px;border-radius:12px;background:#3A4652;position:relative;cursor:pointer;flex:none;margin:0}"
    ".sw input:before{content:'';position:absolute;top:3px;left:3px;width:18px;height:18px;border-radius:50%;background:#fff;transition:left .15s}"
    ".sw input:checked{background:var(--pri)}.sw input:checked:before{left:21px}"
    ".secret{display:flex;gap:8px}.secret input{flex:1;min-width:0}"
    ".secret button{flex:none;width:auto;margin:0;background:#232B33;color:var(--err);border:0;border-radius:9px;padding:0 12px;font-size:13px}"
    ".btn{display:block;width:100%;padding:11px;border:0;border-radius:10px;background:#232B33;color:#fff;font-size:15px;margin-top:12px;text-align:center;text-decoration:none;cursor:pointer}"
    ".btn.pri{background:var(--pri);color:#062A30;font-weight:600}"
    ".bar{position:fixed;left:0;right:0;bottom:0;background:#0D1013ee;border-top:1px solid var(--line);padding:12px 16px;display:flex;justify-content:center;gap:10px;transform:translateY(110%);transition:transform .2s}"
    ".bar.show{transform:none}.bar .btn{max-width:340px;margin:0}"
    "a{color:var(--pri)}";

static void page_status_dot(uint8_t st)
{
  pg("<i class='s");
  pg(String(st));
  pg("'></i>");
}

// Opens a collapsible section. svc >= 0 adds that service's status dot and
// summary; otherwise summary (may be NULL) is shown as is. Services with
// a problem open by themselves.
static void page_section(const char *title, int svc, const char *summary, bool open = false)
{
  char st_txt[64] = "";
  uint8_t st = WS_OFF;
  if (svc >= 0) st = web_service_status(svc, st_txt, sizeof(st_txt));
  pg(open || st == WS_ERR ? "<details open><summary>" : "<details><summary>");
  if (svc >= 0) page_status_dot(st);
  pg("<b>");
  pg(title);
  pg("</b><span>");
  pg(html_escape(svc >= 0 ? String(st_txt) : String(summary ? summary : "")));
  pg("</span></summary><div class='body'>");
}

static void page_section_end() { pg(F("</div></details>")); }

static void page_text(const char *label, const char *name, const String &value, const char *placeholder = "",
                      const char *type = "text", const char *extra = "")
{
  pg("<label>");
  pg(label);
  pg("</label><input type='");
  pg(type);
  pg("' name='");
  pg(name);
  pg("' placeholder='");
  pg(placeholder);
  pg("' value='");
  pg(html_escape(value));
  pg("' ");
  pg(extra);
  pg(">");
}

// A key/password field that never shows the saved value: empty keeps it,
// Remove sends "-" (which handle_save_post() turns into "not set").
static void page_secret(const char *label, const char *name, bool saved)
{
  pg("<label>");
  pg(label);
  pg("</label><div class='secret'><input type='password' autocomplete='new-password' name='");
  pg(name);
  pg("' placeholder='");
  pg(saved ? "saved \xE2\x80\x94 leave empty to keep" : "not set");
  pg("'>");
  if (saved) pg(F("<button type='button' onclick='rm(this)'>Remove</button>"));
  pg("</div>");
}

static void page_switch(const char *label, const char *name, bool on)
{
  pg("<label class='sw'>");
  pg(label);
  pg("<input type='checkbox' name='");
  pg(name);
  pg("' value='1'");
  if (on) pg(" checked");
  pg("></label>");
}

static void page_hint(const __FlashStringHelper *text)
{
  pg("<p class='hint'>");
  pg(text);
  pg("</p>");
}

static void page_select(const char *label, const char *name, const char *const *options, uint8_t count, uint8_t selected)
{
  pg("<label>");
  pg(label);
  pg("</label><select name='");
  pg(name);
  pg("'>");
  for (uint8_t i = 0; i < count; i++)
  {
    pg("<option value='" + String(i) + "'");
    if (i == selected) pg(" selected");
    pg(">");
    pg(options[i]);
    pg("</option>");
  }
  pg("</select>");
}

// "HH:MM" time picker (the browser's own), e.g. the night schedule.
static void page_time(const char *label, const char *name, uint8_t h, uint8_t m)
{
  char o[160];
  snprintf(o, sizeof(o), "<div><label>%s</label><input type='time' name='%s' value='%02d:%02d' required></div>", label, name, h, m);
  pg(o);
}


void send_settings_page()
{
  config_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  config_server.send(200, "text/html", "");
  page_buf = "";
  page_buf.reserve(2200);
  pg(F("<!DOCTYPE html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>Dashboard Setup</title><style>"));
  pg(FPSTR(SETUP_CSS));
  pg(F("</style></head><body><main><h1>Dashboard Setup</h1>"));

  // ---- Status ----
  {
    char line[160];
    uint32_t up = millis() / 1000;
    pg(F("<div class='card'><div class='status'>"));
    snprintf(line, sizeof(line), "<div>Firmware <b>v%s</b></div><div>Up <b>%lu d %lu h %lu min</b></div>", FW_VERSION,
             (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24), (unsigned long)(up / 60 % 60));
    pg(line);
    if (wifi_connected)
    {
      pg("<div>IP <b>" + WiFi.localIP().toString() + "</b></div><div>Wi-Fi <b>" + html_escape(WiFi.SSID()) + " \xC2\xB7 " +
           String(WiFi.RSSI()) + " dBm</b></div>");
    }
    else pg(F("<div>Wi-Fi <b>not connected</b></div>"));
    pg(F("</div><div class='chips'>"));
    for (int i = 0; i < SVC_COUNT; i++)
    {
      char txt[64];
      uint8_t st = web_service_status(i, txt, sizeof(txt));
      pg("<span class='chip' title='");
      pg(html_escape(txt));
      pg("'>");
      page_status_dot(st);
      pg(SVC_NAMES[i]);
      pg("</span>");
    }
    pg(F("</div></div>"));
  }

  pg(F("<form id='f' action='/save' method='POST'><h3>DEVICE</h3>"));

  // ---- Wi-Fi ----
  page_section("Wi-Fi", -1, settings.sta_ssid, !settings.sta_ssid[0]);
  pg(F("<label>Network</label><select name='ssid' id='ssid'>"));
  if (settings.sta_ssid[0]) pg("<option value='" + html_escape(settings.sta_ssid) + "' selected>" + html_escape(settings.sta_ssid) + " (saved)</option>");
  else pg(F("<option value=''>Scanning...</option>"));
  pg(F("</select>"));
  page_secret("Password", "pass", settings.sta_pass[0]);
  page_hint(F("Empty keeps the saved password while the network stays the same. Remove: an open network."));
  page_section_end();

  // ---- Clock, language, weather ----
  {
    static const char *LANG_NAMES[] = {"English", "Italiano", "Fran\xC3\xA7" "ais"};
    char sum[96];
    snprintf(sum, sizeof(sum), "%s \xC2\xB7 %s", settings.weather_location[0] ? settings.weather_location : settings.tz_name,
             LANG_NAMES[settings.lang < LANG_COUNT ? settings.lang : 0]);
    page_section("Clock, language &amp; weather", -1, sum);
    pg(F("<div class='row'><div><label>Time zone</label><select name='tz'>"));
    for (size_t i = 0; i < TZ_TABLE_COUNT; i++)
    {
      pg("<option value='");
      pg(TZ_TABLE[i].name);
      pg("'");
      if (strcmp(TZ_TABLE[i].name, settings.tz_name) == 0) pg(" selected");
      pg(">");
      pg(TZ_TABLE[i].name);
      pg("</option>");
    }
    pg(F("</select></div><div>"));
    page_select("Language", "lang", LANG_NAMES, LANG_COUNT, settings.lang);
    pg(F("</div></div>"));
    page_text("Weather location", "wloc", settings.weather_location, "Empty: the time zone city");
    page_hint(F("A city with an optional country code (<i>Montreal, CA</i>) or <i>45.50,-73.57</i>. The language covers dates, "
                   "weather, Home, Calendar, Agenda, Forecast, Packages and alerts; the technical screens stay in English."));
    page_section_end();
  }

  // ---- Screen ----
  {
    char sum[64];
    if (settings.night_dim)
      snprintf(sum, sizeof(sum), "%d%% \xC2\xB7 dim %02d:%02d\xE2\x80\x93%02d:%02d", settings.day_level, settings.night_start,
               settings.night_start_min, settings.night_end, settings.night_end_min);
    else snprintf(sum, sizeof(sum), "%d%%", settings.day_level);
    page_section("Screen", -1, sum);
    static const char *SHORTCUT_MODES[] = {"Always on Home (top-left menu on the other screens)", "Top-left corner menu", "Off"};
    page_select("Shortcut icons", "shortcuts", SHORTCUT_MODES, 3, settings.home_shortcuts);
    page_hint(F("One icon per screen; a tap on the top-left corner of any screen opens them as a menu."));
    page_text("Brightness (%)", "dlevel", String(settings.day_level), "", "number", "min='5' max='100'");
    pg(F("<input type='hidden' name='ndim_form' value='1'>"));
    page_switch("Dim at night", "ndim", settings.night_dim);
    pg(F("<div class='row'>"));
    page_time("From", "nstart", settings.night_start, settings.night_start_min);
    page_time("Until", "nend", settings.night_end, settings.night_end_min);
    pg(F("</div>"));
    page_text("Night brightness (%, 0 = screen off)", "nlevel", String(settings.night_level), "", "number", "min='0' max='100'");
    page_switch("Big clock instead of the dashboard while dimmed", "nclock", settings.night_clock);
    page_hint(F("At night the screen dims after 30 s without a touch; a touch brings it back."));
    page_section_end();
  }

  pg(F("<h3>SERVICES</h3>"));

  // ---- Bambu ----
  page_section("Bambu printer", SVC_BAMBU, NULL);
  page_text("Printer name", "pname", settings.printer_name);
  pg(F("<div class='row'><div>"));
  page_text("IP address", "pip", settings.printer_ip, "192.168.1.60");
  pg(F("</div><div>"));
  page_text("Port", "pport", String(settings.printer_port), "", "number", "min='1' max='65535'");
  pg(F("</div></div>"));
  page_text("Serial number", "mqtt_sn", settings.mqtt_sn);
  page_secret("LAN access code", "mqtt_pass", settings.mqtt_pass[0]);
  page_section_end();

  // ---- Calendar & flights ----
  page_section("Calendar &amp; flights", SVC_CALENDAR, NULL);
  for (int i = 0; i < ICS_MAX_FEEDS; i++)
  {
    char lab[16];
    snprintf(lab, sizeof(lab), "iCal URL %d", i + 1);
    page_text(lab, ICS_PREF_KEYS[i], settings.ics_urls[i], "https://calendar.google.com/calendar/ical/.../basic.ics");
  }
  page_hint(F("Up to 3 .ics links, merged. Google Calendar: Settings -&gt; your calendar -&gt; Integrate calendar -&gt; "
                 "Secret address in iCal format (treat it like a password). Flights in a title like <i>AC871 YUL-&gt;YYZ</i> "
                 "are pinned on Home with a countdown."));
  page_secret("AeroDataBox API key (live flight status, optional)", "adbkey", settings.aerodatabox_key[0]);
  page_hint(F("Free key: rapidapi.com -&gt; AeroDataBox -&gt; Basic plan. Delays, gate and departed/landed on the day."));
  page_section_end();

  // ---- Stocks ----
  page_section("Stocks", SVC_STOCKS, NULL);
  page_text("Symbols (up to 7, comma separated)", "stocks", settings.stock_symbols, "AAPL,NVDA,^IXIC");
  page_hint(F("Yahoo Finance symbols. Indexes start with ^."));
  page_section_end();

  // ---- MakerWorld ----
  page_section("MakerWorld", SVC_MAKERWORLD, NULL);
  page_text("User ID", "mwuid", settings.makerworld_uid, "", "text", "inputmode='numeric'");
  page_hint(F("The number in your profile's published-models address (/design-service/published/&lt;number&gt;/design)."));
  page_section_end();

  // ---- Packages ----
  page_section("Packages", SVC_PACKAGES, NULL);
  page_secret("PKGE API key", "pkge", settings.pkge_api_key[0]);
  page_hint(F("From business.pkge.net -&gt; Company profile -&gt; API key. Add tracking numbers in the PKGE dashboard."));
  page_section_end();

  // ---- Proxmox ----
  page_section("Proxmox cluster", SVC_PROXMOX, NULL);
  page_text("Host (ip:port)", "pxhost", settings.proxmox_host, "192.168.1.50:8006");
  page_text("API token ID", "pxtok", settings.proxmox_token_id, "root@pam!dashboard");
  page_secret("API token secret", "pxsec", settings.proxmox_token_secret[0]);
  page_hint(F("Datacenter -&gt; Permissions -&gt; API Tokens. A read-only PVEAuditor token is enough."));
  page_section_end();

  // ---- Home Assistant ----
  page_section("Home Assistant", SVC_HA, NULL);
  page_text("Address", "haurl", settings.ha_url, "http://homeassistant.local:8123");
  page_secret("Long-lived access token", "hatok", settings.ha_token[0]);
  page_text("Entities (up to 8, comma separated)", "haent", settings.ha_entities, "light.desk, switch.fan, sensor.outdoor_temperature");
  page_hint(F("Token: your HA profile -&gt; Security -&gt; Long-lived access tokens. Lights, switches, fans, input "
                 "booleans, automations and scripts toggle with a tap."));
  page_section_end();

  // ---- Asterisk ----
  page_section("Asterisk", SVC_PHONE, NULL);
  pg(F("<div class='row'><div>"));
  page_text("Manager host", "amihost", settings.ami_host, "192.168.1.10:5038");
  pg(F("</div><div>"));
  page_text("AMI user", "amiuser", settings.ami_user, "dashboard");
  pg(F("</div></div>"));
  page_secret("AMI secret", "amisec", settings.ami_secret[0]);
  pg(F("<div class='row'><div>"));
  page_text("Extensions to watch", "amiext", settings.ami_ext, "101, 102 - empty: all");
  pg(F("</div><div>"));
  page_text("Dial context (call back)", "amictx", settings.ami_context, "from-internal");
  pg(F("</div></div><input type='hidden' name='ami_form' value='1'>"));
  page_switch("Big caller ID (the number in big digits)", "callbig", settings.call_big);
  page_hint(F("<code>manager.conf</code>: <code>read = call</code>, <code>write = reporting,originate</code> (voicemail "
                 "counts; call back rings the first extension, then dials the number)."));
  page_section_end();

  // ---- Jellyfin ----
  page_section("Jellyfin", SVC_JELLYFIN, NULL);
  page_text("Address", "jfurl", settings.jf_url, "http://192.168.1.20:8096");
  page_secret("API key", "jfkey", settings.jf_key[0]);
  pg(F("<input type='hidden' name='jf_form' value='1'>"));
  page_switch("Pop-ups on the TV: calls (with Asterisk), print started / finished / failed, printer problems", "jfpop",
              settings.jf_popups);
  {
    // One switch per Jellyfin device seen (and every muted one): off = no pop-ups there.
    char names[16][40]; // up to 8 seen (JF_DEVICE_MAX in jellyfin.ino) + muted ones
    int n = jf_device_list(names, 16);
    if (n) pg(F("<label>Pop-ups on</label>"));
    for (int i = 0; i < n; i++)
    {
      String esc = html_escape(names[i]);
      pg("<input type='hidden' name='jfall' value='" + esc + "'>");
      pg("<label class='sw'>" + esc + "<input type='checkbox' name='jfdev' value='" + esc + "'");
      if (!jf_device_muted(names[i])) pg(" checked");
      pg("></label>");
    }
  }
  page_hint(F("Jellyfin Dashboard -&gt; API Keys. Pop-ups go to the Jellyfin apps used in the last 15 minutes; a device shows "
              "up here once the dashboard has seen it."));
  page_section_end();

  // ---- Settings that are maintenance, but saved with the rest ----
  pg(F("<h3>MAINTENANCE</h3>"));
  {
    static const char *UPDATE_MODES[] = {"Show an icon on Home - I choose", "Install automatically", "Don't check (Settings -> Check update only)"};
    static const char *UPDATE_SHORT[] = {"ask", "automatic", "off"};
    char sum[48];
    snprintf(sum, sizeof(sum), "v%s \xC2\xB7 updates: %s", FW_VERSION, UPDATE_SHORT[settings.update_mode < 3 ? settings.update_mode : 0]);
    page_section("Firmware", -1, sum);
    page_select("When a new release is out", "updmode", UPDATE_MODES, 3, settings.update_mode);
    page_hint(F("Checked once a day against github.com/" GH_REPO " releases."));
    pg(F("<div class='row'><a class='btn' href='/firmware.bin'>Download running (.bin)</a>"
           "<a class='btn' href='https://github.com/" GH_REPO "/releases/latest/download/" GH_ASSET "'>Download latest</a></div>"
           "<button class='btn' type='button' onclick='ghi()'>Install from GitHub\xE2\x80\xA6</button>"
           "<label>Upload a .bin</label><input type='file' id='fwfile' accept='.bin'>"
           "<button class='btn' type='button' onclick='upl()'>Upload firmware</button>"));
    page_hint(F("Upload the <i>.ino.bin</i> from a release (not the merged one). Install from GitHub also reinstalls the "
                   "running version or goes back to an older one. The screen goes black for about 90 s, then the device restarts."));
    page_section_end();
  }

  page_section("Display", -1, NULL);
  pg(F("<div class='row'><button class='btn' type='button' onclick=\"fetch('/panel?do=stream').then(r=>r.text()).then(alert)\">"
       "Restart picture stream</button>"
       "<button class='btn' type='button' onclick=\"fetch('/panel?do=init').then(r=>r.text()).then(alert)\">"
       "Re-initialise panel</button></div>"
       "<div class='row'><button class='btn' type='button' onclick=\"fetch('/panel?do=reset').then(r=>r.text()).then(alert)\">"
       "Reset display controller</button>"
       "<button class='btn' type='button' onclick=\"fetch('/panel?do=info').then(r=>r.text()).then(alert)\">"
       "Display diagnostics</button></div>"
       "<button class='btn' type='button' onclick=\"fetch('/panel?do=swreset').then(r=>r.text()).then(alert)\">"
       "Panel software reset</button>"
       "<button class='btn' type='button' onclick=\"fetch('/panel?do=pause').then(r=>r.text()).then(alert)\">"
       "Stop panel signal 1.5 s</button>"
       "<button class='btn pri' type='button' onclick=\"fetch('/panel?do=boot').then(r=>r.text()).then(alert)\">"
       "Restart panel as at boot</button>"
       "<button class='btn' type='button' onclick=\"if(confirm('Reboot the dashboard now?'))"
       "fetch('/reboot',{method:'POST'}).then(r=>r.text()).then(alert)\">Reboot</button>"));
  page_hint(F("If the picture is shifted (the top shows at the bottom): try the first button, then the second. The "
              "first restarts the stream the board sends to the panel (it also runs every second by itself); the "
              "second re-sends the panel's setup, as at power-on; the third resets the board's display "
              "controller and sets it up again, as at boot. The screen may blink."));
  page_section_end();

  page_section("Backup &amp; screenshots", -1, settings.shot_open ? "screenshots open" : NULL);
  pg(F("<a class='btn' href='/backup'>Download settings</a>"
         "<label>Restore from a file</label><input type='file' id='bkfile' accept='.json'>"
         "<button class='btn' type='button' onclick='rst()'>Restore settings</button>"));
  page_hint(F("The backup includes your passwords and keys - keep it private. Restoring restarts the device."));
  pg(F("<input type='hidden' name='shot_form' value='1'>"));
  page_switch("Screenshots without login", "shotopen", settings.shot_open);
  pg(F("<label>Screenshot of</label><select id='shotpage'><option value=''>What's on screen now</option>"));
  {
    // Same list as SHOT_PAGES (defined further down, with handle_screenshot()).
    static const char *const PAGES[][2] = {
        {"home", "Home"}, {"bambu", "Bambu printer"}, {"calendar", "Calendar"}, {"calday", "Calendar - today"},
        {"agenda", "Agenda"}, {"forecast", "Forecast"}, {"proxmox", "Proxmox"}, {"stocks", "Stocks"},
        {"makerworld", "MakerWorld"}, {"packages", "Packages"}, {"flight", "Flight"}, {"homeassistant", "Home Assistant"},
        {"phone", "Phone (Asterisk)"}, {"jellyfin", "Jellyfin"}, {"history", "Print history"}, {"settings", "Settings screen"},
        {"quick", "Quick panel"}, {"night", "Night clock"}};
    for (auto &page : PAGES)
    {
      pg("<option value='");
      pg(page[0]);
      pg("'>");
      pg(page[1]);
      pg("</option>");
    }
  }
  pg(F("</select><button class='btn' type='button' onclick=\"var v=document.getElementById('shotpage').value;"
         "window.open('/screenshot'+(v?'?page='+v:''),'_blank')\">Take screenshot</button>"));
  page_hint(F("<i>/screenshot?page=forecast</i> returns that screen. With screenshots open, anyone on your Wi-Fi can see "
                 "the screen - turn it on only while you need it."));
  page_section_end();
  pg(F("</form></main>"
         "<div class='bar' id='bar'><button class='btn' type='button' onclick='location.reload()'>Discard</button>"
         "<button class='btn pri' type='submit' form='f'>Save &amp; restart</button></div>"
         "<script>"
         "var f=document.getElementById('f'),bar=document.getElementById('bar');"
         "function dirty(){bar.classList.add('show')}"
         "f.addEventListener('input',dirty);f.addEventListener('change',dirty);"
         "function rm(b){var i=b.previousElementSibling;i.value='-';i.type='text';i.readOnly=true;b.textContent='Removed';b.disabled=true;dirty()}"
         // Files go as their own POST (not part of the settings form).
         "function post(url,id,field,msg){var x=document.getElementById(id).files[0];if(!x){alert('Pick a file first');return}"
         "if(!confirm(msg))return;var d=new FormData();d.append(field,x);document.body.style.opacity=.5;"
         "fetch(url,{method:'POST',body:d}).then(r=>r.text()).then(t=>{document.open();document.write(t);document.close()})"
         ".catch(e=>{alert('Failed: '+e);document.body.style.opacity=1})}"
         "function upl(){post('/update','fwfile','fw','Upload this firmware? The device restarts.')}"
         "function rst(){post('/restore','bkfile','backup','Restore these settings? The device restarts.')}"
         "function ghi(){var v=prompt('Install from GitHub: a version (e.g. 3.19), or empty for the latest','');if(v===null)return;"
         "if(v&&!/^[0-9]+(\\.[0-9]+)*$/.test(v)){alert('Digits and dots only');return}"
         "var d=new URLSearchParams();d.append('ver',v);fetch('/ghinstall',{method:'POST',body:d}).then(r=>r.text())"
         ".then(t=>{document.open();document.write(t);document.close()})}"
         "var saved='"));
  pg(js_escape(settings.sta_ssid));
  pg(F("';"
         "fetch('/scan').then(r=>r.json()).then(list=>{"
         "var sel=document.getElementById('ssid');sel.innerHTML='';var found=false;"
         "list.sort((a,b)=>b.rssi-a.rssi);"
         "list.forEach(n=>{var o=document.createElement('option');o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+' dBm)';"
         "if(n.ssid===saved){o.selected=true;found=true;}sel.appendChild(o);});"
         "if(saved&&!found){var o=document.createElement('option');o.value=saved;o.textContent=saved+' (saved, not in range)';o.selected=true;sel.insertBefore(o,sel.firstChild);}"
         "});"
         "</script></body></html>"));
  pg_flush();
  page_buf = String(); // give the memory back
  config_server.sendContent("");
}

void handle_root()
{
  if (!web_auth()) return;
  send_settings_page();
}

void handle_scan()
{
  if (!web_auth()) return;
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
  if (!web_auth()) return;
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
  // The page never sends secrets back: empty keeps the saved one, "-" removes it.
  if (config_server.hasArg("mqtt_pass"))
  {
    String v = config_server.arg("mqtt_pass");
    if (v == "-") settings.mqtt_pass[0] = '\0';
    else if (v.length()) v.toCharArray(settings.mqtt_pass, sizeof(settings.mqtt_pass));
  }
  if (config_server.hasArg("ssid"))
  {
    // Wi-Fi: an empty password keeps the saved one only while the network
    // stays the same - another network with no password is an open one.
    String ssid = config_server.arg("ssid"), pass = config_server.arg("pass");
    bool same = ssid == settings.sta_ssid;
    ssid.toCharArray(settings.sta_ssid, sizeof(settings.sta_ssid));
    if (pass == "-" || (pass.length() == 0 && !same)) settings.sta_pass[0] = '\0';
    else if (pass.length()) pass.toCharArray(settings.sta_pass, sizeof(settings.sta_pass));
  }
  if (config_server.hasArg("shot_form")) settings.shot_open = config_server.hasArg("shotopen");
  if (config_server.hasArg("ami_form")) settings.call_big = config_server.hasArg("callbig");
  if (config_server.hasArg("jf_form"))
  {
    settings.jf_popups = config_server.hasArg("jfpop");
    // Muted = devices listed on the page whose switch is off.
    String mute;
    for (int i = 0; i < config_server.args(); i++)
    {
      if (config_server.argName(i) != "jfall") continue;
      String dev = config_server.arg(i);
      bool on = false;
      for (int k = 0; k < config_server.args() && !on; k++)
        on = config_server.argName(k) == "jfdev" && config_server.arg(k) == dev;
      if (on || dev.length() == 0 || mute.length() + dev.length() + 1 >= sizeof(settings.jf_mute)) continue;
      if (mute.length()) mute += "|";
      mute += dev;
    }
    mute.toCharArray(settings.jf_mute, sizeof(settings.jf_mute));
  }
  if (config_server.hasArg("lang")) settings.lang = (uint8_t)constrain(config_server.arg("lang").toInt(), 0, LANG_COUNT - 1);
  if (config_server.hasArg("updmode")) settings.update_mode = (uint8_t)constrain(config_server.arg("updmode").toInt(), 0, 2);
  if (config_server.hasArg("ndim_form")) // unchecked boxes aren't sent
  {
    settings.night_dim = config_server.hasArg("ndim");
    settings.night_clock = config_server.hasArg("nclock");
  }
  // Night schedule: "HH:MM" (a bare hour still works).
  struct { const char *arg; uint8_t *h, *m; } times[] = {{"nstart", &settings.night_start, &settings.night_start_min},
                                                         {"nend", &settings.night_end, &settings.night_end_min}};
  for (auto &t : times)
  {
    if (!config_server.hasArg(t.arg)) continue;
    String v = config_server.arg(t.arg);
    int colon = v.indexOf(':');
    *t.h = (uint8_t)constrain(v.toInt(), 0, 23);
    *t.m = colon > 0 ? (uint8_t)constrain(v.substring(colon + 1).toInt(), 0, 59) : 0;
  }
  if (config_server.hasArg("nlevel")) settings.night_level = (uint8_t)constrain(config_server.arg("nlevel").toInt(), 0, 100);
  if (config_server.hasArg("dlevel")) settings.day_level = (uint8_t)constrain(config_server.arg("dlevel").toInt(), 5, 100);
  if (config_server.hasArg("shortcuts")) settings.home_shortcuts = (uint8_t)constrain(config_server.arg("shortcuts").toInt(), 0, 2);
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
  if (config_server.hasArg("haurl"))
  {
    String v = config_server.arg("haurl");
    v.trim();
    v.toCharArray(settings.ha_url, sizeof(settings.ha_url));
  }
  if (config_server.hasArg("hatok"))
  {
    String key = config_server.arg("hatok");
    key.trim();
    if (key == "-") settings.ha_token[0] = '\0';
    else if (key.length() > 0) key.toCharArray(settings.ha_token, sizeof(settings.ha_token));
  }
  if (config_server.hasArg("haent"))
  {
    String v = config_server.arg("haent");
    v.trim();
    v.toCharArray(settings.ha_entities, sizeof(settings.ha_entities));
  }
  if (config_server.hasArg("pxhost")) config_server.arg("pxhost").toCharArray(settings.proxmox_host, sizeof(settings.proxmox_host));
  if (config_server.hasArg("pxtok")) config_server.arg("pxtok").toCharArray(settings.proxmox_token_id, sizeof(settings.proxmox_token_id));
  if (config_server.hasArg("pxsec"))
  {
    String v = config_server.arg("pxsec");
    if (v == "-") settings.proxmox_token_secret[0] = '\0';
    else if (v.length()) v.toCharArray(settings.proxmox_token_secret, sizeof(settings.proxmox_token_secret));
  }
  // Asterisk and Jellyfin: plain fields are trimmed and stored as typed,
  // secrets keep the saved value when left empty and are removed with "-".
  struct { const char *arg; char *dst; size_t n; bool secret; } extra[] = {
      {"amihost", settings.ami_host, sizeof(settings.ami_host), false},
      {"amiuser", settings.ami_user, sizeof(settings.ami_user), false},
      {"amisec", settings.ami_secret, sizeof(settings.ami_secret), true},
      {"amiext", settings.ami_ext, sizeof(settings.ami_ext), false},
      {"amictx", settings.ami_context, sizeof(settings.ami_context), false},
      {"jfurl", settings.jf_url, sizeof(settings.jf_url), false},
      {"jfkey", settings.jf_key, sizeof(settings.jf_key), true},
  };
  for (auto &f : extra)
  {
    if (!config_server.hasArg(f.arg)) continue;
    String v = config_server.arg(f.arg);
    v.trim();
    if (f.secret && v == "-") f.dst[0] = '\0';
    else if (!f.secret || v.length() > 0) v.toCharArray(f.dst, f.n);
  }
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

// Firmware upload from the setup page (see send_settings_page()). Runs on
// network_task; the screen shows "Updating firmware" via ota_apply_to_ui().
static volatile uint8_t ota_percent = 0;
static volatile bool ota_needs_ui_refresh = false;

// Updates from GitHub releases (gh_update.cpp): checked once a day and from
// the Settings screen's Update button, installed only from that button.
enum { GH_IDLE, GH_CHECKING, GH_UP_TO_DATE, GH_AVAILABLE, GH_INSTALLING, GH_FAILED };
static volatile uint8_t gh_state = GH_IDLE;
static volatile bool gh_check_request = false;
static volatile bool gh_install_request = false;
// From the setup page: install a release even when it isn't newer -
// gh_force_version, or the latest when empty.
static volatile bool gh_force_request = false;
static char gh_force_version[16] = "";
static volatile bool gh_needs_ui_refresh = false;
static char gh_latest[16] = "";
static char gh_error[48] = "";
static uint32_t last_gh_check_ms = 0;
#define GH_CHECK_INTERVAL_MS (24UL * 60UL * 60UL * 1000UL)
#define GH_RETRY_INTERVAL_MS (15UL * 60UL * 1000UL)
#define GH_FIRST_CHECK_MS (3UL * 60UL * 1000UL)
#define GH_MIN_BLOCK 24000 // largest free internal block a TLS handshake needs (~17 KB record buffer + margin)

void gh_progress(int done, int total)
{
  esp_task_wdt_reset();
  uint8_t pct = total > 0 ? (uint8_t)(done * 100LL / total) / 5 * 5 : 0; // redraw every 5% only
  if (pct != ota_percent) { ota_percent = pct; ota_needs_ui_refresh = true; }
}

// network_task side: run whatever the UI asked for.
void gh_update_maintain()
{
  if (gh_force_request)
  {
    gh_force_request = false;
    char v[sizeof(gh_latest)];
    if (gh_force_version[0]) strlcpy(v, gh_force_version, sizeof(v));
    else if (!gh_latest_version(v, sizeof(v)))
    {
      snprintf(gh_error, sizeof(gh_error), "Couldn't reach GitHub");
      gh_state = GH_FAILED;
      gh_needs_ui_refresh = true;
      return;
    }
    Serial.printf("[Update] forced install of %s from the setup page\n", v);
    memcpy(gh_latest, v, sizeof(gh_latest));
    gh_state = GH_AVAILABLE; // the install below runs on the next pass
    gh_install_request = true;
    gh_needs_ui_refresh = true;
    return;
  }
  if (gh_install_request && gh_state != GH_AVAILABLE) gh_install_request = false; // stale tap
  // Scheduled checks: not in the first minutes after boot (every widget
  // fetches then, and internal RAM is tight), once a day, sooner again
  // after a failure.
  uint32_t interval = gh_state == GH_FAILED ? GH_RETRY_INTERVAL_MS : GH_CHECK_INTERVAL_MS;
  bool due = settings.update_mode != UPDATE_OFF && millis() > GH_FIRST_CHECK_MS &&
             (last_gh_check_ms == 0 || millis() - last_gh_check_ms > interval);
  if (gh_install_request && gh_state == GH_AVAILABLE)
  {
    gh_install_request = false;
    gh_state = GH_INSTALLING;
    ota_in_progress = true;
    ota_started_ms = millis();
    ota_percent = 0;
    ota_needs_ui_refresh = true;
    gh_needs_ui_refresh = true;
    vTaskDelay(pdMS_TO_TICKS(OTA_DARK_AFTER_MS + 500)); // let the card be read before the screen goes dark
    gh_install(gh_latest, gh_progress, gh_error, sizeof(gh_error)); // restarts on success
    ota_in_progress = false;
    ota_needs_ui_refresh = true;
    gh_state = GH_FAILED;
    gh_needs_ui_refresh = true;
    return;
  }
  if (!gh_check_request && !due) return;
  bool manual = gh_check_request;
  if (ESP.getMaxAllocHeap() < GH_MIN_BLOCK)
  {
    // Not enough contiguous RAM for TLS right now: try again in a bit.
    Serial.printf("[Update] check postponed: largest free block %u\n", (unsigned)ESP.getMaxAllocHeap());
    if (!manual) last_gh_check_ms = millis() - GH_CHECK_INTERVAL_MS + GH_RETRY_INTERVAL_MS;
    if (manual)
    {
      gh_check_request = false;
      snprintf(gh_error, sizeof(gh_error), "Low memory, try again");
      gh_state = GH_FAILED;
      gh_needs_ui_refresh = true;
    }
    return;
  }
  gh_check_request = false;
  last_gh_check_ms = millis();
  gh_state = GH_CHECKING;
  gh_needs_ui_refresh = true;
  char v[sizeof(gh_latest)];
  if (!gh_latest_version(v, sizeof(v)))
  {
    snprintf(gh_error, sizeof(gh_error), "Couldn't reach GitHub");
    gh_state = GH_FAILED;
  }
  else
  {
    memcpy(gh_latest, v, sizeof(gh_latest));
    gh_state = gh_version_newer(gh_latest, FW_VERSION) ? GH_AVAILABLE : GH_UP_TO_DATE;
    Serial.printf("[Update] latest %s, running " FW_VERSION "\n", gh_latest);
    // Automatic mode: a scheduled check that finds a release installs it
    // right away (the next pass of this function does it).
    if (gh_state == GH_AVAILABLE && settings.update_mode == UPDATE_AUTO && !manual) gh_install_request = true;
  }
  gh_needs_ui_refresh = true;
}

void handle_update_upload()
{
  if (!web_auth_quiet()) return;
  HTTPUpload &up = config_server.upload();
  if (up.status == UPLOAD_FILE_START)
  {
    Serial.printf("[OTA] receiving %s\n", up.filename.c_str());
    ota_in_progress = true;
    ota_started_ms = millis();
    ota_percent = 0;
    ota_needs_ui_refresh = true;
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  }
  else if (up.status == UPLOAD_FILE_WRITE)
  {
    esp_task_wdt_reset();
    if (Update.isRunning() && Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
    // Content-Length covers the whole multipart body - close enough for a progress bar.
    size_t total = config_server.clientContentLength();
    uint8_t pct = total ? (uint8_t)min<size_t>(99, Update.progress() * 100 / total) / 5 * 5 : 0; // every 5%
    if (pct != ota_percent) { ota_percent = pct; ota_needs_ui_refresh = true; }
  }
  else if (up.status == UPLOAD_FILE_END)
  {
    if (Update.end(true)) Serial.printf("[OTA] %u bytes written\n", (unsigned)up.totalSize);
    else Update.printError(Serial);
  }
  else if (up.status == UPLOAD_FILE_ABORTED)
  {
    Update.abort();
    Serial.println("[OTA] upload aborted");
  }
}

void handle_update_done()
{
  if (!web_auth()) return;
  bool ok = !Update.hasError() && Update.isFinished();
  config_server.send(200, "text/html",
                     ok ? "<html><body style='background:#121212;color:#fff;font-family:sans-serif;text-align:center;padding-top:60px'>"
                          "<h2 style='color:#00BCD4'>Updated</h2><p>The device is restarting with the new firmware.</p></body></html>"
                        : "<html><body style='background:#121212;color:#fff;font-family:sans-serif;text-align:center;padding-top:60px'>"
                          "<h2 style='color:#EF5350'>Update failed</h2><p>Nothing was changed. Check that the file is the "
                          "<i>.ino.bin</i> for this board and try again.</p></body></html>");
  if (ok)
  {
    Serial.println("[OTA] done - restarting");
    delay(1000);
    ESP.restart();
  }
  ota_in_progress = false;
  ota_needs_ui_refresh = true;
}

// ==================== Settings backup / restore ====================
// The whole "dash" NVS namespace as JSON: {"key": ["type", value], ...}.
// Generic over whatever keys exist, so new settings are covered without
// touching this. The AP password stays per device and isn't included.
void handle_backup()
{
  if (!web_auth()) return;
  JsonDocument doc;
  doc["firmware"] = FW_VERSION;
  JsonObject out = doc["settings"].to<JsonObject>();
  nvs_handle_t h;
  if (nvs_open("dash", NVS_READONLY, &h) == ESP_OK)
  {
    nvs_iterator_t it = NULL;
    esp_err_t res = nvs_entry_find("nvs", "dash", NVS_TYPE_ANY, &it);
    while (res == ESP_OK)
    {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      if (strcmp(info.key, "appass") != 0)
      {
        JsonArray v = out[info.key].to<JsonArray>();
        switch (info.type)
        {
          case NVS_TYPE_U8: { uint8_t x; nvs_get_u8(h, info.key, &x); v.add("u8"); v.add(x); break; }
          case NVS_TYPE_I8: { int8_t x; nvs_get_i8(h, info.key, &x); v.add("i8"); v.add(x); break; }
          case NVS_TYPE_U16: { uint16_t x; nvs_get_u16(h, info.key, &x); v.add("u16"); v.add(x); break; }
          case NVS_TYPE_I16: { int16_t x; nvs_get_i16(h, info.key, &x); v.add("i16"); v.add(x); break; }
          case NVS_TYPE_U32: { uint32_t x; nvs_get_u32(h, info.key, &x); v.add("u32"); v.add(x); break; }
          case NVS_TYPE_I32: { int32_t x; nvs_get_i32(h, info.key, &x); v.add("i32"); v.add(x); break; }
          case NVS_TYPE_STR:
          {
            size_t len = 0;
            nvs_get_str(h, info.key, NULL, &len);
            char *buf = (char *)malloc(len + 1);
            if (buf && nvs_get_str(h, info.key, buf, &len) == ESP_OK) { v.add("str"); v.add(buf); }
            free(buf);
            break;
          }
          default: out.remove(info.key); break; // blobs etc.: not settings
        }
      }
      res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(h);
  }
  String json;
  serializeJsonPretty(doc, json);
  config_server.sendHeader("Content-Disposition", "attachment; filename=\"dashboard-settings.json\"");
  config_server.send(200, "application/json", json);
}

static String restore_body;

void handle_restore_upload()
{
  if (!web_auth_quiet()) return;
  HTTPUpload &up = config_server.upload();
  if (up.status == UPLOAD_FILE_START) restore_body = "";
  else if (up.status == UPLOAD_FILE_WRITE && restore_body.length() < 32768) restore_body.concat((const char *)up.buf, up.currentSize);
}

void handle_restore_done()
{
  if (!web_auth()) return;
  JsonDocument doc;
  bool ok = !deserializeJson(doc, restore_body) && doc["settings"].is<JsonObject>();
  restore_body = "";
  nvs_handle_t h;
  int written = 0;
  if (ok && nvs_open("dash", NVS_READWRITE, &h) == ESP_OK)
  {
    for (JsonPair kv : doc["settings"].as<JsonObject>())
    {
      const char *key = kv.key().c_str();
      const char *type = kv.value()[0] | "";
      JsonVariant v = kv.value()[1];
      if (strlen(key) > 15 || strcmp(key, "appass") == 0) continue;
      esp_err_t e = ESP_FAIL;
      if (!strcmp(type, "str")) e = nvs_set_str(h, key, v | "");
      else if (!strcmp(type, "u8")) e = nvs_set_u8(h, key, v.as<uint8_t>());
      else if (!strcmp(type, "i8")) e = nvs_set_i8(h, key, v.as<int8_t>());
      else if (!strcmp(type, "u16")) e = nvs_set_u16(h, key, v.as<uint16_t>());
      else if (!strcmp(type, "i16")) e = nvs_set_i16(h, key, v.as<int16_t>());
      else if (!strcmp(type, "u32")) e = nvs_set_u32(h, key, v.as<uint32_t>());
      else if (!strcmp(type, "i32")) e = nvs_set_i32(h, key, v.as<int32_t>());
      if (e == ESP_OK) written++;
    }
    nvs_commit(h);
    nvs_close(h);
  }
  ok = ok && written > 0;
  Serial.printf("[Backup] restore: %d settings\n", written);
  config_server.send(200, "text/html",
                     ok ? "<html><body style='background:#121212;color:#fff;font-family:sans-serif;text-align:center;padding-top:60px'>"
                          "<h2 style='color:#00BCD4'>Restored</h2><p>The device is restarting with those settings.</p></body></html>"
                        : "<html><body style='background:#121212;color:#fff;font-family:sans-serif;text-align:center;padding-top:60px'>"
                          "<h2 style='color:#EF5350'>Restore failed</h2><p>That isn't a settings file from this dashboard. "
                          "Nothing was changed.</p></body></html>");
  if (ok)
  {
    delay(1000);
    ESP.restart();
  }
}

// Setup page: install a GitHub release even when it isn't newer (or an
// older one). The download and flashing run on network_task.
void handle_gh_install()
{
  if (!web_auth()) return;
  String v = config_server.arg("ver");
  v.trim();
  bool ok = v.length() < sizeof(gh_force_version);
  for (size_t i = 0; ok && i < v.length(); i++) ok = isdigit((unsigned char)v[i]) || v[i] == '.';
  if (!ok)
  {
    config_server.send(400, "text/plain", "Version: digits and dots only, e.g. 3.19");
    return;
  }
  strlcpy(gh_force_version, v.c_str(), sizeof(gh_force_version));
  gh_force_request = true;
  String msg = v.length() ? "v" + v : String("the latest release");
  config_server.send(200, "text/html",
                     "<html><body style='background:#121212;color:#fff;font-family:sans-serif;text-align:center;padding-top:60px'>"
                     "<h2 style='color:#00BCD4'>Installing " + msg + "</h2><p>From GitHub: the screen goes black for "
                     "about 90 seconds, then the device restarts. If it can't, the Settings screen says why.</p></body></html>");
}

// Setup page: the firmware this board is running, as a .bin (the app
// image only, as the GitHub release has it - ready to upload again).
// The display's start-up order at boot, without rebooting: the panel's
// signals stop (pins idle, as during a reboot), the ST7701 is reset and
// set up while there's no picture signal, then the signals come back and
// the stream restarts from the top. loop() only (blocks ~1 s).
void panel_boot_sequence()
{
  Serial.println("[Panel] boot sequence: signals off, reset + setup, signals on");
  rgb_panel_signals(false, 18, 17, 16, RGB_PCLK_GPIO); // DE, VSYNC, HSYNC, PCLK - see rgbpanel
  delay(300);
  static const uint8_t swreset[] = {BEGIN_WRITE, WRITE_COMMAND_8, 0x01, END_WRITE, DELAY, 150};
  bus->batchOperation((uint8_t *)swreset, sizeof(swreset));
  bus->batchOperation((uint8_t *)st7701_type9_init_operations, sizeof(st7701_type9_init_operations));
  delay(100);
  rgb_panel_signals(true, 18, 17, 16, RGB_PCLK_GPIO);
  rgb_panel_resync(rgbpanel);
  lv_obj_invalidate(lv_scr_act());
}

// Setup page, Maintenance -> Display -> Reboot.
void handle_reboot()
{
  if (!web_auth()) return;
  config_server.send(200, "text/plain", "Rebooting - the page works again in about 20 seconds.");
  Serial.println("Reboot from the setup page");
  delay(1000); // let the answer reach the browser
  ESP.restart();
}

// Setup page, Maintenance -> Display: fixes for a shifted picture, by hand,
// to tell which part lost sync. "stream": restart the RGB DMA (the same as
// rgb_resync_timer_cb()); "init": re-send the ST7701 setup over its SPI pins
// (separate from the RGB pins, so it can run while the picture streams).
void handle_panel()
{
  if (!web_auth()) return;
  String what = config_server.arg("do");
  if (what == "stream")
  {
    rgb_panel_resync(rgbpanel);
    Serial.println("[Panel] stream restart (setup page)");
    config_server.send(200, "text/plain", "Picture stream restarted. Is the picture back in place?");
  }
  else if (what == "init")
  {
    bus->batchOperation((uint8_t *)st7701_type9_init_operations, sizeof(st7701_type9_init_operations));
    rgb_panel_resync(rgbpanel);
    Serial.println("[Panel] re-initialised (setup page)");
    config_server.send(200, "text/plain", "Panel re-initialised. Is the picture back in place?");
  }
  else if (what == "swreset")
  {
    // The board's stream checks out (Display diagnostics) while the panel
    // shows the picture from the wrong line: reset the ST7701 itself, as at
    // power-on - software reset (0x01), 150 ms, then its whole setup again.
    static const uint8_t swreset[] = {BEGIN_WRITE, WRITE_COMMAND_8, 0x01, END_WRITE, DELAY, 150};
    bus->batchOperation((uint8_t *)swreset, sizeof(swreset));
    bus->batchOperation((uint8_t *)st7701_type9_init_operations, sizeof(st7701_type9_init_operations));
    rgb_panel_resync(rgbpanel);
    Serial.println("[Panel] software reset + setup (setup page)");
    config_server.send(200, "text/plain", "Panel reset and set up again. Is the picture back in place?");
  }
  else if (what == "boot")
  {
    panel_boot_request = true; // loop(): panel_boot_sequence()
    config_server.send(200, "text/plain", "Panel restarted as at boot (signals off, reset, setup, signals on). Is the picture back in place?");
  }
  else if (what == "pause")
  {
    panel_pause_request = true; // loop(): stops the pixel clock 1.5 s
    config_server.send(200, "text/plain", "Panel signal stopped for 1.5 s and given back. Is the picture back in place?");
  }
  else if (what == "reset")
  {
    panel_reset_request = true; // loop(): between draws
    config_server.send(200, "text/plain", "Display controller reset. Is the picture back in place?");
  }
  else if (what == "info")
  {
    char txt[320];
    rgb_panel_diag(rgbpanel, txt, sizeof(txt));
    Serial.printf("[Panel] %s\n", txt);
    config_server.send(200, "text/plain", txt);
  }
  else config_server.send(400, "text/plain", "do=stream, do=init, do=reset or do=info");
}

void handle_firmware_download()
{
  if (!web_auth()) return;
  const esp_partition_t *part = esp_ota_get_running_partition();
  esp_image_metadata_t meta = {};
  esp_partition_pos_t pos = {};
  if (part) pos = {part->address, part->size};
  // PSRAM first: internal RAM is tight (see the TLS notes).
  uint8_t *buf = (uint8_t *)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t *)malloc(4096);
  if (!part || !buf || esp_image_get_metadata(&pos, &meta) != ESP_OK || meta.image_len == 0 || meta.image_len > part->size)
  {
    free(buf);
    config_server.send(500, "text/plain", "Can't read the running firmware");
    return;
  }
  config_server.sendHeader("Content-Disposition", "attachment; filename=\"ESP32-4848S040-v" FW_VERSION ".ino.bin\"");
  config_server.setContentLength(meta.image_len);
  config_server.send(200, "application/octet-stream", "");
  WiFiClient client = config_server.client();
  for (uint32_t off = 0; off < meta.image_len && client.connected();)
  {
    uint32_t n = min((uint32_t)4096, meta.image_len - off);
    if (esp_partition_read(part, off, buf, n) != ESP_OK) break;
    if (client.write(buf, n) != n) break;
    off += n;
    esp_task_wdt_reset();
  }
  free(buf);
}

// The setup page runs on both networks: the setup AP (open to whoever has
// its Wi-Fi password, plus the captive-portal tricks below) and, once the
// board is on your Wi-Fi, http://dashboard.local or its IP - there behind
// a login (user "admin", the setup password shown in Settings). Routes are
// registered once, the first time either network comes up.
static bool web_started = false;
static bool ap_enabled = false;

void start_config_server()
{
  if (web_started) return;
  web_started = true;
  config_server.on("/", handle_root);
  config_server.on("/backup", HTTP_GET, handle_backup);
  config_server.on("/restore", HTTP_POST, handle_restore_done, handle_restore_upload);
  config_server.on("/update", HTTP_POST, handle_update_done, handle_update_upload);
  config_server.on("/ghinstall", HTTP_POST, handle_gh_install);
  config_server.on("/firmware.bin", HTTP_GET, handle_firmware_download);
  config_server.on("/panel", HTTP_GET, handle_panel);
  config_server.on("/reboot", HTTP_POST, handle_reboot);
  config_server.on("/scan", handle_scan);
  config_server.on("/save", HTTP_POST, handle_save_post);
  config_server.on("/notify", HTTP_POST, handle_notify);
  config_server.on("/screenshot", HTTP_GET, handle_screenshot);
  // Catch-all: iOS/Android/Windows each probe a different well-known URL
  // (e.g. captive.apple.com/hotspot-detect.html, connectivitycheck.gstatic.com)
  // right after joining a Wi-Fi network. Serving our page for *any* unmatched
  // path - combined with the DNS server redirecting every hostname to us
  // (set_ap_enabled()) - makes those probes fail "as expected" for a captive
  // portal, which is exactly what makes the OS auto-open a browser to this page.
  config_server.onNotFound(handle_root);
  const char *keys[] = {"Authorization"};
  config_server.collectHeaders(keys, 1);
  config_server.begin();
}

// ==================== Messages from Home Assistant ====================
// POST /notify {"message": "Washing machine done", "color": "green",
// "seconds": 60} shows it on the alert banner (alerts.ino). Same login as
// the setup page - see the README for the rest_command to put in HA.
static char notify_text[96];
static uint32_t notify_color = 0;
static uint16_t notify_seconds = 60;
static volatile bool notify_pending = false;

void handle_notify()
{
  if (!web_auth()) return;
  JsonDocument doc;
  if (deserializeJson(doc, config_server.arg("plain")) || !doc["message"].is<const char *>())
  {
    config_server.send(400, "application/json", "{\"error\":\"send JSON with a \\\"message\\\"\"}");
    return;
  }
  static const struct { const char *name; uint32_t rgb; } COLORS[] = {
      {"red", 0xC62828}, {"green", 0x2E7D32}, {"orange", 0xE65100}, {"blue", 0x1565C0}, {"purple", 0x6A1B9A}};
  const char *c = doc["color"] | "blue";
  uint32_t rgb = 0x1565C0;
  for (auto &k : COLORS)
    if (!strcasecmp(c, k.name)) rgb = k.rgb;
  data_lock();
  strlcpy(notify_text, doc["message"] | "", sizeof(notify_text));
  notify_color = rgb;
  notify_seconds = constrain(doc["seconds"] | 60, 5, 3600);
  data_unlock();
  notify_pending = true;
  config_server.send(200, "application/json", "{\"ok\":true}");
}

// loop() side.
void notify_apply_to_ui()
{
  char text[sizeof(notify_text)];
  data_lock();
  strlcpy(text, notify_text, sizeof(text));
  uint32_t rgb = notify_color;
  uint16_t secs = notify_seconds;
  data_unlock();
  alert_show(text, lv_color_hex(rgb), secs * 1000UL);
}

// ==================== Screenshots ====================
// GET /screenshot[?page=...] streams the frame buffer as a 480x480 16-bit
// BMP. The page switch runs on the LVGL thread (shot_apply_page() from
// loop()); this handler waits for it and a few frames before capturing.
static const char *const SHOT_PAGES[] = {"home", "bambu", "calendar", "proxmox", "stocks", "makerworld", "packages",
                                         "flight", "homeassistant", "phone", "jellyfin", "forecast", "agenda", "history", "settings",
                                         "quick", "night", "calday"};
#define SHOT_PAGE_COUNT (sizeof(SHOT_PAGES) / sizeof(SHOT_PAGES[0]))
static volatile int shot_page_req = -1; // index into SHOT_PAGES, -1 none
static volatile uint32_t shot_ready_ms = 0;

// loop() side: puts that page on screen, out of any other overlay first.
void shot_apply_page(int p)
{
  last_activity_ms = millis();
  dim_forced = false;
  quick_panel_hide();
  nav_menu_hide();
  if (in_settings) close_settings_screen();
  if (in_subscreen)
  {
    in_subscreen = false;
    if (page_dots_row) lv_obj_clear_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN);
  }
  const char *name = SHOT_PAGES[p];
  if (p < SCREEN_COUNT) switch_screen(p, LV_SCR_LOAD_ANIM_NONE);
  else if (!strcmp(name, "forecast")) { switch_screen(0, LV_SCR_LOAD_ANIM_NONE); open_forecast_screen(); }
  else if (!strcmp(name, "agenda")) { switch_screen(0, LV_SCR_LOAD_ANIM_NONE); open_agenda_screen(); }
  else if (!strcmp(name, "history")) { switch_screen(1, LV_SCR_LOAD_ANIM_NONE); open_print_history(NULL); }
  else if (!strcmp(name, "calday")) { switch_screen(2, LV_SCR_LOAD_ANIM_NONE); open_calendar_day_screen(clock_mday); }
  else if (!strcmp(name, "settings")) open_settings_cb(NULL);
  else if (!strcmp(name, "quick")) quick_panel_open();
  else if (!strcmp(name, "night")) { dim_forced = true; dim_forced_at = millis(); }
  backlight_update();
}

void handle_screenshot()
{
  if (!settings.shot_open && !web_auth()) return;
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb)
  {
    config_server.send(500, "text/plain", "no frame buffer");
    return;
  }
  if (config_server.hasArg("page"))
  {
    String want = config_server.arg("page");
    int p = -1;
    for (size_t i = 0; i < SHOT_PAGE_COUNT; i++)
      if (want.equalsIgnoreCase(SHOT_PAGES[i])) p = i;
    if (p < 0)
    {
      String list;
      for (size_t i = 0; i < SHOT_PAGE_COUNT; i++) list += String(i ? ", " : "") + SHOT_PAGES[i];
      config_server.send(400, "text/plain", "unknown page - one of: " + list);
      return;
    }
    shot_ready_ms = 0;
    shot_page_req = p;
    // Wait for loop() to switch, then let it draw (and the night clock's 1 s tick).
    uint32_t t0 = millis();
    while ((shot_page_req >= 0 || shot_ready_ms == 0) && millis() - t0 < 3000) vTaskDelay(pdMS_TO_TICKS(20));
    while ((int32_t)(shot_ready_ms - millis()) > 0) vTaskDelay(pdMS_TO_TICKS(20));
  }

  // BMP: 66-byte header (BITMAPINFOHEADER + RGB565 masks), rows bottom-up.
  const uint32_t W = screenWidth, H = screenHeight, row = W * 2, img = row * H;
  uint8_t hdr[66] = {'B', 'M'};
  auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; i++) hdr[at + i] = (v >> (8 * i)) & 0xFF; };
  put32(2, 66 + img);  // file size
  put32(10, 66);       // pixel data offset
  put32(14, 40);       // info header size
  put32(18, W);
  put32(22, H);
  hdr[26] = 1;         // planes
  hdr[28] = 16;        // bits per pixel
  put32(30, 3);        // BI_BITFIELDS
  put32(34, img);
  put32(54, 0xF800);   // red mask
  put32(58, 0x07E0);   // green
  put32(62, 0x001F);   // blue
  config_server.setContentLength(66 + img);
  config_server.send(200, "image/bmp", "");
  config_server.sendContent((const char *)hdr, sizeof(hdr));
  uint16_t *buf = (uint16_t *)heap_caps_malloc(480 * 8 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); // only while sending
  if (!buf) return;
  for (int32_t y = H - 1; y >= 0;)
  {
    // 8 rows at a time, copied out of PSRAM in bottom-up order.
    int n = 0;
    for (; n < 8 && y >= 0; n++, y--) memcpy(&buf[n * W], fb + (uint32_t)y * W, row);
    config_server.sendContent((const char *)buf, n * row);
    esp_task_wdt_reset();
  }
  heap_caps_free(buf);
}

// True when the request may go ahead: always from the setup AP, otherwise
// with the login. Asks the browser for it when missing.
bool web_auth()
{
  if (config_server.client().localIP() == WiFi.softAPIP() && ap_enabled) return true;
  if (config_server.authenticate("admin", ap_password)) return true;
  config_server.requestAuthentication(BASIC_AUTH, "Dashboard setup");
  return false;
}

// Same check for upload callbacks (no response possible from there).
bool web_auth_quiet()
{
  return (config_server.client().localIP() == WiFi.softAPIP() && ap_enabled) ||
         config_server.authenticate("admin", ap_password);
}

// ==================== Settings (long-press top-right corner) ====================
static uint8_t screen_before_settings = 0;
static lv_obj_t *settings_screen;
static lv_obj_t *cal_day_screen; // Calendar day-detail screen - see calendar.ino
static lv_obj_t *ap_switch;
static lv_obj_t *auto_bambu_switch;
static lv_obj_t *flight_alert_switch;
static lv_obj_t *wifi_autohide_switch;
static lv_obj_t *settings_info_lab;
static lv_obj_t *update_btn;
static lv_obj_t *update_btn_lab;
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
    lv_label_set_text_fmt(ap_status_lab, "On - %s / %s (%s)", AP_SSID, ap_password, AP_IP_STR);
  }
  else if (wifi_signal_ui_connected)
  {
    lv_label_set_text_fmt(ap_status_lab, "Off - or open http://%s  (admin / %s)", WiFi.localIP().toString().c_str(), ap_password);
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
void ap_password_init()
{
  prefs.begin("dash", false);
  prefs.getString("appass", ap_password, sizeof(ap_password));
  if (strlen(ap_password) != 8)
  {
    static const char ALPHABET[] = "abcdefghjkmnpqrstuvwxyz23456789"; // no 0/o, 1/l/i look-alikes
    // esp_random() is only truly random with the radio on; otherwise borrow
    // the bootloader's entropy source (it must not run while Wi-Fi is on).
    bool radio_off = WiFi.getMode() == WIFI_OFF;
    if (radio_off) bootloader_random_enable();
    for (int i = 0; i < 8; i++) ap_password[i] = ALPHABET[esp_random() % (sizeof(ALPHABET) - 1)];
    if (radio_off) bootloader_random_disable();
    ap_password[8] = '\0';
    prefs.putString("appass", ap_password);
  }
  prefs.end();
}

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
    bool ok = WiFi.softAP(AP_SSID, ap_password);
    start_config_server();
    // Redirect all DNS queries from AP clients to our own IP, regardless of
    // hostname - the other half of the captive-portal trick (start_config_server()).
    dns_server.start(53, "*", WiFi.softAPIP());
    Serial.printf("AP %s - SSID: %s  IP: %s  free heap: %u\n",
                   ok ? "started" : "FAILED TO START",
                   AP_SSID, WiFi.softAPIP().toString().c_str(), ESP.getFreeHeap());
  }
  else
  {
    dns_server.stop(); // the setup page itself stays up on your Wi-Fi
    WiFi.softAPdisconnect(true);
    Serial.println("AP stopped");
  }
}

void ap_switch_toggled_cb(lv_event_t *e)
{
  set_ap_enabled(lv_obj_has_state(ap_switch, LV_STATE_CHECKED));
  update_ap_status_label(); // safe here - this callback runs on the LVGL thread
}

void flight_alert_switch_toggled_cb(lv_event_t *e)
{
  settings.flight_alert = lv_obj_has_state(flight_alert_switch, LV_STATE_CHECKED);
  save_settings(); // on-device control, applies live immediately
}

void wifi_autohide_switch_toggled_cb(lv_event_t *e)
{
  settings.wifi_icon_autohide = lv_obj_has_state(wifi_autohide_switch, LV_STATE_CHECKED);
  save_settings(); // on-device control, applies live immediately
  wifi_icon_until_ms = millis() + WIFI_ICON_SHOW_MS;
  wifi_indicator_update_visibility();
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

// Pulls current time over NTP into the system clock. Runs on network_task -
// only sets flags; the Home screen labels are updated from loop() (the
// LVGL-owning thread) when clock_needs_ui_refresh is set, via
// clock_apply_to_ui(). Returns whether it
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

// WEEKDAY_NAMES / MONTH_NAMES: see lang.h (they follow the language setting).

// Copies the system's local time into clock_* - true when it's set.
bool clock_read_local()
{
  if (!clock_synced_once) return false;
  struct tm t;
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  if (t.tm_year < 120) return false;
  clock_hour = t.tm_hour;
  clock_min = t.tm_min;
  clock_sec = t.tm_sec;
  clock_wday = t.tm_wday;
  clock_mday = t.tm_mday;
  clock_mon = t.tm_mon;
  clock_year = t.tm_year + 1900;
  return true;
}

void clock_apply_to_ui()
{
  clock_read_local();
  if (home_hour_label) lv_label_set_text_fmt(home_hour_label, "%02d", clock_hour);
  if (home_min_label) lv_label_set_text_fmt(home_min_label, "%02d", clock_min);
  if (home_date_label)
  {
    char d[48];
    format_long_date(d, sizeof(d), clock_wday, clock_mon, clock_mday);
    lv_label_set_text(home_date_label, d);
  }
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
// "v3.6" over "192.168.1.23 - -58 dBm" (or "Wi-Fi not connected").
void settings_info_update()
{
  if (!settings_info_lab) return;
  char fw[48];
  switch (gh_state)
  {
    case GH_CHECKING: snprintf(fw, sizeof(fw), "v" FW_VERSION " - checking..."); break;
    case GH_UP_TO_DATE: snprintf(fw, sizeof(fw), "v" FW_VERSION " - up to date"); break;
    case GH_AVAILABLE: snprintf(fw, sizeof(fw), "v" FW_VERSION " - v%s available", gh_latest); break;
    case GH_INSTALLING: snprintf(fw, sizeof(fw), "Installing v%s...", gh_latest); break;
    case GH_FAILED: snprintf(fw, sizeof(fw), "v" FW_VERSION " - %s", gh_error); break;
    default: snprintf(fw, sizeof(fw), "Firmware v" FW_VERSION); break;
  }
  // Uptime and free memory, to spot a slow leak.
  uint32_t up = millis() / 60000UL; // minutes (wraps after 49 days, fine for a glance)
  char upt[16];
  if (up >= 1440) snprintf(upt, sizeof(upt), "%lud %luh", (unsigned long)(up / 1440), (unsigned long)(up / 60 % 24));
  else snprintf(upt, sizeof(upt), "%luh %lum", (unsigned long)(up / 60), (unsigned long)(up % 60));
  unsigned ram_kb = ESP.getFreeHeap() / 1024;
  float psram_mb = ESP.getFreePsram() / 1048576.0f;
  // Third line only after unplanned restarts (crash, watchdog, power dip).
  char diag[64] = "";
  if (crash_count)
    snprintf(diag, sizeof(diag), "\n%u unplanned restart%s%s%s - tap to clear", crash_count, crash_count == 1 ? "" : "s",
             restart_reason[0] ? ", last: " : "", restart_reason);
  if (wifi_signal_ui_connected)
    lv_label_set_text_fmt(settings_info_lab, "%s  \xE2\x80\xA2  up %s\n%s  \xE2\x80\xA2  %d dBm  \xE2\x80\xA2  free %uK + %.1fM%s",
                          fw, upt, WiFi.localIP().toString().c_str(), (int)wifi_signal_rssi_dbm, ram_kb, psram_mb, diag);
  else
    lv_label_set_text_fmt(settings_info_lab, "%s  \xE2\x80\xA2  up %s\nWi-Fi not connected  \xE2\x80\xA2  free %uK + %.1fM%s",
                          fw, upt, ram_kb, psram_mb, diag);

  if (update_btn_lab)
  {
    if (gh_state == GH_AVAILABLE) lv_label_set_text_fmt(update_btn_lab, "Install v%s", gh_latest);
    else lv_label_set_text(update_btn_lab, "Check update");
    lv_obj_set_style_bg_color(update_btn, gh_state == GH_AVAILABLE ? COLOR_SUCCESS : COLOR_BG_LIGHT, LV_PART_MAIN);
  }
}

// Check first; once a newer release is known the same button installs it.
void update_btn_clicked_cb(lv_event_t *e)
{
  if (gh_state == GH_CHECKING || gh_state == GH_INSTALLING) return;
  if (gh_state == GH_AVAILABLE) gh_install_request = true;
  else gh_check_request = true;
}

void open_settings_cb(lv_event_t *e)
{
  if (in_settings) return;
  in_settings = true;
  screen_before_settings = current_screen;

  if (ap_enabled) lv_obj_add_state(ap_switch, LV_STATE_CHECKED);
  else lv_obj_clear_state(ap_switch, LV_STATE_CHECKED);
  update_ap_status_label();
  settings_info_update();

  if (page_dots_row) lv_obj_add_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN);
  nav_menu_hide();
  nav_shortcuts_apply_mode();
  wifi_indicator_update_visibility();
  lv_scr_load_anim(settings_screen, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
}

// ==================== Shared helpers ====================

// Small rounded stat card with a big value on top and a muted caption below.
// Used for weather stats and printer temps/spool so the whole UI reads as one system.
lv_obj_t *make_stat_chip(lv_obj_t *parent, const char *value, const char *label, lv_color_t value_color, lv_obj_t **value_label_out = NULL, lv_coord_t chip_width = 132, const lv_font_t *value_font = &lv_font_ui_22, lv_flex_align_t main_place = LV_FLEX_ALIGN_CENTER, lv_coord_t chip_height = 74)
{
  lv_obj_t *chip = lv_obj_create(parent);
  lv_obj_set_size(chip, chip_width, chip_height);
  lv_obj_set_style_bg_color(chip, COLOR_BG, LV_PART_MAIN);
  lv_obj_set_style_border_width(chip, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(chip, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);
  lv_obj_clear_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_COLUMN);
  // main_place: CENTER (the default) or END to bottom-align the content.
  lv_obj_set_flex_align(chip, main_place, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

  lv_obj_t *val = lv_label_create(chip);
  lv_label_set_text(val, value);
  lv_obj_set_style_text_color(val, value_color, LV_PART_MAIN);
  lv_obj_set_style_text_font(val, value_font, LV_PART_MAIN);
  if (value_label_out) *value_label_out = val;

  lv_obj_t *lab = lv_label_create(chip);
  lv_label_set_text(lab, label);
  lv_obj_set_style_text_color(lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(lab, &lv_font_ui_12, LV_PART_MAIN);
  lv_obj_set_style_text_letter_space(lab, 1, LV_PART_MAIN);

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
  lv_obj_set_style_text_font(lab, &lv_font_ui_14, LV_PART_MAIN);
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
  lv_obj_set_pos(icon, 8, 8); // leaves room for the condition text under it
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
  // Clock font: lv_font_roboto_bold_126 (digits and ':' only, 72 px per
  // digit), so each slot is 160 px wide.
  lv_obj_t *time_row = lv_obj_create(clock_panel);
  lv_obj_set_size(time_row, 460, 100);
  lv_obj_set_style_bg_opa(time_row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(time_row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(time_row, 0, LV_PART_MAIN);
  lv_obj_clear_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(time_row, 0, LV_PART_MAIN);
  // 126 px (the night clock keeps 136) and a bit lower: clear of the
  // voicemail envelope and Wi-Fi icon in the top-right corner.
  lv_obj_align(time_row, LV_ALIGN_CENTER, 0, -2);
  home_time_row = time_row; // re-placed by nav_shortcuts_apply_mode(): higher when the Home icon row is off

  lv_obj_t *hour_val = lv_label_create(time_row);
  lv_obj_set_width(hour_val, 160);
  lv_obj_set_style_text_align(hour_val, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(hour_val, "14");
  lv_obj_set_style_text_color(hour_val, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(hour_val, &lv_font_roboto_bold_126, LV_PART_MAIN);

  lv_obj_t *colon_val = lv_label_create(time_row);
  lv_label_set_text(colon_val, ":");
  lv_obj_set_style_text_color(colon_val, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(colon_val, &lv_font_roboto_bold_126, LV_PART_MAIN);

  lv_obj_t *min_val = lv_label_create(time_row);
  lv_obj_set_width(min_val, 160);
  lv_obj_set_style_text_align(min_val, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(min_val, "32");
  lv_obj_set_style_text_color(min_val, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(min_val, &lv_font_roboto_bold_126, LV_PART_MAIN);

  home_hour_label = hour_val;
  home_min_label = min_val;

  lv_obj_t *divider = lv_obj_create(clock_panel);
  lv_obj_set_size(divider, 50, 3);
  lv_obj_set_style_bg_color(divider, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_set_style_border_width(divider, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(divider, 2, LV_PART_MAIN);
  lv_obj_align(divider, LV_ALIGN_CENTER, 0, 60);
  lv_obj_clear_flag(divider, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *date_val = lv_label_create(clock_panel);
  lv_label_set_text(date_val, "Saturday, August 15"); // placeholder until clock_apply_to_ui() runs post-NTP-sync
  lv_obj_set_style_text_color(date_val, lv_color_hex(0xB0BEC5), LV_PART_MAIN);
  lv_obj_set_style_text_font(date_val, &lv_font_ui_18, LV_PART_MAIN);
  lv_obj_align(date_val, LV_ALIGN_CENTER, 0, 78);
  home_date_label = date_val;

  // Shortcut row ("always on Home" mode): one icon per screen, centered
  // between the top-left menu corner (x < 80) and the top-right corner (x > 390).
  // Up to 8 icons are 36 px, 4 px apart (x 82-398); with Home Assistant,
  // Phone and Jellyfin all set up there can be 10, so they get smaller
  // (28 px, 2 px apart: 298 px) to stay clear of the corners.
  int shortcut_n = 0;
  for (size_t i = 1; i < SHORTCUT_COUNT; i++)
    if (SHORTCUTS[i].screen == FLIGHT_SCREEN_INDEX || screen_available(SHORTCUTS[i].screen)) shortcut_n++;
  lv_coord_t shortcut_size = shortcut_n > 8 ? 28 : 36;
  lv_obj_t *shortcuts = lv_obj_create(scr);
  lv_obj_set_size(shortcuts, LV_SIZE_CONTENT, shortcut_size);
  lv_obj_align(shortcuts, LV_ALIGN_TOP_MID, 0, 11 + (36 - shortcut_size) / 2);
  lv_obj_set_style_bg_opa(shortcuts, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(shortcuts, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(shortcuts, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(shortcuts, shortcut_n > 8 ? 2 : 4, LV_PART_MAIN);
  lv_obj_set_flex_flow(shortcuts, LV_FLEX_FLOW_ROW);
  lv_obj_clear_flag(shortcuts, LV_OBJ_FLAG_SCROLLABLE);
  home_shortcuts_row = shortcuts;

  for (size_t i = 1; i < SHORTCUT_COUNT; i++) // no Home icon on Home itself
  {
    bool flight = SHORTCUTS[i].screen == FLIGHT_SCREEN_INDEX;
    lv_obj_t *btn = make_shortcut_btn(shortcuts, SHORTCUTS[i].icon, flight ? COLOR_SECONDARY : COLOR_PRIMARY,
                                      home_shortcut_clicked_cb, SHORTCUTS[i].screen, shortcut_size);
    if (SHORTCUTS[i].screen >= HA_SCREEN_INDEX && !screen_available(SHORTCUTS[i].screen)) lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
    if (flight)
    {
      home_flight_btn = btn;
      lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN); // shown by flight_refresh_screen() while a flight is coming up
    }
  }

  // ---- Weather panel: bottom 60% of the screen ----
  lv_obj_t *weather_panel = lv_obj_create(scr);
  lv_obj_set_size(weather_panel, 480, 300);
  lv_obj_set_pos(weather_panel, 0, 180);
  // A tap on the weather opens the Forecast page (forecast.ino).
  lv_obj_add_flag(weather_panel, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(weather_panel, home_weather_clicked_cb, LV_EVENT_SHORT_CLICKED, NULL);
  lv_obj_set_style_bg_opa(weather_panel, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(weather_panel, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(weather_panel, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(weather_panel, 0, LV_PART_MAIN);
  lv_obj_clear_flag(weather_panel, LV_OBJ_FLAG_SCROLLABLE);

  build_weather_icon(weather_panel);

  lv_obj_t *temp = lv_label_create(weather_panel);
  lv_label_set_text(temp, "24°C");
  lv_obj_set_style_text_color(temp, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(temp, &lv_font_roboto_medium_90, LV_PART_MAIN);
  lv_obj_align(temp, LV_ALIGN_TOP_RIGHT, -24, 24);
  weather_temp_lab = temp;

  // The right side is all about the current temperature: the big number,
  // then feels-like and today's high/low right under it.
  lv_obj_t *extra = lv_label_create(weather_panel);
  lv_label_set_text(extra, "");
  lv_obj_set_style_text_color(extra, lv_color_hex(0xB0BEC5), LV_PART_MAIN);
  lv_obj_set_style_text_font(extra, &lv_font_ui_16, LV_PART_MAIN);
  lv_obj_align(extra, LV_ALIGN_TOP_RIGHT, -24, 104); // clear gap under the big temperature
  weather_extra_lab = extra;

  // The condition ("Overcast", "Rain", ...) belongs to the icon: centered under it.
  lv_obj_t *desc = lv_label_create(weather_panel);
  lv_label_set_text(desc, "Partly Cloudy");
  lv_obj_set_style_text_color(desc, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(desc, &lv_font_ui_16, LV_PART_MAIN);
  lv_obj_set_width(desc, 210);
  lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_pos(desc, 8, 141);
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
  home_weather_stats = stats;

  // Slightly smaller than make_stat_chip()'s default 132x74.
  make_stat_chip(stats, "58%", tr(S_HUMIDITY), COLOR_PRIMARY, &weather_humidity_val, 110, &lv_font_ui_22, LV_FLEX_ALIGN_CENTER, 62);
  make_stat_chip(stats, "12 km/h", tr(S_WIND), COLOR_PRIMARY, &weather_wind_val, 110, &lv_font_ui_22, LV_FLEX_ALIGN_CENTER, 62);
  make_stat_chip(stats, "4", tr(S_UV), COLOR_PRIMARY, &weather_uv_val, 110, &lv_font_ui_22, LV_FLEX_ALIGN_CENTER, 62);

  // Same spot as the stats row: an incoming call (asterisk.ino) and what's
  // playing on Jellyfin (jellyfin.ino) take turns with it - see weather_slot.ino.
  build_home_slot_cards(weather_panel);

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
  lv_obj_set_style_text_font(next_event_lab, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_set_width(next_event_lab, 396);
  lv_obj_set_style_text_align(next_event_lab, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
  lv_label_set_long_mode(next_event_lab, LV_LABEL_LONG_DOT);
  lv_obj_align(next_event_lab, LV_ALIGN_LEFT_MID, 26, 0);
  home_next_event_lab = next_event_lab;

  // Taps: the "Next" card opens the Agenda (calendar.ino), everything else
  // in the weather panel - except the badges - opens the Forecast.
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(weather_panel); i++)
  {
    lv_obj_t *child = lv_obj_get_child(weather_panel, i);
    if (child != next_card) make_click_through(child);
  }
  make_click_through(next_accent);
  home_slot_cards_clickable(); // weather_slot.ino: media / voicemail / call cards open their screens
  lv_obj_add_flag(next_card, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_event_cb(next_card, home_next_clicked_cb, LV_EVENT_SHORT_CLICKED, NULL);

  return scr;
}

// Lets taps on obj (and everything in it) fall through to its parent.
void make_click_through(lv_obj_t *obj)
{
  lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj); i++) make_click_through(lv_obj_get_child(obj, i));
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
  lv_obj_set_style_text_font(title, &lv_font_ui_28, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  // Firmware version, IP and signal - filled by settings_info_update().
  settings_info_lab = lv_label_create(scr);
  lv_label_set_text(settings_info_lab, "v" FW_VERSION);
  lv_obj_set_style_text_color(settings_info_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(settings_info_lab, &lv_font_ui_12, LV_PART_MAIN);
  lv_obj_set_style_text_align(settings_info_lab, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_obj_align(settings_info_lab, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_add_flag(settings_info_lab, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(settings_info_lab, 10);
  lv_obj_add_event_cb(settings_info_lab, restart_counter_reset_cb, LV_EVENT_CLICKED, NULL);

  // ---- Setup Access Point ----
  lv_obj_t *ap_row = lv_obj_create(scr);
  lv_obj_set_size(ap_row, 432, 74);
  lv_obj_align(ap_row, LV_ALIGN_TOP_LEFT, 0, 50); // room for the 3-line info top right
  lv_obj_set_style_bg_color(ap_row, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(ap_row, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(ap_row, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(ap_row, 16, LV_PART_MAIN);
  lv_obj_clear_flag(ap_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *ap_title = lv_label_create(ap_row);
  lv_label_set_text(ap_title, "Setup Access Point");
  lv_obj_set_style_text_color(ap_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(ap_title, &lv_font_ui_16, LV_PART_MAIN);
  lv_obj_align(ap_title, LV_ALIGN_TOP_LEFT, 0, 0);

  ap_switch = lv_switch_create(ap_row);
  lv_obj_set_style_bg_color(ap_switch, COLOR_PRIMARY, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_align(ap_switch, LV_ALIGN_TOP_RIGHT, 0, -4);
  lv_obj_add_event_cb(ap_switch, ap_switch_toggled_cb, LV_EVENT_VALUE_CHANGED, NULL);

  ap_status_lab = lv_label_create(ap_row);
  lv_label_set_text(ap_status_lab, "Off");
  lv_obj_set_style_text_color(ap_status_lab, COLOR_TEXT_MUTED, LV_PART_MAIN);
  lv_obj_set_style_text_font(ap_status_lab, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_align(ap_status_lab, LV_ALIGN_BOTTOM_LEFT, 0, 0);

  // ---- Auto-open Bambu screen when a print starts ----
  lv_obj_t *abos_row = lv_obj_create(scr);
  lv_obj_set_size(abos_row, 432, 54);
  lv_obj_align(abos_row, LV_ALIGN_TOP_LEFT, 0, 130);
  lv_obj_set_style_bg_color(abos_row, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(abos_row, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(abos_row, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(abos_row, 16, LV_PART_MAIN);
  lv_obj_clear_flag(abos_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *abos_title = lv_label_create(abos_row);
  lv_label_set_text(abos_title, "Show Bambu screen at print start/end");
  lv_obj_set_style_text_color(abos_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(abos_title, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_align(abos_title, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_width(abos_title, 340);

  auto_bambu_switch = lv_switch_create(abos_row);
  lv_obj_set_style_bg_color(auto_bambu_switch, COLOR_PRIMARY, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_align(auto_bambu_switch, LV_ALIGN_TOP_RIGHT, 0, -4);
  if (settings.auto_show_bambu_on_start) lv_obj_add_state(auto_bambu_switch, LV_STATE_CHECKED);
  lv_obj_add_event_cb(auto_bambu_switch, auto_bambu_switch_toggled_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // ---- Show the Flight screen for 2 min when the flight changes ----
  lv_obj_t *falert_row = lv_obj_create(scr);
  lv_obj_set_size(falert_row, 432, 54);
  lv_obj_align(falert_row, LV_ALIGN_TOP_LEFT, 0, 192);
  lv_obj_set_style_bg_color(falert_row, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(falert_row, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(falert_row, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(falert_row, 16, LV_PART_MAIN);
  lv_obj_clear_flag(falert_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *falert_title = lv_label_create(falert_row);
  lv_label_set_text(falert_title, "Show Flight screen on changes (2 min)");
  lv_obj_set_style_text_color(falert_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(falert_title, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_align(falert_title, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_width(falert_title, 340);

  flight_alert_switch = lv_switch_create(falert_row);
  lv_obj_set_style_bg_color(flight_alert_switch, COLOR_PRIMARY, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_align(flight_alert_switch, LV_ALIGN_TOP_RIGHT, 0, -4);
  if (settings.flight_alert) lv_obj_add_state(flight_alert_switch, LV_STATE_CHECKED);
  lv_obj_add_event_cb(flight_alert_switch, flight_alert_switch_toggled_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // ---- Wi-Fi icon: only on Home for 20 s, or always ----
  lv_obj_t *wifi_row = lv_obj_create(scr);
  lv_obj_set_size(wifi_row, 432, 54);
  lv_obj_align(wifi_row, LV_ALIGN_TOP_LEFT, 0, 254);
  lv_obj_set_style_bg_color(wifi_row, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(wifi_row, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(wifi_row, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(wifi_row, 16, LV_PART_MAIN);
  lv_obj_clear_flag(wifi_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *wifi_title = lv_label_create(wifi_row);
  lv_label_set_text(wifi_title, "Auto-hide Wi-Fi icon (Home, 20 s)");
  lv_obj_set_style_text_color(wifi_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(wifi_title, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_align(wifi_title, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_width(wifi_title, 340);

  wifi_autohide_switch = lv_switch_create(wifi_row);
  lv_obj_set_style_bg_color(wifi_autohide_switch, COLOR_PRIMARY, LV_PART_INDICATOR | LV_STATE_CHECKED);
  lv_obj_align(wifi_autohide_switch, LV_ALIGN_TOP_RIGHT, 0, -4);
  if (settings.wifi_icon_autohide) lv_obj_add_state(wifi_autohide_switch, LV_STATE_CHECKED);
  lv_obj_add_event_cb(wifi_autohide_switch, wifi_autohide_switch_toggled_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // ---- Idle return-to-Home timeout ----
  lv_obj_t *idle_row = lv_obj_create(scr);
  lv_obj_set_size(idle_row, 432, 54);
  lv_obj_align(idle_row, LV_ALIGN_TOP_LEFT, 0, 316);
  lv_obj_set_style_bg_color(idle_row, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_border_width(idle_row, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(idle_row, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(idle_row, 16, LV_PART_MAIN);
  lv_obj_clear_flag(idle_row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *idle_title = lv_label_create(idle_row);
  lv_label_set_text(idle_title, "Return to Home after");
  lv_obj_set_style_text_color(idle_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(idle_title, &lv_font_ui_14, LV_PART_MAIN);
  lv_obj_align(idle_title, LV_ALIGN_TOP_LEFT, 0, 0);

  idle_timeout_dd = lv_dropdown_create(idle_row);
  lv_dropdown_set_options(idle_timeout_dd, "1 min\n2 min\n5 min\n10 min");
  lv_obj_set_width(idle_timeout_dd, 120);
  lv_obj_align(idle_timeout_dd, LV_ALIGN_TOP_RIGHT, 0, -6);
  lv_dropdown_set_selected(idle_timeout_dd, idle_timeout_min_to_index(settings.idle_return_home_min));
  lv_obj_add_event_cb(idle_timeout_dd, idle_timeout_dd_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);

  // ---- Restart device (bottom row, left of Close) ----
  lv_obj_t *restart_btn = lv_btn_create(scr);
  lv_obj_set_size(restart_btn, 136, 48);
  lv_obj_align(restart_btn, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_color(restart_btn, lv_color_hex(0xEF5350), LV_PART_MAIN);
  lv_obj_set_style_radius(restart_btn, 12, LV_PART_MAIN);
  lv_obj_add_event_cb(restart_btn, restart_btn_clicked_cb, LV_EVENT_CLICKED, NULL);

  lv_obj_t *restart_lab = lv_label_create(restart_btn);
  lv_label_set_text(restart_lab, "Restart");
  lv_obj_center(restart_lab);

  // ---- Close: the AP toggle above already applies live, so this is the
  // only exit - it turns the AP off and connects to the configured Wi-Fi.
  // ---- Update from GitHub (middle of the bottom row) ----
  update_btn = lv_btn_create(scr);
  lv_obj_set_size(update_btn, 144, 48);
  lv_obj_align(update_btn, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(update_btn, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_radius(update_btn, 12, LV_PART_MAIN);
  lv_obj_add_event_cb(update_btn, update_btn_clicked_cb, LV_EVENT_CLICKED, NULL);
  update_btn_lab = lv_label_create(update_btn);
  lv_label_set_text(update_btn_lab, "Check update");
  lv_obj_center(update_btn_lab);

  lv_obj_t *close_btn = lv_btn_create(scr);
  lv_obj_set_size(close_btn, 136, 48);
  lv_obj_align(close_btn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
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
// (5s, configured on the touch indev in setup()) opens the settings screen.
// A short tap does nothing - next page is only the bottom-right corner.
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
}

// Invisible bottom-left hotspot on the always-on-top layer: a tap goes to
// the previous screen (mirrors create_next_hotspot()). Also builds the pin
// icon shown top-left while the screen is pinned - the pin itself is
// toggled by holding the top-left corner 3 s (see pin_hold_event()).
void create_pin_hotspot()
{
  lv_obj_t *layer = lv_disp_get_layer_top(NULL);
  pin_hotspot = lv_obj_create(layer);
  lv_obj_set_size(pin_hotspot, 90, 90);
  lv_obj_align(pin_hotspot, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_style_bg_opa(pin_hotspot, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(pin_hotspot, 0, LV_PART_MAIN);
  lv_obj_clear_flag(pin_hotspot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(pin_hotspot, prev_hotspot_cb, LV_EVENT_SHORT_CLICKED, NULL);

  // Not clickable, so taps fall through to the top-left hotspot under it.
  pin_indicator = lv_img_create(layer);
  lv_img_set_src(pin_indicator, &home_icon_pin);
  lv_obj_set_style_img_recolor(pin_indicator, lv_color_hex(0xEF5350), LV_PART_MAIN);
  lv_obj_set_style_img_recolor_opa(pin_indicator, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(pin_indicator, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(pin_indicator, LV_ALIGN_TOP_LEFT, 10, 8);
  lv_obj_add_flag(pin_indicator, LV_OBJ_FLAG_HIDDEN);
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
  wifi_signal_box = box;
  wifi_icon_until_ms = millis() + WIFI_ICON_SHOW_MS; // boot starts on Home
  wifi_indicator_update_visibility();
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
// start_finish_screen_flash() and bambu_warning_cycle_cb() moved to bambu.ino.

// Shows a detail page (not part of the swipe sequence). Its back button -
// and the top-left corner - call close_subscreen(), which returns to
// current_screen, the page it was opened from.
void open_subscreen(lv_obj_t *scr)
{
  in_subscreen = true;
  nav_menu_hide();
  if (page_dots_row) lv_obj_add_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN);
  lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, false);
  wifi_indicator_update_visibility();
}

void close_subscreen(lv_event_t *e)
{
  if (!in_subscreen) return;
  in_subscreen = false;
  if (page_dots_row) lv_obj_clear_flag(page_dots_row, LV_OBJ_FLAG_HIDDEN);
  switch_screen(current_screen, LV_SCR_LOAD_ANIM_NONE);
}

// Back button for a detail page, top-left (where the corner tap also goes back).
lv_obj_t *make_back_button(lv_obj_t *scr)
{
  lv_obj_t *back_btn = lv_btn_create(scr);
  lv_obj_set_size(back_btn, 60, 50);
  lv_obj_align(back_btn, LV_ALIGN_TOP_LEFT, 12, 12);
  lv_obj_set_style_bg_color(back_btn, COLOR_BG_LIGHT, LV_PART_MAIN);
  lv_obj_set_style_radius(back_btn, 12, LV_PART_MAIN);
  lv_obj_add_event_cb(back_btn, close_subscreen, LV_EVENT_CLICKED, NULL);
  lv_obj_t *back_lab = lv_label_create(back_btn);
  lv_label_set_text(back_lab, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(back_lab, COLOR_PRIMARY, LV_PART_MAIN);
  lv_obj_center(back_lab);
  return back_btn;
}

void switch_screen(uint8_t num, lv_scr_load_anim_t anim)
{
  if (num >= SCREEN_COUNT) return;
  // time=0, delay=0 is the only combination lv_scr_load_anim() treats as an
  // immediate, synchronous swap (see lv_disp.c) - anything else, even with
  // anim=NONE, schedules the swap via its async animation machinery and
  // keeps showing the old screen until that "animation" completes.
  lv_scr_load_anim(screens[num], anim, 0, 0, false);
  current_screen = num;
  if (num == 0) wifi_icon_until_ms = millis() + WIFI_ICON_SHOW_MS; // back on Home: show the Wi-Fi icon again for a while
  wifi_indicator_update_visibility();
  update_page_indicator(num);
  nav_shortcuts_apply_mode(); // the menu corner is free on Home in "always on Home" mode
}


// ==================== Network task (runs on core 0, separate from LVGL) ====================
// Everything here can block for a while (HTTPS GETs, TLS handshakes, DNS/HTTP
// servers) without affecting rendering or touch responsiveness, because it's
// pinned to the opposite core from loop()/lv_timer_handler(). The one rule
// this task must never break: no LVGL calls from here, ever - LVGL is not
// thread-safe. Anything that needs to reach the screen sets a
// *_needs_ui_refresh flag and lets loop() apply it.
static uint32_t last_clock_sync_ms = 0;
#define CLOCK_SYNC_INTERVAL_MS (60UL * 60UL * 1000UL)
#define CLOCK_RETRY_INTERVAL_MS (15UL * 1000UL)

// ==================== Watchdog / restart reason ====================
// Both tasks feed the task watchdog; if either hangs for WDT_TIMEOUT_S the
// board restarts by itself. Restarts that weren't asked for (crash,
// watchdog, power dip) are counted and shown on the Settings screen.
#define WDT_TIMEOUT_S 90 // generous: some fetches block for several seconds


// mbedTLS allocates from internal RAM only in this core's build
// (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC): the always-on Bambu MQTT/TLS link
// plus each HTTPS fetch left too little for the next handshake ("Couldn't
// reach GitHub", HTTP -1) and probably starved lwIP too. But TLS buffers in
// PSRAM compete with the RGB panel for PSRAM bandwidth (the printer's
// encrypted report arrives every second), which knocked the picture out of
// alignment for good. So: internal RAM while there's room to spare, PSRAM
// only when internal RAM runs short.
#define TLS_INTERNAL_SPARE (24 * 1024) // keep at least this much contiguous internal RAM free

static void *tls_calloc(size_t n, size_t size)
{
  size_t total = n * size;
  void *p = NULL;
  if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > total + TLS_INTERNAL_SPARE)
    p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  return p;
}

void tls_memory_to_psram()
{
  mbedtls_platform_set_calloc_free(tls_calloc, heap_caps_free);
}

// Tap on the Settings info text: the unplanned restart count starts over.
void restart_counter_reset_cb(lv_event_t *e)
{
  if (!crash_count) return;
  crash_count = 0;
  Preferences p;
  p.begin("diag", false);
  p.putUShort("crashes", 0);
  p.end();
  settings_info_update();
}

void restart_reason_init()
{
  esp_reset_reason_t r = esp_reset_reason();
  switch (r)
  {
    case ESP_RST_PANIC: restart_reason = "crash"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: restart_reason = "watchdog"; break;
    case ESP_RST_BROWNOUT: restart_reason = "power dip"; break;
    default: restart_reason = ""; break; // power on, restart, update
  }
  Preferences p;
  p.begin("diag", false);
  crash_count = p.getUShort("crashes", 0);
  if (restart_reason[0]) p.putUShort("crashes", ++crash_count);
  p.end();
  Serial.printf("Reset reason %d%s%s, unplanned restarts so far: %u\n", (int)r, restart_reason[0] ? " - " : "",
                restart_reason, crash_count);
}

void watchdog_start()
{
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = WDT_TIMEOUT_S * 1000;
  cfg.idle_core_mask = 0;
  cfg.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
  esp_task_wdt_add(NULL); // the calling task: loop() (setup() runs in it)
}

// Reads the Wi-Fi state for the signal icon (applied from loop()).
static void wifi_signal_sample()
{
  last_wifi_signal_sample_ms = millis();
  wifi_signal_ui_connected = wifi_connected;
  wifi_signal_rssi_dbm = wifi_connected ? (int8_t)WiFi.RSSI() : 0;
  wifi_signal_needs_ui_refresh = true;
}

void network_task(void *param)
{
  esp_task_wdt_add(NULL);
  for (;;)
  {
    esp_task_wdt_reset();
    if (web_started) config_server.handleClient();
    if (ap_enabled) dns_server.processNextRequest();
    if (wifi_got_ip)
    {
      wifi_got_ip = false;
      // The icon first: the fetches below take a while, and until now it
      // only caught up after all of them.
      wifi_signal_sample();
      start_config_server();
      static bool mdns_started = false;
      if (!mdns_started && MDNS.begin("dashboard")) // http://dashboard.local
      {
        MDNS.addService("http", "tcp", 80);
        mdns_started = true;
      }
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
    if (millis() - last_wifi_signal_sample_ms > WIFI_SIGNAL_SAMPLE_INTERVAL_MS) wifi_signal_sample();
    bambu_mqtt_maintain();
    if (wifi_connected && clock_synced_once) gh_update_maintain();
    if (wifi_connected) ha_maintain();
    if (wifi_connected) ami_maintain();
    if (wifi_connected) jf_maintain();
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== Dashboard starting (firmware " FW_VERSION ") ===");
  restart_reason_init();
  tls_memory_to_psram();

  data_mutex = xSemaphoreCreateRecursiveMutex();

  load_settings();
  ap_password_init(); // also the setup page login - see web_auth()

  WiFi.onEvent(on_wifi_event);
  connect_saved_wifi();

  Serial.println("Initializing touch...");
  ts.begin();
  ts.setRotation(ROTATION_NORMAL);
  Serial.println("Touch init complete");

  Serial.println("Initializing display...");
  gfx->begin(16000000);
  Serial.println("Display begin() complete");
  Serial.printf("[Panel] diagnostics callbacks: %s\n", rgb_panel_diag_start(rgbpanel) ? "ok" : "FAILED");

  delay(500);
  gfx->fillScreen(BLACK);
  Serial.println("Screen filled black");

#ifdef GFX_BL
  // PWM so it can dim at night - see backlight_update().
  bool bl_ok = ledcAttach(GFX_BL, BACKLIGHT_PWM_HZ, 8);
  Serial.printf("[Backlight] PWM on GPIO %d: %s\n", GFX_BL, bl_ok ? "ok" : "FAILED");
  backlight_set(settings.day_level);
#endif

  lv_init();

  screenWidth = gfx->width();
  screenHeight = gfx->height();

  // LVGL draws into this buffer, then my_disp_flush() copies it into the
  // panel's frame buffer (PSRAM). A small buffer in internal RAM keeps all
  // the drawing work (blending reads + writes) off PSRAM, whose bandwidth the
  // panel's refresh shares with Wi-Fi/TLS - starving it shows as flashing
  // lines. 32 lines (~30 KB) is small enough to leave Wi-Fi its internal RAM
  // (a 190 KB internal buffer once caused AP start failures); falls back to
  // the old 200-line PSRAM buffer if the allocation fails.
  uint32_t draw_buf_px = screenWidth * 32;
  disp_draw_buf = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * draw_buf_px, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!disp_draw_buf)
  {
    draw_buf_px = screenWidth * 200;
    disp_draw_buf = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * draw_buf_px, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  Serial.printf("LVGL draw buffer: %u lines in %s\n", (unsigned)(draw_buf_px / screenWidth),
                draw_buf_px == screenWidth * 32 ? "internal RAM" : "PSRAM");

  if (!disp_draw_buf) {
    Serial.println("LVGL disp_draw_buf allocate failed!");
  }
  else
  {
    lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, draw_buf_px);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_t *disp = lv_disp_drv_register(&disp_drv);
    // Same default theme LVGL sets up itself, but with lv_font_ui_14 (the
    // built-in Montserrat plus accented letters) for text without its own font.
    lv_disp_set_theme(disp, lv_theme_default_init(disp, lv_palette_main(LV_PALETTE_BLUE), lv_palette_main(LV_PALETTE_RED),
                                                  LV_THEME_DEFAULT_DARK, &lv_font_ui_14));

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
    screens[HA_SCREEN_INDEX] = create_ha_screen();
    screens[PHONE_SCREEN_INDEX] = create_phone_screen();
    screens[JF_SCREEN_INDEX] = create_jf_screen();
    screens[FLIGHT_SCREEN_INDEX] = create_flight_screen();
    settings_screen = create_settings_screen();
    cal_day_screen = create_calendar_day_screen();
    create_forecast_screen();
    create_agenda_screen();
    create_print_history_screen();

    for (int i = 0; i < SCREEN_COUNT; i++) {
      lv_obj_add_event_cb(screens[i], screen_gesture_cb, LV_EVENT_GESTURE, NULL);
    }

    create_page_indicator();
    create_settings_hotspot();
    create_pin_hotspot();
    create_next_hotspot();
    create_nav_menu();
    create_wifi_signal_indicator();
    create_update_ui();
    create_quick_panel();
    create_screensaver();
    create_alert_banner(); // last: above everything else on the top layer
    create_blackout_overlay();
    create_finish_flash_overlay();
    last_activity_ms = millis();
    lv_timer_create(clock_tick_cb, 1000, NULL);
    rgb_resync_timer_start();
    lv_timer_create(bambu_warning_cycle_cb, 5000, NULL);

    switch_screen(0, LV_SCR_LOAD_ANIM_NONE);

    // Network I/O (Wi-Fi/AP servers, HTTPS fetches, MQTT) runs on core 0 so
    // it can never stall LVGL rendering/touch on this core - see
    // network_task() above for why and the thread-safety rules it follows.
    // 12 KB stack: the iCal/MakerWorld/PKGE parsers keep their buffers on
    // the heap, but the TLS client + JSON filters still need some headroom.
    watchdog_start(); // before the network task, which adds itself
    xTaskCreatePinnedToCore(network_task, "network", 12288, NULL, 1, NULL, 0);

    Serial.println("Setup done");
  }
}

// ==================== "Update available" icon + dialog ====================
// Green download icon on Home (right edge, under the Settings corner) while
// a newer GitHub release is known and not skipped; a tap asks Install / Skip.
static lv_obj_t *update_icon = NULL;
static lv_obj_t *update_dialog = NULL;     // backdrop holding the card
static lv_obj_t *update_dialog_text = NULL;

bool update_offer_pending()
{
  return gh_state == GH_AVAILABLE && settings.update_mode == UPDATE_NOTIFY &&
         strcmp(gh_latest, settings.update_skip) != 0;
}

void update_icon_refresh()
{
  if (!update_icon) return;
  bool show = update_offer_pending() && current_screen == 0 && !in_settings && !in_subscreen;
  if (show == lv_obj_has_flag(update_icon, LV_OBJ_FLAG_HIDDEN))
    (show ? lv_obj_clear_flag : lv_obj_add_flag)(update_icon, LV_OBJ_FLAG_HIDDEN);
  if (update_dialog && !update_offer_pending()) lv_obj_add_flag(update_dialog, LV_OBJ_FLAG_HIDDEN);
}

void update_icon_clicked_cb(lv_event_t *e)
{
  lv_label_set_text_fmt(update_dialog_text, tr(S_UPDATE_TEXT), gh_latest, FW_VERSION);
  lv_obj_clear_flag(update_dialog, LV_OBJ_FLAG_HIDDEN);
}

void update_dialog_cb(lv_event_t *e)
{
  int choice = (int)(intptr_t)lv_event_get_user_data(e); // 1 install, 2 skip, 0 just close
  lv_obj_add_flag(update_dialog, LV_OBJ_FLAG_HIDDEN);
  if (choice == 1) gh_install_request = true;
  if (choice == 2)
  {
    strlcpy(settings.update_skip, gh_latest, sizeof(settings.update_skip));
    save_settings();
  }
  update_icon_refresh();
}

static lv_obj_t *update_dialog_btn(lv_obj_t *card, const char *text, lv_color_t color, int choice, lv_align_t align)
{
  lv_obj_t *btn = lv_btn_create(card);
  lv_obj_set_size(btn, 150, 46);
  lv_obj_align(btn, align, 0, 0);
  lv_obj_set_style_bg_color(btn, color, LV_PART_MAIN);
  lv_obj_set_style_radius(btn, 12, LV_PART_MAIN);
  lv_obj_add_event_cb(btn, update_dialog_cb, LV_EVENT_CLICKED, (void *)(intptr_t)choice);
  lv_obj_t *lab = lv_label_create(btn);
  lv_label_set_text(lab, text);
  lv_obj_center(lab);
  return btn;
}

void create_update_ui()
{
  lv_obj_t *layer = lv_disp_get_layer_top(NULL);

  update_icon = lv_btn_create(layer);
  lv_obj_set_size(update_icon, 40, 40);
  lv_obj_align(update_icon, LV_ALIGN_TOP_RIGHT, -8, 98); // just below the 90x90 Settings corner
  lv_obj_set_style_radius(update_icon, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(update_icon, COLOR_SUCCESS, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(update_icon, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(update_icon, update_icon_clicked_cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t *sym = lv_label_create(update_icon);
  lv_label_set_text(sym, LV_SYMBOL_DOWNLOAD);
  lv_obj_set_style_text_color(sym, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(sym);
  lv_obj_add_flag(update_icon, LV_OBJ_FLAG_HIDDEN);

  // Opaque-only styling (no partial opacity on full-screen objects - see blackout_overlay).
  update_dialog = lv_obj_create(layer);
  lv_obj_set_size(update_dialog, 480, 480);
  lv_obj_set_pos(update_dialog, 0, 0);
  lv_obj_set_style_bg_opa(update_dialog, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(update_dialog, 0, LV_PART_MAIN);
  lv_obj_clear_flag(update_dialog, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(update_dialog, update_dialog_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);
  lv_obj_add_flag(update_dialog, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *card = lv_obj_create(update_dialog);
  lv_obj_set_size(card, 360, 190);
  lv_obj_center(card);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x10181F), LV_PART_MAIN);
  lv_obj_set_style_border_color(card, COLOR_SUCCESS, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
  lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(card);
  lv_label_set_text(title, tr(S_UPDATE_AVAIL));
  lv_obj_set_style_text_color(title, COLOR_SUCCESS, LV_PART_MAIN);
  lv_obj_set_style_text_font(title, &lv_font_ui_22, LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

  update_dialog_text = lv_label_create(card);
  lv_label_set_text(update_dialog_text, "");
  lv_obj_set_style_text_color(update_dialog_text, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_text_font(update_dialog_text, &lv_font_ui_16, LV_PART_MAIN);
  lv_obj_align(update_dialog_text, LV_ALIGN_TOP_LEFT, 0, 36);

  update_dialog_btn(card, tr(S_SKIP), lv_color_hex(0x34424F), 2, LV_ALIGN_BOTTOM_LEFT);
  update_dialog_btn(card, tr(S_INSTALL), COLOR_SUCCESS, 1, LV_ALIGN_BOTTOM_RIGHT);
}

// "Updating firmware NN%" card over everything while an upload runs.
void ota_apply_to_ui()
{
  static lv_obj_t *card = NULL;
  static lv_obj_t *lab = NULL;
  if (!card)
  {
    card = lv_obj_create(lv_disp_get_layer_top(NULL));
    lv_obj_set_size(card, 420, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x10181F), LV_PART_MAIN);
    lv_obj_set_style_border_color(card, COLOR_PRIMARY, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 16, LV_PART_MAIN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lab = lv_label_create(card);
    lv_obj_set_style_text_color(lab, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lab, &lv_font_ui_18, LV_PART_MAIN);
    lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_width(lab, 380);
    lv_label_set_long_mode(lab, LV_LABEL_LONG_WRAP);
    lv_obj_center(lab);
  }
  lv_label_set_text_fmt(lab, "%s\n%s", tr(S_UPDATING), tr(S_UPDATING_DARK));
  // While flash is being written the panel can't always read its picture
  // from PSRAM and smears shifted copies of it - so everything goes black
  // (nothing to smear) except this small card.
  if (blackout_overlay) (ota_in_progress ? lv_obj_clear_flag : lv_obj_add_flag)(blackout_overlay, LV_OBJ_FLAG_HIDDEN);
  (ota_in_progress ? lv_obj_clear_flag : lv_obj_add_flag)(card, LV_OBJ_FLAG_HIDDEN);
  if (ota_in_progress)
  {
    if (blackout_overlay) lv_obj_move_foreground(blackout_overlay);
    lv_obj_move_foreground(card);
  }
  else rgb_panel_resync(rgbpanel);
}

void loop()
{
  esp_task_wdt_reset();
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
  if (flight_change_pending) { flight_change_pending = false; flight_alert_start(flight_change_mask); flight_change_mask = 0; }
  if (proxmox_needs_ui_refresh) { proxmox_needs_ui_refresh = false; proxmox_apply_to_ui(); }
  if (wifi_signal_needs_ui_refresh) { wifi_signal_needs_ui_refresh = false; wifi_signal_apply_to_ui(); }
  if (ota_needs_ui_refresh) { ota_needs_ui_refresh = false; ota_apply_to_ui(); }
  if (ha_take_ui_refresh()) ha_apply_to_ui();
  if (call_take_ui_refresh()) { home_call_apply(); phone_apply_to_ui(); }
  if (vm_take_ui_refresh()) { home_vm_apply(); phone_apply_to_ui(); }
  if (ami_take_status_refresh()) phone_apply_to_ui();
  if (jf_take_ui_refresh()) { home_media_apply(); jf_apply_to_ui(); }
  if (notify_pending) { notify_pending = false; notify_apply_to_ui(); }
  if (panel_boot_request)
  {
    panel_boot_request = false;
    panel_boot_sequence();
  }
  if (panel_pause_request)
  {
    panel_pause_request = false;
    Serial.println("[Panel] pixel clock paused 1.5 s (setup page)");
    rgb_panel_signal_pause(RGB_PCLK_GPIO, 1500);
    rgb_panel_resync(rgbpanel);
    lv_obj_invalidate(lv_scr_act());
  }
  if (panel_reset_request)
  {
    panel_reset_request = false;
    bool ok = rgb_panel_full_reset(rgbpanel);
    Serial.printf("[Panel] display controller reset (setup page): %s\n", ok ? "ok" : "FAILED");
    lv_obj_invalidate(lv_scr_act()); // redraw everything
  }
  if (shot_page_req >= 0)
  {
    shot_apply_page(shot_page_req);
    shot_ready_ms = millis() + 1500; // a few frames, and the 1 s clock tick for the night clock / quick panel
    shot_page_req = -1;
  }
  if (gh_needs_ui_refresh) { gh_needs_ui_refresh = false; settings_info_update(); update_icon_refresh(); }

  delay(5);
}

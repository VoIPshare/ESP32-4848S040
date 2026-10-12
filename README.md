# ESP32-4848S040

A dashboard application for the 480×480 4-inch ESP32-4848S040 display (ESP32-S3), built with LVGL, featuring up to eight swipeable touch screens. The ESP32 fetches every widget's data itself, directly from each service: no Google Apps Script or other server in between (same approach as the [e-ink dashboard](https://github.com/VoIPshare/ESP32-eInk-Dashboard)).

## Features

| Screen | Data source | Needs | Refresh |
| --- | --- | --- | --- |
| Home: clock, weather, next event | NTP, [Open-Meteo](https://open-meteo.com/), your iCal feeds | city or coordinates | weather every 10 min |
| Forecast (tap the weather) | Open-Meteo: next 12 hours and 5 days | - | with the weather |
| Agenda (tap the "Next" card) | your iCal feeds: today and tomorrow | iCal URL | with the calendar |
| Bambu Lab printer | the printer, over local MQTT | printer IP, serial, access code | live |
| Calendar (month + day detail) | up to 3 iCal (`.ics`) feeds, merged, recurrence included | iCal URL | every 30 min |
| Proxmox cluster | Proxmox VE API | host + API token | every 20 s |
| Stocks (up to 7) | Yahoo Finance | ticker symbols | every 10 min |
| MakerWorld stats | Bambu's MakerWorld API | MakerWorld user ID | every hour |
| Packages (up to 5) | [PKGE](https://business.pkge.net) | PKGE API key | every 2 hours |
| Home Assistant (up to 8 entities, tap to toggle) | [Home Assistant](https://www.home-assistant.io/) REST API | address, long-lived token, entity IDs | every 10 s while showing, 5 min otherwise |
| Flight on the Home strip (optional live status) | calendar titles, [AeroDataBox](https://rapid.aerodatabox.com) via RapidAPI | RapidAPI key (optional) | every 30 min on the day, 10 min around departure/landing |

Flights in your calendar (titles like `AC871 YUL->YYZ`, `TS399 PUJ->YUL` or anything containing "flight") are pinned on the Home strip from 24 hours before departure, with a countdown. With an AeroDataBox key the strip also shows delays, gate, "In the air" and "Landed".

Also:
- **Alerts**: a blinking banner across the top for a delivered package, a Proxmox node going offline or a failed print. It lights the screen up (not at night) and goes away after a minute or on a tap.
- **Night**: dimming on a schedule you choose, with a big dim clock instead of the dashboard (or the screen off).
- **Updates**: firmware updates from GitHub releases straight from the device, or by uploading a file in the browser.
- **Backup**: download all settings as a file and restore them later.
- Dark theme with color-coded status (green/yellow/red); while printing, Home shows the progress and time left.

## Hardware Requirements

- **Display Module:** ESP32-4848S040 (480×480, 4.0")
- **Microcontroller:** ESP32-S3 (built into the display module)
- **Power Supply:** 5V USB, 2A recommended
- Touch: the built-in GT911 capacitive panel

## Installation

### 1. Arduino IDE Setup

1. Arduino IDE → Preferences → Additional Board Manager URLs, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
2. Tools → Board → Boards Manager → search "ESP32" → install "esp32 by Espressif Systems"
3. Select Board: `ESP32S3 Dev Module`, PSRAM: `OPI PSRAM`, Flash Size: `16MB`, Partition Scheme: `16M Flash (3MB APP/9.9MB FATFS)`, Upload Speed: `115200` (faster speeds can fail on this board's USB-serial chip). The sketch refuses to build for any chip other than the ESP32-S3.

### 2. Install Libraries

Sketch → Include Library → Manage Libraries, install:
- **GFX Library for Arduino** 1.6.9 (by moononournation)
- **lvgl** 8.4.0 (see [Compatibility](#lvgl-version-compatibility) below)
- **TAMC_GT911** 1.0.2 (touch)
- **ArduinoJson** 7.4.3
- **PubSubClient** 2.8 (Bambu MQTT)

### 3. Configure LVGL

Copy the config template into place:
```bash
cp ~/Arduino/libraries/lvgl/lv_conf_template.h ~/Arduino/libraries/lv_conf.h
```

Edit `lv_conf.h` and uncomment/set:
```c
#define LV_COLOR_DEPTH 16
#define LV_TICK_CUSTOM 1
#define LV_COLOR_16_SWAP 1      // For SPI displays (0 for RGB parallel)
#define LV_USE_PERF_MONITOR 1   // Optional: shows FPS
#define LV_USE_MEM_MONITOR 0    // Avoid redefinition conflicts

// Fonts used by the dashboard
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_10 1
```

### 4. Upload

1. Connect the ESP32-4848S040 via USB-C; select the port under Tools → Port
2. Open `ESP32-4848S040.ino`, click Upload
3. If upload fails, hold the **BOOT** button while uploading, release once it starts
4. Tools → Serial Monitor, baud rate **115200** — you should see `Setup complete!`

The display should light up on the Home screen.

### LVGL Version Compatibility

This sketch targets **LVGL 8.4** (8.3.x also works). If your Library Manager installed **LVGL 9.x** instead, you'll get compile errors like `Arduino_ST7701_RGBPanel not found` or `lv_disp_draw_buf_t not declared`.

**Fix — install LVGL 8.4 (recommended):**
1. Sketch → Include Library → Manage Libraries → search "LVGL"
2. Version dropdown → select **8.4.0** → Install
3. In `lv_conf.h`, confirm `LV_USE_MEM_MONITOR 0`
4. Close and fully reopen Arduino IDE, then upload again

If you'd rather stay on LVGL 9.5+, you'll need to update the sketch's display-init calls to the 9.x API (font/config macros above still apply) and keep `Arduino_GFX_Library` at 1.6.7+.

## Navigation

| Where | Tap | Hold |
| --- | --- | --- |
| Bottom-left corner | previous screen | |
| Bottom-right corner | next screen | |
| Top-left corner | jump menu: one icon per screen, including Home | 3 s: pin / unpin the current screen |
| Top-right corner | | 5 s: Settings |
| Anywhere | swipe left/right to change screen | |
| Top edge (middle) | swipe down: **quick panel** with brightness, pin, "Dim now", Settings and Wi-Fi info | |

- **Pinned** (a red pin icon top-left): the screen stays put. Swipes, corner taps, the jump menu, automatic returns to Home and automatic screen changes (print start, flight changes) are all paused until you hold the top-left corner again.
- **Update available** (a green download icon on Home, right edge): tap it to install the new release or skip that version. See [Firmware updates](#firmware-updates).
- The Wi-Fi icon (top-right) shows only on Home, for 20 seconds after you get there or after the connection changes, and stays while Wi-Fi is down. Turn **Auto-hide Wi-Fi icon** off in Settings to always show it on every screen.
- On Home, tap the weather for the **Forecast** and the "Next" card for the **Agenda**. Detail pages (these two and the Calendar day view) have a back button top-left, and the top-left corner works as back too.
- The jump menu (two rows of icons) closes after 10 seconds or on a tap outside it.
- **Dim now** in the quick panel dims the screen (or shows the night clock) right away, day or night, until the next touch.
- At night (if dimming is on) the first touch only wakes the screen.
- With "Always on Home" (setup page), the icons also stay on Home, right of the menu corner.

## Screens

Use the corners or swipe left/right to move between them (see [Navigation](#navigation)):

1. **Home**: clock, date, current weather (icon, temperature, humidity, wind, UV) and the next calendar event or flight; a print in progress shows its percentage and time left. Tap the weather for the Forecast, the "Next" card for the Agenda
2. **Bambu Lab printer**: status, progress, temperatures, layers, AMS trays and humidity (orange from 40%, red from 50%), fans and HMS warnings. Tap the printer name for the **print history**: the last 6 prints, finished or failed, with date and duration
3. **Calendar**: the current month with event days marked; tap a day for its events
4. **Proxmox cluster**: up to 3 nodes with status, CPU, RAM, disk and uptime
5. **Stocks**: up to 7 quotes with daily change and a sparkline of today's price (15-minute steps, with a faint line at yesterday's close)
6. **MakerWorld**: likes, downloads, prints, collections and boosts with today's growth, designs/followers/level, and the 3 most downloaded designs
7. **Packages**: up to 5 PKGE parcels with status and estimated delivery
8. **Flight**: only while a calendar flight is coming up or in progress
9. **Home Assistant**: only once it's set up; up to 8 entities as tiles, lit when on. Lights, switches, fans, input booleans, automations and scripts toggle with a tap

## Settings screen

Hold the top-right corner for 5 seconds. Top right shows the firmware version and update status, uptime, IP address, Wi-Fi signal and free memory (RAM + PSRAM).

| Control | What it does |
| --- | --- |
| **Setup Access Point** | Starts the setup Wi-Fi network and web page (see below). |
| **Show Bambu screen on print start** | Jumps to the Bambu screen when a print starts. |
| **Show Flight screen on changes** | Shows the Flight screen for 2 minutes when its data changes. |
| **Auto-hide Wi-Fi icon** | On: the icon shows only on Home, for 20 seconds. Off: always, on every screen. |
| **Return to Home after** | 1, 2, 5 or 10 minutes without a touch. |
| **Restart** | Restarts the device. |
| **Check update / Install vX.Y** | Looks for a newer GitHub release; once one is found, the same button installs it. |
| **Close** | Turns the setup network off and goes back. |

## Setup page

Two ways in:
- **On your Wi-Fi** (once the display is connected): open `http://dashboard.local` or the IP shown in Settings, and log in as `admin` with the setup password shown in Settings (random, made once per device).
- **Setup network** (first setup, or a new Wi-Fi): in Settings turn on **Setup Access Point**, join `ESP32-Dashboard-Setup` with that same password, and browse to `http://192.168.4.1` (no login needed there).

Saving restarts the device.

| Field | What to enter |
| --- | --- |
| **Wi-Fi** | Your network (the list is scanned live) and its password. |
| **Time zone** | Your zone; summer/winter time is automatic. |
| **Weather location** | A city, optionally with a 2-letter country code (`London, GB`), or `latitude,longitude`. Empty uses the city of the time zone. A city is looked up once with Open-Meteo's geocoding. |
| **Language** | English, Italiano or Français: dates, weather, Home, Calendar, Agenda, Forecast, Packages, alerts and the quick panel. The technical screens (Bambu, Proxmox, Stocks, MakerWorld, Settings) stay in English. |
| **Home screen: shortcut icons** | Top-left corner menu (default), always on Home as well, or off. See [Navigation](#navigation). |
| **Display brightness** | Normal brightness, and night dimming: on/off, from/until hour (it may wrap past midnight, e.g. 23 to 7), the big night clock on/off, and night brightness (0 turns the screen off). At night the screen dims after 30 seconds without a touch; a touch brings it back. |
| **Firmware updates from GitHub** | Show an icon and let me choose (default), install automatically, or don't check. |
| **Bambu printer** | Printer name (shown on screen), IP address, port (8883), serial number and LAN access code, from the printer's network settings. |
| **Stocks** | Up to 7 Yahoo Finance symbols, comma separated: `AAPL,NVDA,^IXIC`. |
| **Calendar** | Up to 3 `.ics` links. Google Calendar: Settings → your calendar → Integrate calendar → *Secret address in iCal format* (treat it like a password). |
| **MakerWorld user ID** | The number in your profile's `/design-service/published/<number>/design` address. |
| **PKGE API key** | From business.pkge.net → Company profile → API key. Add tracking numbers in the PKGE dashboard. Empty keeps the saved key, `-` removes it. |
| **AeroDataBox API key** | Optional. Free key: rapidapi.com → AeroDataBox → Basic plan. Empty keeps the saved key, `-` removes it. |
| **Home Assistant** | Address (`http://homeassistant.local:8123`), a long-lived access token (your HA profile → Security), and up to 8 entity IDs, comma separated: `light.desk, switch.fan, sensor.outdoor_temperature`. Empty token keeps the saved one, `-` removes it. |
| **Proxmox cluster** | Host as `ip:port` (8006), API token ID (`user@realm!name`) and secret. A read-only PVEAuditor token is enough. |
| **Screenshots** | *Allow screenshots without login* (off by default). See [Screenshots](#screenshots). |
| **Firmware update** | Upload a `.ino.bin` file to update from the browser. |
| **Backup** | *Download all settings* saves them as a JSON file (with your passwords and keys, so keep it private). Pick that file and press **Restore settings** to load it back, e.g. after a full re-flash or on a second display. |

## Screenshots

`http://dashboard.local/screenshot` returns what the screen shows as a 480×480 BMP image. Add `?page=` to switch first: `home`, `bambu`, `calendar`, `proxmox`, `stocks`, `makerworld`, `packages`, `flight`, `homeassistant`, `forecast`, `agenda`, `history`, `calday`, `settings`, `quick` (quick panel), `night` (night clock, until the next touch).

```bash
curl -o forecast.bmp "http://dashboard.local/screenshot?page=forecast"
```

It needs the setup login, unless **Allow screenshots without login** is on (setup page). Turn that on only while you need it: anyone on your Wi-Fi could see the screen.

## Messages from Home Assistant

Anything in Home Assistant can show a message on the alert banner. Add this to `configuration.yaml` (password: the setup password from Settings):

```yaml
rest_command:
  dashboard_message:
    url: http://dashboard.local/notify
    method: POST
    username: admin
    password: !secret dashboard_password
    content_type: application/json
    payload: '{"message": "{{ message }}", "color": "{{ color | default(''blue'') }}", "seconds": {{ seconds | default(60) }}}'
```

Then in an automation: `action: rest_command.dashboard_message` with `data: {message: "Washing machine done", color: green}`. Colors: red, green, orange, blue, purple. It lights up the screen (not at night) and stays until the time is up or you tap it.

## Firmware updates

- **From GitHub (on the device):** the device checks the latest release of this repository once a day. Depending on the setup page choice it shows a green icon on Home (tap it, then **Install** or **Skip** this version), installs by itself, or does nothing until you press **Check update** in Settings. Downloads are checked against pinned root certificates, so only files really coming from GitHub are accepted.
- **From the browser:** on the setup page, pick the `ESP32S3-4848S040-ESP32-4848S040.ino.bin` file from a release (not the `merged` one) and press **Upload firmware**.
- **By USB:** as in [Installation](#installation), or flash the `merged` file at address 0 with esptool.

The screen shows "Updating firmware - the screen will go black for about 90 seconds" for a few seconds, then stays black until the display restarts with the new version. Writing the flash makes this panel flicker whatever it shows, so the backlight stays off meanwhile. Settings are kept.

A watchdog restarts the display by itself if it ever freezes. Unplanned restarts (crash, watchdog, power dip) are counted and shown on the Settings screen.

## Continuous integration

`.github/workflows/compile.yml` (run it from the Actions tab) builds the firmware for the ESP32-S3 only, with LVGL 8.4.0 and `ci/lv_conf.h`, reads `FW_VERSION` from `ESP32-4848S040.ino`, and on `main` publishes the binaries as release `firmware-<version>` (named "ESP32-S3Dashboard <version>"), which is where the devices update from. Bump `FW_VERSION` before each release; the workflow refuses a version that's already released.

## Hardware Reference

### Display Pins (built-in, RGB parallel)

```
R Channel: R0-R4 → GPIO 11,12,13,14,0
G Channel: G0-G5 → GPIO 8,20,3,46,9,10
B Channel: B0-B4 → GPIO 4,5,6,7,15
Control:   PCLK→21  HSYNC→16  VSYNC→17  DE→18
SPI/Cfg:   CS→39  SCK→48  SDA/MOSI→47
Backlight: BL (PWM, dimmable)→38
Touch:     GT911 I2C  SDA→19  SCL→45  (no INT/RST)
```

## Development

- Each screen lives in its own tab (`bambu.ino`, `calendar.ino`, ...) with a `create_*_screen()` builder and a `*_apply_to_ui()` refresh. To add one, add it to `screens[]` in `setup()` and bump `SCREEN_COUNT`.
- Network work runs on `network_task` (core 0) and must never call LVGL: write the data under `data_lock()`, set a `*_needs_ui_refresh` flag, and let `loop()` apply it.
- Set `DEBUG` at the top of `ESP32-4848S040.ino` to 0 to silence verbose Serial output.

## Fonts

The UI uses `lv_font_ui_<size>.c` instead of LVGL's built-in Montserrat, so accented letters (Italian, French, accented calendar or design titles) show. They're made from the same files as the built-in ones (`scripts/built_in_font` in the LVGL library), for sizes 12-28:

```bash
npx lv_font_conv --no-compress --no-prefilter --bpp 4 --size 16 --font Montserrat-Medium.ttf -r 0x20-0x7F,0xA0-0xFF,0x2013-0x2014,0x2018-0x2019,0x201C-0x201D,0x2022,0x2026 --font FontAwesome5-Solid+Brands+Regular.woff -r <the symbol list in built_in_font_gen.py> --format lvgl --lv-font-name lv_font_ui_16 -o lv_font_ui_16.c --force-fast-kern-format
```

Then change its include to plain `#include "lvgl.h"`.

## Data flow

```
network_task (core 0)                         loop() (core 1, LVGL)
  fetch_weather, proxmox_fetch, Bambu MQTT
  web_fetch.ino -> direct_fetch.cpp      -->  *_needs_ui_refresh flags --> *_apply_to_ui()
  (stocks, iCal, MakerWorld, PKGE, flight)    data copied in under data_lock()
```

## Performance

| Item | Value |
|--------|-------|
| Display | 480×480 RGB565, ST7701S, 10 MHz pixel clock (~35 fps panel refresh) |
| Frame buffer | PSRAM (480×480×2 bytes) |
| LVGL draw buffer | 32 lines in internal RAM |
| Firmware size | ~1.7 MB of the 3 MB app partition (two partitions, for updates) |

## Color Palette

| Purpose | Color | Hex | RGB |
|---------|-------|-----|-----|
| Primary | Cyan | #00BCD4 | 0,188,212 |
| Secondary | Orange | #FF9800 | 255,152,0 |
| Success | Green | #4CAF50 | 76,175,80 |
| Warning | Yellow | #FFC107 | 255,193,7 |
| Error | Red | #F44336 | 244,67,54 |
| BG Dark | — | #121212 | 18,18,18 |
| BG Light | — | #1E1E1E | 30,30,30 |

## Troubleshooting

**Display is black**
- Check power supply (5V, 2A minimum)
- Verify backlight pin (GPIO 38) is receiving power
- Check board selection is `ESP32S3 Dev Module`

**Upload fails / times out**
- Hold BOOT button while uploading, release once it starts
- Try a different USB cable
- Verify Tools → Port shows the correct port (install CH340 driver on Windows if missing)

**Colors look wrong**
- Check `LV_COLOR_16_SWAP` in `lv_conf.h` matches your display
- Recheck display init parameters in the main sketch

**Lines flashing, picture rolled up by a few lines, or small dark dashes**
- Also expected for a moment while a firmware update is being written.
- The panel streams its picture from PSRAM, which it shares with Wi-Fi and drawing. The firmware runs the panel at 10 MHz, draws in internal RAM (the boot log says `LVGL draw buffer: 32 lines in internal RAM`), writes the drawn rows back from the cache in whole lines, and re-aligns the panel every 2 seconds and on every screen change (`rgb_resync.cpp`), so a rolled picture recovers on its own.
- Brief flashes can still happen during large downloads (MakerWorld, hourly). Use a solid 5 V / 2 A supply.

**Compilation errors**
- See [LVGL Version Compatibility](#lvgl-version-compatibility) above

**Touch in the wrong place**
- Adjust `TOUCH_ROTATION_STEPS` (0-3) at the top of `ESP32-4848S040.ino`

**A screen stays empty or shows "fetch failed"**
- Check its field on the setup page, and watch the Serial log (115200 baud): each source logs its HTTP result (`[ICS]`, `[Stocks]`, `[MakerWorld]`, `[Tracking]`, `[Flight]`, `[Weather]`)

## File Structure

```
ESP32-4848S040/
├── ESP32-4848S040.ino          ← Main sketch (upload this): display, Home, settings, Wi-Fi portal, network task
├── bambu.ino / calendar.ino / proxmox.ino / stocks.ino / makerworld.ino / tracking.ino / flight.ino / homeassistant.ino  ← one tab per screen (calendar.ino also has the Agenda)
├── forecast.ino                 ← Forecast page
├── alerts.ino / screensaver.ino ← alert banner, night clock
├── web_fetch.ino                ← schedules the direct fetches and copies results into the screens
├── direct_fetch.cpp/.h          ← Open-Meteo geocoding, Yahoo, iCal parser, MakerWorld, PKGE, AeroDataBox
├── gh_update.cpp/.h             ← firmware updates from GitHub releases (pinned root certificates)
├── lv_font_roboto_*.c           ← clock/temperature digits (made with lv_font_conv)
├── lv_font_ui_*.c               ← UI text: LVGL's Montserrat + symbols, plus Latin-1 accents (see below)
├── lang.h                       ← English / Italian / French strings
├── quickpanel.ino / printhistory.ino ← quick panel, Bambu print history
├── rgb_resync.cpp               ← re-aligns the RGB panel after a PSRAM bandwidth hiccup
├── home_icons.c                 ← shortcut icons (generated by tools/make_home_icons.py)
├── calendar_types.h             ← calendar/flight structs shared by the tabs and direct_fetch.cpp
├── ci/lv_conf.h                 ← LVGL 8.4 config used by the CI build
├── .github/workflows/compile.yml← ESP32-S3 build + release (manual trigger)
└── README.md                    ← This file
```

## Resources

Data sources (all called directly by the ESP32):
- [Open-Meteo](https://open-meteo.com/en/docs) - weather forecast, and [geocoding](https://open-meteo.com/en/docs/geocoding-api) for the city lookup (free, no key)
- [Yahoo Finance](https://finance.yahoo.com/) - stock quotes (unofficial chart endpoint, no key)
- iCal (`.ics`) feeds - e.g. a [Google Calendar secret address](https://support.google.com/calendar/answer/37648)
- [MakerWorld](https://makerworld.com/) - published designs and public profile, through Bambu's `api.bambulab.com` (unofficial, no key)
- [PKGE](https://business.pkge.net/) - package tracking (API key)
- [AeroDataBox on RapidAPI](https://rapidapi.com/aedbx-aedbx/api/aerodatabox) - live flight status (optional API key)
- [Proxmox VE API](https://pve.proxmox.com/pve-docs/api-viewer/) - cluster status (API token)
- [Home Assistant REST API](https://developers.home-assistant.io/docs/api/rest/) - entity states and toggles (long-lived token)
- Bambu Lab printer - local MQTT over TLS in LAN mode ([OpenBambuAPI](https://github.com/Doridian/OpenBambuAPI) documents the messages)
- NTP (`pool.ntp.org`) - clock

Libraries and tools:
- [LVGL 8.4](https://docs.lvgl.io/8.4/) · [Widgets](https://docs.lvgl.io/8.4/widgets/index.html)
- [GFX Library for Arduino](https://github.com/moononournation/Arduino_GFX_Library) (ST7701 RGB panel)
- [TAMC_GT911](https://github.com/TAMCTec/gt911-arduino) (touch)
- [ArduinoJson](https://arduinojson.org/)
- [PubSubClient](https://github.com/knolleary/pubsubclient) (MQTT)
- [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32)
- [lv_font_conv](https://github.com/lvgl/lv_font_conv) - the Roboto clock/temperature fonts
- [Material Design Icons](https://pictogrammers.com/library/mdi/) - shortcut and pin icons (`tools/make_home_icons.py`)

## License

Open source — use, modify, and distribute freely.

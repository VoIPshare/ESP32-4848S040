# ESP32-4848S040

A dashboard application for the 480×480 4-inch ESP32-4848S040 display (ESP32-S3), built with LVGL, featuring up to eight swipeable touch screens. The ESP32 fetches every widget's data itself, directly from each service: no Google Apps Script or other server in between (same approach as the [e-ink dashboard](https://github.com/VoIPshare/ESP32-eInk-Dashboard)).

## Features

| Screen | Data source | Needs | Refresh |
| --- | --- | --- | --- |
| Home: clock, weather, next event | NTP, [Open-Meteo](https://open-meteo.com/), your iCal feeds | city or coordinates | weather every 10 min |
| Bambu Lab printer | the printer, over local MQTT | printer IP, serial, access code | live |
| Calendar (month + day detail) | up to 3 iCal (`.ics`) feeds, merged, recurrence included | iCal URL | every 30 min |
| Proxmox cluster | Proxmox VE API | host + API token | every 20 s |
| Stocks (up to 7) | Yahoo Finance | ticker symbols | every 10 min |
| MakerWorld stats | Bambu's MakerWorld API | MakerWorld user ID | every hour |
| Packages (up to 5) | [PKGE](https://business.pkge.net) | PKGE API key | every 2 hours |
| Flight on the Home strip (optional live status) | calendar titles, [AeroDataBox](https://rapid.aerodatabox.com) via RapidAPI | RapidAPI key (optional) | every 30 min on the day, 10 min around departure/landing |

Flights in your calendar (titles like `AC871 YUL->YYZ`, `TS399 PUJ->YUL` or anything containing "flight") are pinned on the Home strip from 24 hours before departure, with a countdown. With an AeroDataBox key the strip also shows delays, gate, "In the air" and "Landed".

Dark theme, color-coded status indicators (green/yellow/red), 60 FPS LVGL rendering.

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
- **lvgl** 8.3.11 (see [Compatibility](#lvgl-version-compatibility) below)
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

This sketch targets **LVGL 8.3**. If your Library Manager installed **LVGL 9.5+** instead, you'll get compile errors like `Arduino_ST7701_RGBPanel not found` or `lv_disp_draw_buf_t not declared`.

**Fix — downgrade to LVGL 8.3 (recommended):**
1. Sketch → Include Library → Manage Libraries → search "LVGL"
2. Version dropdown → select **8.3.0** → Install
3. In `lv_conf.h`, confirm `LV_USE_MEM_MONITOR 0`
4. Close and fully reopen Arduino IDE, then upload again

If you'd rather stay on LVGL 9.5+, you'll need to update the sketch's display-init calls to the 9.x API (font/config macros above still apply) and keep `Arduino_GFX_Library` at 1.6.7+.

## Navigation

| Where | Tap | Hold |
| --- | --- | --- |
| Bottom-left corner | previous screen | 3 s: pin / unpin the current screen |
| Bottom-right corner | next screen | |
| Top-left corner | jump menu: one icon per screen, including Home | |
| Top-right corner | next screen | 5 s: Settings |
| Anywhere | swipe left/right to change screen | |

- **Pinned** (a "PINNED" badge at the top): the screen stays put. Swipes, corner taps, the jump menu, automatic returns to Home and automatic screen changes (print start, flight changes) are all paused until you hold the bottom-left corner again.
- The jump menu closes after 10 seconds or on a tap outside it. On the Calendar day view the top-left corner is its back button.
- With "Always on Home" (setup page), the icons also stay on Home, right of the menu corner.

## Screens

Use the corners or swipe left/right to move between them (see [Navigation](#navigation)):

1. **Home**: clock, date, current weather (icon, temperature, humidity, wind, UV) and the next calendar event or flight
2. **Bambu Lab printer**: status, progress, temperatures, layers, AMS trays, fans and HMS warnings
3. **Calendar**: the current month with event days marked; tap a day for its events
4. **Proxmox cluster**: up to 3 nodes with status, CPU, RAM, disk and uptime
5. **Stocks**: up to 7 quotes with daily change
6. **MakerWorld**: likes, downloads, prints, collections and boosts
7. **Packages**: up to 5 PKGE parcels with status and estimated delivery
8. **Flight**: only while a calendar flight is coming up or in progress

## Setup page

Hold the top-right corner for 5 seconds to open Settings, turn on **Setup Access Point**, join `ESP32-Dashboard-Setup` (password `12345678`) and browse to `http://192.168.4.1`. Saving restarts the device.

| Field | What to enter |
| --- | --- |
| **Weather location** | A city, optionally with a 2-letter country code (`London, GB`), or `latitude,longitude`. A city is looked up once with Open-Meteo's geocoding. |
| **Stocks** | Up to 7 Yahoo Finance symbols, comma separated: `AAPL,NVDA,^IXIC`. |
| **Calendar** | Up to 3 `.ics` links. Google Calendar: Settings → your calendar → Integrate calendar → *Secret address in iCal format* (treat it like a password). |
| **MakerWorld user ID** | The number in your profile's `/design-service/published/<number>/design` address. |
| **PKGE API key** | From business.pkge.net → Company profile → API key. Add tracking numbers in the PKGE dashboard. Empty keeps the saved key, `-` removes it. |
| **AeroDataBox API key** | Optional. Free key: rapidapi.com → AeroDataBox → Basic plan. Empty keeps the saved key, `-` removes it. |
| **Home screen: shortcut icons** | Top-left corner menu (default), always on Home as well, or off. See [Navigation](#navigation). |
| **Bambu printer / Proxmox** | As before. |

Settings from older firmware are kept; the old latitude/longitude become the weather location. The old "Dashboard Data" (Apps Script) ID is no longer used.

## Continuous integration

`.github/workflows/compile.yml` (run it from the Actions tab) builds the firmware for the ESP32-S3 only, with LVGL 8.3.11 and `ci/lv_conf.h`, reads `FW_VERSION` from `ESP32-4848S040.ino`, and on `main` publishes the binaries as release `firmware-<version>`. Bump `FW_VERSION` before each release; the workflow refuses a version that's already released.

## Hardware Reference

### Display Pins (built-in, RGB parallel)

```
R Channel: R0-R4 → GPIO 11,12,13,14,0
G Channel: G0-G5 → GPIO 8,20,3,46,9,10
B Channel: B0-B4 → GPIO 4,5,6,7,15
Control:   PCLK→21  HSYNC→16  VSYNC→17  DE→18
SPI/Cfg:   CS→39  SCK→48  SDA/MOSI→47
Backlight: BL (PWM)→38
```

### Optional Wiring

```
Touch (GT911, common pins):
  SDA → GPIO 17   SCL → GPIO 18   INT → GPIO 8 (optional)

DHT22 sensor:
  Data → GPIO 9   VCC → 3.3V   GND → GND

BME680 (I2C):
  SDA → GPIO 15   SCL → GPIO 16   VCC → 3.3V   GND → GND
```

Available GPIO for other uses: 2, 19, 34, 35, 36, 37, 40, 41, 42, 43, 44, 45 (some restricted).

## Development

- Each screen lives in its own tab (`bambu.ino`, `calendar.ino`, ...) with a `create_*_screen()` builder and a `*_apply_to_ui()` refresh. To add one, add it to `screens[]` in `setup()` and bump `SCREEN_COUNT`.
- Network work runs on `network_task` (core 0) and must never call LVGL: write the data under `data_lock()`, set a `*_needs_ui_refresh` flag, and let `loop()` apply it.
- Set `DEBUG` at the top of `ESP32-4848S040.ino` to 0 to silence verbose Serial output.

## Data flow

```
network_task (core 0)                         loop() (core 1, LVGL)
  fetch_weather, proxmox_fetch, Bambu MQTT
  web_fetch.ino -> direct_fetch.cpp      -->  *_needs_ui_refresh flags --> *_apply_to_ui()
  (stocks, iCal, MakerWorld, PKGE, flight)    data copied in under data_lock()
```

## Performance

| Metric | Value |
|--------|-------|
| Display Resolution | 480×480 |
| Refresh Rate | ~60 FPS |
| Draw Buffer | ~200KB |
| VRAM Used | ~460KB (480×480×2 bytes) |
| Free Heap Required | ~500KB (LVGL + buffers) |
| Boot Time | ~2-3 seconds |
| Screen Switch Time | <100ms |
| Power Usage | ~1-2W at full brightness |

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
├── bambu.ino / calendar.ino / proxmox.ino / stocks.ino / makerworld.ino / tracking.ino / flight.ino  ← one tab per screen
├── web_fetch.ino                ← schedules the direct fetches and copies results into the screens
├── direct_fetch.cpp/.h          ← Open-Meteo geocoding, Yahoo, iCal parser, MakerWorld, PKGE, AeroDataBox
├── rgb_resync.cpp               ← re-aligns the RGB panel after a PSRAM bandwidth hiccup
├── home_icons.c                 ← shortcut icons (generated by tools/make_home_icons.py)
├── calendar_types.h             ← calendar/flight structs shared by the tabs and direct_fetch.cpp
├── ci/lv_conf.h                 ← LVGL 8.3 config used by the CI build
├── .github/workflows/compile.yml← ESP32-S3 build + release (manual trigger)
└── README.md                    ← This file
```

## Resources

- [LVGL Documentation](https://docs.lvgl.io/) · [Widgets](https://docs.lvgl.io/8.3/widgets/) · [Examples](https://github.com/lvgl/lv_demos)
- [Arduino_GFX Library](https://github.com/moononournation/Arduino_GFX_Library)
- [ESP32 Arduino Core](https://github.com/espressif/arduino-esp32)
- [ESP32-4848S040 (Waveshare Wiki)](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-4.0)
- [ArduinoJson](https://arduinojson.org/)
- APIs: [BambuLab](https://wiki.bambu.tech/), [Proxmox](https://pve.proxmox.com/pve-docs/api-viewer/), [OpenWeatherMap](https://openweathermap.org/api)

## License

Open source — use, modify, and distribute freely.
# ESP32-4848S040

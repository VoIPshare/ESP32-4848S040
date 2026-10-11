# ESP32-4848S040 Multi-Screen Dashboard

A dashboard application for the 480×480 4-inch ESP32-4848S040 display, built with LVGL, featuring four scrollable screens.

## Features

- 🌡️ **Climate Control** — temperature, humidity, and clock display
- 🖨️ **BambuLab Printer** — print status, progress, and temperature monitoring
- 🖥️ **Proxmox Cluster** — VM/container host monitoring (CPU, RAM, status per node)
- 📦 **Package Tracking** — delivery status with estimated dates

Dark theme, color-coded status indicators (green/yellow/red), 60 FPS LVGL rendering.

## Hardware Requirements

- **Display Module:** ESP32-4848S040 (480×480, 4.0")
- **Microcontroller:** ESP32-S3 (built into the display module)
- **Power Supply:** 5V USB, 2A recommended
- **Optional:** Touch input (GT911, FT6X36, or XPT2046), 2 GPIO buttons for navigation

## Installation

### 1. Arduino IDE Setup

1. Arduino IDE → Preferences → Additional Board Manager URLs, add:
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
2. Tools → Board → Boards Manager → search "ESP32" → install "esp32 by Espressif Systems"
3. Select Board: `ESP32S3 Dev Module`, Upload Speed: `921600`

### 2. Install Libraries

Sketch → Include Library → Manage Libraries, install:
- **Arduino_GFX_Library** (by Arduino)
- **LVGL** (v8.3, by Kisvegry — see [Compatibility](#lvgl-version-compatibility) below)
- **ArduinoJson** (for API integration)
- Touch driver as needed (GT911, FT6X36, XPT2046)

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
2. Open `esp32_dashboard.ino`, click Upload
3. If upload fails, hold the **BOOT** button while uploading, release once it starts
4. Tools → Serial Monitor, baud rate **115200** — you should see `Setup complete!`

The display should light up on the Climate Control screen.

### LVGL Version Compatibility

This sketch targets **LVGL 8.3**. If your Library Manager installed **LVGL 9.5+** instead, you'll get compile errors like `Arduino_ST7701_RGBPanel not found` or `lv_disp_draw_buf_t not declared`.

**Fix — downgrade to LVGL 8.3 (recommended):**
1. Sketch → Include Library → Manage Libraries → search "LVGL"
2. Version dropdown → select **8.3.0** → Install
3. In `lv_conf.h`, confirm `LV_USE_MEM_MONITOR 0`
4. Close and fully reopen Arduino IDE, then upload again

If you'd rather stay on LVGL 9.5+, you'll need to update the sketch's display-init calls to the 9.x API (font/config macros above still apply) and keep `Arduino_GFX_Library` at 1.6.7+.

## Navigation

- **Buttons (optional):** GPIO 6 → next screen, GPIO 7 → previous screen (wire each to GND, `INPUT_PULLUP`)
- **Touch (optional):** implement a touch driver and `#include` it in the sketch — not included by default

## Screens

### Climate Control
Temperature, humidity, digital clock/date, system status.

### BambuLab Printer
Status (Online/Printing/etc.), progress bar, nozzle & bed temps, time remaining.

### Proxmox Cluster
3 nodes with online/degraded status, CPU %, RAM %, overall cluster health.

### Package Tracking
3 packages with status (Processing/In Transit/Delivered), estimated dates, carrier.

## Configuration

Edit `config.h` to customize:

```c
// Colors
#define COLOR_PRIMARY_RGB 0x00, 0xBC, 0xD4

// WiFi
#define ENABLE_WIFI
#define WIFI_SSID "YOUR_SSID"
#define WIFI_PASSWORD "YOUR_PASSWORD"

// API endpoints
#define ENABLE_BAMBU_API
#define BAMBU_PRINTER_IP "192.168.1.100"

// Update intervals (ms)
#define CLIMATE_UPDATE_INTERVAL 5000
#define PRINTER_UPDATE_INTERVAL 2000
```

## Adding Real Data

`data_sources.h` currently returns placeholder/static values. To connect real data (sensors, BambuLab, Proxmox, delivery carriers, WiFi/HTTPS setup, error handling, rate limiting), see **[API_INTEGRATION_GUIDE.md](API_INTEGRATION_GUIDE.md)** — it has complete code samples for each integration.

For package tracking, stocks, calendar, flight status, and weather in one call, see **[google_apps_script/](google_apps_script/)** — a Google Apps Script backend you deploy behind a Google Sheet and hit from the ESP32 with a single HTTP GET.

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
Navigation buttons:
  GPIO 6 → Button (next)     → GND
  GPIO 7 → Button (previous) → GND

Touch (GT911, common pins):
  SDA → GPIO 17   SCL → GPIO 18   INT → GPIO 8 (optional)

DHT22 sensor:
  Data → GPIO 9   VCC → 3.3V   GND → GND

BME680 (I2C):
  SDA → GPIO 15   SCL → GPIO 16   VCC → 3.3V   GND → GND
```

Available GPIO for other uses: 2, 19, 34, 35, 36, 37, 40, 41, 42, 43, 44, 45 (some restricted).

## Development

### Adding a New Screen
```c
lv_obj_t *create_my_screen() {
  lv_obj_t *scr = lv_obj_create(NULL);
  // ... add widgets
  return scr;
}
```
Add it to the `screens[]` array in the main sketch and update the navigation loop bounds (4 screens → 5).

### Adding a New Sensor
```c
// In data_sources.h
typedef struct { float value; uint32_t timestamp; } MyData;
void my_sensor_update() { /* read sensor */ }
```
Call it from `update_all_data_sources()` in the main loop.

### Debugging
```c
#define DEBUG_SERIAL 1   // in config.h
```
```c
DEBUG_PRINTLN("My debug message");
print_heap_info();   // from utils.h
```

## Data Flow

```
Data Sources (sensors / cloud APIs, optional WiFi)
        │
        ▼
data_sources.h  — climate_update(), printer_update(), proxmox_update(), vessel_update()
        │
        ▼
LVGL objects  — label text, progress bars, status colors
        │
        ▼
Arduino_GFX  — renders to 480×480 RGB display at 60 FPS
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

**Slow performance / low FPS**
- Lower the SPI clock: change `gfx->begin(16000000)` to `gfx->begin(10000000)`
- Disable `LV_USE_PERF_MONITOR` in `lv_conf.h`
- Watch serial output for `LVGL disp_draw_buf allocate failed`

**Compilation errors**
- See [LVGL Version Compatibility](#lvgl-version-compatibility) above

**Touch not working**
- No touch driver ships with this project — implement one (e.g. GT911) separately and `#include` it

**WiFi/API integration not working**
- Uncomment `#define ENABLE_WIFI` in `config.h` and set credentials
- Implement the actual HTTP calls in `data_sources.h` (see API_INTEGRATION_GUIDE.md)

## File Structure

```
esp32_dashboard/
├── esp32_dashboard.ino          ← Main sketch (upload this)
├── config.h                     ← Colors, pins, update intervals
├── data_sources.h               ← Data structures & API placeholders
├── utils.h                      ← Helper functions
├── README.md                    ← This file
├── API_INTEGRATION_GUIDE.md     ← Real data source integration (sensors, BambuLab, Proxmox, carriers)
└── google_apps_script/          ← Optional Google Sheets–backed JSON API (tracking, stocks, calendar, flight, weather)
    ├── Code.gs
    └── README.md
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

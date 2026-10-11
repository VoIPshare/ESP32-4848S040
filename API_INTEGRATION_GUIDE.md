# API Integration Guide

This guide explains how to integrate real-time data from external sources into your dashboard.

## Overview

The dashboard framework is designed to work with placeholder data initially, then easily transition to real API data. The `data_sources.h` file contains templates for each data source.

## Climate Data Integration

### Option 1: Local Sensor (DHT22)

**Hardware:**
- DHT22 sensor on GPIO 9

**Implementation:**

```c
// In data_sources.h, modify climate_init():

#include <DHT.h>

#define DHTPIN 9
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

void climate_init() {
  dht.begin();
  DEBUG_PRINTLN("DHT22 sensor initialized");
}

void climate_update() {
  climate_data.temperature = dht.readTemperature();
  climate_data.humidity = dht.readHumidity();
  climate_data.timestamp = millis();

  if (isnan(climate_data.temperature) || isnan(climate_data.humidity)) {
    DEBUG_PRINTLN("Failed to read DHT22!");
  }
}
```

### Option 2: OpenWeatherMap API

**Setup:**
1. Register at https://openweathermap.org/api
2. Get your API key
3. Enable WiFi in config.h

**Implementation:**

```c
// In data_sources.h:

#include <HTTPClient.h>
#include <ArduinoJson.h>

#define OWM_API_KEY "your_api_key"
#define OWM_CITY "London"
#define OWM_COUNTRY_CODE "UK"

void climate_update() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "https://api.openweathermap.org/data/2.5/weather?q=";
  url += OWM_CITY "," OWM_COUNTRY_CODE "&appid=" OWM_API_KEY "&units=metric";

  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    DynamicJsonDocument doc(1024);
    deserializeJson(doc, http.getString());

    climate_data.temperature = doc["main"]["temp"];
    climate_data.humidity = doc["main"]["humidity"];
    climate_data.timestamp = millis();
  } else {
    DEBUG_PRINT("OpenWeatherMap HTTP error: ");
    DEBUG_PRINTLN(httpCode);
  }
  http.end();
}
```

### Option 3: BME680 I2C

**Hardware:**
- BME680 on I2C (SDA: GPIO 15, SCL: GPIO 16)

**Implementation:**

```c
#include <Adafruit_BME680.h>

Adafruit_BME680 bme;

void climate_init() {
  if (!bme.begin(0x76)) {
    DEBUG_PRINTLN("BME680 initialization failed!");
  }
  // Set oversampling and filter initialization
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150); // 320°C for 150 ms
}

void climate_update() {
  if (!bme.performReading()) {
    DEBUG_PRINTLN("BME680 read failed!");
    return;
  }
  climate_data.temperature = bme.temperature;
  climate_data.humidity = bme.humidity;
  climate_data.timestamp = millis();
}
```

---

## BambuLab Printer Integration

### Bambu Studio API

**Setup:**
1. Get your printer's IP address
2. Generate access code from Bambu Studio settings
3. Printer must be connected to WiFi

**Implementation:**

```c
// In data_sources.h:

#include <HTTPClient.h>
#include <ArduinoJson.h>

#define BAMBU_IP "192.168.1.100"
#define BAMBU_ACCESS_CODE "your_access_code"

void printer_update() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = "http://" BAMBU_IP "/api/v1/printer";
  String auth = "Authorization: Bearer " BAMBU_ACCESS_CODE;

  http.begin(url);
  http.addHeader("Authorization", "Bearer " BAMBU_ACCESS_CODE);
  int httpCode = http.GET();

  if (httpCode == 200) {
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, http.getString());

    printer_data.is_printing = (doc["print"]["status"] == "printing");
    printer_data.progress = doc["print"]["progress"];
    printer_data.nozzle_temp = doc["print"]["nozzle_temper"];
    printer_data.nozzle_target_temp = doc["print"]["nozzle_target_temper"];
    printer_data.bed_temp = doc["print"]["bed_temper"];
    printer_data.bed_target_temp = doc["print"]["bed_target_temper"];
    printer_data.time_remaining = doc["print"]["mc_remaining_time"];
    printer_data.timestamp = millis();
  } else {
    DEBUG_PRINT("Bambu API error: ");
    DEBUG_PRINTLN(httpCode);
  }
  http.end();
}
```

---

## Proxmox Cluster Monitoring

### Proxmox API

**Setup:**
1. Generate API token in Proxmox UI (Datacenter → API Tokens)
2. Note token ID and secret
3. Note node names

**Implementation:**

```c
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define PROXMOX_HOST "192.168.1.50:8006"
#define PROXMOX_TOKEN_ID "root@pam!dashboard"
#define PROXMOX_TOKEN_SECRET "your_token_secret"

void proxmox_update() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String auth = "PVEAPIToken=" PROXMOX_TOKEN_ID ":" PROXMOX_TOKEN_SECRET;
  String url = "https://" PROXMOX_HOST "/api2/json/nodes";

  http.begin(url, PROXMOX_CA_CERT);  // Use CA cert for HTTPS
  http.addHeader("Authorization", auth);
  int httpCode = http.GET();

  if (httpCode == 200) {
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, http.getString());

    JsonArray nodes = doc["data"];
    int idx = 0;

    for (JsonObject node : nodes) {
      if (idx >= 3) break;

      strncpy(proxmox_data.nodes[idx].node_name, 
              node["node"].as<const char*>(), 31);
      proxmox_data.nodes[idx].is_online = (node["status"] == "online");
      proxmox_data.nodes[idx].cpu_usage = (node["cpu"] * 100);
      proxmox_data.nodes[idx].ram_usage = (node["mem"] * 100 / node["maxmem"]);
      idx++;
    }
    proxmox_data.node_count = idx;
  }
  http.end();
}
```

---

## Package/Delivery Tracking

### Multiple Carrier Support

```c
// In data_sources.h:

#include <HTTPClient.h>
#include <ArduinoJson.h>

// DHL API
void update_dhl_package(const char *tracking_num) {
  HTTPClient http;
  String url = "https://api.dhl.com/track/v1/tracking";
  url += "?trackingNumber=" + String(tracking_num);

  http.begin(url);
  http.addHeader("DHL-API-Key", "your_dhl_api_key");
  
  int httpCode = http.GET();
  if (httpCode == 200) {
    DynamicJsonDocument doc(2048);
    deserializeJson(doc, http.getString());
    
    // Update package status from JSON
    // doc["shipments"][0]["status"]["statusCode"]
    // doc["shipments"][0]["status"]["description"]
  }
  http.end();
}

// Example usage in vessel_update():
void vessel_update() {
  if (WiFi.status() != WL_CONNECTED) return;

  for (int i = 0; i < vessel_data.package_count; i++) {
    if (strlen(vessel_data.packages[i].tracking_number) > 0) {
      // Determine carrier and call appropriate function
      if (strncmp(vessel_data.packages[i].carrier, "DHL", 3) == 0) {
        update_dhl_package(vessel_data.packages[i].tracking_number);
      }
    }
  }
}
```

---

## All-in-One Backend (Google Apps Script)

If you'd rather not wire up carrier/stock/weather/flight APIs individually, **[google_apps_script/](google_apps_script/)** has a ready-to-paste Google Apps Script that aggregates package tracking (pkge.net), stocks, Google Calendar, MakerWorld stats, flight status (AviationStack), and weather (Open-Meteo) behind a single deployable URL, with caching and rate-limiting built in. See its [README](google_apps_script/README.md) for setup.

---

## WiFi Integration

### Basic WiFi Setup

```c
// In config.h:
#define ENABLE_WIFI
#define WIFI_SSID "your_ssid"
#define WIFI_PASSWORD "your_password"

// In main sketch setup():

#ifdef ENABLE_WIFI
  #include <WiFi.h>
  
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);
  
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed!");
  }
#endif
```

### HTTPS/SSL Certificates

```c
// For secure connections, include CA certificate

const char* root_ca = R"(-----BEGIN CERTIFICATE-----
MIIDdTCCAl2gAwIBAgILBAAAAAAAG7DpgJEwDQYJKoZIhvcNAQEFBQAwVzELMAkG
...certificate content...
-----END CERTIFICATE-----)";

// Use in HTTPClient:
http.begin(url, root_ca);
```

---

## Updating UI Elements with Real Data

### Modifying Screen Widgets

```c
// Store label pointers for updating:
static lv_obj_t *temp_label = NULL;
static lv_obj_t *humidity_label = NULL;
static lv_obj_t *printer_progress = NULL;

// In create_climate_screen():
temp_label = lv_label_create(temp_container);
// ... rest of creation

// In climate_update(), after fetching data:
void climate_update() {
  climate_data.temperature = read_temp();
  climate_data.humidity = read_humidity();

  // Update display
  if (temp_label) {
    char buf[16];
    format_temperature(buf, climate_data.temperature);
    lv_label_set_text(temp_label, buf);
  }
  if (humidity_label) {
    char buf[16];
    format_humidity(buf, climate_data.humidity);
    lv_label_set_text(humidity_label, buf);
  }
}
```

---

## Error Handling

```c
typedef enum {
  API_OK,
  API_CONNECTION_FAILED,
  API_TIMEOUT,
  API_INVALID_RESPONSE,
  API_UNAUTHORIZED,
  API_RATE_LIMITED
} APIStatus;

APIStatus handle_http_error(int httpCode) {
  switch (httpCode) {
    case 200: return API_OK;
    case 401: return API_UNAUTHORIZED;
    case 429: return API_RATE_LIMITED;
    case -1: return API_CONNECTION_FAILED;
    default: return API_INVALID_RESPONSE;
  }
}
```

---

## Rate Limiting & Caching

```c
// Add to data_sources.h:

typedef struct {
  uint32_t last_update;
  uint32_t cache_ttl;
  bool is_stale;
} CacheEntry;

bool should_update(CacheEntry *cache) {
  if (millis() - cache->last_update > cache->cache_ttl) {
    cache->last_update = millis();
    return true;
  }
  cache->is_stale = false;
  return false;
}
```

---

## Testing Without WiFi

During development, use the placeholder data in `data_sources.h`:

```c
// All update functions can be tested with static values
void climate_update() {
  // For testing, just use default values
  // climate_data.temperature = 22.5f;  // Already set in struct init
  
  // Once WiFi is enabled, replace with:
  // climate_data.temperature = read_from_api();
}
```

---

## Performance Considerations

- **Update Intervals:** Set in config.h, default 5-10 seconds
- **Request Timeout:** Keep HTTP requests under 5 seconds
- **Memory:** Limit JSON doc size to available heap
- **Heap Monitoring:** Use `print_heap_info()` from utils.h

---

## Useful Resources

- ArduinoJSON: https://arduinojson.org/
- HTTPClient Docs: https://github.com/espressif/arduino-esp32/tree/master/libraries/HTTPClient
- BambuLab API: https://wiki.bambu.tech/en/Software/BambuConnect_MQTTGuide
- Proxmox API: https://pve.proxmox.com/pve-docs/api-viewer/
- DHL API: https://developer.dhl.com/
- OpenWeatherMap: https://openweathermap.org/api

Start with placeholder data, then gradually enable each API integration as needed!

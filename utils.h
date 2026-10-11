/*******************************************************************************
 * Utility Functions and Helpers
 ******************************************************************************/

#ifndef __UTILS_H__
#define __UTILS_H__

#include <Arduino.h>
#include <time.h>

// ==================== String Formatting ====================

void format_temperature(char *buf, float temp) {
  snprintf(buf, 16, "%.1f°C", temp);
}

void format_humidity(char *buf, float humidity) {
  snprintf(buf, 16, "%.0f%%", humidity);
}

void format_time_remaining(char *buf, uint32_t seconds) {
  uint32_t hours = seconds / 3600;
  uint32_t minutes = (seconds % 3600) / 60;
  snprintf(buf, 32, "%luh %um", hours, minutes);
}

void format_percentage(char *buf, uint8_t percent) {
  snprintf(buf, 16, "%d%%", percent);
}

void format_date(char *buf, uint32_t timestamp) {
  // Simple date formatter - implement with actual RTC if available
  // For now, formats as "YYYY-MM-DD"
  // TODO: Add RTC support for accurate dates
  snprintf(buf, 32, "2026-08-15");
}

void format_time(char *buf, uint32_t timestamp) {
  // Simple time formatter
  // TODO: Add RTC support for accurate time
  uint32_t seconds = (millis() / 1000) % 86400;
  uint32_t hours = seconds / 3600;
  uint32_t minutes = (seconds % 3600) / 60;
  uint32_t secs = seconds % 60;
  snprintf(buf, 16, "%02lu:%02lu:%02lu", hours, minutes, secs);
}

// ==================== Color Utilities ====================

uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

uint32_t rgb888_to_hex(uint8_t r, uint8_t g, uint8_t b) {
  return (r << 16) | (g << 8) | b;
}

// ==================== Validation ====================

bool is_valid_temp(float temp) {
  return temp >= -50 && temp <= 150;
}

bool is_valid_humidity(float humidity) {
  return humidity >= 0 && humidity <= 100;
}

bool is_valid_percentage(uint8_t percent) {
  return percent <= 100;
}

// ==================== Math Utilities ====================

float lerp(float a, float b, float t) {
  return a + (b - a) * t;
}

uint8_t clamp_u8(uint8_t val, uint8_t min_val, uint8_t max_val) {
  return val < min_val ? min_val : (val > max_val ? max_val : val);
}

float clamp_f(float val, float min_val, float max_val) {
  return val < min_val ? min_val : (val > max_val ? max_val : val);
}

// ==================== Checksum & Validation ====================

uint32_t simple_crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    uint8_t byte = data[i];
    for (int j = 0; j < 8; j++) {
      uint8_t bit = (byte ^ crc) & 1;
      crc >>= 1;
      if (bit) crc ^= 0xEDB88320;
      byte >>= 1;
    }
  }
  return crc ^ 0xFFFFFFFF;
}

// ==================== Memory Utilities ====================

void print_heap_info() {
  Serial.print("Free Heap: ");
  Serial.print(ESP.getFreeHeap());
  Serial.print(" bytes | Largest Free Block: ");
  Serial.print(ESP.getMaxAllocHeap());
  Serial.println(" bytes");
}

// ==================== Button Debouncing ====================

typedef struct {
  uint8_t pin;
  uint8_t state;
  uint8_t last_state;
  uint32_t last_change_time;
  uint32_t debounce_delay;
  bool (*on_pressed)(void);
  bool (*on_released)(void);
} Button;

Button create_button(uint8_t pin, uint32_t debounce_ms) {
  Button btn;
  btn.pin = pin;
  btn.state = HIGH;
  btn.last_state = HIGH;
  btn.last_change_time = 0;
  btn.debounce_delay = debounce_ms;
  btn.on_pressed = NULL;
  btn.on_released = NULL;
  pinMode(pin, INPUT_PULLUP);
  return btn;
}

bool update_button(Button *btn) {
  uint8_t reading = digitalRead(btn->pin);

  if (reading != btn->last_state) {
    btn->last_change_time = millis();
  }

  if ((millis() - btn->last_change_time) > btn->debounce_delay) {
    if (reading != btn->state) {
      btn->state = reading;

      if (btn->state == LOW && btn->on_pressed) {
        return btn->on_pressed();
      } else if (btn->state == HIGH && btn->on_released) {
        return btn->on_released();
      }
    }
  }

  btn->last_state = reading;
  return false;
}

// ==================== Performance Monitoring ====================

typedef struct {
  uint32_t loop_count;
  uint32_t last_report_time;
  float avg_loop_time_ms;
  float max_loop_time_ms;
  uint32_t last_loop_time_us;
} PerformanceMonitor;

static PerformanceMonitor perf_monitor = {
  .loop_count = 0,
  .last_report_time = 0,
  .avg_loop_time_ms = 0,
  .max_loop_time_ms = 0,
  .last_loop_time_us = 0
};

void perf_monitor_start() {
  perf_monitor.last_loop_time_us = micros();
}

void perf_monitor_end() {
  uint32_t loop_time_us = micros() - perf_monitor.last_loop_time_us;
  float loop_time_ms = loop_time_us / 1000.0f;

  perf_monitor.avg_loop_time_ms = (perf_monitor.avg_loop_time_ms * 0.9f) + (loop_time_ms * 0.1f);
  perf_monitor.max_loop_time_ms = max(perf_monitor.max_loop_time_ms, loop_time_ms);
  perf_monitor.loop_count++;

  if (millis() - perf_monitor.last_report_time >= 5000) {
    Serial.print("Perf - Avg Loop: ");
    Serial.print(perf_monitor.avg_loop_time_ms, 2);
    Serial.print("ms | Max: ");
    Serial.print(perf_monitor.max_loop_time_ms, 2);
    Serial.print("ms | FPS: ");
    Serial.println(1000.0f / perf_monitor.avg_loop_time_ms, 1);

    perf_monitor.max_loop_time_ms = 0;
    perf_monitor.last_report_time = millis();
  }
}

// ==================== Error Handling ====================

typedef enum {
  ERR_NONE = 0,
  ERR_DISPLAY_INIT,
  ERR_TOUCH_INIT,
  ERR_SENSOR_INIT,
  ERR_WIFI_CONNECT,
  ERR_API_REQUEST,
  ERR_MEMORY_ALLOC,
  ERR_UNKNOWN
} ErrorCode;

const char* error_to_string(ErrorCode err) {
  switch (err) {
    case ERR_NONE: return "No error";
    case ERR_DISPLAY_INIT: return "Display initialization failed";
    case ERR_TOUCH_INIT: return "Touch initialization failed";
    case ERR_SENSOR_INIT: return "Sensor initialization failed";
    case ERR_WIFI_CONNECT: return "WiFi connection failed";
    case ERR_API_REQUEST: return "API request failed";
    case ERR_MEMORY_ALLOC: return "Memory allocation failed";
    case ERR_UNKNOWN: return "Unknown error";
    default: return "Undefined error";
  }
}

void report_error(ErrorCode err, const char *context = NULL) {
  Serial.print("ERROR [");
  Serial.print(error_to_string(err));
  Serial.print("]");
  if (context) {
    Serial.print(" in ");
    Serial.print(context);
  }
  Serial.println();
}

#endif // __UTILS_H__

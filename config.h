/*******************************************************************************
 * Dashboard Configuration Header
 * Customize display pins, colors, and behavior here
 ******************************************************************************/

#ifndef __CONFIG_H__
#define __CONFIG_H__

// ==================== Display Pins ====================
// Backlight control pin
#define BACKLIGHT_PIN 38
#define BACKLIGHT_PWM_CHANNEL 0

// ==================== Color Scheme ====================
// Primary accent color (Cyan)
#define COLOR_PRIMARY_RGB 0x00, 0xBC, 0xD4

// Secondary color (Orange)
#define COLOR_SECONDARY_RGB 0xFF, 0x98, 0x00

// Success color (Green)
#define COLOR_SUCCESS_RGB 0x4C, 0xAF, 0x50

// Warning color (Yellow)
#define COLOR_WARNING_RGB 0xFF, 0xC1, 0x07

// Error color (Red)
#define COLOR_ERROR_RGB 0xF4, 0x43, 0x36

// Dark background
#define COLOR_BG_RGB 0x12, 0x12, 0x12

// Light background
#define COLOR_BG_LIGHT_RGB 0x1E, 0x1E, 0x1E

// ==================== Screen Navigation ====================
// Buttons to change screens
#define PREV_SCREEN_BTN 7
#define NEXT_SCREEN_BTN 6

// ==================== Update Intervals (milliseconds) ====================
#define CLIMATE_UPDATE_INTERVAL 5000      // Update climate data every 5s
#define PRINTER_UPDATE_INTERVAL 2000      // Update printer data every 2s
#define PROXMOX_UPDATE_INTERVAL 3000      // Update Proxmox data every 3s

// ==================== API Configuration ====================
// Uncomment to enable network features
// #define ENABLE_WIFI
// #define WIFI_SSID "YOUR_SSID"
// #define WIFI_PASSWORD "YOUR_PASSWORD"

// #define ENABLE_BAMBU_API
// #define BAMBU_PRINTER_IP "192.168.1.100"
// #define BAMBU_ACCESS_CODE "YOUR_ACCESS_CODE"

// #define ENABLE_PROXMOX_API
// #define PROXMOX_HOST "proxmox.local"
// #define PROXMOX_USER "root@pam"
// #define PROXMOX_PASSWORD "YOUR_PASSWORD"

// ==================== Display Settings ====================
#define DISPLAY_WIDTH 480
#define DISPLAY_HEIGHT 480
#define DISPLAY_REFRESH_RATE 60  // Target refresh rate in Hz

// ==================== Debug ====================
#define DEBUG_SERIAL 1
#if DEBUG_SERIAL
#define DEBUG_PRINT(x) Serial.print(x)
#define DEBUG_PRINTLN(x) Serial.println(x)
#else
#define DEBUG_PRINT(x)
#define DEBUG_PRINTLN(x)
#endif

#endif // __CONFIG_H__

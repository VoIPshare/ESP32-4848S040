// Re-aligns the ESP32-S3 RGB panel after a bounce-buffer underrun.
//
// The panel streams the frame buffer from PSRAM through small internal
// "bounce" buffers refilled by an interrupt. When PSRAM is busy (Wi-Fi/TLS,
// LVGL drawing) a refill can come too late, and the picture then stays
// shifted by one bounce buffer (40 lines here) until the LCD DMA is
// restarted. esp_lcd_rgb_panel_restart() does that at the next VSYNC, so
// calling it while the picture is aligned changes nothing on screen.
//
// Arduino_GFX keeps the panel handle private and has no getter; the
// explicit template instantiation below is the standard-conforming way to
// read a private member (access checks don't apply to explicit
// instantiations), so the library itself stays unmodified.
#include <Arduino_GFX_Library.h>

#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include <esp_lcd_panel_rgb.h>
#include <esp_timer.h>
#include <stdio.h>
#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <soc/gpio_sig_map.h>

namespace
{
template <typename Tag, typename Tag::type Member>
struct PrivateAccess
{
  friend typename Tag::type private_member(Tag) { return Member; }
};

struct RgbPanelHandle
{
  typedef esp_lcd_panel_handle_t Arduino_ESP32RGBPanel::*type;
  friend type private_member(RgbPanelHandle);
};

template struct PrivateAccess<RgbPanelHandle, &Arduino_ESP32RGBPanel::_panel_handle>;
} // namespace

void rgb_panel_resync(Arduino_ESP32RGBPanel *panel)
{
  if (!panel) return;
  esp_lcd_panel_handle_t handle = panel->*private_member(RgbPanelHandle());
  if (handle) esp_lcd_rgb_panel_restart(handle);
}

// ---- Diagnostics: is the stream in step with the panel? ----
// Two driver events per frame: "frame buffer complete" (the last line has
// gone into a bounce buffer) and VSYNC (the panel starts a new frame). In
// step, they're a fixed time apart; a stream shifted by N lines moves that
// gap by N line times (~55 us at 10 MHz). The gap from the first seconds
// after boot - picture still aligned - is kept as the reference.
static volatile uint32_t diag_vsync = 0, diag_fb_done = 0;
static volatile int64_t diag_fb_done_us = 0, diag_gap_us = 0, diag_ref_gap_us = 0;
static volatile uint32_t diag_ref_samples = 0;
static volatile uint32_t diag_auto_restarts = 0;

// True once the boot reference is in and the stream is more than 5 lines
// (~55 us each) away from it - then a restart is worth doing; otherwise
// leave the stream alone (a restart briefly disturbs the signal).
bool rgb_panel_stream_offset()
{
  if (diag_ref_samples < 50 || esp_timer_get_time() < 10000000) return false;
  int64_t d = diag_gap_us - diag_ref_gap_us;
  return d > 5 * 55 || d < -5 * 55;
}

// rgb_resync_timer_cb(): restart only when the stream is measured out of step.
void rgb_panel_resync_if_needed(Arduino_ESP32RGBPanel *panel)
{
  if (!rgb_panel_stream_offset()) return;
  diag_auto_restarts++;
  rgb_panel_resync(panel);
}

// Stops the pixel clock for ms (the panel loses its signal, as during a
// reboot), then gives it back to the LCD controller. Blocks for ms.
void rgb_panel_signal_pause(int pclk_gpio, uint32_t ms)
{
  esp_rom_gpio_connect_out_signal(pclk_gpio, SIG_GPIO_OUT_IDX, false, false);
  gpio_set_direction((gpio_num_t)pclk_gpio, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)pclk_gpio, 0);
  vTaskDelay(pdMS_TO_TICKS(ms));
  esp_rom_gpio_connect_out_signal(pclk_gpio, LCD_PCLK_IDX, false, false);
}

static bool IRAM_ATTR diag_on_vsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *)
{
  diag_vsync++;
  int64_t now = esp_timer_get_time();
  if (diag_fb_done_us)
  {
    int64_t gap = now - diag_fb_done_us;
    diag_gap_us = gap;
    // Reference: average of the frames between 3 and 10 s after boot.
    if (now > 3000000 && now < 10000000)
    {
      diag_ref_gap_us = (diag_ref_gap_us * diag_ref_samples + gap) / (diag_ref_samples + 1);
      diag_ref_samples++;
    }
  }
  return false;
}

static bool IRAM_ATTR diag_on_fb_done(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *)
{
  diag_fb_done++;
  diag_fb_done_us = esp_timer_get_time();
  return false;
}

bool rgb_panel_diag_start(Arduino_ESP32RGBPanel *panel)
{
  if (!panel) return false;
  esp_lcd_panel_handle_t handle = panel->*private_member(RgbPanelHandle());
  if (!handle) return false;
  esp_lcd_rgb_panel_event_callbacks_t cbs = {};
  cbs.on_vsync = diag_on_vsync;
  cbs.on_frame_buf_complete = diag_on_fb_done;
  return esp_lcd_rgb_panel_register_event_callbacks(handle, &cbs, NULL) == ESP_OK;
}

// One line for the setup page. Waits a second to count the events.
void rgb_panel_diag(Arduino_ESP32RGBPanel *panel, char *out, size_t n)
{
  esp_lcd_panel_handle_t handle = panel ? panel->*private_member(RgbPanelHandle()) : NULL;
  uint32_t v0 = diag_vsync, f0 = diag_fb_done;
  vTaskDelay(pdMS_TO_TICKS(1000));
  uint32_t v = diag_vsync - v0, f = diag_fb_done - f0;
  esp_err_t r = handle ? esp_lcd_rgb_panel_restart(handle) : ESP_ERR_INVALID_ARG;
  long gap = (long)diag_gap_us, ref = (long)diag_ref_gap_us;
  snprintf(out, n,
           "VSYNC: %lu/s, frame done: %lu/s\nGap frame done -> VSYNC: %ld us (at boot %ld us, %lu frames)\n"
           "Difference: %ld us = about %ld lines\nStream restart call: %s\nAutomatic restarts since boot: %lu",
           (unsigned long)v, (unsigned long)f, gap, ref, (unsigned long)diag_ref_samples, gap - ref, (gap - ref) / 55,
           r == ESP_OK ? "ESP_OK" : esp_err_to_name(r), (unsigned long)diag_auto_restarts);
}

// Stronger: resets the LCD controller and sets it up again - the same two
// calls Arduino_GFX makes at boot (the frame buffer is kept). For when the
// restart above doesn't bring the picture back. Returns false on an error.
bool rgb_panel_full_reset(Arduino_ESP32RGBPanel *panel)
{
  if (!panel) return false;
  esp_lcd_panel_handle_t handle = panel->*private_member(RgbPanelHandle());
  if (!handle) return false;
  return esp_lcd_panel_reset(handle) == ESP_OK && esp_lcd_panel_init(handle) == ESP_OK;
}
#endif

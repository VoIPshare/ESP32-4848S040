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

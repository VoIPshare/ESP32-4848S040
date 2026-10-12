#ifndef __GH_UPDATE_H__
#define __GH_UPDATE_H__

// Firmware updates straight from this project's GitHub releases (the ones
// .github/workflows/compile.yml publishes). TLS is verified against pinned
// root CAs, so nobody in between can push their own firmware. Blocking:
// call from network_task only - never touches LVGL.

#include <Arduino.h>

#define GH_REPO "VoIPshare/ESP32-4848S040"
#define GH_ASSET "ESP32S3-4848S040-ESP32-4848S040.ino.bin"

// Version of the latest release ("3.7" from tag "firmware-3.7"). False on
// any network/HTTP error.
bool gh_latest_version(char *out, size_t n);

// True when version a is newer than b ("3.10" > "3.9").
bool gh_version_newer(const char *a, const char *b);

// Downloads and flashes that release's firmware, then restarts. Only
// returns on failure, with a short reason in err. progress(done, total)
// is called while it downloads.
bool gh_install(const char *version, void (*progress)(int, int), char *err, size_t errlen);

#endif

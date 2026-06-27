#pragma once
#include <FastLED.h>
#include <ESPAsyncWebServer.h>

// ---------------------------------------------------------------------------
// spriteplay.h — User-uploadable PPM sprite animator
//
// Upload up to 16 frames as P6 binary PPM files (16x16 pixels) via the web UI.
// Optional PGM sidecar files (same basename, .pgm extension) provide alpha masks.
// If no PGM is present, all pixels are treated as fully opaque.
//
// Files stored in SPIFFS under /sprites/frame_00.ppm, /sprites/frame_00.pgm etc.
// Frame delay and loop count stored in NVS via Preferences.
// ---------------------------------------------------------------------------

void spriteplaySetup(AsyncWebServer* server);
void spriteplay(CRGB* leds);

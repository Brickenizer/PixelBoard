#pragma once
#include <FastLED.h>
#include <ESPAsyncWebServer.h>

// ---------------------------------------------------------------------------
// pacman.h — Pac-Man chase animation pattern
//
// Two sub-modes (toggled via web UI or cycling automatically):
//
//   MODE_PACMAN_CHASES  — Pac-Man chases 1-4 blue ghosts and optionally a beer
//   MODE_GHOSTS_CHASE   — 1-4 coloured ghosts + optional chicken chase Pac-Man
//
// Between each chase sequence a configurable scrolling text message is shown.
// All settings are persisted in NVS via Preferences.
// ---------------------------------------------------------------------------

void pacmanSetup(AsyncWebServer* server);
void pacman(CRGB* leds);

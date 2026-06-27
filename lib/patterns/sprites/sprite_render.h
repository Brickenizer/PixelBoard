#pragma once
#include <FastLED.h>
#include <stdint.h>
#include "led_display.h"

// ---------------------------------------------------------------------------
// blitSprite8x8 — blit one 8x8 frame from the Piskel ABGR sprite sheet
//
// Piskel exports uint32 as ABGR little-endian: 0xAABBGGRR
//   low byte  = R
//   byte 1    = G
//   byte 2    = B
//   high byte = A
//
// Sprites are NEVER rotated or mirrored. They are drawn exactly as stored.
// destX, destY: top-left corner of the 8x8 sprite on the 16x16 matrix.
// ---------------------------------------------------------------------------
inline void blitSprite8x8(CRGB* leds,
                           const uint32_t* frameData,
                           int destX, int destY,
                           CRGB bg = CRGB::Black)
{
    for (int row = 0; row < 8; row++) {
        int ly = destY + row;
        if (ly < 0 || ly >= 16) continue;
        for (int col = 0; col < 8; col++) {
            int lx = destX + col;
            if (lx < 0 || lx >= 16) continue;

            uint32_t abgr = frameData[row * 8 + col];
            uint8_t  a    = (abgr >> 24) & 0xFF;
            uint8_t  r    = (abgr      ) & 0xFF;
            uint8_t  g    = (abgr >>  8) & 0xFF;
            uint8_t  b    = (abgr >> 16) & 0xFF;

            int idx = XY(lx, ly);

            if (a == 0) {
                continue;
            } else if (a == 255) {
                leds[idx] = CRGB(r, g, b);
            } else {
                uint8_t inv = 255 - a;
                leds[idx] = CRGB(
                    ((uint16_t)r * a + (uint16_t)bg.r * inv) >> 8,
                    ((uint16_t)g * a + (uint16_t)bg.g * inv) >> 8,
                    ((uint16_t)b * a + (uint16_t)bg.b * inv) >> 8
                );
            }
        }
    }
}

// ---------------------------------------------------------------------------
// spriteplay.cpp — Upload and animate user-defined 16x16 PPM sprite sequences
//
// PPM format expected: P6 binary, 16x16, maxval 255.
// PGM format expected: P5 binary, 16x16, maxval 255 (optional alpha mask).
// Files stored in SPIFFS: /sprites/frame_00.ppm .. /sprites/frame_15.ppm
//                          /sprites/frame_00.pgm .. /sprites/frame_15.pgm
// Settings (frameDelay, frameCount) stored in NVS.
// ---------------------------------------------------------------------------

#include "spriteplay.h"
#include "sprite_render.h"
#include "led_display.h"
#include <FastLED.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <Arduino.h>

static const uint8_t  MAX_FRAMES   = 16;
static const uint16_t SPRITE_W     = 16;
static const uint16_t SPRITE_H     = 16;
static const uint16_t PIXELS       = SPRITE_W * SPRITE_H; // 256

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
static uint16_t g_frameDelay  = 100;   // ms per frame
static uint8_t  g_frameCount  = 0;     // how many frames are stored
static bool     g_initialized = false;

static void loadSpriteSettings() {
    Preferences p;
    p.begin("spriteplay", true);
    g_frameDelay = p.getUShort("delay", 100);
    g_frameCount = p.getUChar ("frames", 0);
    p.end();
}

static void saveSpriteSettings() {
    Preferences p;
    p.begin("spriteplay", false);
    p.putUShort("delay",  g_frameDelay);
    p.putUChar ("frames", g_frameCount);
    p.end();
}

// ---------------------------------------------------------------------------
// PPM/PGM parser
// Reads a P6 (PPM) or P5 (PGM) binary file from SPIFFS.
// Skips the ASCII header (handles comment lines starting with #).
// Returns true on success. outBuf must be PIXELS*3 bytes (PPM) or PIXELS (PGM).
// ---------------------------------------------------------------------------
static bool parsePNM(const char* path, uint8_t* outBuf, size_t outLen,
                     char expectedType)   // '6' for PPM, '5' for PGM
{
    File f = SPIFFS.open(path, "r");
    if (!f) return false;

    // Read magic "P5" or "P6"
    char magic[3] = {0};
    f.read((uint8_t*)magic, 2);
    if (magic[0] != 'P' || magic[1] != ('0' + expectedType)) {
        f.close(); return false;
    }

    // Parse header tokens (width, height, maxval) skipping whitespace/comments
    auto skipWS = [&]() {
        while (f.available()) {
            char c = (char)f.peek();
            if (c == '#') {
                while (f.available() && (char)f.read() != '\n') {}
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                f.read();
            } else break;
        }
    };
    auto readInt = [&]() -> int {
        skipWS();
        int val = 0;
        while (f.available()) {
            char c = (char)f.peek();
            if (c < '0' || c > '9') break;
            val = val * 10 + (f.read() - '0');
        }
        return val;
    };

    int w      = readInt();
    int h      = readInt();
    int maxval = readInt();
    // Consume exactly one whitespace after maxval (spec requires it)
    if (f.available()) f.read();

    if (w != SPRITE_W || h != SPRITE_H || maxval != 255) {
        Serial.printf("[Sprite] %s: expected %dx%d/255, got %dx%d/%d\n",
                      path, SPRITE_W, SPRITE_H, w, h, maxval);
        f.close(); return false;
    }

    size_t got = f.read(outBuf, outLen);
    f.close();
    return (got == outLen);
}

// ---------------------------------------------------------------------------
// Render one frame onto the LED array.
// Loads PPM (and optional PGM) from SPIFFS each call — small enough to be fast.
// For a production version you could cache in PSRAM if available.
// ---------------------------------------------------------------------------
static void renderFrame(CRGB* leds, uint8_t frameIdx) {
    char ppmPath[32], pgmPath[32];
    snprintf(ppmPath, sizeof(ppmPath), "/sprites/frame_%02d.ppm", frameIdx);
    snprintf(pgmPath, sizeof(pgmPath), "/sprites/frame_%02d.pgm", frameIdx);

    static uint8_t rgbBuf[PIXELS * 3];  // 768 bytes
    static uint8_t alphaBuf[PIXELS];    // 256 bytes

    if (!parsePNM(ppmPath, rgbBuf, sizeof(rgbBuf), '6')) {
        // Frame missing or corrupt — show a magenta error pixel at top-left
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        leds[XY(0, 0)] = CRGB::Magenta;
        return;
    }

    bool hasAlpha = parsePNM(pgmPath, alphaBuf, sizeof(alphaBuf), '5');

    for (uint16_t i = 0; i < PIXELS; i++) {
        uint8_t r = rgbBuf[i * 3 + 0];
        uint8_t g = rgbBuf[i * 3 + 1];
        uint8_t b = rgbBuf[i * 3 + 2];
        uint8_t a = hasAlpha ? alphaBuf[i] : 255;

        uint8_t px = i % SPRITE_W;
        uint8_t py = i / SPRITE_W;
        int     idx = XY(px, py);

        if (a == 0) {
            leds[idx] = CRGB::Black;
        } else if (a == 255) {
            leds[idx] = CRGB(r, g, b);
        } else {
            uint8_t inv = 255 - a;
            leds[idx].r = ((uint16_t)r * a + (uint16_t)leds[idx].r * inv) >> 8;
            leds[idx].g = ((uint16_t)g * a + (uint16_t)leds[idx].g * inv) >> 8;
            leds[idx].b = ((uint16_t)b * a + (uint16_t)leds[idx].b * inv) >> 8;
        }
    }
}

// ---------------------------------------------------------------------------
// Public pattern function
// ---------------------------------------------------------------------------
void spriteplay(CRGB* leds) {
    if (!g_initialized) {
        loadSpriteSettings();
        g_initialized = true;
    }

    if (g_frameCount == 0) {
        // No frames uploaded yet — show a placeholder pattern
        static uint8_t hue = 0;
        fill_solid(leds, NUM_LEDS, CHSV(hue++, 200, 60));
        FastLED.show();
        delay(50);
        return;
    }

    static uint8_t  currentFrame = 0;
    static uint32_t lastFrameTime = 0;

    uint32_t now = millis();
    if (now - lastFrameTime >= g_frameDelay) {
        renderFrame(leds, currentFrame);
        FastLED.show();
        currentFrame    = (currentFrame + 1) % g_frameCount;
        lastFrameTime   = now;
    }
}

// ---------------------------------------------------------------------------
// Web endpoints
// ---------------------------------------------------------------------------
void spriteplaySetup(AsyncWebServer* server) {
    loadSpriteSettings();

    if (!SPIFFS.exists("/sprites")) {
        SPIFFS.mkdir("/sprites");
    }

    // GET /sprites — list uploaded frames as JSON
    server->on("/sprites", HTTP_GET, [](AsyncWebServerRequest* req) {
        String json = "{\"frames\":[";
        for (uint8_t i = 0; i < g_frameCount; i++) {
            if (i) json += ",";
            char pgmPath[32];
            snprintf(pgmPath, sizeof(pgmPath), "/sprites/frame_%02d.pgm", i);
            bool hasAlpha = SPIFFS.exists(pgmPath);
            json += "{\"idx\":" + String(i) +
                    ",\"ppm\":\"/sprites/frame_" + (i < 10 ? "0" : "") + String(i) + ".ppm\"" +
                    ",\"alpha\":" + (hasAlpha ? "true" : "false") + "}";
        }
        json += "],\"delay\":" + String(g_frameDelay) +
                ",\"frameCount\":" + String(g_frameCount) + "}";
        req->send(200, "application/json", json);
    });

    // POST /sprites/upload?type=ppm&frame=N  — upload a PPM or PGM file
    server->on("/sprites/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            req->send(200, "text/plain", "OK");
        },
        [](AsyncWebServerRequest* req, const String& filename,
           size_t index, uint8_t* data, size_t len, bool final)
        {
            static File uploadFile;
            static uint8_t uploadFrame = 0;
            static bool    uploadIsPGM = false;

            if (index == 0) {
                // First chunk — open the file
                uploadFrame = 0;
                uploadIsPGM = false;
                if (req->hasParam("frame"))
                    uploadFrame = (uint8_t)req->getParam("frame")->value().toInt();
                if (req->hasParam("type"))
                    uploadIsPGM = (req->getParam("type")->value() == "pgm");

                char path[32];
                snprintf(path, sizeof(path), "/sprites/frame_%02d.%s",
                         uploadFrame, uploadIsPGM ? "pgm" : "ppm");
                uploadFile = SPIFFS.open(path, "w");
                if (!uploadFile) {
                    Serial.printf("[Sprite] Failed to open %s for write\n", path);
                }
            }

            if (uploadFile) uploadFile.write(data, len);

            if (final && uploadFile) {
                uploadFile.close();
                // Update frame count if this is a new PPM frame
                if (!uploadIsPGM && uploadFrame >= g_frameCount) {
                    g_frameCount = uploadFrame + 1;
                    saveSpriteSettings();
                }
                Serial.printf("[Sprite] Frame %d %s uploaded OK\n",
                              uploadFrame, uploadIsPGM ? "PGM" : "PPM");
            }
        }
    );

    // GET /sprites/delete?frame=N — delete a frame
    server->on("/sprites/delete", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("frame")) {
            req->send(400, "text/plain", "Missing frame param");
            return;
        }
        uint8_t fi = (uint8_t)req->getParam("frame")->value().toInt();
        char ppm[32], pgm[32];
        snprintf(ppm, sizeof(ppm), "/sprites/frame_%02d.ppm", fi);
        snprintf(pgm, sizeof(pgm), "/sprites/frame_%02d.pgm", fi);
        SPIFFS.remove(ppm);
        SPIFFS.remove(pgm);
        // Compact: renumber remaining frames
        uint8_t dst = fi;
        for (uint8_t src = fi + 1; src < g_frameCount; src++, dst++) {
            char srcP[32], dstP[32], srcG[32], dstG[32];
            snprintf(srcP, sizeof(srcP), "/sprites/frame_%02d.ppm", src);
            snprintf(dstP, sizeof(dstP), "/sprites/frame_%02d.ppm", dst);
            snprintf(srcG, sizeof(srcG), "/sprites/frame_%02d.pgm", src);
            snprintf(dstG, sizeof(dstG), "/sprites/frame_%02d.pgm", dst);
            // SPIFFS rename = copy + delete
            File sf = SPIFFS.open(srcP, "r");
            File df = SPIFFS.open(dstP, "w");
            if (sf && df) { while (sf.available()) df.write(sf.read()); }
            if (sf) sf.close(); if (df) df.close();
            SPIFFS.remove(srcP);
            if (SPIFFS.exists(srcG)) {
                File sg = SPIFFS.open(srcG, "r");
                File dg = SPIFFS.open(dstG, "w");
                if (sg && dg) { while (sg.available()) dg.write(sg.read()); }
                if (sg) sg.close(); if (dg) dg.close();
                SPIFFS.remove(srcG);
            }
        }
        if (g_frameCount > 0) g_frameCount--;
        saveSpriteSettings();
        req->send(200, "text/plain", "Deleted");
    });

    // GET /sprites/delay?value=N — set frame delay in ms
    server->on("/sprites/delay", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (req->hasParam("value")) {
            g_frameDelay = (uint16_t)constrain(
                req->getParam("value")->value().toInt(), 10, 10000);
            saveSpriteSettings();
        }
        req->send(200, "text/plain", "OK");
    });

    // Serve the sprite management UI
    server->on("/sprites/ui", HTTP_GET, [](AsyncWebServerRequest* req) {
        String html = R"html(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sprite Animator</title>
<link rel="stylesheet" href="/style.css">
<style>
  .frame-list { display:flex; flex-wrap:wrap; gap:8px; margin:12px 0; }
  .frame-card { border:1px solid #444; padding:8px; border-radius:6px; text-align:center; }
  .frame-card canvas { display:block; margin:0 auto 4px; image-rendering:pixelated; }
</style>
</head><body>
<h2>🖼️ Sprite Animator</h2>

<div>
  <label>Frame delay (ms):
    <input type="number" id="delay" min="10" max="10000" value="100">
    <button onclick="setDelay()">Set</button>
  </label>
</div><br>

<h3>Upload Frame</h3>
<label>Frame number (0-15): <input type="number" id="upFrame" min="0" max="15" value="0"></label><br>
<label>PPM file: <input type="file" id="ppmFile" accept=".ppm"></label>
<button onclick="upload('ppm')">Upload PPM</button><br>
<label>PGM alpha mask (optional): <input type="file" id="pgmFile" accept=".pgm"></label>
<button onclick="upload('pgm')">Upload PGM</button><br><br>

<h3>Frames</h3>
<div class="frame-list" id="frameList">Loading...</div>

<script>
async function loadFrames() {
  const r = await fetch('/sprites');
  const d = await r.json();
  document.getElementById('delay').value = d.delay;
  const list = document.getElementById('frameList');
  list.innerHTML = '';
  d.frames.forEach(f => {
    const card = document.createElement('div');
    card.className = 'frame-card';
    card.innerHTML = `<img src="${f.ppm}" width="64" height="64"
                          style="image-rendering:pixelated"><br>
                      Frame ${f.idx}${f.alpha ? ' ✓α' : ''}<br>
                      <button onclick="del(${f.idx})">🗑</button>`;
    list.appendChild(card);
  });
}

async function upload(type) {
  const frame = document.getElementById('upFrame').value;
  const fileInput = document.getElementById(type === 'ppm' ? 'ppmFile' : 'pgmFile');
  if (!fileInput.files.length) { alert('Select a file first'); return; }
  const fd = new FormData();
  fd.append('file', fileInput.files[0]);
  const r = await fetch(`/sprites/upload?type=${type}&frame=${frame}`, {
    method:'POST', body: fd
  });
  if (r.ok) { alert('Uploaded!'); loadFrames(); }
  else       alert('Upload failed');
}

async function del(idx) {
  if (!confirm(`Delete frame ${idx}?`)) return;
  await fetch(`/sprites/delete?frame=${idx}`);
  loadFrames();
}

async function setDelay() {
  const v = document.getElementById('delay').value;
  await fetch(`/sprites/delay?value=${v}`);
  alert('Delay set to ' + v + 'ms');
}

loadFrames();
</script>
</body></html>
)html";
        req->send(200, "text/html", html);
    });
}

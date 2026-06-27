// ---------------------------------------------------------------------------
// spriteplay.cpp — Upload and animate user-defined 16x16 PPM sprite sequences
//
// Files stored in SPIFFS as /sprites/frame_00.ppm .. /sprites/frame_15.ppm
// Upload multiple PPMs at once via the web UI — sorted by filename, played
// in order. No PGM masks — transparent backgrounds via black pixels.
//
// Settings (frameDelay, frameCount) persisted in NVS via Preferences.
// ---------------------------------------------------------------------------

#include "spriteplay.h"
#include "led_display.h"
#include <FastLED.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <ESPAsyncWebServer.h>
#include <Arduino.h>

static const uint8_t  MAX_FRAMES = 16;
static const uint16_t SPRITE_W   = 16;
static const uint16_t SPRITE_H   = 16;
static const uint16_t PIXELS     = SPRITE_W * SPRITE_H; // 256

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
static uint16_t g_frameDelay  = 150;
static uint8_t  g_frameCount  = 0;
static bool     g_initialized = false;

static void loadSpriteSettings() {
    Preferences p;
    p.begin("spriteplay", true);
    g_frameDelay = p.getUShort("delay",  150);
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

// Scan SPIFFS and count how many frame_xx.ppm files actually exist
static uint8_t countFrames() {
    uint8_t n = 0;
    for (uint8_t i = 0; i < MAX_FRAMES; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/sprites/frame_%02d.ppm", i);
        if (SPIFFS.exists(path)) n = i + 1;
        else break;
    }
    return n;
}

// ---------------------------------------------------------------------------
// PPM parser (P6 binary, 16x16, maxval 255)
// ---------------------------------------------------------------------------
static bool parsePPM(const char* path, uint8_t* rgbBuf) {
    File f = SPIFFS.open(path, "r");
    if (!f) return false;

    // Read and validate magic
    char magic[3] = {0};
    f.read((uint8_t*)magic, 2);
    if (magic[0] != 'P' || magic[1] != '6') { f.close(); return false; }

    // Skip whitespace/comments, read integers
    auto skipWS = [&]() {
        while (f.available()) {
            char c = (char)f.peek();
            if (c == '#') { while (f.available() && (char)f.read() != '\n') {} }
            else if (c==' '||c=='\t'||c=='\r'||c=='\n') f.read();
            else break;
        }
    };
    auto readInt = [&]() -> int {
        skipWS(); int v=0;
        while (f.available()) {
            char c=(char)f.peek();
            if (c<'0'||c>'9') break;
            v = v*10 + (f.read()-'0');
        }
        return v;
    };

    int w=readInt(), h=readInt(), maxval=readInt();
    if (f.available()) f.read(); // consume single whitespace after maxval

    if (w!=SPRITE_W || h!=SPRITE_H || maxval!=255) {
        Serial.printf("[Sprite] %s: bad size %dx%d or maxval %d\n",path,w,h,maxval);
        f.close(); return false;
    }

    size_t got = f.read(rgbBuf, PIXELS*3);
    f.close();
    return (got == PIXELS*3);
}

// ---------------------------------------------------------------------------
// Render one frame onto the LED array
// ---------------------------------------------------------------------------
static void renderFrame(CRGB* leds, uint8_t frameIdx) {
    char path[32];
    snprintf(path, sizeof(path), "/sprites/frame_%02d.ppm", frameIdx);

    static uint8_t rgbBuf[PIXELS * 3];

    if (!parsePPM(path, rgbBuf)) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        leds[XY(0,0)] = CRGB::Magenta; // error indicator
        return;
    }

    // PPM stores pixels left-to-right, top-to-bottom.
    // XY() handles the serpentine matrix layout (even rows reversed).
    // We must use XY() so the image renders correctly on the physical matrix.
    for (uint8_t y = 0; y < SPRITE_H; y++) {
        for (uint8_t x = 0; x < SPRITE_W; x++) {
            uint16_t i = y * SPRITE_W + x;
            leds[XY(x, y)] = CRGB(rgbBuf[i*3], rgbBuf[i*3+1], rgbBuf[i*3+2]);
        }
    }
}

// ---------------------------------------------------------------------------
// Public pattern function
// ---------------------------------------------------------------------------
void spriteplay(CRGB* leds) {
    if (!g_initialized) {
        loadSpriteSettings();
        g_frameCount  = countFrames(); // sync with what's actually on SPIFFS
        g_initialized = true;
    }

    if (g_frameCount == 0) {
        // No frames — slow rainbow placeholder
        static uint8_t hue = 0;
        fill_solid(leds, NUM_LEDS, CHSV(hue++, 200, 60));
        FastLED.show();
        delay(50);
        return;
    }

    static uint8_t  currentFrame  = 0;
    static uint32_t lastFrameTime = 0;

    uint32_t now = millis();
    if (now - lastFrameTime >= g_frameDelay) {
        renderFrame(leds, currentFrame);
        FastLED.show();
        currentFrame  = (currentFrame + 1) % g_frameCount;
        lastFrameTime = now;
    }
}

// ---------------------------------------------------------------------------
// Web endpoints
// ---------------------------------------------------------------------------
void spriteplaySetup(AsyncWebServer* server) {
    loadSpriteSettings();

    // ---------------------------------------------------------------------------
    // GET /sprites/data — frame list + settings as JSON
    // ---------------------------------------------------------------------------
    server->on("/sprites/data", HTTP_GET, [](AsyncWebServerRequest* req) {
        // Rescan SPIFFS so UI always reflects reality
        uint8_t actual = countFrames();
        if (actual != g_frameCount) {
            g_frameCount = actual;
            saveSpriteSettings();
        }
        String json = "{\"frameCount\":" + String(g_frameCount) +
                      ",\"delay\":"      + String(g_frameDelay) +
                      ",\"frames\":[";
        for (uint8_t i = 0; i < g_frameCount; i++) {
            if (i) json += ",";
            json += "{\"idx\":" + String(i) +
                    ",\"url\":\"/sprites/frame_" +
                    (i<10?"0":"") + String(i) + ".ppm\"}";
        }
        json += "]}";
        req->send(200, "application/json", json);
    });

    // ---------------------------------------------------------------------------
    // POST /sprites/upload?frame=N — upload one PPM file
    // Called once per file by the multi-upload JS loop
    // ---------------------------------------------------------------------------
    server->on("/sprites/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            req->send(200, "text/plain", "OK");
        },
        [](AsyncWebServerRequest* req, const String& filename,
           size_t index, uint8_t* data, size_t len, bool final)
        {
            static File  uploadFile;
            static uint8_t uploadFrame = 0;

            if (index == 0) {
                uploadFrame = req->hasParam("frame")
                              ? (uint8_t)req->getParam("frame")->value().toInt()
                              : 0;
                char path[32];
                snprintf(path, sizeof(path), "/sprites/frame_%02d.ppm", uploadFrame);
                uploadFile = SPIFFS.open(path, "w");
                if (!uploadFile)
                    Serial.printf("[Sprite] Failed to open %s\n", path);
                else
                    Serial.printf("[Sprite] Receiving frame %d → %s\n", uploadFrame, path);
            }

            if (uploadFile) uploadFile.write(data, len);

            if (final && uploadFile) {
                uploadFile.close();
                if (uploadFrame >= g_frameCount) {
                    g_frameCount = uploadFrame + 1;
                    saveSpriteSettings();
                }
                Serial.printf("[Sprite] Frame %d complete\n", uploadFrame);
                // Force re-init so pattern picks up new frame immediately
                g_initialized = false;
            }
        }
    );

    // ---------------------------------------------------------------------------
    // GET /sprites/clear — delete all frames
    // ---------------------------------------------------------------------------
    server->on("/sprites/clear", HTTP_GET, [](AsyncWebServerRequest* req) {
        for (uint8_t i = 0; i < MAX_FRAMES; i++) {
            char path[32];
            snprintf(path, sizeof(path), "/sprites/frame_%02d.ppm", i);
            SPIFFS.remove(path);
        }
        g_frameCount  = 0;
        g_initialized = false;
        saveSpriteSettings();
        req->send(200, "text/plain", "Cleared");
    });

    // ---------------------------------------------------------------------------
    // GET /sprites/delay?value=N — set frame delay in ms
    // ---------------------------------------------------------------------------
    server->on("/sprites/delay", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (req->hasParam("value")) {
            g_frameDelay = (uint16_t)constrain(
                req->getParam("value")->value().toInt(), 10, 10000);
            saveSpriteSettings();
        }
        req->send(200, "text/plain", "OK");
    });

    // ---------------------------------------------------------------------------
    // GET /sprites/ui — management page
    // ---------------------------------------------------------------------------
    server->on("/sprites/ui", HTTP_GET, [](AsyncWebServerRequest* req) {
        String html = R"html(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sprite Animator</title>
<link rel="stylesheet" href="/style.css">
<style>
  .section{background:#282c34;border-radius:6px;padding:12px;margin-bottom:12px}
  .section h3{margin:0 0 10px;color:#61dafb;font-size:1em}
  .frames{display:flex;flex-wrap:wrap;gap:8px;margin-top:8px}
  .frame-card{border:1px solid #444;border-radius:6px;padding:8px;
              text-align:center;min-width:80px}
  .frame-card img{display:block;width:64px;height:64px;
                  image-rendering:pixelated;margin:0 auto 4px}
  .drop-zone{border:2px dashed #61dafb;border-radius:8px;padding:24px;
             text-align:center;cursor:pointer;color:#61dafb;margin:8px 0}
  .drop-zone.over{background:#1a2a3a}
  .btn{background:#61dafb;color:#282c34;border:none;padding:8px 20px;
       border-radius:4px;font-weight:bold;cursor:pointer;margin:4px}
  .btn.danger{background:#e74c3c;color:#fff}
  .btn:disabled{background:#444;color:#666;cursor:not-allowed}
  progress{width:100%;height:16px;border-radius:4px;margin-top:8px}
  #status{margin-top:8px;min-height:1.4em;font-size:.9em;color:#aaa}
  .delay-row{display:flex;align-items:center;gap:8px}
  input[type=number]{width:70px;background:#1a1a2e;color:#fff;
                     border:1px solid #444;padding:4px 6px;border-radius:4px}
</style>
</head><body>
<h2>&#x1F5BC;&#xFE0F; Sprite Animator</h2>

<div class="section">
  <h3>Frame delay</h3>
  <div class="delay-row">
    <input type="number" id="delay" min="10" max="10000" value="150">
    <span>ms per frame</span>
    <button class="btn" onclick="setDelay()">Set</button>
  </div>
</div>

<div class="section">
  <h3>Upload frames</h3>
  <p style="color:#aaa;font-size:.85em;margin:0 0 8px">
    Select multiple <code>.ppm</code> files at once. They will be sorted by
    filename and assigned frame numbers in order. Use the naming convention
    <code>frame_00.ppm</code>, <code>frame_01.ppm</code>, etc.
  </p>
  <div class="drop-zone" id="dropZone" onclick="document.getElementById('fileInput').click()">
    &#x1F4C2; Click or drop <code>.ppm</code> files here
  </div>
  <input type="file" id="fileInput" accept=".ppm" multiple style="display:none">
  <div id="fileList" style="color:#aaa;font-size:.85em;margin:4px 0"></div>
  <progress id="progress" value="0" max="100" style="display:none"></progress>
  <div id="status"></div>
  <button class="btn" id="uploadBtn" onclick="startUpload()" disabled>Upload</button>
  <button class="btn danger" onclick="clearAll()">&#x1F5D1; Clear all frames</button>
</div>

<div class="section">
  <h3>Current frames (<span id="frameCountLabel">0</span>)</h3>
  <div class="frames" id="frameList">Loading...</div>
</div>

<script>
let selectedFiles = [];
const dz = document.getElementById('dropZone');
const fi = document.getElementById('fileInput');
const btn = document.getElementById('uploadBtn');
const prog = document.getElementById('progress');
const stat = document.getElementById('status');

fi.onchange = e => setFiles([...e.target.files]);
dz.ondragover = e => { e.preventDefault(); dz.classList.add('over'); };
dz.ondragleave = () => dz.classList.remove('over');
dz.ondrop = e => {
  e.preventDefault(); dz.classList.remove('over');
  setFiles([...e.dataTransfer.files].filter(f=>f.name.endsWith('.ppm')));
};

function setFiles(files) {
  // Sort by filename so frame_00 comes before frame_01 etc.
  selectedFiles = files.sort((a,b) => a.name.localeCompare(b.name));
  document.getElementById('fileList').textContent =
    selectedFiles.length + ' file(s): ' + selectedFiles.map(f=>f.name).join(', ');
  btn.disabled = selectedFiles.length === 0;
  stat.textContent = '';
}

async function startUpload() {
  if (!selectedFiles.length) return;
  btn.disabled = true;
  prog.style.display = 'block';
  prog.value = 0;
  stat.textContent = 'Uploading...';

  for (let i = 0; i < selectedFiles.length; i++) {
    const file = selectedFiles[i];
    stat.textContent = `Uploading ${file.name} (${i+1}/${selectedFiles.length})...`;
    prog.value = Math.round(i / selectedFiles.length * 100);

    const fd = new FormData();
    fd.append('file', file);
    try {
      const r = await fetch(`/sprites/upload?frame=${i}`, {
        method: 'POST', body: fd
      });
      if (!r.ok) throw new Error(await r.text());
    } catch(e) {
      stat.textContent = `Error on frame ${i}: ${e.message}`;
      btn.disabled = false;
      return;
    }
  }

  prog.value = 100;
  stat.textContent = `✅ Uploaded ${selectedFiles.length} frame(s)!`;
  selectedFiles = [];
  document.getElementById('fileList').textContent = '';
  btn.disabled = true;
  await loadFrames();
}

async function clearAll() {
  if (!confirm('Delete all frames?')) return;
  await fetch('/sprites/clear');
  stat.textContent = 'All frames cleared.';
  await loadFrames();
}

async function setDelay() {
  const v = document.getElementById('delay').value;
  await fetch('/sprites/delay?value=' + v);
  stat.textContent = 'Frame delay set to ' + v + 'ms';
}

async function loadFrames() {
  const r = await fetch('/sprites/data');
  const d = await r.json();
  document.getElementById('delay').value = d.delay;
  document.getElementById('frameCountLabel').textContent = d.frameCount;
  const list = document.getElementById('frameList');
  if (d.frameCount === 0) {
    list.innerHTML = '<span style="color:#666">No frames uploaded yet</span>';
    return;
  }
  list.innerHTML = d.frames.map(f => `
    <div class="frame-card">
      <img src="${f.url}?t=${Date.now()}" alt="Frame ${f.idx}">
      <div style="font-size:.8em;color:#aaa">Frame ${f.idx}</div>
    </div>`).join('');
}

loadFrames();
</script>
</body></html>)html";
        req->send(200, "text/html", html);
    });
}

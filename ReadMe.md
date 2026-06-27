# PixelBoard — 16×16 NeoPixel Matrix

ESP32-WROOM-32 driven 16×16 WS2812B LED matrix with WiFi control, animated
patterns, Pac-Man chase mode, sprite animator, and OTA firmware updates.

---

## Hardware

| Component | Detail |
|---|---|
| Microcontroller | ESP32-WROOM-32 |
| LED matrix | 16×16 WS2812B NeoPixel (256 LEDs) |
| Data pin | GPIO 26 (change `DATA_PIN` in `main.cpp` if needed) |
| Power | 5V, 10A recommended for full brightness |

---

## First-Time Setup (USB Flash)

You must flash via USB the first time, or any time you change the partition
table. After that, all updates can be done over WiFi.

### 1. Install prerequisites
- [VS Code](https://code.visualstudio.com/) + [PlatformIO extension](https://platformio.org/install/ide?install=vscode)
- Or PlatformIO CLI: `pip install platformio`

### 2. Find your COM port
With the ESP32 connected via USB:
```
C:\Users\<you>\.platformio\penv\Scripts\pio.exe device list
```
Look for a USB Serial device (e.g. `COM7` on Windows, `/dev/ttyUSB0` on Linux).

### 3. Set your COM port
Edit `platformio.ini`, find this line under `[env:esp32dev]` and update it:
```ini
upload_port = COM7        ; <<<< change to your actual COM port
```

### 4. Build and flash firmware
```bash
pio run -e esp32dev -t upload
```

### 5. Upload filesystem (SPIFFS) — optional
Only needed if you want to pre-load sprite files. For normal use, skip this.
```bash
pio run -e esp32dev -t uploadfs
```

### 6. Open serial monitor (optional)
```bash
pio device monitor
```
You should see WiFi connection status and the device IP address.

---

## WiFi Setup

On first boot (no credentials saved), the device starts as an access point:
- **SSID:** `PixelBoardSetup`
- **IP:** `192.168.4.1`

Connect to it and enter your WiFi credentials. The device will reboot and
connect to your network. It will then be reachable at:
- `http://pixelboard.local` (mDNS, most networks)
- Or the IP shown in the serial monitor

---

## OTA Firmware Updates

After the first USB flash, you never need a cable again.

### Option A — Web browser upload (recommended, no tools needed)

1. Build the firmware binary in PlatformIO:
   ```bash
   pio run -e esp32dev
   ```
   The compiled binary will be at:
   ```
   .pio\build\esp32dev\firmware.bin
   ```

2. Open a browser and go to:
   ```
   http://pixelboard.local/update
   ```
   Or click **⚙️ gear icon → Firmware Update (OTA)** in the web UI.

3. Click the upload area, select `firmware.bin`, and click **Upload Firmware**.

4. The matrix shows a green progress bar on row 7 during upload.
   When complete: green flash → white flash → device reboots automatically.

5. Wait ~5 seconds, then the page will redirect to the home screen.

### Option B — PlatformIO CLI

```bash
pio run -e esp32dev_ota -t upload
```

The device must be on your network and reachable as `pixelboard.local`.
If mDNS doesn't work on your network, edit `platformio.ini`:
```ini
upload_port = 192.168.1.42   ; use the actual IP instead
```

### OTA visual feedback on the matrix

| Event | Display |
|---|---|
| Upload started | 3 cyan pixels, top-left |
| Uploading | Green bar sweeping across row 7 |
| Success | Green flash → white flash → black |
| Error | Red flash |

---

## Partition Table

This project uses `min_spiffs.csv` which provides:

| Partition | Size |
|---|---|
| App0 (active) | 960 KB |
| App1 (OTA) | 960 KB |
| SPIFFS | 960 KB |
| NVS | 20 KB |

If you previously flashed with the default partition table, you **must**
reflash via USB once to update the partition layout.

---

## Adding a Password to OTA

By default OTA has no password (fine for home use). To add one:

1. In `PixelWifiServer.cpp`, uncomment:
   ```cpp
   ArduinoOTA.setPassword("pixelboard");
   ```

2. In `platformio.ini`, uncomment:
   ```ini
   upload_flags = --auth=pixelboard
   ```

3. The web upload page handles authentication automatically.

---

## Settings Persistence

All settings (WiFi credentials, Pac-Man configuration, brightness, pattern
selection) are stored in NVS (Non-Volatile Storage) flash and survive reboots
and OTA updates. They are only cleared by a full flash erase:
```bash
pio run -e esp32dev -t erase
```

---

## Web Interface

| URL | Description |
|---|---|
| `http://pixelboard.local/` | Main pattern selector |
| `http://pixelboard.local/pacman/ui` | Pac-Man chase settings |
| `http://pixelboard.local/sprites/ui` | Sprite animator |
| `http://pixelboard.local/update` | OTA firmware update |
| `http://pixelboard.local/pacman/data` | Pac-Man settings (JSON) |
| `http://pixelboard.local/list` | SPIFFS file listing (debug) |

---

## Sprite CSV Workflow

The Pac-Man sprite roles and score text are managed via `tools/sprites.csv`.

```bash
# Validate the CSV
python tools/sprites.py check

# Push CSV changes into pacman.cpp, then rebuild and flash
python tools/sprites.py import
pio run -e esp32dev_ota -t upload   # or USB for first time

# Pull current C++ state back into CSV
python tools/sprites.py export
```

---

## Troubleshooting

**OTA upload fails / device not found**
- Check device is on your WiFi (not in AP mode)
- Try using the IP address instead of `pixelboard.local`
- Ensure no firewall blocks UDP port 3232 (ArduinoOTA)

**Colors look wrong**
- Verify your strip is WS2812B (not WS2811)
- Check `COLOR_ORDER` in `led_display.h` — try `GRB` or `RGB`

**Device boot-loops after bad OTA flash**
- Flash via USB: `pio run -e esp32dev -t upload`
- The previous firmware is retained in the inactive partition

**Settings lost after flash**
- NVS survives OTA. Only `pio run -t erase` clears settings.

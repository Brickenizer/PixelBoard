# PixelBoard

A versatile ESP32-based 16x16 LED matrix display project featuring multiple interactive patterns, a web interface for control, and real-time pattern preview.

<p align="center">
  <a href="https://youtu.be/Xm2CHKMk3TU">
    <img width="500px" src="https://img.youtube.com/vi/Xm2CHKMk3TU/maxresdefault.jpg" alt="How to Create a Discord Bot in Python">
  </a>
  <br>
  <em>How I built the Pixelboard - Click to watch the video overview</em>
</p>


## Features

- 🌈 20+ Built-in Patterns
- 🎨 Interactive Drawing Interface
- 🎥 Video Pattern Support
- 🌐 Web-based Control Interface
- 📱 Mobile-friendly Design
- 🔄 Real-time Pattern Preview
- 🎛️ Brightness & Speed Controls

## Hardware Requirements

- ESP32 Development Board
- 16x16 WS2812B LED Matrix (256 LEDs)
- 5V Power Supply (capable of providing enough current for the LED matrix)
- USB Cable for Programming

## Initial Setup

1. **First Boot**
   - On first power-up, the device will create a WiFi access point named "PixelBoardSetup"
   - Connect to this network with your phone or computer
   - Navigate to `192.168.4.1` in your web browser
   - Enter your WiFi credentials
   - The device will restart and connect to your network

2. **Normal Operation**
   - After connecting to your WiFi, the device will be accessible at:
     - `http://pixelboard.local` (if your device supports mDNS)
     - The IP address shown in the ESP32's serial output

## Available Patterns

### Classic Patterns
- **Fire**: Realistic fire simulation
- **The Matrix**: Digital rain effect inspired by The Matrix
- **Rainbow Drift**: Smooth rainbow color transitions
- **Twinkle**: Random twinkling stars effect
- **Color Wipe**: Sequential color filling animation

### Game Characters
- **Pac Man Ghost**: Animated ghost from Pac-Man
- **Ms Pac-Man**: Classic Pac-Man character
- **Super Mario**: Pixelated Mario animation
- **Qbert**: Classic Qbert character

### Many more... 
- **Jelly Fish**: Underwater jellyfish animation
- **Draw**: Interactive drawing interface with color selection and image upload
- **Video**: Upload and display video content
- **Game of Life**: Conway's Game of Life implementation
- **Swirl**: Rotating color patterns
- **Clock Countdown**: Visual timer display
- **Rainbow Glitter**: Rainbow pattern with sparkle effects
- **Confetti**: Random colorful pixel explosions
- **Juggle**: Sinusoidal pattern movements
- **Color Rave**: Dynamic color pulsing


## Contributing

Contributions are welcome! Feel free to submit pull requests or create issues for bugs and feature requests.

## License

This project is open source and available under the MIT License.

## Acknowledgments

- FastLED Library for LED control
- ESPAsyncWebServer for the web interface

## OTA Updates (Phase 4)

After the initial USB flash, firmware can be updated over WiFi.

### First flash (USB)
```bash
pio run -e esp32dev -t upload
```

### Subsequent updates (OTA)
```bash
pio run -e esp32dev_ota -t upload
```

The device must be on your WiFi network. It advertises as `pixelboard.local` via mDNS.

**OTA visual feedback on the matrix:**
- Upload starts: 3 cyan pixels top-left
- Progress: green bar sweeping across row 7 (0-16 pixels)
- Success: full green flash → white flash → black
- Error: red flash

### Notes
- OTA only works in STA mode (connected to your network). AP/onboarding mode does not support OTA.
- The `min_spiffs.csv` partition table is required for OTA — it creates two app partitions (~960KB each) at the cost of slightly reduced SPIFFS space (~960KB vs ~1.4MB).
- If a bad flash bricks the device, reflash via USB.
- To add a password: uncomment `ArduinoOTA.setPassword("pixelboard")` in `PixelWifiServer.cpp` and add `upload_flags = --auth=pixelboard` to `platformio.ini`.

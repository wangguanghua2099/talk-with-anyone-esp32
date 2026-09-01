# Talk With Anyone · ESP32 Smart Speaker Frontend

English | [简体中文](README.md)

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

An ESP32-S3 voice-chat speaker firmware for the local-first voice assistant
**[talk-with-anyone](https://github.com/wangguanghua2099/talk-with-anyone)**.
Press the button (or let auto-listen take over) and start talking:

> You speak → speech recognition → AI replies out loud → live Chinese subtitles
> on the 1.54" display → back to listening, automatically.

All audio stays between your computer and the speaker — no third-party cloud involved.

## Features

- **Fully automatic conversation**: auto-listens after boot, answers when you
  stop speaking, and keeps listening afterwards — no button presses needed
- **Chinese subtitles**: live typewriter subtitles on a 240×240 TFT,
  green for you / cyan for the AI, word-wrapped and scrolling, 80-line history
- **Flicker-free rendering**: row-by-row diffing with incremental redraw —
  only the newly typed glyphs touch the bus. No full-screen clears, no strobing,
  zero extra framebuffer memory
- **Physical buttons**: BOOT = interrupt playback / enter or leave conversation;
  dedicated volume +/- keys (step 10, persisted across reboots)
- **Status bar**: conversation state in four colors, NTP-synced clock,
  Wi-Fi signal bars, battery level and charging indicator
- **Audio pipeline**: linear-interpolation resampling to 16 kHz output;
  square-law volume curve with input pre-gain and soft limiting for
  loud, undistorted output at low volume settings

## Hardware

Developed and verified on the **Luxiaoban 3rd-gen "Xiaozhi" 1.54" Wi-Fi board**
(ESP32-S3, 8MB Flash + 8MB PSRAM):

| Part | Spec | Pins |
|------|------|------|
| Microphone | PDM digital mic | CLK=GPIO2, DATA=GPIO3 |
| Amplifier | NS4168 I2S | DOUT=7, BCLK=15, LRCK=16 |
| Display | ST7789 240×240 SPI | MOSI=10, SCLK=9, DC=8, CS=14, RST=18, backlight=13 |
| Buttons | BOOT / Vol+ / Vol− | GPIO0 / GPIO39 / GPIO40 |
| Battery | voltage on ADC2_CH6 (GPIO17), charging detect GPIO38 | |

All pins live in [`firmware/src/config.h`](firmware/src/config.h) —
porting to another board only touches that file (plus the panel class in
`display_ui.cpp`).

## Getting Started

### Prerequisites

- [PlatformIO](https://platformio.org/) (VSCode extension or CLI)
- A computer running the
  [talk-with-anyone](https://github.com/wangguanghua2099/talk-with-anyone)
  server (default port 7862, see that repository for setup)
- The speaker and the computer on the same LAN

### Steps

1. **Create your private config** (Wi-Fi credentials and server IP never enter
   the repository):

   ```bash
   cd firmware/src
   cp config_private.example.h config_private.h
   ```

   Edit `config_private.h`:
   - `WIFI_SSID` / `WIFI_PASSWORD`: the Wi-Fi the speaker joins (same network
     as the server; a phone hotspot works too)
   - `SERVER_HOST`: LAN IPv4 of the computer running the server
     (`ipconfig` on Windows; a static DHCP reservation is recommended)

2. **Build and flash**:

   ```bash
   cd firmware
   pio run                # build
   pio run -t upload      # flash (USB connected)
   pio device monitor     # serial log (115200)
   ```

3. **Talk**: the speaker connects and starts listening automatically.
   - Short-press BOOT while the AI is talking: interrupt and keep listening
   - Short-press BOOT while listening: leave the conversation (press again to resume)
   - Volume +/-: adjust live and persist; briefly shown in the status bar

## How It Works

```
PDM mic ──PCM16@16k──▶ WebSocket /ws/voice ──▶ server ASR ─▶ LLM
        (interrupt control frame on button press)        │
Display   ◀── asr.result / assistant.completed text ─────┘
Speaker   ◀── WebSocket /ws/tts-stream ◀── PCM16 audio stream
                (ring buffer + resampling → I2S 32-bit out)
```

| Module | File | Responsibility |
|--------|------|----------------|
| State machine | [`main.cpp`](firmware/src/main.cpp) | idle/listening/playing, buttons, volume |
| Protocol | [`voice_client.cpp`](firmware/src/voice_client.cpp) | dual WebSockets, auto-reconnect, interrupt |
| Capture | [`audio_in.cpp`](firmware/src/audio_in.cpp) | PDM mic, 16 kHz mono |
| Playback | [`audio_out.cpp`](firmware/src/audio_out.cpp) | ring buffer, resampling, volume curve |
| Subtitles | [`display_ui.cpp`](firmware/src/display_ui.cpp) | partial redraw, typewriter, status bar |

## FAQ

- **No response to speech**: check `[麦克风] 峰值` in the serial log — it should
  exceed 10000 while speaking. Stuck near 0? Raise `MIC_GAIN_SHIFT` in
  `config.h` or check the PDM wiring.
- **Too quiet / too loud**: adjust `SPK_INPUT_GAIN` (default 4.0, soft-limited).
- **Cannot reach the server**: same subnet, firewall allows port 7862,
  and `SERVER_HOST` matches the computer's current IP.
- **Clock shows `--:--`**: NTP not synced yet — make sure the router allows
  outbound UDP 123.

## Known Limitations

- Voice barge-in (interrupting by speaking) is not implemented on this
  hardware; use the BOOT button instead. 
- The display driver is a generic ST7789 LovyanGFX panel config; other panels
  need their own setup.
- Edge TTS is an on-cloud online speech synthesis system. It has not performed
  streaming generation, the project is not adapted, and there is no audio playback.

## Acknowledgements

- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — the Xiaozhi AI
  ecosystem and the origin of this hardware
- [LovyanGFX](https://github.com/lovyan03/LovyanGFX),
  [WebSockets](https://github.com/Links2004/arduinoWebSockets),
  [ArduinoJson](https://github.com/bblanchon/ArduinoJson)
- [talk-with-anyone](https://github.com/wangguanghua2099/talk-with-anyone) — the companion server

## License

[MIT](LICENSE)

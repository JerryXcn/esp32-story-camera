# esp32-story-camera

[简体中文](README.zh-CN.md) | English

An AI picture-book storyteller for kids, built on an ESP32-P4 board with a camera and a touch screen.
Point it at a page of a picture book, press the shutter, and a friendly fox tells a short story about
that page — written by a vision LLM and read aloud with TTS. Stories are saved to the SD card, so kids
can listen again offline.

| Home | Camera | Player |
|---|---|---|
| ![Home](docs/images/mockup-home.jpg) | ![Camera](docs/images/mockup-camera.jpg) | ![Player](docs/images/mockup-player.jpg) |

<sub>Design mockups (the picture-book page is AI-generated). Real device screen: [docs/images/device-home.png](docs/images/device-home.png).</sub>

## Features

- **Snap a page, get a story** — full-screen 3:2 camera preview (~28 fps), photo → backend → vision LLM writes
  an age-appropriate story → TTS → the device downloads, verifies and auto-plays it.
- **Story library** — offline replay from the SD card, favorites, regenerate (keeps old versions),
  swipe / previous / next in the player, "play next" after a story ends.
- **Kid-friendly UI** — big buttons, a fox mascot, sound feedback, bulk actions only after a 6-second
  long press, confirmation before deleting.
- **Robust by design** — crash-safe writes on FAT (double-slot metadata, delete intents, recovery scan),
  idempotent uploads, resumable generation, TLS certificate verification always on.
- **Self-hosted backend** — FastAPI service with per-device tokens, quotas and a mock provider for tests;
  deployment templates for nginx + systemd.
- **Settings on the device** — Wi-Fi, child's age, story length, volume, backend address (domain or IP),
  storage usage.

## How it works

```
 ┌──────────── ESP32-P4 board ─────────────┐        HTTPS         ┌──────── backend (server/) ────────┐
 │ OV5647 → ISP → PPA → LVGL UI (480×320)  │  photo + settings    │ device auth · idempotency · quota │
 │ story_service: capture → upload → poll  │ ───────────────────▶ │ vision LLM: page → story          │
 │ → download MP3 → verify → play (ES8311) │ ◀─────────────────── │ TTS → MP3 24 kHz, -16 LUFS        │
 │ SD card: photos, stories, audio         │  title, text, audio  └───────────────────────────────────┘
 └─────────────────────────────────────────┘
```

The backend currently uses Zhipu BigModel (`glm-4.5v`) for vision + story writing and Xiaomi MiMo
(`mimo-v2.5-tts`) for speech. Providers are isolated in `server/app/providers/`, so others can be added.
The UI and stories are in Simplified Chinese.

## Hardware

| Part | Notes |
|---|---|
| Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 | ESP32-P4 (rev v1.x) + ESP32-C6 (Wi-Fi 6), 32 MB PSRAM, 16 MB flash, 3.5" 480×320 ST7796S SPI LCD, FT5x06 touch, ES8311 codec |
| OV5647 MIPI-CSI camera module | Mounted sideways on this board; orientation is configurable (`STORY_CAMERA_ROTATION`) |
| Speaker | Connected to the board's amplifier output |
| microSD card | FAT32, 8–32 GB |

## Repository layout

| Path | Content |
|---|---|
| `firmware/` | ESP-IDF v6.1 project (LVGL 9.2): UI, camera, storage, audio, networking, story pipeline — see [firmware/README.md](firmware/README.md) |
| `server/` | FastAPI backend, tests, deployment templates — see [server/README.md](server/README.md) |
| `docs/api-contract-v1.md` | Device ↔ backend API contract |
| `assets/` | UI illustrations and icons (sources for the firmware's generated C arrays) |
| `tests/fixtures/` | Shared test images and contract response samples |
| `tools/` | UART debugging tools (screenshots, taps, persistent console) |

## Quick start

### 1. Backend

You need API keys for [Zhipu BigModel](https://open.bigmodel.cn/) and Xiaomi MiMo (TTS),
Python 3.12+ (tested with 3.13 / 3.14) and ffmpeg.

```bash
cd server
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
cp .env.example .env.local      # fill in the API keys and SC_DEVICE_TOKENS (see "Pairing" below)
```

The device only talks HTTPS. Either deploy to a server with a real domain and a Let's Encrypt certificate
(`server/deploy/`, nginx on port 38800), or run on your LAN with a self-signed certificate
(`server/scripts/gen_dev_certs.sh` + `server/scripts/run_dev.sh`). Details: [server/README.md](server/README.md).

### 2. Firmware

Install [ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/get-started/), then:

```bash
cd firmware
idf.py menuconfig     # Story Camera → backend URL and device token (see "Pairing")
idf.py build
idf.py -p <PORT> flash monitor
```

### 3. On the device

1. **Settings → Wi-Fi**: join a 2.4 GHz network (the ESP32-C6 has no 5 GHz radio).
2. **Settings → Server**: enter your backend host (domain or IP) and port, tap *Test*, then *Save*.
3. Take a photo of a picture-book page. The first time, a parent confirms that photos are uploaded for
   story generation.

## Pairing the device with your backend

Each device authenticates with a 32-hex-character token; the backend only accepts tokens listed in
`SC_DEVICE_TOKENS`.

1. Generate a token: `python3 -c "import secrets; print(secrets.token_hex(16))"`
2. Firmware: `menuconfig → Story Camera → Device token` (`CONFIG_STORY_DEV_TOKEN`).
3. Backend: `SC_DEVICE_TOKENS=<token>:<any-device-name>` (comma-separate multiple devices).

The token is written to NVS only on the first boot when NVS has no token yet. If the device has already
booted with an empty token (which generates a random, unexported one), erase NVS first:
`idf.py -p <PORT> erase-flash` (this also clears saved Wi-Fi and settings), then flash again.

## Development notes

- `CONFIG_STORY_DEBUG_SILENT=y` builds a silent firmware (amplifier off) for debugging in quiet places.
- The UART console supports screenshots, simulated taps, page navigation, camera diagnostics and more —
  see [firmware/README.md](firmware/README.md#uart-调试控制台).
- Server tests: `cd server && .venv/bin/python -m pytest tests/` (mock provider, no network needed).

## Status and limitations

- Tested on one Waveshare ESP32-P4-WIFI6-Touch-LCD-3.5 (chip rev v1.3) with ESP-IDF v6.1.
- Chinese UI and stories only; one narrator voice.
- No OTA updates yet (single factory partition, USB flashing).
- Photos of book pages are sent to your backend and to the AI providers you configure. Review the providers'
  terms and privacy policies before letting children use it.

## License

[Apache License 2.0](LICENSE). Third-party components, fonts and generated content are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

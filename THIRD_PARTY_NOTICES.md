# Third-party notices / 第三方声明

This project is licensed under the [Apache License 2.0](LICENSE). It builds on the following
third-party software and content.

## Firmware dependencies (downloaded at build time)

These components are fetched by the ESP-IDF Component Manager (versions pinned in
`firmware/dependencies.lock`). They are **not** stored in this repository, but they are compiled
into firmware binaries built from it. Each is distributed under its own license
(see `firmware/managed_components/<name>/LICENSE` after a build).

| Component | Version | License |
|---|---|---|
| ESP-IDF | 6.1 | Apache-2.0 |
| lvgl/lvgl | 9.2.2 | MIT |
| espressif/cjson | 1.7.19 | MIT (cJSON) |
| espressif/esp_video | 2.3.0 | ESPRESSIF MIT (Espressif products only) |
| espressif/esp_ipa | 2.2.0 | ESPRESSIF MIT (Espressif products only) |
| espressif/esp_audio_codec | 2.5.0 | Espressif Modified MIT (Espressif products only) |
| espressif/esp_cam_sensor, esp_codec_dev, esp_hosted, esp_wifi_remote, esp_lcd_st7796, esp_lcd_touch, esp_lcd_touch_ft5x06, esp_sccb_intf, esp_serial_slave_link, eppp_link, wifi_remote_over_eppp, esp_h264, usb, usb_host_uvc, cmake_utilities | see lock file | Apache-2.0 |

## Fonts (embedded as generated bitmaps)

`firmware/components/app_ui/fonts/*.c` are bitmap fonts generated with
[lv_font_conv](https://github.com/lvgl/lv_font_conv) from:

| Font | License | License text |
|---|---|---|
| Resource Han Rounded CN (源圆黑体) by Cyano Hao, based on Source Han Sans | SIL Open Font License 1.1 (fonts), MIT (build code) | `firmware/components/app_ui/fonts/licenses/Resource-Han-Rounded-LICENSE.md` |
| ZCOOL KuaiLe (站酷快乐体) | SIL Open Font License 1.1 | `firmware/components/app_ui/fonts/licenses/ZCOOL-KuaiLe-OFL.txt` |

## Generated content

- **Illustrations** (`assets/v0.2/`, fox mascot, home tiles, background) and the picture-book page in the
  README mockups were generated with an AI image model for this project.
- **Voice prompts** (`firmware/components/audio_service/clips/prompt_*.mp3`) were synthesized with
  Xiaomi MiMo TTS and loudness-normalized. Click and shutter sounds, and `testdata/reference.mp3`,
  are synthesized tones.
- **Icons** (`assets/v0.2/icons/`) are drawn as SVG in `firmware/components/app_ui/tools/icons/icons.html`.

## Backend dependencies

Installed with pip from `server/requirements.txt` (FastAPI, Uvicorn, HTTPX, python-multipart, Pillow,
pytest), each under its own license. ffmpeg is used as an external program for audio transcoding.

## AI services

The backend calls Zhipu BigModel and Xiaomi MiMo APIs with **your own** API keys. Generated stories and
audio are subject to those providers' terms of service.

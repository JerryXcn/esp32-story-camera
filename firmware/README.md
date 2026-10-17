# esp32-story-camera 固件 / Firmware

ESP32-P4-WIFI6-Touch-LCD-3.5 上的 AI 绘本故事机固件（480×320 横屏，LVGL 9，ESP-IDF v6.1）。
总体介绍、硬件清单与快速上手见仓库根目录 [README](../README.md)。

## 目录

| 目录 | 职责 |
|---|---|
| `main/` | 启动顺序与全局状态 |
| `components/board_adapter/` | 唯一接触引脚/外设的组件：LCD（ST7796S SPI）、触摸（FT5x06）、TF、相机、ES8311 音频、LVGL 显示通路 |
| `components/app_ui/` | 主题、素材、字库、全部页面与导航；拍照页旁路直绘（`ui_camera_direct.c`）；故事库后台操作（`ui_story_ops.c`） |
| `components/camera_service/` | 采集、PPA 旋转缩放、JPEG 编码、传感器曝光调整 |
| `components/storage_service/` | TF 卡数据：双槽元数据、版本提交、删除意图、收藏标记、恢复扫描 |
| `components/audio_service/` | MP3 → PCM → I2S，播放状态机，提示音片段 |
| `components/network_service/` | C6 协处理器 Wi-Fi（esp_hosted）、扫描、重连、SNTP |
| `components/api_client/` | 后端 V1 契约客户端（`../docs/api-contract-v1.md`） |
| `components/story_service/` | 拍照 → 上传 → 生成 → 下载 → 自动播放的单任务编排 |
| `components/settings_service/` | NVS：Wi-Fi、产品设置、服务器地址、设备令牌 |
| `components/debug_console/` | UART 调试命令行 |

## 构建与烧录

```bash
. $IDF_PATH/export.sh          # ESP-IDF v6.1
idf.py build                   # sdkconfig 由 sdkconfig.defaults 生成（不入库）
idf.py -p <PORT> flash monitor
```

关键配置（`sdkconfig.defaults`，均经实机验证）：

- `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`：开发板芯片为 rev v1.x，默认最低版本 v3.1 会拒绝烧写。
- `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`、`CONFIG_LV_USE_CLIB_MALLOC=y`：大缓冲放 PSRAM。
- OV5647 使用 1280×960 RAW10 binning 模式；`CONFIG_COMPILER_OPTIMIZATION_PERF=y`（-O2）。

`menuconfig → Story Camera`：

| 选项 | 说明 |
|---|---|
| `STORY_SERVER_BASE_URL` | 默认后端地址；设备上"设置 → 服务器"可随时修改 |
| `STORY_DEV_TOKEN` | 设备令牌，与后端 `SC_DEVICE_TOKENS` 配对（见根目录 README） |
| `STORY_CAMERA_ROTATION` / `STORY_CAMERA_MIRROR` | 摄像头安装方向；本板模组侧装，默认 270° 不镜像 |
| `STORY_DEBUG_SILENT` | 静音调试构建：功放常关、DAC 静音（公共场合调试用，发布关闭） |

## 自签名后端（局域网联调）

1. `server/scripts/gen_dev_certs.sh <电脑局域网IP>`：生成开发 CA 与服务端证书，并把 CA 复制到
   `components/api_client/server_ca.pem`（不入库）。
2. `menuconfig → Component config → mbedTLS → Certificate Bundle`：开启 *Add custom certificates to the default bundle*，
   路径填 `components/api_client/server_ca.pem`，重新编译烧录。
3. 设备"设置 → 服务器"填电脑 IP 与端口。换 IP 时 `gen_dev_certs.sh --keep-ca <新IP>` 重签服务端证书即可，固件不用重烧。

## 性能要点

- 刷屏异步：`flush_cb` 只发起 SPI DMA，完成中断计数，LVGL 复用缓冲前经 `flush_wait_cb` 等待。
- 拍照页旁路直绘：LVGL 只在界面变化时把屏幕快照成 ARGB8888 浮层，每个相机帧由 PPA 硬件叠加后整帧写屏，预览约 27–30 帧/秒。
- 传感器 30 帧/秒（帧长 1640 行）+ 防闪烁上限 3 档（30 ms），降低室内增益与噪点。

## 素材、字库与声音

- 素材转换：`python3 components/app_ui/tools/convert_assets.py`（输出 `components/app_ui/generated/`）。
- 字库：`components/app_ui/tools/gen_fonts.sh`（源圆黑体 + 站酷快乐体，见 `components/app_ui/fonts/README.md`）。
- 声音：`components/audio_service/clips/`（点击、快门为程序合成；4 句提示语由 MiMo TTS 生成并做响度归一）。

## C6 协处理器（esp_hosted）

主控与 C6 固件的 esp_hosted 主版本必须一致（`dependencies.lock` 中为 3.0.7）。若开机日志提示版本不匹配：

1. 复制 `managed_components/espressif__esp_hosted/examples/ota/coprocessor_ota/cp`，将其 esp_hosted 锁定到相同版本，
   `idf.py set-target esp32c6 build`。
2. 把镜像写入 P4 未分区区域：`esptool --chip esp32p4 -p <PORT> write-flash --force 0xC00000 <cp>.bin`。
3. 串口控制台执行 `c6ota <镜像字节数>`，完成后自动重启，日志应为 `coprocessor=3.0.7 (match)`。

## UART 调试控制台

固件内置串口命令（`components/debug_console`），配合 `../tools/`：

```bash
python3 ../tools/device.py info                    # 端口默认取 STORY_CAMERA_PORT 或第一个 USB 串口
python3 ../tools/device.py screenshot /tmp/ui.png  # LVGL 场景截图（约 40 秒）
python3 ../tools/device.py tap 440 60              # 模拟点击
```

常驻会话用 `../tools/uartd.py`（串口只打开一次，避免每条命令复位设备）。其他命令：
`goto <页面>`、`tap x y [按住毫秒]`、`wifi <ssid> <password>`、`server [url|-]`、`nettest [job_id]`、
`camera jpg|dump|ctrl`、`camreg r|w`、`tasks`（栈水位）、`c6ota <size>`。注意：打开串口会复位设备。

# esp32-story-camera server

故事机的自建后端（FastAPI）：设备经 HTTPS 访问，服务端负责设备认证、幂等去重、任务编排、
看图编故事（智谱 BigModel `glm-4.5v`）与语音合成（小米 MiMo `mimo-v2.5-tts`），
统一输出契约 MP3（mono / 24 kHz / 64 kbps CBR，响度 -16 LUFS）。

接口契约：[`../docs/api-contract-v1.md`](../docs/api-contract-v1.md)。

## 本地运行

需要 Python 3.12+（已在 3.13 / 3.14 上测试）与 ffmpeg（转码与响度归一）。

```bash
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
cp .env.example .env.local            # 填 SC_BIGMODEL_API_KEY、SC_MIMO_API_KEY、SC_DEVICE_TOKENS
set -a; . ./.env.local; set +a
.venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8080
```

不调用真实供应商的离线开发 / 测试：`SC_PROVIDER_MODE=mock`。

设备只接受 HTTPS。局域网联调用自签名证书：

```bash
scripts/gen_dev_certs.sh <电脑局域网IP>   # 生成 ca.pem/server.pem，并把 CA 复制到固件（固件需开启自定义证书包，见 firmware/README.md）
echo "SC_DEV_TOKEN=<与固件 STORY_DEV_TOKEN 相同>" > dev-token.env
scripts/run_dev.sh                         # 读取 .env.local 与 dev-token.env，HTTPS :8443
```

## 公网部署（nginx + systemd）

`deploy/` 提供 Debian 服务器的部署模板：nginx 在 38800 端口终止 HTTPS（Let's Encrypt 证书），
反向代理到只监听本机的 uvicorn `127.0.0.1:38801`；systemd 服务 `storybox` 以独立用户运行。

1. 域名解析到服务器，并用 certbot 为该域名签发证书。
2. 在服务器上创建 `/etc/storybox/storybox.env`（权限 640，属主 root:storybox），内容参照 `.env.example`，
   另设 `SC_DATA_DIR=/var/lib/storybox`。
3. 本机执行 `STORY_DOMAIN=<你的域名> deploy/deploy_remote.sh root@<服务器>`：上传代码、安装依赖、
   安装 systemd 与 nginx 配置、放行 ufw 38800、重启并自检 `/v1/health`。
4. 端到端自检：`SC_DEV_TOKEN=<设备令牌> python3 scripts/e2e_remote.py https://<你的域名>:38800`。

日志：`journalctl -u storybox -f`。

## 配置（环境变量，完整列表见 `app/config.py`）

| 变量 | 默认 | 说明 |
|---|---|---|
| `SC_DATA_DIR` | `./data` | SQLite 与任务媒体 |
| `SC_DEVICE_TOKENS` | — | `token:device_id`，逗号分隔多台设备；服务端只存哈希 |
| `SC_PROVIDER_MODE` | `real` | `mock` 用于测试 |
| `SC_BIGMODEL_API_KEY` / `SC_MIMO_API_KEY` | 空 | 供应商密钥，只放在服务端 |
| `SC_VOICE_MAP` | `warm_female_01:mimo_default` | 产品声音 → 供应商音色 |
| `SC_QUOTA_PER_DEVICE_DAILY` | 50 | 每台设备每日生成次数 |
| `SC_MEDIA_TTL_H` / `SC_DIGEST_TTL_D` | 24 / 7 | 媒体 / 幂等摘要保留时长 |
| `SC_BIGMODEL_STORY_MODEL` | `glm-4.5v` | 换纯文本模型时同时设 `SC_BIGMODEL_STORY_INCLUDE_IMAGE=0` |

## 测试

```bash
.venv/bin/python -m pytest tests/                                     # mock 供应商，无需网络
SC_BIGMODEL_API_KEY=... SC_MIMO_API_KEY=... .venv/bin/python scripts/smoke_real.py   # 真实供应商冒烟
```

## 音频与文本

- TTS 输出统一转码为契约 MP3 并响度归一（-16 LUFS / -1.5 dBTP）。
- 朗读正文超过 `SC_STORY_TTS_MAX_CHARS`（默认 900）时在句末截断，保证音频不超过 240 s / 2 MiB。
- 标题只保留设备字库里有的字（GB2312 一级 + ASCII + 常用标点，`app/services/charset.py`），否则使用兜底标题。

## 安全

- 供应商密钥只在服务端；设备令牌按设备隔离、只存哈希、可随时从 `SC_DEVICE_TOKENS` 撤销。
- 日志不记录照片、故事正文、令牌。
- 设备端 TLS 证书校验始终开启。

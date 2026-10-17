# 云端任务 API 契约 V1（唯一版本）

依据《详细实施计划 V1.1》第 7 节。服务端实现见 `server/`（FastAPI），
设备端实现见 `firmware/components/api_client/`。契约样本见
`tests/fixtures/responses/`。

## 路由

| 方法与路径 | 语义 | 成功码 |
|---|---|---|
| `POST /v1/jobs` | multipart 创建首次/重新生成任务；重复请求返回原任务 | 202 |
| `GET /v1/jobs/by-operation/{operation_id}` | 查本设备已提交操作（POST 响应丢失/断电恢复） | 200 |
| `GET /v1/jobs/{job_id}` | 状态、attempt、revision、stage、error | 200 |
| `GET /v1/jobs/{job_id}/result` | 已通过检查的 title/story 与 audio 描述 | 200 |
| `POST /v1/jobs/{job_id}/retry` | 对已失败且允许的任务显式恢复 | 202 |
| `GET /v1/jobs/{job_id}/audio` | 下载契约音频（鉴权，无任意外链） | 200 |
| `GET /v1/health` | 服务探测（不返回内部配置） | 200 |

## 身份

- `Authorization: Bearer <device_token>`；后端从令牌确定设备身份，
  不相信请求体中的 device_id。令牌只存 SHA-256，按设备隔离、可撤销。
- 上传/重试附 `Idempotency-Key`；创建任务时 = operation_id。

## POST /v1/jobs

multipart 字段（`server/app/api/jobs.py` 为准）：

| 字段 | 限制 |
|---|---|
| operation_id | 32 位十六进制，与 Idempotency-Key 一致 |
| story_id | 32 位十六进制，仅作关联 |
| mode | generate / regenerate |
| base_version | 首次 0，重新生成为当前已提交版本号 |
| image | JPEG ≤1 MiB，解码后长边 ≤1280、总像素 ≤1280×1280 |
| image_sha256 | 上传图实际字节 SHA-256（服务端重算验证） |
| age_group | 3-4 / 5-6 / 7-10 |
| story_length | short / standard / long |
| voice_id | P0 仅 warm_female_01 |
| language | P0 仅 zh-CN |

- 同键同内容 → 返回原任务（运行中 202，终态 200）
- 同键不同内容 → 409 IDEMPOTENCY_CONFLICT
- 每设备并发 1：存在 queued/running 任务时 → 409 DEVICE_BUSY（携带该 job_id）
- 每设备每日额度耗尽 → 429 QUOTA_EXCEEDED
- multipart 总量上限 1 MiB+16 KiB（Content-Length 超限直接 413）

## 状态与阶段

- status：`queued / running / succeeded / failed / rejected / expired`
- stage：`queued / understanding / story_generating / tts_generating / ready`
- 同一 attempt 内阶段只前进；`(attempt, revision)` 单调，客户端拒绝回退。
- 终止 24h 后媒体删除、status=expired；幂等摘要保留 7 天。

## 错误信封（统一）

```json
{ "error": { "code": "TTS_FAILED", "retryable": true, "stage": "tts_generating", "job_id": "job_example_001" } }
```

契约内业务码：`AUTH_FAILED(401/403)`、`IMAGE_INVALID(413/422)`、
`IDEMPOTENCY_CONFLICT(409)`、`DEVICE_BUSY(409)`、`QUOTA_EXCEEDED(429)`、
`RESULT_NOT_READY(409)`、`RESULT_EXPIRED(410)`、`CONTENT_REJECTED`、
`AI_FAILED`、`TTS_FAILED`、`PROVIDER_RESULT_UNKNOWN`（任务查询体内）。

服务端扩展码（设备按通用文案兜底，已记录于实施计划 10 节表外）：
`JOB_NOT_FOUND(404)`、`INVALID_REQUEST(422)`、`NOT_RETRYABLE(409)`、
`STALE_ATTEMPT(409)`、`AUDIO_TOO_LONG`。

## retry 语义（§7.4）

- 请求体 `{"expected_attempt": N}`；新的独立 Idempotency-Key（恢复命令 ID）
- 允许：`retryable=true` 的失败，或 `PROVIDER_RESULT_UNKNOWN`（仅显式）
- 拒绝：内容拒绝/非法输入/过期任务 → 409 NOT_RETRYABLE / 410 RESULT_EXPIRED
- 过期 expected_attempt → 409 STALE_ATTEMPT（客户端随后 GET 当前状态）
- 恢复点：正文已持久化 → 只重做 TTS；否则整条管线重跑（安全检查不跳过）

## 音频契约

MP3、单声道、24kHz、64kbps CBR，≤240 秒、≤2 MiB。供应商直接产出
~24kHz/mono/~64kbps；不符时服务端 ffmpeg 转码（无 ffmpeg 且规格不符
则 TTS_FAILED）。下载前核对 result 描述、下载后核对字节与 SHA-256。

## 超时与退避（§7.5 初始值，已按此实现）

| 项目 | 值 |
|---|---|
| TCP/TLS 建连 | 10s |
| 上传 | 单次 30s |
| 状态轮询 | 遵守 poll_after（2s 起步） |
| 音频下载 | 单次 30s，校验失败从头重下 |
| 服务端单 attempt | 120s 截止 |
| 自动重试 | 每阶段额外 ≤2 次，1s/3s 退避+抖动 |

## 供应商映射（服务端配置，密钥不出服务端）

- 视觉理解 + 故事：智谱 bigmodel `glm-4.5v`（thinking 关闭，强制 JSON 输出；
  容错解析围栏/字符串内换行）。理解阶段输出 `{suitable, reason, description}`；
  不适宜直接 CONTENT_REJECTED，不做 TTS。
- TTS：小米 MiMo `mimo-v2.5-tts`（chat audio 模态：文本→assistant 消息，
  风格→user 消息，`audio: {voice, format: "mp3"}`，响应内 base64 MP3）。
  产品逻辑声音 `warm_female_01` → 供应商音色由 `SC_VOICE_MAP` 配置
  （默认 mimo_default）。

## 启动与运维

```bash
cd server
python3 -m venv ../.venv && ../.venv/bin/pip install -r requirements.txt
export SC_DATA_DIR=./data
export SC_DEVICE_TOKENS="<token>:<device_id>"
export SC_BIGMODEL_API_KEY=...      # 智谱
export SC_MIMO_API_KEY=...          # 小米 MiMo
../.venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8080
```

测试：`../.venv/bin/python -m pytest tests/`（mock 供应商，21 例）
真实端到端冒烟：`../.venv/bin/python scripts/smoke_real.py`

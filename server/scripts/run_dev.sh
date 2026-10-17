#!/bin/bash
# 局域网开发联调：HTTPS 0.0.0.0:8443，证书由 scripts/gen_dev_certs.sh 生成（固件需信任其 CA）。
# 读取 server/.env.local（供应商密钥）与 server/dev-token.env（SC_DEV_TOKEN），
# 两者均不入库。日志写 stdout，由调用方重定向。
set -euo pipefail
cd "$(dirname "$0")/.."
set -a
. ./.env.local
. ./dev-token.env
set +a
export SC_DATA_DIR=${SC_DATA_DIR:-./data}
export SC_PROVIDER_MODE=${SC_PROVIDER_MODE:-real}
export SC_DEVICE_TOKENS="${SC_DEV_TOKEN}:storycam-dev-01"
UVICORN=${UVICORN:-$( [ -x .venv/bin/uvicorn ] && echo .venv/bin/uvicorn || echo uvicorn )}
exec "$UVICORN" app.main:app --host 0.0.0.0 --port 8443 \
  --ssl-certfile server.pem --ssl-keyfile server.key

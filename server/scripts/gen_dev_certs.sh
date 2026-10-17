#!/bin/bash
# 生成开发联调用自签名 CA 与服务端证书（仅局域网开发；量产走公共 CA + 证书包）。
# 用法：
#   scripts/gen_dev_certs.sh [主机...]            新建 CA + 服务端证书
#   scripts/gen_dev_certs.sh --keep-ca [主机...]  沿用现有 CA，只重签服务端证书（固件无需重烧）
# 主机可写 IP 或域名（至少一个，如运行后端的电脑的局域网 IP）；localhost 总会加上。
# 设备"设置 → 服务器"里填的地址必须在这份证书里，否则 TLS 校验失败。
# 产物在 server/（均不入库）：ca.pem/ca.key、server.pem/server.key；新建 CA 时复制到固件
# components/api_client/server_ca.pem，固件需开启 CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE 才会信任它
# （见根目录 README "Local development with a self-signed certificate"）。
set -euo pipefail
KEEP_CA=0
if [ "${1:-}" = "--keep-ca" ]; then KEEP_CA=1; shift; fi
[ $# -eq 0 ] && { echo "用法: $0 [--keep-ca] <IP或域名>..." >&2; exit 1; }
cd "$(dirname "$0")/.."

if [ $KEEP_CA -eq 0 ]; then
  openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
    -keyout ca.key -out ca.pem -subj "/CN=storycam-dev-ca"
  cp ca.pem ../firmware/components/api_client/server_ca.pem
elif [ ! -f ca.key ] || [ ! -f ca.pem ]; then
  echo "--keep-ca 需要现有的 ca.pem/ca.key" >&2; exit 1
fi

SAN="DNS:localhost"
for H in "$@"; do
  if [[ $H =~ ^[0-9]+(\.[0-9]+){3}$ ]]; then SAN="$SAN,IP:$H"; else SAN="$SAN,DNS:$H"; fi
done
printf 'subjectAltName=%s\nextendedKeyUsage=serverAuth\n' "$SAN" > san.cnf
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr \
  -subj "/CN=storycam-dev-server"
openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
  -days 825 -extfile san.cnf -out server.pem

openssl x509 -in server.pem -noout -subject -enddate -ext subjectAltName

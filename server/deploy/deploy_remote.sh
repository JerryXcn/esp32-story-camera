#!/bin/bash
# 把后端部署/更新到远程服务器（Debian 13）：代码 → /opt/storybox，systemd + nginx 38800。
# 用法：STORY_DOMAIN=story.example.com server/deploy/deploy_remote.sh root@<服务器>
# 首次部署前在服务器上准备 /etc/storybox/storybox.env（API 密钥、设备令牌，权限 640 root:storybox），
# 内容见 server/README.md；本脚本不传任何密钥。
set -euo pipefail
HOST=${1:?用法: STORY_DOMAIN=<域名> $0 root@<服务器>}
DOMAIN=${STORY_DOMAIN:-story.example.com}
cd "$(dirname "$0")/.."
COPYFILE_DISABLE=1 tar --exclude='__pycache__' -czf - app requirements.txt deploy | ssh "$HOST" '
set -e
export DEBIAN_FRONTEND=noninteractive
command -v ffmpeg >/dev/null || apt-get install -y -qq --no-install-recommends ffmpeg python3-venv </dev/null
id storybox >/dev/null 2>&1 || useradd --system --home-dir /var/lib/storybox --shell /usr/sbin/nologin storybox
mkdir -p /opt/storybox/server /var/lib/storybox
chown storybox:storybox /var/lib/storybox && chmod 750 /var/lib/storybox
test -f /etc/storybox/storybox.env || { echo "missing /etc/storybox/storybox.env"; exit 1; }
rm -rf /opt/storybox/server/app && tar -xzf - -C /opt/storybox/server 2>/dev/null
cd /opt/storybox
test -x venv/bin/python || python3 -m venv venv
grep -v "^pytest" server/requirements.txt > /tmp/storybox-req.txt
venv/bin/pip install -q -r /tmp/storybox-req.txt
install -m 644 server/deploy/storybox.service /etc/systemd/system/storybox.service
sed "s/story.example.com/'"$DOMAIN"'/g" server/deploy/nginx-storybox.conf > /etc/nginx/sites-available/storybox
ln -sf /etc/nginx/sites-available/storybox /etc/nginx/sites-enabled/storybox
nginx -t
ufw allow 38800/tcp comment "story camera backend" >/dev/null
systemctl daemon-reload
systemctl enable --now storybox >/dev/null 2>&1
systemctl restart storybox
systemctl reload nginx
sleep 2
systemctl is-active storybox
'
curl -s -m 10 "https://$DOMAIN:38800/v1/health"; echo

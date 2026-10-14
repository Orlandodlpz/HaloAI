#!/usr/bin/env bash
# Deploy origin/main to the Oracle server (only reviewed code, never local changes).
#
# Run from anywhere inside the repo, on a machine with the server's SSH key:
#   pipeline/scripts/deploy.sh
# Overrides: HALOAI_HOST (default ubuntu@api.haloai-cov.io), HALOAI_SSH_KEY (default ~/.ssh/haloai_oracle)
#
# What it does: copies pipeline/ from origin/main, swaps it in at /opt/haloai/app, installs
# dependencies into /opt/haloai/venv, runs migrations, installs the systemd unit and Caddyfile,
# restarts the backend and checks /health. Secrets stay in /opt/haloai/env/haloai.env on the server.
set -euo pipefail

HOST="${HALOAI_HOST:-ubuntu@api.haloai-cov.io}"
KEY="${HALOAI_SSH_KEY:-$HOME/.ssh/haloai_oracle}"
SSH=(ssh -i "$KEY" -o BatchMode=yes -o StrictHostKeyChecking=accept-new "$HOST")

cd "$(git rev-parse --show-toplevel)"
git fetch -q origin main
SHA="$(git rev-parse --short origin/main)"
echo "deploying origin/main $SHA to $HOST"

git archive --format=tar.gz origin/main pipeline | "${SSH[@]}" "cat > /tmp/haloai-$SHA.tgz"

"${SSH[@]}" "sudo bash -s -- $SHA" <<'REMOTE'
set -euo pipefail
SHA="$1"
APP=/opt/haloai/app
ENV=/opt/haloai/env/haloai.env

rm -rf "$APP.new" && install -d -o haloai -g haloai "$APP.new"
sudo -u haloai tar -xzf "/tmp/haloai-$SHA.tgz" -C "$APP.new"
echo "$SHA" > "$APP.new/DEPLOYED_SHA"
rm -f "/tmp/haloai-$SHA.tgz"
if [ -d "$APP" ]; then mv "$APP" "$APP.old"; fi
mv "$APP.new" "$APP"
rm -rf "$APP.old"

sudo -u haloai /opt/haloai/venv/bin/pip install -q -e "$APP/pipeline"
sudo -u haloai bash -c "set -a; . $ENV; set +a; cd $APP/pipeline && /opt/haloai/venv/bin/python scripts/migrate.py"

install -m 644 "$APP/pipeline/deploy/haloai.service" /etc/systemd/system/haloai.service
systemctl daemon-reload
systemctl enable -q haloai
systemctl restart haloai

if ! cmp -s "$APP/pipeline/deploy/Caddyfile" /etc/caddy/Caddyfile; then
  caddy validate --config "$APP/pipeline/deploy/Caddyfile" --adapter caddyfile >/dev/null 2>&1
  install -m 644 "$APP/pipeline/deploy/Caddyfile" /etc/caddy/Caddyfile
  systemctl reload caddy
fi

# Health check on the machine itself, with the key read on the server (never printed).
KEYVAL="$(grep '^BACKEND_API_KEY=' "$ENV" | cut -d= -f2-)"
for i in $(seq 1 30); do
  if curl -fsS -H "Authorization: Bearer $KEYVAL" http://127.0.0.1:8000/health >/dev/null 2>&1; then
    echo "deployed $SHA: backend healthy"; exit 0
  fi
  sleep 1
done
echo "deployed $SHA but /health did not answer; see: journalctl -u haloai -n 50" >&2
exit 1
REMOTE

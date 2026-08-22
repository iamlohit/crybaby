#!/usr/bin/env bash
# crybaby classifier — idempotent install for any Linux box.
# Usage: ./install.sh [--port 8977]   (run from the server/ directory, as your user)
set -euo pipefail
PORT="${2:-8977}"
DIR="$HOME/crybaby-server"

echo "==> sync sources to $DIR"
mkdir -p "$DIR" && cp -r ./. "$DIR"/ && cd "$DIR"

echo "==> python venv + deps"
python3 -m venv venv 2>/dev/null || true
venv/bin/pip install -q --upgrade pip
venv/bin/pip install -q -r requirements.txt

echo "==> train model (skipped if model.joblib exists; delete it to retrain)"
[ -f model.joblib ] || venv/bin/python train.py

echo "==> systemd user service"
mkdir -p "$HOME/.config/systemd/user"
cp systemd/crybaby.service "$HOME/.config/systemd/user/"
systemctl --user daemon-reload
systemctl --user enable --now crybaby
loginctl enable-linger "$USER" 2>/dev/null || echo "   (run: sudo loginctl enable-linger $USER — keeps service up after logout)"

echo "==> firewall (open LAN access yourself, e.g.:)"
echo "    sudo ufw allow from 192.168.0.0/16 to any port $PORT proto tcp"

sleep 3
curl -s "http://localhost:$PORT/health" && echo && echo "==> DONE"

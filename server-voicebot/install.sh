#!/usr/bin/env bash
# crybaby voicebot — idempotent install for any Linux box.
# Usage: ./install.sh [--port 8978]   (run from the server-voicebot/ directory, as your user)
set -euo pipefail
PORT="${2:-8978}"
DIR="$HOME/crybaby-voicebot"

echo "==> sync sources to $DIR"
mkdir -p "$DIR" && cp -r ./. "$DIR"/ && cd "$DIR"

if [ ! -f context/README.md ] || [ ! -f context/HARDWARE.md ]; then
    echo "!! missing context/README.md and/or context/HARDWARE.md" >&2
    echo "   copy them in first, e.g. from the repo root:" >&2
    echo "     cp README.md docs/HARDWARE.md server-voicebot/context/" >&2
    exit 1
fi

echo "==> python venv + deps"
python3 -m venv venv 2>/dev/null || true
venv/bin/pip install -q --upgrade pip
venv/bin/pip install -q -r requirements.txt

echo "==> piper voice model (skipped if already present)"
mkdir -p voices
if [ ! -f "voices/${PIPER_VOICE:-en_US-hfc_female-medium}.onnx" ]; then
    venv/bin/python -m piper.download_voices --download-dir voices "${PIPER_VOICE:-en_US-hfc_female-medium}"
fi

echo "==> kokoro model (fp32 — int8 is slower than real-time on desktop CPUs; skipped if present)"
if [ ! -f voices/kokoro-v1.0.onnx ]; then
    wget -q -O voices/kokoro-v1.0.onnx https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/kokoro-v1.0.onnx
fi
if [ ! -f voices/voices-v1.0.bin ]; then
    wget -q -O voices/voices-v1.0.bin https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/voices-v1.0.bin
fi

if ! command -v claude >/dev/null 2>&1 && [ ! -x "$HOME/.local/bin/claude" ]; then
    echo "!! claude CLI not found on PATH or in ~/.local/bin — /talk will fail until it's installed"
    echo "   (install it, or set CLAUDE_BIN in .env to its path)"
fi

echo "==> systemd user service"
mkdir -p "$HOME/.config/systemd/user"
cp systemd/crybaby-voicebot.service "$HOME/.config/systemd/user/"
systemctl --user daemon-reload
# enable --now is a no-op on an already-running service — restart to pick up new code/config
systemctl --user enable crybaby-voicebot
systemctl --user restart crybaby-voicebot
loginctl enable-linger "$USER" 2>/dev/null || echo "   (run: sudo loginctl enable-linger $USER — keeps service up after logout)"

echo "==> firewall (open LAN access yourself, e.g.:)"
echo "    sudo ufw allow from 192.168.0.0/16 to any port $PORT proto tcp"

sleep 3
curl -s "http://localhost:$PORT/health" && echo && echo "==> DONE"

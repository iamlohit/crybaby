# Voicebot service (any Linux box on your LAN, port 8978)

Push-to-talk chat with the device about its own implementation. STT via
faster-whisper, replies via `claude -p --model haiku` (headless, resumed across turns
so it remembers the conversation), TTS via Piper. See
[`docs/VOICEBOT.md`](../docs/VOICEBOT.md) for the full design.

The system prompt is this project's own docs (README + docs/HARDWARE.md) plus a live
read of the deployed classifier's model card — the bot answers from the actual repo,
not a hand-written script.

## Deploy

Needs its own copy of the project docs alongside the code (kept out of git —
`context/*.md` is gitignored — so it never drifts from a stale committed copy):

**On the box** (from the repo root):
```bash
cd server-voicebot
mkdir -p context && cp ../README.md ../docs/HARDWARE.md context/
bash install.sh
```

**From afar** (repo root on your laptop; `yourserver` = the box that runs it):
```bash
rsync -a server-voicebot/ yourserver:crybaby-voicebot/
rsync -a README.md docs/HARDWARE.md yourserver:crybaby-voicebot/context/
ssh yourserver 'cd crybaby-voicebot && bash install.sh'
ssh yourserver 'sudo ufw allow from 192.168.0.0/16 to any port 8978 proto tcp'  # your LAN
```

Requires the `claude` CLI already installed and logged in on the box (voicebot shells
out to it headless — no ANTHROPIC_API_KEY needed if you're already authenticated
interactively). `install.sh` warns but doesn't fail if it's missing.

## Test

```bash
curl -s http://your-server-ip:8978/health
curl -s --data-binary @question.wav -H 'Content-Type: audio/wav' \
  http://your-server-ip:8978/talk -o reply.wav -D -
# -D - dumps the X-Transcript / X-Reply-Text response headers (base64-encoded)
```

## Config (env vars, or `.env` next to app.py — same pattern as `server/`)

| Var | Default | What |
|---|---|---|
| `CLAUDE_BIN` | `claude` on PATH, else `~/.local/bin/claude` | path to the CLI |
| `CLAUDE_MODEL` | `haiku` | model alias passed to `claude -p --model` |
| `WHISPER_MODEL` | `base` | faster-whisper model size |
| `PIPER_VOICE` | `en_US-lessac-medium` | voice name (must be downloaded into `voices/`) |
| `PIPER_VOICE_DIR` | `voices/` next to app.py | where voice models are looked up |
| `PIPER_BIN` | `venv/bin/piper` | path to the Piper binary |
| `CLASSIFIER_MODEL_PATH` | `~/crybaby-server/model.joblib` | where to read the live model card from |

## Known gap

The first `claude -p` call after the 1-hour prompt-cache window expires costs noticeably
more (re-creates the ~60k-token default CLI system prompt) — seen ~$0.12–0.14 cold vs.
~$0.02–0.03 warm in testing. Fine for a toy used a few times a day; `--bare` mode would
avoid it but requires switching auth to a raw `ANTHROPIC_API_KEY` instead of the CLI's
logged-in session.

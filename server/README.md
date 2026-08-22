# Classifier service (any Linux box on your LAN, port 8977)

Two-stage pipeline: YAMNet cry-gate → logistic regression over YAMNet embeddings
trained on donateacry (5 classes). Optional Gemini second opinion via
`GEMINI_API_KEY` in `~/baby-voice-server/.env`.

## Deploy

```bash
rsync -a server/ yourserver:baby-voice-server/
ssh yourserver 'cd baby-voice-server && python3 -m venv venv && venv/bin/pip install -r requirements.txt'
ssh yourserver 'cd baby-voice-server && venv/bin/python train.py'   # clones corpus, ~5 min
ssh yourserver 'mkdir -p .config/systemd/user && cp baby-voice-server/systemd/baby-voice.service .config/systemd/user/ && systemctl --user daemon-reload && systemctl --user enable --now baby-voice'
```

## Test

```bash
curl -s http://your-server-ip:8977/health
curl -s --data-binary @cry.wav -H 'Content-Type: audio/wav' http://your-server-ip:8977/analyze
```

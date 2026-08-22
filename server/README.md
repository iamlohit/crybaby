# Classifier service (any Linux box on your LAN, port 8977)

Two-stage pipeline: YAMNet cry-gate → logistic regression over YAMNet embeddings
trained on donateacry (5 classes). Optional Gemini second opinion via
`GEMINI_API_KEY` in `~/crybaby-server/.env`.

## Deploy

```bash
rsync -a server/ yourserver:crybaby-server/
ssh yourserver 'cd crybaby-server && python3 -m venv venv && venv/bin/pip install -r requirements.txt'
ssh yourserver 'cd crybaby-server && venv/bin/python train.py'   # clones corpus, ~5 min
ssh yourserver 'mkdir -p .config/systemd/user && cp crybaby-server/systemd/crybaby.service .config/systemd/user/ && systemctl --user daemon-reload && systemctl --user enable --now crybaby'
```

## Test

```bash
curl -s http://your-server-ip:8977/health
curl -s --data-binary @cry.wav -H 'Content-Type: audio/wav' http://your-server-ip:8977/analyze
```

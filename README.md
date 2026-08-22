<div align="center">

# 👶 crybaby

**Your baby is telling you something. crybaby translates.**

A push-button baby-cry analyzer reflashed onto a $15 AI toy — press the button, get the likely reason on screen.

Press a button when your baby cries → the device records 7 seconds → a two-stage
audio-ML pipeline tells you the most likely reason — *hungry, tired, uncomfortable,
belly pain/gas, or needs burping* — right on its little screen.

[![ESP32-S3](https://img.shields.io/badge/ESP32--S3-ESP--IDF%205.5-red?logo=espressif)](firmware/)
[![Server](https://img.shields.io/badge/classifier-FastAPI%20%2B%20YAMNet-009688?logo=fastapi)](server/)
[![UI](https://img.shields.io/badge/UI-LVGL%209-blue)](firmware/main/ui.c)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

</div>

---

## How it works

```mermaid
sequenceDiagram
    participant P as Parent
    participant D as Device (ESP32-S3)
    participant S as Classifier server
    P->>D: press button
    D->>D: record 7 s @ 16 kHz (I2S MEMS mic)
    D->>S: POST WAV over Wi-Fi
    S->>S: Stage 1 — YAMNet gate: "is this a baby cry?"
    S->>S: Stage 2 — rank 5 causes (YAMNet embeddings + classifier)
    S-->>D: JSON: ranked causes + confidence
    D->>P: verdict + confidence bars on 240×240 LCD
```

- **Stage 1 (cry gate)** — [YAMNet](https://tfhub.dev/google/yamnet/1) rejects TV noise,
  speech, and silence so you never get a confident-sounding answer to a non-cry.
- **Stage 2 (cause ranking)** — logistic regression over YAMNet embeddings, trained on the
  [donateacry corpus](https://github.com/gveres/donateacry-corpus) (5 labeled cry causes).
- Optional **LLM second opinion** — set `GEMINI_API_KEY` and each response also carries an
  independent [Gemini audio-understanding](https://ai.google.dev/gemini-api/docs/interactions/audio) verdict.

## The hardware

Built for the **Zhengchen ZC-1.54TFT-WiFi** — a ubiquitous ~$15 "xiaozhi" AI-chatbot toy
(ESP32-S3, 1.54" 240×240 LCD, I2S mic, speaker, battery, three buttons), fully reflashed.
Any ESP32-S3 board with an I2S mic and SPI LCD works with pin changes in
[`firmware/main/board.h`](firmware/main/board.h). Full teardown: [docs/HARDWARE.md](docs/HARDWARE.md).

## Quick start

### 1. Classifier server (any Linux box on your LAN)

```bash
rsync -a server/ yourserver:baby-voice-server/
ssh yourserver
cd baby-voice-server
python3 -m venv venv && venv/bin/pip install -r requirements.txt
venv/bin/python train.py            # clones the corpus, trains, prints honest CV metrics
venv/bin/uvicorn app:app --host 0.0.0.0 --port 8977
# open firewall: e.g. sudo ufw allow from 192.168.0.0/16 to any port 8977 proto tcp
curl http://yourserver:8977/health  # {"ok": true, "model_loaded": true}
```

### 2. Firmware

```bash
cd firmware
cp main/secrets.h.example main/secrets.h   # your Wi-Fi + server URL (gitignored)
idf.py set-target esp32s3 && idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

**Device won't enter download mode?** These toys hide the BOOT strap on the main/middle
button: *hold middle → tap power → keep holding 2 s → release*. Flaky USB link? See the
battle-tested procedure in [docs/FLASHING.md](docs/FLASHING.md).

## Honest accuracy ⚠️

Read this before trusting the gadget: the only public training data
([donateacry](https://github.com/gveres/donateacry-corpus)) is small (457 clips) and
heavily skewed (84 % "hungry"). Our stratified 5-fold CV:

| Class | F1 | Training clips |
|---|---|---|
| hungry | **0.84** | 382 |
| discomfort | 0.17 | 27 |
| belly pain | 0.21 | 16 |
| tired | 0.04 | 24 |
| burping | 0.00 | 8 |

Papers claiming 90 %+ on this dataset are typically inflated by augmentation leakage.
Treat the output as a **ranked suggestion, never a diagnosis** — and the cry-gate ensures
at least you're only ever ranking actual cries. Improving the minority classes
(augmentation, frame-voting, more data) is the roadmap's top item.

## Repo map

| Path | What |
|---|---|
| [`firmware/`](firmware/) | ESP-IDF 5.5 app — I2S capture, Wi-Fi POST, LVGL UI |
| [`server/`](server/) | FastAPI classifier — YAMNet gate + trained model + optional Gemini |
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | Device teardown: chip, pin map, flash layout |
| [`docs/FLASHING.md`](docs/FLASHING.md) | Download-mode entry + flashing over unstable USB |
| [`docs/RESEARCH.md`](docs/RESEARCH.md) | Prior art: ChatterBaby, Dunstan, datasets, model benchmarks |

## Roadmap

- [ ] Minority-class accuracy: augmentation + per-frame voting
- [ ] Gemini second-opinion fusion on the result screen
- [ ] History screen (last N analyses) using the spare flash partition
- [ ] On-device TinyML fallback for Wi-Fi-less operation (ESP32-S3 has the headroom)
- [ ] Speaker feedback (device has an unused I2S amp)

## Acknowledgments

- [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) — board definitions for the donor device
- [donateacry-corpus](https://github.com/gveres/donateacry-corpus) — the dataset making any of this possible
- [YAMNet](https://tfhub.dev/google/yamnet/1) / AudioSet — the audio backbone
- UCLA's [ChatterBaby](https://chatterbaby.org/) — prior art proving the concept

## License

[MIT](LICENSE) — built with care, shipped with honesty. Not a medical device.

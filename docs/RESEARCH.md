# Baby cry classification — problem-space research (2026-08-22)

## TL;DR

- Cry **detection** (is a baby crying?) is a solved problem — trivial even on-device.
- Cry **cause classification** (hungry vs gas vs pain vs tired) is *partially* solved:
  commercial and academic systems exist and claim 88–96% accuracy, but on small,
  parent-labeled datasets. Real-world accuracy is lower and honest framing matters:
  labels in every public dataset are a parent's *guess* at the cause.
- **Deepgram is the wrong tool** — it is speech-to-text (words). A cry has no words.
  The right cloud tools are audio-*classification* models or audio-understanding LLMs.
- Recommended architecture: device records 7 s → POSTs WAV to a thin cloud endpoint →
  two-stage pipeline (cry-gate, then classifier) → result string back to the screen.

## Is this a solved problem? Who has tried?

| Effort | Approach | Classes | Claimed accuracy |
|---|---|---|---|
| [ChatterBaby (UCLA)](https://chatterbaby.org/pages/) | server-side ML, app front-end | pain / hunger / fussy | >90% |
| [Dunstan Baby Language](https://pmc.ncbi.nlm.nih.gov/articles/PMC12804663/) | 5 universal pre-cry sounds (neh=hungry, eh=burp, eairh=gas, owh=tired, heh=discomfort) | 5 | human-coach based; CNN cross-culture validation exists |
| [SE-ResNet-Transformer](https://www.mdpi.com/1424-8220/24/20/6575) | deep net on spectrograms | multi | 92–95% |
| [AlgNet](https://www.sciencedirect.com/science/article/pii/S2307410824000464) | graph+attention | 3–5 | 87.8% (Dunstan), 93.8% (donateacry) |
| [MFCC + Random Forest](https://www.frontiersin.org/journals/artificial-intelligence/articles/10.3389/frai.2024.1337356/full) | classical ML | 5 | 96.4% |
| [E-Nanny](https://www.hackster.io/nafihahmd/e-nanny-esp32-based-smart-baby-sleep-monitoring-system-9e68fb), [TinyML cry detector](https://www.hackster.io/shahizat/tinyml-baby-cry-detection-using-chatgpt-and-synthetic-data-1e715b) | ESP32/TinyML on-device | cry vs no-cry only | detection only |

Caveats from the systematic reviews: needs overlap (a baby can be hungry *and* want
holding), datasets are tiny and imbalanced, and lab accuracy does not transfer cleanly
to a noisy nursery. Treat output as a ranked suggestion, not a diagnosis.

## Datasets

- **[donateacry-corpus](https://github.com/gveres/donateacry-corpus)** — the standard.
  457 cleaned 7 s WAVs, 5 labels (hungry, tired, discomfort, belly_pain, burping),
  16 kHz predominant. Small, imbalanced (hungry dominates), parent-labeled.
- Dunstan Baby Language dataset (commercial), Baby Chillanto (pathology), ICSD, AudioSet
  (infant-cry clips — powers YAMNet's cry classes).

## Classification back-end options (ranked)

1. **Hosted two-stage pipeline (recommended)** — replicate the
   [ResNet34 + YAMNet-gate reference architecture](https://github.com/Omshinde01/InfantCry_Classification):
   YAMNet first confirms the clip *is* a cry (kills false positives), then a
   donateacry-trained classifier ranks causes. Run it on a small server or borrow the
   pre-trained [foduucom/baby-cry-classification](https://huggingface.co/foduucom/baby-cry-classification)
   model behind a thin FastAPI/Worker endpoint. Deterministic, cheap, offline-trainable.
2. **[Gemini audio understanding](https://ai.google.dev/gemini-api/docs/interactions/audio)** —
   accepts raw audio + a prompt; understands paralinguistic/nonverbal audio, so it can be
   prompted with the Dunstan taxonomy. Zero infra, but unbenchmarked for cries and
   nondeterministic. Great as a *second opinion* field alongside option 1.
3. **On-device TinyML (Edge Impulse)** — 5-class MFCC model fits an ESP32(-S3);
   [prior art exists for detection](https://blog.arduino.cc/2023/04/24/detect-a-crying-baby-with-tinyml-and-synthetic-data/).
   No WiFi needed, instant. But retraining/iterating means reflashing, and 5-class
   real-world accuracy on 457 training clips will be noticeably worse than claimed.
   Worth adding *later* as an offline fallback once the cloud loop proves the UX.
4. **Deepgram — ruled out.** ASR/TTS/voice-agent platform; transcribes words. Cries
   contain no words. (Its audio-intelligence features target speech, not audio events.)

## Sources

- https://pmc.ncbi.nlm.nih.gov/articles/PMC12804663/ (systematic review)
- https://chatterbaby.org/pages/
- https://www.mdpi.com/1424-8220/24/20/6575
- https://www.sciencedirect.com/science/article/pii/S2307410824000464
- https://www.frontiersin.org/journals/artificial-intelligence/articles/10.3389/frai.2024.1337356/full
- https://github.com/gveres/donateacry-corpus
- https://github.com/Omshinde01/InfantCry_Classification
- https://huggingface.co/foduucom/baby-cry-classification
- https://ai.google.dev/gemini-api/docs/interactions/audio
- https://www.hackster.io/nafihahmd/e-nanny-esp32-based-smart-baby-sleep-monitoring-system-9e68fb
- https://www.hackster.io/shahizat/tinyml-baby-cry-detection-using-chatgpt-and-synthetic-data-1e715b
- https://blog.arduino.cc/2023/04/24/detect-a-crying-baby-with-tinyml-and-synthetic-data/

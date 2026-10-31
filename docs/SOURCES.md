# Sources log — voice/TTS/model ideas from YouTube etc.

Running collection of external sources (mostly YouTube) with model/tool ideas relevant to
this project — voicebot TTS/STT, cry classifier models, on-device ML. Not vetted or acted
on; just capture + one-line relevance so a future session can triage. Promote anything
actually adopted into `docs/VOICEBOT.md` or `docs/RESEARCH.md` with a DECISIONS.md entry.

## TTS / voice

- **Qwen3 TTS** (ComfyUI workflow) — YouTube, 24 Jan 2026, 462K views (URL not given, title/
  description only). Voice cloning + AI voice design + emotion control, via
  [ComfyUI-Qwen-TTS](https://github.com/flybirdxx/ComfyUI-Qwen-TTS) or the official
  [`qwen-tts`](https://pypi.org/project/qwen-tts/) pip package (Apache 2.0, Alibaba Qwen
  team). Model IDs: `Qwen/Qwen3-TTS-12Hz-{0.6B,1.7B}-{Base,CustomVoice,VoiceDesign}` on HF.

  **TESTED on the home server, 2026-08-23 — runs, but not viable as Piper's replacement:**
  - the home server has no NVIDIA GPU (only a 2009 AMD Radeon HD 4770, no ROCm) → CPU-only. The
    *official* Qwen3-TTS code path is documented CUDA-only elsewhere online, but the
    `qwen-tts` pip package in fact loads and runs fine on plain CPU (`device_map="cpu"`,
    `dtype=torch.float32`) — no crash, no CUDA required.
  - Did a real round trip: installed `qwen-tts` in an isolated venv, downloaded the
    **0.6B Base** checkpoint (2.5 GB), generated a Piper-synthesized reference clip,
    voice-cloned a new sentence from it, then fed the output back through faster-whisper
    — transcript matched the input text almost verbatim, so the audio is genuinely
    intelligible speech, not noise.
  - **Speed: RTF ≈ 5.5x on CPU** (16 cores, no GPU) — 7 s of audio took 38.6 s to generate.
    Model load itself is fast once cached (~7 s). Piper by comparison is <1 s total for a
    typical reply. At this RTF, Qwen3-TTS is roughly 40–60x slower than Piper for the
    voicebot's few-sentence replies — would blow the existing 4–7 s/turn latency budget
    (docs/VOICEBOT.md) by itself.
  - The **0.6B Base checkpoint only does voice cloning** (`generate_voice_clone`, needs a
    reference audio + transcript); it has no built-in speakers and `generate_voice_design`
    / `generate_custom_voice` raise `ValueError` on it — those need the separate
    CustomVoice/VoiceDesign checkpoints (untested here, presumably similar or worse RTF).
  - Minor snag, non-fatal: the package's optional `sox` dependency isn't installed on
    the home server — prints a warning on import but doesn't block anything.

  **Bottom line:** technically runnable on the home server today (no GPU purchase needed), and the
  cloned voice quality/intelligibility is real — but at ~5.5x real-time on CPU it's too
  slow to swap in for Piper as the voicebot's live TTS. Worth revisiting only if (a) the home server
  gets a real GPU, or (b) a future feature can tolerate multi-second-per-sentence TTS
  (e.g. pre-baking a fixed set of prompts/voice lines offline, not live push-to-talk replies).

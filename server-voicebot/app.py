"""crybaby voicebot service.

POST /talk with a WAV body -> WAV body back (device's own voice), plus:
  X-Transcript: base64(utf-8) of what the STT heard
  X-Reply-Text: base64(utf-8) of the LLM's reply text
(headers must be ASCII; the device displays both on screen while it plays the audio)

Pipeline: faster-whisper (STT) -> `claude -p --model haiku` headless, resumed across
turns via one in-memory session id (one device, one conversation) -> Piper (TTS),
resampled to the device's 16 kHz mono speaker rate. System prompt is assembled once at
startup from this project's own docs, so the bot can talk about its own implementation.
"""
import base64
import io
import json
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import threading
import wave

import numpy as np
import soxr
from fastapi import FastAPI, HTTPException, Request, Response
from faster_whisper import WhisperModel

HERE = pathlib.Path(__file__).parent
CONTEXT_DIR = HERE / "context"
VOICE_DIR = pathlib.Path(os.environ.get("PIPER_VOICE_DIR", HERE / "voices"))
PIPER_VOICE = os.environ.get("PIPER_VOICE", "en_US-lessac-medium")
PIPER_BIN = os.environ.get("PIPER_BIN", str(HERE / "venv" / "bin" / "piper"))
CLAUDE_BIN = os.environ.get("CLAUDE_BIN") or shutil.which("claude") or str(pathlib.Path.home() / ".local" / "bin" / "claude")
CLAUDE_MODEL = os.environ.get("CLAUDE_MODEL", "haiku")
WHISPER_MODEL_SIZE = os.environ.get("WHISPER_MODEL", "base")
CLASSIFIER_MODEL_PATH = pathlib.Path(os.environ.get(
    "CLASSIFIER_MODEL_PATH", pathlib.Path.home() / "crybaby-server" / "model.joblib"))
DEVICE_SAMPLE_RATE = 16000
MAX_REPLY_CHARS = 800  # answers are spoken aloud; keep a hard ceiling even if the model rambles
CLAUDE_DISALLOWED_TOOLS = "Bash,Read,Write,Edit,WebFetch,WebSearch,Agent"

app = FastAPI(title="crybaby voicebot")
_whisper = WhisperModel(WHISPER_MODEL_SIZE, device="cpu", compute_type="int8")
_session_lock = threading.Lock()
_session_id: str | None = None


def _read_context(name: str) -> str:
    path = CONTEXT_DIR / name
    try:
        return path.read_text()
    except FileNotFoundError as e:
        raise RuntimeError(
            f"missing {path} — copy the project docs in first, see server-voicebot/README.md"
        ) from e


def _model_card() -> str:
    """Best-effort live read of the deployed classifier bundle; degrades gracefully."""
    try:
        import joblib
        bundle = joblib.load(CLASSIFIER_MODEL_PATH)
        labels = ", ".join(sorted(str(l) for l in bundle["labels"]))
        algo = type(bundle["model"].steps[-1][1]).__name__
        return f"Deployed classifier: {algo} over mean-pooled YAMNet embeddings. Classes: {labels}."
    except Exception as e:
        return f"(live classifier model card unavailable right now: {e})"


def _build_system_prompt() -> str:
    readme = _read_context("README.md")
    hardware = _read_context("HARDWARE.md")
    card = _model_card()
    return (
        "You are the voice of \"crybaby\", a $15 ESP32-S3 toy reflashed into a baby-cry "
        "analyzer. A parent is talking to you through the device's push-to-talk button. "
        "Answer using the reference material below — this project's actual documentation "
        "— and say plainly when something isn't covered by it instead of guessing.\n\n"
        "Answer style: at most 3 short sentences, phrased for text-to-speech playback — "
        "no markdown, no lists, no code blocks, no emoji.\n\n"
        f"=== README.md ===\n{readme}\n\n"
        f"=== docs/HARDWARE.md ===\n{hardware}\n\n"
        f"=== Live classifier model card ===\n{card}"
    )


SYSTEM_PROMPT = _build_system_prompt()


def _pcm16_to_wav_bytes(pcm16: np.ndarray, sample_rate: int) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm16.astype("<i2").tobytes())
    return buf.getvalue()


def _transcribe(wav_bytes: bytes) -> str:
    with tempfile.NamedTemporaryFile(suffix=".wav") as f:
        f.write(wav_bytes)
        f.flush()
        segments, _ = _whisper.transcribe(f.name)
        return " ".join(s.text.strip() for s in segments).strip()


def _ask_claude(question: str) -> str:
    global _session_id
    cmd = [CLAUDE_BIN, "-p", "--model", CLAUDE_MODEL, "--output-format", "json",
           "--disallowedTools", CLAUDE_DISALLOWED_TOOLS]
    with _session_lock:
        sid = _session_id
    if sid:
        cmd += ["--resume", sid]
    else:
        cmd += ["--append-system-prompt", SYSTEM_PROMPT]
    cmd.append(question)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
    if proc.returncode != 0:
        raise RuntimeError(f"claude CLI failed (exit {proc.returncode}): {proc.stderr[-500:]}")
    data = json.loads(proc.stdout)
    if data.get("is_error"):
        raise RuntimeError(f"claude CLI reported an error: {data}")
    with _session_lock:
        _session_id = data.get("session_id", _session_id)
    return (data.get("result") or "").strip()[:MAX_REPLY_CHARS]


def _speakable(text: str) -> str:
    """Strip markdown the model reaches for by habit — it gets read aloud literally
    and shown on a 240x240 screen, where `**bold**` is just noise."""
    text = re.sub(r"```.*?```", " ", text, flags=re.S)
    text = re.sub(r"[*_`#>]+", "", text)
    text = re.sub(r"^\s*[-•]\s*", "", text, flags=re.M)
    text = re.sub(r"\s*\n+\s*", " ", text)
    return re.sub(r"\s{2,}", " ", text).strip()


def _synthesize(text: str) -> bytes:
    voice_model = VOICE_DIR / f"{PIPER_VOICE}.onnx"
    voice_config = VOICE_DIR / f"{PIPER_VOICE}.onnx.json"
    with tempfile.NamedTemporaryFile(suffix=".wav") as f:
        proc = subprocess.run(
            [PIPER_BIN, "-m", str(voice_model), "-c", str(voice_config), "-f", f.name],
            input=text, capture_output=True, text=True, timeout=30,
        )
        if proc.returncode != 0:
            raise RuntimeError(f"piper failed: {proc.stderr[-500:]}")
        with wave.open(f.name, "rb") as w:
            sr = w.getframerate()
            pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    if sr != DEVICE_SAMPLE_RATE:
        pcm_f = pcm.astype(np.float32) / 32768.0
        pcm_f = soxr.resample(pcm_f, sr, DEVICE_SAMPLE_RATE)
        pcm = np.clip(pcm_f * 32768.0, -32768, 32767).astype(np.int16)
    return _pcm16_to_wav_bytes(pcm, DEVICE_SAMPLE_RATE)


@app.get("/health")
def health():
    return {"ok": True, "has_session": _session_id is not None}


@app.post("/talk")
async def talk(request: Request):
    body = await request.body()
    if not body:
        raise HTTPException(400, "empty body")
    transcript = _transcribe(body)
    if not transcript:
        raise HTTPException(422, "couldn't make out any speech")
    reply = _speakable(_ask_claude(transcript))
    wav = _synthesize(reply)
    headers = {
        "X-Transcript": base64.b64encode(transcript.encode()).decode("ascii"),
        "X-Reply-Text": base64.b64encode(reply.encode()).decode("ascii"),
    }
    return Response(content=wav, media_type="audio/wav", headers=headers)

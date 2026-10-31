"""crybaby voicebot service.

POST /talk with a WAV body -> WAV body back (device's own voice), plus:
  X-Transcript: base64(utf-8) of what the STT heard
  X-Reply-Text: base64(utf-8) of the LLM's reply text
(headers must be ASCII; the device displays both on screen while it plays the audio)

Pipeline: faster-whisper (STT) -> persistent Claude Agent SDK client (haiku, one
long-lived process holding the conversation; measured ~2 s/turn vs 15-20 s for the
old per-turn `claude -p --resume` spawn, which re-paid a huge prompt prefill every
turn) -> Kokoro TTS (resident, fp32), resampled to the device's 16 kHz mono speaker
rate. System prompt is assembled once at startup from this project's own docs, so
the bot can talk about its own implementation.

Every /talk call is traced with Langfuse (stt / claude-generation / tts as child spans,
grouped by a per-conversation session id) when LANGFUSE_PUBLIC_KEY/_SECRET_KEY/_HOST
are set in the env; otherwise the SDK client is a no-op — see docs/OBSERVABILITY.md.
"""
import asyncio
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
import uuid
import wave

import numpy as np
import soxr
from fastapi import FastAPI, HTTPException, Request, Response
from faster_whisper import WhisperModel
from langfuse import get_client, propagate_attributes

HERE = pathlib.Path(__file__).parent
CONTEXT_DIR = HERE / "context"
VOICE_DIR = pathlib.Path(os.environ.get("PIPER_VOICE_DIR", HERE / "voices"))
TTS_ENGINE = os.environ.get("TTS_ENGINE", "kokoro")  # "kokoro" | "piper"
KOKORO_VOICE = os.environ.get("KOKORO_VOICE", "af_bella")
KOKORO_SPEED = float(os.environ.get("KOKORO_SPEED", "1.0"))
# fp32 model on purpose: int8 runs SLOWER than real-time on this class of CPU
# (RTF 1.3 vs 0.22 measured on a Ryzen 3700X) — do not "optimise" to the int8 file
KOKORO_MODEL = VOICE_DIR / "kokoro-v1.0.onnx"
KOKORO_VOICES_BIN = VOICE_DIR / "voices-v1.0.bin"
PIPER_VOICE = os.environ.get("PIPER_VOICE", "en_US-hfc_female-medium")
PIPER_LENGTH_SCALE = os.environ.get("PIPER_LENGTH_SCALE")  # >1.0 = slower/clearer, e.g. "1.1"
PIPER_NOISE_SCALE = os.environ.get("PIPER_NOISE_SCALE")    # <0.667 = softer/steadier, e.g. "0.5"
PIPER_BIN = os.environ.get("PIPER_BIN", str(HERE / "venv" / "bin" / "piper"))
CLAUDE_BIN = os.environ.get("CLAUDE_BIN") or shutil.which("claude") or str(pathlib.Path.home() / ".local" / "bin" / "claude")
CLAUDE_MODEL = os.environ.get("CLAUDE_MODEL", "haiku")
WHISPER_MODEL_SIZE = os.environ.get("WHISPER_MODEL", "base")
CLASSIFIER_MODEL_PATH = pathlib.Path(os.environ.get(
    "CLASSIFIER_MODEL_PATH", pathlib.Path.home() / "crybaby-server" / "model.joblib"))
DEVICE_SAMPLE_RATE = 16000
MAX_REPLY_CHARS = 500  # answers are spoken aloud; keep a hard ceiling even if the model rambles
CLAUDE_DISALLOWED_TOOLS = "Bash,Read,Write,Edit,WebFetch,WebSearch,Agent"

# the SDK subprocess finds `claude` via PATH; systemd user units don't have ~/.local/bin
os.environ["PATH"] = str(pathlib.Path(CLAUDE_BIN).parent) + os.pathsep + os.environ.get("PATH", "")
try:
    from claude_agent_sdk import ClaudeSDKClient, ClaudeAgentOptions, AssistantMessage, TextBlock
    _HAVE_SDK = True
except ImportError:
    _HAVE_SDK = False

app = FastAPI(title="crybaby voicebot")
# get_client() reads LANGFUSE_* from the env; with none set every span call below
# becomes a no-op, so this is always safe to construct
langfuse = get_client()
_whisper = WhisperModel(WHISPER_MODEL_SIZE, device="cpu", compute_type="int8")

# Kokoro loads once at startup and stays resident — no per-request model load,
# unlike the Piper subprocess path. Falls back to Piper if anything is missing.
_kokoro = None
if TTS_ENGINE == "kokoro":
    try:
        from kokoro_onnx import Kokoro
        _kokoro = Kokoro(str(KOKORO_MODEL), str(KOKORO_VOICES_BIN))
    except Exception as e:  # missing package or model files → Piper still works
        print(f"kokoro unavailable ({e}); falling back to piper")
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


def _ask_claude_cli(question: str) -> str:
    """Fallback path: per-turn CLI spawn with --resume. Slow (~15-20 s/turn measured —
    every resumed turn re-pays a huge prompt prefill); kept only for boxes without
    the claude-agent-sdk package."""
    global _session_id
    cmd = [CLAUDE_BIN, "-p", "--model", CLAUDE_MODEL, "--output-format", "json",
           "--disallowedTools", CLAUDE_DISALLOWED_TOOLS]
    with _session_lock:
        sid = _session_id
    if sid:
        cmd += ["--resume", sid]
    else:
        cmd += ["--system-prompt", SYSTEM_PROMPT]
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


_sdk_client = None
_sdk_lock = asyncio.Lock()
_conv_id = uuid.uuid4().hex[:8]  # langfuse session id: one per live conversation


async def _ask_claude(question: str) -> str:
    """One persistent SDK client for the device's single conversation. Our compact
    doc-based prompt REPLACES the CLI's huge default system prompt (that prefill was
    the old path's main cost); the live process keeps history, so no --resume."""
    global _sdk_client, _conv_id
    if not _HAVE_SDK:
        return _ask_claude_cli(question)
    async with _sdk_lock:
        for attempt in (0, 1):
            try:
                if _sdk_client is None:
                    opts = ClaudeAgentOptions(
                        model=CLAUDE_MODEL, system_prompt=SYSTEM_PROMPT, max_turns=1,
                        disallowed_tools=CLAUDE_DISALLOWED_TOOLS.split(","))
                    _sdk_client = ClaudeSDKClient(options=opts)
                    await _sdk_client.connect()
                    _conv_id = uuid.uuid4().hex[:8]  # rebuilt client = fresh conversation
                await _sdk_client.query(question)
                text = ""
                async for m in _sdk_client.receive_response():
                    if isinstance(m, AssistantMessage):
                        for b in m.content:
                            if isinstance(b, TextBlock):
                                text += b.text
                return text.strip()[:MAX_REPLY_CHARS]
            except Exception:
                try:
                    await _sdk_client.disconnect()
                except Exception:
                    pass
                _sdk_client = None  # dead process — rebuild once, then give up
                if attempt:
                    raise
    return ""  # unreachable; keeps type checkers calm


def _speakable(text: str) -> str:
    """Strip markdown the model reaches for by habit — it gets read aloud literally
    and shown on a 240x240 screen, where `**bold**` is just noise."""
    text = re.sub(r"```.*?```", " ", text, flags=re.S)
    text = re.sub(r"[*_`#>]+", "", text)
    text = re.sub(r"^\s*[-•]\s*", "", text, flags=re.M)
    text = re.sub(r"\s*\n+\s*", " ", text)
    return re.sub(r"\s{2,}", " ", text).strip()


def _synthesize_piper(text: str) -> tuple[np.ndarray, int]:
    voice_model = VOICE_DIR / f"{PIPER_VOICE}.onnx"
    voice_config = VOICE_DIR / f"{PIPER_VOICE}.onnx.json"
    cmd = [PIPER_BIN, "-m", str(voice_model), "-c", str(voice_config)]
    if PIPER_LENGTH_SCALE:
        cmd += ["--length-scale", PIPER_LENGTH_SCALE]
    if PIPER_NOISE_SCALE:
        cmd += ["--noise-scale", PIPER_NOISE_SCALE]
    with tempfile.NamedTemporaryFile(suffix=".wav") as f:
        proc = subprocess.run(
            cmd + ["-f", f.name],
            input=text, capture_output=True, text=True, timeout=30,
        )
        if proc.returncode != 0:
            raise RuntimeError(f"piper failed: {proc.stderr[-500:]}")
        with wave.open(f.name, "rb") as w:
            sr = w.getframerate()
            pcm = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2")
    return pcm.astype(np.float32) / 32768.0, sr


def _synthesize(text: str) -> bytes:
    if _kokoro:
        samples, sr = _kokoro.create(text, voice=KOKORO_VOICE, speed=KOKORO_SPEED, lang="en-us")
        pcm_f = np.asarray(samples, dtype=np.float32)
    else:
        pcm_f, sr = _synthesize_piper(text)
    if sr != DEVICE_SAMPLE_RATE:
        pcm_f = soxr.resample(pcm_f, sr, DEVICE_SAMPLE_RATE)
    # peak-normalise to ~-1 dBFS: replies land at a consistent, full level on
    # the device's tiny speaker instead of varying with the voice model's output
    peak = float(np.abs(pcm_f).max()) if pcm_f.size else 0.0
    if peak > 1e-4:
        pcm_f *= 0.891 / peak
    pcm = np.clip(pcm_f * 32768.0, -32768, 32767).astype(np.int16)
    return _pcm16_to_wav_bytes(pcm, DEVICE_SAMPLE_RATE)


@app.get("/health")
def health():
    tts = f"kokoro/{KOKORO_VOICE}" if _kokoro else f"piper/{PIPER_VOICE}"
    llm = "sdk-persistent" if _HAVE_SDK else "cli-subprocess"
    has_session = _sdk_client is not None if _HAVE_SDK else _session_id is not None
    return {"ok": True, "has_session": has_session, "tts": tts, "llm": llm}


@app.post("/talk")
async def talk(request: Request):
    body = await request.body()
    if not body:
        raise HTTPException(400, "empty body")
    with propagate_attributes(session_id=_conv_id), \
         langfuse.start_as_current_observation(name="talk") as trace:
        with langfuse.start_as_current_observation(name="stt-whisper") as sp:
            transcript = _transcribe(body)
            sp.update(input={"audio_seconds": round(len(body) / 2 / DEVICE_SAMPLE_RATE, 1)},
                      output=transcript)
        if not transcript:
            raise HTTPException(422, "couldn't make out any speech")
        with langfuse.start_as_current_observation(name="claude", as_type="generation",
                                                   model=CLAUDE_MODEL, input=transcript) as gen:
            reply = _speakable(await _ask_claude(transcript))
            gen.update(output=reply)
        with langfuse.start_as_current_observation(name="tts-kokoro" if _kokoro else "tts-piper") as sp:
            wav = _synthesize(reply)
            sp.update(output={"wav_seconds": round(len(wav) / 2 / DEVICE_SAMPLE_RATE, 1)})
        trace.set_trace_io(input=transcript, output=reply)
    headers = {
        "X-Transcript": base64.b64encode(transcript.encode()).decode("ascii"),
        "X-Reply-Text": base64.b64encode(reply.encode()).decode("ascii"),
    }
    return Response(content=wav, media_type="audio/wav", headers=headers)

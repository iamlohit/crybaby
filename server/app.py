"""crybaby classifier service.

POST /analyze with a WAV body (or multipart 'file') ->
  { is_cry, cry_confidence, results: [{label, prob}], top, ambient, second_opinion? }

Two-stage: YAMNet cry-gate, then a classifier over the embeddings named by the
model bundle (yamnet reuses the gate's embeddings; other encoders — e.g. the
ast winner from benchmark.py --fit — load once at startup). Optional Gemini
audio second opinion when GEMINI_API_KEY is set.

Every /analyze call is traced with Langfuse (cry-gate / classify / second-opinion as
child spans) when LANGFUSE_PUBLIC_KEY/_SECRET_KEY/_HOST are set in the env; otherwise
the SDK client is a no-op and this runs with zero tracing overhead — see
docs/OBSERVABILITY.md for the deployed instance and span shapes.
"""
import base64
import json
import os
import pathlib

import joblib
import numpy as np
from fastapi import FastAPI, Request, UploadFile, HTTPException
from langfuse import get_client

from audio_pipeline import decode_to_16k_mono, yamnet_analyze_frames

CRY_GATE_THRESHOLD = float(os.environ.get("CRY_GATE_THRESHOLD", "0.15"))
MODEL_PATH = pathlib.Path(os.environ.get("MODEL_PATH", pathlib.Path(__file__).parent / "model.joblib"))
GEMINI_MODEL = os.environ.get("GEMINI_MODEL", "gemini-3.6-flash")
GEMINI_LABELS = ("hungry", "tired", "discomfort", "belly_pain", "burping", "unclear")

# label -> parent-facing meaning
LABEL_TEXT = {
    "hungry": "Hungry",
    "tired": "Tired / sleepy",
    "discomfort": "Uncomfortable (diaper, position, temperature)",
    "belly_pain": "Belly pain / gas",
    "burping": "Needs burping",
}

app = FastAPI(title="crybaby classifier")
# get_client() reads LANGFUSE_* from the env; with none set every span call below
# becomes a no-op, so this is always safe to construct
langfuse = get_client()
_bundle = joblib.load(MODEL_PATH) if MODEL_PATH.exists() else None

# bundles from benchmark.py --fit name their encoder; yamnet bundles reuse the
# cry-gate's embeddings, anything else loads its own encoder once at startup
_encode = None
if _bundle is not None and _bundle.get("encoder", "yamnet") != "yamnet":
    from benchmark import ENCODERS
    _encode = ENCODERS[_bundle["encoder"]][0]()


@app.get("/health")
def health():
    return {"ok": True, "model_loaded": _bundle is not None, "gate_threshold": CRY_GATE_THRESHOLD,
            "encoder": _bundle.get("encoder", "yamnet") if _bundle else None}


async def _read_audio(request: Request) -> bytes:
    ctype = request.headers.get("content-type", "")
    if ctype.startswith("multipart/"):
        form = await request.form()
        f = form.get("file")
        if isinstance(f, UploadFile):
            return await f.read()
        raise HTTPException(400, "multipart request needs a 'file' field")
    body = await request.body()
    if not body:
        raise HTTPException(400, "empty body")
    return body


def _gemini_second_opinion(wav_bytes: bytes) -> dict | None:
    key = os.environ.get("GEMINI_API_KEY")
    if not key:
        return None
    import httpx
    prompt = (
        "This is a recording of a baby crying. Using acoustic cues only "
        "(Dunstan-style pre-cry sounds, rhythm, pitch), classify the most likely "
        "cause as one of: hungry, tired, discomfort, belly_pain, burping, or unclear "
        "when the cues do not support any of them. "
        'Reply with JSON only: {"reason": "one short sentence", "label": ..., "confidence": 0-1}'
    )
    payload = {
        "contents": [{"parts": [
            {"text": prompt},
            {"inline_data": {"mime_type": "audio/wav",
                             "data": base64.b64encode(wav_bytes).decode()}},
        ]}],
        "generationConfig": {
            "response_mime_type": "application/json",
            "response_schema": {
                "type": "OBJECT",
                "properties": {
                    "reason": {"type": "STRING"},
                    "label": {"type": "STRING", "enum": list(GEMINI_LABELS)},
                    "confidence": {"type": "NUMBER"},
                },
                "required": ["reason", "label", "confidence"],
                "propertyOrdering": ["reason", "label", "confidence"],
            },
        },
    }
    with langfuse.start_as_current_observation(name="gemini-second-opinion", as_type="generation",
                                               model=GEMINI_MODEL) as gen:
        try:
            r = httpx.post(
                f"https://generativelanguage.googleapis.com/v1beta/models/{GEMINI_MODEL}:generateContent",
                params={"key": key}, json=payload, timeout=45,
            )
            r.raise_for_status()
            text = r.json()["candidates"][0]["content"]["parts"][0]["text"]
            result = json.loads(text)
        except httpx.HTTPStatusError as e:
            # never echo e.request.url back to a client -- it carries ?key=<GEMINI_API_KEY>
            result = {"error": f"gemini http {e.response.status_code}: {e.response.text[:300]}"}
        except Exception as e:
            result = {"error": type(e).__name__}
        gen.update(output=result)
        return result


@app.post("/analyze")
async def analyze(request: Request):
    if _bundle is None:
        raise HTTPException(503, "model not trained yet — run train.py")
    data = await _read_audio(request)
    with langfuse.start_as_current_observation(name="analyze") as trace:
        try:
            mono = decode_to_16k_mono(data)
        except Exception as e:
            raise HTTPException(400, f"could not decode audio: {e}")
        if len(mono) < 16000:
            raise HTTPException(400, "need at least 1 second of audio")

        with langfuse.start_as_current_observation(name="cry-gate-yamnet") as sp:
            cry_conf, frames, ambient = yamnet_analyze_frames(mono)
            sp.update(output={"cry_confidence": round(cry_conf, 3)})
        out = {
            "is_cry": cry_conf >= CRY_GATE_THRESHOLD,
            "cry_confidence": round(cry_conf, 3),
            "ambient": [{"label": n, "score": round(s, 3)} for n, s in ambient],
        }
        if out["is_cry"]:
            model, labels = _bundle["model"], _bundle["labels"]
            with langfuse.start_as_current_observation(name=f"classify-{_bundle.get('encoder', 'yamnet')}") as sp:
                # bundles carry their pooling: frame-voting averages per-frame probabilities
                emb = _encode(mono) if _encode is not None else frames
                X = emb if _bundle.get("pooling") == "frames" else emb.mean(axis=0, keepdims=True)
                probs = model.predict_proba(X).mean(axis=0)
                order = np.argsort(probs)[::-1]
                out["results"] = [
                    {"label": model.classes_[i], "text": LABEL_TEXT.get(model.classes_[i], model.classes_[i]),
                     "prob": round(float(probs[i]), 3)}
                    for i in order
                ]
                out["top"] = out["results"][0]["label"]
                sp.update(output=out["results"])
            so = _gemini_second_opinion(data)
            if so is not None:
                out["second_opinion"] = so
        trace.set_trace_io(
            input={"audio_seconds": round(len(mono) / 16000, 1), "audio_bytes": len(data)},
            output=out,
        )
    return out

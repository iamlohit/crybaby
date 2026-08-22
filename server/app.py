"""baby-voice classifier service.

POST /analyze with a WAV body (or multipart 'file') ->
  { is_cry, cry_confidence, results: [{label, prob}], top, ambient, second_opinion? }

Two-stage: YAMNet cry-gate, then logistic-regression over YAMNet embeddings
(train.py). Optional Gemini audio second opinion when GEMINI_API_KEY is set.
"""
import base64
import json
import os
import pathlib

import joblib
import numpy as np
from fastapi import FastAPI, Request, UploadFile, HTTPException

from audio_pipeline import decode_to_16k_mono, yamnet_analyze

CRY_GATE_THRESHOLD = float(os.environ.get("CRY_GATE_THRESHOLD", "0.15"))
MODEL_PATH = pathlib.Path(os.environ.get("MODEL_PATH", pathlib.Path(__file__).parent / "model.joblib"))
GEMINI_MODEL = os.environ.get("GEMINI_MODEL", "gemini-2.5-flash")

# label -> parent-facing meaning
LABEL_TEXT = {
    "hungry": "Hungry",
    "tired": "Tired / sleepy",
    "discomfort": "Uncomfortable (diaper, position, temperature)",
    "belly_pain": "Belly pain / gas",
    "burping": "Needs burping",
}

app = FastAPI(title="baby-voice classifier")
_bundle = joblib.load(MODEL_PATH) if MODEL_PATH.exists() else None


@app.get("/health")
def health():
    return {"ok": True, "model_loaded": _bundle is not None, "gate_threshold": CRY_GATE_THRESHOLD}


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
        "cause as one of: hungry, tired, discomfort, belly_pain, burping. "
        'Reply with JSON only: {"label": ..., "confidence": 0-1, "reason": "one short sentence"}'
    )
    payload = {
        "contents": [{"parts": [
            {"text": prompt},
            {"inline_data": {"mime_type": "audio/wav",
                             "data": base64.b64encode(wav_bytes).decode()}},
        ]}],
        "generationConfig": {"response_mime_type": "application/json"},
    }
    try:
        r = httpx.post(
            f"https://generativelanguage.googleapis.com/v1beta/models/{GEMINI_MODEL}:generateContent",
            params={"key": key}, json=payload, timeout=20,
        )
        r.raise_for_status()
        text = r.json()["candidates"][0]["content"]["parts"][0]["text"]
        return json.loads(text)
    except Exception as e:
        return {"error": str(e)}


@app.post("/analyze")
async def analyze(request: Request):
    if _bundle is None:
        raise HTTPException(503, "model not trained yet — run train.py")
    data = await _read_audio(request)
    try:
        mono = decode_to_16k_mono(data)
    except Exception as e:
        raise HTTPException(400, f"could not decode audio: {e}")
    if len(mono) < 16000:
        raise HTTPException(400, "need at least 1 second of audio")

    cry_conf, emb, ambient = yamnet_analyze(mono)
    out = {
        "is_cry": cry_conf >= CRY_GATE_THRESHOLD,
        "cry_confidence": round(cry_conf, 3),
        "ambient": [{"label": n, "score": round(s, 3)} for n, s in ambient],
    }
    if out["is_cry"]:
        model, labels = _bundle["model"], _bundle["labels"]
        probs = model.predict_proba([emb])[0]
        order = np.argsort(probs)[::-1]
        out["results"] = [
            {"label": model.classes_[i], "text": LABEL_TEXT.get(model.classes_[i], model.classes_[i]),
             "prob": round(float(probs[i]), 3)}
            for i in order
        ]
        out["top"] = out["results"][0]["label"]
        so = _gemini_second_opinion(data)
        if so is not None:
            out["second_opinion"] = so
    return out

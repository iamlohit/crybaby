"""Shared audio pipeline: decode -> 16 kHz mono float32 -> YAMNet scores/embeddings."""
import io
import csv
import functools
import numpy as np
import soundfile as sf
import soxr

TARGET_SR = 16000

# AudioSet class names treated as "this is a baby cry"
CRY_CLASSES = {"Baby cry, infant cry", "Crying, sobbing", "Whimper"}


def decode_to_16k_mono(data: bytes) -> np.ndarray:
    """Decode WAV/FLAC/OGG bytes to 16 kHz mono float32 in [-1, 1]."""
    wav, sr = sf.read(io.BytesIO(data), dtype="float32", always_2d=True)
    mono = wav.mean(axis=1)
    if sr != TARGET_SR:
        mono = soxr.resample(mono, sr, TARGET_SR)
    return np.ascontiguousarray(mono, dtype=np.float32)


@functools.cache
def _yamnet():
    import tensorflow_hub as hub
    return hub.load("https://tfhub.dev/google/yamnet/1")


@functools.cache
def _class_names() -> list[str]:
    import tensorflow as tf
    model = _yamnet()
    path = model.class_map_path().numpy().decode()
    with tf.io.gfile.GFile(path) as f:
        return [row["display_name"] for row in csv.DictReader(f)]


def yamnet_analyze(mono_16k: np.ndarray):
    """Returns (cry_confidence, mean_embedding[1024], top_ambient[(name, score)...])."""
    scores, embeddings, _ = _yamnet()(mono_16k)
    scores = scores.numpy()          # [frames, 521]
    embeddings = embeddings.numpy()  # [frames, 1024]
    names = _class_names()
    cry_idx = [i for i, n in enumerate(names) if n in CRY_CLASSES]
    # max over frames of summed cry-class scores
    cry_conf = float(scores[:, cry_idx].sum(axis=1).max())
    mean_scores = scores.mean(axis=0)
    top = np.argsort(mean_scores)[::-1][:3]
    ambient = [(names[i], float(mean_scores[i])) for i in top]
    return cry_conf, embeddings.mean(axis=0), ambient

"""Encoder bake-off: which frozen audio embedding best ranks cry causes?

Implements step 1 of docs/ACCURACY-V2.md: every available encoder x head x
pooling (clip-mean vs frame-voting) x augmentation (off / fold-safe), 5-fold
stratified CV split at CLIP level, one per-class-F1 table at the end.

Embeddings are cached per (clip, variant) under --cache-dir, so re-runs and new
heads are cheap. Augmented variants (speed perturb +-10%, added noise at 20/10 dB
SNR) are embedded once and only ever joined to TRAINING folds -- validation
clips stay pristine. That is the "honest gains" rule: papers that augment
before splitting leak augmented copies of validation clips into training.

Encoders `yamnet` and `melstats` run with the server's existing deps. The rest
need extras (see requirements-benchmark.txt):
  ast / wavlm / hubert / wav2vec2 / whisper / clap -> torch + transformers
  ecapa (Ubenwa cry-domain)                        -> + speechbrain
  panns (CNN14)                                    -> + panns_inference
BEATs has no pip-installable checkpoint path; add it here once a torchscript or
HF port is vetted.

Usage:
  python benchmark.py --list-encoders
  python benchmark.py                                  # all available, overnight on CPU
  python benchmark.py --encoders yamnet,wavlm --heads logreg,mlp --augment on
"""
import argparse
import csv
import hashlib
import pathlib
import sys
import time

import numpy as np
import soxr
from sklearn.calibration import CalibratedClassifierCV
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import classification_report
from sklearn.model_selection import StratifiedKFold
from sklearn.neural_network import MLPClassifier
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler
from sklearn.svm import SVC

from audio_pipeline import TARGET_SR, decode_to_16k_mono
from train import ensure_corpus

SEED = 42
FRAME_SEC = 0.48         # YAMNet hop; other time-series encoders are chunk-averaged to match
SVM_MAX_TRAIN = 8000     # RBF-SVM is O(n^2); cap frame-level training rows (seeded subsample)


# ---------------------------------------------------------------- augmentation

def _speed(mono: np.ndarray, rate: float) -> np.ndarray:
    # resample-as-speed: changes duration AND pitch together (kaldi-style speed perturb)
    return soxr.resample(mono, TARGET_SR * rate, TARGET_SR).astype(np.float32)


def _noise(mono: np.ndarray, snr_db: float, seed: int) -> np.ndarray:
    rng = np.random.default_rng(seed)
    noise = rng.standard_normal(len(mono)).astype(np.float32)
    rms = np.sqrt(np.mean(mono**2)) + 1e-9
    noise *= rms / (10 ** (snr_db / 20)) / (np.sqrt(np.mean(noise**2)) + 1e-9)
    return mono + noise


AUG_VARIANTS = {
    "speed0.9": lambda mono, seed: _speed(mono, 0.9),
    "speed1.1": lambda mono, seed: _speed(mono, 1.1),
    "noise20": lambda mono, seed: _noise(mono, 20.0, seed),
    "noise10": lambda mono, seed: _noise(mono, 10.0, seed + 1),
}


# ------------------------------------------------------------------- encoders
# Each factory returns embed(mono_16k) -> float32 [n_frames, dim]; clip-level
# encoders return n_frames == 1 (frame-voting then degenerates to clip-mean and
# is skipped for them).

def chunk_mean(frames: np.ndarray, size: int) -> np.ndarray:
    """Average consecutive rows in windows of `size` -> roughly YAMNet's frame rate."""
    if len(frames) <= size:
        return frames.mean(axis=0, keepdims=True)
    return np.stack([frames[i : i + size].mean(axis=0) for i in range(0, len(frames), size)])


def make_yamnet():
    from audio_pipeline import _yamnet

    model = _yamnet()

    def embed(mono):
        _, embeddings, _ = model(mono)
        return embeddings.numpy()  # [frames, 1024] @ 0.48 s hop

    return embed


def _mel_filterbank(n_mels=64, n_fft=512, fmin=50.0, fmax=8000.0) -> np.ndarray:
    def hz_to_mel(f):
        return 2595.0 * np.log10(1.0 + np.asarray(f) / 700.0)

    def mel_to_hz(m):
        return 700.0 * (10.0 ** (np.asarray(m) / 2595.0) - 1.0)

    pts = mel_to_hz(np.linspace(hz_to_mel(fmin), hz_to_mel(fmax), n_mels + 2))
    bins = np.floor((n_fft + 1) * pts / TARGET_SR).astype(int)
    fb = np.zeros((n_mels, n_fft // 2 + 1), dtype=np.float32)
    for m in range(n_mels):
        lo, mid, hi = bins[m], bins[m + 1], bins[m + 2]
        if mid > lo:
            fb[m, lo:mid] = np.arange(mid - lo) / (mid - lo)
        if hi > mid:
            fb[m, mid:hi] = 1.0 - np.arange(hi - mid) / (hi - mid)
    return fb


def make_melstats():
    """Dependency-free sanity baseline: log-mel mean+std per 0.48 s window (128-d)."""
    n_fft, hop = 512, 160
    fb = _mel_filterbank(n_fft=n_fft)
    window = np.hanning(n_fft).astype(np.float32)

    def embed(mono):
        if len(mono) < n_fft:
            mono = np.pad(mono, (0, n_fft - len(mono)))
        starts = np.arange(0, len(mono) - n_fft + 1, hop)
        frames = np.stack([mono[s : s + n_fft] * window for s in starts])
        power = np.abs(np.fft.rfft(frames, axis=1)) ** 2
        logmel = np.log(power @ fb.T + 1e-6)  # [T, 64] @ 10 ms hop
        per_window = int(FRAME_SEC / (hop / TARGET_SR))
        out = []
        for i in range(0, len(logmel), per_window):
            w = logmel[i : i + per_window]
            out.append(np.concatenate([w.mean(axis=0), w.std(axis=0)]))
        return np.stack(out).astype(np.float32)

    return embed


def make_hf_wav(model_id: str):
    """wav2vec2-family SSL encoders: last_hidden_state @ 50 fps -> 0.5 s chunks."""
    import torch
    from transformers import AutoFeatureExtractor, AutoModel

    fe = AutoFeatureExtractor.from_pretrained(model_id)
    model = AutoModel.from_pretrained(model_id).eval()

    def embed(mono):
        inputs = fe(mono, sampling_rate=TARGET_SR, return_tensors="pt")
        with torch.no_grad():
            h = model(**inputs).last_hidden_state[0].numpy()
        return chunk_mean(h, 25)

    return embed


def make_whisper(model_id="openai/whisper-base"):
    import torch
    from transformers import WhisperFeatureExtractor, WhisperModel

    fe = WhisperFeatureExtractor.from_pretrained(model_id)
    encoder = WhisperModel.from_pretrained(model_id).get_encoder().eval()

    def embed(mono):
        inputs = fe(mono, sampling_rate=TARGET_SR, return_tensors="pt")
        with torch.no_grad():
            h = encoder(inputs.input_features).last_hidden_state[0].numpy()
        valid = max(1, min(len(h), int(np.ceil(len(mono) / TARGET_SR * 50))))  # 30 s zero-pad
        return chunk_mean(h[:valid], 25)

    return embed


def make_ast(model_id="MIT/ast-finetuned-audioset-10-10-0.4593"):
    import torch
    from transformers import ASTFeatureExtractor, ASTModel

    fe = ASTFeatureExtractor.from_pretrained(model_id)
    model = ASTModel.from_pretrained(model_id).eval()

    def embed(mono):
        inputs = fe(mono, sampling_rate=TARGET_SR, return_tensors="pt")
        with torch.no_grad():
            h = model(**inputs).last_hidden_state[0].numpy()
        # patch tokens are freq x time, not a time axis -> clip-level only
        return h.mean(axis=0, keepdims=True)

    return embed


def make_clap(model_id="laion/clap-htsat-unfused"):
    import torch
    from transformers import ClapModel, ClapProcessor

    processor = ClapProcessor.from_pretrained(model_id)
    model = ClapModel.from_pretrained(model_id).eval()

    def embed(mono):
        audio48 = soxr.resample(mono, TARGET_SR, 48000).astype(np.float32)
        inputs = processor(audio=audio48, sampling_rate=48000, return_tensors="pt")
        with torch.no_grad():
            feats = model.get_audio_features(**inputs)
        if not isinstance(feats, torch.Tensor):  # transformers >=5 returns BaseModelOutputWithPooling
            feats = feats.pooler_output
        return feats.numpy()  # [1, 512]

    return embed


def make_ecapa(model_id="Ubenwa/ecapa-voxceleb-ft2-cryceleb"):
    import torch
    from speechbrain.inference.speaker import EncoderClassifier

    model = EncoderClassifier.from_hparams(source=model_id, run_opts={"device": "cpu"})

    def embed(mono):
        with torch.no_grad():
            emb = model.encode_batch(torch.from_numpy(mono)[None])
        return emb.squeeze().numpy()[None, :]  # [1, 192]

    return embed


def make_panns():
    from panns_inference import AudioTagging

    at = AudioTagging(checkpoint_path=None, device="cpu")

    def embed(mono):
        audio32 = soxr.resample(mono, TARGET_SR, 32000).astype(np.float32)
        _, emb = at.inference(audio32[None, :])
        return emb.astype(np.float32)  # [1, 2048]

    return embed


ENCODERS = {
    # name: (factory, required importable modules)
    "yamnet": (make_yamnet, ["tensorflow_hub"]),
    "melstats": (make_melstats, []),
    "ast": (make_ast, ["torch", "transformers"]),
    "wavlm": (lambda: make_hf_wav("microsoft/wavlm-base-plus"), ["torch", "transformers"]),
    "hubert": (lambda: make_hf_wav("facebook/hubert-base-ls960"), ["torch", "transformers"]),
    "wav2vec2": (lambda: make_hf_wav("facebook/wav2vec2-base"), ["torch", "transformers"]),
    "whisper": (make_whisper, ["torch", "transformers"]),
    "clap": (make_clap, ["torch", "transformers"]),
    "ecapa": (make_ecapa, ["torch", "speechbrain"]),
    "panns": (make_panns, ["torch", "panns_inference"]),
}


def encoder_available(name: str) -> bool:
    import importlib.util

    return all(importlib.util.find_spec(m) is not None for m in ENCODERS[name][1])


# ---------------------------------------------------------------------- heads

def make_head(name: str):
    if name == "logreg":
        return LogisticRegression(max_iter=5000, class_weight="balanced", C=1.0)
    if name == "svm":
        # calibrated wrapper gives predict_proba (SVC probability=True is deprecated)
        return CalibratedClassifierCV(
            SVC(kernel="rbf", class_weight="balanced", random_state=SEED), ensemble=False)
    if name == "mlp":
        return MLPClassifier(hidden_layer_sizes=(256,), early_stopping=True,
                             max_iter=500, random_state=SEED)
    if name == "hgb":
        return HistGradientBoostingClassifier(class_weight="balanced", random_state=SEED)
    raise ValueError(f"unknown head {name!r}")


HEADS = ["logreg", "svm", "mlp", "hgb"]


# ----------------------------------------------------------------- extraction

def load_clip_list(corpus: pathlib.Path):
    clips = []  # (path, label)
    for label_dir in sorted(p for p in corpus.iterdir() if p.is_dir()):
        for f in sorted(label_dir.glob("*.wav")):
            clips.append((f, label_dir.name))
    return clips


def cache_path(cache_dir: pathlib.Path, encoder: str, clip: pathlib.Path, variant: str) -> pathlib.Path:
    key = hashlib.sha1(f"{clip.parent.name}/{clip.name}|{variant}".encode()).hexdigest()
    return cache_dir / encoder / f"{key}.npy"


def extract(encoder_name: str, clips, cache_dir: pathlib.Path, with_aug: bool):
    """Returns list of dicts variant -> [frames, dim]; embeds only cache misses."""
    variants = ["orig"] + (list(AUG_VARIANTS) if with_aug else [])
    (cache_dir / encoder_name).mkdir(parents=True, exist_ok=True)
    embed = None
    out, misses = [], 0
    t0 = time.time()
    for n, (path, _label) in enumerate(clips):
        per_clip = {}
        mono = None
        for variant in variants:
            cpath = cache_path(cache_dir, encoder_name, path, variant)
            if cpath.exists():
                per_clip[variant] = np.load(cpath)
                continue
            if embed is None:
                print(f"  loading encoder {encoder_name} ...", flush=True)
                embed = ENCODERS[encoder_name][0]()
            if mono is None:
                mono = decode_to_16k_mono(path.read_bytes())
            wav = mono if variant == "orig" else AUG_VARIANTS[variant](
                mono, int(hashlib.sha1(path.name.encode()).hexdigest()[:8], 16))
            arr = np.asarray(embed(wav), dtype=np.float32)
            np.save(cpath, arr)
            per_clip[variant] = arr
            misses += 1
        out.append(per_clip)
        if (n + 1) % 50 == 0:
            print(f"  {encoder_name}: {n + 1}/{len(clips)} clips ({misses} embedded, "
                  f"{time.time() - t0:.0f}s)", flush=True)
    return out


# ----------------------------------------------------------------- evaluation

def evaluate(clip_embs, y, head_name: str, pooling: str, use_aug: bool, n_folds: int):
    """Clip-level stratified CV; returns per-clip predicted labels."""
    variants = (["orig"] + list(AUG_VARIANTS)) if use_aug else ["orig"]
    preds = np.empty(len(y), dtype=object)
    skf = StratifiedKFold(n_splits=n_folds, shuffle=True, random_state=SEED)
    for tr, te in skf.split(np.zeros(len(y)), y):
        Xtr, ytr = [], []
        for i in tr:
            for v in variants:
                e = clip_embs[i][v]
                if pooling == "mean":
                    Xtr.append(e.mean(axis=0))
                    ytr.append(y[i])
                else:
                    Xtr.extend(e)
                    ytr.extend([y[i]] * len(e))
        Xtr, ytr = np.asarray(Xtr), np.asarray(ytr)
        if head_name == "svm" and len(Xtr) > SVM_MAX_TRAIN:
            keep = np.random.default_rng(SEED).permutation(len(Xtr))[:SVM_MAX_TRAIN]
            Xtr, ytr = Xtr[keep], ytr[keep]
        pipe = make_pipeline(StandardScaler(), make_head(head_name))
        pipe.fit(Xtr, ytr)
        for i in te:
            e = clip_embs[i]["orig"]  # validation stays pristine
            X = e.mean(axis=0)[None] if pooling == "mean" else e
            probs = pipe.predict_proba(X).mean(axis=0)  # frame-voting
            preds[i] = pipe.classes_[int(np.argmax(probs))]
    return list(preds)


# ----------------------------------------------------------------------- main

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--corpus", default=str(pathlib.Path.home() / "datasets" / "donateacry-corpus"))
    ap.add_argument("--cache-dir", default=str(pathlib.Path.home() / "datasets" / "crybaby-embeddings"))
    ap.add_argument("--encoders", default="all", help="comma list or 'all' (= all available)")
    ap.add_argument("--heads", default="logreg,svm,mlp", help=f"comma list from {HEADS} or 'all'")
    ap.add_argument("--pooling", default="both", choices=["mean", "frames", "both"])
    ap.add_argument("--augment", default="both", choices=["off", "on", "both"])
    ap.add_argument("--folds", type=int, default=5)
    ap.add_argument("--out", default="benchmark_results.csv")
    ap.add_argument("--list-encoders", action="store_true")
    ap.add_argument("--fit", metavar="OUT",
                    help="skip the sweep: fit one config on ALL data and save a serving "
                         "bundle (requires exactly one encoder/head/pooling and "
                         "--augment on|off). yamnet bundles are app.py-compatible.")
    args = ap.parse_args()

    if args.list_encoders:
        for name, (_, deps) in ENCODERS.items():
            status = "available" if encoder_available(name) else f"missing deps: {deps}"
            print(f"  {name:10s} {status}")
        return

    if args.encoders == "all":
        encoders = [n for n in ENCODERS if encoder_available(n)]
        skipped = [n for n in ENCODERS if n not in encoders]
        if skipped:
            print(f"skipping (missing deps, see --list-encoders): {', '.join(skipped)}")
    else:
        encoders = [n.strip() for n in args.encoders.split(",")]
        for n in encoders:
            if n not in ENCODERS:
                sys.exit(f"unknown encoder {n!r}; choose from {list(ENCODERS)}")
    heads = HEADS if args.heads == "all" else [h.strip() for h in args.heads.split(",")]
    for h in heads:
        make_head(h)  # validate early
    poolings = ["mean", "frames"] if args.pooling == "both" else [args.pooling]
    aug_modes = [False, True] if args.augment == "both" else [args.augment == "on"]
    with_aug = any(aug_modes)

    corpus = ensure_corpus(pathlib.Path(args.corpus))
    clips = load_clip_list(corpus)
    y = np.array([label for _, label in clips])
    labels = sorted(set(y))
    print(f"{len(clips)} clips, classes: " +
          ", ".join(f"{l}={np.sum(y == l)}" for l in labels))

    cache_dir = pathlib.Path(args.cache_dir)

    if args.fit:
        import joblib
        if len(encoders) != 1 or len(heads) != 1 or len(poolings) != 1 or len(aug_modes) != 1:
            sys.exit("--fit needs exactly one encoder, head, pooling and --augment on|off")
        enc, head_name, pooling, use_aug = encoders[0], heads[0], poolings[0], aug_modes[0]
        clip_embs = extract(enc, clips, cache_dir, use_aug)
        variants = (["orig"] + list(AUG_VARIANTS)) if use_aug else ["orig"]
        Xtr, ytr = [], []
        for i in range(len(y)):
            for v in variants:
                e = clip_embs[i][v]
                if pooling == "mean":
                    Xtr.append(e.mean(axis=0))
                    ytr.append(y[i])
                else:
                    Xtr.extend(e)
                    ytr.extend([y[i]] * len(e))
        pipe = make_pipeline(StandardScaler(), make_head(head_name))
        pipe.fit(np.asarray(Xtr), np.asarray(ytr))
        joblib.dump({"model": pipe, "labels": labels, "pooling": pooling,
                     "encoder": enc, "head": head_name,
                     "augment": "on" if use_aug else "off"}, args.fit)
        print(f"fitted {enc}/{head_name}/{pooling}/aug={'on' if use_aug else 'off'} "
              f"on {len(ytr)} rows -> {args.fit}")
        return

    rows = []
    for enc in encoders:
        print(f"\n=== encoder: {enc} ===", flush=True)
        clip_embs = extract(enc, clips, cache_dir, with_aug)
        clip_level = max(len(c["orig"]) for c in clip_embs) == 1
        for pooling in poolings:
            if pooling == "frames" and clip_level:
                continue  # single frame per clip: voting == mean
            for use_aug in aug_modes:
                for head in heads:
                    t0 = time.time()
                    preds = evaluate(clip_embs, y, head, pooling, use_aug, args.folds)
                    rep = classification_report(y, preds, output_dict=True, zero_division=0)
                    row = {"encoder": enc, "head": head, "pooling": pooling,
                           "augment": "on" if use_aug else "off",
                           **{l: round(rep[l]["f1-score"], 3) for l in labels},
                           "macro_f1": round(rep["macro avg"]["f1-score"], 3),
                           "accuracy": round(rep["accuracy"], 3),
                           "secs": round(time.time() - t0, 1)}
                    rows.append(row)
                    print("  " + " ".join(f"{k}={v}" for k, v in row.items()), flush=True)

    rows.sort(key=lambda r: r["macro_f1"], reverse=True)
    cols = list(rows[0])
    with open(args.out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)

    print(f"\n=== bake-off results ({args.folds}-fold CV, sorted by macro F1) ===")
    widths = {c: max(len(c), *(len(str(r[c])) for r in rows)) for c in cols}
    print("  ".join(c.ljust(widths[c]) for c in cols))
    for r in rows:
        print("  ".join(str(r[c]).ljust(widths[c]) for c in cols))
    print(f"\nsaved -> {args.out}")


if __name__ == "__main__":
    main()

"""Train the cry-cause classifier: YAMNet mean-embeddings -> logistic regression.

Dataset: donateacry-corpus (cleaned), 5 classes. ~457 clips, heavily imbalanced
(hungry dominates), so we use class_weight='balanced' and report per-class CV recall.
Usage: python train.py [--corpus DIR] [--out model.joblib]
"""
import argparse
import pathlib
import subprocess
import sys

import joblib
import numpy as np
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import classification_report
from sklearn.model_selection import cross_val_predict, StratifiedKFold
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler

from audio_pipeline import decode_to_16k_mono, yamnet_analyze

CORPUS_REPO = "https://github.com/gveres/donateacry-corpus"
CLEAN_DIR = "donateacry_corpus_cleaned_and_updated_data"


def ensure_corpus(root: pathlib.Path) -> pathlib.Path:
    clean = root / CLEAN_DIR
    if not clean.is_dir():
        print(f"cloning {CORPUS_REPO} -> {root}")
        subprocess.run(["git", "clone", "--depth", "1", CORPUS_REPO, str(root)], check=True)
    return clean


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", default=str(pathlib.Path.home() / "datasets" / "donateacry-corpus"))
    ap.add_argument("--out", default="model.joblib")
    args = ap.parse_args()

    clean = ensure_corpus(pathlib.Path(args.corpus))
    X, y = [], []
    for label_dir in sorted(p for p in clean.iterdir() if p.is_dir()):
        files = sorted(label_dir.glob("*.wav"))
        print(f"{label_dir.name}: {len(files)} files")
        for f in files:
            try:
                mono = decode_to_16k_mono(f.read_bytes())
                _, emb, _ = yamnet_analyze(mono)
            except Exception as e:  # skip corrupt clips, keep going
                print(f"  skip {f.name}: {e}", file=sys.stderr)
                continue
            X.append(emb)
            y.append(label_dir.name)

    X = np.array(X)
    y = np.array(y)
    print(f"\nfeatures: {X.shape}, classes: {sorted(set(y))}")

    clf = make_pipeline(
        StandardScaler(),
        LogisticRegression(max_iter=5000, class_weight="balanced", C=1.0),
    )
    cv = StratifiedKFold(n_splits=5, shuffle=True, random_state=42)
    pred = cross_val_predict(clf, X, y, cv=cv)
    print("\n=== 5-fold CV (honest estimate) ===")
    print(classification_report(y, pred))

    clf.fit(X, y)
    joblib.dump({"model": clf, "labels": sorted(set(y))}, args.out)
    print(f"saved -> {args.out}")


if __name__ == "__main__":
    main()

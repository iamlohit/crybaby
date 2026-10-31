# Accuracy v2 — models & data that could beat the current baseline

Brainstorm for the "Honest accuracy" problem (README): hungry F1 0.84, everything else
0.00–0.21. Baseline: mean-pooled YAMNet embeddings (1024-d, one vector per clip) →
logistic regression, 457 donateacry clips, stratified 5-fold CV.

Server constraint: 16-core CPU, 128 GB RAM, **no GPU**. Everything below is chosen to be
CPU-viable: frozen-embedding extraction and classical heads are trivial; fine-tuning a
~90 M-param transformer on 457 clips is hours, not days; from-scratch or large-scale SSL
training is out (or a one-off cloud-GPU rental).

## Why the baseline underperforms

1. **8–27 training clips** for the minority classes — no model fixes that alone.
2. **Mean-pooling throws away time**: one 1024-d vector per 7 s clip = 457 total training
   vectors. YAMNet actually emits an embedding every ~0.48 s.
3. **Labels are parents' guesses** — there is an accuracy ceiling no model exceeds.
   (Papers claiming 90 %+ on donateacry augment before splitting; we won't.)

## Track A — squeeze the current data (no new model, days of work)

| Idea | Mechanism | Expected effect |
|---|---|---|
| **Frame-level training + clip voting** | train the head on per-frame YAMNet embeddings (~15/clip → ~6 800 samples), average predicted probabilities per clip | biggest cheap win; already roadmap item |
| **Augmentation *inside* CV folds** | pitch ±2 st, time-stretch, background noise, SpecAugment — applied to training folds only, never validation | honest gains where papers got fake ones |
| **Classical prosody features alongside** | openSMILE eGeMAPS / ComParE (pitch, jitter, shimmer, HNR — 6 373-d) concatenated with YAMNet; this was Ubenwa's clinical baseline | cries are paralinguistic; AudioSet embeddings underweight prosody |
| **Stronger heads + calibration** | RBF-SVM, gradient boosting, small MLP; `CalibratedClassifierCV` so on-screen % is meaningful | marginal but free in a bake-off |

## Track B — swap the encoder (frozen embeddings, drop-in for YAMNet)

All of these replace `yamnet_analyze()`'s embedding and reuse the same head + CV harness,
so the right move is a **bake-off script** that caches embeddings per encoder and prints
one per-class-F1 table:

| Encoder | Why it might win | CPU cost (7 s clip) |
|---|---|---|
| [BEATs iter3+](https://arxiv.org/abs/2212.09058) (90 M) | best-in-class AudioSet model, much stronger than YAMNet on general audio | ~1–2 s |
| [AST](https://huggingface.co/MIT/ast-finetuned-audioset-10-10-0.4593) (87 M) | same family, first-class HF support → easiest to *fine-tune* later | ~1–2 s |
| PANNs CNN14 (80 M) | strong CNN baseline, lighter than transformers | <1 s |
| **WavLM / HuBERT / wav2vec2-base** (95 M) | speech-SSL models encode prosody & vocal-tract detail that AudioSet-supervised models miss — cries are vocalizations, not "events"; benchmarks show the two families win on different task types | ~1–2 s |
| Whisper encoder (base/small) | robust to noisy home recordings; cheap to try | ~1 s |
| [LAION-CLAP](https://arxiv.org/abs/2503.22104) / M2D-CLAP | embeddings + free zero-shot text probes ("a baby crying in pain") as extra features | ~1 s |
| [Ubenwa ECAPA cry model](https://huggingface.co/Ubenwa/ecapa-voxceleb-ft2-cryceleb) | the only public **cry-domain** encoder (trained for infant identity); identity features could also *normalize out* baby-specific traits | <1 s |

Then, only after the bake-off picks a winner: **fine-tune it** (AST or wav2vec2-style,
discriminative learning rates, frozen lower layers). On 457 clips this is a few CPU-hours;
overfitting is the risk, so it must beat the frozen version under the same CV.

## Track C — more / better data (the real bottleneck)

| Source | Size | Fit | Catch |
|---|---|---|---|
| donateacry **raw** submissions (same repo, pre-cleaning) | ~3× the cleaned set | same 5 labels | needs manual QC — clips were cut for a reason |
| [Baby Chillanto](https://link.springer.com/article/10.1186/s13636-021-00197-5) | 2 268 × 1 s segments | *hunger* and *pain* overlap our classes | request-only license (INAOE, Mexico); pathology focus |
| [CryCeleb / Ubenwa](https://arxiv.org/abs/2305.00969) | 26 k clips, 6.5 h, 786 newborns | **no cause labels** → use for domain-adaptive SSL pretraining, then fine-tune on donateacry. Ubenwa showed [SSL-on-cries beats supervised pretraining for exactly our "cry trigger" task](https://arxiv.org/abs/2305.01578) | pretraining wants a GPU (one-off cloud rental ~hours) — the *resulting* encoder runs fine on CPU |
| [ICSD](https://arxiv.org/abs/2408.10561) | detection corpus | improves the **cry-gate**, not cause ranking | not a cause dataset |
| LLM pseudo-labeling | unlimited | Gemini audio labels unlabeled cries (CryCeleb, Freesound) with the Dunstan taxonomy → distill into the classifier | labels unvalidated; treat as weak supervision experiment |
| **The device itself** | grows forever | add a "was this right?" follow-up button → real parent-confirmed labels for *your* baby, enables per-baby few-shot adaptation | needs opt-in recording retention + UI work; the only path to labels better than strangers' guesses — **designed, not built: see `docs/FEEDBACK-FLOW.md`** |

## Recommended order

1. **Bake-off harness** (`server/benchmark.py`) — BUILT: frame-voting + fold-safe
   augmentation + Track-B encoders × {logreg, SVM, MLP, HGB} → one table. On this
   dataset size it runs overnight on CPU. Still to wire in: BEATs (no clean pip
   checkpoint path) and the eGeMAPS/ComParE prosody features from step 2.
2. **Prosody hypothesis**: add eGeMAPS/ComParE + WavLM to the bake-off — most likely
   single jump, because it attacks the "wrong feature family" problem, not sample count.
3. **Data**: request Baby Chillanto; QC-salvage donateacry raw; prototype the
   feedback-button flywheel (product feature *and* dataset).
4. **Later / GPU-rental**: SSL pretraining on CryCeleb, then fine-tune — the Ubenwa
   result says this is the ceiling-raiser, but it's the most work.

Honesty rule unchanged: whatever wins, report fold-safe per-class F1 in the README table
and keep "ranked suggestion, never a diagnosis."

## Outcome — bake-off complete, winner deployed (2026-08-23)

Best config per encoder, 5-fold CV macro-F1 (full per-class rows in
`bench-stage2.log` / `results-stage*.csv` on the home server):

| Encoder | Best config | Macro-F1 |
|---|---|---|
| **ast** | logreg / mean / aug=on | **0.384** ← deployed |
| hubert | logreg / frames / aug=on | 0.341 |
| wav2vec2 | logreg / mean / aug=off | 0.320 |
| clap | mlp / mean / aug=on | 0.319 |
| wavlm | logreg / frames / aug=on | 0.307 |
| yamnet (stage 1) | logreg / frames / aug=off | 0.292 |
| whisper | logreg / frames / aug=on | 0.270 |
| melstats (stage 1) | logreg / mean / aug=off | 0.258 |

Notes: fold-safe augmentation is worth ~+0.05–0.10 macro-F1 on most encoders;
RBF-SVM collapses to majority-class on this imbalance; burping (8 clips) is ≈0
for every encoder except the winner (0.167) — sample count, not model choice, is
the binding constraint, which is why the feedback flow (Track C) is next.
Never run: ecapa and panns (deps not installed on the home server), BEATs (no pip
checkpoint), eGeMAPS/ComParE prosody features (step 2) — still open.

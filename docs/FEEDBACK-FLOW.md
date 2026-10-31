# Feedback flow — "was this right?" (design only — not built yet)

Fleshes out Track C's "the device itself" row in `ACCURACY-V2.md`: every other data
source teaches the model from strangers' guesses. This is the only source of labels
from someone who actually knows the baby. Owner requirements that shaped this: the
result screen must **not** time out into idle until the parent confirms, corrects, or
explicitly ignores — no silent disappearance — and the recording is kept on the server.

## UX — one mental model, three physical buttons

Result screen stops auto-returning to idle after ~30 s (current `main.c` behavior) and
instead enters a persistent **feedback state**:

- **Vol− (tap)** — cycle the highlighted cause through the ranked list (wraps; starts
  on the model's top guess).
- **Vol+ (tap)** — confirm: "yes, the highlighted cause is the real one." Sends
  `confirmed` if it's still the original top guess, `corrected` if the parent cycled
  to a different one. One button, one meaning, regardless of which cause is lit.
- **Vol− (hold ~1.5 s)** — explicit **Ignore**: no verdict, dismiss to idle.
- **Main (tap)** — starts a new recording immediately, same as always. Interrupting
  feedback this way is a deliberate user action (not a timeout), so it's fine to treat
  as an implicit `abandoned` — fire-and-forget one last request so the pending clip
  doesn't sit orphaned server-side.

So the screen only ever leaves feedback state on a real signal: confirmed, corrected,
ignored, or abandoned-by-starting-over. Never on a clock.

⚠️ **Button overlap with `feat/voicebot`**: that branch also claims Vol+, as
hold-to-talk. They shouldn't collide — voicebot's hold-to-talk only makes sense from
idle, and this design's Vol+ tap only fires while the feedback screen is showing — but
whoever merges both branches needs to verify the tap-vs-hold detection is consistent
across both call sites and doesn't race (e.g. a tap that's about to become a hold).

## Server changes

- `/analyze` gains a `request_id` (uuid4) in its JSON response. Before responding, it
  saves the submitted WAV + a small sidecar (`{prediction, model_config, timestamp}`)
  to `feedback/pending/<uuid>.wav` / `.json` on the server.
- New `POST /feedback`: `{request_id, outcome: "confirmed"|"corrected"|"ignored"|"abandoned", label?}`.
  Moves the pending pair into:
  - `feedback/confirmed/<label>/<uuid>.wav`
  - `feedback/corrected/<label>/<uuid>.wav` (label = what the parent actually picked)
  - `feedback/ignored/<uuid>.wav` and `feedback/abandoned/<uuid>.wav`
  Sidecar gets the outcome + a second timestamp appended, never overwritten.
- Housekeeping: a pending clip with no feedback after ~48 h (device died, parent walked
  away) gets auto-swept into `ignored/` so `pending/` doesn't grow unbounded.

**The payoff**: `confirmed/<label>/*.wav` + `corrected/<label>/*.wav` together are a
second corpus in the exact same shape as donateacry's label-subfolder layout — `train.py`
/ `benchmark.py` can point straight at it (or merge it in) once there's enough volume.
This is Track C item 5 made concrete, and the numbers only get better with real use.

## Privacy note — this is a real behavior change, flag it

Today nothing about `/analyze` persists audio to disk; the WAV is used in memory and
optionally relayed once to Gemini. This feature makes the server **start keeping actual
recordings of a real baby crying**, indefinitely, in labeled folders. Fine as a
conscious choice on a private LAN box for personal use — but if this ships in the public
repo, retention should default to **off** (or be unmissable in the README) for anyone
else's deployment. Don't let this default on silently in the OSS release.

## Build order for whoever picks this up

1. Server: `request_id` + `feedback/pending/` persistence in `/analyze`; `/feedback`
   endpoint; the 48 h sweep.
2. Firmware: feedback UI state (cycle/confirm/ignore), replacing the current 30 s
   auto-idle after a result; `net.c` gets a `net_send_feedback()` call.
3. Point `train.py`/`benchmark.py` at the growing `feedback/confirmed+corrected/` tree
   once there's real volume; re-run the bake-off with it folded in.

## Open questions to settle before building

- Exact on-screen wording for the cycle/confirm prompt (HIG pass, like the rest of the UI).
- Does cycling skip the already-top-ranked guess, or start there and require one tap to
  confirm the obvious case? (Design above assumes the latter — lowest-friction for the
  common "yes it's right" case.)
- Sweep window (48 h chosen arbitrarily) and whether `abandoned` needs to be distinct
  from `ignored` in the data, or if they're the same "no signal" bucket for training
  purposes even if kept in separate folders operationally.

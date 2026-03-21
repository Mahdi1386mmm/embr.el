# PLAN-3: CloakBrowser Screencast Data Plane

Document status: implementation specification
Last updated: 2026-03-20
Owner: `embr` maintainers
Audience: core implementers and performance agents

## 1. Executive Decision

Single goal:

- move frame pixels off per-frame screenshot polling to Chromium CDP screencast push.

`PLAN-3` targets the capture-side bottleneck while preserving current Emacs UX.

## 2. Scope (Pruned)

PLAN-3 focuses on one item only:

- stop relying on per-frame screenshot polling for pixel transport,
- keep existing control/input semantics,
- use CloakBrowser/Chromium CDP screencast push with fallback to screenshot mode.

Scope gate (hard):

- Any work not required for item 2 is out of scope for PLAN-3.
- Keep existing runtime tuning behavior as-is (adaptive capture, hover policy, etc.).
- Do not add new render backends in this plan.
- Booster work is out of scope (current direction is direct transport).

### 2.1 Item 2: "Stop using screenshot RPC for frame pixels" (primary focus)

For CloakBrowser/Chromium, this is available now via standard CDP:

- `Page.startScreencast`
- `Page.screencastFrame`
- `Page.screencastFrameAck`
- `Page.stopScreencast`

Key decision:

- No Chromium source compilation.
- No CloakBrowser fork.
- No mandatory Playwright driver patch for baseline screencast support.

## 3. Capability Status: As-Is vs Patch

Current status (validated locally in this repo environment):

- `BrowserContext.new_cdp_session(page)` works on CloakBrowser/Chromium.
- `Page.startScreencast` succeeds.
- `Page.screencastFrame` events stream and can be acked with `Page.screencastFrameAck`.

Probe summary:

- static page: screencast start/stop works (frame event observed),
- animated page: 116 frames in 2.0s, 0 ack errors.

Conclusion:

- Cast is available as-is for PLAN-3 baseline.
- JIT patching is not required to unlock screencast transport.

Patch policy:

- only consider limited JIT patching later if required for a proven gap
  (for example missing metadata, event backpressure hooks, or measurable p95 regressions),
- any such patch must be optional and fail closed to screenshot mode.

## 4. Non-Negotiable Constraints

- CloakBrowser/Chromium remains the engine.
- No compiling Chromium from source.
- No CloakBrowser source fork.
- Existing keyboard/mouse/navigation behavior must remain unchanged.
- Existing screenshot path remains available as hard fallback.
- No user-visible breakage on stock Emacs.
- No requirement to land PLAN-5 first.

## 5. Architecture Decision

Control/data split:

- control plane (unchanged): JSON command path for input/navigation.
- data plane (new): pushed screencast frames via CDP session in daemon.

Fallback:

- if screencast init fails or runtime health degrades, daemon reverts to `page.screenshot()` path.

## 6. Implementation Requirements

## 6.1 Daemon (`embr.py`) Changes (required)

Add frame source selection:

- `frame_source=auto|screenshot|screencast`.

Mode rules:

- `auto`: probe screencast first, fallback to screenshot.
- `screenshot`: current behavior exactly.
- `screencast`: require screencast; hard error if unavailable.

Screencast behavior:

- open CDP session with `context.new_cdp_session(page)`,
- enable page domain and start screencast with JPEG settings,
- ack each screencast frame via `Page.screencastFrameAck`,
- maintain queue depth 1 semantics (latest frame wins),
- keep emitted frame metadata contract compatible with Emacs.

Failure behavior:

- in `auto`, repeated screencast errors trigger fallback to screenshot without restart,
- in forced `screencast`, return explicit error and stop stream cleanly.

Lifecycle requirements:

- stop screencast cleanly on tab switch, page replacement, and quit,
- recreate CDP session safely when active page changes.

## 6.2 Emacs (`embr.el`) Changes (required)

Add user control:

- `embr-frame-source` values: `auto`, `screenshot`, `screencast`.

Init payload:

- include selected `frame_source` in daemon init.

No render rewrite in PLAN-3:

- keep current JPEG decode/display path in Emacs,
- PLAN-5 remains responsible for canvas-side acceleration.

## 6.3 Setup and Tooling Changes (required)

`setup.sh` requirements:

- no driver patch step in baseline PLAN-3 path,
- keep deterministic install flow.

Diagnostics:

- log active frame source at daemon startup (`screenshot` vs `screencast`),
- expose fallback reason when `auto` drops back to screenshot.

## 7. Milestones

### M0: Feasibility Gate (must pass before integration)

Deliver:

- proof that CloakBrowser screencast starts/stops via CDP session,
- proof that frame ack path is correct,
- proof that screenshot fallback still works.

Exit gate:

- >= 300 frames received in 30s on animated test page, no ack error loop, no daemon deadlock.

### M1: Daemon Integration

Deliver:

- `frame_source` negotiation and probe logic,
- screencast ingest + latest-frame queue,
- automatic fallback behavior.

### M2: Emacs Integration

Deliver:

- new `embr-frame-source` defcustom and init wiring,
- status messaging of active frame source.

### M3: Validation and Rollout

Deliver:

- benchmark report versus screenshot baseline,
- stability report and rollback instructions,
- recommendation for default (`auto` vs `screenshot`).

## 8. Acceptance Criteria

## 8.1 Functional

- `auto` selects screencast when healthy.
- fallback to screenshot works without restart.
- no regressions in navigate/back/forward/click/type/scroll/tab workflows.

## 8.2 Performance (vs screenshot baseline on same machine)

- `input_to_next_frame_ms p95` improves by >= 20% in video + interaction scenario.
- `frame_staleness_ms p95` does not regress.
- freeze events (>750ms no frame while active) are not worse than baseline.

## 8.3 Stability

- zero new daemon crash class in acceptance scenarios,
- no persistent frame stall after transient screencast failure,
- no unbounded queue growth under prolonged animation.

## 9. Test Matrix

Each run: 10 minutes.

- `S1`: baseline browsing.
- `S2`: 1080p60 playback + mixed input.
- `S3`: heavy hover/click stress.
- `S4`: long-session endurance.

Run with:

- screenshot mode,
- auto mode,
- forced screencast mode (when available).

## 10. Rollback Requirements

- user can set `embr-frame-source` to `screenshot` and recover immediately.
- screencast startup failure must not block browsing startup in `auto`.
- logs must clearly state fallback reason.

## 11. Reviewer Rejection Criteria

Reject if any are true:

- plan requires Chromium compilation or CloakBrowser forking,
- no deterministic fallback to screenshot,
- forced screencast mode can leave daemon in hung state,
- no measured p95 responsiveness improvement,
- implementation expands scope beyond transport item.

## 12. Relationship to Other Plans

PLAN-3 and PLAN-5 are complementary:

- PLAN-3: capture/data-plane improvement (browser -> daemon transport).
- PLAN-5: Emacs-side rendering path acceleration.

Expected order:

1. land PLAN-3 screencast transport,
2. then layer PLAN-5 rendering acceleration as needed.

## 13. Deliverables

- `embr.py` screencast mode with safe fallback,
- `embr.el` `embr-frame-source` wiring,
- benchmark report (screenshot vs auto vs screencast),
- README updates for frame-source behavior and rollback.

## 14. Definition of Done

PLAN-3 is complete when:

- CloakBrowser-based screencast path works in `auto`,
- fallback to screenshot is reliable and documented,
- acceptance criteria are evaluated and reported,
- setup/docs/runtime logs are synchronized with behavior.


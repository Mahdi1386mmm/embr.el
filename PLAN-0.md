# PLAN-0: Camoufox to CloakBrowser Migration

Document status: implementation specification
Last updated: 2026-03-20
Owner: `embr` maintainers
Audience: core implementers and migration reviewers

## 1. Executive Decision

`PLAN-0` replaces Camoufox/Firefox with CloakBrowser/Chromium in `embr`.

The migration is explicitly phased:

- Part 1: ship CloakBrowser with booster disabled (`embr-booster` = nil).
- Feedback Gate 1: pause and gather user feedback before booster work.
- Part 2: enable and validate `embr-booster` with CloakBrowser.
- Feedback Gate 2: pause again for user feedback before default flips.

## 2. Non-Negotiable Constraints

- CloakBrowser is the browser engine target.
- No fake stealth via JS injection-only tricks.
- No patched config-only substitution pretending to be migration complete.
- Existing Emacs UX and keybindings remain intact.
- JSON-line protocol stays compatible unless a protocol version bump is introduced intentionally.
- Direct mode (no booster) must work first before booster integration is attempted.

## 3. Scope

In scope:

- `embr.py`: browser launch/runtime migration from Camoufox-specific API to CloakBrowser-compatible Chromium flow.
- `setup.sh`: installer changes to provision CloakBrowser runtime and dependencies.
- `uninstall.sh`: uninstall changes to remove CloakBrowser artifacts and prompts.
- `embr.el` lifecycle functions: management command updates for setup/uninstall/info and booster management flow.
- `README.md`: docs updates for engine, setup, and defaults.
- Compatibility preservation for modern performance flags listed in Section 6.

Out of scope for this plan:

- New rendering pipeline work (canvas, shared memory, screencast redesign).
- Feature additions unrelated to engine swap.
- Default-on booster policy change before Part 2 acceptance.

## 4. Migration Architecture

Introduce a browser backend seam in daemon startup:

- backend `cloakbrowser` (new primary).
- backend `camoufox` (temporary fallback only during transition window).

The seam must be short-lived and removed once CloakBrowser is stable, but it enables controlled rollout and rollback while migration is in progress.

## 5. Two-Part Rollout with Feedback Gates

## 5.1 Part 1: CloakBrowser in Direct Mode (Booster Disabled)

Delivery requirements:

- Replace Camoufox launch path with CloakBrowser launch path in `embr.py`.
- Keep transport in direct mode (`Emacs <-> embr.py`) with:
  - `embr-booster` = nil
  - `embr-booster-args` = nil
- Maintain all existing browser commands (`navigate`, `click`, `mousemove`, `scroll`, `type`, tabs, hints, etc.).
- Keep frame loop stable (same frame notification contract).
- Preserve ad-block route interception behavior unless CloakBrowser provides a better native equivalent that is verified safe.

Part 1 exit gate:

- `make test` passes.
- Manual smoke workflows pass (browse, click, type, scroll, back/forward, tab open/switch/close).
- User feedback is explicitly requested and recorded before proceeding to Part 2.

Feedback Gate 1 prompt (required):

- "CloakBrowser direct mode is working with booster disabled. Do you want us to proceed with booster integration now?"

## 5.2 Part 2: CloakBrowser with Booster Enabled

Part 2 starts only after Feedback Gate 1 approval.

Delivery requirements:

- Validate that `embr-booster` still behaves as a protocol-transparent proxy with CloakBrowser traffic profile.
- Keep `EMBR_FRAME_FD` and frame suppression semantics working.
- Confirm no command starvation regressions under hover/video pressure.
- Preserve clean fallback to direct mode if booster binary is missing or fails.

Part 2 exit gate:

- `make test` passes.
- Booster-on manual stress pass (video + input churn) shows no functional regressions.
- User feedback is explicitly requested and recorded after booster validation.

Feedback Gate 2 prompt (required):

- "CloakBrowser booster mode is validated. Do you want any tuning/default changes, or should we keep current defaults?"

## 6. Modern Flag Compatibility Contract

These settings must remain supported and wired end-to-end during migration:

```elisp
(setq embr-fps 60
      embr-jpeg-quality 80
      embr-hover-rate 20
      embr-default-width 1280
      embr-default-height 720
      embr-screen-width 1920
      embr-screen-height 1080
      embr-color-scheme 'dark
      embr-search-engine 'google
      embr-click-method 'immediate
      embr-scroll-method 'instant
      embr-scroll-step 120
      embr-dom-caret-hack nil
      embr-perf-log nil
      embr-input-priority-window-ms 35
      embr-adaptive-capture t
      embr-adaptive-fps-min 40
      embr-adaptive-jpeg-quality-min 65
      embr-hover-move-threshold-px 0
      embr-hover-rate-min 14
      embr-external-command "yt-dlp -o - %s | mpv -"
      embr-booster nil
      embr-booster-args nil)
```

Caret policy for migration:

- Keep `embr-dom-caret-hack` disabled (`nil`) by default during CloakBrowser migration.
- Re-evaluate only after Part 1 and Part 2 feedback, because native caret capture behavior may differ on Chromium path.

## 7. Implementation Work Breakdown

W1. Engine launch migration (`embr.py`)

- Remove direct dependency on `camoufox.async_api.AsyncNewBrowser`.
- Add CloakBrowser-compatible browser/context/page startup flow.
- Ensure viewport and screen sizing still honor Emacs init params.

W2. Setup/install/uninstall script migration (`setup.sh`, `uninstall.sh`)

- Replace Camoufox install/fetch steps with CloakBrowser install/bootstrap steps.
- Keep atomic venv swap and rollback behavior.
- Keep blocklist download and optional booster build behavior.
- Update uninstall behavior and prompts for CloakBrowser cache/data locations.
- Preserve safe confirmation flow and clear output messages for removals.

W3. Emacs management function sync (`embr.el`)

- Update `embr-setup-or-update` messaging to reflect CloakBrowser install/update path.
- Update `embr-uninstall` prompts and optional browser-cache deletion wording.
- Update `embr-info` reported browser cache/profile paths and labels.
- Keep `embr-build-booster` flow unchanged except for any engine-specific text references.

W4. Documentation sync (`README.md`)

- Replace Camoufox references with CloakBrowser references.
- Update install/runtime wording.
- Ensure `use-package` blocks and config tables remain synchronized with actual `defcustom` defaults.
- Reflect caret-hack disabled migration stance.
- Ensure setup/uninstall command descriptions match actual script/function behavior.

W5. Compatibility and fallback controls

- Add explicit startup logging of active engine and booster mode.
- Keep clear fallback/error messages for missing runtime dependencies.
- Keep rollback path to prior known-good engine branch/tag documented.

## 8. Validation Matrix

Run each scenario in both modes where applicable:

- A: direct mode (`embr-booster nil`) - mandatory for Part 1 and Part 2.
- B: booster mode (`embr-booster t`) - mandatory for Part 2.

Scenarios:

- S1: normal browsing and search.
- S2: media playback plus interaction.
- S3: rapid hover/click/scroll stress.
- S4: long session stability.

Core checks:

- command responsiveness,
- frame cadence stability,
- no hangs/deadlocks,
- no protocol parse failures,
- expected keybinding behavior.
- `M-x embr-setup-or-update` invokes updated `setup.sh` successfully.
- `M-x embr-uninstall` invokes updated `uninstall.sh` with correct prompts/targets.
- `M-x embr-info` reports correct engine-related cache/profile locations.

## 9. Risks and Mitigations

Risk: Chromium/CDP behavior differs from prior Firefox/Camoufox assumptions.

Mitigation:

- keep command handlers behaviorally identical,
- test every input path in Part 1 before booster changes.

Risk: booster queue policy tuned for previous traffic shape.

Mitigation:

- booster work is deferred to Part 2,
- explicit stress validation and direct-mode fallback retained.

Risk: caret behavior differences.

Mitigation:

- keep caret hack disabled for migration baseline,
- re-evaluate after user feedback at each gate.

## 10. Acceptance Criteria

`PLAN-0` is accepted only when all are true:

- Part 1 completed and approved at Feedback Gate 1.
- Part 2 completed and reviewed at Feedback Gate 2.
- Modern flag compatibility contract preserved.
- `make test` passes at each implementation checkpoint.
- Setup and uninstall scripts are migrated and validated end-to-end.
- Emacs management functions are updated to match script behavior and paths.
- README/setup instructions reflect the migrated engine accurately.

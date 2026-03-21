#!/usr/bin/env python3
"""M3 acceptance criteria gate for PLAN-3.

Validates PLAN-3 section 8.2 performance acceptance criteria by
comparing perf logs from candidate mode(s) against screenshot baseline.

Exit code 0 on pass, 1 on fail, 2 on insufficient data.

Usage:
    python3 tools/acceptance-gate.py \\
        --baseline screenshot_r1.jsonl screenshot_r2.jsonl screenshot_r3.jsonl \\
        --candidate auto_r1.jsonl auto_r2.jsonl auto_r3.jsonl

Criteria (PLAN-3 section 8.2):
    - input_to_next_frame_ms p95 improves by >= 20% vs baseline
    - frame_staleness_ms p95 does not regress (candidate <= baseline)
    - freeze events (>750ms) are not worse than baseline

Measurement quality (PLAN-3 section 8.4):
    - minimum 3 runs per group
    - warm-up excluded (first 60s)
    - deltas computed from medians across runs
"""

import json
import os
import sys
import tempfile

PERF_LOG_PATH = os.path.join(tempfile.gettempdir(), "embr-perf.jsonl")
WARMUP_MS = 60000
FREEZE_THRESHOLD_MS = 750
MIN_RUNS = 3


def extract_scenario(path):
    """Extract scenario tag from filename (e.g., S1_screenshot_r1.jsonl -> S1)."""
    base = os.path.splitext(os.path.basename(path))[0]
    parts = base.split("_")
    if (parts and len(parts[0]) >= 2
            and parts[0][0] == "S" and parts[0][1:].isdigit()):
        return parts[0]
    return None


def check_scenario_consistency(baseline_paths, candidate_paths):
    """Verify all logs in both groups are from the same scenario."""
    b_scenarios = {extract_scenario(p) for p in baseline_paths}
    c_scenarios = {extract_scenario(p) for p in candidate_paths}
    # Remove None (files without scenario tags are allowed).
    b_scenarios.discard(None)
    c_scenarios.discard(None)
    if not b_scenarios and not c_scenarios:
        print("  WARNING: no scenario tags in filenames, "
              "cannot verify scenario consistency.",
              file=sys.stderr)
        return None
    all_scenarios = b_scenarios | c_scenarios
    if len(all_scenarios) > 1:
        return (f"scenario mismatch: baseline has {b_scenarios or '{none}'}, "
                f"candidate has {c_scenarios or '{none}'}")
    return None

# Acceptance thresholds (PLAN-3 section 8.2).
INPUT_IMPROVEMENT_PCT = 20.0  # >= 20% improvement required


def percentiles(values, pcts=(50, 95, 99)):
    """Return dict of percentile values."""
    if not values:
        return {p: None for p in pcts}
    s = sorted(values)
    n = len(s)
    out = {}
    for p in pcts:
        k = (p / 100) * (n - 1)
        lo = int(k)
        hi = min(lo + 1, n - 1)
        out[p] = s[lo] + (k - lo) * (s[hi] - s[lo])
    return out


def median(values):
    """Return median of a list."""
    if not values:
        return None
    s = sorted(values)
    n = len(s)
    if n % 2 == 1:
        return s[n // 2]
    return (s[n // 2 - 1] + s[n // 2]) / 2


def load_events(path, warmup_ms=WARMUP_MS):
    """Load JSONL events, excluding warm-up."""
    events = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    events.append(json.loads(line))
                except json.JSONDecodeError:
                    pass
    if not events or warmup_ms <= 0:
        return events
    ts_list = [e["ts_ms"] for e in events if "ts_ms" in e]
    if not ts_list:
        return events
    cutoff = min(ts_list) + warmup_ms
    return [e for e in events if e.get("ts_ms", 0) >= cutoff]


def extract_metrics(events):
    """Extract key metrics from events for one run."""
    frame_emits = [e for e in events if e.get("event") == "frame_emit"]
    frame_renders = [e for e in events if e.get("event") == "frame_render"]

    input_to_frame = [e["input_to_frame_ms"] for e in frame_emits
                      if "input_to_frame_ms" in e]
    staleness = [e["frame_staleness_ms"] for e in frame_renders
                 if "frame_staleness_ms" in e]
    intervals = [e["interval_ms"] for e in frame_emits
                 if "interval_ms" in e]
    freezes = sum(1 for i in intervals if i >= FREEZE_THRESHOLD_MS)

    return {
        "input_to_frame_p95": percentiles(input_to_frame).get(95),
        "staleness_p95": percentiles(staleness).get(95),
        "freezes": freezes,
        "total_frames": len(frame_emits),
    }


def main():
    baseline_paths = []
    candidate_paths = []
    current_group = None

    for arg in sys.argv[1:]:
        if arg == "--baseline":
            current_group = "baseline"
        elif arg == "--candidate":
            current_group = "candidate"
        elif current_group == "baseline":
            baseline_paths.append(arg)
        elif current_group == "candidate":
            candidate_paths.append(arg)
        else:
            print(f"Unknown argument: {arg}")
            print("Usage: acceptance-gate.py "
                  "--baseline <logs...> --candidate <logs...>")
            sys.exit(2)

    if not baseline_paths or not candidate_paths:
        print("Usage: acceptance-gate.py "
              "--baseline <logs...> --candidate <logs...>")
        sys.exit(2)

    # Check all files exist.
    for p in baseline_paths + candidate_paths:
        if not os.path.exists(p):
            print(f"File not found: {p}")
            sys.exit(2)

    # Check scenario consistency.
    scenario_err = check_scenario_consistency(baseline_paths, candidate_paths)
    if scenario_err:
        print(f"ERROR: {scenario_err}")
        print("All logs in --baseline and --candidate must be from "
              "the same scenario (e.g., S2_screenshot_r1.jsonl).")
        sys.exit(2)

    print("=" * 60)
    print("  PLAN-3 M3 Acceptance Criteria Gate")
    print("=" * 60)

    # Check minimum run count.
    print(f"\n  Baseline runs:  {len(baseline_paths)} "
          f"(minimum: {MIN_RUNS})")
    print(f"  Candidate runs: {len(candidate_paths)} "
          f"(minimum: {MIN_RUNS})")

    if len(baseline_paths) < MIN_RUNS or len(candidate_paths) < MIN_RUNS:
        print(f"\n  FAIL: insufficient runs "
              f"(need >= {MIN_RUNS} per group)")
        sys.exit(2)

    # Extract per-run metrics.
    baseline_metrics = []
    for p in baseline_paths:
        events = load_events(p)
        if not events:
            print(f"  WARNING: empty log (after warmup): {p}")
            continue
        baseline_metrics.append(extract_metrics(events))

    candidate_metrics = []
    for p in candidate_paths:
        events = load_events(p)
        if not events:
            print(f"  WARNING: empty log (after warmup): {p}")
            continue
        candidate_metrics.append(extract_metrics(events))

    if (len(baseline_metrics) < MIN_RUNS
            or len(candidate_metrics) < MIN_RUNS):
        print(f"\n  FAIL: insufficient valid runs after loading")
        sys.exit(2)

    # Compute medians across runs.
    b_input = median([m["input_to_frame_p95"] for m in baseline_metrics
                      if m["input_to_frame_p95"] is not None])
    c_input = median([m["input_to_frame_p95"] for m in candidate_metrics
                      if m["input_to_frame_p95"] is not None])
    b_stale = median([m["staleness_p95"] for m in baseline_metrics
                      if m["staleness_p95"] is not None])
    c_stale = median([m["staleness_p95"] for m in candidate_metrics
                      if m["staleness_p95"] is not None])
    b_freeze = median([m["freezes"] for m in baseline_metrics])
    c_freeze = median([m["freezes"] for m in candidate_metrics])

    print(f"\n  Medians across runs:")
    print(f"    {'metric':35s}  {'baseline':>10s}  {'candidate':>10s}")
    print(f"    {'-' * 35}  {'-' * 10}  {'-' * 10}")

    def fmt(v):
        return f"{v:10.1f}" if v is not None else "       n/a"

    print(f"    {'input_to_frame_ms p95':35s}  {fmt(b_input)}  {fmt(c_input)}")
    print(f"    {'staleness_ms p95':35s}  {fmt(b_stale)}  {fmt(c_stale)}")
    print(f"    {'freezes (>750ms)':35s}  {fmt(b_freeze)}  {fmt(c_freeze)}")

    # ── Check acceptance criteria ────────────────────────────────
    results = []
    print(f"\n  Acceptance criteria:")

    # Criterion 1: input_to_frame p95 improves >= 20%.
    if b_input is not None and c_input is not None and b_input > 0:
        improvement = (b_input - c_input) / b_input * 100
        passed = improvement >= INPUT_IMPROVEMENT_PCT
        results.append(passed)
        status = "PASS" if passed else "FAIL"
        print(f"    [{status}] input_to_frame p95 improvement: "
              f"{improvement:+.1f}% (need >= {INPUT_IMPROVEMENT_PCT}%)")
    else:
        results.append(False)
        print(f"    [FAIL] input_to_frame p95: insufficient data")

    # Criterion 2: staleness p95 does not regress.
    if b_stale is not None and c_stale is not None:
        passed = c_stale <= b_stale
        results.append(passed)
        status = "PASS" if passed else "FAIL"
        print(f"    [{status}] staleness p95: candidate={c_stale:.1f}ms, "
              f"baseline={b_stale:.1f}ms (must not regress)")
    else:
        results.append(False)
        print(f"    [FAIL] staleness p95: insufficient data")

    # Criterion 3: freeze events not worse.
    if b_freeze is not None and c_freeze is not None:
        passed = c_freeze <= b_freeze
        results.append(passed)
        status = "PASS" if passed else "FAIL"
        print(f"    [{status}] freeze events: "
              f"candidate={c_freeze:.0f}, baseline={b_freeze:.0f}")
    else:
        results.append(False)
        print(f"    [FAIL] freeze events: insufficient data")

    # ── Verdict ──────────────────────────────────────────────────
    all_passed = all(results)
    print(f"\n  {'PASS' if all_passed else 'FAIL'}: "
          f"{sum(results)}/{len(results)} criteria met")
    print("=" * 60)
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()

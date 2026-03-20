#!/usr/bin/env python3
"""Read /tmp/embr-perf.jsonl and print a performance summary.

Uses only the Python standard library.  Run after a browsing session
with `embr-perf-log` enabled:

    python3 tools/embr-perf-report.py
"""

import json
import os
import sys
import tempfile

PERF_LOG_PATH = os.path.join(tempfile.gettempdir(), "embr-perf.jsonl")

FREEZE_THRESHOLD_MS = 750
SEVERE_FREEZE_MS = 1500


def percentiles(values, pcts=(50, 95, 99)):
    """Return dict of percentile values from a sorted list."""
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


def fmt_ms(v):
    if v is None:
        return "  n/a"
    return f"{v:8.1f}"


def print_section(title, values, unit="ms"):
    """Print percentile breakdown for a list of values."""
    if not values:
        print(f"\n  {title}: no data")
        return
    p = percentiles(values)
    mx = max(values)
    print(f"\n  {title} (n={len(values)})")
    print(f"    p50  {fmt_ms(p[50])} {unit}")
    print(f"    p95  {fmt_ms(p[95])} {unit}")
    print(f"    p99  {fmt_ms(p[99])} {unit}")
    print(f"    max  {fmt_ms(mx)} {unit}")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else PERF_LOG_PATH
    if not os.path.exists(path):
        print(f"No perf log found at {path}")
        print("Enable with (setq embr-perf-log t) and restart embr.")
        sys.exit(1)

    events = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                try:
                    events.append(json.loads(line))
                except json.JSONDecodeError:
                    pass

    if not events:
        print("Perf log is empty.")
        sys.exit(1)

    # Classify events.
    cmd_acks = [e for e in events if e.get("event") == "cmd_ack"]
    frame_emits = [e for e in events if e.get("event") == "frame_emit"]
    frame_renders = [e for e in events if e.get("event") == "frame_render"]
    capture_dones = [e for e in events if e.get("event") == "capture_done"]
    cmd_receives = [e for e in events if e.get("event") == "cmd_receive"]

    # ── Command ack latency ──────────────────────────────────────
    interactive_cmds = {
        "mousemove", "click", "mousedown", "mouseup", "key", "type", "scroll",
    }
    all_latencies = [e["latency_ms"] for e in cmd_acks if "latency_ms" in e]
    interactive_latencies = [
        e["latency_ms"] for e in cmd_acks
        if "latency_ms" in e and e.get("cmd") in interactive_cmds
    ]

    # Per-command breakdown.
    by_cmd = {}
    for e in cmd_acks:
        c = e.get("cmd", "?")
        by_cmd.setdefault(c, []).append(e.get("latency_ms", 0))

    print("=" * 60)
    print("  embr performance report")
    print("=" * 60)

    print_section("Command ack latency (all)", all_latencies)
    print_section("Command ack latency (interactive)", interactive_latencies)

    if by_cmd:
        print("\n  Per-command breakdown:")
        for cmd in sorted(by_cmd):
            vals = by_cmd[cmd]
            p = percentiles(vals)
            print(f"    {cmd:20s}  n={len(vals):5d}  "
                  f"p50={fmt_ms(p[50])}  p95={fmt_ms(p[95])}  "
                  f"max={fmt_ms(max(vals))}")

    # ── Input-to-next-frame latency ──────────────────────────────
    input_to_frame = [
        e["input_to_frame_ms"] for e in frame_emits
        if "input_to_frame_ms" in e
    ]
    print_section("Input-to-next-frame latency", input_to_frame)

    # ── Frame interval ───────────────────────────────────────────
    intervals = [e["interval_ms"] for e in frame_emits if "interval_ms" in e]
    print_section("Frame interval", intervals)
    if intervals:
        p50 = percentiles(intervals)[50]
        if p50 and p50 > 0:
            print(f"    effective FPS (1000/p50): {1000 / p50:.1f}")

    # ── Frame staleness (measured at Emacs render time) ─────────
    staleness = [e["frame_staleness_ms"] for e in frame_renders
                 if "frame_staleness_ms" in e]
    print_section("Frame staleness (capture_done → render)", staleness)

    # ── Capture duration ─────────────────────────────────────────
    captures = [e["capture_ms"] for e in capture_dones if "capture_ms" in e]
    print_section("Capture duration", captures)

    # ── Frame sizes ──────────────────────────────────────────────
    sizes = [e["bytes"] for e in capture_dones if "bytes" in e]
    if sizes:
        p = percentiles(sizes)
        print(f"\n  Frame size (n={len(sizes)})")
        print(f"    p50  {fmt_ms(p[50])} bytes")
        print(f"    p95  {fmt_ms(p[95])} bytes")
        print(f"    max  {fmt_ms(max(sizes))} bytes")

    # ── Freeze events ────────────────────────────────────────────
    freezes = [i for i in intervals if i >= FREEZE_THRESHOLD_MS]
    severe = [i for i in intervals if i >= SEVERE_FREEZE_MS]
    print(f"\n  Freeze events (>={FREEZE_THRESHOLD_MS}ms): {len(freezes)}")
    if freezes:
        print(f"    durations: {', '.join(f'{v:.0f}ms' for v in sorted(freezes, reverse=True)[:10])}")
    print(f"  Severe freezes (>={SEVERE_FREEZE_MS}ms): {len(severe)}")

    # ── Mode time distribution ────────────────────────────────
    mode_changes = [e for e in events if e.get("event") == "mode_change"]
    if mode_changes:
        # Compute time spent in each mode from mode_change transitions.
        mode_time = {"interactive": 0.0, "watch": 0.0}
        for i, mc in enumerate(mode_changes):
            old = mc.get("old_mode", "watch")
            ts = mc["ts_ms"]
            if i > 0:
                prev_ts = mode_changes[i - 1]["ts_ms"]
                mode_time[old] = mode_time.get(old, 0) + (ts - prev_ts)
        # Account for time after the last mode_change until session end.
        ts_list_tmp = [e["ts_ms"] for e in events if "ts_ms" in e]
        if ts_list_tmp:
            last_mode = mode_changes[-1].get("new_mode", "watch")
            mode_time[last_mode] = mode_time.get(last_mode, 0) + (
                max(ts_list_tmp) - mode_changes[-1]["ts_ms"])
        total_mode = sum(mode_time.values())
        print(f"\n  Mode time distribution ({len(mode_changes)} transitions)")
        for m in ("interactive", "watch"):
            t = mode_time.get(m, 0)
            pct = (t / total_mode * 100) if total_mode > 0 else 0
            print(f"    {m:15s} {t / 1000:8.1f}s  ({pct:5.1f}%)")

    # ── Adaptation step counts ────────────────────────────────
    adapt_steps = [e for e in events if e.get("event") == "adapt_step"]
    if adapt_steps:
        downs = [e for e in adapt_steps if e.get("direction") == "down"]
        ups = [e for e in adapt_steps if e.get("direction") == "up"]
        last = adapt_steps[-1]
        print(f"\n  Adaptive capture ({len(adapt_steps)} steps)")
        print(f"    step_down: {len(downs)}    step_up: {len(ups)}")
        print(f"    final fps={last.get('fps', '?')}  "
              f"jpeg_quality={last.get('jpeg_quality', '?')}")
    else:
        print(f"\n  Adaptive capture (0 steps)")
        print(f"    step_down: 0    step_up: 0")

    # ── FPS distribution ──────────────────────────────────────
    if intervals:
        fps_values = [1000 / i for i in intervals if i > 0]
        if fps_values:
            buckets = [(0, 10), (10, 20), (20, 30), (30, 40),
                       (40, 50), (50, 999)]
            labels = ["0-10", "10-20", "20-30", "30-40",
                      "40-50", "50+"]
            print(f"\n  FPS distribution (n={len(fps_values)})")
            for (lo, hi), label in zip(buckets, labels):
                count = sum(1 for f in fps_values if lo <= f < hi)
                pct = count / len(fps_values) * 100
                bar = "#" * int(pct / 2)
                print(f"    {label:6s}  {count:5d}  ({pct:5.1f}%)  {bar}")

    # ── Frame drop stats ──────────────────────────────────────
    # Daemon-side drops (input-priority suppression).
    frame_drops = [e for e in events if e.get("event") == "frame_drop"]
    daemon_drops = len(frame_drops)
    by_reason = {}
    for e in frame_drops:
        r = e.get("reason", "unknown")
        by_reason[r] = by_reason.get(r, 0) + 1

    # Render-side skips (Emacs dropped intermediate frames).
    render_skips = sum(e.get("frames_skipped", 0) for e in frame_renders)

    total_drops = daemon_drops + render_skips
    total_opportunities = len(frame_emits) + daemon_drops
    ratio = total_drops / total_opportunities if total_opportunities else 0

    print(f"\n  Frame drops (daemon={daemon_drops}, render_skips={render_skips})")
    if by_reason:
        for reason in sorted(by_reason):
            print(f"    {reason:20s}  {by_reason[reason]}")
    print(f"    dropped_frame_ratio: {ratio:.3f} "
          f"({total_drops}/{total_opportunities})")

    # ── Summary ──────────────────────────────────────────────────
    total_cmds = len(cmd_receives)
    total_frames = len(frame_emits)
    ts_list = [e["ts_ms"] for e in events if "ts_ms" in e]
    duration_s = (max(ts_list) - min(ts_list)) / 1000 if len(ts_list) >= 2 else 0
    avg_fps = total_frames / duration_s if duration_s > 0 else 0

    print(f"\n  Summary")
    print(f"    Total commands:  {total_cmds}")
    print(f"    Total frames:    {total_frames}")
    print(f"    Duration:        {duration_s:.1f}s")
    print(f"    Avg FPS:         {avg_fps:.1f}")
    print("=" * 60)


if __name__ == "__main__":
    main()

#!/usr/bin/env bash
# M3 benchmark protocol: PLAN-3 screencast vs screenshot comparison.
#
# Prints the benchmark protocol, run matrix, and post-run commands.
# Benchmarks are run manually via Emacs (automated Emacs interaction
# is out of scope).
#
# Prerequisites:
#   - embr installed and working (M-x embr-browse)
#   - embr-perf-log enabled in Emacs config: (setq embr-perf-log t)
#
# Usage:
#   bash tools/run-benchmarks.sh [OUTPUT_DIR] [REPS]
#
# Protocol (per PLAN-3 section 9):
#   Scenarios: S1 (browsing), S2 (video+input), S3 (hover/click), S4 (endurance)
#   Modes: screenshot, auto, screencast
#   Repetitions: 3 per scenario per mode (default)
#   Duration: 10 minutes per run

set -euo pipefail

OUTPUT_DIR="${1:-benchmark-results}"
REPS="${2:-3}"
MODES=("screenshot" "auto" "screencast")
SCENARIOS=("S1" "S2" "S3" "S4")

mkdir -p "$OUTPUT_DIR"

cat <<'PROTOCOL'
==============================================================
  PLAN-3 M3 Benchmark Protocol
==============================================================

Manual procedure for each run:

  1. Set embr-frame-source to the target mode in your Emacs config.
  2. Enable perf logging: (setq embr-perf-log t)
  3. Restart embr: M-x embr-browse
  4. Execute the scenario actions (see below) for 10 minutes.
  5. Quit embr: C-c q
  6. Copy /tmp/embr-perf.jsonl to the output directory.

Scenario definitions:
  S1 - Baseline browsing: navigate 5 sites, scroll, read, follow links.
  S2 - Video playback: play a 1080p60 video, interact (pause/seek/scroll).
  S3 - Hover/click stress: rapid mouse movement over interactive elements.
  S4 - Long-session endurance: leave running for 10 minutes, mix of idle
       and bursts of interaction.

Measurement quality (per PLAN-3 section 8.4):
  - Minimum 3 repetitions per mode per scenario.
  - Identical machine/load conditions across runs.
  - Warm-up: discard first 60s of each run before sampling.
  - Report medians across runs with p95 shown per run set.

==============================================================
PROTOCOL

echo ""
echo "Output directory: $OUTPUT_DIR"
echo "Repetitions per mode per scenario: $REPS"
echo ""

# Generate run matrix.
run_id=0
for scenario in "${SCENARIOS[@]}"; do
    for mode in "${MODES[@]}"; do
        for rep in $(seq 1 "$REPS"); do
            run_id=$((run_id + 1))
            dest="$OUTPUT_DIR/${scenario}_${mode}_r${rep}.jsonl"
            printf "  Run %3d: %-4s %-12s rep %d -> %s\n" \
                "$run_id" "$scenario" "$mode" "$rep" "$dest"
        done
    done
done

total=$run_id
echo ""
echo "Total runs: $total"
echo ""
echo "After completing all runs, generate per-scenario reports:"
echo ""
echo "  # Per-scenario comparison with per-run appendix (recommended):"
for scenario in "${SCENARIOS[@]}"; do
    echo "  python3 tools/embr-perf-report.py --min-runs $REPS \\"
    echo "    --appendix-dir $OUTPUT_DIR/appendix \\"
    echo "    $OUTPUT_DIR/${scenario}_*.jsonl"
done
echo ""
echo "  # Acceptance gate (requires >= $REPS baseline + candidate runs):"
echo "  python3 tools/acceptance-gate.py \\"
echo "    --baseline $OUTPUT_DIR/S2_screenshot_r*.jsonl \\"
echo "    --candidate $OUTPUT_DIR/S2_auto_r*.jsonl"

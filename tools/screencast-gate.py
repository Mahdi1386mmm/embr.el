#!/usr/bin/env python3
"""M0 feasibility gate: verify CDP screencast and screenshot fallback.

Phase 1: screencast throughput (>= 300 frames in 30s on animated page).
Phase 2: screenshot fallback (5 successful captures after stopping screencast).

Exit code 0 on pass, 1 on fail.

Usage:
    python tools/screencast-gate.py [--url URL] [--duration SEC] [--threshold N]
"""

import argparse
import asyncio
import sys
import time

# Animated test page: CSS spinner forces compositor repaints.
_ANIMATED_PAGE = (
    "data:text/html,"
    "<style>"
    "@keyframes spin{to{transform:rotate(360deg)}}"
    ".b{width:200px;height:200px;"
    "background:linear-gradient(red,blue);"
    "animation:spin 1s linear infinite}"
    "</style>"
    "<div class='b'></div>"
)


async def run_gate(url, duration, threshold):
    from playwright.async_api import async_playwright
    from cloakbrowser.download import ensure_binary
    from cloakbrowser.config import IGNORE_DEFAULT_ARGS, get_default_stealth_args

    pw = await async_playwright().start()
    binary_path = ensure_binary()
    browser = await pw.chromium.launch(
        executable_path=binary_path,
        headless=True,
        args=get_default_stealth_args(),
        ignore_default_args=IGNORE_DEFAULT_ARGS + ["--mute-audio"],
    )
    page = await browser.new_page(viewport={"width": 1280, "height": 720})
    await page.goto(url, wait_until="domcontentloaded", timeout=15000)

    # ── Phase 1: screencast throughput ────────────────────────────
    cdp = await page.context.new_cdp_session(page)
    await cdp.send("Page.enable")
    frame_count = 0
    ack_errors = 0

    def _on_ack_done(fut):
        nonlocal ack_errors
        if fut.exception():
            ack_errors += 1

    def on_frame(params):
        nonlocal frame_count
        frame_count += 1
        fut = asyncio.ensure_future(
            cdp.send("Page.screencastFrameAck",
                     {"sessionId": params["sessionId"]}))
        fut.add_done_callback(_on_ack_done)

    cdp.on("Page.screencastFrame", on_frame)
    await cdp.send("Page.startScreencast", {
        "format": "jpeg",
        "quality": 80,
        "maxWidth": 1280,
        "maxHeight": 720,
        "everyNthFrame": 1,
    })

    print(f"screencast-gate: phase 1 — recording for {duration}s on {url}")
    t0 = time.monotonic()
    await asyncio.sleep(duration)
    elapsed = time.monotonic() - t0

    await cdp.send("Page.stopScreencast")
    # Drain in-flight ack futures so late failures are counted.
    await asyncio.sleep(0.5)
    await cdp.detach()

    fps = frame_count / elapsed if elapsed > 0 else 0
    frames_ok = frame_count >= threshold
    # Ack error rate > 1% is a fail.
    ack_rate = ack_errors / max(1, frame_count)
    ack_ok = ack_rate <= 0.01

    print(f"  frames:     {frame_count} (threshold: {threshold})")
    print(f"  duration:   {elapsed:.1f}s")
    print(f"  fps:        {fps:.1f}")
    print(f"  ack_errors: {ack_errors} ({ack_rate:.2%})")
    print(f"  phase 1:    {'PASS' if frames_ok and ack_ok else 'FAIL'}")

    # ── Phase 2: screenshot fallback ─────────────────────────────
    print("screencast-gate: phase 2 — screenshot fallback")
    fallback_ok = True
    fallback_count = 5
    for i in range(fallback_count):
        try:
            jpg = await page.screenshot(type="jpeg", quality=80)
            if len(jpg) < 100:
                print(f"  screenshot {i+1}: suspiciously small ({len(jpg)} bytes)")
                fallback_ok = False
                break
        except Exception as e:
            print(f"  screenshot {i+1}: error: {e}")
            fallback_ok = False
            break
    print(f"  fallback:   {'PASS' if fallback_ok else 'FAIL'}"
          f" ({fallback_count} captures)")

    await browser.close()
    await pw.stop()

    # ── Verdict ──────────────────────────────────────────────────
    passed = frames_ok and ack_ok and fallback_ok
    print(f"screencast-gate: {'PASS' if passed else 'FAIL'}")
    return 0 if passed else 1


def main():
    parser = argparse.ArgumentParser(description="M0 screencast feasibility gate")
    parser.add_argument("--url", default=_ANIMATED_PAGE,
                        help="URL to test (default: animated data URI)")
    parser.add_argument("--duration", type=int, default=30,
                        help="Test duration in seconds (default: 30)")
    parser.add_argument("--threshold", type=int, default=300,
                        help="Minimum frame count to pass (default: 300)")
    args = parser.parse_args()
    sys.exit(asyncio.run(run_gate(args.url, args.duration, args.threshold)))


if __name__ == "__main__":
    main()

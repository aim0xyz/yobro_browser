#!/usr/bin/env python3
"""Visual comparison between a shell capture and its reference image.

The comparison is a warning system, not a blind pixel target (PLAN.md Phase 1):
differences from web content, anti-aliasing and native traffic lights are
masked or tolerated. It writes a per-pixel difference heatmap and reports one
numeric score (mean absolute delta per channel, 0-255).

Exit codes:
    0  within tolerance (score below the soft threshold)
    1  structural mismatch (score above the hard threshold, size mismatch,
       or a missing input)
"""

import sys
from pathlib import Path

from PIL import Image, ImageChops

# Masks are rectangles (x, y, w, h) in pixels, applied to both images before
# scoring. The native traffic lights exist only in real windows, never in an
# offscreen capture, and page content behind a WebEngineView may drift.
DEFAULT_MASKS = [(0, 0, 200, 100)]

SOFT_THRESHOLD = 12.0
HARD_THRESHOLD = 45.0


def load(path: Path) -> Image.Image:
    image = Image.open(path).convert("RGB")
    if image.width % 2 == 0 and image.height % 2 == 0:
        pass
    return image


def apply_masks(image: Image.Image, masks) -> Image.Image:
    work = image.copy()
    for x, y, w, h in masks:
        # Fill with the local background instead of black so masked areas do
        # not dominate the score.
        sample = work.getpixel((min(x + w + 8, work.width - 1), min(y + h // 2, work.height - 1)))
        for yy in range(max(0, y), min(y + h, work.height)):
            for xx in range(max(0, x), min(x + w, work.width)):
                work.putpixel((xx, yy), sample)
    return work


def normalize(reference: Image.Image, capture: Image.Image):
    width = min(reference.width, capture.width)
    height = min(reference.height, capture.height)
    if (reference.width, reference.height) != (capture.width, capture.height):
        print(
            f"note: sizes differ — reference {reference.size}, capture {capture.size}; "
            f"comparing the common {width}x{height} region"
        )
    return (
        reference.crop((0, 0, width, height)),
        capture.crop((0, 0, width, height)),
    )


def main() -> int:
    if len(sys.argv) < 4:
        print("usage: VisualReferenceDiff.py <reference.png> <capture.png> <diff.png> [fail-mean]")
        return 1
    reference_path = Path(sys.argv[1])
    capture_path = Path(sys.argv[2])
    diff_path = Path(sys.argv[3])
    hard = float(sys.argv[4]) if len(sys.argv) > 4 else HARD_THRESHOLD

    if not reference_path.exists():
        print(f"chromium-visual-diff FAILED: missing reference {reference_path}")
        return 1
    if not capture_path.exists():
        print(f"chromium-visual-diff FAILED: missing capture {capture_path}")
        return 1

    reference, capture = normalize(load(reference_path), load(capture_path))
    reference = apply_masks(reference, DEFAULT_MASKS)
    capture = apply_masks(capture, DEFAULT_MASKS)

    difference = ImageChops.difference(reference, capture)
    histogram = difference.convert("L").histogram()
    total = sum(histogram)
    if total == 0:
        print("chromium-visual-diff FAILED: empty comparison region")
        return 1
    weighted = sum(value * count for value, count in enumerate(histogram))
    score = weighted / total

    # Amplify so small deltas stay visible in the heatmap.
    amplified = difference.point(lambda value: min(255, value * 6))
    amplified.save(diff_path)

    verdict = "OK"
    if score >= hard:
        verdict = "FAILED"
    elif score >= SOFT_THRESHOLD:
        verdict = "WARN"

    print(
        f"chromium-visual-diff {verdict}: mean delta {score:.2f}/255 "
        f"({capture_path.name} vs {reference_path.name}); heatmap {diff_path}"
    )
    return 1 if verdict == "FAILED" else 0


if __name__ == "__main__":
    sys.exit(main())

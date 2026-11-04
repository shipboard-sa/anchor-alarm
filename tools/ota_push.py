"""
Purpose: Push a signed anchor-box firmware image to the box over Wi-Fi (OTA) and report the result.
Version: v1 (2026-10-02)
Scope: STANDALONE UTILITY - laptop-side tool for the anchor box (SPEC 8).
Architecture: ./tools. Talks to main/net.c on the box: GET / (status), POST /ota (raw image).
    The box verifies the image signature itself; this script only uploads. Stdlib only.
Invocation: python tools\\ota_push.py [--host anchor-box.local] [--bin build\\anchor_box.bin] [--status]
    Run from code\\anchor-box after `idf.py build` (boat build). Bench build: --bin build-bench\\anchor_box.bin.
"""
from __future__ import annotations

import argparse
import sys
import time
import urllib.error
import urllib.request


def status(host: str) -> str:
    with urllib.request.urlopen(f"http://{host}/", timeout=10) as r:
        return r.read().decode("utf-8", "replace")


def main() -> int:
    ap = argparse.ArgumentParser(description="Push signed firmware to the anchor box over Wi-Fi")
    ap.add_argument("--host", default="anchor-box.local", help="box name or IP (default anchor-box.local)")
    ap.add_argument("--bin", default=r"build\anchor_box.bin", help="signed app image (default: boat build)")
    ap.add_argument("--status", action="store_true", help="only show the box's status")
    a = ap.parse_args()

    try:
        print(status(a.host))
    except OSError as e:
        print(f"cannot reach the box at {a.host}: {e}")
        return 1
    if a.status:
        return 0

    data = open(a.bin, "rb").read()
    print(f"uploading {a.bin} ({len(data)} bytes) to {a.host} ...")
    req = urllib.request.Request(f"http://{a.host}/ota", data=data, method="POST",
                                 headers={"Content-Type": "application/octet-stream"})
    try:
        with urllib.request.urlopen(req, timeout=180) as r:
            print(r.read().decode("utf-8", "replace").strip())
    except urllib.error.HTTPError as e:
        print(f"REFUSED ({e.code}): {e.read().decode('utf-8', 'replace').strip()}")
        return 2

    print("waiting for the box to restart ...")
    for _ in range(30):
        time.sleep(2)
        try:
            print(status(a.host))
            return 0
        except OSError:
            pass
    print("box did not answer after the restart - check the screen / USB log")
    return 3


if __name__ == "__main__":
    sys.exit(main())

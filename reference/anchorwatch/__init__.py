"""
Purpose: Package marker for the anchor watch - circle-fit anchor alarm (fit the swing arc, centre =
    anchor, lock, freeze, alarm outside radius + tolerance).
Version: v1 (2026-09-30)
Scope: STANDALONE UTILITY - dry-run only; nothing live depends on it (OQ-098 'start from scratch' path).
Architecture: reference - the Python REFERENCE MODEL of the anchor box (moved out of your-pi-hostname stacks/anchor-watch 2026-10-02, ANC-14); the ESP32 C port must pass the same cases. geo.py (projection) + fit.py (circle fit) -> watch.py
    (state machine); sim.py (synthetic swings), replay.py (GPX/CSV), service.py (SignalK dry run),
    __main__.py (CLI).
Invocation: imported as `anchorwatch`; CLI `python -m anchorwatch ...`; tests `python -m pytest`
    from reference.
"""
from .watch import AnchorWatch, WatchConfig  # noqa: F401

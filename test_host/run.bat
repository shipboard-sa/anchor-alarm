@echo off
REM Purpose: Build + run the PC host tests of the C ports against the Python reference: boat time
REM   (main\boattime.c), the anchor watch (main\anchorwatch.c, scenario replay), the monitoring-host watch
REM   (main\hostwatch.c, ANC-25), the night-watch remote (main\nightwatch.c, ANQ-30) and the engine alerts
REM   (main\alerts.c, ANQ-10) and the tank-fill taps (main\fill.c, ANC-41) - the last three fixed cases.
REM Version: v10 (2026-10-21) - v10: watchkeeper check-in test added (main\wkeep.c, ANQ-27). v9 (2026-10-18) - v9: cold-box EXPECTED? test added (main\restock.c, ANC-45). v8 (2026-10-15) - v8: DOCK / LEAVE test added (main\boatmode.c, ANC-44). v7 (2026-10-11) - v7: hot-water test added (main\hotwater.c, ANC-43). v6 (2026-10-09) - v6: tank-fill test added. v5 (2026-10-07) - v5: engine-alerts test added. v4 (2026-10-06) - v4: night-watch test added. v3: hostwatch test added. v2: anchor-watch scenario test added. v1: boat time
REM Scope: STANDALONE UTILITY - anchor-box test tooling.
REM Architecture: regenerates test vectors from reference\ (tools\make_c_vectors.py, make_watch_vectors.py,
REM   make_hostwatch_vectors.py), compiles with zig cc (pip package ziglang in reference\.venv), runs the exes.
REM   No ESP-IDF needed.
REM Invocation: test_host\run.bat from D:\boat\AnchorAlarm. Exit code 0 = all pass.
setlocal
cd /d "%~dp0.."
set PY=reference\.venv\Scripts\python.exe
set CC=%PY% -m ziglang cc -std=c11 -O1 -Wall -Wextra -ffp-contract=off
%PY% tools\make_c_vectors.py || exit /b 1
%PY% tools\make_watch_vectors.py || exit /b 1
%PY% tools\make_hostwatch_vectors.py || exit /b 1
%CC% -o test_host\test_boattime.exe test_host\test_boattime.c main\boattime.c || exit /b 1
%CC% -o test_host\test_watch.exe test_host\test_watch.c main\anchorwatch.c || exit /b 1
%CC% -o test_host\test_hostwatch.exe test_host\test_hostwatch.c main\hostwatch.c || exit /b 1
%CC% -o test_host\test_nightwatch.exe test_host\test_nightwatch.c main\nightwatch.c || exit /b 1
%CC% -o test_host\test_alerts.exe test_host\test_alerts.c main\alerts.c || exit /b 1
%CC% -o test_host\test_fill.exe test_host\test_fill.c main\fill.c || exit /b 1
%CC% -o test_host\test_hotwater.exe test_host\test_hotwater.c main\hotwater.c || exit /b 1
%CC% -o test_host\test_boatmode.exe test_host\test_boatmode.c main\boatmode.c || exit /b 1
%CC% -o test_host\test_restock.exe test_host\test_restock.c main\restock.c || exit /b 1
%CC% -o test_host\test_wkeep.exe test_host\test_wkeep.c main\wkeep.c || exit /b 1
test_host\test_boattime.exe || exit /b 1
test_host\test_watch.exe || exit /b 1
test_host\test_hostwatch.exe || exit /b 1
test_host\test_nightwatch.exe || exit /b 1
test_host\test_alerts.exe || exit /b 1
test_host\test_fill.exe || exit /b 1
test_host\test_hotwater.exe || exit /b 1
test_host\test_boatmode.exe || exit /b 1
test_host\test_restock.exe || exit /b 1
test_host\test_wkeep.exe

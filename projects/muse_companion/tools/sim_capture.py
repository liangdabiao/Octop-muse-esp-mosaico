#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Capture simulator frames of the companion UI for visual review.

Every capture is one headless sim_host run against the already built
`build-sim/companion_backend_assets/app.gspb` plus its native backend, so the
frames show the real `companion_ui.c` render path, not a static scene.

    python tools/sim_capture.py --out .codex-runs/uicheck
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path

PROJ = Path(__file__).resolve().parents[1]
COMPONENT = PROJ / "managed_components/espressif__esp-gsp"
BUNDLE = PROJ / "build-sim/companion_backend_assets/app.gspb"
BACKEND = PROJ / "build-sim/companion_backend.exe"
FONT = PROJ / "ui/fonts/NotoSansSC-Regular.ttf"
FPS = 60


def sim_binary() -> Path:
    version = re.search(r'^version:\s*"?([\d.]+)"?',
                        (COMPONENT / "idf_component.yml").read_text(encoding="utf-8"),
                        re.MULTILINE).group(1)
    path = Path.home() / ".cache/esp-mosaico/sim" / version / "sim.exe"
    if not path.is_file():
        raise SystemExit(f"simulator not cached: {path} (run the gsp-sim wrapper once)")
    return path


def actions(spec: list[list[str]]) -> list[str]:
    out = []
    for action in spec:
        out.extend(action)
    return out


def capture(sim: Path, dump: Path, spec: list[list[str]],
            env: dict[str, str], timeout: float) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory(prefix="gsp-capture-") as tmp:
        ready = Path(tmp) / "ready.json"
        command = [str(sim), "--bundle", str(BUNDLE), "--dynamic-font", str(FONT),
                   "--headless", "--fps", str(FPS), "--backend-enable", "--backend-required",
                   "--backend-idle-timeout", "30", "--ready-file", str(ready),
                   "--dump", str(dump), "--dump-format", "png", "--log-level", "warn"]
        command.extend(actions(spec))
        sim_proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, encoding="utf-8", errors="replace")
        backend = None
        trace = None
        deadline = time.monotonic() + timeout
        info = None
        while time.monotonic() < deadline:
            if sim_proc.poll() is not None:
                break
            try:
                info = json.loads(ready.read_text(encoding="utf-8"))
                break
            except (FileNotFoundError, json.JSONDecodeError):
                time.sleep(0.05)
        if info is not None:
            child = dict(os.environ)
            child.update(env)
            trace = open(dump.with_suffix(".log"), "wb")
            backend = subprocess.Popen([str(BACKEND), "--endpoint", info["backend"]],
                                       stdout=trace, stderr=subprocess.STDOUT, env=child)
        try:
            out = sim_proc.communicate(timeout=max(1.0, deadline - time.monotonic()))[0]
        except subprocess.TimeoutExpired:
            sim_proc.kill()
            out = sim_proc.communicate()[0]
        finally:
            if backend and backend.poll() is None:
                subprocess.run(["taskkill", "/PID", str(backend.pid), "/T", "/F"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
                backend.wait()
            elif backend:
                backend.wait()
            if trace:
                trace.close()
        return dump.is_file(), (out or "")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--only", default=None, help="run one capture by label")
    parser.add_argument("--language", default="zh", choices=["zh", "en"])
    args = parser.parse_args()

    if not BUNDLE.is_file() or not BACKEND.is_file():
        raise SystemExit(f"simulator build missing: run the gsp-sim bridge first ({BUNDLE})")
    args.out.mkdir(parents=True, exist_ok=True)
    turn = {"OCTOP_SIM_AUTOTURN": "1", "OCTOP_SIM_LANGUAGE": ""}
    w = lambda n: [["--wait", str(int(n * FPS))]]
    tap = lambda x, y: [["--tap", str(x), str(y)]]
    base = dict(turn)
    if args.language == "en":
        base["OCTOP_SIM_LANGUAGE"] = "en"
    quiet = dict(base, OCTOP_SIM_AUTOTURN="0")

    # Simulated time, not frames, owns the mock's phases: the backend advances
    # its clock 100 ms per UI tick and a tick costs about seven frames, so one
    # simulated second is roughly 84 frames. The mock turn is key release at
    # 2.0 s, recognition result at 2.5 s, eight streamed pieces from 2.9 s to
    # 7.8 s and the final reply at 8.5 s; each capture below is placed against
    # that table, and each .log now carries the push clocks it reached.
    captures = [
        ("01_home_boot", 20, w(1.2), base),
        ("02_home_listening", 30, w(12.0), dict(base, OCTOP_SIM_KEY_MS="2:30")),
        ("03_home_thinking", 20, w(3.4), base),
        # Measured on this build: a 100 ms UI tick costs about 12 sim frames, so
        # one simulated second is ~120 frames, not the 84 the table below assumed
        # when it was written. The transcript arrives at 2.5 s and the first
        # streamed piece at 2.9 s, which is the only window where the card
        # carrying the spoken question exists.
        ("03b_home_heard_card", 20, w(5.2), base),
        ("04_read_streaming", 30, w(7.0), base),
        ("05_read_final_page", 40, w(12.0), base),
        ("06_settings", 30, w(2.0) + tap(438, 80) + w(2.0), quiet),
        ("07_home_after_back", 30, w(2.0) + tap(438, 80) + w(2.0) + tap(61, 44) + w(2.0), quiet),
        ("08_read_manual_next", 40, w(12.0) + tap(413, 463) + w(0.5), base),
        ("09_setup_hotspot", 30, w(2.0), dict(quiet, OCTOP_SIM_SETUP="1")),
        ("10_link_error", 30, w(6.0), dict(base, OCTOP_SIM_REPLY_ERROR="1")),
        ("11_offline", 30, w(2.0), dict(quiet, OCTOP_SIM_OFFLINE="1")),
        ("12_pet_happy", 30, w(5.0) + tap(240, 220) + w(0.5), quiet),
    ]
    sim = sim_binary()
    failures = []
    for label, timeout, spec, env in captures:
        if args.only and args.only != label:
            continue
        dump = args.out / f"{label}.png"
        if dump.is_file():
            dump.unlink()
        ok, log = capture(sim, dump, spec, env, timeout)
        print(f"{'OK ' if ok else 'FAIL'} {label} -> {dump.name}")
        if not ok:
            failures.append(label)
            print(log[-1500:])
    if failures:
        print("failed captures:", ",".join(failures))
        return 1
    print(f"ALL FRAMES IN {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

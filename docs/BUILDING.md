# Build and preview (Octop backend)

Requirements: Python 3.10+, target `esp32s31`, and ESP-IDF commit `7b9cc1ac79f865983f59bb8ff3ff43eb74ff1dbe`. Do not substitute IDF master — the GSP bundle, `esp_display_present` and the Mosaico BSP are pinned to it. Initialize the pinned dependencies first:

```powershell
python tools/bootstrap.py
python -m pip install -r requirements.txt
```

## Firmware

Regenerate the UI resources (never hand-edit `ui/*.json` or the generated scene C sources), then build:

```powershell
python projects/muse_companion/tools/prepare_ui.py --character original
powershell -NoProfile -ExecutionPolicy Bypass -File projects/muse_companion/build_octop.ps1
```

`build_octop.ps1` is the only supported entry point because it encodes four environment rules that fail silently otherwise; read it before changing anything. The script resolves the workspace from its own location, so a clone anywhere builds without editing absolute paths, and it reads `IDF_PATH`, `IDF_TOOLS_PATH` and `IDF_COMPONENT_CACHE_PATH` from the environment when you set them.

1. `IDF_PATH` and `IDF_TOOLS_PATH` must stay a registered pair — export the pair your machine actually uses (the script's built-in fallbacks are the original author's Windows layout). Switching pairs requires `build_octop.ps1 -Clean`, and a pair mismatch requires a fullclean.
2. Launched from Git Bash, MSYS2 re-injects `MSYSTEM` into child processes and `idf_tools.py` rejects it fatally, so the script deletes it for the run.
3. The default component-manager cache under `%LOCALAPPDATA%` cannot be resolved by git-for-windows; the script falls back to a repo-local `.component-manager-cache` directory (git-ignored), which you can override by exporting `IDF_COMPONENT_CACHE_PATH`.
4. ninja's `.ninja_deps` records `/D:/`-style paths for GSP generated files and then errors on the next incremental build, so the script deletes that single file each run.

ccache: this machine's `idf.py` always configures with `-DCCACHE_ENABLE=True`, even when `IDF_CCACHE_ENABLE` is unset, so compiler lines are ccache-wrapped and a rebuild of unchanged sources is fast. That affects reported build *time* only — the link step always runs, so the size numbers below are real. Set `CCACHE_DISABLE=1` when you need an honest cold-build timing baseline.

The retained factory Recovery slot is smaller than the application: the application installs into `ota_0`, not `factory`. Do not resize Recovery to silence a warning, and do not touch the partition table.

## Host checks

Both run without hardware and are the fastest way to catch a regression in credentials or the state machine:

```powershell
cmake -S projects/muse_companion/tools/tests -B .codex-runs/setup-tests -DCMAKE_BUILD_TYPE=Release
cmake --build .codex-runs/setup-tests -j4
.codex-runs/setup-tests/setup_tests.exe

gcc -Wall -Wextra -Werror -I projects/muse_companion/main -I projects/muse_companion/pc `
    -o model_test.exe projects/muse_companion/pc/companion_model_test.c `
    projects/muse_companion/main/companion_model.c
./model_test.exe
```

`setup_tests` covers the Octop URL rules, the optional STT overrides, persistence reload and legacy-record wiping. `companion_model_test` covers the link gates, the recognition-then-thinking transition, turn paging, streaming versus final display, long-reply truncation, the 220-hanzi page geometry (line width, ten lines, no split codepoint), dimming and sleep suppression. Both must print `PASS`.

## Live Octop check

`tools/octop_probe.py` replays the device's own request sequence - login, agent list, thread, chat WebSocket, `/api/voice/stt` - against a real server, so firmware is never the first thing to discover a contract change. The password comes from `OCTOP_PASS` and is never read from a file or an argument:

```powershell
python -m pip install websockets
$env:OCTOP_PASS="…"
python projects/muse_companion/tools/octop_probe.py --url http://HOST:8088 --user USER
python projects/muse_companion/tools/octop_probe.py --url http://HOST:8088 --user USER --wav speech.wav --language zh-CN
```

The WAV must be 16 kHz mono PCM16, the shape the device uploads, and `--language` takes exactly the two hints `octop_stt.c` sends (`zh-CN`, `en-US`). The chat line reports the frame kinds it saw and when the first frame and first answer token arrived; the link aborts a turn after 45 s with no frame at all, so a server slower than that needs the constant raised rather than a device-side workaround.

## Simulator

```powershell
$env:GSP_SIM_DYNAMIC_FONT=(Resolve-Path projects/muse_companion/ui/fonts/NotoSansSC-Regular.ttf).Path
python mosaico.py project sim --project projects/muse_companion --interactive
```

The simulator mocks the link only; nothing it shows counts as Octop validation. Override the mock with `pc/platform_pc.c`'s environment switches: `OCTOP_SIM_SETUP`, `OCTOP_SIM_REPLY_ERROR`, `OCTOP_SIM_LANGUAGE` (`en`), `OCTOP_SIM_OFFLINE`, `OCTOP_SIM_UNPAIRED`, `OCTOP_SIM_LINK_STATE` (0–5, default 4 = reachable), `OCTOP_SIM_LINK_DETAIL`, `OCTOP_SIM_AUTOTURN`.

`GSP_SIM_DYNAMIC_FONT` matters for the reading page: GSPC packs a font subset built from the static strings in `main/*.c`, so without a dynamic TTF a mock Octop reply renders tofu boxes in the simulator while the device — which passes the whole `NotoSansSC-Regular.ttf` through `app_config.ttf` — shows every glyph. `OCTOP_SIM_AUTOTURN=1` holds the AI key from 1 s to 2 s of simulated time (move that window with `OCTOP_SIM_KEY_MS="start:end"` in seconds), so recognition, thinking, streaming and the reading page run unattended and can be captured with a scripted `--wait`/`--tap`/`--drag` sequence.

`projects/muse_companion/tools/sim_capture.py` drives exactly that: it runs one headless sim per scene state against the built `build-sim/companion_backend_assets/app.gspb`, so the PNGs show the real `companion_ui.c` render path rather than a static scene, and writes a `.log` next to each frame with the mock's own effect trace. Pass `--language en` to a second output directory to capture the English chrome.

Frames and simulated time are not the same clock: the backend advances 100 ms per UI tick and a tick costs several frames, so a capture's phase can only be confirmed after the fact. Every `push()` in `pc/platform_pc.c` therefore prints `sim push kind=… clock_ms=…`, and the waits in the capture list are placed against the mock's own turn table (key release 2.0 s, recognition 2.5 s, eight streamed pieces 2.9-7.8 s, final reply 8.5 s).

```powershell
python projects/muse_companion/tools/sim_capture.py --out .codex-runs/uicheck
```

Rebuild the native backend first with the `gsp-sim` wrapper (`--headless --duration 1`) after editing `main/*.c` or `pc/platform_pc.c`.

## Flashing and resources

From the IDF tools environment, build the full resource bundle with `cmake --build projects/muse_companion/build --target system-update-bundle`. Use `python mosaico.py iris system-update --project projects/muse_companion` when resources or layout change, and `iris app-update` only for code changes with an identical full partition table. See the dependency documentation under `submodule/esp-mosaico-utils/mosaico-tools/docs/` for device operations.

`projects/muse_companion/vendor/` (the superseded Muse Gadget SDK, Noise and product-info components) is deliberately outside the build graph — do not add it back to `main/CMakeLists.txt`. Bluetooth, the speaker/TTS path and the noise pipeline are gone; the image talks Wi-Fi only.

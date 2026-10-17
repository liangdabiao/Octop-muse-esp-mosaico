#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Walk the exact request sequence the device sends, against a real Octop.

Every step mirrors one function in components/octop_link so a failure names the
device code path that would break: login (octop_login), the agent probe
(probe_agent, which needs a string `agent_id`), the thread probe
(probe_thread, which needs 201 plus `thread_id`), the chat socket
(octop_ws_start, which authenticates by query token only) and the speech
upload (octop_stt_transcribe, multipart field `audio`).

    set OCTOP_PASS=...   # credentials never live in this file
    python tools/octop_probe.py --url http://host:8088 --user YOUR_USERNAME --text "你好"

Add --wav speech.wav to exercise recognition with a real 16 kHz mono recording.
"""
from __future__ import annotations

import argparse
import asyncio
import json
import os
import sys
import time
import urllib.error
import urllib.request

try:
    import websockets
except ImportError:  # pragma: no cover - the WS step degrades to a skip
    websockets = None


def request(url, method="GET", body=None, token=None, content_type=None, timeout=25):
    data = None
    if body is not None:
        if isinstance(body, dict):
            data = json.dumps(body).encode("utf-8")
            content_type = content_type or "application/json"
        else:
            data = body
    req = urllib.request.Request(url, data=data, method=method)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    if content_type:
        req.add_header("Content-Type", content_type)
    req.add_header("Accept", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, resp.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        return exc.code, exc.read().decode("utf-8", "replace")
    except Exception as exc:  # noqa: BLE001 - report, do not crash the probe
        return 0, repr(exc)


def split_base(url):
    """Return (scheme://host:port, base-path) the way octop_url_split does."""
    scheme, _, rest = url.partition("://")
    slash = rest.find("/")
    host = rest if slash < 0 else rest[:slash]
    base = "" if slash < 0 or rest[slash + 1:] == "" else rest[slash:]
    return f"{scheme}://{host}", base


def wav_bytes(path):
    with open(path, "rb") as handle:
        return handle.read()


async def chat(ws_url, thread, text, seconds):
    """Mirror on_frame(): only `token` frames are answer text, `reasoning` and
    `state_*` frames are proof of life. Timings decide whether the device's
    45 s first-token watchdog can fire on a healthy but slow turn."""
    frames, content = [], []
    first_frame = first_token = None
    waited = 0.0
    started = time.monotonic()
    async with websockets.connect(ws_url, max_size=4 * 1024 * 1024) as ws:
        await ws.send(json.dumps({"type": "user_turn", "thread_id": thread, "text": text}))
        try:
            while True:
                raw = await asyncio.wait_for(ws.recv(), timeout=seconds)
                waited = time.monotonic() - started
                if first_frame is None:
                    first_frame = waited
                try:
                    frame = json.loads(raw)
                except (json.JSONDecodeError, TypeError):
                    frames.append("<non-json>")
                    continue
                kind = frame.get("type", "?")
                frames.append(kind)
                if kind == "token" and frame.get("content"):
                    if first_token is None:
                        first_token = waited
                    content.append(str(frame["content"]))
                if kind == "done":
                    break
        except asyncio.TimeoutError:
            pass
    return frames, "".join(content), first_frame, first_token, waited


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", required=True, help="Octop base URL, e.g. http://host:8088")
    parser.add_argument("--user", required=True)
    parser.add_argument("--password", default=os.environ.get("OCTOP_PASS", ""))
    parser.add_argument("--text", default="用一句话回答：你现在在线吗？")
    parser.add_argument("--wav", default=None, help="16 kHz mono WAV to send to /api/voice/stt")
    parser.add_argument("--stt-url", default=None, help="optional recognition endpoint override")
    parser.add_argument("--language", default="zh-CN",
                        help="hint exactly as octop_stt.c sends it (zh-CN or en-US)")
    parser.add_argument("--wait", type=float, default=60.0, help="seconds to wait for `done`")
    args = parser.parse_args()
    if not args.password:
        raise SystemExit("set OCTOP_PASS (the password is never taken from a file)")

    origin, base = split_base(args.url.rstrip("/"))
    failures = []

    def step(name, ok, detail):
        print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")
        if not ok:
            failures.append(name)

    status, body = request(f"{origin}{base}/api/auth/login", "POST",
                           {"username": args.user, "password": args.password})
    token = ""
    try:
        token = json.loads(body).get("access_token", "")
    except json.JSONDecodeError:
        pass
    step("octop_login", status == 200 and token,
         f"HTTP {status}, token {len(token)} chars, body {body[:120]!r}")
    if not token:
        return 1

    status, body = request(f"{origin}{base}/api/agents", token=token)
    agent = ""
    try:
        parsed = json.loads(body)
        first = parsed[0] if isinstance(parsed, list) and parsed else parsed
        agent = str(first.get("agent_id", ""))
    except (json.JSONDecodeError, AttributeError, IndexError):
        pass
    step("probe_agent", status == 200 and agent,
         f"HTTP {status}, agent_id={agent!r}, body {body[:120]!r}")
    if not agent:
        return 1

    path = f"{origin}{base}/api/agents/{agent}/threads"
    status, body = request(path, token=token)
    thread = ""
    try:
        parsed = json.loads(body)
        first = parsed[0] if isinstance(parsed, list) and parsed else parsed
        thread = str(first.get("thread_id", ""))
    except (json.JSONDecodeError, AttributeError, IndexError):
        pass
    if not thread:
        status, body = request(path, "POST", {}, token=token)
        try:
            thread = str(json.loads(body).get("thread_id", ""))
        except json.JSONDecodeError:
            pass
    step("probe_thread", bool(thread), f"HTTP {status}, thread_id={thread!r}")
    if not thread:
        return 1

    if websockets is None:
        print("SKIP chat_ws: python -m pip install websockets")
    else:
        ws_scheme = "wss" if origin.startswith("https") else "ws"
        host = origin.split("://", 1)[1]
        ws_url = f"{ws_scheme}://{host}{base}/api/agents/{agent}/chat/ws?token={token}"
        try:
            frames, text, t_first, t_token, t_total = asyncio.run(
                chat(ws_url, thread, args.text, args.wait))
            secs = lambda t: "never" if t is None else f"{t:.2f}s"
            step("chat_ws", "done" in frames and bool(text),
                 f"counts={ {k: frames.count(k) for k in sorted(set(frames)) } }, "
                 f"first frame {secs(t_first)}, first token {secs(t_token)}, "
                 f"turn {t_total:.2f}s, reply {len(text)} chars")
            if text:
                print(f"     reply: {text[:200]}")
        except Exception as exc:  # noqa: BLE001
            step("chat_ws", False, repr(exc))

    stt_base = args.stt_url or f"{origin}{base}"
    stt_origin, stt_path = split_base(stt_base) if args.stt_url else (stt_base, "/api/voice/stt")
    if not args.wav:
        print("SKIP voice_stt: pass --wav speech.wav (16 kHz mono) to exercise recognition")
    else:
        boundary = "----OctopPassportBoundary"
        audio = wav_bytes(args.wav)
        parts = [f"--{boundary}\r\nContent-Disposition: form-data; name=\"audio\"; "
                 f"filename=\"speech.wav\"\r\nContent-Type: audio/wav\r\n\r\n".encode()]
        parts.append(audio)
        parts.append(f"\r\n--{boundary}\r\nContent-Disposition: form-data; name=\"language\""
                     f"\r\n\r\n{args.language}\r\n--{boundary}--\r\n".encode())
        payload = b"".join(parts)
        status, body = request(f"{stt_origin}{stt_path}", "POST", payload,
                               token=token if not args.stt_url else None,
                               content_type=f"multipart/form-data; boundary={boundary}",
                               timeout=40)
        heard = ""
        try:
            heard = str(json.loads(body).get("text", ""))
        except json.JSONDecodeError:
            pass
        step("voice_stt", status == 200 and len(heard) >= 2,
             f"HTTP {status}, transcript={heard[:60]!r}")

    print("PROBE OK" if not failures else "PROBE FAILED: " + ",".join(failures))
    return 0 if not failures else 1


if __name__ == "__main__":
    sys.exit(main())

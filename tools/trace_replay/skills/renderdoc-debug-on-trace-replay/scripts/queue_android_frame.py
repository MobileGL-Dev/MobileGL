#!/usr/bin/env python3
"""Queue an exact RenderDoc frame through an Android TargetControl socket."""

from __future__ import annotations

import argparse
import json
import sys
import time
from typing import Any

from rdc.discover import find_renderdoc


MESSAGE_DISCONNECTED = 1
MESSAGE_NOOP = 3
MESSAGE_NEW_CAPTURE = 4


def _drain_pending_messages(target: Any) -> None:
    """Consume asynchronous registration messages before queueing a capture."""
    for _ in range(20):
        message = target.ReceiveMessage(None)
        message_type = int(message.type)
        if message_type == MESSAGE_NOOP:
            return
        if message_type == MESSAGE_DISCONNECTED:
            raise RuntimeError("RenderDoc target disconnected during initial message drain")


def _connect(rd: Any, host: str, ident: int, timeout: float) -> Any:
    deadline = time.monotonic() + timeout
    last_error = "target is not listening"
    while time.monotonic() < deadline:
        target = None
        try:
            target = rd.CreateTargetControl(host, ident, "renderdoc-trace-capture", True)
            if target is not None and target.Connected():
                return target
            last_error = "CreateTargetControl returned a disconnected target"
        except Exception as exc:  # noqa: BLE001 - RenderDoc raises SWIG exceptions
            last_error = str(exc)
        if target is not None:
            target.Shutdown()
        time.sleep(0.05)
    raise RuntimeError(
        f"failed to connect to RenderDoc target {host}:{ident} within {timeout:g}s: "
        f"{last_error}"
    )


def queue_capture(
    *,
    host: str,
    ident: int,
    frame: int,
    count: int,
    connect_timeout: float,
    capture_timeout: float,
) -> dict[str, object]:
    rd = find_renderdoc()
    if rd is None:
        raise RuntimeError("RenderDoc Python module not found; run 'rdc setup-renderdoc'")

    target = _connect(rd, host, ident, connect_timeout)
    started = time.monotonic()
    try:
        _drain_pending_messages(target)
        target.QueueCapture(frame, count)
        deadline = time.monotonic() + capture_timeout
        while time.monotonic() < deadline:
            if not target.Connected():
                raise RuntimeError("RenderDoc target disconnected before capture completed")
            message = target.ReceiveMessage(None)
            message_type = int(message.type)
            if message_type == MESSAGE_NEW_CAPTURE and message.newCapture is not None:
                capture = message.newCapture
                return {
                    "queued": True,
                    "host": host,
                    "ident": ident,
                    "frame": frame,
                    "count": count,
                    "elapsedSeconds": round(time.monotonic() - started, 3),
                    "capture": {
                        "captureId": capture.captureId,
                        "path": capture.path,
                        "frame": capture.frameNumber,
                        "byteSize": capture.byteSize,
                        "api": capture.api,
                        "local": capture.local,
                    },
                }
            if message_type == MESSAGE_DISCONNECTED:
                raise RuntimeError("RenderDoc target disconnected before capture completed")
            time.sleep(0.01)
        raise RuntimeError(
            f"timed out after {capture_timeout:g}s waiting for frame {frame} capture"
        )
    finally:
        target.Shutdown()


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Wait for an Android RenderDoc TargetControl socket, queue an exact frame, "
            "and keep the connection alive until the RDC is complete."
        )
    )
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--ident", type=int, default=38920)
    parser.add_argument("--frame", type=int, required=True)
    parser.add_argument("--count", type=int, default=1)
    parser.add_argument("--connect-timeout", type=float, default=20.0)
    parser.add_argument("--capture-timeout", type=float, default=90.0)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    try:
        result = queue_capture(
            host=args.host,
            ident=args.ident,
            frame=args.frame,
            count=args.count,
            connect_timeout=args.connect_timeout,
            capture_timeout=args.capture_timeout,
        )
    except Exception as exc:  # noqa: BLE001 - convert tool failures to stable CLI output
        if args.json:
            print(json.dumps({"success": False, "error": str(exc)}))
        else:
            print(f"error: {exc}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps({"success": True, **result}))
    else:
        capture = result["capture"]
        assert isinstance(capture, dict)
        print(capture["path"])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

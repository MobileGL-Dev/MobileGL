#!/usr/bin/env python3
"""Capture an exact frame from MobileGL's Android trace replay activity."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Sequence

from rdc.discover import find_renderdoc


DEFAULT_PACKAGE = "top.mobilegl.plugin.trace"
DEFAULT_ACTIVITY = "top.mobilegl.plugin.trace.TraceReplayActivity"
DEFAULT_ACTION = "top.mobilegl.plugin.TRACE_REPLAY"
RENDERDOC_VULKAN_LAYER = "VK_LAYER_RENDERDOC_Capture"
RENDERDOC_GLES_LAYER = "libVkLayer_GLES_RenderDoc.so"
RENDERDOC_CMD_PACKAGE = "org.renderdoc.renderdoccmd.arm64"
REMOTE_GLES_LAYER = f"/data/local/debug/vulkan/{RENDERDOC_GLES_LAYER}"


class CommandRunner:
    def __init__(self, *, verbose: bool = True) -> None:
        self.verbose = verbose
        self.commands: list[list[str]] = []

    def run(
        self,
        command: Sequence[object],
        *,
        check: bool = True,
        timeout: float | None = None,
        capture_output: bool = True,
    ) -> subprocess.CompletedProcess[str]:
        argv = [str(part) for part in command]
        self.commands.append(argv)
        if self.verbose:
            print(f"+ {subprocess.list2cmdline(argv)}", file=sys.stderr, flush=True)
        completed = subprocess.run(
            argv,
            check=False,
            timeout=timeout,
            capture_output=capture_output,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        if self.verbose and completed.stdout and not capture_output:
            print(completed.stdout, end="", file=sys.stderr)
        if check and completed.returncode != 0:
            detail = completed.stderr.strip() or completed.stdout.strip() or "no output"
            raise RuntimeError(
                f"command failed with exit code {completed.returncode}: "
                f"{subprocess.list2cmdline(argv)}\n{detail}"
            )
        return completed


def _adb(runner: CommandRunner, serial: str, *args: object, **kwargs: Any) -> subprocess.CompletedProcess[str]:
    return runner.run(["adb", "-s", serial, *args], **kwargs)


def _connected_serials(runner: CommandRunner) -> list[str]:
    completed = runner.run(["adb", "devices"], timeout=10)
    serials: list[str] = []
    for line in completed.stdout.splitlines()[1:]:
        fields = line.split()
        if len(fields) >= 2 and fields[1] == "device":
            serials.append(fields[0])
    return serials


def _resolve_serial(runner: CommandRunner, requested: str | None) -> str:
    serials = _connected_serials(runner)
    if requested:
        if requested not in serials:
            raise RuntimeError(
                f"Android device {requested!r} is not connected; available: {serials or 'none'}"
            )
        return requested
    if len(serials) != 1:
        raise RuntimeError(
            "exactly one Android device must be connected when --serial is omitted; "
            f"found {serials or 'none'}"
        )
    return serials[0]


def _setup_renderdoc(runner: CommandRunner, serial: str, connected: list[str]) -> None:
    command = ["rdc", "android", "setup", "--serial", serial, "--json"]
    completed = runner.run(command, check=False, timeout=90)
    if completed.returncode == 0:
        return
    # rdc-cli 1.41 can enumerate a raw serial but compare it against adb://SERIAL.
    # Falling back is safe only when the requested device is the sole connected device.
    if connected == [serial] and "not found" in (completed.stderr + completed.stdout).lower():
        runner.run(["rdc", "android", "setup", "--json"], timeout=90)
        return
    detail = completed.stderr.strip() or completed.stdout.strip()
    raise RuntimeError(f"rdc android setup failed: {detail}")


def _get_property(runner: CommandRunner, serial: str, key: str) -> str:
    return _adb(runner, serial, "shell", "getprop", key, timeout=10).stdout.strip()


def _set_property(runner: CommandRunner, serial: str, key: str, value: str) -> None:
    _adb(runner, serial, "shell", "setprop", key, value, timeout=10)


def _get_setting(runner: CommandRunner, serial: str, key: str) -> str | None:
    value = _adb(
        runner, serial, "shell", "settings", "get", "global", key, timeout=10
    ).stdout.strip()
    return None if value in {"", "null"} else value


def _set_setting(runner: CommandRunner, serial: str, key: str, value: str | None) -> None:
    if value is None:
        _adb(runner, serial, "shell", "settings", "delete", "global", key, timeout=10)
    else:
        _adb(runner, serial, "shell", "settings", "put", "global", key, value, timeout=10)


def _ensure_debug_settings(
    runner: CommandRunner,
    serial: str,
    package: str,
    changed: dict[str, str | None],
) -> None:
    expected = {
        "gpu_debug_app": package,
        "enable_gpu_debug_layers": "1",
        "gpu_debug_layers": RENDERDOC_VULKAN_LAYER,
    }
    original = {key: _get_setting(runner, serial, key) for key in expected}
    for key, value in expected.items():
        if original[key] != value:
            changed[key] = original[key]
            _set_setting(runner, serial, key, value)
            actual = _get_setting(runner, serial, key)
            if actual != value:
                raise RuntimeError(
                    f"failed to set Android global setting {key}={value!r}; got {actual!r}. "
                    "Enable the developer option that permits USB debugging to write secure "
                    "settings, or configure RenderDoc's GPU debug layer from the device UI."
                )


def _encode_capture_options(soft_memory_limit_mib: int) -> str:
    rd = find_renderdoc()
    if rd is None:
        raise RuntimeError("RenderDoc Python module not found; run 'rdc setup-renderdoc'")
    options = rd.GetDefaultCaptureOptions()
    options.softMemoryLimit = soft_memory_limit_mib
    return options.EncodeAsString()


def _map_target_call_to_swap(
    runner: CommandRunner, trace: Path, target_call: int, apitrace: str
) -> tuple[int, int, str]:
    completed = runner.run(
        [apitrace, "dump", "--grep=eglSwapBuffers", str(trace)], timeout=120
    )
    swaps: list[int] = []
    for line in completed.stdout.splitlines():
        match = re.match(r"^(\d+)\s+eglSwapBuffers\b", line)
        if match:
            swaps.append(int(match.group(1)))
    if not swaps:
        raise RuntimeError(f"no eglSwapBuffers calls found in {trace}")
    for index, call in enumerate(swaps):
        if call == target_call:
            return index, call, "exact-swap-call"
        if call > target_call:
            return index, call, "first-swap-after-call"
    raise RuntimeError(
        f"target call {target_call} is after the final eglSwapBuffers call {swaps[-1]}"
    )


def _capture_frame_for_backend(backend: str, target_swap: int) -> int:
    if backend == "DirectVulkan":
        if target_swap == 0:
            raise RuntimeError("DirectVulkan cannot capture target swap 0 with the -1 frame rule")
        return target_swap - 1
    return target_swap


def _forward_map(runner: CommandRunner, serial: str) -> dict[int, str]:
    completed = _adb(runner, serial, "forward", "--list", timeout=10)
    forwards: dict[int, str] = {}
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) < 3 or fields[0] != serial or not fields[1].startswith("tcp:"):
            continue
        try:
            forwards[int(fields[1].removeprefix("tcp:"))] = fields[2]
        except ValueError:
            continue
    return forwards


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def _read_remote_json(
    runner: CommandRunner, serial: str, package: str, path: str
) -> dict[str, object] | None:
    completed = _adb(
        runner,
        serial,
        "exec-out",
        "run-as",
        package,
        "cat",
        path,
        check=False,
        timeout=15,
    )
    if completed.returncode != 0 or not completed.stdout.strip():
        return None
    return json.loads(completed.stdout)


def _write_manifest(path: Path, data: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Replay an apitrace fixture with MobileGL on Android and capture an exact "
            "DirectVulkan or DirectGLES frame with RenderDoc."
        )
    )
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--target-call", type=int, required=True)
    parser.add_argument("--target-swap", type=int, help="zero-based target swap; infer when omitted")
    parser.add_argument("--capture-frame", type=int, help="override backend-specific frame mapping")
    parser.add_argument("--backend", choices=("DirectVulkan", "DirectGLES"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--serial")
    parser.add_argument("--package", default=DEFAULT_PACKAGE)
    parser.add_argument("--activity", default=DEFAULT_ACTIVITY)
    parser.add_argument("--action", default=DEFAULT_ACTION)
    parser.add_argument("--width", type=int, default=854)
    parser.add_argument("--height", type=int, default=480)
    parser.add_argument("--ssim-threshold", type=float, default=0.99)
    parser.add_argument("--apitrace", default="apitrace")
    parser.add_argument("--local-port", type=int, default=38920)
    parser.add_argument("--target-ident", type=int, default=38920)
    parser.add_argument("--connect-timeout", type=float, default=20.0)
    parser.add_argument("--capture-timeout", type=float, default=90.0)
    parser.add_argument("--soft-memory-limit", type=int, default=256, metavar="MIB")
    parser.add_argument("--no-setup-renderdoc", action="store_true")
    parser.add_argument("--keep-device-files", action="store_true")
    parser.add_argument("--keep-device-state", action="store_true")
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    runner = CommandRunner(verbose=True)
    trace = args.trace.resolve()
    golden = args.golden.resolve()
    output = args.output.resolve()
    manifest_path = output.with_suffix(output.suffix + ".json")
    for path, label in ((trace, "trace"), (golden, "golden")):
        if not path.is_file():
            raise RuntimeError(f"{label} file does not exist: {path}")
    if output.exists() and not args.overwrite:
        raise RuntimeError(f"output exists; pass --overwrite to replace it: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)

    serial = _resolve_serial(runner, args.serial)
    connected = _connected_serials(runner)
    initial_forwards = _forward_map(runner, serial)

    if args.target_swap is None:
        target_swap, target_swap_call, mapping = _map_target_call_to_swap(
            runner, trace, args.target_call, args.apitrace
        )
    else:
        target_swap = args.target_swap
        target_swap_call = args.target_call
        mapping = "explicit"
    capture_frame = (
        args.capture_frame
        if args.capture_frame is not None
        else _capture_frame_for_backend(args.backend, target_swap)
    )

    token = f"{int(time.time())}-{os.getpid()}"
    remote_trace = f"/data/local/tmp/renderdoc-retrace-{token}.trace"
    remote_golden = f"/data/local/tmp/renderdoc-retrace-{token}-golden.png"
    replay_root = (
        f"/data/user/0/{args.package}/files/trace-replay/renderdoc-capture-{token}"
    )
    replay_output = f"{replay_root}/output"
    remote_result = f"{replay_output}/result.json"
    queue_script = Path(__file__).with_name("queue_android_frame.py")

    original_capopts = _get_property(runner, serial, "debug.rdoc.RENDERDOC_CAPOPTS")
    original_gles_layers = _get_property(runner, serial, "debug.gles.layers")
    original_settings: dict[str, str | None] = {}
    original_forward = initial_forwards.get(args.local_port)
    touched_forward_ports = {args.local_port}
    private_gles_layer = f"/data/user/0/{args.package}/{RENDERDOC_GLES_LAYER}"
    private_layer_existed = (
        _adb(
            runner,
            serial,
            "shell",
            "run-as",
            args.package,
            "test",
            "-f",
            private_gles_layer,
            check=False,
            timeout=10,
        ).returncode
        == 0
    )
    capture_process: subprocess.Popen[str] | None = None
    result: dict[str, object] = {}
    succeeded = False
    stop_renderdoc_server = False

    try:
        if not args.no_setup_renderdoc:
            _setup_renderdoc(runner, serial, connected)
            setup_forwards = _forward_map(runner, serial)
            stop_renderdoc_server = any(
                remote == "localabstract:renderdoc_39920"
                and initial_forwards.get(port) != remote
                for port, remote in setup_forwards.items()
            )
            touched_forward_ports.update(
                port
                for port in set(initial_forwards) | set(setup_forwards)
                if initial_forwards.get(port) != setup_forwards.get(port)
            )
            original_forward = setup_forwards.get(args.local_port)

        _adb(runner, serial, "shell", "am", "force-stop", args.package, timeout=15)
        _ensure_debug_settings(runner, serial, args.package, original_settings)
        if args.backend == "DirectVulkan":
            capopts = _encode_capture_options(args.soft_memory_limit)
            _set_property(runner, serial, "debug.rdoc.RENDERDOC_CAPOPTS", capopts)
            _set_property(runner, serial, "debug.gles.layers", ":")
        else:
            _adb(
                runner,
                serial,
                "shell",
                "run-as",
                args.package,
                "cp",
                REMOTE_GLES_LAYER,
                private_gles_layer,
                timeout=30,
            )
            _adb(
                runner,
                serial,
                "shell",
                "run-as",
                args.package,
                "chmod",
                "755",
                private_gles_layer,
                timeout=10,
            )
            _set_property(runner, serial, "debug.gles.layers", RENDERDOC_GLES_LAYER)

        _adb(runner, serial, "push", trace, remote_trace, timeout=120)
        _adb(runner, serial, "push", golden, remote_golden, timeout=60)
        _adb(
            runner,
            serial,
            "shell",
            "run-as",
            args.package,
            "rm",
            "-rf",
            replay_root,
            timeout=20,
        )
        _adb(
            runner,
            serial,
            "shell",
            "run-as",
            args.package,
            "mkdir",
            "-p",
            replay_output,
            timeout=20,
        )

        wanted_forward = f"localabstract:renderdoc_{args.target_ident}"
        if original_forward != wanted_forward:
            if original_forward is not None:
                _adb(
                    runner,
                    serial,
                    "forward",
                    "--remove",
                    f"tcp:{args.local_port}",
                    timeout=10,
                )
            _adb(
                runner,
                serial,
                "forward",
                f"tcp:{args.local_port}",
                wanted_forward,
                timeout=10,
            )

        queue_command = [
            sys.executable,
            str(queue_script),
            "--host",
            "127.0.0.1",
            "--ident",
            str(args.local_port),
            "--frame",
            str(capture_frame),
            "--connect-timeout",
            str(args.connect_timeout),
            "--capture-timeout",
            str(args.capture_timeout),
            "--json",
        ]
        runner.commands.append(queue_command)
        print(f"+ {subprocess.list2cmdline(queue_command)}", file=sys.stderr, flush=True)
        capture_process = subprocess.Popen(
            queue_command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        time.sleep(0.5)

        component = f"{args.package}/{args.activity}"
        launch_command = [
            "adb",
            "-s",
            serial,
            "shell",
            "am",
            "start",
            "-W",
            "-a",
            args.action,
            "-n",
            component,
            "--es",
            "trace_path",
            remote_trace,
            "--es",
            "golden_path",
            remote_golden,
            "--es",
            "output_dir",
            replay_output,
            "--es",
            "diff_path",
            f"{replay_output}/diff.png",
            "--es",
            "backend",
            args.backend,
            "--el",
            "target_call",
            str(args.target_call),
            "--ei",
            "width",
            str(args.width),
            "--ei",
            "height",
            str(args.height),
            "--es",
            "ssim_threshold",
            str(args.ssim_threshold),
            "--ei",
            "crop_x",
            "0",
            "--ei",
            "crop_y",
            "0",
            "--ei",
            "crop_width",
            "0",
            "--ei",
            "crop_height",
            "0",
            "--ez",
            "coherent_as_flush",
            "true",
        ]
        runner.run(launch_command, timeout=30)

        stdout, stderr = capture_process.communicate(
            timeout=args.connect_timeout + args.capture_timeout + 15
        )
        capture_returncode = capture_process.returncode
        capture_process = None
        if stderr.strip():
            print(stderr, end="", file=sys.stderr)
        if not stdout.strip():
            raise RuntimeError("queue helper returned no JSON output")
        capture_result = json.loads(stdout)
        if capture_result.get("success") is not True:
            raise RuntimeError(f"RenderDoc capture failed: {capture_result.get('error')}")
        if capture_returncode != 0:
            raise RuntimeError(f"queue helper exited with {capture_returncode}")
        capture = capture_result["capture"]
        if not isinstance(capture, dict) or not capture.get("path"):
            raise RuntimeError(f"queue helper returned invalid capture metadata: {capture!r}")

        _adb(runner, serial, "pull", capture["path"], output, timeout=180)
        if not output.is_file() or output.stat().st_size <= 0:
            raise RuntimeError(f"adb pull did not produce a non-empty RDC: {output}")
        retrace_result = _read_remote_json(
            runner, serial, args.package, remote_result
        )
        result = {
            "success": True,
            "deviceSerial": serial,
            "backend": args.backend,
            "targetCall": args.target_call,
            "targetSwap": target_swap,
            "targetSwapCall": target_swap_call,
            "targetSwapMapping": mapping,
            "captureFrame": capture_frame,
            "capture": capture,
            "retraceResult": retrace_result,
            "output": str(output),
            "byteSize": output.stat().st_size,
            "sha256": _sha256(output),
        }
        succeeded = True
    except Exception as exc:  # noqa: BLE001 - stable CLI error boundary
        result = {
            "success": False,
            "deviceSerial": serial,
            "backend": args.backend,
            "targetCall": args.target_call,
            "targetSwap": target_swap,
            "captureFrame": capture_frame,
            "output": str(output),
            "error": str(exc),
        }
    finally:
        if capture_process is not None and capture_process.poll() is None:
            capture_process.kill()
            capture_process.wait(timeout=10)
        if not args.keep_device_state:
            try:
                _set_property(
                    runner,
                    serial,
                    "debug.rdoc.RENDERDOC_CAPOPTS",
                    original_capopts,
                )
                _set_property(runner, serial, "debug.gles.layers", original_gles_layers or ":")
                for key, value in original_settings.items():
                    _set_setting(runner, serial, key, value)
                if args.backend == "DirectGLES" and not private_layer_existed:
                    _adb(
                        runner,
                        serial,
                        "shell",
                        "run-as",
                        args.package,
                        "rm",
                        "-f",
                        private_gles_layer,
                        check=False,
                        timeout=10,
                    )
                _adb(
                    runner,
                    serial,
                    "shell",
                    "am",
                    "force-stop",
                    args.package,
                    check=False,
                    timeout=15,
                )
            except Exception as exc:  # noqa: BLE001
                print(f"warning: device state cleanup failed: {exc}", file=sys.stderr)
            try:
                current_forwards = _forward_map(runner, serial)
                for port in sorted(touched_forward_ports):
                    current = current_forwards.get(port)
                    original = initial_forwards.get(port)
                    if current == original:
                        continue
                    if current is not None:
                        _adb(
                            runner,
                            serial,
                            "forward",
                            "--remove",
                            f"tcp:{port}",
                            check=False,
                            timeout=10,
                        )
                    if original is not None:
                        _adb(
                            runner,
                            serial,
                            "forward",
                            f"tcp:{port}",
                            original,
                            timeout=10,
                        )
                if stop_renderdoc_server:
                    _adb(
                        runner,
                        serial,
                        "shell",
                        "am",
                        "force-stop",
                        RENDERDOC_CMD_PACKAGE,
                        check=False,
                        timeout=15,
                    )
            except Exception as exc:  # noqa: BLE001
                print(f"warning: adb forward cleanup failed: {exc}", file=sys.stderr)

        if not args.keep_device_files:
            try:
                _adb(
                    runner,
                    serial,
                    "shell",
                    "rm",
                    "-f",
                    remote_trace,
                    remote_golden,
                    check=False,
                    timeout=20,
                )
                _adb(
                    runner,
                    serial,
                    "shell",
                    "run-as",
                    args.package,
                    "rm",
                    "-rf",
                    replay_root,
                    check=False,
                    timeout=20,
                )
            except Exception as exc:  # noqa: BLE001
                print(f"warning: device file cleanup failed: {exc}", file=sys.stderr)

        result["commands"] = runner.commands
        result["commandLines"] = [
            subprocess.list2cmdline(command) for command in runner.commands
        ]
        try:
            _write_manifest(manifest_path, result)
        except Exception as exc:  # noqa: BLE001
            print(f"warning: failed to write manifest: {exc}", file=sys.stderr)

    if args.json:
        print(json.dumps(result, ensure_ascii=False))
    elif succeeded:
        print(output)
        print(f"manifest: {manifest_path}", file=sys.stderr)
        print(f"sha256: {result['sha256']}", file=sys.stderr)
    else:
        print(f"error: {result['error']}", file=sys.stderr)
    return 0 if succeeded else 1


if __name__ == "__main__":
    raise SystemExit(main())

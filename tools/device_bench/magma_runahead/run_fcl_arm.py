#!/usr/bin/env python3
"""One FCL arm on Redmi 2f7cbe2e; root coordinates backup/reboot beforehand.

No logcat clear, clock pin, APK install, or global-setting change. The app is left
in its final state for inspection. Only mg_env.txt, mg_transport.txt and the new
library log are written on the device. All subprocess calls use argv lists.
The 180-second deadline includes the required 60-second stable in-world window.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ADB = Path(os.environ.get("ADB_BIN") or shutil.which("adb") or "adb")
SERIAL = "2f7cbe2e"
PACKAGE = "com.tungsten.fcl.mgdebug.debug"
ACTIVITY = PACKAGE + "/com.tungsten.fcl.activity.SplashActivity"
LATEST = "/sdcard/FCL/.minecraft/versions/26.3-rc-3/logs/latest.log"
POLL_SECONDS, DEADLINE_SECONDS, STABLE_SECONDS = 5, 180, 60
FATAL = re.compile(r"Fatal\{|Fatal signal|FATAL EXCEPTION|SIGSEGV|SIGABRT|Abort message", re.I)
WORLD = re.compile(r"joined the game|logged in with entity id|Loaded \d+ advancements", re.I)
BACKEND_NAMES = {"DirectGLES": "Direct (OpenGL ES)", "DirectVulkan": "Direct (Vulkan)"}


def redact(text: str) -> str:
    # Keep complete diagnostic logs locally without retaining game credential arguments.
    text = re.sub(r'(?i)(--(?:accessToken|clientToken|refreshToken|idToken)\s+)("[^"]*"|\S+)', r'\1<redacted>', text)
    text = re.sub(r'(?i)((?:accessToken|clientToken|refreshToken|idToken)["\s]*[:=]\s*["\s]*)([^"\s,}]+)', r'\1<redacted>', text)
    return re.sub(r'(?i)(Authorization\s*[:=]\s*Bearer\s+)\S+', r'\1<redacted>', text)


def decode(data: bytes | None) -> str:
    return (data or b"").decode("utf-8", errors="replace").replace("\r\n", "\n")


class Arm:
    def __init__(self, args: argparse.Namespace):
        self.args, self.out = args, args.out.resolve()
        self.out.mkdir(parents=True, exist_ok=False)
        tag = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
        self.remote_log = f"/sdcard/FCL/p5f-fcl-{tag}-{args.backend}-{args.transport}.log"
        self.start = time.monotonic()
        self.log_since = ""
        self.library = self.latest = self.logcat = ""
        self.known_pids: set[str] = set()
        self.summary = {
            "backend": args.backend, "transport": args.transport, "run_ahead": args.run_ahead,
            "serial": SERIAL, "package": PACKAGE, "game_version": "26.3-rc-3",
            "world": "test", "remote_library_log": self.remote_log,
            "deadline_seconds": DEADLINE_SECONDS, "stable_seconds_required": STABLE_SECONDS,
            "status": "preparing", "screenshot_review_required": True,
        }

    def save(self, name: str, text: str) -> None:
        (self.out / name).write_text(redact(text), encoding="utf-8")

    def event(self, kind: str, **fields) -> None:
        record = {"utc": datetime.now(timezone.utc).isoformat(), "elapsed": round(time.monotonic() - self.start, 3), "kind": kind, **fields}
        with (self.out / "lifecycle.jsonl").open("a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")

    def adb(self, *args: str, required: bool = False, timeout: int = 15) -> subprocess.CompletedProcess:
        argv = [str(ADB), "-s", SERIAL, *args]
        try:
            result = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            result = subprocess.CompletedProcess(argv, 124, exc.stdout or b"", exc.stderr or b"")
        self.event("adb", argv=argv[3:], returncode=result.returncode)
        if required and result.returncode:
            self.save("last-adb-error.txt", decode(result.stdout) + decode(result.stderr))
            raise RuntimeError(f"adb command failed (rc={result.returncode}); see last-adb-error.txt")
        return result

    def remote_text(self, path: str) -> str | None:
        result = self.adb("exec-out", "cat", path)
        return decode(result.stdout) if result.returncode == 0 else None

    def pids(self) -> set[str]:
        result = self.adb("shell", "pidof", PACKAGE)
        found = set(re.findall(r"\b\d+\b", decode(result.stdout))) if result.returncode == 0 else set()
        self.known_pids.update(found)
        return found

    def screenshot(self, name: str) -> None:
        result = self.adb("exec-out", "screencap", "-p")
        if result.returncode == 0 and result.stdout.startswith(b"\x89PNG\r\n\x1a\n"):
            (self.out / name).write_bytes(result.stdout)
        else:
            self.save(name + ".error.txt", decode(result.stderr) or "No valid PNG returned")

    def collect(self, final: bool = False) -> None:
        for path, attr, filename in ((self.remote_log, "library", "library.log"), (LATEST, "latest", "latest.log")):
            data = self.remote_text(path)
            if data is not None:
                setattr(self, attr, data)
                self.save(filename, data)
        if self.log_since:
            result = self.adb("logcat", "-d", "-v", "epoch", "-T", self.log_since,
                              "-b", "main", "-b", "system", "-b", "crash",
                              "MobileGL:V", "FCLFPS:I", "AndroidRuntime:E", "libc:E", "DEBUG:E", "ActivityManager:I", "*:S")
            if result.returncode == 0:
                self.logcat = decode(result.stdout)
                self.save("logcat-window.txt", self.logcat)
        if final:
            self.screenshot("final.png")
            result = self.adb("logcat", "-d", "-v", "epoch", "-T", self.log_since or "1", "-b", "crash")
            self.save("crash-window.txt", decode(result.stdout) + decode(result.stderr))

    def stats(self) -> list[dict]:
        result = []
        for line in self.library.splitlines():
            if "MGPipe stats:" not in line or not re.search(r"\bframes=\d+", line):
                continue
            values = dict(re.findall(r"\b(frames|window|draws|draws/f|rsp|vbs)=([0-9.]+)", line))
            if values:
                result.append({key: float(value) if "." in value else int(value) for key, value in values.items()})
        return result

    def runtime(self, stats: list[dict]) -> tuple[bool, bool, str | None]:
        names = re.findall(r"Backend Name:\s*([^\r\n]+)", self.library)
        expected = BACKEND_NAMES[self.args.backend]
        if names and any(name.strip() != expected for name in names):
            return False, False, "backend_mismatch"
        backend_ok = bool(names)
        ipc = re.findall(r"Config: IPC [^\r\n]+", self.library)
        if self.args.transport == "inproc":
            if ipc and not all("strict=1" in line and "role-split-state=1" in line for line in ipc):
                return backend_ok, False, "ipc_configuration_mismatch"
            expected_knob = "run-ahead=" + str(self.args.run_ahead)
            capability_ok = ("run-ahead ARMED" in self.library) if self.args.run_ahead else (
                "run-ahead ARMED" not in self.library and any(expected_knob in line for line in ipc))
            ipc_ok = bool(ipc) and capability_ok and any(row.get("vbs", 0) > 0 for row in stats)
        else:
            if ipc or any(row.get("vbs", 0) != 0 for row in stats):
                return backend_ok, False, "monolith_control_used_transport"
            ipc_ok = bool(stats) and all("vbs" in row for row in stats)
        if any(row.get("rsp", 0) != 0 for row in stats):
            return backend_ok, ipc_ok, "nonzero_residual_pull"
        return backend_ok, ipc_ok, None

    def run(self) -> int:
        status, reason = "FAILED", "not_started"
        world_at = None
        world_frame = 0
        stable_pids: set[str] = set()
        seen_process, missing_polls = False, 0
        try:
            if not ADB.is_file():
                raise RuntimeError("Expected Windows adb executable is missing")
            self.adb("get-state", required=True)
            self.save("boot-id.txt", decode(self.adb("shell", "cat", "/proc/sys/kernel/random/boot_id", required=True).stdout))
            before = self.remote_text(LATEST) or ""
            before_hash = hashlib.sha256(before.encode()).hexdigest()
            self.save("latest.before.log", before)
            self.event("pid_before", pids=sorted(self.pids()))
            env = {
                "MOBILEGL_BACKEND_TYPE": self.args.backend,
                "MOBILEGL_TRANSPORT": self.args.transport,
                "MOBILEGL_IPC_ROLE_SPLIT_STATE": "1" if self.args.transport == "inproc" else "0",
                "MOBILEGL_IPC_STRICT_ERRORS": "1", "MOBILEGL_IPC_RUN_AHEAD": str(self.args.run_ahead),
                "MOBILEGL_IPC_STAGE_MB": "256", "MOBILEGL_PIPE_STATS": "1",
                "MOBILEGL_PIPE_STATS_PERIOD": "120", "MOBILEGL_LOG_FILE_PATH": self.remote_log,
            }
            self.save("mg_env.txt", "".join(f"{key}={value}\n" for key, value in env.items()))
            self.save("mg_transport.txt", self.args.transport + "\n")
            self.adb("shell", "am", "force-stop", PACKAGE, required=True)
            self.adb("push", str(self.out / "mg_env.txt"), "/sdcard/FCL/mg_env.txt", required=True)
            self.adb("push", str(self.out / "mg_transport.txt"), "/sdcard/FCL/mg_transport.txt", required=True)
            self.adb("shell", "input", "keyevent", "224", required=True)
            self.adb("shell", "input", "keyevent", "82", required=True)
            self.log_since = decode(self.adb("shell", "date '+%m-%d %H:%M:%S.000'", required=True).stdout).strip()
            if not re.fullmatch(r"\d\d-\d\d \d\d:\d\d:\d\d\.000", self.log_since):
                raise RuntimeError("Could not obtain a device timestamp for the logcat window")
            self.summary["logcat_since_device_time"] = self.log_since
            self.start = time.monotonic()
            launched = self.adb("shell", "am", "start", "-n", ACTIVITY, required=True, timeout=30)
            self.save("launch.txt", decode(launched.stdout) + decode(launched.stderr))
            self.event("launched")
            while time.monotonic() - self.start < DEADLINE_SECONDS:
                current_pids = self.pids()
                self.collect()
                rows = self.stats()
                fresh_latest = bool(self.latest) and hashlib.sha256(self.latest.encode()).hexdigest() != before_hash
                target_logcat = "\n".join(line for line in self.logcat.splitlines()
                    if PACKAGE in line or any(re.search(r"\b" + pid + r"\b", line) for pid in self.known_pids))
                fatal_match = FATAL.search(self.library + (self.latest if fresh_latest else "") + target_logcat)
                if fatal_match:
                    reason = "fatal_or_crash_marker"
                    break
                backend_ok, ipc_ok, problem = self.runtime(rows)
                if problem:
                    reason = problem
                    break
                if current_pids:
                    seen_process, missing_polls = True, 0
                elif seen_process:
                    missing_polls += 1
                if missing_polls >= 2:
                    reason = "app_process_exited"
                    break
                if world_at is not None and current_pids and current_pids != stable_pids:
                    reason = "app_process_restarted_during_stable_window"
                    break
                world_log = bool(fresh_latest and WORLD.search(self.latest))
                drawing = len(rows) >= 3 and all(row.get("window") == 120 and row.get("draws/f", 0) >= 400 for row in rows[-3:])
                # Magma does not implement the draws/f counter: use the fresh game log
                # and actual frame progress, then require root to inspect the screenshots.
                progress = bool(rows and rows[-1].get("frames", 0) > 0)
                ready = bool(current_pids and world_log and backend_ok and ipc_ok and
                             (drawing if self.args.backend == "DirectGLES" else progress))
                if world_at is None and ready:
                    world_at, world_frame, stable_pids = time.monotonic(), rows[-1]["frames"], current_pids
                    self.summary["time_to_in_world_seconds"] = round(world_at - self.start, 3)
                    self.event("in_world", frame=world_frame, pids=sorted(current_pids), source="fresh_latest_log_and_runtime")
                    self.screenshot("in-world.png")
                stable = 0 if world_at is None else time.monotonic() - world_at
                self.event("poll", pids=sorted(current_pids), backend_ok=backend_ok, ipc_ok=ipc_ok,
                           fresh_latest=fresh_latest, world_log=world_log, stable_seconds=round(stable, 2),
                           stats=rows[-1] if rows else None)
                print(f"{self.args.backend}/{self.args.transport}: elapsed={int(time.monotonic()-self.start)}s "
                      f"pid={'alive' if current_pids else 'absent'} world={'yes' if world_at else 'waiting'} stable={int(stable)}s", flush=True)
                if stable >= STABLE_SECONDS:
                    if not current_pids or not rows or rows[-1].get("frames", 0) <= world_frame:
                        reason = "no_frame_progress_during_stable_window"
                        break
                    status, reason = "PASS_PENDING_SCREENSHOT_REVIEW", "stable_window_completed"
                    self.summary["stable_seconds"] = round(stable, 3)
                    self.summary["stable_frame_delta"] = rows[-1]["frames"] - world_frame
                    self.summary["fps_approx_from_polled_stats"] = round((rows[-1]["frames"] - world_frame) / stable, 3)
                    break
                time.sleep(min(POLL_SECONDS, max(0, DEADLINE_SECONDS - (time.monotonic() - self.start))))
            else:
                reason = "timeout_before_in_world" if world_at is None else "timeout_before_60s_stable"
        except (RuntimeError, OSError, KeyboardInterrupt) as exc:
            reason = "interrupted" if isinstance(exc, KeyboardInterrupt) else "runner_or_adb_error"
            self.save("runner-error.txt", str(exc))
        finally:
            # Best effort capture cannot replace the original failure or lose prior polls.
            try:
                self.collect(final=True)
                final_pids = sorted(self.pids())
                self.event("final_pid", pids=final_pids)
                self.summary["final_pids"] = final_pids
                if status == "PASS_PENDING_SCREENSHOT_REVIEW":
                    if not final_pids or FATAL.search(self.library):
                        status, reason = "FAILED", "app_failed_during_final_capture"
                    elif not (self.out / "final.png").is_file():
                        status, reason = "FAILED", "final_screenshot_unavailable"
            except (OSError, RuntimeError) as exc:
                self.save("collection-error.txt", str(exc))
            self.summary.update(status=status, reason=reason, elapsed_seconds=round(time.monotonic() - self.start, 3),
                                stats=self.stats(), app_left_in_final_state=True)
            (self.out / "summary.json").write_text(json.dumps(self.summary, ensure_ascii=False, indent=2), encoding="utf-8")
            self.event("finished", status=status, reason=reason)
        print(f"{status}: {reason}; evidence={self.out}", flush=True)
        return 0 if status == "PASS_PENDING_SCREENSHOT_REVIEW" else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", choices=BACKEND_NAMES, required=True)
    parser.add_argument("--transport", choices=("inproc", "monolith"), required=True)
    parser.add_argument("--run-ahead", type=int, choices=(0, 1), default=1)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if args.out.exists():
        parser.error("--out must be a new directory; existing evidence is never overwritten")
    return Arm(args).run()


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Run the Android replay ownership regression without an emulator or Gradle."""

import pathlib
import subprocess
import tempfile
import xml.etree.ElementTree as ET


def main():
    root = pathlib.Path(__file__).resolve().parents[2]
    trace = root / "android-plugin/app/src/trace"
    android = "{http://schemas.android.com/apk/res/android}"
    manifest = ET.parse(trace / "AndroidManifest.xml")
    activity = next(a for a in manifest.findall("application/activity")
                    if a.get(android + "name") == ".trace.TraceReplayActivity")
    config_value = activity.get(android + "configChanges", "0")
    if config_value == "@integer/trace_replay_config_changes":
        resources = ET.parse(trace / "res/values/config.xml")
        config_value = resources.find("integer[@name='trace_replay_config_changes']").text
    changes = int(config_value, 0)
    # The failing emulator added resource overlays after replay had already started.
    # orientation/screenSize alone does not handle CONFIG_ASSETS_PATHS.
    if changes & 0x80000000 == 0:
        raise RuntimeError("TraceReplayActivity must handle CONFIG_ASSETS_PATHS")
    if changes & 0xF80 != 0xF80:
        raise RuntimeError("TraceReplayActivity must retain the fixed-size render surface on display changes")
    # P12 (on-screen server window), D7: the display Activity. Each attribute below is load-bearing
    # and none of them is visible to a build: its own process (the in-process server's environment,
    # backend pin and EGL teardown stay out of every other process of the package, and the service
    # finds it by this name to enforce one server at a time), software window rendering (HWUI must
    # not share the EGL display Espryt terminates), and the same configChanges as the replay
    # Activity (a rotation or an overlay must not destroy the window a session renders into).
    display = next((a for a in manifest.findall("application/activity")
                    if a.get(android + "name") == ".MobileGLDisplayActivity"), None)
    if display is None:
        raise RuntimeError("the trace manifest has no MobileGLDisplayActivity (P12 D7)")
    for attribute, expected in (("process", ":mglwin"), ("hardwareAccelerated", "false"),
                                ("exported", "true"),
                                ("configChanges", activity.get(android + "configChanges"))):
        if display.get(android + attribute) != expected:
            raise RuntimeError(f"MobileGLDisplayActivity android:{attribute} must be {expected!r}, "
                               f"not {display.get(android + attribute)!r}")
    # Every trace-flavour Java class that has NO android.* import belongs here, because javac
    # plus `java` is the only way any of them is exercised outside an APK on a device. P7 added
    # SpawnServerPath: a hand-rolled K=V;K=V parse whose failure mode on device is "the knob had
    # no effect", which is why a defect in it survived from P6. P12 added ServerEnvironment: the
    # server role's environment both servers build (the service's exec environment, the display
    # Activity's own process) and the display's aspect-fit.
    java_units = [
        ("top.mobilegl.plugin.trace", "TraceReplaySession", "TraceReplaySessionTest"),
        ("top.mobilegl.plugin.trace", "SpawnServerPath", "SpawnServerPathTest"),
        ("top.mobilegl.plugin", "ServerEnvironment", "ServerEnvironmentTest"),
    ]
    with tempfile.TemporaryDirectory(prefix="mobilegl-replay-lifecycle-") as output:
        sources = []
        for package, unit, test in java_units:
            sources.append(str(trace / "java" / package.replace(".", "/") / f"{unit}.java"))
            sources.append(str(root / f"tools/trace_replay/{test}.java"))
        subprocess.run(["javac", "--release", "11", "-d", output, *sources], check=True)
        for package, _, test in java_units:
            subprocess.run(["java", "-cp", output, f"{package}.{test}"], check=True)
    print("Android replay manifest and lifecycle checks passed")


if __name__ == "__main__":
    main()

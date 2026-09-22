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
    # Every trace-flavour Java class that has NO android.* import belongs here, because javac
    # plus `java` is the only way any of them is exercised outside an APK on a device. P7 added
    # SpawnServerPath: a hand-rolled K=V;K=V parse whose failure mode on device is "the knob had
    # no effect", which is why a defect in it survived from P6.
    java_units = [
        ("TraceReplaySession", "TraceReplaySessionTest"),
        ("SpawnServerPath", "SpawnServerPathTest"),
    ]
    with tempfile.TemporaryDirectory(prefix="mobilegl-replay-lifecycle-") as output:
        sources = []
        for unit, test in java_units:
            sources.append(str(trace / f"java/top/mobilegl/plugin/trace/{unit}.java"))
            sources.append(str(root / f"tools/trace_replay/{test}.java"))
        subprocess.run(["javac", "--release", "11", "-d", output, *sources], check=True)
        for _, test in java_units:
            subprocess.run(["java", "-cp", output, f"top.mobilegl.plugin.trace.{test}"], check=True)
    print("Android replay manifest and lifecycle checks passed")


if __name__ == "__main__":
    main()

#!/usr/bin/env python
"""Copy the MobileGL dEQP platform port into a VK-GL-CTS checkout.

The port is version-controlled here, in the MobileGL repo, so it survives a
throwaway CTS clone. This drops it into the places VK-GL-CTS expects:

    framework/platform/mobilegl/     <- platform sources
    targets/mobilegl/mobilegl.cmake  <- target definition (-DDEQP_TARGET=mobilegl)

Usage:
    python sync_to_cts.py <path-to-VK-GL-CTS>
"""

import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CTS_TOOLS = os.path.dirname(HERE)

COPIES = [
    (os.path.join(CTS_TOOLS, "platform"), "framework/platform/mobilegl", None),
    (os.path.join(CTS_TOOLS, "targets"), "targets/mobilegl", ["mobilegl.cmake", "ndk-modern.cmake"]),
]


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    cts = sys.argv[1]
    if not os.path.isfile(os.path.join(cts, "CMakeLists.txt")):
        print(f"error: {cts} does not look like a VK-GL-CTS checkout", file=sys.stderr)
        return 1

    for src, reldst, only in COPIES:
        dst = os.path.join(cts, reldst)
        os.makedirs(dst, exist_ok=True)
        for name in sorted(os.listdir(src)):
            if only is not None and name not in only:
                continue
            s = os.path.join(src, name)
            if not os.path.isfile(s):
                continue
            shutil.copy2(s, os.path.join(dst, name))
            print(f"  {reldst}/{name}")

    print("\nsynced. configure with -DDEQP_TARGET=mobilegl")
    return 0


if __name__ == "__main__":
    sys.exit(main())

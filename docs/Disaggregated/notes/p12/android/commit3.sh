#!/usr/bin/env bash
set -euo pipefail
cd /home/swung/w7/p12-onscreen
git add MobileGL/MG_Remote/Server/ServerMain.cpp
git commit -q -m "[MG_Remote] (P12): the in-process pin line names the pin it records - MOBILEGL_BACKEND_TYPE when the environment set it, the first session's otherwise"
git add android-plugin/app/src/trace/java/top/mobilegl/plugin/MobileGLServerService.java
git commit -q -m "[android-plugin] (P12): MobileGLServerService logs its supervisor's closed output as the service stopping, not as a failure with a stack trace, when the display Activity stops it"
git add tools/trace_replay/README.md
git commit -q -m "[tools] (P12): trace_replay README - the two device servers, tcp_device_server.py --surface window, and the headless client's MOBILEGL_IPC_SURFACE=server --window-surface replay"
git log --oneline -3
git status --short | grep -v '^??' || true

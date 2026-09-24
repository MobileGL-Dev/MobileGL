#!/usr/bin/env bash
set -euo pipefail
cd /home/swung/w7/p12-onscreen
git add MobileGL/MG_Backend/DirectGLES/BackendObject_DirectGLES.cpp MobileGL/MG_Backend/DirectGLES/Managers.cpp \
        MobileGL/MG_Backend/DirectGLES/Managers.h MobileGL/MG_Test/SanityTest.cpp
git commit -q -m "[MG_Backend, MG_Test] (P12): the server backend's destruction drops every Espryt twin under a transport, so the in-process display server's next session no longer adopts the ended session's programs, VAOs and buffers (split build only; the pull build is unchanged, G1)"
git add MobileGL/MG_Remote/Server/DisplayServerJni.cpp
git commit -q -m "[MG_Remote] (P12): the display geometry up-call attaches the apply thread to the JVM under its own name, so the rest of the session is not logged as the up-call's thread"
git log --oneline -3

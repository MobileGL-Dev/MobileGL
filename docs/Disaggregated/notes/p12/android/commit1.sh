#!/usr/bin/env bash
set -euo pipefail
cd /home/swung/w7/p12-onscreen
git add MobileGL/MG_Remote/Server/DisplayServerJni.cpp CMakeLists.txt
git commit -q -m "[MG_Remote] (P12): the in-process display server's JNI - install the display with ANativeWindow hooks and a JVM-attached geometry up-call, serve/stop on a Java thread, attach from surfaceCreated/Changed and a blocking detach from surfaceDestroyed (Android split build only)"
git add android-plugin/app/src/trace/AndroidManifest.xml \
        android-plugin/app/src/trace/java/top/mobilegl/plugin/MobileGLDisplayActivity.java \
        android-plugin/app/src/trace/java/top/mobilegl/plugin/ServerEnvironment.java \
        android-plugin/app/src/trace/java/top/mobilegl/plugin/MobileGLServerService.java \
        tools/trace_replay/ServerEnvironmentTest.java tools/trace_replay/test_android_lifecycle.py
git commit -q -m "[android-plugin, tools] (P12): MobileGLDisplayActivity - the on-screen display server in :mglwin (software window, fullscreen SurfaceView, aspect-fit geometry, env applied before the library loads), one shared server environment with MobileGLServerService, and one server at a time (the Activity stops the service, the service kills :mglwin)"
git add tools/trace_replay/tcp_device_server.py scripts/ci/tcp_lane_tools_test.py
git commit -q -m "[tools, scripts] (P12): tcp_device_server.py --surface window|pbuffer (default pbuffer, unchanged) starts the on-screen display Activity on a lit screen, and --backend pins either server"
git log --oneline -4

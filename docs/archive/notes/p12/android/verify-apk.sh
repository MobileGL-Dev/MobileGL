#!/usr/bin/env bash
# verify-apk.sh <stamp>: the APK carries the display Activity, its JNI and the in-process entry.
O=/home/swung/w7/logs/p7w7/$1
NM=/home/swung/android-sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm
BT=$(ls -d /home/swung/android-sdk/build-tools/* | sort -V | tail -1)
echo "--- JNI + in-process entry exported by the APK's libMobileGL.so"
$NM -D --defined-only "$O/apk/lib/libMobileGL.so" | grep -E "MobileGLDisplayActivity|serve_inprocess|stop_inprocess"
echo "--- manifest"
"$BT/aapt2" dump xmltree --file AndroidManifest.xml "$O/apk/trace-$1.apk" | grep -B2 -A14 'MobileGLDisplayActivity' | grep -E "name|process|hardware|launchMode|taskAffinity|exported|configChanges" | head -12
echo "--- classes"
unzip -p "$O/apk/trace-$1.apk" classes.dex | strings | grep -E "^Ltop/mobilegl/plugin/(MobileGLDisplayActivity|ServerEnvironment|MobileGLServerService);" | sort -u
echo "--- stamp"
strings "$O/apk/lib/libMobileGL.so" | grep -F "$1" | head -2
cat "$O/build.env"

#!/usr/bin/env bash
# Quick compile checks before the full APK build: the trace flavour's new/changed Java against
# android-34, and DisplayServerJni.cpp for aarch64-linux-android26 with the split build's flags.
set -uo pipefail
cd /home/swung/w7/p12-onscreen || exit 1
export JAVA_HOME=/usr/lib/jvm/java-21-openjdk PATH=/usr/lib/jvm/java-21-openjdk/bin:$PATH
OUT=$(mktemp -d /tmp/p12-precheck-XXXX)
J=android-plugin/app/src/trace/java/top/mobilegl/plugin
echo "--- javac (android-34)"
javac --release 11 -Xlint:all -cp /home/swung/android-sdk/platforms/android-34/android.jar -d "$OUT/classes" \
    $J/MobileGLDisplayActivity.java $J/MobileGLServerService.java $J/ServerEnvironment.java 2>&1 | grep -v "^warning: \[options\]" | tail -30
echo "javac rc=${PIPESTATUS[0]}"

echo "--- clang++ -fsyntax-only DisplayServerJni.cpp (android26)"
NDK=/home/swung/android-sdk/ndk/27.3.13750724/toolchains/llvm/prebuilt/linux-x86_64
# The split build's command line for ServerDisplay.cpp gives the include dirs and defines.
CMD=$(python3 - <<'EOF'
import json,shlex
db=json.load(open('build-split/compile_commands.json'))
e=next(x for x in db if x['file'].endswith('MG_Remote/Server/ServerDisplay.cpp'))
args=shlex.split(e['command'])
keep=[a for a in args if a.startswith(('-I','-D','-isystem','-std=')) ]
# -isystem takes the next arg
out=[];i=0
while i<len(args):
    a=args[i]
    if a=='-isystem': out+= [a,args[i+1]]; i+=2; continue
    if a.startswith(('-I','-D','-std=')): out.append(a)
    i+=1
print(' '.join(shlex.quote(a) for a in out))
EOF
)
eval "$NDK/bin/clang++ --target=aarch64-linux-android26 -fsyntax-only -Wall -Wextra $CMD \
    MobileGL/MG_Remote/Server/DisplayServerJni.cpp" 2>&1 | grep -E "error|warning: .*DisplayServerJni" | head -30
echo "clang rc=${PIPESTATUS[0]}"
eval "$NDK/bin/clang++ --target=aarch64-linux-android26 -fsyntax-only $CMD \
    MobileGL/MG_Remote/Server/ServerDisplay.cpp" 2>&1 | grep -E "error" | head -10
echo "clang ServerDisplay.cpp (android arm) rc=${PIPESTATUS[0]}"
rm -rf "$OUT"

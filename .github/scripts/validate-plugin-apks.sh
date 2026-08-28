#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "Usage: $0 <aapt2> <plugin-apk> <trace-apk>" >&2
  exit 64
fi

aapt2=$1
plugin_apk=$2
trace_apk=$3

require() {
  local needle=$1
  local content=$2
  local description=$3
  if ! grep -Fq -- "$needle" <<<"$content"; then
    echo "::error::Missing ${description}: ${needle}" >&2
    exit 1
  fi
}

for apk in "$plugin_apk" "$trace_apk"; do
  [[ -f "$apk" ]] || { echo "::error::APK not found: $apk" >&2; exit 1; }
done

plugin_manifest=$("$aapt2" dump xmltree --file AndroidManifest.xml "$plugin_apk")
plugin_resources=$("$aapt2" dump resources "$plugin_apk")
plugin_resource_text=$(tr -d '"' <<<"$plugin_resources")
trace_manifest=$("$aapt2" dump xmltree --file AndroidManifest.xml "$trace_apk")
plugin_contents=$(unzip -Z1 "$plugin_apk")

require 'top.mobilegl.plugin' "$plugin_manifest" 'plugin package name'
require 'MobileGL' "$plugin_manifest" 'plugin label'
require 'fclPlugin' "$plugin_manifest" 'legacy plugin marker'
require 'fclPlugin_V2' "$plugin_manifest" 'V2 plugin marker'
require 'LIBGL_ES=3:POJAV_RENDERER=opengles3:MOBILEGL_BACKEND_TYPE=DirectGLES' "$plugin_manifest" 'V1 DirectGLES fallback'
require 'string/config' "$plugin_resources" 'V2 renderer configuration resource'
require '{displayName:MobileGL,rendererId:opengles3' "$plugin_resource_text" 'V2 MobileGL entry and renderer ID'
require 'rendererGLPath:**|libMobileGL.so' "$plugin_resource_text" 'V2 GL library path'
require 'rendererEGLPath:**|libMobileGL.so' "$plugin_resource_text" 'V2 EGL library path'
require 'key:LIBGL_ES,value:3' "$plugin_resource_text" 'V2 fixed LIBGL_ES variable'
require 'key:MOBILEGL_BACKEND_TYPE' "$plugin_resource_text" 'V2 backend variable'
require 'defaultValue:DirectGLES' "$plugin_resource_text" 'V2 DirectGLES default'
require 'DirectVulkan' "$plugin_resource_text" 'V2 DirectVulkan option'
require 'key:MOBILEGL_DISABLE_TIMERQUERY' "$plugin_resource_text" 'V2 timer-query toggle'
require 'key:MOBILEGL_MAGMA_DISABLE_SUBGROUP' "$plugin_resource_text" 'V2 Vulkan subgroup toggle'
require 'key:MOBILEGL_MAGMA_R11G11B10F_FALLBACK' "$plugin_resource_text" 'V2 Magma format fallback toggle'
require 'key:MOBILEGL_MAGMA_FRAMESINFLIGHT' "$plugin_resource_text" 'V2 Magma frames-in-flight setting'
require 'key:MOBILEGL_ESPRYT_AVOID_SAMPLER_MIPMAP_MIN_FILTER' "$plugin_resource_text" 'V2 sampler workaround toggle'
require 'key:MOBILEGL_COHERENT_AS_FLUSH' "$plugin_resource_text" 'V2 coherent-as-flush toggle'
require 'key:MOBILEGL_ESPRYT_USE_ANGLE' "$plugin_resource_text" 'V2 ANGLE toggle'

if [[ $(grep -Fc 'fclPlugin_V2' <<<"$plugin_manifest") -ne 1 ]]; then
  echo '::error::Plugin manifest must expose exactly one V2 descriptor' >&2
  exit 1
fi

if ! grep -Eq '^lib/[^/]+/libMobileGL\.so$' <<<"$plugin_contents"; then
  echo '::error::Plugin APK does not contain libMobileGL.so' >&2
  exit 1
fi

require 'top.mobilegl.plugin.trace' "$trace_manifest" 'trace package name'
require 'top.mobilegl.plugin.TRACE_REPLAY' "$trace_manifest" 'trace replay action'
if grep -Fq 'fclPlugin' <<<"$trace_manifest"; then
  echo '::error::Trace APK must not advertise renderer-plugin metadata' >&2
  exit 1
fi
if grep -Fq 'android.intent.action.MAIN' <<<"$trace_manifest"; then
  echo '::error::Trace APK must not expose a launcher activity' >&2
  exit 1
fi

echo 'Validated unified MobileGL plugin APK and isolated trace APK.'

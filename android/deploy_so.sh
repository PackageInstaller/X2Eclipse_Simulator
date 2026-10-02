#!/usr/bin/env bash
# deploy_so.sh — 热替换已安装游戏的 libx2offline.so 并重启(免重装)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
PKG="${PKG:-com.siva.project.x2}"
SO="${1:-$ROOT/build/lib/arm64-v8a/libx2offline.so}"
SU() { adb shell su -c "\"$*\""; }

if [ "${BUILD:-0}" = "1" ]; then
  SKIP_APK=1 "$ROOT/build.sh"
fi

LIB_DIR="$(dirname "$(adb shell pm path "$PKG" | sed -n 's/^package://p' | head -1 | tr -d '\r')")/lib/arm64"
[ "$LIB_DIR" != "./lib/arm64" ] || { echo "ERROR: 设备上未安装 $PKG" >&2; exit 1; }

adb shell am force-stop "$PKG"

if [ "${NO_SO:-0}" != "1" ]; then
  [ -f "$SO" ] || { echo "ERROR: so 不存在: $SO" >&2; exit 1; }
  SU "ls '$LIB_DIR/libx2offline.so'" >/dev/null 2>&1 ||
    { echo "ERROR: $LIB_DIR 下没有 libx2offline.so,先安装一次 x2_offline.apk" >&2; exit 1; }
  adb push "$SO" /data/local/tmp/libx2offline.so >/dev/null
  SU "rm -f '$LIB_DIR/libx2offline.so' && cp /data/local/tmp/libx2offline.so '$LIB_DIR/libx2offline.so' && chown system:system '$LIB_DIR/libx2offline.so' && chmod 755 '$LIB_DIR/libx2offline.so' && restorecon '$LIB_DIR/libx2offline.so'; rm -f /data/local/tmp/libx2offline.so"
  REMOTE_MD5="$(SU "md5sum '$LIB_DIR/libx2offline.so'" | cut -d' ' -f1 | tr -d '\r')"
  LOCAL_MD5="$(md5sum "$SO" | cut -d' ' -f1)"
  [ "$REMOTE_MD5" = "$LOCAL_MD5" ] || { echo "ERROR: md5 不一致 ($REMOTE_MD5 != $LOCAL_MD5)" >&2; exit 1; }
  echo "已替换 $LIB_DIR/libx2offline.so ($LOCAL_MD5)"
fi

if [ "${BACKEND:-embedded}" = "host" ]; then
  SU "setprop debug.x2.backend host"
  adb reverse tcp:9999 tcp:9999 && adb reverse tcp:10001 tcp:10001
  echo "后端: host (电脑上运行宿主版 x2eclipse_offline)"
else
  SU "setprop debug.x2.backend embedded"
  adb reverse --remove tcp:9999 2>/dev/null || true
  adb reverse --remove tcp:10001 2>/dev/null || true
  echo "后端: embedded"
fi

if [ "${RESTART:-1}" != "0" ]; then
  adb logcat -c
  adb shell am start -n "$(adb shell cmd package resolve-activity --brief "$PKG" | tail -1 | tr -d '\r')" >/dev/null
  echo "游戏已重启; 日志: adb logcat -s x2offline Unity"
fi

#!/usr/bin/env bash
# build.sh — 一键构建 X2Eclipse 离线包 (libx2offline.so + 重打包 APK)
#
#   ./build.sh                 # 全流程; 检测到 adb 设备则自动安装
#   INSTALL=0 ./build.sh       # 只构建,不安装
#   SKIP_APK=1 ./build.sh      # 只构建 so
#   RELEASE=1 ./build.sh       # strip 符号的发布构建
#   NDK_ROOT=/path ./build.sh  # 指定 NDK
set -euo pipefail

NDK_ROOT="${NDK_ROOT:-/opt/android-ndk}"
ANDROID_API="${ANDROID_API:-21}"
JOBS="${JOBS:-$(nproc)}"

BUILD_DIR="build"
SO_OUT="$BUILD_DIR/lib/arm64-v8a/libx2offline.so"
APK_OUT="$BUILD_DIR/x2_offline.apk"
KEYSTORE="$BUILD_DIR/debug.keystore"
SOURCE_APK="apk/解神者_2.4.apk"
CONFIG_JSON="config/GameConfigx2.json"
CONFIG_AB="data/$(python3 -c "import hashlib;print(hashlib.md5(b'GameConfigx2').hexdigest())").ab"

if [ ! -f "data/tables.x2data" ]; then
  echo "生成 tables.x2data..."
  python3 tools/prepare_data.py \
    --input-dir "masterdata/table" \
    --output "data/tables.x2data" \
    --manifest "data/tables.json"
fi

echo "打包 GameConfigx2..."
python3 - "$CONFIG_JSON" "data/GameConfigx2.offline.json" <<'EOF'
import json, sys
src = json.load(open(sys.argv[1], encoding="utf-8"))
gray = next(c for c in src["configs"] if "gray" in c["packageName"])
names = ["gray"] + [n for c in src["configs"] for n in c["packageName"] if n != "gray"]
json.dump({"configs": [dict(gray, packageName=names, Login_Url="http://127.0.0.1:9999")], "a": src.get("a", "")}, 
          open(sys.argv[2], "w", encoding="utf-8"), ensure_ascii=False)
EOF
(cd data && python3 ../tools/GameConfigx2.py GameConfigx2.offline.json)

KEEP_SYMBOLS=ON
if [ "${RELEASE:-0}" = "1" ]; then
  KEEP_SYMBOLS=OFF
fi

echo "编译 libx2offline.so..."
cmake -S . -B "$BUILD_DIR" \
  -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM="android-$ANDROID_API" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DKEEP_SYMBOLS="$KEEP_SYMBOLS"

cmake --build "$BUILD_DIR" --target x2offline -j"$JOBS"
[ -f "$SO_OUT" ] || { echo "ERROR: 未生成 $SO_OUT" >&2; exit 2; }

if [ "${SKIP_APK:-0}" = "1" ]; then
  echo "跳过 APK 打包 (SKIP_APK=1)"
  exit 0
fi

echo "打包 APK..."
INSTALL_FLAG=()
if [ "${INSTALL:-auto}" != "0" ] && command -v adb >/dev/null 2>&1 \
    && adb devices | grep -qE '\bdevice$'; then
  INSTALL_FLAG=(--install)
fi

python3 tools/patch_apk.py \
  --apk "$SOURCE_APK" \
  --so "$SO_OUT" \
  --output "$APK_OUT" \
  --keystore "$KEYSTORE" \
  --config-ab "$CONFIG_AB" \
  "${INSTALL_FLAG[@]}"

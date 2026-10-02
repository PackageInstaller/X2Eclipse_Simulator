import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile


zipfile.ZIP64_LIMIT = 0xFFFFFFFE

PATCH_MARK = "# x2-offline: load our backend so first"

LB_SMALI = "com/excelliance/open/LBApplication.smali"
LOAD_INJECT = f"""
{PATCH_MARK}
    const-string v0, "x2offline"

    invoke-static {{v0}}, Ljava/lang/System;->loadLibrary(Ljava/lang/String;)V
"""

BUILD_TOOLS_CANDIDATES = [
    "/usr/sbin",
    "/opt/android-sdk/build-tools/37.0.0",
    "/opt/android-sdk/build-tools/36.0.0",
    "/opt/android-sdk/build-tools/35.0.0",
    "/opt/android-sdk/build-tools/34.0.0",
]


def find_tool(name: str) -> str:
    for cand in BUILD_TOOLS_CANDIDATES:
        path = os.path.join(cand, name)
        if os.path.isfile(path) and os.access(path, os.X_OK):
            return path
    raise SystemExit(f"ERROR: tool {name} not found")


def run(cmd: list[str], cwd: str | None = None) -> None:
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True, cwd=cwd)


def patch_smali(smali_path: str) -> None:
    with open(smali_path, "r", encoding="utf-8") as f:
        content = f.read()
    if PATCH_MARK in content:
        print("  smali already patched")
        return
    marker = ".method protected attachBaseContext(Landroid/content/Context;)V"
    pos = content.find(marker)
    if pos < 0:
        raise SystemExit("ERROR: attachBaseContext not found in LBApplication.smali")
    insert_at = pos + len(marker)
    # attachBaseContext uses .registers 4; v0/v1 are scratch before first use.
    content = content[:insert_at] + "\n" + LOAD_INJECT + content[insert_at:]
    with open(smali_path, "w", encoding="utf-8") as f:
        f.write(content)
    print(f"  patched {smali_path}")


def rebuild_dex(smali_dir: str, out_dex: str) -> None:
    run(["smali", "a", smali_dir, "-o", out_dex])


def ensure_keystore(keystore: str) -> None:
    if os.path.exists(keystore):
        return
    run([
        "keytool", "-genkeypair", "-v",
        "-keystore", keystore,
        "-storepass", "android", "-keypass", "android",
        "-alias", "androiddebugkey",
        "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000",
        "-dname", "CN=Android Debug,O=Android,C=US",
    ])


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--apk", required=True, help="原始 APK 路径")
    parser.add_argument("--so", required=True, help="libx2offline.so 路径")
    parser.add_argument("--config-ab", help="替换 assets/42c44a...ab 的离线配置(可选)")
    parser.add_argument("--output", required=True, help="输出 APK 路径")
    parser.add_argument("--keystore", default="out/debug.keystore")
    parser.add_argument("--install", action="store_true", help="构建后 adb install -r")
    args = parser.parse_args()

    apk = os.path.abspath(args.apk)
    so = os.path.abspath(args.so)
    output = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    keystore = os.path.abspath(args.keystore)

    zipalign = find_tool("zipalign")
    apksigner = find_tool("apksigner")

    with tempfile.TemporaryDirectory(prefix="x2apk_") as work:
        dex_name = "classes.dex"
        run(["unzip", "-o", "-q", apk, dex_name, "-d", work])
        smali_dir = os.path.join(work, "smali")
        run(["baksmali", "d", os.path.join(work, dex_name), "-o", smali_dir])
        patch_smali(os.path.join(smali_dir, LB_SMALI))
        run(["smali", "a", smali_dir, "-o", os.path.join(work, dex_name)])
        stage_apk = os.path.join(work, "staged.apk")
        with zipfile.ZipFile(apk) as src, zipfile.ZipFile(stage_apk, "w") as dst:
            for item in src.infolist():
                if item.filename == dex_name:
                    dst.write(os.path.join(work, dex_name), dex_name,
                              compress_type=item.compress_type)
                    continue
                if args.config_ab and item.filename.startswith("assets/") and item.filename.endswith(".ab") \
                        and os.path.basename(item.filename) == os.path.basename(args.config_ab):
                    print(f"  replace config: {item.filename}")
                    dst.write(args.config_ab, item.filename, compress_type=item.compress_type)
                    continue
                if item.filename.startswith("lib/"):
                    continue 
                dst.writestr(item, src.read(item.filename))
            for item in src.infolist():
                if item.filename.startswith("lib/arm64-v8a/"):
                    dst.writestr(item, src.read(item.filename))
            dst.write(so, "lib/arm64-v8a/libx2offline.so", compress_type=zipfile.ZIP_STORED)

        aligned = os.path.join(work, "aligned.apk")
        run([zipalign, "-f", "-p", "4", stage_apk, aligned])
        ensure_keystore(keystore)
        run([apksigner, "sign", "--ks", keystore, "--ks-pass", "pass:android",
             "--key-pass", "pass:android", "--out", output, aligned])
        run([apksigner, "verify", output])
        print(f"  signed: {output}")

    if args.install:
        run(["adb", "install", "-r", "-d", "--no-incremental", output])
        print("  installed to device")


if __name__ == "__main__":
    main()

# X2Eclipse Simulator

《解神者》离线客户端后端。把 `libx2offline.so` 注入官方 APK 后，游戏在设备上会启动一个
本地服务端，配合内嵌的数据表即可离线运行。

## 目录结构

```
.
├── CMakeLists.txt          NDK 构建 libx2offline.so
├── build.sh                构建入口(唯一)
├── build/                  构建产物,全部集中在这里(不入库)
├── android/
│   └── deploy_so.sh        免重装热替换 so / 切换后端模式
├── config/
│   └── GameConfigx2.json   离线登录配置来源
├── data/                   构建数据与产物(表 blob、离线配置)
├── masterdata/             原始加密数据表
├── src/                    C++ 后端源码
├── tools/                  构建期 Python 工具
│   ├── prepare_data.py     数据表解密并打包成 tables.x2data
│   ├── decrypt_table.py    单表解密
│   ├── GameConfigx2.py     离线登录配置加解密
│   └── patch_apk.py        注入 so + 重打包 + 签名
└── apk/                    原始游戏 APK(不入库)
```

## 依赖

- Android NDK(默认 `/opt/android-ndk`，可用 `NDK_ROOT` 覆盖)
- CMake 3.21+、Ninja
- Python 3 + pycryptodome
- 打包工具：`smali`、`baksmali`、`zipalign`、`apksigner`
- 可选：`adb`(自动安装、日志、热替换)

## 构建

```bash
1. apk目录下放置 解神者_2.4.apk ，注意必须是关服前最好一版的官方版，不是bilibili版

2. ./build.sh 
```

流程分四步：

1. 数据表：`data/tables.x2data` 缺失时，从 `masterdata/table` 重新解密打包
2. 配置：由 `config/GameConfigx2.json` 生成离线登录配置并加密
3. 编译：CMake + NDK 构建 `libx2offline.so`
4. 打包：注入 `LBApplication`、重打包、zipalign、debug 签名

检测到 adb 设备时会自动安装。产物：

- `build/lib/arm64-v8a/libx2offline.so`
- `build/x2_offline.apk`

## 常用选项

```bash
INSTALL=0 ./build.sh               # 只构建，不安装
SKIP_APK=1 ./build.sh              # 只构建 so
RELEASE=1 ./build.sh               # strip 符号的发布构建
NDK_ROOT=/path/to/ndk ./build.sh   # 指定 NDK
android/deploy_so.sh               # 热替换设备上的 so 并重启游戏
```

查看运行日志：

```bash
adb logcat -s x2offline
```

## 许可

见 [LICENSE](LICENSE)。

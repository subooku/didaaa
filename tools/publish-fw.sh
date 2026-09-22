#!/usr/bin/env bash
# 把刚编译出来的固件发布给 CW 服务器，设备就能通过菜单 FIRMWARE 一页 OTA 拉走。
#
# 用法：./tools/publish-fw.sh
#
# 做三件事：
#   1) 从 main/cw_radio.h 的 #define CW_FW_VERSION 抓版本号（不手写第二遍，
#      免得服务器上的版本和固件里编译进去的版本对不上）；
#   2) 把 build/FoloToy-AI-Passport.bin 拷成 cw-server/firmware/cw.bin；
#   3) 写 cw-server/firmware/version.json（版本 / 字节数 / SHA-256 / 发布时间 / 分区布局）。
#
# ★ 服务器不用重启：/fw/version 每次请求都重新读盘（见 server.js）。
# ★ 只发布 app 镜像 —— 分区表烧在 0x8000 且改不了 OTA，永远只能串口烧。
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER_DIR="${REPO_ROOT}/../didaaa-server"
FW_DIR="${SERVER_DIR}/firmware"
APP_BIN="${REPO_ROOT}/build/FoloToy-AI-Passport.bin"
VER_HDR="${REPO_ROOT}/main/cw_radio.h"

[[ -f "${APP_BIN}" ]] || { echo "找不到编译产物: ${APP_BIN}（先跑 ./tools/build-cw.sh build）" >&2; exit 1; }

VER="$(sed -nE 's/^#define[[:space:]]+CW_FW_VERSION[[:space:]]+"([^"]+)".*/\1/p' "${VER_HDR}" | head -1)"
[[ -n "${VER}" ]] || { echo "从 ${VER_HDR} 读不到 CW_FW_VERSION" >&2; exit 1; }

mkdir -p "${FW_DIR}"
cp "${APP_BIN}" "${FW_DIR}/cw.bin"

SIZE="$(wc -c < "${FW_DIR}/cw.bin" | tr -d ' ')"
SHA="$(shasum -a 256 "${FW_DIR}/cw.bin" | awk '{print $1}')"
WHEN="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

cat > "${FW_DIR}/version.json" <<EOF
{
  "version": "${VER}",
  "size": ${SIZE},
  "sha256": "${SHA}",
  "built": "${WHEN}",
  "chip": "esp32c3",
  "layout": "ota_0@0x10000 ota_1@0x400000 (8MB)",
  "notes": "OTA via menu FIRMWARE. Partition table is serial-flash only."
}
EOF

echo "已发布固件 v${VER}"
echo "  镜像    : ${FW_DIR}/cw.bin (${SIZE} B)"
echo "  SHA-256 : ${SHA}"
echo "  版本文件: ${FW_DIR}/version.json"
echo
echo "设备端：菜单 → FIRMWARE → UPDATE。服务器在跑的话不用重启它。"

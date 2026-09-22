#!/usr/bin/env bash
# 一键编译：编译 →（可选）自动抬版本号 → 发布到 CW 服务器 →（可选）串口烧录。
#
# 用法：
#   ./tools/onekey.sh                 只编译（不动版本号）
#   ./tools/onekey.sh -p              编译 + 自动抬版本号（patch+1）+ 发布  ← 最常用
#   ./tools/onekey.sh -p --minor      抬 minor（1.1.3 → 1.2.0）
#   ./tools/onekey.sh -p --major      抬 major（1.1.3 → 2.0.0）
#   ./tools/onekey.sh -p --keep-ver   发布但不抬版本号（服务器上已有同名固件时会推不动）
#   ./tools/onekey.sh -f              编译 + 串口烧录（自动探测串口）
#   ./tools/onekey.sh -f --monitor    烧完接着开串口监视器（Ctrl-] 退出）
#   ./tools/onekey.sh -p -f           编译 + 抬版本号 + 发布 + 烧录
#   ./tools/onekey.sh --clean         编译前先 fullclean（慢，但干净）
#   ./tools/onekey.sh --test          额外跑一遍协议单测
#
# ★ 为什么要自动抬版本号：设备端 OTA 是按 cw_ota_ver_newer(服务器版本, 本机版本)
#   逐段比 x.y.z 的。版本号不变时，哪怕服务器上的 bin 已经换了，设备菜单里也只显示
#   "UP TO DATE"，改动根本传不下去。所以要发布就一定要抬号 —— 这条踩过。
# ★ 编译失败会把版本号回滚：先改版本号再编译时，编译挂了不能留个没编译过的新版本号。
set -euo pipefail

# 先把自身路径定下来：下面要 cd 到仓库根，之后再拿相对路径就找不着自己了。
SELF="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/$(basename -- "${BASH_SOURCE[0]}")"
REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}"

VER_HDR="${REPO_ROOT}/main/cw_radio.h"
APP_BIN="${REPO_ROOT}/build/FoloToy-AI-Passport.bin"
LOG="${REPO_ROOT}/build/onekey.log"

DO_PUBLISH=0; DO_FLASH=0; DO_CLEAN=0; DO_TEST=0; DO_MONITOR=0
BUMP="patch"; KEEP_VER=0; PORT=""

# ---------------------------------------------------------------- 参数解析
while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--publish)  DO_PUBLISH=1 ;;
    -f|--flash)    DO_FLASH=1 ;;
    --clean)       DO_CLEAN=1 ;;
    --test)        DO_TEST=1 ;;
    --monitor)     DO_MONITOR=1 ;;
    --keep-ver)    KEEP_VER=1 ;;
    --major)       BUMP="major" ;;
    --minor)       BUMP="minor" ;;
    --patch)       BUMP="patch" ;;
    --port|-P)     PORT="${2:-}"; shift ;;
    -h|--help)     sed -n '2,19p' "${SELF}"; exit 0 ;;
    *) echo "不认识的参数: $1（-h 看用法）" >&2; exit 1 ;;
  esac
  shift
done

# ---------------------------------------------------------------- 小工具
say()  { printf '\033[1;36m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m    %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m✗  %s\033[0m\n' "$*" >&2; exit 1; }

fw_ver() {   # 从 cw_radio.h 读当前版本号
  sed -nE 's/^#define[[:space:]]+CW_FW_VERSION[[:space:]]+"([^"]+)".*/\1/p' "${VER_HDR}" | head -1
}
set_ver() { # 写回版本号（macOS 自带 perl，-i 不需要备份后缀）
  perl -pi -e "s/#define(\s+)CW_FW_VERSION(\s+)\"[^\"]+\"/#define\${1}CW_FW_VERSION\${2}\"$1\"/" "${VER_HDR}"
}
bump() {    # 1.2.3 + patch/minor/major
  local v="$1" part="$2" a b c
  IFS='.' read -r a b c <<< "${v}"
  [[ -n "${c}" ]] || { a="${v}"; b=0; c=0; }
  case "${part}" in
    major) echo "$((a+1)).0.0" ;;
    minor) echo "${a}.$((b+1)).0" ;;
    *)     echo "${a}.${b}.$((c+1))" ;;
  esac
}
detect_port() {  # 常见 USB 转串芯片，挑第一个
  local p
  for pat in /dev/cu.usbmodem* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART* /dev/cu.usbserial*; do
    p="$(ls ${pat} 2>/dev/null | head -1)"; [[ -n "${p}" ]] && { echo "${p}"; return 0; }
  done
  return 1
}

mkdir -p build

# ---------------------------------------------------------------- 1. 版本号
OLD_VER="$(fw_ver)"
[[ -n "${OLD_VER}" ]] || die "从 ${VER_HDR} 读不到 CW_FW_VERSION"
NEW_VER="${OLD_VER}"

if (( DO_PUBLISH )) && (( ! KEEP_VER )); then
  NEW_VER="$(bump "${OLD_VER}" "${BUMP}")"
  say "版本号 ${OLD_VER} → ${NEW_VER}（${BUMP}）"
  set_ver "${NEW_VER}"
fi

# 编译挂了就把版本号改回去，别留一个没编译过的新号
rollback() {
  if [[ "${NEW_VER}" != "${OLD_VER}" ]]; then
    warn "编译失败，版本号回滚为 ${OLD_VER}"
    set_ver "${OLD_VER}"
  fi
}
trap rollback ERR

# ---------------------------------------------------------------- 2. clean
if (( DO_CLEAN )); then
  say "fullclean（慢，但干净）"
  ./tools/build-cw.sh fullclean 2>&1 | tee -a "${LOG}" | tail -3
fi

# ---------------------------------------------------------------- 3. 编译
# ★ idf.py 自己会在成功的那次构建末尾创建 build/log（里面是 idf_py_stdout_output_*）。
#   下一次构建它又要 mkdir 同一个目录 → PermissionError: EEXIST，直接失败退出 1。
#   不是我们建的，是它的产物；所以每次编译前都得替它清掉。这个坑踩过好几次。
rm -rf "${REPO_ROOT}/build/log"

say "编译固件 v${NEW_VER}"
set +e
./tools/build-cw.sh build 2>&1 | tee "${LOG}" \
  | grep -E "error|Error|warning:|Project build complete|\.bin|Total image size" | head -30
BUILD_RC="${PIPESTATUS[0]}"
set -e
if (( BUILD_RC != 0 )); then
  warn "完整日志：${LOG}"
  die "编译失败（退出码 ${BUILD_RC}）"
fi
# 保险：idf.py 偶尔 errors 打完了还返回 0（比如部分目标失败）。日志里出现这些
# 关键字就一律当失败，别让一个失败的构建被当成成功发布出去。
if grep -qE "PermissionError|CMake Error|ninja: build stopped|FAILED:|undefined reference" "${LOG}"; then
  warn "完整日志：${LOG}"
  die "编译日志里有失败关键字，按失败处理（退出码虽为 0）"
fi
trap - ERR

[[ -f "${APP_BIN}" ]] || die "编译过了但没找到 ${APP_BIN}"

SIZE="$(wc -c < "${APP_BIN}" | tr -d ' ')"
printf '    镜像: %s (%s 字节)\n' "${APP_BIN}" "${SIZE}"

# ---------------------------------------------------------------- 4. 单测
if (( DO_TEST )); then
  say "协议单测"
  cc -std=c11 -Wall -Wextra -Werror -Imain tests/test_cw_proto.c main/cw_proto.c -o /tmp/t_proto \
    && /tmp/t_proto && echo "    单测 PASS"
fi

# ---------------------------------------------------------------- 5. 发布
if (( DO_PUBLISH )); then
  say "发布到 CW 服务器"
  ./tools/publish-fw.sh | sed 's/^/    /'
  SERVER_VER="$(curl -s --max-time 5 http://localhost:8080/fw/version | sed -nE 's/.*"version":"([^"]+)".*/\1/p')"
  if [[ -n "${SERVER_VER}" ]]; then
    [[ "${SERVER_VER}" == "${NEW_VER}" ]] \
      && echo "    服务器已在提供 v${SERVER_VER}" \
      || warn "服务器提供的还是 v${SERVER_VER}（服务器在跑的话会自动读盘，稍等再查）"
  else
    warn "服务器没在跑（localhost:8080 无响应），固件已落盘，等服务器起来就能 OTA"
  fi
fi

# ---------------------------------------------------------------- 6. 烧录
if (( DO_FLASH )); then
  [[ -n "${PORT}" ]] || PORT="$(detect_port)" || die "没找到串口，用 --port /dev/cu.xxx 指定"
  say "串口烧录 → ${PORT}"
  warn "板子若卡在等待同步，按住 BOOT 再上电/按 RST 重试"
  ./tools/build-cw.sh -p "${PORT}" flash 2>&1 | tee -a "${LOG}" | tail -5
  if (( DO_MONITOR )); then
    say "打开串口监视器（Ctrl-] 退出）"
    ./tools/build-cw.sh -p "${PORT}" monitor
  fi
fi

# ---------------------------------------------------------------- 汇总
say "完成 v${NEW_VER}（${SIZE} 字节）"
(( DO_PUBLISH )) && echo "    设备端：菜单 → FIRMWARE → UPDATE" || echo "    只编译未发布：要 OTA 就加 -p"

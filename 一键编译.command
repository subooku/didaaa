#!/usr/bin/env bash
# 双击运行的一键编译入口。真正的逻辑在 tools/onekey.sh，这里只负责弹菜单 + 收尾不关窗。
cd -- "$(dirname -- "$0")" || exit 1

echo "=============== CW 电台固件 · 一键编译 ==============="
echo "  当前版本: $(sed -nE 's/^#define[[:space:]]+CW_FW_VERSION[[:space:]]+"([^"]+)".*/\1/p' main/cw_radio.h | head -1)"
echo
echo "  1) 只编译"
echo "  2) 编译 + 自动抬版本号 + 发布到服务器   ← 常用"
echo "  3) 编译 + 串口烧录"
echo "  4) 编译 + 抬版本号 + 发布 + 烧录"
echo "  5) 全新编译（先 fullclean）"
echo
printf '  选一个 [默认 2]: '
read -r choice
case "${choice}" in
  1) ARGS=() ;;
  3) ARGS=(-f) ;;
  4) ARGS=(-p -f) ;;
  5) ARGS=(--clean -p) ;;
  *) ARGS=(-p) ;;
esac

echo
./tools/onekey.sh "${ARGS[@]}"
RC=$?

echo
if (( RC == 0 )); then
  echo "===================== 完成 ====================="
else
  echo "=================== 失败（rc=$RC）===================="
fi
printf '按任意键关闭本窗口...'
read -n 1 -s -r
echo
exit "${RC}"

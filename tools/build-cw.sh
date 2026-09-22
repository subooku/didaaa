#!/usr/bin/env bash
# 交付用的一次性构建封装：激活 ESP-IDF 5.5.3 后执行 idf.py 的任意子命令。
#
# ★ 为什么激活要放进 `bash -c '...' bash ...` 这个子 shell：
#   eim 3.0 生成的 activate_idf_v5.5.3.sh 末尾用 is_sourced() 判断自己是被 source
#   还是被执行，判断依据是 `${0##*/}` 是否等于 bash/sh/dash 等壳名。
#     - 在 `bash -c 'source ...'` 里 $0 = "bash"   → 判定为被 source → 正常导出环境
#     - 在 ./tools/build-cw.sh 里   $0 = "build-cw.sh" → 判定为被执行 → 打印提示后
#       `exit 1`，把本脚本一起带走。所以必须让 $0 是 bash，这里靠 `bash -c` 的
#       第一个参数（argv[0]）显式传 "bash" 实现。
#   另外它还会引用未定义变量，在 set -u 下当场终止，故子 shell 内先 set +eu。
set -euo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
export IDF_PATH="${IDF_PATH:-/Users/zgf/.espressif/v5.5.3/esp-idf}"
ACTIVATE="/Users/zgf/.espressif/tools/activate_idf_v5.5.3.sh"

[[ -f "${ACTIVATE}" ]] || { echo "找不到激活脚本: ${ACTIVATE}" >&2; exit 1; }

exec bash -c '
    set +eu
    # shellcheck disable=SC1090
    source "$1" >/dev/null 2>&1
    set -eu
    # eim 3.0 的 activate 不把 $IDF_PATH/tools 加进 PATH，idf.py 就在那儿。
    export PATH="${IDF_PATH}/tools:${PATH}"
    command -v idf.py >/dev/null 2>&1 || { echo "idf.py 不可用：5.5.3 激活失败" >&2; exit 1; }
    cd "$2"; shift 2
    echo "IDF: $(idf.py --version 2>&1 | tail -1)"
    exec idf.py "$@"
' bash "${ACTIVATE}" "${REPO_ROOT}" "$@"

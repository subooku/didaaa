#!/bin/zsh
# 监听设备串口一段时间并把日志落盘（非交互，适合脚本调用）。
# 结束后必定清理监听进程，否则下一次 idf.py flash 会报 "port is busy"。
# 用法: tools/serial-peek.sh [设备] [秒数]
set -u
PORT="${1:-/dev/cu.usbmodem14401}"
SECS="${2:-25}"

IDF_ROOT="/Users/zgf/.espressif/v5.5.3/esp-idf"
source /Users/zgf/.espressif/tools/activate_idf_v5.5.3.sh >/dev/null 2>&1
export PATH="$IDF_ROOT/tools:$PATH"
PY="$IDF_PYTHON_ENV_PATH/bin/python"

# 先把可能残留的旧监听清掉
pkill -9 -f "esp_idf_monitor" 2>/dev/null
sleep 0.5

"$PY" "$IDF_ROOT/tools/idf_monitor.py" -p "$PORT" -b 115200 \
    build/FoloToy-AI-Passport.elf < /dev/null > /tmp/serial-peek.log 2>&1 &
MON=$!

sleep "$SECS"

kill -INT "$MON" 2>/dev/null
sleep 1
kill -9 "$MON" 2>/dev/null
pkill -9 -f "esp_idf_monitor" 2>/dev/null
wait "$MON" 2>/dev/null

# addr2line 别名：idf_monitor 对 C3 会误用 xtensa 工具链，这里补一个正确的
sed -n '/main_task: Calling app_main/,$p' /tmp/serial-peek.log

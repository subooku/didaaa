#!/usr/bin/env python3
"""抓 CW 设备的串口日志（可选复位）。

用法: serial-log.py [seconds] [--reset] [/dev/cu.xxx]
端口按顺序取:命令行参数 > 环境变量 CW_PORT > 默认 /dev/cu.usbmodem1201
（插拔/换口后设备名会变，比如 1201 → 14401，用 ls /dev/cu.* 查当前是哪个。）
"""
import os, sys, time, serial

sec = 15.0
reset = '--reset' in sys.argv
for a in sys.argv[1:]:
    if a.replace('.', '', 1).isdigit():
        sec = float(a)

port = next((a for a in sys.argv[1:] if a.startswith('/dev/')),
            os.environ.get('CW_PORT', '/dev/cu.usbmodem1201'))
s = serial.Serial(port, 115200, timeout=0.2)
if reset:
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    s.dtr = False
end = time.time() + sec
out = []
while time.time() < end:
    b = s.read(4096)
    if b:
        out.append(b.decode('utf-8', 'replace'))
s.close()
print(''.join(out))

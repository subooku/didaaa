#!/usr/bin/env python3
"""核对 sdkconfig.defaults 与实际编译生效的配置是否一致（只读，不改任何东西）。

为什么需要它：sdkconfig 是生成物。一旦有人手动改过 sdkconfig（比如 VSCode 里
menuconfig 调了 LVGL 内存池），而没回头改 sdkconfig.defaults，两边就分叉了。
平时编译一切正常，等哪天 fullclean / 换台机器重新生成 sdkconfig，改动就悄悄丢失。

用法：
    python3 tools/check-sdkconfig.py

三个数据源，按权威度从低到高：
    sdkconfig.defaults  →  sdkconfig  →  build/config/sdkconfig.h
（最后一个才是真正编译进固件的，最准）
"""
from __future__ import annotations
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEF = ROOT / "sdkconfig.defaults"
CFG = ROOT / "sdkconfig"
HDR = ROOT / "build" / "config" / "sdkconfig.h"

NOT_SET = re.compile(r"^# (CONFIG_\w+) is not set")
KV_DEFINE = re.compile(r"^\s*(?:#define\s+)?(CONFIG_\w+)\s+(.*?)\s*$")


def load_txt(path: Path) -> tuple[dict[str, str], set[str]]:
    """解析 sdkconfig / defaults：返回 {键: 值} 与 {被显式禁用的键}。"""
    values: dict[str, str] = {}
    unset: set[str] = set()
    if not path.is_file():
        return values, unset
    for line in path.read_text(encoding="utf-8").splitlines():
        s = line.strip()
        m = NOT_SET.match(s)
        if m:
            unset.add(m.group(1))
            continue
        if not s or s.startswith("#"):
            continue
        if "=" in s:
            k, v = s.split("=", 1)
            values[k.strip()] = v.strip().strip('"')
    return values, unset


def load_header(path: Path) -> dict[str, str]:
    """解析 build/config/sdkconfig.h —— 真正编译进固件的值。"""
    out: dict[str, str] = {}
    if not path.is_file():
        return out
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip().startswith("#define"):
            continue
        m = KV_DEFINE.match(line)
        if not m:
            continue
        k, v = m.group(1), m.group(2).strip().strip('"')
        # 宏互相引用（CONFIG_A CONFIG_B）是 ESP-IDF 的写法，跳过
        if re.fullmatch(r"CONFIG_\w+", v):
            continue
        out[k] = v
    return out


def norm(v: str | None, unset_keys: set[str] | None = None, key: str = "") -> str:
    """归一化后比较：'y'/'1' 都算启用，'n'/'未出现'/'is not set' 都算禁用 '-'。

    sdkconfig.h 里布尔写成 1，sdkconfig/defaults 里写成 y，不归一就全是假差异。
    """
    if unset_keys and key in unset_keys:
        return "-"
    if v is None or v in ("n", "", "0", "-"):
        return "-"
    if v == "y":
        return "1"
    return v


def main() -> int:
    if not DEF.is_file():
        print(f"找不到 {DEF}")
        return 1

    defaults, _ = load_txt(DEF)
    cfg_vals, cfg_unset = load_txt(CFG)
    hdr = load_header(HDR)

    width = max((len(k) for k in defaults), default=10)

    def real_value(key: str) -> str:
        """实际生效值优先取编译产物里的，其次 sdkconfig。"""
        if key in hdr:
            return norm(hdr[key])
        if key in cfg_vals:
            return norm(cfg_vals[key], cfg_unset, key)
        return "-"

    print(f"工程根目录: {ROOT}")
    print(f"defaults {len(defaults)} 项；sdkconfig {'有' if CFG.is_file() else '无'}；"
          f"编译产物头 {'有' if hdr else '无'}")
    print()
    print(f"{'配置项':<{width}}  {'defaults':<12} {'实际生效':<12} 判定")
    print("-" * (width + 34))

    diffs: list[tuple[str, str, str]] = []
    for key, want in defaults.items():
        got = real_value(key)
        expect = norm(want)
        exp_show = "(禁用)" if expect == "-" else want
        ok = got == expect
        if not ok:
            diffs.append((key, exp_show, got))
        print(f"{key:<{width}}  {exp_show:<12} {got:<12} {'OK' if ok else '<<< 不一致'}")

    print("-" * (width + 34))
    if not diffs:
        print("全部一致 ✓")
    else:
        print(f"{len(diffs)} 项不一致：")
        for k, w, g in diffs:
            print(f"  {k}: defaults={w}  实际={g}")
        print()
        print("处理办法：把 sdkconfig.defaults 改成实际值（推荐），或者用 "
              "idf.py fullclean 重新按 defaults 生成 —— 后者会丢掉实际值带来的效果，想清楚再干。")

    # 独立的过期值提示：defaults 里没管、但代码会用到的自定义项
    stale = []
    for key in ("CONFIG_CW_SERVER_IP", "CONFIG_CW_MQTT_URI", "CONFIG_CW_CALLSIGN",
                "CONFIG_CW_WIFI_SSID", "CONFIG_CW_WIFI_PASSWORD"):
        v = cfg_vals.get(key)
        if v and v not in ("myssid", "mypassword"):
            stale.append((key, v))
    if stale:
        print()
        print("编译进固件的 CW 默认值（运行时会被 NVS 里的配网值覆盖）：")
        for k, v in stale:
            print(f"  {k} = {v}")

    return 1 if diffs else 0


if __name__ == "__main__":
    sys.exit(main())

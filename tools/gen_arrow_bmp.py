#!/usr/bin/env python3
# 生成主屏"当前频道位置"的向上箭头位图（RGB565，main/cw_arrow_bmp.h）。
#
# 为什么要用位图：只编译了 Montserrat，里面既没有 ▲ 也没有 ↑ 这类字形，
# 而 LVGL 的符号字体要额外挂一份进来，为 11x9 的箭头不划算。底色填成页面背景色
# （0x0B1016），整块放 flash，不占 RAM。改形状就改 SHAPE 后重跑本脚本。
import sys

# 细长版：7 宽 14 高（原来是 11x9，看着扁而宽，像个小三角而不像"指着频率"的指针）。
# 上半是箭头（7 行，最宽处占满 7px），下半是等宽 3px 的短柄。
W, H = 7, 14
BG = (0x0B, 0x10, 0x16)
FG = (0xFF, 0xB3, 0x00)

# 每一行用 (起点, 终点) 表示要填成前景的横向区间；越靠上越窄 = 朝上的箭头。
SHAPE = [
    (3, 3),      # 尖端
    (2, 4),
    (2, 4),
    (1, 5),
    (1, 5),
    (0, 6),      # 箭头最宽处，占满整幅宽度
    (0, 6),
    (2, 4),      # 下面接一截细柄，远看像指针而不是纯三角
    (2, 4),
    (2, 4),
    (2, 4),
    (2, 4),
    (2, 4),
    (2, 4),
]
assert len(SHAPE) == H


def rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def main(path):
    px = []
    for y in range(H):
        lo, hi = SHAPE[y]
        for x in range(W):
            c = FG if lo <= x <= hi else BG
            v = rgb565(*c)
            px.append(v & 0xFF)
            px.append((v >> 8) & 0xFF)

    with open(path, "w") as f:
        f.write("// main/cw_arrow_bmp.h\n")
        f.write("// ★ 自动生成，不要手改。改形状请编辑 tools/gen_arrow_bmp.py 里的 SHAPE 后重跑。\n")
        f.write("//\n")
        f.write("// 主屏频谱下方的频道位置指示：一个朝上的箭头，%dx%d 的 RGB565 位图。\n" % (W, H))
        f.write("#pragma once\n\n#include <stdint.h>\n\n")
        f.write("#define CW_ARROW_W %d\n" % W)
        f.write("#define CW_ARROW_H %d\n" % H)
        f.write("#define CW_ARROW_BYTES (CW_ARROW_W * CW_ARROW_H * 2)\n\n")
        f.write("static const uint8_t cw_arrow_bmp[CW_ARROW_BYTES] = {\n")
        for i in range(0, len(px), 16):
            f.write("    " + ", ".join("0x%02X" % b for b in px[i:i + 16]) + ",\n")
        f.write("};\n")
    print("已写入 %s（%d 字节）" % (path, len(px)))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "main/cw_arrow_bmp.h")

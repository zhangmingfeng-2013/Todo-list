#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成 cpp-todo 应用图标 —— 几何扁平海报版。

构成（现代海报语言：大色块 + 硬边缘 + 长投影）：
1. 双色斜切块面：青（左上主导）+ 橙（右下主导），分界线左低右高
2. 长投影（flat long shadow）：对勾沿 45° 方向的实心投影延伸至图标边缘
3. 白色粗对勾：跨越双色分界 —— 短臂在青、顶点在橙、长臂收于青

技术要点：
- 沿用 iOS 超椭圆 mask（n=5）—— 杜绝 Dock 灰角
- 长投影 = 对勾 mask 沿 (1,1) 多次位移取并集，再裁剪到 squircle 内
- 纯色无渐变：扁平风格的生命线是「大色块 + 干净边缘」
- 对勾跨度刻意横穿分界线，形成 X 形交叉构图（海报张力）
"""
import math
import os
import subprocess
import sys

from PIL import Image, ImageChops, ImageDraw

SIZE = 1024
HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "icons")

# ---- 几何扁平配色 ----
CYAN = (0x00, 0xB4, 0xD8)      # 青：左上块面
ORANGE = (0xFF, 0x6B, 0x1A)    # 橙：右下块面
WHITE = (255, 255, 255, 255)

# 斜切分界线：左缘 y → 右缘 y（左低右高，与对勾长臂形成交叉构图）
Y_LEFT = 620
Y_RIGHT = 460

# 对勾几何（顶点落在橙色区，两端收在青色区）
LW = 150
PTS = [(280, 535), (458, 712), (764, 316)]

# 任务圆点（视觉配重 + "待办"语义）
BULLET_XY = (280, 300)
BULLET_R = 62

# 长投影参数
SHADOW_ALPHA = 82      # 投影不透明度（~32% 黑）
SHADOW_STEP = 4        # 位移步进
SHADOW_DIST = 780      # 投影延伸距离（足够抵达图标边缘）


def boundary_y(x):
    """分界线在 x 处的 y 坐标。"""
    return Y_LEFT + (Y_RIGHT - Y_LEFT) * (x / SIZE)


def superellipse_mask(size, n=5.0):
    """iOS 风格超椭圆 mask：内部 255，外部 0。"""
    mask = Image.new("L", (size, size), 0)
    px = mask.load()
    cx = cy = (size - 1) / 2.0
    a = size / 2.0
    inv_n = 1.0 / n
    for y in range(size):
        for x in range(size):
            u = abs(x - cx) / a
            v = abs(y - cy) / a
            if u < 1.0 and v < 1.0:
                val = (u ** n + v ** n) ** inv_n
                if val <= 1.0:
                    px[x, y] = 255
    return mask


def checkmark_mask(size, pts, lw):
    """对勾形状的 L mask（含圆头端点），供长投影使用。"""
    m = Image.new("L", (size, size), 0)
    d = ImageDraw.Draw(m)
    d.line(pts, fill=255, width=lw, joint="curve")
    r = lw // 2
    for p in pts:
        d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=255)
    return m


def long_shadow(shape, step, dist):
    """45° 长投影：形状沿 (1,1) 方向多次位移的并集。

    经典 flat design 长投影技法：从元素边缘一路延伸到画布边缘，
    形成实心对角带，给纯扁平构图提供纵深感。
    """
    acc = Image.new("L", (SIZE, SIZE), 0)
    n = dist // step
    for i in range(1, n + 1):
        shifted = Image.new("L", (SIZE, SIZE), 0)
        shifted.paste(shape, (step * i, step * i))
        acc = ImageChops.lighter(acc, shifted)
    return acc


def build_icon():
    mask = superellipse_mask(SIZE)

    # ---- 1. 双色斜切块面 ----
    canvas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    blocks = Image.new("RGBA", (SIZE, SIZE), CYAN + (255,))
    db = ImageDraw.Draw(blocks)
    poly = [(0, boundary_y(0)), (SIZE, boundary_y(SIZE)),
            (SIZE, SIZE), (0, SIZE)]
    db.polygon(poly, fill=ORANGE + (255,))
    canvas.paste(blocks, (0, 0), mask)

    # ---- 2. 长投影（对勾的 45° 实心投影） ----
    cm = checkmark_mask(SIZE, PTS, LW)
    shadow_shape = long_shadow(cm, SHADOW_STEP, SHADOW_DIST)
    shadow_layer = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    shadow_layer.paste(
        Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, SHADOW_ALPHA)),
        (0, 0), shadow_shape)
    shadow_in = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    shadow_in.paste(shadow_layer, (0, 0), mask)
    canvas = Image.alpha_composite(canvas, shadow_in)

    # ---- 3. 白色粗对勾（本体压在投影起点上） ----
    d = ImageDraw.Draw(canvas)
    d.line(PTS, fill=WHITE, width=LW, joint="curve")
    r = LW // 2
    for p in PTS:
        d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=WHITE)

    # ---- 4. 任务圆点（视觉配重 + 列表语义） ----
    bx, by = BULLET_XY
    d.ellipse([bx - BULLET_R, by - BULLET_R,
               bx + BULLET_R, by + BULLET_R], fill=WHITE)

    return canvas


def write_iconset(img, iconset_dir):
    os.makedirs(iconset_dir, exist_ok=True)
    sizes = {
        "icon_16x16.png": 16,
        "icon_16x16@2x.png": 32,
        "icon_32x32.png": 32,
        "icon_32x32@2x.png": 64,
        "icon_128x128.png": 128,
        "icon_128x128@2x.png": 256,
        "icon_256x256.png": 256,
        "icon_256x256@2x.png": 512,
        "icon_512x512.png": 512,
        "icon_512x512@2x.png": 1024,
    }
    for name, s in sizes.items():
        img.resize((s, s), Image.LANCZOS).save(
            os.path.join(iconset_dir, name))
    return sizes


def main():
    icon = build_icon()
    os.makedirs(OUT, exist_ok=True)

    png = os.path.join(OUT, "icon.png")
    icon.save(png)
    print(f"[icon] 主图标: {png}")

    if sys.platform == "darwin":
        iconset = os.path.join(OUT, "icon.iconset")
        write_iconset(icon, iconset)
        icns = os.path.join(OUT, "icon.icns")
        subprocess.run(["iconutil", "-c", "icns", iconset,
                        "-o", icns], check=True)
        print(f"[icon] macOS 图标包: {icns}")
        subprocess.run(["rm", "-rf", iconset], check=True)


if __name__ == "__main__":
    main()

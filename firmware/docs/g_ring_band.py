# -*- coding: utf-8 -*-
"""
G 页边缘环：把"逐圈 drawCircle"改成"逐带扫描线填充"的 1:1 验证脚本。

问题：1px 的 Bresenham 圆在 45° 方向天然留棋盘格针孔，34 圈叠起来针孔仍然对齐，
      屏幕上就是一圈一圈的黑色空隙。折线（弦）也一样，步长越细针孔越密。
方案：按"带"做面积填充 —— 每一带是一个薄圆环（rIn < r <= rOut），逐行求
      [CX-xo, CX-xi-1] 和 [CX+xi+1, CX+xo] 两段水平线，段内像素全覆盖，没有针孔。
      带内用同一个颜色，带与带之间亮度差 1~2 个 565 阶，肉眼是连续渐变。

跑法：python docs/g_ring_band.py
输出：_band_*.png（逻辑 466 画布）+ _bandpanel_*.png（PortScale 缩到 360 后）
"""
import math
import sys
from g_ring_render import Canvas, blend565, downscale, L, P, CX, CCY

R_OUT = 229          # 环外沿（离屏边 4px）
BANDS = 16           # 亮度渐变分带数
C_YELLOW, C_ORANGE, C_RED = 0xFFE0, 0xFD20, 0xF800


def fill_hspan(cv, x0, y, w, c):
    if w <= 0:
        return
    for x in range(x0, x0 + w):
        cv.set(x, y, c)


def ring_band(cv, cx, cy, r_out, r_in, c):
    """一个薄圆环，扫描线填充。等价于固件 ringBand()"""
    if r_in < 0:
        r_in = 0
    r_out2, r_in2 = r_out * r_out, r_in * r_in
    for y in range(cy - r_out, cy + r_out + 1):
        dy = y - cy
        dy2 = dy * dy
        if dy2 > r_out2:
            continue
        xo = int(math.sqrt(r_out2 - dy2))
        if dy2 < r_in2:
            xi = int(math.sqrt(r_in2 - dy2))
        else:
            xi = -1
        if xi < 0:
            fill_hspan(cv, cx - xo, y, xo * 2 + 1, c)
        elif xo > xi:
            fill_hspan(cv, cx - xo, y, xo - xi, c)
            fill_hspan(cv, cx + xi + 1, y, xo - xi, c)


def draw_ring_scan(cv, mag, bands=BANDS, r_out=R_OUT, span=34.0, notch=True):
    gk = min(1.0, mag / 1.5)
    thick = 10 + int(gk * gk * span + 0.5)
    bright = 140 + int(gk * 115 + 0.5)
    if mag <= 0.7:
        ring_col = blend565(C_YELLOW, C_ORANGE, int(mag * 364.0))
    elif mag <= 1.3:
        ring_col = blend565(C_ORANGE, C_RED, int((mag - 0.7) * 425.0))
    else:
        ring_col = C_RED
    for k in range(bands):
        i0 = k * thick // bands
        i1 = (k + 1) * thick // bands - 1
        if i1 < i0:
            i1 = i0
        w = i1 - i0 + 1
        ro = r_out - i0
        ri = ro - w
        imid = i0 + w // 2
        c = blend565(0x0000, ring_col, max(40, bright - imid * 3))
        ring_band(cv, CX, CCY, ro, ri, c)
    if notch:                      # 底部 28° 缺口（±14°，tan14°=0.2493）
        for y in range(CCY, L):
            half = int((y - CCY) * 0.2493)
            if half > 0:
                fill_hspan(cv, CX - half, y, half * 2 + 1, 0x0000)
    return thick, ring_col


def count_holes(cv, mag, thick, r_out=R_OUT):
    """统计环带内（不含缺口）的纯黑像素 —— 就是肉眼看到的针孔"""
    hole = 0
    tot = 0
    r_in = r_out - thick
    for y in range(CCY - r_out, CCY + r_out + 1):
        dy = y - CCY
        for x in range(CX - r_out, CX + r_out + 1):
            dx = x - CX
            r2 = dx * dx + dy * dy
            if r_in * r_in < r2 <= r_out * r_out:
                if dy > 0 and abs(dx) <= int(dy * 0.2493):   # 缺口不算
                    continue
                tot += 1
                o = (y * L + x) * 3
                if cv.buf[o] == 0 and cv.buf[o + 1] == 0 and cv.buf[o + 2] == 0:
                    hole += 1
    return hole, tot


if __name__ == "__main__":
    for name, mag in [("g003", 0.03), ("g060", 0.60), ("g120", 1.20), ("g150", 1.50)]:
        cv = Canvas(L, L, 0x0000)
        thick, rc = draw_ring_scan(cv, mag)
        h, tot = count_holes(cv, mag, thick)
        cv.png(f"_band_{name}.png")
        downscale(cv).png(f"_bandpanel_{name}.png")
        print(f"{name}  thick={thick:2d}  环内像素={tot:6d}  针孔={h}")

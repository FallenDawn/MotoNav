# -*- coding: utf-8 -*-
"""
1:1 复刻固件 G 页边缘环的绘制，并按 PortScale.h 的双线性缩放到 360 面板，
用来复现"环上有黑色空隙"的问题。纯 Python，无第三方依赖（自带 PNG 编码）。

对照固件：
  imuArcSeg(cx,cy,r,a0,a1,c,step)  -> 每 step 度一条 drawLine((int)px,(int)py,...)
  for i in 0..thick-1: imuArcSeg(CX,cyG,229-i,104,436, blend565(BLACK,ringCol,bright-i*3), 6)
  PortScale::map() 的 spread/blend 双线性
"""
import math, zlib, struct

L = 466
P = 360
CX = CCY = 233


def blend565(a, b, t):
    """固件 blend565：t=0 取 a，t=255 取 b"""
    t = max(0, min(255, t))
    ar, ag, ab = (a >> 11) & 0x1F, (a >> 5) & 0x3F, a & 0x1F
    br, bg, bb = (b >> 11) & 0x1F, (b >> 5) & 0x3F, b & 0x1F
    return (((ar + (br - ar) * t // 255) << 11) |
            ((ag + (bg - ag) * t // 255) << 5) |
            (ab + (bb - ab) * t // 255))


def px565(c):
    r = (c >> 11) & 0x1F
    g = (c >> 5) & 0x3F
    b = c & 0x1F
    return (r * 255 // 31, g * 255 // 63, b * 255 // 31)


class Canvas:
    def __init__(self, w, h, fill=0):
        self.w, self.h = w, h
        self.buf = bytearray(w * h * 3)
        r, g, b = px565(fill)
        for i in range(w * h):
            self.buf[i * 3:i * 3 + 3] = bytes((r, g, b))

    def set(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            r, g, b = px565(c)
            o = (y * self.w + x) * 3
            self.buf[o:o + 3] = bytes((r, g, b))

    def line(self, x0, y0, x1, y1, c):
        """Bresenham，和 GFX 库的 drawLine 等价"""
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx + dy
        while True:
            self.set(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def png(self, path):
        raw = b"".join(b"\x00" + bytes(self.buf[y * self.w * 3:(y + 1) * self.w * 3]) for y in range(self.h))

        def chunk(t, d):
            c = t + d
            return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)
        data = (b"\x89PNG\r\n\x1a\n" +
                chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
        open(path, "wb").write(data)


def imu_arc_seg(cv, cx, cy, r, a0, a1, c, step=3.0):
    D = math.pi / 180.0
    pxf = cx + math.cos(a0 * D) * r
    pyf = cy + math.sin(a0 * D) * r
    a = a0
    while a < a1 - 0.01:
        na = a + step
        if na > a1:
            na = a1
        nxf = cx + math.cos(na * D) * r
        nyf = cy + math.sin(na * D) * r
        cv.line(int(pxf), int(pyf), int(nxf), int(nyf), c)
        pxf, pyf, a = nxf, nyf, na


def draw_ring(cv, mag, thick_span=34, notch=(104, 436), step=6.0, r_out=229):
    gk = min(1.0, mag / 1.5)
    thick = 10 + int(gk * gk * thick_span + 0.5)
    bright = 140 + int(gk * 115 + 0.5)
    ring_col = 0xF800  # 红
    for i in range(thick):
        imu_arc_seg(cv, CX, CCY, r_out - i, notch[0], notch[1],
                    blend565(0x0000, ring_col, max(40, bright - i * 3)), step)
    return thick


def spread(v):
    return (v << 5) | (v >> 10)


def downscale(src, notch_probe=None):
    """PortScale::map 的双线性缩放（LOGICAL 466 -> PHYSICAL 360）"""
    out = Canvas(P, P, 0)
    for py in range(P):
        for px_ in range(P):
            # 与 PortScale 一致的映射
            sx = ((2 * px_ + 1) * L) // (2 * P)
            sy = ((2 * py + 1) * L) // (2 * P)
            if sx >= L - 1 or sy >= L - 1:
                o = (sy * L + min(sx, L - 1)) * 3
                out.buf[(py * P + px_) * 3:(py * P + px_) * 3 + 3] = src.buf[o:o + 3]
                continue
            a = (sy * L + sx) * 3
            b = a + 3
            cc = a + L * 3
            dd = cc + 3
            for k in range(3):
                top = (src.buf[a + k] + src.buf[b + k]) // 2
                bot = (src.buf[cc + k] + src.buf[dd + k]) // 2
                out.buf[(py * P + px_) * 3 + k] = (top + bot) // 2
    return out


if __name__ == "__main__":
    for name, mag, span, notch, step in [
        ("ring_g03", 0.03, 34, (104, 436), 6.0),
        ("ring_g12", 1.20, 34, (104, 436), 6.0),
        ("ring_full_notch", 1.20, 34, (0, 360), 6.0),
    ]:
        cv = Canvas(L, L, 0x0000)
        th = draw_ring(cv, mag, span, notch, step)
        cv.png(f"_logical_{name}.png")
        downscale(cv).png(f"_panel_{name}.png")
        print(name, "thick=", th)

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MotoNav 模拟器演示脚本：通过 UDP 7700/7777 端口注入手机协议数据，
驱动真实固件渲染完整导航会话。用法：

    # 先启动模拟器（build/motonav_sim.exe）
    python demo_drive.py                # 全流程演示 + 截图到 shots/
    python demo_drive.py --phase nav    # 只跑某一阶段（connect/weather/music/nav/lanes/night/map）

依赖：pip install pillow（中文路名位图需要；没有 PIL 则跳过文字图片）
"""
import socket
import struct
import sys
import time
import os

SIM = ("127.0.0.1", 7777)
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "shots")
os.makedirs(OUT, exist_ok=True)

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

_img_id = 0


def send(data):
    if isinstance(data, str):
        data = data.encode()
    sock.sendto(data, SIM)


def bmp_to_png(bmp_path, png_path):
    import zlib
    data = open(bmp_path, "rb").read()
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    rowBytes = (w * 3 + 3) & ~3
    rows = []
    for y in range(h - 1, -1, -1):
        base = off + y * rowBytes
        row = data[base:base + rowBytes]
        rows.append(b"".join(row[x * 3 + 2:x * 3 + 3] + row[x * 3 + 1:x * 3 + 2] + row[x * 3:x * 3 + 1]
                             for x in range(w)))

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(b"".join(b"\x00" + r for r in rows), 9))
    png += chunk(b"IEND", b"")
    open(png_path, "wb").write(png)


def shot(name):
    send(b"!shot")
    time.sleep(0.4)
    bmp = os.path.join(os.path.dirname(OUT), "..", "build", "screenshot.bmp")
    if os.path.exists(bmp):
        bmp_to_png(bmp, os.path.join(OUT, name + ".png"))
        print(f"  [shot] {name}.png")
    else:
        print("  [shot] screenshot.bmp not found (simulator running?)")


# ---------------- 文字图片（'B' 协议，4 位灰度） ----------------
def textimg(slot, img):
    """img: PIL L 模式图像（灰度）。按协议分包发送。"""
    global _img_id
    _img_id += 1
    w, h = img.size
    raw = bytearray(((w + 1) // 2) * h)
    for y in range(h):
        for x in range(0, w, 2):
            hi = img.getpixel((x, y)) >> 4
            lo = (img.getpixel((x + 1, y)) >> 4) if x + 1 < w else 0
            raw[y * ((w + 1) // 2) + x // 2] = (hi << 4) | lo
    total = len(raw)
    parts = (total + 400 - 1) // 400 or 1
    for p in range(parts):
        off = p * 400
        chunkd = raw[off:off + 400]
        hdr = b"B" + bytes([slot, _img_id & 0xFF, p, parts,
                            w & 0xFF, (w >> 8) & 0xFF, h & 0xFF,
                            off & 0xFF, (off >> 8) & 0xFF])
        send(hdr + bytes(chunkd))
        time.sleep(0.02)


def render_text(text, width, height, font_size=36, font="msyh.ttc"):
    """用 Windows 中文字体渲染一行文字到灰度图（白字透明底）"""
    from PIL import Image, ImageDraw, ImageFont
    img = Image.new("L", (width, height), 0)
    d = ImageDraw.Draw(img)
    try:
        f = ImageFont.truetype(f"C:/Windows/Fonts/{font}", font_size)
    except OSError:
        print("  [warn] 中文字体不可用，跳过文字图片")
        return None
    box = d.textbbox((0, 0), text, font=f)
    d.text(((width - (box[2] - box[0])) // 2 - box[0], (height - (box[3] - box[1])) // 2 - box[1]),
           text, fill=255, font=f)
    return img


# ---------------- 演示阶段 ----------------
def phase_connect():
    print("[1] 连接（画首页 ready）")
    send(b"!conn 1")
    time.sleep(1.5)
    shot("01_home_ready")


def phase_weather():
    print("[2] 天气页")
    # #W,图标,温度,时间,H/h,条数, [星期,图标,高温,低温]...
    send(b"#W,1,23,14:30,H,6,"
         b"1,0,28,21, 1,3,30,22, 2,1,26,20, 3,2,25,19, 4,0,27,20, 5,3,29,22")
    send("#WX,湿度62%,东南风3级,AQI 45 优")
    slot3 = render_text("杭州市", 200, 44, 34)
    if slot3:
        textimg(3, slot3)
    slot4 = render_text("湿度62% 风力3", 260, 36, 28)
    if slot4:
        textimg(4, slot4)
    # 滑到天气页（主页 → 右滑两次）
    for _ in range(2):
        send(b"")  # 触摸合成走键盘/鼠标，这里直接靠 #t 不可行；用位置流保持导航关
        time.sleep(0.3)
    time.sleep(1.0)
    shot("02_weather")


def phase_music():
    print("[3] 音乐页")
    send(b"#Z,1,1,7")
    s5 = render_text("公路之歌", 240, 44, 34)
    if s5:
        textimg(5, s5)
    s6 = render_text("痛仰乐队", 240, 36, 28)
    if s6:
        textimg(6, s6)
    time.sleep(1.0)


def phase_nav():
    print("[4] 导航 HUD：右转 + 车速 + 行程 + 测速")
    send(b"!conn 1")
    send(b"C")                       # 回到待机
    time.sleep(0.5)
    send(b"R,180")                   # 前方 180m 右转
    send(b"#e,12300,1432,8")         # 剩余 12.3km，14:32 到达，8 个红绿灯
    send(b"#k,80,300")               # 前方 300m 测速，限速 80
    send(b"#p,1,10000,20000,90,66")  # 位置流（速度 66km/h）
    send(b"#v,66")
    s0 = render_text("文一西路", 260, 56, 44)
    if s0:
        textimg(0, s0)               # 路名（栏位 0）
    s1 = render_text("12.3km · 14:32 · 红绿灯8", 320, 40, 30)
    if s1:
        textimg(1, s1)               # 底部信息（栏位 1）
    time.sleep(1.0)
    shot("03_nav_arrow")

    print("[5] 车道线 + 电子眼 + 拥堵环")
    send(b"#j,0300F0")               # 3 条车道：直行/直行/右转(不推荐)
    send(b"#c,80,300,60,120,1,220,150,4")  # 电子眼位置
    send(b"#TR,4,600,400")           # 前方严重拥堵
    time.sleep(1.0)
    shot("04_nav_lanes")

    print("[6] 夜间配色")
    send(b"#n,1")
    time.sleep(1.0)
    shot("05_nav_night")
    send(b"#n,0")

    print("[7] 转弯临近（左转 + 路口角度）")
    send(b"L,80,-135")
    send(b"#v,32")
    time.sleep(1.2)
    shot("06_nav_left")

    print("[8] 直行巡航")
    send(b"S,800")
    send(b"#v,72")
    send(b"#j")
    send(b"#TR")
    time.sleep(1.0)
    shot("07_cruise")


PHASES = {
    "connect": phase_connect,
    "weather": phase_weather,
    "music": phase_music,
    "nav": phase_nav,
}


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "all"
    print(f"MotoNav 演示 → UDP {SIM[0]}:{SIM[1]}，截图输出 {OUT}")
    if arg == "all":
        phase_connect()
        phase_weather()
        phase_music()
        phase_nav()
    elif arg in PHASES:
        PHASES[arg]()
    else:
        print(f"未知阶段 {arg}，可选：all / {' / '.join(PHASES)}")
        return
    print("完成。")


if __name__ == "__main__":
    main()

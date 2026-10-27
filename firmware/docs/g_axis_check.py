# -*- coding: utf-8 -*-
"""
车身坐标映射的验证脚本（v7.14.3）。

真机安装（185B，屏幕朝上、屏幕顶边朝车头）：IMU X=车身右、Y=车身前、Z=车身上。
旧代码按 X=前 写的，于是：
  lonNow = ax = 车身"右"分量 ≈ 0        → 纵向加速度没被减掉
  latSq  = |f|²-1-lon² = A²             → 纵向加速度被当成横向，而且开平方永远为正
  imuLeanSign 靠 gyro X（=俯仰率≈0）学   → 永远停在 +1
结果：球只往 RIGHT 走，前进后退都一样 —— 就是用户报的现象。

物理模型（比力 f = a - g，单位 g）：
  稳态压弯 φ（右压为正）时加速度是**水平**的（指向弯心，大小 tanφ），不是沿车身横轴，
  所以世界系里 f = (tanφ, A, 1)；投影到车身轴后 aR≡0、aF=A、aU=1/cosφ。
  这正说明稳态压弯时加速度计拿不到倾角，只能靠 |f| = 1/cosφ 反解。
"""
import math

D2R = math.pi / 180.0
R2D = 180.0 / math.pi


def body_accel(phi_deg, lon_A, pitch_deg=0.0, lat_world=None):
    """返回真实的车身坐标比力 (aR, aF, aU)。
    安装为 R=X, F=Y, U=Z，所以原始读数就等于车身分量。

    lat_world：世界系水平横向加速度。默认 None = 稳态压弯，取 tanφ（加速度是水平的、
               指向弯心）。静止倾斜时传 0.0 —— 车身歪着，但没有任何横向加速度，
               这时 |f| 仍然是 1g，这是 v7.14.3 之前 latA 恒为 0 的根源。"""
    phi, th = phi_deg * D2R, pitch_deg * D2R
    lat_w = math.tan(phi) if lat_world is None else lat_world
    fw = (lat_w, lon_A, 1.0)
    # 机身轴在世界系里的方向：先俯仰（绕 E），再横滚（绕新的前向轴）
    Fw = (0.0, math.cos(th), math.sin(th))
    Uw = (0.0, -math.sin(th), math.cos(th))
    # 绕 Fw 横滚 φ：右手定则，+φ 让 U 倒向右侧
    def rot_about(v, axis, ang):
        c, s = math.cos(ang), math.sin(ang)
        k = axis
        dot = sum(v[i] * k[i] for i in range(3))
        cross = (k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0])
        return tuple(v[i] * c + cross[i] * s + k[i] * dot * (1 - c) for i in range(3))
    Rw = rot_about((1.0, 0.0, 0.0), Fw, phi)
    Uw = rot_about(Uw, Fw, phi)
    aR = sum(fw[i] * Rw[i] for i in range(3))
    aF = sum(fw[i] * Fw[i] for i in range(3))
    aU = sum(fw[i] * Uw[i] for i in range(3))
    return aR, aF, aU


def new_algo(ax, ay, az, roll, pitch, v=7144):
    """v7.14.3 / v7.14.4：先换成车身坐标再算。
    v7143：wA 只看 |lonNow|（静止倾斜时 latA=0 且 wA=1 → 球不动，就是这次的 bug）
    v7144：wA 改成看 |f|-1，静止倾斜时切到 latB = tan(roll)"""
    aR, aF, aU = ax, ay, az                       # R=X, F=Y, U=Z
    g = math.sqrt(ax * ax + ay * ay + az * az)
    th = pitch * D2R
    lonNow = aF * math.cos(th) - aU * math.sin(th)
    latSq = g * g - 1.0 - lonNow * lonNow
    latA = math.sqrt(latSq) if latSq > 0 else 0.0
    latB = math.tan(max(-1.1, min(1.1, roll * D2R)))
    gate_lon = max(0.0, min(1.0, 1.0 - (abs(lonNow) - 0.20) / 0.25))
    if v == 7143:
        wA = gate_lon
    else:
        wA = max(0.0, min(1.0, (g - 1.0) / 0.03)) * gate_lon
    bTrust = max(0.0, min(1.0, (abs(roll) - 1.0) / 2.0))     # 3° 以下降权，防直线微摆抖动
    lat = wA * latA + (1.0 - wA) * abs(latB) * bTrust
    sign = 1.0 if roll >= 0 else -1.0
    return lat * sign, lonNow


def old_algo(ax, ay, az, roll, pitch):
    """v7.14.2：按 X=前 的假设"""
    g = math.sqrt(ax * ax + ay * ay + az * az)
    th = pitch * D2R
    lonNow = ax * math.cos(th) + az * math.sin(th)
    latSq = g * g - 1.0 - lonNow * lonNow
    lat = math.sqrt(latSq) if latSq > 0 else 0.0
    sign = 1.0 if roll >= 0 else -1.0          # 真机上永远学不到 -1
    return lat * sign, lonNow


def dot(lat, lon):
    """屏幕上球的方向：+x 向右（RIGHT），-y 向上（FRONT）"""
    def arrow(x, y):
        a = math.degrees(math.atan2(y, x))
        if math.hypot(x, y) < 0.02:
            return "不动"
        for lo, hi, name in [(-22.5, 22.5, "RIGHT"), (22.5, 67.5, "右上"), (67.5, 112.5, "FRONT"),
                             (112.5, 157.5, "左上"), (157.5, 180.01, "LEFT"), (-180, -157.5, "LEFT"),
                             (-157.5, -112.5, "左下"), (-112.5, -67.5, "REAR"), (-67.5, -22.5, "右下")]:
            if lo <= a < hi:
                return name
        return "?"
    return arrow(lat, lon)


print("=" * 78)
print("A. 直线加减速（直立，真值：只有纵向 G，球应上下走）")
print(f"{'工况':>14} {'真值纵向':>9} {'旧:lat/lon':>16} {'旧球':>7}   {'新:lat/lon':>16} {'新球':>7}")
for name, A in [("加速 +0.30g", 0.30), ("匀速 0", 0.0), ("刹车 -0.30g", -0.30), ("急刹 -0.60g", -0.60)]:
    aR, aF, aU = body_accel(0, A)
    ol, olo = old_algo(aR, aF, aU, 0, 0)
    nl, nlo = new_algo(aR, aF, aU, 0, 0)
    print(f"{name:>14} {A:+9.2f} {ol:+8.2f}/{olo:+6.2f} {dot(ol, olo):>7}   "
          f"{nl:+8.2f}/{nlo:+6.2f} {dot(nl, nlo):>7}")

print("\n" + "=" * 78)
print("B. 稳态压弯（真值侧向 G = tan φ，右压为正）")
print("（旧代码的倾角符号靠 gyro X 学，而 X 是俯仰轴 → 永远停在 +1，所以左弯也往右跑）")
print(f"{'倾角':>8} {'真值':>7} {'旧:符号锁死':>11} {'旧球':>7}   {'新:lat':>8} {'新球':>7}")
for phi in (-45, -30, -15, 0, 15, 30, 45, 55):
    aR, aF, aU = body_accel(phi, 0.0)
    truth = math.tan(phi * D2R)
    ol_stuck, _ = old_algo(aR, aF, aU, 0.0, 0)      # 真机：roll 恒为 0 → sign 恒 +1
    nl, _ = new_algo(aR, aF, aU, phi, 0)
    print(f"{phi:+6d}° {truth:+7.2f} {ol_stuck:+11.2f} {dot(ol_stuck, 0):>7}   {nl:+8.2f} {dot(nl, 0):>7}")

print("\n" + "=" * 78)
print("C. 压弯 + 加速出弯（45°，+0.2g）：球应在右上方")
aR, aF, aU = body_accel(45, 0.2)
ol, olo = old_algo(aR, aF, aU, 45, 0)
nl, nlo = new_algo(aR, aF, aU, 45, 0)
print(f"   旧: lat={ol:+.2f} lon={olo:+.2f} → {dot(ol, olo)}")
print(f"   新: lat={nl:+.2f} lon={nlo:+.2f} → {dot(nl, nlo)}   (真值 lat=1.00 lon=0.20)")

print("\n" + "=" * 78)
print("D. 安装俯仰 8° 静止（真值都是 0）")
aR, aF, aU = body_accel(0, 0.0, pitch_deg=8)
for tag, fn in (("旧", old_algo), ("新", new_algo)):
    l, lo = fn(aR, aF, aU, 0, 8)
    print(f"   {tag}: lat={l:+.3f} lon={lo:+.3f}")


print("\n" + "=" * 78)
print("E. 静止倾斜（车身歪着但没有横向加速度，|f| 恒为 1g）")
print("   —— 物理上侧向 G 确实应该是 0，但用户侧倾车身时球必须跟着动")
print(f"{'倾角':>8} {'latA':>7} {'latB':>7} {'v7.14.3 球':>11} {'v7.14.4 球':>11}")
for phi in (-45, -30, -15, 0, 15, 30, 45, 55):
    aR, aF, aU = body_accel(phi, 0.0, lat_world=0.0)
    g = math.sqrt(aR * aR + aF * aF + aU * aU)
    latSq = g * g - 1.0
    latA = math.sqrt(latSq) if latSq > 0 else 0.0
    latB = math.tan(phi * D2R)
    o3, _ = new_algo(aR, aF, aU, phi, 0, v=7143)
    o4, _ = new_algo(aR, aF, aU, phi, 0, v=7144)
    print(f"{phi:+6d}° {latA:7.3f} {latB:7.3f} {dot(o3, 0):>11} {dot(o4, 0):>11}")

print("\n   v7.14.4 的数值（应与 tan φ 一致）：")
for phi in (-45, 0, 30, 45):
    aR, aF, aU = body_accel(phi, 0.0, lat_world=0.0)
    o4, _ = new_algo(aR, aF, aU, phi, 0, v=7144)
    print(f"     {phi:+4d}°  球 lat={o4:+.3f}   tanφ={math.tan(phi*D2R):+.3f}")


print("\n" + "=" * 78)
print("F. v7.14.6 还原成基线的「去重力投影」（绘制层反旋 roll/pitch）")
print("   lon = aF·cosθ − aU·sinθ   up = aF·sinθ + aU·cosθ   lat = aR·cosφ + up·sinφ")

def v7146(aR, aF, aU, roll, pitch):
    ph, th = roll * D2R, pitch * D2R
    lon = aF * math.cos(th) - aU * math.sin(th)
    up = aF * math.sin(th) + aU * math.cos(th)
    lat = aR * math.cos(ph) + up * math.sin(ph)
    return lat, lon

print("\n  A) 直线加减速（直立）—— 应只走 FRONT/REAR")
for name, A in [("加速+0.30", 0.30), ("刹车-0.30", -0.30)]:
    aR, aF, aU = body_accel(0, A)
    l, lo = v7146(aR, aF, aU, 0, 0)
    print(f"     {name:>10}  lat={l:+.2f} lon={lo:+.2f} → {dot(l, lo)}")

print("\n  B) 稳态压弯 —— 应等于 tanφ")
for phi in (-45, -30, 0, 30, 45):
    aR, aF, aU = body_accel(phi, 0.0)
    l, lo = v7146(aR, aF, aU, phi, 0)
    print(f"     {phi:+4d}°  lat={l:+.3f} (tanφ={math.tan(phi*D2R):+.3f}) lon={lo:+.2f} → {dot(l, lo)}")

print("\n  C) 静止倾斜（无横向加速度）—— 应为 0")
for phi in (-45, -30, 30, 45):
    aR, aF, aU = body_accel(phi, 0.0, lat_world=0.0)
    l, lo = v7146(aR, aF, aU, phi, 0)
    print(f"     {phi:+4d}°  lat={l:+.3f} lon={lo:+.3f} → {dot(l, lo)}")

print("\n  D) 安装俯仰 8° 静止 —— 应为 0")
aR, aF, aU = body_accel(0, 0.0, pitch_deg=8)
l, lo = v7146(aR, aF, aU, 0, 8)
print(f"     lat={l:+.3f} lon={lo:+.3f}")

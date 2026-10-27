# -*- coding: utf-8 -*-
"""
G 值算法对比仿真（v7.13.8 旧算法 vs v7.14.0 新算法）

传感器模型取自 d3sd1/gnss-imu-lean-angle-lstm 的 simulate.py（Sharp 侧倾动力学论文配套代码）：
    a_cent = v^2 * tan(phi) / L                 # 向心加速度（世界系水平）
    a_y    = a_cent * cos(phi) - g * sin(phi)   # 车身系
    a_z    = a_cent * sin(phi) + g * cos(phi)
稳态压弯时 a_cent = g*tan(phi)，代入得 a_y ≡ 0、a_z = g/cos(phi)。
即：加速度计在稳态压弯时读不到任何倾角信息，只有合力大小 |f| = g/cos(phi) 有用。
"""

import math
import random

G = 9.80665
D2R = math.pi / 180.0
R2D = 180.0 / math.pi

# ---------------- 场景：一串弯 + 刹车 ----------------
# (起始时刻 s, 结束时刻 s, 目标倾角 deg)
SEGMENTS = [
    (0.0, 3.0, 0.0),
    (3.0, 11.0, 15.0),      # 小弯
    (11.0, 13.0, 0.0),
    (13.0, 21.0, 30.0),     # 中等
    (21.0, 23.0, 0.0),
    (23.0, 31.0, 45.0),     # 大弯
    (31.0, 33.0, 0.0),
    (33.0, 41.0, 55.0),     # 赛道级
    (41.0, 43.0, 0.0),
    (43.0, 46.0, 0.0),      # 直线急刹
]
BRAKE = (43.0, 44.2, -1.0)  # 43.0~44.2s 刹车 1.0g


def lean_target(t):
    """目标倾角（阶跃），实际倾角再走一阶过渡，这样倾角速度是连续的。"""
    phi = 0.0
    for t0, t1, target in SEGMENTS:
        if t < t0:
            break
        phi = target
    return phi


def lon_profile(t):
    t0, t1, a = BRAKE
    if t0 <= t <= t1:
        ramp = min(1.0, (t - t0) / 0.15) * min(1.0, (t1 - t) / 0.15)
        return a * max(0.0, ramp)
    return 0.0


def make_data(dt, seed=7):
    rng = random.Random(seed)
    n = int(46.0 / dt)
    out = []
    phi_lag = 0.0          # 一阶过渡后的实际倾角（deg）
    tau_roll = 0.55        # turn-in 时间常数
    for i in range(n):
        t = i * dt
        target = lean_target(t)
        phi_prev = phi_lag
        phi_lag += (target - phi_lag) * (dt / (tau_roll + dt))
        roll_rate = (phi_lag - phi_prev) / dt          # 陀螺测的就是这个

        phi = phi_lag * D2R
        lon = lon_profile(t)
        a_cent = G * math.tan(phi) + lon * G * math.sin(phi)
        ay = a_cent * math.cos(phi) - G * math.sin(phi)
        az = a_cent * math.sin(phi) + G * math.cos(phi)
        ax = lon * G

        # 传感器噪声：QMI8658 @ ±8g，噪声约 3~5mg（经片上 LPF 后更低）
        nay = ay / G + rng.gauss(0, 0.004)
        naz = az / G + rng.gauss(0, 0.004)
        nax = ax / G + rng.gauss(0, 0.004)
        nwx = roll_rate + rng.gauss(0, 0.06)      # dps
        out.append((t, phi_lag, nax, nay, naz, nwx))
    return out


# ---------------- 旧算法（v7.13.8） ----------------
def run_old(data, loop_hz=500.0, draw_hz=30.0, bias_dps=0.35):
    """imuTick 跑在 loop_hz（远高于 112Hz ODR），滤波系数按每次调用给；drawImuPage 在 30Hz 反旋。"""
    dt_loop = 1.0 / loop_hz
    dt_draw = 1.0 / draw_hz
    roll = 0.0
    sLat = sLon = 0.0
    next_draw = 0.0
    out = []
    started = False
    for (t, phi_true, ax, ay, az, wx) in data:
        gmag = math.sqrt(ax * ax + ay * ay + az * az)
        if not started:
            roll = math.atan2(ay, az) * R2D
            started = True
        # 旧代码：0.04 的加速度权重，每次 loop 调用都执行
        accelWeight = 0.04 if abs(gmag - 1.0) < 0.20 else 0.0
        gyroWeight = 1.0 - accelWeight
        arCorr = math.atan2(ay, az) * R2D
        latAcc = ay * math.cos(roll * D2R) - az * math.sin(roll * D2R)
        if abs(latAcc) > 0.015:
            arCorr = arCorr + math.atan2(-latAcc, abs(az)) * R2D
        roll = gyroWeight * (roll + (wx - bias_dps) * dt_loop) + accelWeight * arCorr

        if t >= next_draw:
            next_draw += dt_draw
            th = 0.0
            xLon = ax * math.cos(th) + az * math.sin(th)
            z1 = -ax * math.sin(th) + az * math.cos(th)
            yLat = ay * math.cos(roll * D2R) - z1 * math.sin(roll * D2R)
            sLat += (yLat - sLat) * 0.35
            sLon += (xLon - sLon) * 0.35
            out.append((t, phi_true, roll, math.hypot(sLat, sLon)))
    return out


# ---------------- 新算法（v7.14.0） ----------------
def run_new(data, tick_hz=125.0, bias_dps=0.35):
    dt = 1.0 / tick_hz
    roll = 0.0
    lat = lon_s = 0.0
    sign = 1.0
    started = False
    out = []
    for (t, phi_true, ax, ay, az, wx) in data:
        gmag = math.sqrt(ax * ax + ay * ay + az * az)
        if not started:
            roll = math.atan2(ay, az) * R2D
            sign = 1.0 if roll >= 0 else -1.0
            started = True
        th_abs = 0.0
        lonNow = ax * math.cos(th_abs) + az * math.sin(th_abs)
        latSq = gmag * gmag - 1.0 - lonNow * lonNow
        dev = abs(gmag - 1.0)

        roll += (wx - bias_dps) * dt
        if abs(roll) > 3.0:
            sign = 1.0 if roll > 0 else -1.0
        latMag = math.degrees(math.atan(math.sqrt(latSq))) if latSq > 0 else 0.0
        # 观测 A：|f|≈1 时 atan2(ay,az) 就是真倾角（零漂）—— v7.14.1 新增
        w1 = min(1.0, max(0.0, 1.0 - (dev - 0.004) / 0.010))
        # 观测 B：|f|=1/cos φ 反解 |φ|，双重门限
        w2 = min(1.0, max(0.0, (latMag - 8.0) / 10.0))
        w2 *= min(1.0, max(0.0, (dev - 0.010) / 0.006))
        w2 *= min(1.0, max(0.0, 1.0 - (abs(lonNow) - 0.20) / 0.25))
        obs = 0.0
        wsum = 0.0
        if w1 > 0:
            obs += w1 * (math.atan2(ay, az) * R2D)
            wsum += w1
        if w2 > 0:
            obs += w2 * sign * latMag
            wsum += w2
        if wsum > 0.001:
            roll += (obs / wsum - roll) * (dt / (0.8 + dt)) * min(1.0, wsum)

        latA = math.sqrt(latSq) if latSq > 0 else 0.0
        latB = math.tan(max(-1.1, min(1.1, roll * D2R)))
        wA = min(1.0, max(0.0, 1.0 - (abs(lonNow) - 0.20) / 0.25))
        latRaw = (wA * latA + (1 - wA) * abs(latB)) * sign
        kd = dt / (0.09 + dt)
        lat += (latRaw - lat) * kd
        lon_s += (lonNow - lon_s) * kd
        out.append((t, phi_true, roll, math.hypot(lat, lon_s)))
    return out


def run_stationary(bias_true_dps=2.5, secs=120.0, seed=3, mode="v7141", roll0=0.0, acc_scale=1.0):
    """静止放在桌上：真实倾角 0，陀螺带一个明显的零偏。看 LEAN 会不会一路涨。

    mode="v7140"：复刻 v7.14.0 的判据（|gyro|<2dps 才算静止），零偏 2.5dps 时永远判不成静止
                  → 零偏学不到 → roll 纯积分，一路涨。
    mode="v7141"：ZUPT（看陀螺方差）+ |f|≈1 时用 atan2(ay,az) 当真倾角观测。
    mode="v7142"：在 v7.14.1 基础上，静止判定成立时无条件快速钉到 atan2(ay,az)。

    roll0    ：起始姿态误差（模拟"骑完停下车，姿态还带着残余误差"）
    acc_scale：加速度计刻度误差。未校准的 MEMS 2% 很常见 —— 这时 |f|-1≈0.02 会落进
               0.014~0.04 的缝里（imuStill 判成静止、但 w1 已经衰减到 0）。
    """
    rng = random.Random(seed)
    dt = 1.0 / 125.0
    n = int(secs / dt)
    roll = roll0
    bias = 0.0
    avg = var = 0.0
    cal = 0
    marks = {}
    next_mark = 1.0
    for i in range(n):
        t = i * dt
        ax = rng.gauss(0, 0.004) * acc_scale
        ay = rng.gauss(0, 0.004) * acc_scale
        az = (1.0 + rng.gauss(0, 0.004)) * acc_scale
        wx = bias_true_dps + rng.gauss(0, 0.06)
        gmag = math.sqrt(ax * ax + ay * ay + az * az)

        ka = dt / (0.5 + dt)
        d = wx - avg
        avg += d * ka
        var += (d * d - var) * ka

        if mode == "v7140":
            still = abs(gmag - 1.0) < 0.05 and abs(wx) < 2.0 and abs(wx) < 2.0
            if still:
                bias += (wx - bias) * (dt / (8.0 + dt))
        else:
            still = (t > 0.8) and abs(gmag - 1.0) < 0.04 and var < 0.36
            if still:
                kb = dt / (0.25 + dt) if cal < 200 else dt / (3.0 + dt)
                bias += (avg - bias) * kb
                cal += 1

        roll += (wx - bias) * dt
        if mode == "v7141":
            dev = abs(gmag - 1.0)
            w1 = min(1.0, max(0.0, 1.0 - (dev - 0.004) / 0.010))
            if w1 > 0:
                roll += (math.atan2(ay, az) * R2D - roll) * (dt / (0.8 + dt)) * w1
        elif mode == "v7142":
            # 静止判定成立就无条件、快速钉到 atan2(ay,az)
            if still:
                roll += (math.atan2(ay, az) * R2D - roll) * (dt / (0.10 + dt))
            else:
                dev = abs(gmag - 1.0)
                w1 = min(1.0, max(0.0, 1.0 - (dev - 0.004) / 0.010))
                if w1 > 0:
                    roll += (math.atan2(ay, az) * R2D - roll) * (dt / (0.8 + dt)) * w1

        if t >= next_mark:
            marks[int(next_mark)] = roll
            next_mark += 1.0 if next_mark < 10 else (4.0 if next_mark < 30 else 30.0)
    return marks, bias


def report(name, rows):
    """按稳态段统计（每段跳过前 2s 的建立过程）：真实侧向 G = tan(phi)"""
    buckets = [(5.5, 10.0, 15), (15.5, 20.0, 30), (25.5, 30.0, 45), (35.5, 40.0, 55), (45.0, 46.0, 0)]
    print(f"\n=== {name} ===")
    print(f"{'场景':>10} {'真值G':>7} {'显示均值':>9} {'误差':>8} {'误差%':>8}")
    for t0, t1, deg in buckets:
        sel = [r for r in rows if t0 <= r[0] <= t1]
        if not sel:
            continue
        mean = sum(r[3] for r in sel) / len(sel)
        truth = math.tan(deg * D2R) if deg else 0.0
        err = mean - truth
        pct = (err / truth * 100) if truth else 0.0
        label = f"{deg}°弯" if deg else "直线刹停"
        print(f"{label:>10} {truth:7.2f} {mean:9.2f} {err:+8.2f} {pct:+7.0f}%")
    # 稳态段内的抖动（峰峰值）
    for t0, t1, deg in buckets[:4]:
        sel = [r[3] for r in rows if t0 <= r[0] <= t1]
        if sel:
            print(f"  {deg}° 弯内抖动 pk-pk = {max(sel) - min(sel):.3f} G")


if __name__ == "__main__":
    data = make_data(1.0 / 112.0)
    report("旧算法 v7.13.8（imuTick@500Hz，加速度权重 0.04/次）", run_old(data))
    report("新算法 v7.14.0（imuTick@125Hz，dt 归一 + |f| 反解）", run_new(data))

    print("\n=== 静止漂移测试（放在桌上 120s，陀螺零偏 2.5dps，真值 0°）===")
    cols = [1, 5, 10, 30, 60, 90]
    print(f"{'':>30} " + " ".join(f"{c:>6}s" for c in cols) + "   估计零偏")
    for label, mode in [("v7.14.0（|gyro|<2dps 判静止）", "v7140"),
                        ("v7.14.1（ZUPT + |f|≈1 观测）", "v7141"),
                        ("v7.14.2（静止时无条件钉住）", "v7142")]:
        marks, bias = run_stationary(mode=mode)
        row = " ".join(f"{marks.get(c, float('nan')):7.2f}" for c in cols)
        print(f"{label:>30} {row}   {bias:+.2f}dps")

    print("\n=== 缝里那一档：刻度误差 2% → |f|-1≈0.02（imuStill 成立但 w1=0）+ 残余 4° ===")
    print(f"{'':>30} " + " ".join(f"{c:>6}s" for c in cols))
    for label, mode in [("v7.14.1", "v7141"), ("v7.14.2", "v7142")]:
        marks, bias = run_stationary(mode=mode, acc_scale=1.02, roll0=4.0, secs=100.0)
        row = " ".join(f"{marks.get(c, float('nan')):7.2f}" for c in cols)
        print(f"{label:>30} {row}")

    print("\n=== 骑完停车（起始残余 4°）+ 陀螺零偏 2.5dps，看能不能自己收回 0 ===")
    print(f"{'':>30} " + " ".join(f"{c:>6}s" for c in cols))
    for label, mode in [("v7.14.1", "v7141"), ("v7.14.2", "v7142")]:
        marks, bias = run_stationary(mode=mode, roll0=4.0, secs=100.0)
        row = " ".join(f"{marks.get(c, float('nan')):7.2f}" for c in cols)
        print(f"{label:>30} {row}")

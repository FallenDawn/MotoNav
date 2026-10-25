// TpmsScanner.h — CSC 胎压监测传感器（SY-4300 / YC-0001）BLE 广播接收（数据层，无界面）
// 协议逆向自 com.csc.tpms App（tpms_analysis/CSC-TPMS-协议分析报告.md）：
//   传感器是纯广播型，没有 GATT 连接；广播记录按 AD 结构遍历找 type=0xFF（厂商数据）：
//     payload[0]   = flags  bit0低电压 bit1漏气 bit2充气中 bit3低压
//     payload[1]   = 电压原始值（×0.1 = V）
//     payload[2]   = 温度 ℃（无符号）
//     payload[3..4]= 压力原始值（大端 16 位）
//     压力 Bar = (raw×0.1 − 14.6) × 0.0689476（负值取 0）    电量% = clamp((V−2.0)/1.3×100)
//   YC 协议 payload 长 6，末字节 CRC8（poly 0x39 / init 0x7E / xorout 0x48），
//     输入 = [0x02,0x1B] + MAC 反序 + 广播记录[0..payload 第 5 字节]；SY-4300 长 5 无校验。
// 扫描和手机连接（外设角色）共存：NimBLE 多角色。连着手机传地图时自动降低扫描占空比，
// 少抢导航数据的无线电时间；没连手机时恢复连续扫描（同原 App 的 LOW_LATENCY）。
//
// 用法：setup() 里 BLE 广播开始后 tpmsBegin()；loop() 里 tpmsTick(连着手机与否)；
//       数据用 tpmsCount()/tpmsGet() 取，报警位用 tpmsAlarmBits() 算。画面显示后续再加。
#pragma once

#if defined(CONFIG_NIMBLE_ENABLED)

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <string.h>

// ---- 可调参数 ----
#define TPMS_MAX_SENSORS   4       // 最多记住几个传感器（两轮 + 备胎够用）
#define TPMS_DEDUP_MS      500     // 同一 MAC 500ms 内的重复广播忽略（同原 App 防抖）
#define TPMS_SCAN_INT_BUSY 200     // 连着手机：扫描间隔 ms（占空比 25%）
#define TPMS_WIN_BUSY       50
#define TPMS_SCAN_INT_IDLE 100     // 没连手机：连续扫描
#define TPMS_WIN_IDLE      100
#define TPMS_RESTART_MS  300000    // 5 分钟重启扫描并清结果表：NimBLE 结果表上限 255 台，满了会丢新设备
#define TPMS_DEBUG_MS     15000    // 每个传感器至少隔这么久才打一条串口日志

// ---- 轮位绑定（现在只有数据结构，绑定界面/设置后续做） ----
enum TpmsWheel : uint8_t {
  TPMS_WHEEL_NONE = 0, TPMS_WHEEL_FRONT, TPMS_WHEEL_REAR,
  TPMS_WHEEL_FL, TPMS_WHEEL_FR, TPMS_WHEEL_RL, TPMS_WHEEL_RR,
};

// ---- 报警位（阈值判定 + 传感器自报 flags 原样透传，UI 层决定怎么提示） ----
enum TpmsAlarm : uint16_t {
  TPMS_ALARM_LOW_PRESSURE  = 0x0001,   // 压力 < pressureLowBar（默认 1.7）
  TPMS_ALARM_HIGH_PRESSURE = 0x0002,   // 压力 > pressureHighBar（默认 3.0）
  TPMS_ALARM_HIGH_TEMP     = 0x0004,   // 温度 > tempHighC（默认 60 ℃）
  TPMS_ALARM_LOW_BATT      = 0x0008,   // 电量 < battLowPct（默认 20%）
  TPMS_SENSOR_LOW_VOLTAGE  = 0x0100,   // 以下 4 位是传感器 flags 里自报的
  TPMS_SENSOR_AIR_LEAK     = 0x0200,
  TPMS_SENSOR_PRESSURIZING = 0x0400,
  TPMS_SENSOR_LOW_PRESSURE = 0x0800,
};

struct TpmsReading {
  bool     present;                 // 槽位有效
  uint8_t  mac[6];                  // 显示序 AA:BB:CC:DD:EE:FF（mac[0]=AA）
  uint8_t  proto;                   // 1=SY 2=YC
  uint8_t  wheel;                   // TpmsWheel，默认 NONE
  uint8_t  statusFlags;             // 传感器原始 flags（bit0..3 见上）
  int8_t   rssi;
  float    voltage;                 // V
  float    pressureBar;             // Bar（内部基准单位，显示时再换算）
  int16_t  tempC;
  uint8_t  batteryPct;              // 0-100
  uint32_t lastMs;                  // 最后一次有效帧（判掉线用）
  uint32_t frames;                  // 累计有效帧数（调试）
  uint32_t dedupDropped;            // 被 500ms 防抖丢掉的帧数（调试）
};

// 报警阈值（默认值同原 App，单位 Bar / ℃ / %）
struct TpmsThresholds {
  float   pressureLowBar = 1.7f;
  float   pressureHighBar = 3.0f;
  int16_t tempHighC = 60;
  uint8_t battLowPct = 20;
};
static TpmsThresholds tpmsThresholds;

static TpmsReading tpmsDevs[TPMS_MAX_SENSORS];
static uint32_t tpmsCrcFails = 0;       // YC CRC 没过的帧数（调试）
static uint32_t tpmsRingOverflows = 0;  // 环形队列满了丢掉的帧数（调试）

// ---------- CRC8（YC 协议） ----------
static uint8_t tpmsCrc8(const uint8_t *d, size_t n) {
  uint8_t crc = 0x7E;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x39) : (uint8_t)(crc << 1);
  }
  return crc ^ 0x48;
}

// CRC 输入 = [02 1B] + MAC 反序 + 广播记录前缀。BLEAddress::getNative() 本身就是
// 低字节在前（display AA:BB:.. → native FF..AA），正好等于“MAC 反序”，直接用。
// 两种前缀都试：①整个广播记录 [0..dataStart+4]（同 Android ScanRecord.getBytes()）；
// ②只取厂商 AD 结构自身 [len 字节..dataStart+4]（厂商数据在扫描应答里时 ①可能对不上，
// 报告 8.3 的兜底）。
static bool tpmsCrcOk(const uint8_t *rec, size_t lenIdx, size_t dataStart, const uint8_t *macNative) {
  uint8_t in[8 + 62];
  in[0] = 0x02; in[1] = 0x1B;
  memcpy(in + 2, macNative, 6);
  if (dataStart + 5 <= 62) {
    memcpy(in + 8, rec, dataStart + 5);
    if (tpmsCrc8(in, 8 + dataStart + 5) == rec[dataStart + 5]) return true;
  }
  memcpy(in + 8, rec + lenIdx, 7);            // len,type + 5 个数据字节
  return tpmsCrc8(in, 8 + 7) == rec[dataStart + 5];
}

// ---------- 广播记录解析 ----------
struct TpmsFrame {
  uint8_t  flags, voltRaw, tempRaw;
  uint16_t pressRaw;
};

// 遍历 AD 结构找厂商数据（type=0xFF）。找到返回 true，输出各偏移。
static bool tpmsFindMsd(const uint8_t *rec, size_t len, size_t &lenIdx, size_t &dataStart, uint8_t &payloadLen) {
  size_t i = 0;
  while (i + 1 < len) {
    uint8_t l = rec[i];
    if (l == 0) break;                        // 结束符
    if (i + 1 + l > len) break;               // 长度越界，非法
    if (rec[i + 1] == 0xFF) {
      lenIdx = i; dataStart = i + 2; payloadLen = l - 1;
      return payloadLen >= 5 && dataStart + payloadLen <= len;
    }
    i += 1 + l;
  }
  return false;
}

// 名字判决：1=SY- 前缀 2=YC- 前缀 0=有名字但不匹配 -1=没名字（名字可能在扫描应答里没收到）
static int tpmsNameVerdict(const uint8_t *rec, size_t len) {
  int found = -1;
  size_t i = 0;
  while (i + 1 < len) {
    uint8_t l = rec[i];
    if (l == 0) break;
    if (i + 1 + l > len) break;
    uint8_t t = rec[i + 1];
    if ((t == 0x09 || t == 0x08) && l >= 4) { // 完整/短本地名
      const char *n = (const char *)&rec[i + 2];
      if (!memcmp(n, "SY-", 3)) return 1;
      if (!memcmp(n, "YC-", 3)) return 2;
      found = 0;
    }
    i += 1 + l;
  }
  return found;
}

// 没名字的 5 字节帧（SY 无 CRC）靠字段取值合理性过滤误报（报告 8.4 的退化方案）。
// 范围放得宽：漏气（压力≈0）、低电（<2V）、高温（刹车盘旁 100 ℃+）的帧都要能进来。
static bool tpmsPlausible(const TpmsFrame &f) {
  return f.voltRaw >= 15 && f.voltRaw <= 45 &&          // 1.5 ~ 4.5 V
         f.tempRaw <= 125 &&                            // ℃
         f.pressRaw >= 140 && f.pressRaw <= 900;        // ≈0（爆胎）~ 5.2 Bar
}

// 完整判决，规则同原 App + ESP 兜底。返回 0=丢弃 1=SY 2=YC（out 已填好字段）。
static uint8_t tpmsParseRecord(const uint8_t *rec, size_t len, const uint8_t *macNative, TpmsFrame &out) {
  size_t lenIdx, dataStart; uint8_t payloadLen;
  if (!tpmsFindMsd(rec, len, lenIdx, dataStart, payloadLen)) return 0;
  int verdict = tpmsNameVerdict(rec, len);
  bool proto2 = payloadLen == 6;               // 5 数据 + 1 CRC

  if (verdict == 1) {                          // SY-：SY-4300 无 CRC（payloadLen==6 时 App 也豁免）
    out.flags = rec[dataStart]; out.voltRaw = rec[dataStart + 1];
    out.tempRaw = rec[dataStart + 2];
    out.pressRaw = ((uint16_t)rec[dataStart + 3] << 8) | rec[dataStart + 4];
    return 1;
  }
  if (verdict == 2) {                          // YC-：长 6 必须 CRC；长 5 走无校验路径（同 App）
    if (proto2) {
      if (!tpmsCrcOk(rec, lenIdx, dataStart, macNative)) { tpmsCrcFails++; return 0; }
    }
    out.flags = rec[dataStart]; out.voltRaw = rec[dataStart + 1];
    out.tempRaw = rec[dataStart + 2];
    out.pressRaw = ((uint16_t)rec[dataStart + 3] << 8) | rec[dataStart + 4];
    return 2;
  }
  if (verdict == 0) return 0;                  // 别的设备，丢弃

  // 没拿到名字：长 6 靠 CRC 过滤误报（YC）；长 5 只能靠合理性（SY）
  out.flags = rec[dataStart]; out.voltRaw = rec[dataStart + 1];
  out.tempRaw = rec[dataStart + 2];
  out.pressRaw = ((uint16_t)rec[dataStart + 3] << 8) | rec[dataStart + 4];
  if (proto2) {
    if (!tpmsCrcOk(rec, lenIdx, dataStart, macNative)) { tpmsCrcFails++; return 0; }
    return 2;
  }
  return tpmsPlausible(out) ? 1 : 0;
}

// ---------- 扫描回调（NimBLE host 任务里跑，只做拷贝进环形队列，重活留给 tpmsTick） ----------
struct TpmsRingItem {
  uint8_t mac[6];                              // 显示序
  int8_t  rssi;
  uint8_t len;
  uint8_t rec[62];
};
#define TPMS_RING_SLOTS 8
static TpmsRingItem tpmsRing[TPMS_RING_SLOTS];
static volatile uint8_t tpmsRingHead = 0, tpmsRingTail = 0;
static portMUX_TYPE tpmsMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t tpmsLastAdvMs = 0;    // 最近一次广播回调（健康检查用）

class TpmsScanCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    tpmsLastAdvMs = millis();
    uint8_t *p = dev.getPayload();
    size_t n = dev.getPayloadLength();         // ADV + 扫描应答已合并，同 Android 语义
    if (!p || n == 0) return;
    if (n > 62) n = 62;
    BLEAddress addr = dev.getAddress();
    const uint8_t *nat = addr.getNative();     // 低字节在前 = 显示序的反序
    portENTER_CRITICAL(&tpmsMux);
    uint8_t next = (tpmsRingHead + 1) % TPMS_RING_SLOTS;
    if (next != tpmsRingTail) {
      TpmsRingItem &it = tpmsRing[tpmsRingHead];
      memcpy(it.rec, p, n);
      it.len = n; it.rssi = (int8_t)dev.getRSSI();
      for (int k = 0; k < 6; k++) it.mac[k] = nat[5 - k];
      tpmsRingHead = next;
    } else {
      tpmsRingOverflows++;
    }
    portEXIT_CRITICAL(&tpmsMux);
  }
};

// ---------- 数据表 ----------
static __attribute__((unused)) uint8_t tpmsCount() {
  uint8_t c = 0;
  for (int i = 0; i < TPMS_MAX_SENSORS; i++) if (tpmsDevs[i].present) c++;
  return c;
}
static __attribute__((unused)) TpmsReading *tpmsGet(int i) { return (i >= 0 && i < TPMS_MAX_SENSORS) ? &tpmsDevs[i] : nullptr; }
static int tpmsFindByMac(const uint8_t mac[6]) {
  for (int i = 0; i < TPMS_MAX_SENSORS; i++)
    if (tpmsDevs[i].present && !memcmp(tpmsDevs[i].mac, mac, 6)) return i;
  return -1;
}
static __attribute__((unused)) TpmsReading *tpmsFindByWheel(uint8_t wheel) {
  for (int i = 0; i < TPMS_MAX_SENSORS; i++)
    if (tpmsDevs[i].present && tpmsDevs[i].wheel == wheel) return &tpmsDevs[i];
  return nullptr;
}
static __attribute__((unused)) void tpmsSetWheel(const uint8_t mac[6], uint8_t wheel) {
  int i = tpmsFindByMac(mac);
  if (i >= 0) tpmsDevs[i].wheel = wheel;
}
// 最近 ms 内有帧才算在线（传感器几秒广播一次，15 秒没动静基本就是走了/没电了）
static __attribute__((unused)) bool tpmsFresh(const TpmsReading *d, uint32_t ms = 15000) {
  return d && d->present && millis() - d->lastMs < ms;
}

static __attribute__((unused)) uint16_t tpmsAlarmBits(const TpmsReading *d) {
  if (!d || !d->present) return 0;
  uint16_t a = 0;
  if (d->pressureBar < tpmsThresholds.pressureLowBar) a |= TPMS_ALARM_LOW_PRESSURE;
  if (d->pressureBar > tpmsThresholds.pressureHighBar) a |= TPMS_ALARM_HIGH_PRESSURE;
  if (d->tempC > tpmsThresholds.tempHighC) a |= TPMS_ALARM_HIGH_TEMP;
  if (d->batteryPct < tpmsThresholds.battLowPct) a |= TPMS_ALARM_LOW_BATT;
  if (d->statusFlags & 0x01) a |= TPMS_SENSOR_LOW_VOLTAGE;
  if (d->statusFlags & 0x02) a |= TPMS_SENSOR_AIR_LEAK;
  if (d->statusFlags & 0x04) a |= TPMS_SENSOR_PRESSURIZING;
  if (d->statusFlags & 0x08) a |= TPMS_SENSOR_LOW_PRESSURE;
  return a;
}

static void tpmsMacStr(const uint8_t mac[6], char *buf) {
  sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void tpmsPrint(const TpmsReading *d, bool isNew) {
  char mb[18]; tpmsMacStr(d->mac, mb);
  Serial.printf("[TPMS] %s%s %s bar=%.2f temp=%dC v=%.1f batt=%d%% flags=0x%02X rssi=%d frames=%lu\r\n",
                isNew ? "NEW " : "", d->proto == 2 ? "YC" : "SY", mb,
                (double)d->pressureBar, d->tempC, (double)d->voltage,
                d->batteryPct, d->statusFlags, d->rssi, (unsigned long)d->frames);
}

// 一帧进表：找槽 → 500ms 防抖 → 字段换算
static void tpmsAccept(const TpmsRingItem &it, uint8_t proto, const TpmsFrame &f) {
  uint32_t now = millis();
  int i = tpmsFindByMac(it.mac);
  bool isNew = false;
  if (i < 0) {
    i = -1;
    int stalest = 0;
    for (int k = 0; k < TPMS_MAX_SENSORS; k++) {
      if (!tpmsDevs[k].present) { i = k; break; }          // 空槽优先
      if (tpmsDevs[k].lastMs < tpmsDevs[stalest].lastMs) stalest = k;
    }
    if (i < 0) i = stalest;   // 全满时顶掉最久没更新的（正常 2~4 个传感器用不到）
    isNew = true;
    memset(&tpmsDevs[i], 0, sizeof(TpmsReading));
    memcpy(tpmsDevs[i].mac, it.mac, 6);
    tpmsDevs[i].present = true;
    tpmsDevs[i].wheel = TPMS_WHEEL_NONE;
  }
  TpmsReading &d = tpmsDevs[i];
  if (!isNew && now - d.lastMs < TPMS_DEDUP_MS) { d.dedupDropped++; return; }   // 同 MAC 500ms 防抖
  d.proto = proto;
  d.statusFlags = f.flags;
  d.rssi = it.rssi;
  d.voltage = f.voltRaw * 0.1f;
  d.tempC = f.tempRaw;
  float psi = f.pressRaw * 0.1f - 14.6f;
  d.pressureBar = psi > 0 ? psi * 0.0689476f : 0;
  float pct = (d.voltage - 2.0f) / 1.3f * 100;
  d.batteryPct = pct >= 100 ? 100 : (pct <= 0 ? 0 : (uint8_t)pct);
  d.lastMs = now;
  d.frames++;
  static uint32_t lastPrint[TPMS_MAX_SENSORS] = {0};
  if (isNew || now - lastPrint[i] >= TPMS_DEBUG_MS) { lastPrint[i] = now; tpmsPrint(&d, isNew); }
}

// ---------- 扫描管理 ----------
static BLEScan *tpmsScan = nullptr;
static bool tpmsLinkBusy = false;              // 连着手机（在传地图）→ 降占空比
static uint32_t tpmsRestartAt = 0;

static void tpmsApplyDuty() {
  tpmsScan->setInterval(tpmsLinkBusy ? TPMS_SCAN_INT_BUSY : TPMS_SCAN_INT_IDLE);
  tpmsScan->setWindow(tpmsLinkBusy ? TPMS_WIN_BUSY : TPMS_WIN_IDLE);
}

// stop() 只是把 disc 取消掉（非阻塞）；is_continue=false 时 start() 内部会清结果表
static void tpmsRestart(bool clear) {
  if (!tpmsScan) return;
  tpmsScan->stop();
  tpmsApplyDuty();
  if (!tpmsScan->start(0, nullptr, !clear)) Serial.println("[TPMS] scan restart failed");
  tpmsRestartAt = millis() + TPMS_RESTART_MS;
  tpmsLastAdvMs = millis();                    // 不重置的话 90 秒静默判断会连环触发重启
}

// 在 BLEDevice::init + 开广播之后调用
static void tpmsBegin() {
  memset(tpmsDevs, 0, sizeof(tpmsDevs));
  tpmsScan = BLEDevice::getScan();
  // wantDuplicates=true → 关掉控制器去重（否则每台设备整个扫描期只报一次，收不到周期更新）；
  // shouldParse=false → 不让库解析，只拿原始字节（我们自己遍历 AD，CRC 需要完整原始帧）
  tpmsScan->setAdvertisedDeviceCallbacks(new TpmsScanCallbacks(), true, false);
  tpmsScan->setActiveScan(true);               // 名字/厂商数据可能在扫描应答里
  tpmsApplyDuty();
  if (!tpmsScan->start(0, nullptr, true)) {    // 0 = 一直扫，立即返回不阻塞
    Serial.println("[TPMS] scan start failed");
    tpmsScan = nullptr;
    return;
  }
  tpmsRestartAt = millis() + TPMS_RESTART_MS;
  Serial.println("[TPMS] scanner ready");
}

// 深睡前停扫描（可选，host 关掉时也会一并结束）
static void tpmsEnd() {
  if (tpmsScan) tpmsScan->stop();
}

// loop() 里调用。linkBusy = 连着手机（影响扫描占空比）。
static void tpmsTick(bool linkBusy) {
  if (!tpmsScan) return;
  if (linkBusy != tpmsLinkBusy) {              // 连接状态变了：换占空比重启扫描
    tpmsLinkBusy = linkBusy;
    tpmsRestart(false);
  }
  // 1. 取出扫描回调塞进来的所有广播，逐条解析
  static TpmsRingItem item;
  for (int n = 0; n < TPMS_RING_SLOTS; n++) {
    bool got = false;
    portENTER_CRITICAL(&tpmsMux);
    if (tpmsRingTail != tpmsRingHead) {
      item = tpmsRing[tpmsRingTail];
      tpmsRingTail = (tpmsRingTail + 1) % TPMS_RING_SLOTS;
      got = true;
    }
    portEXIT_CRITICAL(&tpmsMux);
    if (!got) break;
    TpmsFrame f;
    uint8_t macNative[6];
    for (int k = 0; k < 6; k++) macNative[k] = item.mac[5 - k];   // 换回 native 序做 CRC
    uint8_t proto = tpmsParseRecord(item.rec, item.len, macNative, f);
    if (proto) tpmsAccept(item, proto, f);
  }
  // 2. 扫描健康：掉了几分钟一条广播都没有（host 出问题）或到 5 分钟清一次结果表
  static uint32_t checkAt = 0;
  uint32_t now = millis();
  if ((int32_t)(now - checkAt) >= 1000) {
    checkAt = now;
    uint32_t lastAdv = tpmsLastAdvMs;
    bool silent = lastAdv && (int32_t)(now - lastAdv) > 90000;    // 90 秒一条广播都没有（环境里总有 BLE 噪声）
    if (!tpmsScan->isScanning() || silent || (int32_t)(now - tpmsRestartAt) >= 0) {
      Serial.printf("[TPMS] scan restart (active=%d silent=%d crcFail=%lu ringOvf=%lu)\r\n",
                    tpmsScan->isScanning() ? 1 : 0, silent ? 1 : 0,
                    (unsigned long)tpmsCrcFails, (unsigned long)tpmsRingOverflows);
      tpmsRestart(true);
    }
  }
}

#else  // !CONFIG_NIMBLE_ENABLED（本项目固件都是 NimBLE，留个空壳防别的配置编译不过）
static void tpmsBegin() {}
static void tpmsEnd() {}
static void tpmsTick(bool linkBusy) {}
#endif

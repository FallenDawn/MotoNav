// TPMS 数据与报警判定；扫描器和页面共用。
#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <stdlib.h>
#include <string.h>

#define TPMS_MAX_SENSORS 4

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

// ---- 显示顺序 + 备注（A/B/C/D） ----
// 槽位是"谁先扫进来给谁"分配的，重启/掉线/槽位回收后都可能变；备注和卡片顺序
// 一律按 MAC 记，存进 NVS，这样传感器重连、设备重启后位置和备注还在。
static const char TPMS_NOTE_CHAR[] = "-ABCD";     // 0=无 1=A 2=B 3=C 4=D（按单字符取，不能当子串用）

static uint64_t tpmsNoteMac[TPMS_MAX_SENSORS];    // 0 = 空
static uint8_t  tpmsNoteVal[TPMS_MAX_SENSORS];    // 1=A 2=B 3=C 4=D
static uint8_t  tpmsRowNoteVal[TPMS_MAX_SENSORS]; // 空槽的占位备注（还没扫到传感器时用）
static uint64_t tpmsOrderMac[TPMS_MAX_SENSORS];   // 显示顺序（MAC 列表），0 = 空
static uint8_t  tpmsOrderLen = 0;
static bool     tpmsPrefsReady = false;

static uint64_t tpmsMacKey(const uint8_t m[6]) {
  uint64_t k = 0;
  for (int i = 0; i < 6; i++) k = (k << 8) | m[i];
  return k;
}

static __attribute__((unused)) uint8_t tpmsNoteGet(uint64_t mac) {
  if (!mac) return 0;
  for (int i = 0; i < TPMS_MAX_SENSORS; i++)
    if (tpmsNoteMac[i] == mac) return tpmsNoteVal[i];
  return 0;
}

static __attribute__((unused)) void tpmsNoteSet(uint64_t mac, uint8_t v) {
  if (!mac) return;
  int empty = -1;
  for (int i = 0; i < TPMS_MAX_SENSORS; i++) {
    if (tpmsNoteMac[i] == mac) { tpmsNoteVal[i] = v; return; }
    if (!tpmsNoteMac[i] && empty < 0) empty = i;
  }
  if (v && empty >= 0) { tpmsNoteMac[empty] = mac; tpmsNoteVal[empty] = v; }
}

// 空槽（还没扫到传感器）的占位备注：让卡片在没设备时也能标字母、也能试拖动
static __attribute__((unused)) uint8_t tpmsRowNoteGet(int row) {
  return (row >= 0 && row < TPMS_MAX_SENSORS) ? tpmsRowNoteVal[row] : 0;
}
static __attribute__((unused)) void tpmsRowNoteSet(int row, uint8_t v) {
  if (row >= 0 && row < TPMS_MAX_SENSORS) tpmsRowNoteVal[row] = v;
}

static __attribute__((unused)) void tpmsPrefsSave() {
  char notes[20 * TPMS_MAX_SENSORS] = {0}, order[20 * TPMS_MAX_SENSORS] = {0};
  char rows[4 * TPMS_MAX_SENSORS] = {0};
  int n = 0;
  for (int i = 0; i < TPMS_MAX_SENSORS; i++) {
    if (!tpmsNoteMac[i] || !tpmsNoteVal[i]) continue;
    n += snprintf(notes + n, sizeof(notes) - n, "%s%012llX:%u", n ? "," : "",
                  (unsigned long long)tpmsNoteMac[i], tpmsNoteVal[i]);
  }
  n = 0;
  for (int i = 0; i < tpmsOrderLen && i < TPMS_MAX_SENSORS; i++) {
    if (!tpmsOrderMac[i]) continue;
    n += snprintf(order + n, sizeof(order) - n, "%s%012llX", n ? "," : "",
                  (unsigned long long)tpmsOrderMac[i]);
  }
  n = 0;
  for (int i = 0; i < TPMS_MAX_SENSORS; i++)
    n += snprintf(rows + n, sizeof(rows) - n, "%s%u", i ? "," : "", tpmsRowNoteVal[i]);
  Preferences pr;
  if (!pr.begin("motonav", false)) return;
  pr.putString("tpmsnote", notes);
  pr.putString("tpmsord", order);
  pr.putString("tpmsrow", rows);
  pr.end();
}

// "010203040506:2,0A0B0C0D0E0F:1" / "010203040506,0A0B0C0D0E0F"
static __attribute__((unused)) void tpmsPrefsLoad() {
  if (tpmsPrefsReady) return;
  tpmsPrefsReady = true;
  String ns, os, rs;
  {
    Preferences pr;
    if (pr.begin("motonav", true)) {
      ns = pr.getString("tpmsnote", "");
      os = pr.getString("tpmsord", "");
      rs = pr.getString("tpmsrow", "");
      pr.end();
    }
  }
  memset(tpmsNoteMac, 0, sizeof(tpmsNoteMac));
  memset(tpmsNoteVal, 0, sizeof(tpmsNoteVal));
  memset(tpmsRowNoteVal, 0, sizeof(tpmsRowNoteVal));
  memset(tpmsOrderMac, 0, sizeof(tpmsOrderMac));
  tpmsOrderLen = 0;

  int idx = 0;
  const char *p = ns.c_str();
  while (*p && idx < TPMS_MAX_SENSORS) {
    char *end = nullptr;
    uint64_t k = strtoull(p, &end, 16);
    if (end == p) break;
    p = end;
    uint8_t v = 0;
    if (*p == ':') { v = (uint8_t)strtoul(p + 1, &end, 10); p = end; }
    if (k && v >= 1 && v <= 4) { tpmsNoteMac[idx] = k; tpmsNoteVal[idx] = v; idx++; }
    if (*p == ',') p++; else break;
  }
  p = os.c_str();
  while (*p && tpmsOrderLen < TPMS_MAX_SENSORS) {
    char *end = nullptr;
    uint64_t k = strtoull(p, &end, 16);
    if (end == p) break;
    p = end;
    if (k) tpmsOrderMac[tpmsOrderLen++] = k;
    if (*p == ',') p++; else break;
  }
  // "1,2,0,0"：空槽的占位备注
  p = rs.c_str();
  for (int i = 0; i < TPMS_MAX_SENSORS && *p; i++) {
    char *end = nullptr;
    long v = strtol(p, &end, 10);
    if (end == p) break;
    p = end;
    if (v >= 1 && v <= 4) tpmsRowNoteVal[i] = (uint8_t)v;
    if (*p == ',') p++; else break;
  }
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


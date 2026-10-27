// 466x466 圆屏胎压页。和扫描器处在同一个翻译单元，数据均在 loop 中读写。
//
// 两个卡片（前后轮）的交互：
//   · 竖直拖动某张卡 → 跟手上下移，松手超过半个卡高就和另一张换位（顺序按 MAC 存 NVS）
//   · 双击卡片 → 备注字母 A/B/C/D 循环切换（A→B→C→D→无→A），同样按 MAC 存 NVS
// 备注是给人看的记号：传感器谁先扫进来谁在上面，分不清哪个是前轮时打个字母对一下就行。
#pragma once
#include "TpmsData.h"

// ---- 两卡布局几何（拖动命中和换位动画都按这套坐标） ----
#define TPMS_CARD_X       54
#define TPMS_CARD_W      358
#define TPMS_CARD_Y0     100
#define TPMS_CARD_PITCH  156
#define TPMS_CARD_H      136
#define TPMS_SWAP_COMMIT  78      // 拖过这么远松手就换位（半个卡高）
#define TPMS_SWAP_MS     130      // 换位/回弹动画时长

// 拖动/双击的交互状态（放前面：画页时要判断"是不是正在拖"）
static int8_t tpmsSwapRow = -1;      // 正在拖的行（0/1），-1 = 没在拖
static int    tpmsSwapOff = 0;       // 这张卡相对原位的 y 偏移（正 = 往下）
static int8_t tpmsTapRow = -1;       // 上一次单击的行（双击判定用）
static unsigned long tpmsTapAt = 0;

static const char *tpmsWheelLabel(uint8_t wheel) {
  switch (wheel) {
    case TPMS_WHEEL_FRONT: return "FRONT";
    case TPMS_WHEEL_REAR: return "REAR";
    case TPMS_WHEEL_FL: return "FRONT L";
    case TPMS_WHEEL_FR: return "FRONT R";
    case TPMS_WHEEL_RL: return "REAR L";
    case TPMS_WHEEL_RR: return "REAR R";
    default: return nullptr;
  }
}

static const char *tpmsAlarmLabel(uint16_t alarm) {
  if (alarm & TPMS_SENSOR_AIR_LEAK) return "AIR LEAK";
  if (alarm & (TPMS_ALARM_LOW_PRESSURE | TPMS_SENSOR_LOW_PRESSURE)) return "LOW PRESSURE";
  if (alarm & TPMS_ALARM_HIGH_PRESSURE) return "HIGH PRESSURE";
  if (alarm & TPMS_ALARM_HIGH_TEMP) return "HIGH TEMP";
  if (alarm & (TPMS_ALARM_LOW_BATT | TPMS_SENSOR_LOW_VOLTAGE)) return "LOW BATTERY";
  if (alarm & TPMS_SENSOR_PRESSURIZING) return "INFLATING";
  return "LIVE";
}

// ---------- 显示行 ↔ 槽位 ----------
// 先按记下来的 MAC 顺序排，剩下的（新扫到、还没记过的）按槽位号接在后面。
static int tpmsRowSlots(uint8_t *out, int maxOut) {
  int n = 0;
  for (int k = 0; k < tpmsOrderLen && n < maxOut; k++) {
    if (!tpmsOrderMac[k]) continue;
    for (int s = 0; s < TPMS_MAX_SENSORS && n < maxOut; s++) {
      if (!tpmsDevs[s].present) continue;
      if (tpmsMacKey(tpmsDevs[s].mac) != tpmsOrderMac[k]) continue;
      bool dup = false;
      for (int j = 0; j < n; j++) if (out[j] == s) dup = true;
      if (!dup) out[n++] = (uint8_t)s;
    }
  }
  for (int s = 0; s < TPMS_MAX_SENSORS && n < maxOut; s++) {
    if (!tpmsDevs[s].present) continue;
    bool dup = false;
    for (int j = 0; j < n; j++) if (out[j] == s) dup = true;
    if (!dup) out[n++] = (uint8_t)s;
  }
  return n;
}

static TpmsReading *tpmsRowData(int row) {
  uint8_t slots[TPMS_MAX_SENSORS];
  const int n = tpmsRowSlots(slots, TPMS_MAX_SENSORS);
  return (row >= 0 && row < n) ? tpmsGet(slots[row]) : nullptr;
}

// 这一行该显示的备注：有传感器看 MAC 记的，空槽看占位的
static uint8_t tpmsNoteOfRow(int row) {
  TpmsReading *d = tpmsRowData(row);
  if (d) {
    const uint8_t v = tpmsNoteGet(tpmsMacKey(d->mac));
    if (v) return v;
  }
  return tpmsRowNoteGet(row);
}

// ---------- 画卡 ----------
// 备注徽标：实心底 + 黑字，一眼能看到。没设备注时留一个暗框，提示这里可以双击。
static void tpmsNoteBadge(uint8_t v, int x, int y, int w, int h, int size, uint16_t color) {
  if (v < 1 || v > 4) {
    gfx->drawRoundRect(x, y, w, h, h / 3, 0x4208);
    return;
  }
  gfx->fillRoundRect(x, y, w, h, h / 3, color);
  const char ch[2] = {TPMS_NOTE_CHAR[v], '\0'};      // 只取一个字母（不能直接传 &TPMS_NOTE_CHAR[v]）
  printCentered(ch, x + w / 2, y + (h - 8 * size) / 2, size, C_BLACK);
}

// 左对齐 / 右对齐（printCentered 只会居中，这里换算一下中心点）
static void printLeft(const char *s, int x, int y, int size, uint16_t color) {
  printCentered(s, x + (int)strlen(s) * 6 * size / 2, y, size, color);
}
static void printRight(const char *s, int x, int y, int size, uint16_t color) {
  printCentered(s, x - (int)strlen(s) * 6 * size / 2, y, size, color);
}

static void drawTpmsCard(const TpmsReading *d, int slot, int y, bool compact, uint8_t note) {
  const bool present = d && d->present;
  const bool fresh = tpmsFresh(d);
  const uint16_t alarm = fresh ? tpmsAlarmBits(d) : 0;
  const uint16_t warnings = alarm & ~TPMS_SENSOR_PRESSURIZING;
  const uint16_t color = !fresh ? C_GREY : warnings ? C_ORANGE : C_CYAN;
  const uint16_t pressureColor = !fresh ? C_GREY :
      alarm & (TPMS_ALARM_LOW_PRESSURE | TPMS_ALARM_HIGH_PRESSURE |
               TPMS_SENSOR_LOW_PRESSURE | TPMS_SENSOR_AIR_LEAK) ? C_RED : C_WHITE;
  const uint16_t tempColor = !fresh ? C_GREY : alarm & TPMS_ALARM_HIGH_TEMP ? C_RED : C_WHITE;
  const int height = compact ? 74 : TPMS_CARD_H;
  gfx->fillRoundRect(TPMS_CARD_X, y, TPMS_CARD_W, height, 14, 0x1082);
  gfx->fillRoundRect(TPMS_CARD_X, y + 14, 4, height - 28, 2, color);

  char label[24], id[20], pressure[16], temp[16], status[56];
  const char *wheel = present ? tpmsWheelLabel(d->wheel) : nullptr;
  if (!present) snprintf(label, sizeof(label), "NO SENSOR");
  else if (wheel) snprintf(label, sizeof(label), "%s", wheel);
  else snprintf(label, sizeof(label), "SENSOR %d", slot + 1);
  if (present) snprintf(id, sizeof(id), "%02X:%02X:%02X", d->mac[3], d->mac[4], d->mac[5]);
  else snprintf(id, sizeof(id), "NO DATA");
  if (fresh) {
    snprintf(pressure, sizeof(pressure), "%.2f", (double)d->pressureBar);
    snprintf(temp, sizeof(temp), "%d", d->tempC);
    snprintf(status, sizeof(status), "%s  /  BAT %u%%", tpmsAlarmLabel(alarm), d->batteryPct);
  } else {
    snprintf(pressure, sizeof(pressure), "--");
    snprintf(temp, sizeof(temp), "--");
    if (present) snprintf(status, sizeof(status), "OFFLINE  /  LAST %lus AGO", (unsigned long)((millis() - d->lastMs) / 1000));
    else snprintf(status, sizeof(status), "WAITING FOR SENSOR");
  }

  if (compact) {
    tpmsNoteBadge(note, 58, y + 24, 30, 28, 2, color);
    printCentered(label, 126, y + 7, 1, color);
    printCentered(id, 330, y + 7, 1, C_GREY);
    printCentered(pressure, 166, y + 23, 3, pressureColor);
    printCentered("bar", 228, y + 32, 1, C_GREY);
    printCentered(temp, 316, y + 23, 3, tempColor);
    printCentered("C", 364, y + 32, 1, C_GREY);
    printCentered(status, CX, y + 57, 1, color);
  } else {
    // 头行：[A]  FRONT ·················· 3F:2A:19
    // 备注做成卡片自己的标签块，跟名字同一行，不再是飘在左边空白里的一个方块。
    tpmsNoteBadge(note, 66, y + 8, 36, 32, 3, color);
    printLeft(label, 112, y + 16, 2, color);
    printRight(id, 400, y + 20, 1, C_GREY);
    // 主体：左边压力（6 号字），右边温度（5 号字），下面各跟一行单位
    printCentered(pressure, 150, y + 44, 6, pressureColor);
    printCentered("bar", 150, y + 94, 2, C_GREY);
    printCentered(temp, 330, y + 48, 5, tempColor);
    printCentered("TEMP C", 330, y + 98, 1, C_GREY);
    printCentered(status, CX, y + 118, 1, color);
  }
}

static int tpmsCardY(int row, bool compact) {
  return (compact ? 92 : TPMS_CARD_Y0) + row * (compact ? 79 : TPMS_CARD_PITCH);
}

static void drawTpmsPage() {
  if (tpmsSwapRow >= 0) return;                        // 换位拖动中，别被定时重画打断
  tpmsPrefsLoad();
  onWaitScreen = false;
  frameIsMap = false;
  gfx->fillScreen(C_BLACK);
  printCentered("TPMS", CX, 39, 3, C_CYAN);
  printCentered("TIRE PRESSURE / TEMPERATURE", CX, 73, 1, C_GREY);
  drawBattery(300, 45);                                // 本机电量（小字，跟着标题右侧）
  const int count = tpmsCount();
  const bool compact = count > 2;
  uint8_t slots[TPMS_MAX_SENSORS];
  const int n = tpmsRowSlots(slots, TPMS_MAX_SENSORS);
  // 没传感器时打的占位字母，等传感器扫进来就交给它，免得两张卡撞同一个字母
  bool adopted = false;
  for (int r = 0; r < n; ++r) {
    TpmsReading *d = tpmsGet(slots[r]);
    if (!d || tpmsNoteGet(tpmsMacKey(d->mac)) || !tpmsRowNoteGet(r)) continue;
    tpmsNoteSet(tpmsMacKey(d->mac), tpmsRowNoteGet(r));
    tpmsRowNoteSet(r, 0);
    adopted = true;
  }
  if (adopted) tpmsPrefsSave();
  int row = 0;
  for (int i = 0; i < n; ++i) {
    drawTpmsCard(tpmsGet(slots[i]), slots[i], tpmsCardY(row, compact), compact, tpmsNoteOfRow(row));
    ++row;
  }
  // 两轮布局始终留两个槽；不按广播到达顺序推测前后轮。
  while (!compact && row < 2) {
    drawTpmsCard(nullptr, row, tpmsCardY(row, false), false, tpmsNoteOfRow(row));
    ++row;
  }
  if (count > 2) printCentered("SENSOR ID = LAST 3 MAC BYTES", CX, 416, 1, C_GREY);
  else if (count == 1) printCentered("2-TAP A/B/C/D NOTE  -  SWAP NEEDS 2 SENSORS", CX, 416, 1, C_GREY);
  else printCentered("DRAG UP/DOWN TO SWAP  -  2-TAP A/B/C/D", CX, 416, 1, C_GREY);
  drawPageDots();
  needFlush = true;
}

// ---------- 双卡上下拖动交换 ----------
static bool tpmsSwapActive() { return tpmsSwapRow >= 0; }

// 185B 的触摸 y 和画布是上下颠倒的（音乐按钮的 hit test 也是这么翻回来的，
// 见 musicButtonAt），所以触点要先翻成画布坐标，竖直位移也要取反。
// 实机上如果发现卡片跟着手指反着走，把这个宏改成 0 即可。
#define TPMS_TOUCH_Y_FLIP 1
static int tpmsCanvasY(int y) { return TPMS_TOUCH_Y_FLIP ? (LCD_HEIGHT - 1 - y) : y; }
static int tpmsCanvasDy(int dy) { return TPMS_TOUCH_Y_FLIP ? -dy : dy; }

// 触点落在哪一行卡片上（按当前布局算，-1 = 不在卡片上）
static int tpmsRowAtY(int y) {
  y = tpmsCanvasY(y);
  const bool compact = tpmsCount() > 2;
  const int h = compact ? 74 : TPMS_CARD_H;
  for (int r = 0; r < TPMS_MAX_SENSORS; r++) {
    const int ya = tpmsCardY(r, compact);
    if (y >= ya - 4 && y <= ya + h + 4) return r;
  }
  return -1;
}

// 拖动的中间帧：被拖的那张跟着手指走，另一张留在原位；被拖的最后画，压在上面。
static void tpmsDrawSwapFrame(int row, int off) {
  onWaitScreen = false;
  frameIsMap = false;
  gfx->fillScreen(C_BLACK);
  const bool armed = abs(off) >= TPMS_SWAP_COMMIT;
  printCentered("TPMS", CX, 39, 3, C_CYAN);
  printCentered("SWAP  UP / DOWN", CX, 73, 1, armed ? C_CYAN : C_GREY);
  drawBattery(300, 45);
  uint8_t slots[TPMS_MAX_SENSORS];
  const int n = tpmsRowSlots(slots, TPMS_MAX_SENSORS);
  for (int pass = 0; pass < 2; pass++) {
    for (int r = 0; r < 2; r++) {
      const bool isDrag = (r == row);
      if (isDrag != (pass == 1)) continue;             // 先画另一张，再画被拖的
      const int y = tpmsCardY(r, false) + (isDrag ? off : 0);
      drawTpmsCard(r < n ? tpmsGet(slots[r]) : nullptr, r < n ? slots[r] : r, y, false, tpmsNoteOfRow(r));
    }
  }
  printCentered(armed ? "RELEASE TO SWAP" : "RELEASE TO CANCEL", CX, 416, 1, armed ? C_CYAN : C_GREY);
  drawPageDots();
  needFlush = true;
}

// 换位/回弹动画：每帧整屏重画，推屏和滑页用的是同一套异步 flush
static void tpmsSwapAnimate(int row, int from, int to) {
  unsigned long t0 = millis();
  for (;;) {
    float t = (millis() - t0) / (float)TPMS_SWAP_MS;
    if (t >= 1) break;
    float e = 1 - (1 - t) * (1 - t) * (1 - t);
    tpmsDrawSwapFrame(row, (int)lroundf(from + (to - from) * e));
    flushAsync();
    needFlush = false;
  }
  tpmsDrawSwapFrame(row, to);
  flushAsync();
  needFlush = false;
}

// 落位：把前两行的 MAC 顺序对调后存下来；还没传感器时换的是两个占位字母，
// 这样没设备也能试手势、也能先把 A/B 摆到想要的位置。
static void tpmsSwapApply() {
  uint8_t slots[TPMS_MAX_SENSORS];
  const int n = tpmsRowSlots(slots, TPMS_MAX_SENSORS);
  if (n < 2) {
    const uint8_t a = tpmsRowNoteGet(0), b = tpmsRowNoteGet(1);
    tpmsRowNoteSet(0, b);
    tpmsRowNoteSet(1, a);
    tpmsPrefsSave();
    return;
  }
  const uint64_t first = tpmsMacKey(tpmsDevs[slots[1]].mac);
  const uint64_t second = tpmsMacKey(tpmsDevs[slots[0]].mac);
  memset(tpmsOrderMac, 0, sizeof(tpmsOrderMac));
  tpmsOrderLen = 0;
  tpmsOrderMac[tpmsOrderLen++] = first;
  tpmsOrderMac[tpmsOrderLen++] = second;
  for (int i = 2; i < n && tpmsOrderLen < TPMS_MAX_SENSORS; i++)
    tpmsOrderMac[tpmsOrderLen++] = tpmsMacKey(tpmsDevs[slots[i]].mac);
  tpmsPrefsSave();
}

// 竖直拖过阈值：开始拖卡片。返回 true 表示这次手势被胎压页接管（不做换页）。
static bool tpmsSwapBegin(int x0, int y0) {
  const int cnt = tpmsCount();
  if (cnt > 2 || cnt == 1) return false;               // 紧凑四卡不换；只剩一张卡时空槽不参与排序
  if (x0 < TPMS_CARD_X || x0 > TPMS_CARD_X + TPMS_CARD_W) return false;
  const int row = tpmsRowAtY(y0);
  if (row < 0 || row > 1) return false;
  tpmsSwapRow = (int8_t)row;
  tpmsSwapOff = 0;
  return true;
}

static void tpmsSwapMove(int dy) {
  if (tpmsSwapRow < 0) return;
  dy = tpmsCanvasDy(dy);                               // 手指往下 = 卡片往下
  // 反方向只给一点橡皮筋，免得把卡片拖出屏幕
  const int lo = tpmsSwapRow == 0 ? -40 : -TPMS_CARD_PITCH;
  const int hi = tpmsSwapRow == 0 ? TPMS_CARD_PITCH : 40;
  tpmsSwapOff = dy < lo ? lo : (dy > hi ? hi : dy);
  tpmsDrawSwapFrame(tpmsSwapRow, tpmsSwapOff);
}

static void tpmsSwapFinish() {
  if (tpmsSwapRow < 0) return;
  const int row = tpmsSwapRow, off = tpmsSwapOff;
  const bool swap = abs(off) >= TPMS_SWAP_COMMIT;
  const int target = swap ? (off > 0 ? TPMS_CARD_PITCH : -TPMS_CARD_PITCH) : 0;
  tpmsSwapAnimate(row, off, target);
  tpmsSwapRow = -1; tpmsSwapOff = 0;
  if (swap) tpmsSwapApply();
  if (page == PAGE_TPMS) drawTpmsPage();
}

// 来电/特效等打断：直接弹回原位
static void tpmsSwapCancel() {
  if (tpmsSwapRow < 0) return;
  const int row = tpmsSwapRow, off = tpmsSwapOff;
  tpmsSwapRow = -1; tpmsSwapOff = 0;
  tpmsSwapAnimate(row, off, 0);
  if (page == PAGE_TPMS) drawTpmsPage();
}

// ---------- 双击切备注 ----------
// 有传感器就记在这个 MAC 上（换位/重启后跟着传感器走）；空槽先记在行上。
static void tpmsNoteCycle(int row) {
  const uint8_t v = (uint8_t)((tpmsNoteOfRow(row) + 1) % 5);   // 无 → A → B → C → D → 无
  TpmsReading *d = tpmsRowData(row);
  if (d) tpmsNoteSet(tpmsMacKey(d->mac), v);
  else tpmsRowNoteSet(row, v);
  tpmsPrefsSave();
}

// 单击一次记下行号，450ms 内再点同一行 = 双击
static void tpmsTap(int x, int y) {
  if (tpmsSwapActive()) return;
  if (x < TPMS_CARD_X || x > TPMS_CARD_X + TPMS_CARD_W) return;
  const int row = tpmsRowAtY(y);
  if (row < 0 || row > 1) return;                      // 只认两张卡（紧凑四卡不做备注）
  const unsigned long now = millis();
  if (tpmsTapRow == row && now - tpmsTapAt < 450) {
    tpmsTapRow = -1; tpmsTapAt = 0;
    tpmsPrefsLoad();
    tpmsNoteCycle(row);
    drawTpmsPage();
    return;
  }
  tpmsTapRow = (int8_t)row;
  tpmsTapAt = now;
}

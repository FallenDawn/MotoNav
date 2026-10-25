// 导航与小地图模块：手机（高德 App 桥接）发来的定位、路线、地图块、图片地图（天地图）、
// 电子眼、红绿灯、路况等数据的接收、解析和渲染。拆自 MotoNav.cpp（v7.13.1）。
// 跨文件共享的状态和接口见 NavMap.h；画布/图元等公共工具见 MotoNavCommon.h。
#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include <math.h>
#include <SPIFFS.h>
#include <miniz.h>            // 罪恶都市轮廓：仅接收地图时解压
#include "DragonRadar.h"
#include "MotoNavCommon.h"
#include "NavMap.h"

// RGB565 混合（t: 0~256，256 = 全是 b）；栅格地图和路线描边用
uint16_t blendF(uint16_t a, uint16_t b, uint32_t t) {
  uint32_t ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  uint32_t br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  uint32_t r = (ar * (256 - t) + br * t) >> 8, g = (ag * (256 - t) + bg * t) >> 8, bl = (ab * (256 - t) + bb * t) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

// 寻宝罗盘目的地菱形（定义在文件后部，drawDestBlip 先用到）
void drawCompassBlip(float px, float py, bool outside);



// ================= NavMap 段 01（源 MotoNav.cpp 461-464 行） =================

bool inNav = false;           // 是否处于导航中
String navDir = "";           // 当前方向
int navDist = 0;              // 当前距离（米）
float navAngle = NAN;         // v1.8 路口实际转弯角度；NAN = 没有，用标准角度

// ================= NavMap 段 02（源 MotoNav.cpp 469-475 行） =================

int speedKmh = -1;            // 车速，-1 表示还没有
int camLimit = 0, camDist = 0;
bool hasCam = false;          // 前方是否有测速
int laneN = 0;                // 车道数，0 表示不显示
// v3.1 小地图底部的行程信息（"#e,剩余米,到达时间HHMM,红绿灯数"）和前方红绿灯（"#g,距离"）
bool tripOn = false;
bool cruiseOn = false;        // v4.8 巡航模式（没有目的地，只显示地图、车速、电子眼、安全提示）

// ================= NavMap 段 03（源 MotoNav.cpp 478-482 行） =================

bool cleanOn = false;         // v4.9 清爽模式：地图照常，去掉红绿灯、电子眼、安全提示、测速牌和行程信息（"#w,1/0"）
int tripRemainM = 0, tripEta = 0, tripLights = 0;
// 初版实时路况提醒：手机根据高德路线计算后发送 #TR,等级,前方距离,覆盖长度
int trafficAlertLevel = 0, trafficAlertDist = 0, trafficAlertSpan = 0;
unsigned long trafficAlertAt = 0;

// ================= NavMap 段 04（源 MotoNav.cpp 484-496 行） =================

#include "TripProgress.h"
TripProgress tripProgress;
bool progOn = true;
long progDone = 0;                   // 这趟已经走掉的里程（米）
int progLastRm = -1;                 // 上一次收到的剩余里程
unsigned long progLastAt = 0;
int lightDist = -1;           // 前方红绿灯距离（米），-1 = 不显示
// v7.13.2 红绿灯倒计时：App 从高德导航通知/界面文字解析出 “红/绿灯 N 秒”（“#GL,相位,秒”）。
// 数据只在高德自家导航里有，SDK 不提供；收不到就不显示，5 秒没刷新自动消失。
volatile int lightCdSec = -1;
volatile uint8_t lightCdPhase = 0;      // 0 = 红灯，1 = 绿灯
volatile unsigned long lightCdAt = 0;

// ================= NavMap 段 05（源 MotoNav.cpp 497-507 行） =================

// v3.5 小地图上的电子眼（手机发 "#c,坐标系,x,y,限速,类型,..."，最多 6 个）
#define MCAM_MAX 10           // v4.8 6 → 10（电子眼 + 道路安全提示）
int mapCamN = 0, mapCamFrame = -1;
int32_t mapCamX[MCAM_MAX], mapCamY[MCAM_MAX];
int mapCamSpd[MCAM_MAX];
int mapCamTyp[MCAM_MAX];      // v3.8 图标编号（手机换算好的，0~15）
int8_t mapCamPref[MCAM_MAX];  // v3.8 图标放好了没有（-1 = 还没放过）
float mapCamPrefA[MCAM_MAX];  // v4.1 类型图标在小摄像头的哪个方向（屏幕角度）
int8_t mapCamPrefK[MCAM_MAX]; // v4.3 放图标时这一处有几个类型图标（个数变了就重新放）
float mapCamLockH[MCAM_MAX];  // v4.0 放图标时的地图朝向：之后固定不动，转弯后（朝向变了且已经转稳）才重新放
uint8_t laneBg[8], laneFr[8]; // 每条车道的背景方向码、推荐方向码（0xF = 不推荐）

// ================= NavMap 段 06（源 MotoNav.cpp 1189-1190 行） =================

float min4(float a, float b, float c, float d) { return fminf(fminf(a, b), fminf(c, d)); }
float max4(float a, float b, float c, float d) { return fmaxf(fmaxf(a, b), fmaxf(c, d)); }

// ================= NavMap 段 07（源 MotoNav.cpp 1218-1387 行） =================

// 画一条“先竖直向上、再圆滑地拐向 deg 方向”的粗线。
// (ox, oy) 是拐角；stem = 拐角以下竖直段长度；arm = 拐出段长度；fillet = 圆角半径。
void bentStroke(float ox, float oy, float stem, float arm, float deg, float fillet, float w,
                uint16_t stemCol, uint16_t armCol, float &endX, float &endY, float maxT) {
  float a = deg * PI / 180;
  float dx = sinf(a), dy = -cosf(a);
  float t = fillet * tanf(fabsf(a) / 2);
  if (fabsf(deg) > 3) {                     // v1.8.1：小角度转弯也给一段明显的圆弧，不要折角
    float minT = fminf(fillet * 0.55f, maxT);
    if (t < minT) t = minT;
  }
  if (t > maxT)        t = maxT;          // v1.8：锐角弯时圆角不要太大
  if (t > arm * 0.7f)  t = arm * 0.7f;
  if (t > stem * 0.7f) t = stem * 0.7f;
  float p1x = ox,          p1y = oy + t;
  float p2x = ox + dx * t, p2y = oy + dy * t;
  endX = ox + dx * arm;
  endY = oy + dy * arm;
  if (t < 0.5f && fabsf(deg) < 1 && stemCol == armCol) {   // 直行：一整根，不留接缝
    thickLine(ox, oy + stem, endX, endY, w, stemCol);
    return;
  }
  float lx = p1x, ly = p1y;
  const int N = 12;
  for (int i = 1; i <= N; i++) {
    float u = (float)i / N, v = 1 - u;
    float x = v * v * p1x + 2 * v * u * ox + u * u * p2x;
    float y = v * v * p1y + 2 * v * u * oy + u * u * p2y;
    thickLine(lx, ly, x, y, w, armCol);
    lx = x; ly = y;
  }
  thickLine(p2x, p2y, endX, endY, w, armCol);
  thickLine(ox, oy + stem, p1x, p1y, w, stemCol);
}

// ================= 大图标（中、近阶段），k = 缩放比例，cy = 中心 y =================

void drawTurn(float deg, uint16_t col, float cy, float k) {
  // v1.8：转角超过 90 度（锐角弯）时，竖段和拐出段加长、线条稍细、圆角变小，
  //       让箭头和竖线分得开，看起来是清楚的锐角
  float sv = constrain((fabsf(deg) - 90) / 60, 0.0f, 1.0f);
  const float W = (42 - 6 * sv) * k, STEM = (86 + 40 * sv) * k, ARM = (78 + 30 * sv) * k, HEAD = (62 - 6 * sv) * k;
  float a = deg * PI / 180;
  float dx = sinf(a), dy = -cosf(a);
  float px = cosf(a), py = sinf(a);
  float ex = dx * ARM, ey = dy * ARM;
  float tx = ex + dx * HEAD, ty = ey + dy * HEAD;
  float c1x = ex + px * HEAD * 0.8f, c1y = ey + py * HEAD * 0.8f;
  float c2x = ex - px * HEAD * 0.8f, c2y = ey - py * HEAD * 0.8f;
  float minX = fminf(min4(-W / 2, ex - W / 2, tx, c1x), c2x);
  float maxX = fmaxf(max4(W / 2, ex + W / 2, tx, c1x), c2x);
  float minY = fminf(min4(-W / 2, ey - W / 2, ty, c1y), c2y);
  float maxY = fmaxf(max4(STEM, ey + W / 2, ty, c1y), c2y);     // v1.8：锐角弯箭头朝下，也要算进外框
  float ox = CX - (minX + maxX) / 2;
  float oy = cy - (minY + maxY) / 2;
  float endX, endY;
  bentStroke(ox, oy, STEM, ARM, deg, 40 * k, W, col, col, endX, endY, (40 - 28 * sv) * k);
  arrowHead(endX, endY, deg, HEAD, col);
}

void drawUturn(uint16_t col, float cy, float k) {
  const float W = 42 * k, R = 54 * k, LEG = 130 * k, DOWN = 44 * k, HEAD = 50 * k;
  float minX = -R - HEAD * 0.8f, maxX = R + W / 2;
  float minY = -R - W / 2,       maxY = fmaxf(LEG, DOWN + HEAD);
  float ox = CX - (minX + maxX) / 2;
  float oy = cy - (minY + maxY) / 2;
  thickLine(ox + R, oy + LEG, ox + R, oy, W, col);
  float lx = ox + R, ly = oy;
  for (int i = 1; i <= 16; i++) {
    float t = PI * i / 16;
    float x = ox + R * cosf(t), y = oy - R * sinf(t);
    thickLine(lx, ly, x, y, W, col);
    lx = x; ly = y;
  }
  thickLine(ox - R, oy, ox - R, oy + DOWN, W, col);
  arrowHead(ox - R, oy + DOWN, 180, HEAD, col);
}

void drawArrive(uint16_t col, float cy, float k) {
  iconCircle(CX, cy, 88 * k, col);
  iconCircle(CX, cy, 43 * k, C_BLACK);
  iconCircle(CX, cy, 20 * k, col);
}

bool dirAngle(const String &dir, float &deg) {
  // v1.8：有实际角度、并且左右方向一致时，用实际角度
  if (!isnan(navAngle) && (dir == "L" || dir == "R" || dir == "SL" || dir == "SR")) {
    bool left = (dir == "L" || dir == "SL");
    if ((left && navAngle < 0) || (!left && navAngle > 0)) {
      deg = constrain(navAngle, -150.0f, 150.0f);
      return true;
    }
  }
  if (dir == "L")  { deg = -90; return true; }
  if (dir == "R")  { deg = 90;  return true; }
  if (dir == "SL") { deg = -45; return true; }
  if (dir == "SR") { deg = 45;  return true; }
  return false;
}

// 大图标；laneK = 有车道时的额外缩小比例
void drawBigIconAt(const String &dir, uint16_t col, float cy, float laneK,
                   float cx, float top, float bottom, float maxW) {
  float deg;
  iconBegin();
  if (dir == "U")              drawUturn(col, cy, 0.70f * laneK);
  else if (dir == "A")         drawArrive(col == C_WHITE ? C_GREEN : col, cy, 0.78f * laneK);
  else if (dirAngle(dir, deg)) drawTurn(deg, col, cy, 0.78f * laneK);
  else                         drawTurn(0, col, cy, 0.62f * laneK);   // 直行
  iconFitAt(cx, top, bottom, maxW);
  iconEnd();
}

void drawBigIcon(const String &dir, uint16_t col, float cy, float laneK, float top, float bottom) {
  drawBigIconAt(dir, col, cy, laneK, CX, top, bottom, ICON_MAX_W);
}

// ================= 远阶段预告：白色直行竖条（居中）+ 灰色转向预告 =================

void drawPreviewTurn(float deg, float cy, float k) {
  float sv = constrain((fabsf(deg) - 90) / 60, 0.0f, 1.0f);   // v1.8 锐角弯：拐出段加长
  const float W = 30 * k, STEM = 175 * k, ARM = (52 + 25 * sv) * k, HEAD = 42 * k;
  float a = deg * PI / 180;
  float dy = -cosf(a), py = sinf(a);
  float ey = dy * ARM;
  float ty = ey + dy * HEAD;
  float c1y = ey + py * HEAD * 0.8f, c2y = ey - py * HEAD * 0.8f;
  float minY = fminf(min4(-W / 2, ey - W / 2, ty, c1y), c2y);
  float maxY = fmaxf(max4(STEM, ey + W / 2, ty, c1y), c2y);
  float oy = cy - (minY + maxY) / 2;
  float endX, endY;
  bentStroke(CX, oy, STEM, ARM, deg, 26 * k, W, C_WHITE, C_DIM, endX, endY, (26 - 16 * sv) * k);
  arrowHead(endX, endY, deg, HEAD, C_DIM);
}

void drawPreviewUturn(float cy, float k) {
  const float W = 30 * k, STEM = 175 * k, R = 30 * k, DOWN = 26 * k, HEAD = 42 * k;
  float oy = cy - ((-R - W / 2) + STEM) / 2;
  float ox = CX;
  float lx = ox, ly = oy;
  for (int i = 1; i <= 12; i++) {
    float t = PI * i / 12;
    float x = ox - R + R * cosf(t), y = oy - R * sinf(t);
    thickLine(lx, ly, x, y, W, C_DIM);
    lx = x; ly = y;
  }
  thickLine(ox - 2 * R, oy, ox - 2 * R, oy + DOWN, W, C_DIM);
  arrowHead(ox - 2 * R, oy + DOWN, 180, HEAD * 0.8f, C_DIM);
  thickLine(ox, oy + STEM, ox, oy, W, C_WHITE);
}

void drawPreviewArrive(float cy, float k) {
  const float W = 30 * k, STEM = 175 * k, TR = 32 * k;
  float oy = cy - ((-2 * TR - 8 * k) + STEM) / 2;
  thickLine(CX, oy + STEM, CX, oy, W, C_WHITE);
  float ty = oy - TR - 8 * k;
  iconCircle(CX, ty, TR, C_DIM);
  iconCircle(CX, ty, TR / 2, C_BLACK);
  iconCircle(CX, ty, TR / 5, C_DIM);
}

void drawPreview(const String &dir, float cy, float k, float top, float bottom) {
  float deg;
  iconBegin();
  if (dir == "U")              drawPreviewUturn(cy, k);
  else if (dir == "A")         drawPreviewArrive(cy, k);
  else if (dirAngle(dir, deg)) drawPreviewTurn(deg, cy, k);
  iconFit(top, bottom, ICON_MAX_W);
  iconEnd();
}

// ================= NavMap 段 08（源 MotoNav.cpp 1389-1605 行） =================

// ================= 圆环 =================

// 填充一段圆环：p0~p1 表示位置（0 = 正上方，顺时针，1 = 一整圈），每 0.5° 两个三角形
// v1.8.3 抗锯齿圆环：开机时把半径 RLUT_MIN~RLUT_MAX 之间每个像素的“半径、角度”算好存表，
// 按整数半径分桶；画环时只遍历需要的那几圈像素，按到边缘的距离算覆盖率，和底色混合。
#define RLUT_MIN 212
#define RLUT_MAX 236
#define RLUT_N   (RLUT_MAX - RLUT_MIN)
struct RingPix { uint32_t idx; uint16_t ang; uint16_t r8; };   // 像素位置、角度（0~65535 = 一圈）、半径×256
RingPix *ringLut = nullptr;
int ringOff[RLUT_N + 1];

void buildRingLut() {
  int cnt[RLUT_N] = {0};
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1) {
      int total = 0;
      for (int b = 0; b < RLUT_N; b++) { ringOff[b] = total; total += cnt[b]; cnt[b] = 0; }
      ringOff[RLUT_N] = total;
      ringLut = (RingPix *)ps_malloc(sizeof(RingPix) * (total + 1));
      if (!ringLut) return;
    }
    for (int y = 0; y < LCD_HEIGHT; y++) {
      float dy = y + 0.5f - CX;
      for (int x = 0; x < LCD_WIDTH; x++) {
        float dx = x + 0.5f - CX;
        float r = sqrtf(dx * dx + dy * dy);
        int b = (int)floorf(r) - RLUT_MIN;
        if (b < 0 || b >= RLUT_N) continue;
        if (pass == 0) { cnt[b]++; continue; }
        float a = atan2f(dx, -dy) / (2 * PI);                     // 0 = 正上方，顺时针
        if (a < 0) a += 1;
        RingPix &e = ringLut[ringOff[b] + cnt[b]++];
        e.idx = y * LCD_WIDTH + x;
        e.ang = (uint16_t)fminf(a * 65536.0f, 65535.0f);
        e.r8 = (uint16_t)(r * 256);
      }
    }
  }
}

// 两个 RGB565 颜色按 t（0~255）混合
uint16_t blend565(uint16_t a, uint16_t b, int t) {
  int it = 255 - t;
  int r = (((a >> 11) & 0x1F) * it + ((b >> 11) & 0x1F) * t + 127) / 255;
  int g = (((a >> 5) & 0x3F) * it + ((b >> 5) & 0x3F) * t + 127) / 255;
  int bl = ((a & 0x1F) * it + (b & 0x1F) * t + 127) / 255;
  return (r << 11) | (g << 5) | bl;
}

// 抗锯齿实心圆（和已有内容混合）
void aaDisc(float cx, float cy, float rad, uint16_t c) {
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) { gfx->fillCircle(cx, cy, rad, c); return; }
  int x0 = max(0, (int)floorf(cx - rad - 1)), x1 = min(LCD_WIDTH - 1, (int)ceilf(cx + rad + 1));
  int y0 = max(0, (int)floorf(cy - rad - 1)), y1 = min(LCD_HEIGHT - 1, (int)ceilf(cy + rad + 1));
  for (int y = y0; y <= y1; y++) {
    float dy = y + 0.5f - cy;
    for (int x = x0; x <= x1; x++) {
      float dx = x + 0.5f - cx;
      float a = rad - sqrtf(dx * dx + dy * dy) + 0.5f;
      if (a <= 0) continue;
      int t = a >= 1 ? 255 : (int)(a * 255);
      uint16_t &px = fb[y * LCD_WIDTH + x];
      px = (t == 255) ? c : blend565(px, c, t);
    }
  }
}

// 填充一段圆环：p0~p1 表示位置（0 = 正上方，顺时针，1 = 一整圈），rIn~rOut 为内外半径
// v2.6 整圈圆环：按行填充（中间实心部分直接写，只有边缘一两个像素算覆盖率）
void fillAnnulus(float rIn, float rOut, uint16_t c) {
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;
  int y0 = max(0, (int)floorf(CX - rOut - 1)), y1 = min(LCD_HEIGHT - 1, (int)ceilf(CX + rOut + 1));
  float ro2 = (rOut + 0.5f) * (rOut + 0.5f), ri2 = (rIn - 0.5f) * (rIn - 0.5f);
  float so2 = (rOut - 0.5f) * (rOut - 0.5f), si2 = (rIn + 0.5f) * (rIn + 0.5f);
  for (int y = y0; y <= y1; y++) {
    float dy = y + 0.5f - CX, dy2 = dy * dy;
    if (dy2 >= ro2) continue;
    float ho = sqrtf(ro2 - dy2);                            // 有覆盖的外沿
    float hi = dy2 < ri2 ? sqrtf(ri2 - dy2) : -1;           // 有覆盖的内沿（空心）
    float so = dy2 < so2 ? sqrtf(so2 - dy2) : -1;           // 实心外沿
    float si = dy2 < si2 ? sqrtf(si2 - dy2) : 0;            // 实心内沿
    uint16_t *row = fb + y * LCD_WIDTH;
    for (int side = 0; side < 2; side++) {
      int xa, xb;
      if (hi < 0) {                                          // 这一行没有空心：一整段
        if (side) break;
        xa = (int)floorf(CX - ho); xb = (int)ceilf(CX + ho);
      } else if (side == 0) {
        xa = (int)floorf(CX - ho); xb = (int)ceilf(CX - hi);
      } else {
        xa = (int)floorf(CX + hi); xb = (int)ceilf(CX + ho);
      }
      xa = max(xa, 0); xb = min(xb, LCD_WIDTH - 1);
      for (int x = xa; x <= xb; x++) {
        float dx = fabsf(x + 0.5f - CX);
        if (so >= 0 && dx >= si && dx <= so) { row[x] = c; continue; }
        float r = sqrtf(dx * dx + dy2);
        float a = fminf(r - rIn + 0.5f, rOut - r + 0.5f);
        if (a <= 0) continue;
        if (a >= 1) { row[x] = c; continue; }
        row[x] = blend565(row[x], c, (int)(a * 255));
      }
    }
  }
}

void fillRingBand(float p0, float p1, float rIn, float rOut, uint16_t c) {
  if (p1 <= p0) return;
  if (p1 - p0 >= 0.9999f) { fillAnnulus(rIn, rOut, c); return; }   // v2.6 整圈走快速路径
  uint16_t *fb = gfx->getFramebuffer();
  if (!ringLut || !fb) return;
  bool full = (p1 - p0) >= 0.9999f;
  int b0 = (int)floorf(rIn - 1) - RLUT_MIN, b1 = (int)ceilf(rOut + 1) - RLUT_MIN;
  if (b0 < 0) b0 = 0;
  if (b1 > RLUT_N) b1 = RLUT_N;
  if (b0 >= b1) return;
  const float TWO_PI_F = 2 * PI;
  for (int i = ringOff[b0]; i < ringOff[b1]; i++) {
    const RingPix &e = ringLut[i];
    float r = e.r8 * (1.0f / 256);
    float ar = fminf(r - rIn + 0.5f, rOut - r + 0.5f);
    if (ar <= 0) continue;
    if (ar > 1) ar = 1;
    if (!full) {
      float a = e.ang * (1.0f / 65536);
      float aa = fminf((a - p0) * TWO_PI_F * r, (p1 - a) * TWO_PI_F * r) + 0.5f;   // 到两端的弧长（像素）
      if (aa <= 0) continue;
      if (aa < 1) ar *= aa;
    }
    int t = (int)(ar * 255);
    if (t <= 0) continue;
    uint16_t &px = fb[e.idx];
    px = (t >= 255) ? c : blend565(px, c, t);
  }
}

void drawRingTrack() { fillRingBand(0, 1, RING_R - 5, RING_R + 5, C_TRACK); }

void drawRingProgress(float p, int thick, uint16_t col) {
  if (p <= 0.002f) return;
  if (p > 1) p = 1;
  float h = thick / 2.0f;
  fillRingBand(0, p, RING_R - h, RING_R + h, col);
  float a = p * 2 * PI;
  // 两端圆头（像素中心在 +0.5 处，所以圆心也取 CX 而不是 CX-0.5）
  aaDisc(CX, CX - RING_R, h, col);
  aaDisc(CX + RING_R * sinf(a), CX - RING_R * cosf(a), h, col);
}

// v7.3 抗锯齿圆弧带：角度 a0 → a1（度，0 = 正上方，顺时针；跨度要小于 180°），半径 rIn~rOut，alpha 0~255
void arcBand(float a0, float a1, float rIn, float rOut, uint16_t c, int alpha = 255) {
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb || a1 <= a0) return;
  float s0 = sinf(a0 * PI / 180), c0 = cosf(a0 * PI / 180);
  float s1 = sinf(a1 * PI / 180), c1 = cosf(a1 * PI / 180);
  float ro2 = (rOut + 1) * (rOut + 1), ri2 = rIn > 1 ? (rIn - 1) * (rIn - 1) : 0;
  int y0 = max(0, (int)(CX - rOut - 1)), y1 = min(LCD_HEIGHT - 1, (int)(CX + rOut + 1));
  for (int y = y0; y <= y1; y++) {
    float dy = y + 0.5f - CX, dy2 = dy * dy;
    if (dy2 >= ro2) continue;
    float ho = sqrtf(ro2 - dy2), hi = dy2 < ri2 ? sqrtf(ri2 - dy2) : 0;
    uint16_t *row = fb + y * LCD_WIDTH;
    for (int side = 0; side < 2; side++) {
      int xa = side ? (int)(CX + hi) : (int)(CX - ho) - 1;
      int xb = side ? (int)(CX + ho) + 1 : (int)(CX - hi) + 1;
      xa = max(xa, 0); xb = min(xb, LCD_WIDTH - 1);
      for (int x = xa; x <= xb; x++) {
        float dx = x + 0.5f - CX;
        float e0 = s0 * dy + c0 * dx;                    // 到起点那条半径线的距离（在弧这边为正）
        float e1 = -(s1 * dy + c1 * dx);                 // 到终点那条半径线的距离（在弧这边为正）
        if (e0 < -0.5f || e1 < -0.5f) continue;
        float r = sqrtf(dx * dx + dy2);
        float a = fminf(fminf(r - rIn, rOut - r), fminf(e0, e1)) + 0.5f;
        if (a <= 0) continue;
        int t = a >= 1 ? alpha : (int)(a * alpha);
        if (t <= 0) continue;
        uint16_t &px = row[x];
        px = t >= 255 ? c : blend565(px, c, t);
      }
    }
  }
}

// v7.4 路程进度：0~1（走过的比例）；不显示时返回 -1
#define TRIP_DONE_C 0x5AEB          // 走过的：灰
#define TRIP_LEFT_C 0x26E8          // 没走的：绿 #22DD44
#define TRIP_H      2               // 行程环半宽（v7.5 整环 8 → 4 像素粗）
float tripFrac() {
  if (!progOn || !tripOn || cruiseOn || progLastRm < 0) return -1;
  long total = progDone + tripRemainM;
  if (total < 300) return -1;                            // 太短的路线不显示
  return tripProgress.value(millis());
}

// 行程环上“现在的位置”小白点
void drawTripDot(float p) {
  if (p <= 0.002f || p >= 0.998f) return;
  float a = p * 2 * PI;
  float x = CX + RING_R * sinf(a), y = CX - RING_R * cosf(a);
  aaDisc(x, y, 5.0f, C_BLACK);                           // v7.5 环变细了，小白点保持原来大小
  aaDisc(x, y, 3.5f, C_WHITE);
}

// 大箭头画面用：整圈行程环
void drawTripRing(float p) {
  fillRingBand(0, 1, RING_R - TRIP_H, RING_R + TRIP_H, TRIP_LEFT_C);
  if (p > 0.002f) fillRingBand(0, p, RING_R - TRIP_H, RING_R + TRIP_H, TRIP_DONE_C);
  drawTripDot(p);
}

// 屏幕边缘的红圈（断线、数据中断用）
void drawRedRing() {
  fillRingBand(0, 1, 222, 232, C_RED);      // v1.8.3 抗锯齿
}

// ================= NavMap 段 09（源 MotoNav.cpp 1666-1854 行） =================

// ================= 车速和限速牌 =================

void drawSpeedBlock(bool over) {
  if (speedKmh < 0) return;
  int sx = hasCam ? 200 : CX;
  char sb[12]; snprintf(sb, sizeof(sb), "%d", speedKmh);
  printCentered(sb, sx, SPEED_Y, 5, over ? C_RED : C_WHITE);   // 每个字符 30x40
  printCentered("km/h", sx, SPEED_Y + 46, 2, over ? 0xFC10 : C_GREY);
  if (hasCam) {
    const int bx = 298, by = 76;
    aaDisc(bx, by, 28, C_SIGNRED);
    aaDisc(bx, by, 21, C_WHITE);
    char lim[8]; snprintf(lim, sizeof(lim), "%d", camLimit);
    int sz = strlen(lim) >= 3 ? 2 : 3;
    printCentered(lim, bx, by - 4 * sz, sz, C_BLACK);
    char db[12]; distTextBuf(camDist, db);
    printCentered(db, bx, by + 34, 2, over ? 0xFC10 : C_GREY);
  }
}

// ================= 车道 =================

// 高德车道方向码 → 方向位：1 直行，2 左转，4 右转，8 左掉头，16 右掉头
// 注意：这张对照表来自经验，可能需要实测校准（App 上会显示原始编码）
uint8_t laneFlags(uint8_t code) {
  switch (code) {
    case 0:  return 1;
    case 1:  return 2;
    case 2:  return 1 | 2;
    case 3:  return 4;
    case 4:  return 1 | 4;
    case 5:  return 8;
    case 6:  return 2 | 4;
    case 7:  return 1 | 2 | 4;
    case 8:  return 16;
    case 9:  return 1 | 8;
    case 10: return 1 | 16;
    case 11: return 2 | 8;
    case 12: return 4 | 16;
    default: return 1;
  }
}

// v3.4 车道图标改成抗锯齿：先把线段/三角形“记录”下来（和大箭头同一套），按每个像素的覆盖率
//      算好颜色和透明度存进车道缓存；车道不变就直接用缓存，每帧和地图混合（边缘平滑，拐角是圆弧）
#define LANE_BUF_W 320
#define LANE_BUF_H 44
uint16_t *laneCol = nullptr;
uint8_t *laneA = nullptr;
int laneBX = 0, laneBY = 0, laneBW = 0, laneBH = 0;
uint32_t laneHash = 0;

// 画一段圆弧（用几小段线连起来）；角度：0 = 朝右，90 = 朝下（屏幕坐标）
void arcLine(float ox, float oy, float r, float a0, float a1, float w, uint16_t col) {
  const int N = 4;
  float px = ox + r * cosf(a0 * PI / 180), py = oy + r * sinf(a0 * PI / 180);
  for (int i = 1; i <= N; i++) {
    float a = (a0 + (a1 - a0) * i / N) * PI / 180;
    float nx = ox + r * cosf(a), ny = oy + r * sinf(a);
    thickLine(px, py, nx, ny, w, col);
    px = nx; py = ny;
  }
}

// 在一条车道里画方向符号（只记录图形）；cx = 车道中心 x，y0 = 顶部，h = 高度
void drawLaneGlyph(float cx, float y0, float h, uint8_t flags, uint16_t col) {
  const float k = h / 32.0f;
  const float w = 4.6f * k, hs = 6.8f * k;
  const float base = y0 + h, top = y0 + 3 * k;
  if (flags & 1) {                                         // 直行
    thickLine(cx, base, cx, top + hs, w, col);
    arrowHead(cx, top + hs, 0, hs, col);
  }
  if (flags & (2 | 4)) {                                   // 左转/右转：竖线 → 圆弧 → 横线 → 箭头
    const float rr = 4.5f * k, arm = 9.0f * k;
    float cy = y0 + h * ((flags & 1) ? 0.6f : 0.42f);     // 和直行合在一起时拐点放低
    thickLine(cx, base, cx, cy + rr, w, col);
    if (flags & 4) {
      arcLine(cx + rr, cy + rr, rr, 180, 270, w, col);
      thickLine(cx + rr, cy, cx + arm, cy, w, col);
      arrowHead(cx + arm, cy, 90, hs, col);
    }
    if (flags & 2) {
      arcLine(cx - rr, cy + rr, rr, 0, -90, w, col);
      thickLine(cx - rr, cy, cx - arm, cy, w, col);
      arrowHead(cx - arm, cy, -90, hs, col);
    }
  }
  if (flags & (8 | 16)) {                                  // 掉头：竖线 → 半圆 → 向下 + 箭头
    const float u = 4.2f * k, ut = top + 6 * k + u, down = y0 + h * 0.62f;
    thickLine(cx, base, cx, ut, w, col);
    if (flags & 8) {
      arcLine(cx - u, ut, u, 0, -180, w, col);
      thickLine(cx - 2 * u, ut, cx - 2 * u, down - hs, w, col);
      arrowHead(cx - 2 * u, down - hs, 180, hs, col);
    }
    if (flags & 16) {
      arcLine(cx + u, ut, u, 180, 360, w, col);
      thickLine(cx + 2 * u, ut, cx + 2 * u, down - hs, w, col);
      arrowHead(cx + 2 * u, down - hs, 180, hs, col);
    }
  }
  if (!(flags & (1 | 2 | 4 | 8 | 16))) {                   // 没有方向位时画直行
    thickLine(cx, base, cx, top + hs, w, col);
    arrowHead(cx, top + hs, 0, hs, col);
  }
}

// 把记录好的车道图形算成“颜色 + 透明度”存进缓存
void laneRender() {
  float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
  for (int i = 0; i < primN; i++) {
    bx0 = fminf(bx0, prims[i].x0); by0 = fminf(by0, prims[i].y0);
    bx1 = fmaxf(bx1, prims[i].x1); by1 = fmaxf(by1, prims[i].y1);
  }
  laneBX = max(0, (int)floorf(bx0)); laneBY = max(0, (int)floorf(by0));
  laneBW = min(LANE_BUF_W, (int)ceilf(bx1) + 1 - laneBX);
  laneBH = min(LANE_BUF_H, (int)ceilf(by1) + 1 - laneBY);
  if (laneBX + laneBW > LCD_WIDTH) laneBW = LCD_WIDTH - laneBX;
  if (laneBY + laneBH > LCD_HEIGHT) laneBH = LCD_HEIGHT - laneBY;
  if (laneBW <= 0 || laneBH <= 0) { laneBW = laneBH = 0; return; }
  for (int y = 0; y < laneBH; y++) {
    float py = laneBY + y + 0.5f;
    for (int x = 0; x < laneBW; x++) {
      float px = laneBX + x + 0.5f;
      float r = 0, g = 0, b = 0, A = 0;                  // 从透明开始，按顺序叠加每个图形
      for (int i = 0; i < primN; i++) {
        const Prim &p = prims[i];
        if (px < p.x0 || px > p.x1 || py < p.y0 || py > p.y1) continue;
        float a = primAlpha(p, px, py);
        if (a <= 0) continue;
        float cr = ((p.col >> 11) & 0x1F) / 31.0f, cg = ((p.col >> 5) & 0x3F) / 63.0f, cb = (p.col & 0x1F) / 31.0f;
        r += (cr - r) * a; g += (cg - g) * a; b += (cb - b) * a; A += (1 - A) * a;
      }
      int k = y * LANE_BUF_W + x;
      if (A < 0.01f) { laneA[k] = 0; continue; }
      r /= A; g /= A; b /= A;                              // 去掉透明度，得到纯颜色
      laneCol[k] = ((uint16_t)lroundf(fminf(r, 1) * 31) << 11) | ((uint16_t)lroundf(fminf(g, 1) * 63) << 5) | (uint16_t)lroundf(fminf(b, 1) * 31);
      laneA[k] = (uint8_t)lroundf(fminf(A, 1) * 255);
    }
  }
}

void drawLanes(int y0) { drawLanesAt(CX, y0, 300, 32); }

// v2.1 在指定位置画车道：cx = 中心 x，maxW = 最大总宽，h = 高度
void drawLanesAt(float centerX, float y0, float maxW, float h) {
  if (laneN <= 0) return;
  if (!laneCol) {
    laneCol = (uint16_t *)malloc(LANE_BUF_W * LANE_BUF_H * 2);
    laneA = (uint8_t *)malloc(LANE_BUF_W * LANE_BUF_H);
    if (!laneCol || !laneA) return;
  }
  float lw = fminf(36 * h / 32, maxW / laneN);
  float x0 = centerX - lw * laneN / 2;
  // 1. 记录所有车道的图形
  primN = 0; recIcon = true;
  for (int i = 0; i < laneN; i++) {
    float cx = x0 + lw * (i + 0.5f);
    bool rec = laneFr[i] != 0xF;
    drawLaneGlyph(cx, y0, h, laneFlags(laneBg[i]), rec ? C_SEP : C_DIM);   // 底层：这条车道能走的方向
    if (rec) drawLaneGlyph(cx, y0, h, laneFlags(laneFr[i]), C_WHITE);       // 推荐方向：白色
  }
  recIcon = false;
  // 2. 和上次一样就用缓存，否则重新计算
  uint32_t hs = 2166136261u;
  const uint8_t *bytes = (const uint8_t *)prims;
  for (size_t i = 0; i < sizeof(Prim) * primN; i++) { hs ^= bytes[i]; hs *= 16777619u; }
  hs ^= primN * 7919u;
  if (hs != laneHash) { laneRender(); laneHash = hs; }
  primN = 0;
  // 3. 和底下的画面按透明度混合
  uint16_t *fb = gfx->getFramebuffer();
  if (fb) {
    for (int y = 0; y < laneBH; y++) {
      uint16_t *dst = fb + (laneBY + y) * LCD_WIDTH + laneBX;
      const uint16_t *sc = laneCol + y * LANE_BUF_W;
      const uint8_t *sa = laneA + y * LANE_BUF_W;
      for (int x = 0; x < laneBW; x++) {
        uint8_t a = sa[x];
        if (!a) continue;
        dst[x] = a == 255 ? sc[x] : blend565(dst[x], sc[x], a);
      }
    }
  }
  // 4. 车道之间的虚线
  for (int i = 1; i < laneN; i++)
    for (float y = y0; y < y0 + h; y += 8) gfx->drawFastVLine(rnd(x0 + lw * i), rnd(y), 4, C_SEP);
}

// ================= NavMap 段 10（源 MotoNav.cpp 3196-3199 行） =================

// ---- 进度环动画 ----
float ringShown = 0, ringTarget = 0;
bool ringActive = false;
String ringDir = "";

// ================= NavMap 段 11（源 MotoNav.cpp 3201-4382 行） =================

// ================= v2.0 小地图 =================
#define MAP_MAX      (200 * 1024)   // 地图块最大字节数
#define ROUTE_MAX    (96 * 1024)    // 路线最大字节数
#define FEAT_MAX     7200           // 地图块最多要素数
#define ROUTE_PTS    24000          // 路线最多点数
#define MAP_NEAR_M   150            // v2.1 离路口小于这个距离：圆牌放大、显示车道
#define MAP_PY       262            // 车头在屏幕上的 y 位置（偏下，多看前方）
#define POLY_PTS     4096           // 单个要素最多点数（画图用的临时数组）
#define BADGE_X      130            // 左下角转向小圆牌
#define BADGE_Y      334
#define BADGE_R      60

// v2.9 地图配色主题；v3.0 加上“走过的路线”颜色（done 内芯、done2 描边）和夜间配色
struct MapTheme { uint16_t block, road, park, water, route, route2, hud, blip, done, done2; };
#define THEME_N 12
const MapTheme THEMES[THEME_N] = {
  {0xDEFB, 0x3186, 0x5DEB, 0x4C9B, 0xA21E, 0x58F1, 0xFFFF, 0xFE80, 0x94B3, 0x5AEC},   // 0 原版
  {0xE6D9, 0x3187, 0x760D, 0x4E3B, 0xF258, 0x78EC, 0x6F5F, 0xF258, 0xA4B4, 0x62CC},   // 1 罪恶都市：暖沙色街区、青绿水面、粉紫路线
  {0x3A08, 0x9D15, 0x4327, 0x2A8F, 0xA2DF, 0x38EC, 0xFFFF, 0xFE87, 0x738F, 0x18E4},   // 2 GTA5：深色地面、浅色道路、紫色路线
  {0x0021, 0x04B6, 0x0103, 0x1847, 0xF815, 0x5007, 0xB7FF, 0xFF20, 0x39EA, 0x10A3},   // 3 赛博霓虹
  {0xEF18, 0xF5CD, 0xC6B3, 0x9E3A, 0xB0A3, 0x5841, 0xFFFF, 0x298F, 0xA490, 0x6B0A},   // 4 复古纸地图
  {0x0000, 0x39C7, 0x08A1, 0x0863, 0xFFFF, 0x39E7, 0xFFFF, 0xFFFF, 0x528A, 0x2104},   // 5 极简黑白
  {0x094B, 0xDF5F, 0x1A50, 0x2B35, 0xFE87, 0x08E8, 0xFFFF, 0xFE87, 0x6C76, 0x116B},   // 6 蓝图
  {0x8E0E, 0xE7DA, 0x334A, 0x08C4, 0x08C4, 0xE7DA, 0xE7DA, 0x08C4, 0x334A, 0x8E0E},   // 7 掌机像素
  {0x00C1, 0x0B46, 0x0183, 0x0084, 0x57F0, 0x01C2, 0x97F5, 0xCFF9, 0x1AC6, 0x0101},   // 8 雷达
  {0x0863, 0x29AA, 0x08C4, 0x08A6, 0xFD80, 0x3940, 0xFF35, 0xFD80, 0x6AC7, 0x18A1},   // 9 寻宝罗盘
  {0x5B08, 0x8C50, 0x3285, 0x29E8, 0x073F, 0x0188, 0xFFFF, 0xF9E7, 0xA514, 0x3186},   // 10 卫星图（照片没到时的替代色）
  {DragonRadar::DayBackground, 0, 0, 0, 0xFFF0, 0x0941, 0xFFFF, 0xFDC4, 0x7D69, 0x1283}, // 11 龙珠雷达（无底图）
};
// v3.0 夜间配色：地面压暗、道路变成浅色线条，路线保持鲜艳（晚上一眼就能看到）
const MapTheme THEMES_NIGHT[THEME_N] = {
  {0x18E5, 0x5B0E, 0x19E5, 0x118B, 0xA21E, 0x2848, 0xFFFF, 0xFE80, 0x4A8B, 0x0862},   // 0 原版·夜：深蓝灰
  {0x20C7, 0x6271, 0x1207, 0x0A2B, 0xF258, 0x5047, 0x6F5F, 0xF258, 0x7370, 0x1063},   // 1 罪恶都市·夜：深紫街区、霓虹粉路线
  {0x10C3, 0x5AED, 0x1163, 0x1107, 0xAB3F, 0x30AB, 0xEF5D, 0xFE87, 0x4229, 0x0841},   // 2 GTA5·夜：更深的地面
  {0x0021, 0x04B6, 0x0103, 0x1847, 0xF815, 0x5007, 0xB7FF, 0xFF20, 0x39EA, 0x10A3},   // 3 赛博霓虹·夜
  {0x0021, 0x04B6, 0x0103, 0x1847, 0xF815, 0x5007, 0xB7FF, 0xFF20, 0x39EA, 0x10A3},   // 4 复古纸地图·夜 = 霓虹
  {0x0000, 0x39C7, 0x08A1, 0x0863, 0xFFFF, 0x39E7, 0xFFFF, 0xFFFF, 0x528A, 0x2104},   // 5 极简黑白·夜
  {0x00C7, 0xAE3D, 0x118B, 0x1A50, 0xFE87, 0x00A5, 0xFFFF, 0xFE87, 0x4B52, 0x08E7},   // 6 蓝图·夜
  {0x334A, 0x8E0E, 0x08C4, 0x08C4, 0xE7DA, 0x08C4, 0xE7DA, 0xE7DA, 0x334A, 0x08C4},   // 7 掌机像素·夜
  {0x00C1, 0x0B46, 0x0183, 0x0084, 0x57F0, 0x01C2, 0x97F5, 0xCFF9, 0x1AC6, 0x0101},   // 8 雷达·夜
  {0x0863, 0x29AA, 0x08C4, 0x08A6, 0xFD80, 0x3940, 0xFF35, 0xFD80, 0x6AC7, 0x18A1},   // 9 寻宝罗盘·夜
  {0x5B08, 0x8C50, 0x3285, 0x29E8, 0x073F, 0x0188, 0xFFFF, 0xF9E7, 0xA514, 0x3186},   // 10 卫星图·夜（照片压暗）
  {DragonRadar::NightBackground, 0, 0, 0, 0xFFF0, 0x0941, 0xFFFF, 0xFDC4, 0x7D69, 0x1283}, // 11 龙珠雷达（无底图）
};
int themeIdx = 1;                                  // 默认罪恶都市
bool curveOn = false;                              // v6.1 弯道急缓上色
#define ST_NEON 3
#define ST_PAPER 4
#define ST_MINIMAL 5
#define ST_BLUEPRINT 6
#define ST_PIXEL 7
#define ST_RADAR 8
#define ST_COMPASS 9
#define ST_SAT 10
#define ST_DRAGON 11
bool nightOn = false;                              // v3.0 夜间配色（手机按日出日落发 "#n,1/0"）
int trailMode = 1;                                 // v3.0 走过的路线：1 变灰，2 擦除（"#r,1/2"）
// v6.1 实际用的风格：复古纸地图到了夜间配色时换成霓虹
static inline int styleIdx() { return (nightOn && themeIdx == ST_PAPER) ? ST_NEON : themeIdx; }
#define TH       ((nightOn ? THEMES_NIGHT : THEMES)[themeIdx])
#define C_BLOCK  (TH.block)
#define C_ROADM  (TH.road)
#define C_PARK   (TH.park)
#define C_WATER  (TH.water)
#define C_ROUTE  (TH.route)
#define C_ROUTE2 (TH.route2)

static inline int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | (p[1] << 8)); }
static inline uint16_t rdu16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline int32_t rd32(const uint8_t *p) { return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }

// ---- 大块数据接收（'M'）----
struct Bulk {
  uint8_t *buf; uint32_t cap;
  int id; uint32_t total; uint16_t parts; uint16_t got; bool done;
  uint8_t mask[512];        // 每片一位，最多 4096 片
};
Bulk bulks[6];              // 1~4 地图/路线；5 = 独立圆屏背景图
#define BG_MAX_BYTES (360 * 360 * 2 + 4)
uint16_t *backgroundPixels = nullptr;
int backgroundW = 0, backgroundH = 0;
bool backgroundValid = false;
void drawBackground();

// ---- v4.4 原路线（导入的高德分享路线）：导航时画成一条淡色细线，走错路时能看到原来该走哪 ----
#define GHOST_PTS 6000
#define GHOST_MAX (12 + 4 * GHOST_PTS + 64)
int32_t *ghostX = nullptr, *ghostY = nullptr, *gbBox = nullptr;
int ghostN = 0, ghostFrame = -1;

// ---- v2.5 图片地图（天地图）----
#include "RasterContinuity.h"
#define MAP_RX_LOG(...) do {} while (0)  // 接收链路已确认，默认关闭详细分片日志
// 建筑物像素普查只服务诊断日志：最多要全扫 ~1.7M 像素，收图时会把渲染主循环卡住几十上百毫秒。
// 默认关闭；要核查建筑物数据时改成 1 再编译。
#define MAP_BLD_LOG 0
#define RASTER_MAX_BYTES (500 * 1024)   // 压缩后最大字节数
#define RASTER_MAX_PX    (1300 * 1300)  // 最多像素数
#define RASTER_BUF_BYTES (((1300 + 15) / 16) * ((1300 + 15) / 16) * 128)   // v2.6 按 16×16 小块存放
int rasBW = 0;                          // 每行多少个小块
uint8_t *rasterBuf = nullptr;           // 每像素 4 位颜色编号
bool rasterValid = false;
bool rasterBuildings = false;
bool rasterHeights = false;        // v2 楼块高度格式（CAP,BLD,2）：类 8-15，低 2 位 = 高度档 0..3，bit2 = 绿地/水基底
bool bld3dOn = true;               // v7.13 3D 楼块显示开关（App 设置页 #B3D,0/1）；关 = 俯视平铺（2D）
static void bldBlocksRebuild();    // 定义在 rasterRows 前；解析收齐地图后重建 3D 楼块块位图
uint8_t *rasterBackBuf = nullptr;
RasterHeader rasterBackHeader = {};
bool rasterBackValid = false;

int rasterFrame = -1;
int32_t rasX0 = 0, rasY0 = 0;           // 左上角（米）
int rasW = 0, rasH = 0;
float rasMpp = 2.0f;                    // 每像素多少米
unsigned long rasterAt = 0, vectorAt = 0;
bool pendingBuildingCapability = false;
String pendingAck = "";     // 待发给手机的确认

// ---- 地图块（正在显示的）----
uint8_t *mapShow = nullptr;
bool mapValid = false;
int mapFrame = -1;
int32_t mapOx = 0, mapOy = 0;     // 地图块原点（米，手机坐标系）
int mapFeatN = 0;
uint32_t *featOff = nullptr;      // 每个要素的点数据在 mapShow 里的位置
uint16_t *featNp = nullptr;       // 点数
uint8_t *featCls = nullptr;       // 类别（最高位 = 面）
int16_t *featBox = nullptr;       // 外框 minx,miny,maxx,maxy（相对原点，米）

// ---- 路线 ----
int32_t *routeX = nullptr, *routeY = nullptr;
int routeN = 0, routeFrame = -1;
int32_t *rbBox = nullptr;         // 每 32 个点一组的外框
#define RB_STEP 32
#define LIGHT_MAX 600
int32_t *lightX = nullptr, *lightY = nullptr;   // v3.2 路线上的红绿灯（米）
float *lightU = nullptr;                        // v3.2 红绿灯在路线上的位置（段号 + 段内比例）
int lightN = 0;
float routeProg = -1;             // v3.0 车在路线上走到哪了：段号 + 段内比例（-1 = 还没对上）
float *routeCum = nullptr;        // v5.0 每个路线点离起点的路程（米），给路况颜色用
uint8_t *routeCurv = nullptr;     // v6.1 每个路线点的弯急程度：0 直/缓，1 中弯，2 急弯
// v5.0 路况：缓行/拥堵的路段（离起点的米数），"#u,起,止,状态,..."；状态 2 缓行 3 拥堵 4 严重拥堵
#define TRAF_MAX 40
int trafN = 0;
int32_t trafA[TRAF_MAX], trafB[TRAF_MAX];
uint8_t trafS[TRAF_MAX];
// v5.0 提示条：重新规划中（"#y,1/0"）、更快路线（"#h,省几分钟" / "#h"）、GPS 信号弱（"#x,1/0"）
unsigned long rerouteAt = 0, fasterAt = 0;
int fasterMin = 0;
bool gpsWeak = false;
unsigned long routeSearchAt = 0;  // v3.0 上次全路线搜索的时间

// ---- 当前位置 ----
bool mapOn = true;
int posFrame = -1;
float posX = 0, posY = 0, posHdg = 0, posKmh = 0;
unsigned long posAt = 0;
float viewHdg = 0, viewScale = 0.7f;
float corrX = 0, corrY = 0;          // v2.6 位置偏差（米），逐渐减小到 0
unsigned long corrAt = 0;
float hdgRate = 0;                   // v6.7 正在转弯的速度（度/秒，顺时针为正）
int leadMs = 300;                    // v6.7 预判提前量（毫秒）
float viewTargetHdg = 0;             // v6.7 地图朝向要追的目标（预判后的朝向）

// v6.7 推算 t 时刻（再加 lead 秒）的位置和朝向：按当前车速和转弯速度沿弧线往前推（最多推 1.5 秒）
void predictPose(unsigned long t, float lead, float &x, float &y, float &h) {
  float tau = fminf((t - posAt) / 1000.0f + lead, 1.5f);
  if (tau < 0) tau = 0;
  float v = posKmh / 3.6f;
  float rate = (t - posAt) > 2500 ? 0 : hdgRate;        // 很久没新定位（停车等）：不再按转弯推
  float hEnd = posHdg + rate * tau;
  float hMid = (posHdg + hEnd) * 0.5f * PI / 180;      // 弧线用中间的朝向近似
  x = posX + sinf(hMid) * v * tau;
  y = posY + cosf(hMid) * v * tau;
  h = hEnd;
}
bool viewInit = false;
unsigned long lastMapDraw = 0;

// 画图用的临时数组（内部内存，速度快）
float *tmpX = nullptr, *tmpY = nullptr;
bool mapAllocOk = false;          // mapInit 里全部缓冲都分配成功才为 true；否则丢弃地图数据、跳过地图绘制

void mapInit() {
  bulks[1].cap = MAP_MAX;   bulks[1].buf = (uint8_t *)ps_malloc(MAP_MAX);
  bulks[2].cap = ROUTE_MAX; bulks[2].buf = (uint8_t *)ps_malloc(ROUTE_MAX);
  bulks[3].cap = RASTER_MAX_BYTES; bulks[3].buf = (uint8_t *)ps_malloc(RASTER_MAX_BYTES);
  bulks[4].cap = GHOST_MAX; bulks[4].buf = (uint8_t *)ps_malloc(GHOST_MAX);          // v4.4
  bulks[5].cap = BG_MAX_BYTES; bulks[5].buf = (uint8_t *)ps_malloc(BG_MAX_BYTES);
  ghostX = (int32_t *)ps_malloc(sizeof(int32_t) * GHOST_PTS);
  ghostY = (int32_t *)ps_malloc(sizeof(int32_t) * GHOST_PTS);
  gbBox  = (int32_t *)ps_malloc(sizeof(int32_t) * 4 * (GHOST_PTS / RB_STEP + 2));
  rasterBuf = (uint8_t *)ps_malloc(RASTER_BUF_BYTES);
  for (int t = 1; t <= 5; t++) { bulks[t].id = -1; bulks[t].done = false; }
  mapShow = (uint8_t *)ps_malloc(MAP_MAX);
  featOff = (uint32_t *)ps_malloc(sizeof(uint32_t) * FEAT_MAX);
  featNp  = (uint16_t *)ps_malloc(sizeof(uint16_t) * FEAT_MAX);
  featCls = (uint8_t *)ps_malloc(FEAT_MAX);
  featBox = (int16_t *)ps_malloc(sizeof(int16_t) * 4 * FEAT_MAX);
  routeX = (int32_t *)ps_malloc(sizeof(int32_t) * ROUTE_PTS);
  routeY = (int32_t *)ps_malloc(sizeof(int32_t) * ROUTE_PTS);
  rbBox  = (int32_t *)ps_malloc(sizeof(int32_t) * 4 * (ROUTE_PTS / RB_STEP + 2));
  routeCum = (float *)ps_malloc(sizeof(float) * ROUTE_PTS);                         // v5.0
  routeCurv = (uint8_t *)ps_malloc(ROUTE_PTS);                                        // v6.1
  lightX = (int32_t *)ps_malloc(sizeof(int32_t) * LIGHT_MAX);
  lightY = (int32_t *)ps_malloc(sizeof(int32_t) * LIGHT_MAX);
  lightU = (float *)ps_malloc(sizeof(float) * LIGHT_MAX);
  tmpX = (float *)malloc(sizeof(float) * POLY_PTS);
  tmpY = (float *)malloc(sizeof(float) * POLY_PTS);
  mapAllocOk = bulks[1].buf && bulks[2].buf && bulks[3].buf && bulks[4].buf && bulks[5].buf &&
      ghostX && ghostY && gbBox && rasterBuf && mapShow && featOff && featNp && featCls && featBox &&
      routeX && routeY && rbBox && routeCum && routeCurv && lightX && lightY && lightU && tmpX && tmpY;
  if (!mapAllocOk) Serial.println("[MAP] buffer allocation incomplete; map data will be dropped");
}

// 解析刚收齐的地图块，建立索引
void parseMap(const uint8_t *p, uint32_t len) {
  mapValid = false;
  if (len < 12) return;
  int frame = rdu16(p);
  int32_t ox = rd32(p + 2), oy = rd32(p + 6);
  int n = rdu16(p + 10);
  uint32_t pos = 12;
  int cnt = 0;
  for (int i = 0; i < n && cnt < FEAT_MAX; i++) {
    if (pos + 3 > len) break;
    uint8_t cls = p[pos];
    uint16_t np = rdu16(p + pos + 1);
    pos += 3;
    if (pos + (uint32_t)np * 4 > len) break;
    int16_t x0 = 32767, y0 = 32767, x1 = -32768, y1 = -32768;
    for (int k = 0; k < np; k++) {
      int16_t x = rd16(p + pos + 4 * k), y = rd16(p + pos + 4 * k + 2);
      if (x < x0) x0 = x;
      if (x > x1) x1 = x;
      if (y < y0) y0 = y;
      if (y > y1) y1 = y;
    }
    featCls[cnt] = cls; featNp[cnt] = np; featOff[cnt] = pos;
    featBox[4 * cnt] = x0; featBox[4 * cnt + 1] = y0; featBox[4 * cnt + 2] = x1; featBox[4 * cnt + 3] = y1;
    cnt++;
    pos += (uint32_t)np * 4;
  }
  mapFeatN = cnt; mapOx = ox; mapOy = oy; mapFrame = frame;
  mapValid = cnt > 0;
  vectorAt = millis();
}

// v2.5 解析图片地图：u16 坐标系, i32 左上角 x, i32 左上角 y, u16 宽, u16 高, u16 每像素毫米, 然后是压缩的颜色编号
uint16_t blendF(uint16_t a, uint16_t b, uint32_t t);
bool rasHasPal = false;                    // v6.1 卫星图自带调色板
uint16_t rasPal[16];

// 罪恶都市局部边界几何；其它风格仍读取原来的 4 位材质图。
uint16_t *rasterVice = nullptr;
size_t rasterAllocatedBytes = RASTER_BUF_BYTES;
bool rasterViceValid = false;
tinfl_decompressor *viceInflater = nullptr;
uint16_t viceEdgePal[7][257];
uint16_t vicePalKey[4] = {0xffff, 0xffff, 0xffff, 0xffff};
// 覆盖率按像素方形与半平面的相交面积计算。利用镜像对称，只需保存九个角度的表。
int8_t viceNX[64], viceNY[64];
int32_t viceOffsets[2][32];
uint16_t viceCoverage[9][257];
bool rasterViceFine = false;
uint8_t viceBaseShift = 13;
uint8_t rasterViceVersion = 0;
const uint16_t *vicePatchData = nullptr;
uint16_t vicePatchCount = 0;

void initViceSampling() {
  static bool ready = false;
  if (ready) return;
  for (int i = 0; i < 64; i++) {
    float a = i * (2.0f * PI / 64.0f);
    viceNX[i] = (int8_t)lroundf(cosf(a) * 127);
    viceNY[i] = (int8_t)lroundf(sinf(a) * 127);
  }
  const float offsets[16] = {0.0f, 0.0625f, 0.125f, 0.1875f,
    0.25f, 0.3125f, 0.375f, 0.4375f, 0.5f, 0.5625f,
    0.625f, 0.6875f, 0.8125f, 1.0f, 1.25f, 1.625f};
  for (int i = 0; i < 32; i++) {
    viceOffsets[0][i] = (i < 16 ? i : i - 32) * 4064;
    viceOffsets[1][i] = (int32_t)lroundf(offsets[i & 15] * (i & 16 ? -32512.0f : 32512.0f));
  }
  for (int angle = 0; angle <= 8; angle++) {
    float a = angle * (PI / 32.0f);
    float major = cosf(a), minor = sinf(a);
    for (int i = 0; i <= 256; i++) {
      float distance = (i - 128) * (1.5f / 256.0f);
      float t = distance + (major + minor) * 0.5f;
      float coverage;
      if (t <= 0) coverage = 0;
      else if (t >= major + minor) coverage = 1;
      else if (minor < 0.00001f) coverage = t / major;
      else if (t < minor) coverage = t * t / (2 * major * minor);
      else if (t <= major) coverage = (t - minor * 0.5f) / major;
      else { float tail = major + minor - t; coverage = 1 - tail * tail / (2 * major * minor); }
      viceCoverage[angle][i] = (uint16_t)max(0, min(256, (int)lroundf(coverage * 256.0f)));
    }
  }
  ready = true;
}

static inline uint8_t viceCoverageAngle(float nx, float ny) {
  int angle = (int)lroundf(atan2f(fabsf(ny), fabsf(nx)) * (32.0f / PI));
  if (angle > 8) angle = 16 - angle;
  return (uint8_t)max(0, min(8, angle));
}


void parseViceGeometry(const uint8_t *p, uint32_t len, uint32_t pos, int bw, int bh) {
  rasterViceValid = false;
  vicePatchCount = 0; vicePatchData = nullptr;
  if (pos <= len && len - pos >= 8 && p[pos] == 'V' && p[pos + 1] == 'C' && p[pos + 2] == 'E' &&
      p[pos + 3] >= 1 && p[pos + 3] <= 3) {
    uint8_t version = p[pos + 3];
    uint32_t packed = (uint32_t)rd32(p + pos + 4);
    uint32_t header = version == 3 ? 12 : 8;
    if (len - pos < header) return;
    uint16_t patchCount = version == 3 ? rdu16(p + pos + 8) : 0;
    if (patchCount > 2047 || (version == 3 && rdu16(p + pos + 10) != 0)) return;
    size_t cellsBytes = (size_t)bw * bh * 512;
    size_t needed = cellsBytes + (size_t)patchCount * 8;
    if (packed == len - pos - header && needed <= RASTER_BUF_BYTES * 4 + 2047 * 8) {
      if (rasterAllocatedBytes != needed) {
        free(rasterBuf);
        rasterBuf = (uint8_t *)ps_malloc(needed);
        rasterAllocatedBytes = rasterBuf ? needed : 0;
      }
      rasterVice = (uint16_t *)rasterBuf;
      if (!viceInflater) viceInflater = (tinfl_decompressor *)malloc(sizeof(tinfl_decompressor));
      if (rasterVice && rasterAllocatedBytes == needed && viceInflater) {
        tinfl_init(viceInflater);
        size_t inBytes = packed, outBytes = needed;
        tinfl_status result = tinfl_decompress(viceInflater, p + pos + header, &inBytes,
            (uint8_t *)rasterVice, (uint8_t *)rasterVice, &outBytes,
            TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        rasterViceValid = result == TINFL_STATUS_DONE && inBytes == packed && outBytes == needed;
        const uint16_t *vertices = (const uint16_t *)(rasterBuf + cellsBytes);
        if (rasterViceValid) {
          for (uint32_t i = 0; i < (uint32_t)patchCount * 4; i++) {
            uint16_t v = vertices[i];
            if ((v & 15) + ((v >> 4) & 15) + ((v >> 8) & 15) + (v >> 12) != 15) {
              rasterViceValid = false; break;
            }
          }
        }
        if (rasterViceValid) {
          rasterViceVersion = version;
          rasterViceFine = version >= 2;
          viceBaseShift = rasterViceFine ? 14 : 13;
          vicePatchCount = patchCount; vicePatchData = patchCount ? vertices : nullptr;
        }
      }
    }
  }
  if (!rasterViceValid) rasterVice = nullptr;
}

void prepareVicePalette(const uint16_t *pal) {
  static bool initialized = false;
  initViceSampling();
  bool changed = !initialized;
  for (int i = 0; i < 4; i++) if (vicePalKey[i] != pal[i]) changed = true;
  if (!changed) return;
  const uint8_t pairA[7] = {0, 0, 0, 0, 1, 1, 2};
  const uint8_t pairB[7] = {0, 1, 2, 3, 2, 3, 3};
  for (int p = 1; p <= 6; p++) for (int a = 0; a <= 256; a++)
    viceEdgePal[p][a] = blendF(pal[pairA[p]], pal[pairB[p]], a);
  memcpy(vicePalKey, pal, sizeof(vicePalKey));
  initialized = true;
}

void parseRasterLegacy(const uint8_t *p, uint32_t len) {
  rasterValid = false;
  rasterViceValid = false;
  if (len < 16) return;
  int frame = rdu16(p);
  int32_t x0 = rd32(p + 2), y0 = rd32(p + 6);
  int w = rdu16(p + 10), h = rdu16(p + 12), mm = rdu16(p + 14);
  bool hasPal = (mm & 0x8000) != 0;
  mm &= 0x7FFF;
  uint16_t palNew[16];
  if (hasPal) {
    if (len < 48) return;
    for (int i = 0; i < 16; i++) palNew[i] = rdu16(p + 16 + 2 * i);
  }
  uint32_t n = (uint32_t)w * h;
  if (w == 0 || h == 0 || n > RASTER_MAX_PX || mm == 0) return;
  int bw = (w + 15) >> 4, bh = (h + 15) >> 4;
  size_t legacyBytes = (size_t)bw * bh * 128;
  if (legacyBytes > RASTER_BUF_BYTES) return;
  uint32_t dataStart = hasPal ? 48 : 16;
  // 先检查普通图的游程长度，找到扩展的位置；不另存一份解压后的普通图。
  uint32_t pos = dataStart, i = 0;
  while (pos < len && i < n) {
    uint8_t b = p[pos++], r = b >> 4;
    uint32_t run = r + 1;
    if (r == 15) { if (pos >= len) return; run = 16 + p[pos++]; }
    if (run > n - i) return;
    i += run;
  }
  if (i != n) return;
  if (!hasPal) parseViceGeometry(p, len, pos, bw, bh);
  if (!rasterViceValid) {
    // 不带扩展的地图、卫星图或扩展解码失败，回到原来的 4 位格式。
    if (!rasterBuf || rasterAllocatedBytes != RASTER_BUF_BYTES) {
      free(rasterBuf);
      rasterBuf = (uint8_t *)ps_malloc(RASTER_BUF_BYTES);
      rasterAllocatedBytes = rasterBuf ? RASTER_BUF_BYTES : 0;
    }
    rasterVice = nullptr;
    if (!rasterBuf) return;
    memset(rasterBuf, 0, legacyBytes);
    pos = dataStart; i = 0;
    int x = 0, y = 0;
    while (i < n) {
      uint8_t b = p[pos++], c = b & 15, r = b >> 4;
      uint32_t run = r + 1;
      if (r == 15) run = 16 + p[pos++];
      if (c) {
        for (uint32_t k = 0; k < run; k++) {
          uint32_t a = ((uint32_t)((y >> 4) * bw + (x >> 4)) << 8) | ((y & 15) << 4) | (x & 15);
          rasterBuf[a >> 1] |= (a & 1) ? (c << 4) : c;
          if (++x == w) { x = 0; y++; }
        }
      } else {
        x += run;
        while (x >= w) { x -= w; y++; }
      }
      i += run;
    }
  }
  rasBW = bw;
  rasX0 = x0; rasY0 = y0; rasW = w; rasH = h; rasMpp = mm / 1000.0f; rasterFrame = frame;
  rasHasPal = hasPal;
  if (hasPal) memcpy(rasPal, palNew, sizeof(rasPal));
  rasterBuildings = false;                // 兼容路径不带楼块
  rasterHeights = false;
  bldBlocksRebuild();
  rasterValid = true;
  rasterAt = millis();
}

// 普通地图先在备用缓冲解码，成功后交换；上一块保留，用来补齐新块的覆盖边缘。
bool parseRaster(const uint8_t *p, uint32_t len) {
  RasterHeader next;
  if (!rasterCheck(p,len,next,RASTER_MAX_PX,RASTER_BUF_BYTES)) {
    if (len >= 16) {
      Serial.printf("[MAP-DEC] invalid header/stream len=%u frame=%u size=%ux%u flags=0x%04x heap=%u psram=%u\n",
                    (unsigned)len, rasterU16(p), rasterU16(p+10), rasterU16(p+12), rasterU16(p+14),
                    ESP.getFreeHeap(), ESP.getFreePsram());
    } else {
      Serial.printf("[MAP-DEC] truncated packet len=%u (<16) heap=%u psram=%u\n",
                    (unsigned)len, ESP.getFreeHeap(), ESP.getFreePsram());
    }
    return false;
  }
  if (next.dataEnd != len) {
    // 保留既有轮廓扩展的兼容路径；新 App 只发送无扩展的普通底图。
    if (len-next.dataEnd<8 || p[next.dataEnd]!='V' || p[next.dataEnd+1]!='C' || p[next.dataEnd+2]!='E') {
      Serial.printf("[MAP-DEC] trailing data len=%u decoded_end=%u\n", (unsigned)len, (unsigned)next.dataEnd);
      return false;
    }
    rasterBackValid=false;
    parseRasterLegacy(p,len);
    return rasterValid;
  }
  if (!rasterBackBuf) rasterBackBuf=(uint8_t *)ps_malloc(RASTER_BUF_BYTES);
  if (!rasterBackBuf) {
    Serial.printf("[MAP-DEC] PSRAM allocation failed bytes=%u free_psram=%u\n",
                  (unsigned)RASTER_BUF_BYTES, ESP.getFreePsram());
    return false;
  }
  rasterDecode(p,next,rasterBackBuf);
#if MAP_BLD_LOG
  if (next.buildings && !next.palette) {
    uint32_t buildingPixels = 0, buildingCells = 0;
    const uint32_t pixelCount = (uint32_t)next.width * next.height;
    for (uint32_t py = 0; py < next.height; py++) {
      for (uint32_t px = 0; px < next.width; px++) {
        const uint32_t a = ((py >> 4) * next.blocksWide + (px >> 4)) * 256u + ((py & 15) << 4) + (px & 15);
        const uint8_t packed = rasterBackBuf[a >> 1];
        const uint8_t cls = (a & 1) ? (packed >> 4) : (packed & 15);
        if (cls >= 4 && cls <= 7) buildingPixels++;
      }
    }
    for (uint32_t cell = 0; cell < (uint32_t)next.blocksWide * ((next.height + 15) >> 4); cell++) {
      bool hasRoof = false;
      const uint32_t base = cell * 256u;
      for (uint32_t k = 0; k < 256; k++) {
        const uint8_t packed = rasterBackBuf[(base + k) >> 1];
        const uint8_t cls = ((k & 1) ? (packed >> 4) : (packed & 15));
        if (cls >= 4 && cls <= 7) { hasRoof = true; break; }
      }
      if (hasRoof) buildingCells++;
    }
    Serial.printf("[MAP-BLD] frame=%u flag=1 pixels=%u/%u cells=%u/%u raster=%ux%u mpp=%.3f\n",
                  next.frame, (unsigned)buildingPixels, (unsigned)pixelCount,
                  (unsigned)buildingCells, (unsigned)(next.blocksWide * ((next.height + 15) >> 4)),
                  next.width, next.height, next.mm / 1000.0f);
  }
#endif
  RasterHeader old = {};
  old.frame=rasterFrame; old.x0=rasX0; old.y0=rasY0; old.width=rasW; old.height=rasH;
  old.mm=(uint16_t)lroundf(rasMpp*1000); old.blocksWide=rasBW;
  bool keep=rasterValid && !rasterViceValid && !rasHasPal && !next.palette &&
      rasterFrame==next.frame && old.mm==next.mm && rasterAllocatedBytes==RASTER_BUF_BYTES;
  uint8_t *oldPixels=rasterBuf;
  rasterBuf=rasterBackBuf;
  rasterBackBuf=oldPixels;
  if (rasterAllocatedBytes!=RASTER_BUF_BYTES) { free(rasterBackBuf); rasterBackBuf=nullptr; keep=false; }
  rasterAllocatedBytes=RASTER_BUF_BYTES;
  rasterBackHeader=old; rasterBackValid=keep;
  rasterVice=nullptr; rasterViceValid=false; vicePatchData=nullptr; vicePatchCount=0;
  rasBW=next.blocksWide; rasX0=next.x0; rasY0=next.y0; rasW=next.width; rasH=next.height;
  rasMpp=next.mm/1000.0f; rasterFrame=next.frame; rasHasPal=next.palette;
  rasterBuildings=next.buildings;
  rasterHeights=next.heights;
  if (next.palette) for (int i=0;i<16;i++) rasPal[i]=rasterU16(p+16+2*i);
  rasterValid=true; rasterAt=millis();
  bldBlocksRebuild();                     // v7.13 3D 楼块块位图
  return true;
}

// ---- v5.7 第二个核心帮忙画：把逐行的计算分给两个核心（各画一半的行）----
TaskHandle_t helperTask = nullptr;
SemaphoreHandle_t helpGo = nullptr, helpDone = nullptr;
void (*volatile helpFn)(int y0, int y1, int step) = nullptr;
volatile int helpY0 = 0, helpY1 = 0;

void helperLoop(void *) {
  for (;;) {
    xSemaphoreTake(helpGo, portMAX_DELAY);
    if (helpFn) helpFn(helpY0, helpY1, 1);
    xSemaphoreGive(helpDone);
  }
}

void helperInit() {
  helpGo = xSemaphoreCreateBinary();
  helpDone = xSemaphoreCreateBinary();
  // v5.8 优先级比推屏任务高：画底图只要几毫秒，推屏晚一点开始没关系（它不卡画面）
  if (xTaskCreatePinnedToCore(helperLoop, "help", 4096, nullptr, 3, &helperTask, 0) != pdPASS) helperTask = nullptr;   // v5.8 优先级 1 → 3
}

// 把 [y0, y1) 这些行分给两个核心：v6.0 主核心画上半，另一个核心画下半（圆形的东西上下对称，活一样多）
void parallelRows(void (*fn)(int, int, int), int y0, int y1) {
  if (!helperTask) { fn(y0, y1, 1); return; }
  int mid = (y0 + y1) / 2;
  helpFn = fn; helpY0 = mid; helpY1 = y1;
  xSemaphoreGive(helpGo);
  fn(y0, mid, 1);
  xSemaphoreTake(helpDone, portMAX_DELAY);
}

// v2.5 画图片地图：屏幕上每个像素反算到地图图片上的位置，取颜色（只画圆环以内）
// v5.7 改成按行分给两个核心画（参数放在全局变量里）
struct { float cx, cy, sc, cs, sn; uint16_t pal[16]; uint16_t *fb; bool pix2; int32_t viceGain; int viceRotation;
         bool bld3d; bool bldAny; uint16_t wallPlain, wallGreen; } RM;

// v7.13 3D 楼块：16×16 块级“有楼”位图。行进采样前先查它，空块不读栅格，
// 郊区整图为空时一次比较就跳过全部 3D 计算。
static uint8_t bldBlockBits[841];          // 82×82 块 / 8，覆盖 1300×1300
static int bldBw = 0, bldBh = 0;
static bool bldAny = false;
static inline bool bldBlockAny(int bx, int by) {
  if ((unsigned)bx >= (unsigned)bldBw || (unsigned)by >= (unsigned)bldBh) return false;
  uint32_t k = (uint32_t)by * bldBw + bx;
  return (bldBlockBits[k >> 3] & (0x80 >> (k & 7))) != 0;
}
static void bldBlocksRebuild() {           // 收齐一块地图后调用一次（解码缓冲已就位）
  memset(bldBlockBits, 0, sizeof(bldBlockBits));
  bldBw = (rasW + 15) >> 4; bldBh = (rasH + 15) >> 4;
  bldAny = false;
  if (!rasterValid || !rasterBuildings) return;
  int blocks = bldBw * bldBh;
  for (int b = 0; b < blocks; b++) {
    const uint8_t *q = rasterBuf + b * 128;
    for (int k = 0; k < 128; k++) {
      if ((q[k] | (q[k] >> 4)) & 0x08) {   // 任一半字节 ≥8 即有楼
        bldBlockBits[b >> 3] |= 0x80 >> (b & 7);
        bldAny = true;
        break;
      }
    }
  }
}
// 单位高度、每米离屏心距离的倾出比例：透视相机在屏心正上方约 9 米处的近似，越大越“陡”
static constexpr float BLD_LEAN = 0.11f;
volatile uint32_t rasterBuildingSamples = 0;
#define PIXB 3                                      // v6.1 掌机像素：3×3 个屏幕像素当一个大像素

void rasterRows(int y0, int y1, int step) {
  uint16_t *fb = RM.fb;
  const uint16_t *pal = RM.pal;
  float cx = RM.cx, cy = RM.cy, sc = RM.sc, cs = RM.cs, sn = RM.sn;
  float inv = 1.0f / sc, kpx = inv / rasMpp;
  int32_t DU = (int32_t)(cs * kpx * 65536.0f), DV = (int32_t)(sn * kpx * 65536.0f);
  const bool useBack=rasterBackValid && rasterBackHeader.frame==rasterFrame && !rasHasPal && !rasterViceValid;
  const int backU=useBack ? (int)lroundf((rasX0-rasterBackHeader.x0)/rasMpp) : 0;
  const int backV=useBack ? (int)lroundf((rasY0-rasterBackHeader.y0)/rasMpp) : 0;
  const float leanF = BLD_LEAN / rasMpp;   // 3D 楼块：米 → 栅格像素的倾出换算
  const float R = 212;
  for (int y = y0; y < y1; y += step) {
    float dyc = y + 0.5f - CX;
    if (RM.pix2 && (y % PIXB) && y > y0 && fabsf(dyc) < R - 2) {  // v6.1 掌机像素：每 3 行只算第一行，其余照抄
      float half = sqrtf(R * R - dyc * dyc);
      int xa = max(0, (int)(CX - half)), xb = min(LCD_WIDTH - 1, (int)(CX + half));
      memcpy(fb + y * LCD_WIDTH + xa, fb + (y - 1) * LCD_WIDTH + xa, (xb - xa + 1) * 2);
      continue;
    }
    if (fabsf(dyc) >= R) continue;
    float half = sqrtf(R * R - dyc * dyc);
    int xa = max(0, (int)(CX - half)), xb = min(LCD_WIDTH - 1, (int)(CX + half));
    if (RM.pix2) xa -= xa % PIXB;                       // 掌机像素：列也按 3 个一组对齐
    float rx = (xa + 0.5f - CX) * inv, ry = (MAP_PY - (y + 0.5f)) * inv;
    float dx = rx * cs + ry * sn, dy = -rx * sn + ry * cs;
    float u = (cx + dx - rasX0) / rasMpp, v = (rasY0 - (cy + dy)) / rasMpp;
    int32_t U = (int32_t)(u * 65536.0f), V = (int32_t)(v * 65536.0f);
    // 3D 楼块的行起点单位高度位移（16.16 栅格像素），随像素步进
    int32_t offU = (int32_t)(leanF * dx * 65536.0f), offV = (int32_t)(-leanF * dy * 65536.0f);
    uint16_t *row = fb + y * LCD_WIDTH;
    int xstep = RM.pix2 ? PIXB : 1;
    if (RM.pix2) { DU *= PIXB; DV *= PIXB; }
    int32_t offUD = (int32_t)(DU * BLD_LEAN), offVD = (int32_t)(DV * BLD_LEAN);
    for (int x = xa; x <= xb; x += xstep) {
      int32_t ui = U >> 16, vi = V >> 16;
      uint8_t idx = 0;
      if ((uint32_t)ui < (uint32_t)rasW && (uint32_t)vi < (uint32_t)rasH) {
        uint32_t k = ((uint32_t)((vi >> 4) * rasBW + (ui >> 4)) << 8) | ((vi & 15) << 4) | (ui & 15);
        if (rasterViceValid) {
          idx = (rasterVice[k] >> viceBaseShift) & 3;
        } else {
          uint8_t b = rasterBuf[k >> 1];
          idx = (k & 1) ? (b >> 4) : (b & 15);
        }
      } else if (useBack) {
        rasterRead(rasterBackBuf,rasterBackHeader,ui+backU,vi+backV,idx);
      }
      uint16_t col = pal[idx];
      if (RM.bld3d) {
        if (idx >= 4) rasterBuildingSamples++;
        else if (RM.bldAny) {
          // 楼顶向“屏心→本像素”方向外倾；沿射线向屏心最多走 3 步，被某楼挤出体覆盖则画楼顶/侧墙
          for (int j = 1; j <= 3; j++) {
            int32_t uj = (U - offU * j) >> 16, vj = (V - offV * j) >> 16;
            if ((uint32_t)uj >= (uint32_t)rasW || (uint32_t)vj >= (uint32_t)rasH) continue;
            if (!bldBlockAny(uj >> 4, vj >> 4)) continue;
            uint32_t k2 = ((uint32_t)((vj >> 4) * rasBW + (uj >> 4)) << 8) | ((vj & 15) << 4) | (uj & 15);
            uint8_t b2 = rasterBuf[k2 >> 1];
            uint8_t c2 = (k2 & 1) ? (b2 >> 4) : (b2 & 15);
            if (c2 >= 4) {
              int h2 = c2 >= 8 ? (c2 & 3) : 2;         // 旧格式没有高度档：默认 2 档
              if (h2 >= j) {                        // 该楼高度 ≥ 覆盖到本像素所需的档位
                col = (h2 == j) ? RM.pal[c2] : (((c2 >= 8 ? (c2 & 4) : (c2 & 2)) != 0) ? RM.wallGreen : RM.wallPlain);
                rasterBuildingSamples++;
                break;
              }
            }
          }
        }
      } else if (rasterBuildings && idx >= 4) rasterBuildingSamples++;
      row[x] = col;
      for (int k = 1; k < xstep && x + k <= xb; k++) row[x + k] = col;
      U += DU; V += DV;
      offU += offUD; offV += offVD;
    }
    if (RM.pix2) { DU /= PIXB; DV /= PIXB; }
  }
}

static inline uint16_t vicePatchColor(uint16_t cell, int fx, int fy) {
  uint16_t base = RM.pal[(cell >> 14) & 3];
  uint32_t index = cell & 2047;
  if (index >= vicePatchCount || !vicePatchData) return base;
  const uint16_t *q = vicePatchData + index * 4;
  int tx = max(0, min(256, fx + 128)), ty = max(0, min(256, fy + 128));
  uint32_t r = 0, g = 0, b = 0;
  for (int c = 0; c < 4; c++) {
    int shift = c * 4;
    int a = (q[0] >> shift) & 15, e = (q[1] >> shift) & 15;
    int f = (q[2] >> shift) & 15, d = (q[3] >> shift) & 15;
    int top = a * 256 + (e - a) * tx, bottom = f * 256 + (d - f) * tx;
    uint32_t mass = top * (256 - ty) + bottom * ty;
    uint16_t color = RM.pal[c];
    r += (color >> 11) * mass;
    g += ((color >> 5) & 63) * mass;
    b += (color & 31) * mass;
  }
  // 每个顶点的四材质权重之和固定为 15；双线性权重之和固定为 65536。
  return (uint16_t)((((r + 491520) / 983040) << 11) |
      (((g + 491520) / 983040) << 5) | ((b + 491520) / 983040));
}

static inline bool viceReadColor(int x, int y, int fx, int fy, uint16_t &color) {
  if ((uint32_t)x >= (uint32_t)rasW || (uint32_t)y >= (uint32_t)rasH) return false;
  uint32_t k = ((uint32_t)((y >> 4) * rasBW + (x >> 4)) << 8) | ((y & 15) << 4) | (x & 15);
  uint16_t cell = rasterVice[k];
  int pair = (cell >> 11) & 7;
  color = RM.pal[(cell >> 14) & 3];
  if (pair == 7) { color = vicePatchColor(cell, fx, fy); return true; }
  if (pair == 0) return true;
  int direction = cell & 63, offset = (cell >> 6) & 31;
  int distance = viceNX[direction] * fx + viceNY[direction] * fy + viceOffsets[1][offset];
  int index = 128 + (int)((distance * RM.viceGain) >> 16);
  index = max(0, min(256, index));
  int angle = (direction - RM.viceRotation) & 15;
  if (angle > 8) angle = 16 - angle;
  color = viceEdgePal[pair][viceCoverage[angle][index]];
  return true;
}

static inline uint16_t viceJoinColor(uint16_t color, int x, int y, int qx, int qy) {
  // 仅在格子交界的 1/8 源像素带内过渡；内部轮廓保持原来的清晰度。
  int sx = qx < 32 ? -1 : (qx > 224 ? 1 : 0);
  int sy = qy < 32 ? -1 : (qy > 224 ? 1 : 0);
  int wx = sx < 0 ? (32 - qx) * 4 : (sx > 0 ? (qx - 224) * 4 : 0);
  int wy = sy < 0 ? (32 - qy) * 4 : (sy > 0 ? (qy - 224) * 4 : 0);
  int fx = qx - 128, fy = qy - 128;
  uint16_t bx = color, by = color, diagonal = color;
  if (wx && !viceReadColor(x + sx, y, fx - sx * 256, fy, bx)) wx = 0;
  if (wy && !viceReadColor(x, y + sy, fx, fy - sy * 256, by)) wy = 0;
  if (!wx) return wy ? blendF(color, by, wy) : color;
  if (!wy) return blendF(color, bx, wx);
  if (!viceReadColor(x + sx, y + sy, fx - sx * 256, fy - sy * 256, diagonal)) diagonal = by;
  uint32_t weights[4] = {(uint32_t)((256 - wx) * (256 - wy)),
    (uint32_t)(wx * (256 - wy)), (uint32_t)((256 - wx) * wy), (uint32_t)(wx * wy)};
  uint16_t colors[4] = {color, bx, by, diagonal};
  uint32_t r = 0, g = 0, b = 0;
  for (int i = 0; i < 4; i++) {
    r += (colors[i] >> 11) * weights[i];
    g += ((colors[i] >> 5) & 63) * weights[i];
    b += (colors[i] & 31) * weights[i];
  }
  return (uint16_t)((((r + 32768) >> 16) << 11) |
      (((g + 32768) >> 16) << 5) | ((b + 32768) >> 16));
}

void rasterViceRows(int y0, int y1, int step) {
  uint16_t *fb = RM.fb;
  const uint16_t *pal = RM.pal;
  const uint16_t *cells = rasterVice;
  float inv = 1.0f / RM.sc, kpx = inv / rasMpp;
  int32_t du = (int32_t)(RM.cs * kpx * 65536.0f), dv = (int32_t)(RM.sn * kpx * 65536.0f);
  const int32_t gain = RM.viceGain;
  const int pairShift = rasterViceFine ? 11 : 10;
  const int offsetShift = rasterViceFine ? 6 : 5;
  const int baseShift = viceBaseShift;
  const int32_t *offsets = viceOffsets[rasterViceFine ? 1 : 0];
  const bool fine = rasterViceFine;
  const bool joins = rasterViceVersion == 3;
  const int rotation = RM.viceRotation;
  for (int y = y0; y < y1; y += step) {
    float dyc = y + 0.5f - CX;
    if (fabsf(dyc) >= 212) continue;
    float half = sqrtf(212.0f * 212.0f - dyc * dyc);
    int xa = max(0, (int)(CX - half)), xb = min(LCD_WIDTH - 1, (int)(CX + half));
    float rx = (xa + 0.5f - CX) * inv, ry = (MAP_PY - (y + 0.5f)) * inv;
    float dx = rx * RM.cs + ry * RM.sn, dy = -rx * RM.sn + ry * RM.cs;
    int32_t u = (int32_t)(((RM.cx + dx - rasX0) / rasMpp) * 65536.0f);
    int32_t v = (int32_t)(((rasY0 - (RM.cy + dy)) / rasMpp) * 65536.0f);
    uint16_t *out = fb + y * LCD_WIDTH;
    for (int x = xa; x <= xb; x++, u += du, v += dv) {
      int ui = u >> 16, vi = v >> 16;
      uint16_t color = pal[0];
      if ((uint32_t)ui < (uint32_t)rasW && (uint32_t)vi < (uint32_t)rasH) {
        uint32_t k = ((uint32_t)((vi >> 4) * rasBW + (ui >> 4)) << 8) | ((vi & 15) << 4) | (ui & 15);
        uint16_t cell = cells[k];
        uint32_t pair = (cell >> pairShift) & 7;
        color = pal[(cell >> baseShift) & 3];
        if (pair > 0 && (pair <= 6 || joins)) {
          int qx = (u >> 8) & 255, qy = (v >> 8) & 255;
          int fx = qx - 128, fy = qy - 128;
          if (pair <= 6) {
            int direction = fine ? (cell & 63) : ((cell & 31) << 1);
            int offset = (cell >> offsetShift) & 31;
            int distance = viceNX[direction] * fx + viceNY[direction] * fy + offsets[offset];
            int index = 128 + (int)((distance * gain) >> 16);
            index = max(0, min(256, index));
            int angle = (direction - rotation) & 15;
            if (angle > 8) angle = 16 - angle;
            color = viceEdgePal[pair][viceCoverage[angle][index]];
          } else {
            color = vicePatchColor(cell, fx, fy);
          }
          if (joins && (qx < 32 || qx > 224 || qy < 32 || qy > 224))
            color = viceJoinColor(color, ui, vi, qx, qy);
        }
      }
      out[x] = color;
    }
  }
}

uint32_t rasMsAcc = 0;                     // v5.7 统计：底图用时
void drawRasterMap(float cx, float cy, float sc, float cs, float sn) {
  unsigned long t0 = millis();
  rasterBuildingSamples = 0;
  RM.fb = gfx->getFramebuffer();
  if (rasHasPal) {                                     // v6.1 卫星图：用照片自带的颜色（夜间压暗）
    for (int i = 0; i < 16; i++) RM.pal[i] = nightOn ? blendF(rasPal[i], C_BLACK, 110) : rasPal[i];
  } else {
    for (int i = 0; i < 16; i++) RM.pal[i] = C_BLOCK;
    RM.pal[1] = C_ROADM; RM.pal[2] = C_PARK; RM.pal[3] = C_WATER;
  }
  // v7.13.1 楼块配色跟随主题：墙 = 地面稍亮，顶 = 再亮一档，楼越高顶越亮；
  // 绿地/水基底 variant 微偏绿。不再用固定灰（深色主题下发脏）。
  if (!rasHasPal) {
    for (int hb = 0; hb < 2; hb++) {
      uint16_t roof = hb ? blendF(blendF(C_BLOCK, C_PARK, 70), C_WHITE, 52) : blendF(C_BLOCK, C_WHITE, 60);
      for (int hq = 0; hq < 4; hq++) {
        RM.pal[8 + hb * 4 + hq] = nightOn ? blendF(roof, C_BLACK, 96) : roof;
        roof = blendF(roof, C_WHITE, 36);                    // 楼越高顶面越亮
      }
    }
    uint16_t wp = nightOn ? blendF(blendF(C_BLOCK, C_WHITE, 32), C_BLACK, 16) : blendF(C_BLOCK, C_WHITE, 34);
    RM.wallPlain = wp;                                        // 侧墙：比顶暗、比地面亮一点
    RM.wallGreen = blendF(wp, C_PARK, 26);
    // 旧格式（类 4-7）同样走新配色并按默认 2 档挤出：任何 App/固件版本组合都不再出现红黄调试色
    RM.pal[4] = RM.pal[5] = RM.pal[10];
    RM.pal[6] = RM.pal[7] = RM.pal[14];
  }
  // 3D 楼块开关：楼块图（新旧格式）都生效；2D 时全部类走调色板平铺
  RM.bld3d = bld3dOn && rasterBuildings && !rasHasPal;
  RM.bldAny = bldAny;
  RM.pix2 = styleIdx() == ST_PIXEL;
  RM.cx = cx; RM.cy = cy; RM.sc = sc; RM.cs = cs; RM.sn = sn;
  if (styleIdx() == 1 && !rasHasPal && rasterViceValid) {
    prepareVicePalette(RM.pal);
    // 按方形像素的面积覆盖率过渡，斜边的支撑范围自然扩展到约 1.414 像素。
    RM.viceGain = (int32_t)(sc * rasMpp * (65536.0f / (127.0f * 1.5f)));
    RM.viceRotation = (int)lroundf(atan2f(sn, cs) * (32.0f / PI));
    parallelRows(rasterViceRows, (int)(CX - 213), (int)(CX + 213));
  } else {
    parallelRows(rasterRows, (int)(CX - 213), (int)(CX + 213));
  }
  rasMsAcc += millis() - t0;
}

void parseRoute(const uint8_t *p, uint32_t len) {
  routeN = 0;
  if (len < 12) return;
  int frame = rdu16(p);
  int32_t x = rd32(p + 2), y = rd32(p + 6);
  int n = rdu16(p + 10);
  if (n > ROUTE_PTS) n = ROUTE_PTS;
  if (n < 1 || 12 + (uint32_t)(n - 1) * 4 > len) return;
  routeX[0] = x; routeY[0] = y;
  for (int i = 1; i < n; i++) {
    x += rd16(p + 12 + 4 * (i - 1)); y += rd16(p + 14 + 4 * (i - 1));
    routeX[i] = x; routeY[i] = y;
  }
  // 每 32 个点一组算外框（相邻组共用端点，保证线段完整）
  for (int b = 0; b * RB_STEP < n - 1; b++) {
    int s = b * RB_STEP, e = min(n - 1, s + RB_STEP);
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    for (int i = s; i <= e; i++) {
      x0 = min(x0, routeX[i]); x1 = max(x1, routeX[i]);
      y0 = min(y0, routeY[i]); y1 = max(y1, routeY[i]);
    }
    rbBox[4 * b] = x0; rbBox[4 * b + 1] = y0; rbBox[4 * b + 2] = x1; rbBox[4 * b + 3] = y1;
  }
  routeN = n; routeFrame = frame;
  routeProg = -1; routeSearchAt = 0;                  // v3.0 新路线：重新找车的位置
  if (routeCum) {                                     // v5.0 路程表（路况颜色用）
    routeCum[0] = 0;
    for (int i = 1; i < n; i++) {
      float dx = routeX[i] - routeX[i - 1], dy = routeY[i] - routeY[i - 1];
      routeCum[i] = routeCum[i - 1] + sqrtf(dx * dx + dy * dy);
    }
  }
  if (routeCum && routeCurv) {                        // v6.1 弯急程度：前后各 20 米之间转了多少度
    const float WIN = 20;
    int j0 = 0, j1 = 0;
    for (int i = 0; i < n; i++) {
      while (j0 < i && routeCum[i] - routeCum[j0 + 1] >= WIN) j0++;
      if (j1 < i) j1 = i;
      while (j1 < n - 1 && routeCum[j1] - routeCum[i] < WIN) j1++;
      uint8_t cls = 0;
      if (j0 < i && j1 > i) {
        float h0 = atan2f(routeY[i] - routeY[j0], routeX[i] - routeX[j0]);
        float h1 = atan2f(routeY[j1] - routeY[i], routeX[j1] - routeX[i]);
        float d = fabsf(h1 - h0);
        if (d > PI) d = 2 * PI - d;
        float deg = d * 180 / PI;
        cls = deg < 20 ? 0 : (deg < 50 ? 1 : 2);
      }
      routeCurv[i] = cls;
    }
  }
  // v3.2 路线点后面可选：红绿灯个数(2) + 每个灯 x(4) y(4) 所在段号(2) 段内比例(1，0~255)
  lightN = 0;
  uint32_t pos = 12 + (uint32_t)(n - 1) * 4;
  if (pos + 2 <= len) {
    int ln = rdu16(p + pos); pos += 2;
    for (int i = 0; i < ln && lightN < LIGHT_MAX && pos + 11 <= len; i++, pos += 11) {
      lightX[lightN] = rd32(p + pos); lightY[lightN] = rd32(p + pos + 4);
      lightU[lightN] = rdu16(p + pos + 8) + p[pos + 10] / 255.0f;
      lightN++;
    }
  }
}

// ---- v3.0 走过的路线 ----
static inline float segLen(int k) {
  float dx = routeX[k + 1] - routeX[k], dy = routeY[k + 1] - routeY[k];
  return sqrtf(dx * dx + dy * dy);
}

// 路线上的点：u = 段号 + 段内比例
void routePt(float u, float &x, float &y) {
  int k = (int)u;
  if (k >= routeN - 1) { x = routeX[routeN - 1]; y = routeY[routeN - 1]; return; }
  float t = u - k;
  x = routeX[k] + (routeX[k + 1] - routeX[k]) * t;
  y = routeY[k] + (routeY[k + 1] - routeY[k]) * t;
}

// 找车在路线上的位置：平时只在当前位置附近（往回 30 米、往前 300 米）找，
// 这样掉头、绕回同一条路时不会跳到路线后面去；还没对上时才搜整条路线（最多每秒一次）
void updateRouteProgress(float cx, float cy) {
  if (routeN < 2) return;
  int k0, k1;
  if (routeProg < 0) {
    if (millis() - routeSearchAt < 1000 && routeSearchAt) return;
    routeSearchAt = millis();
    k0 = 0; k1 = routeN - 2;
  } else {
    int base = min((int)routeProg, routeN - 2);
    float acc = 0;
    k0 = base; while (k0 > 0 && acc < 30) { acc += segLen(k0 - 1); k0--; }
    acc = 0;
    k1 = base; while (k1 < routeN - 2 && acc < 300) { acc += segLen(k1); k1++; }
  }
  float best = 1e18f, bestU = -1;
  for (int k = k0; k <= k1; k++) {
    float ax = routeX[k], ay = routeY[k];
    float vx = routeX[k + 1] - ax, vy = routeY[k + 1] - ay;
    float L2 = vx * vx + vy * vy;
    float t = L2 > 0 ? ((cx - ax) * vx + (cy - ay) * vy) / L2 : 0;
    t = constrain(t, 0.0f, 1.0f);
    float ex = ax + vx * t - cx, ey = ay + vy * t - cy;
    float d2 = ex * ex + ey * ey;
    if (d2 < best) { best = d2; bestU = k + t; }
  }
  if (best > 50.0f * 50.0f) return;                   // 离路线超过 50 米（偏航中）：先不动
  routeProg = bestU;
}

void mapPolyline(const float *xs, const float *ys, int n, float w, uint16_t c, bool caps);

// 画路线的一段（u 从 a 到 b），先画整段描边再画内芯
// v6.1 弯道上色：内芯按每段的弯急程度分成几截，各用绿 / 黄 / 红画
const uint16_t CURVE_COL[3] = {0x3EEB, 0xFE45, 0xF9E6};
void drawRouteCurved(float a, float b, float cx, float cy, float sc, float cs, float sn, float viewR, float w1) {
  for (int bi = 0; bi * RB_STEP < routeN - 1; bi++) {
    int s = bi * RB_STEP, e = min(routeN - 1, s + RB_STEP);
    if (e <= a || s >= b) continue;
    const int32_t *bb = rbBox + 4 * bi;
    if (bb[2] < cx - viewR || bb[0] > cx + viewR || bb[3] < cy - viewR || bb[1] > cy + viewR) continue;
    float ua = fmaxf(a, (float)s), ub = fminf(b, (float)e);
    int m = 0, cls = -1;
    auto proj = [&](float x, float y) {
      tmpX[m] = CX + sc * ((x - cx) * cs - (y - cy) * sn); tmpY[m] = MAP_PY - sc * ((x - cx) * sn + (y - cy) * cs); m++;
    };
    float x, y;
    routePt(ua, x, y); proj(x, y);
    cls = routeCurv[(int)floorf(ua)];
    for (int k = (int)floorf(ua) + 1; k < ub; k++) {
      proj((float)routeX[k], (float)routeY[k]);
      int c2 = routeCurv[k];
      if (c2 != cls) {                                   // 换颜色：先把这一截画掉，从这个点接着画
        if (m >= 2) mapPolyline(tmpX, tmpY, m, w1, CURVE_COL[cls], true);
        tmpX[0] = tmpX[m - 1]; tmpY[0] = tmpY[m - 1]; m = 1;
        cls = c2;
      }
    }
    routePt(ub, x, y); proj(x, y);
    if (m >= 2) mapPolyline(tmpX, tmpY, m, w1, CURVE_COL[cls], true);
  }
}

void drawRouteRange(float a, float b, float cx, float cy, float sc, float cs, float sn, float viewR,
                    float w0, float w1, uint16_t c0, uint16_t c1) {
  if (b - a < 0.001f) return;
  for (int pass = 0; pass < 2; pass++) {
    for (int bi = 0; bi * RB_STEP < routeN - 1; bi++) {
      int s = bi * RB_STEP, e = min(routeN - 1, s + RB_STEP);
      if (e <= a || s >= b) continue;
      const int32_t *bb = rbBox + 4 * bi;
      if (bb[2] < cx - viewR || bb[0] > cx + viewR || bb[3] < cy - viewR || bb[1] > cy + viewR) continue;
      float ua = fmaxf(a, (float)s), ub = fminf(b, (float)e);
      int m = 0;
      float x, y;
      routePt(ua, x, y);
      tmpX[m] = CX + sc * ((x - cx) * cs - (y - cy) * sn); tmpY[m] = MAP_PY - sc * ((x - cx) * sn + (y - cy) * cs); m++;
      for (int k = (int)floorf(ua) + 1; k < ub; k++) {
        float dx = routeX[k] - cx, dy = routeY[k] - cy;
        tmpX[m] = CX + sc * (dx * cs - dy * sn); tmpY[m] = MAP_PY - sc * (dx * sn + dy * cs); m++;
      }
      routePt(ub, x, y);
      tmpX[m] = CX + sc * ((x - cx) * cs - (y - cy) * sn); tmpY[m] = MAP_PY - sc * ((x - cx) * sn + (y - cy) * cs); m++;
      if (m >= 2) mapPolyline(tmpX, tmpY, m, pass == 0 ? w0 : w1, pass == 0 ? c0 : c1, true);
    }
  }
}

// v4.4 原路线：格式和导航路线一样（坐标系、起点、点数、差值），后面的红绿灯不管
void parseGhost(const uint8_t *p, uint32_t len) {
  ghostN = 0;
  if (!ghostX || len < 12) return;
  int frame = rdu16(p);
  int32_t x = rd32(p + 2), y = rd32(p + 6);
  int n = rdu16(p + 10);
  if (n > GHOST_PTS) n = GHOST_PTS;
  if (n < 2 || 12 + (uint32_t)(n - 1) * 4 > len) return;
  ghostX[0] = x; ghostY[0] = y;
  for (int i = 1; i < n; i++) {
    x += rd16(p + 12 + 4 * (i - 1)); y += rd16(p + 14 + 4 * (i - 1));
    ghostX[i] = x; ghostY[i] = y;
  }
  for (int b = 0; b * RB_STEP < n - 1; b++) {
    int s = b * RB_STEP, e = min(n - 1, s + RB_STEP);
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    for (int i = s; i <= e; i++) {
      x0 = min(x0, ghostX[i]); x1 = max(x1, ghostX[i]);
      y0 = min(y0, ghostY[i]); y1 = max(y1, ghostY[i]);
    }
    gbBox[4 * b] = x0; gbBox[4 * b + 1] = y0; gbBox[4 * b + 2] = x1; gbBox[4 * b + 3] = y1;
  }
  ghostN = n; ghostFrame = frame;
}

// v4.4 画原路线：橙色细线，画在导航路线下面（两条重合的地方只看到导航路线，分开的地方才露出来）
void drawGhost(float cx, float cy, float sc, float cs, float sn, float viewR, float w) {
  if (ghostN < 2 || ghostFrame != posFrame) return;
  for (int bi = 0; bi * RB_STEP < ghostN - 1; bi++) {
    const int32_t *bb = gbBox + 4 * bi;
    if (bb[2] < cx - viewR || bb[0] > cx + viewR || bb[3] < cy - viewR || bb[1] > cy + viewR) continue;
    int s = bi * RB_STEP, e = min(ghostN - 1, s + RB_STEP), m = 0;
    for (int k = s; k <= e; k++) {
      float dx = ghostX[k] - cx, dy = ghostY[k] - cy;
      tmpX[m] = CX + sc * (dx * cs - dy * sn); tmpY[m] = MAP_PY - sc * (dx * sn + dy * cs); m++;
    }
    if (m >= 2) mapPolyline(tmpX, tmpY, m, w, 0xFD20, true);
  }
}

// 'M' 类型 编号 总片数(2) 片序号(2) 总字节(4) 偏移(4) + 数据
void loadBackground() {
  File f = SPIFFS.open("/motonav_bg.bin", FILE_READ);
  uint32_t n = f ? f.size() : 0;
  if (f && n >= 4 && n <= BG_MAX_BYTES && bulks[5].buf && f.read(bulks[5].buf, n) == n) {
      backgroundW = rdu16(bulks[5].buf); backgroundH = rdu16(bulks[5].buf + 2);
      backgroundValid = backgroundW > 0 && backgroundW <= 360 && backgroundH > 0 && backgroundH <= 360 && n == 4u + (uint32_t)backgroundW * backgroundH * 2u;
      if (backgroundValid) {
        if (!backgroundPixels) backgroundPixels = (uint16_t *)ps_malloc(360 * 360 * 2);
        if (backgroundPixels) {
          memset(backgroundPixels, 0, 360 * 360 * 2);
          for (int y = 0; y < backgroundH; y++) memcpy(backgroundPixels + y * 360, bulks[5].buf + 4 + y * backgroundW * 2, backgroundW * 2);
        } else backgroundValid = false;
      }
  }
  if (f) f.close();
}

void saveBackground(const uint8_t *data, uint32_t total) {
  if (total < 4) { Serial.printf("[BG] reject short total=%u\n", (unsigned)total); return; }
  int w = rdu16(data), h = rdu16(data + 2);
  if (w < 1 || h < 1 || w > 360 || h > 360 || total != 4u + (uint32_t)w * h * 2u) {
    Serial.printf("[BG] reject size=%dx%d total=%u expected=%u\n", w, h, (unsigned)total, (unsigned)(4u + (uint32_t)w * h * 2u));
    return;
  }
  File f = SPIFFS.open("/motonav_bg.tmp", FILE_WRITE);
  size_t saved = 0;
  if (f) {
    // 分块写并让出 CPU：259KB 一次性写入会卡住 loop 数百毫秒以上，BLE 接收队列随之溢出丢包
    for (uint32_t off = 0; off < total; ) {
      size_t chunk = total - off < 8192 ? total - off : 8192;
      if (f.write(data + off, chunk) != chunk) break;
      saved += chunk;
      off += chunk;
      delay(1);
    }
    f.flush(); f.close();
  }
  if (saved == total) SPIFFS.remove("/motonav_bg.bin");
  bool renamed = saved == total && SPIFFS.rename("/motonav_bg.tmp", "/motonav_bg.bin");
  Serial.printf("[BG] SPIFFS save bytes=%u/%u old_removed=%d renamed=%d\n", (unsigned)saved, (unsigned)total, saved == total ? 1 : 0, renamed ? 1 : 0);
  if (!renamed) { SPIFFS.remove("/motonav_bg.tmp"); return; }
  backgroundW = w; backgroundH = h;
  if (!backgroundPixels) backgroundPixels = (uint16_t *)ps_malloc(360 * 360 * 2);
  if (backgroundPixels) {
    memset(backgroundPixels, 0, 360 * 360 * 2);
    for (int y = 0; y < h; y++) memcpy(backgroundPixels + y * 360, data + 4 + y * w * 2, w * 2);
    backgroundValid = true;
    Serial.printf("[BG] saved and active %dx%d bytes=%u\n", w, h, (unsigned)total);
  }
}

void drawBackground() {
  if (!backgroundValid || !backgroundPixels) return;
  int x = (LCD_WIDTH - backgroundW) / 2, y = (LCD_HEIGHT - backgroundH) / 2;
  // The backing buffer has a fixed 360-pixel stride; draw rows separately.
  for (int row = 0; row < backgroundH; row++)
    gfx->draw16bitRGBBitmap(x, y + row, backgroundPixels + row * 360, backgroundW, 1);
}

void handleBulk(const uint8_t *d, int n) {
  static uint32_t transferStartedAt[6] = {};
  if (n < 15) return;
  int type = d[1], id = d[2];
  int parts = d[3] | (d[4] << 8), seq = d[5] | (d[6] << 8);
  uint32_t total = (uint32_t)rd32(d + 7), off = (uint32_t)rd32(d + 11);
  if (type < 1 || type > 5) return;
  Bulk &b = bulks[type];
  if (!b.buf || total == 0 || total > b.cap || parts == 0 || parts > 4096 || seq >= parts) return;
  if (id != b.id || total != b.total || parts != b.parts) {     // 新的一份：从头开始收
    b.id = id; b.total = total; b.parts = parts; b.got = 0; b.done = false;
    memset(b.mask, 0, sizeof(b.mask));
    transferStartedAt[type] = millis();
    if (type == 1 || type == 3) {
      MAP_RX_LOG("[MAP-RX] start type=%d id=%u bytes=%u parts=%u heap=%u psram=%u\n",
                    type, id, (unsigned)total, parts, ESP.getFreeHeap(), ESP.getFreePsram());
    }
  }
  if (b.done) { pendingAck = "K," + String(type) + "," + String(id); return; }   // 重复的：再确认一次
  uint32_t len = n - 15;
  if (off >= total) return;
  if (off + len > total) len = total - off;
  if (!(b.mask[seq >> 3] & (1 << (seq & 7)))) {
    memcpy(b.buf + off, d + 15, len);
    b.mask[seq >> 3] |= (1 << (seq & 7));
    b.got++;
  }
  if ((type == 1 || type == 3) && (b.got == 1 || b.got == b.parts || (b.got % 128) == 0)) {
    MAP_RX_LOG("[MAP-RX] progress type=%d id=%u parts=%u/%u\n",
                  type, id, b.got, b.parts);
  }
  if (b.got >= b.parts) {
    uint32_t decodeAt = micros();
    MAP_RX_LOG("[MAP-RX] complete type=%d id=%u bytes=%u elapsed_ms=%u; processing\n",
                  type, id, (unsigned)total, (unsigned)(millis() - transferStartedAt[type]));
    if (!mapAllocOk) {                 // 开机缓冲没分配全（PSRAM 异常）：丢弃并允许重发，别解引用空指针
      b.done = false; b.got = 0; memset(b.mask, 0, sizeof(b.mask));
      return;
    }
    b.done = true;
    if (type == 1) {
      uint8_t *t = mapShow; mapShow = b.buf; b.buf = t;   // 交换：新图显示，旧缓冲留着收下一块
      parseMap(mapShow, total);
    } else if (type == 2) {
      parseRoute(b.buf, total);
    } else if (type == 4) {
      parseGhost(b.buf, total);                        // v4.4 原路线
    } else if (type == 5) {
      saveBackground(b.buf, total);
      if (!backgroundValid) { Serial.printf("[BG] rejected after receive id=%u\n", id); b.done = false; b.got = 0; memset(b.mask, 0, sizeof(b.mask)); return; }
      if (page == PAGE_HOME && !inNav) drawWaiting(waitSt);
    } else {
      if (!parseRaster(b.buf,total)) {
        if (total >= 16) {
          MAP_RX_LOG("[MAP-RX] reject raster id=%u bytes=%u head=%u,%ld,%ld,%u,%u,0x%04x heap=%u psram=%u\n",
                        id, (unsigned)total, rasterU16(b.buf), (long)rasterI32(b.buf + 2),
                        (long)rasterI32(b.buf + 6), rasterU16(b.buf + 10), rasterU16(b.buf + 12),
                        rasterU16(b.buf + 14), ESP.getFreeHeap(), ESP.getFreePsram());
        }
        // 没有成功解码就不回“已确认”，并允许相同编号完整重发。
        b.done=false; b.got=0; memset(b.mask,0,sizeof(b.mask));
        return;
      }
      MAP_RX_LOG("[MAP-RX] raster accepted id=%u frame=%d size=%dx%d mpp=%.3f buildings=%d decode_ms=%u inNav=%d page=%d style=%d\n",
                    id, rasterFrame, rasW, rasH, rasMpp, rasterBuildings,
                    (unsigned)((micros() - decodeAt) / 1000), inNav, page, styleIdx());
    }
    pendingAck = "K," + String(type) + "," + String(id);
    navDirty = true;
  }
}


// ================= NavMap 段 12（源 MotoNav.cpp 4476-4829 行） =================

// 现在是否显示小地图
bool mapModeActive() {
  if (!mapOn || !inNav) return false;
  bool vecOK = mapValid && mapFrame == posFrame;
  bool rasOK = rasterValid && rasterFrame == posFrame;      // v2.5
  // v4.0 停车或暂停时手机可能不再发位置：不再 5 秒后切回箭头画面，保持地图（位置停在最后一次）
  if (!(styleIdx() == ST_DRAGON || vecOK || rasOK) || posAt == 0) return false;
  return true;                                      // v2.1 一直显示地图，路口信息放大显示在圆牌里
}

// ---- 画图工具 ----

// 粗折线（圆角连接）；屏幕外的线段直接跳过
// v2.2：只在方向变化明显的拐点补圆（直线上的点不画圆，省很多时间）；caps = 两端也画圆头
struct VicePlane { float nx, ny, c, support; uint8_t angle; };

static inline void viceBlendPixel(uint16_t *row, int x, uint16_t color, float coverage) {
  int alpha = (int)(coverage * 256.0f + 0.5f);
  if (alpha <= 0) return;
  row[x] = alpha >= 256 ? color : blendF(row[x], color, alpha);
}

void viceAAQuad(const float *px, const float *py, uint16_t color) {
  VicePlane plane[4];
  float cx = 0, cy = 0, ymin = py[0], ymax = py[0];
  for (int i = 0; i < 4; i++) { cx += px[i]; cy += py[i]; ymin = fminf(ymin, py[i]); ymax = fmaxf(ymax, py[i]); }
  cx *= 0.25f; cy *= 0.25f;
  for (int i = 0; i < 4; i++) {
    int j = (i + 1) & 3;
    float dx = px[j] - px[i], dy = py[j] - py[i], length = sqrtf(dx * dx + dy * dy);
    if (length < 0.001f) return;
    float nx = -dy / length, ny = dx / length;
    if ((cx - px[i]) * nx + (cy - py[i]) * ny < 0) { nx = -nx; ny = -ny; }
    plane[i] = {nx, ny, -nx * px[i] - ny * py[i], (fabsf(nx) + fabsf(ny)) * 0.5f, viceCoverageAngle(nx, ny)};
  }
  uint16_t *fb = gfx->getFramebuffer();
  int ya = max(0, (int)floorf(ymin - 1)), yb = min(LCD_HEIGHT - 1, (int)ceilf(ymax + 1));
  for (int y = ya; y <= yb; y++) {
    float sy = y + 0.5f, left = 0.5f, right = LCD_WIDTH - 0.5f;
    float solidLeft = left, solidRight = right;
    bool outside = false, noSolid = false;
    float q[4];
    for (int i = 0; i < 4; i++) {
      q[i] = plane[i].ny * sy + plane[i].c;
      float nx = plane[i].nx, support = plane[i].support;
      if (fabsf(nx) < 0.00001f) {
        if (q[i] <= -support) outside = true;
        if (q[i] < support) noSolid = true;
      } else if (nx > 0) {
        left = fmaxf(left, (-support - q[i]) / nx);
        solidLeft = fmaxf(solidLeft, (support - q[i]) / nx);
      } else {
        right = fminf(right, (-support - q[i]) / nx);
        solidRight = fminf(solidRight, (support - q[i]) / nx);
      }
    }
    if (outside || left > right) continue;
    int xa = max(0, (int)ceilf(left - 0.5f)), xb = min(LCD_WIDTH - 1, (int)floorf(right - 0.5f));
    int ia = max(xa, (int)ceilf(solidLeft - 0.5f)), ib = min(xb, (int)floorf(solidRight - 0.5f));
    if (noSolid || ia > ib) { ia = xb + 1; ib = xb; }
    uint16_t *row = fb + y * LCD_WIDTH;
    for (int x = xa; x <= xb;) {
      if (x == ia && ia <= ib) {
        gfx->drawFastHLine(x, y, ib - ia + 1, color);
        x = ib + 1; continue;
      }
      float sx = x + 0.5f;
      int alpha = 256;
      for (int i = 0; i < 4; i++) {
        float distance = plane[i].nx * sx + q[i];
        if (distance <= -plane[i].support) { alpha = 0; break; }
        if (distance < plane[i].support) {
          int index = max(0, min(256, (int)floorf(128.5f + distance * (256.0f / 1.5f))));
          alpha = min(alpha, (int)viceCoverage[plane[i].angle][index]);
        }
      }
      if (alpha) row[x] = alpha >= 256 ? color : blendF(row[x], color, alpha);
      x++;
    }
  }
}

void viceAACircle(float cx, float cy, float radius, uint16_t color) {
  uint16_t *fb = gfx->getFramebuffer();
  int ya = max(0, (int)floorf(cy - radius - 1)), yb = min(LCD_HEIGHT - 1, (int)ceilf(cy + radius + 1));
  float outer = radius + 0.5f, inner = fmaxf(0, radius - 0.5f);
  for (int y = ya; y <= yb; y++) {
    float dy = y + 0.5f - cy;
    if (fabsf(dy) >= outer) continue;
    float extent = sqrtf(outer * outer - dy * dy);
    int xa = max(0, (int)ceilf(cx - extent - 0.5f)), xb = min(LCD_WIDTH - 1, (int)floorf(cx + extent - 0.5f));
    int ia = xb + 1, ib = xb;
    if (fabsf(dy) < inner) {
      float core = sqrtf(inner * inner - dy * dy);
      ia = max(xa, (int)ceilf(cx - core - 0.5f)); ib = min(xb, (int)floorf(cx + core - 0.5f));
    }
    uint16_t *row = fb + y * LCD_WIDTH;
    for (int x = xa; x <= xb;) {
      if (x == ia && ia <= ib) {
        gfx->drawFastHLine(x, y, ib - ia + 1, color);
        x = ib + 1; continue;
      }
      float dx = x + 0.5f - cx;
      viceBlendPixel(row, x, color, outer - sqrtf(dx * dx + dy * dy));
      x++;
    }
  }
}

void mapPolyline(const float *xs, const float *ys, int n, float w, uint16_t c, bool caps) {
  bool vice = styleIdx() == 1;
  if (vice) initViceSampling();
  float r = w / 2;
  float pdx = 0, pdy = 0;
  bool havePrev = false;
  for (int i = 0; i + 1 < n; i++) {
    float x0 = xs[i], y0 = ys[i], x1 = xs[i + 1], y1 = ys[i + 1];
    if ((x0 < -w && x1 < -w) || (x0 > LCD_WIDTH + w && x1 > LCD_WIDTH + w) ||
        (y0 < -w && y1 < -w) || (y0 > LCD_HEIGHT + w && y1 > LCD_HEIGHT + w)) continue;
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) continue;
    float ux = dx / len, uy = dy / len;
    float nx = -uy * r, ny = ux * r;
    if (vice) {
      float px[4] = {x0 + nx, x0 - nx, x1 - nx, x1 + nx};
      float py[4] = {y0 + ny, y0 - ny, y1 - ny, y1 + ny};
      viceAAQuad(px, py, c);
    } else {
      gfx->fillTriangle(x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 + nx, y1 + ny, c);
      gfx->fillTriangle(x0 - nx, y0 - ny, x1 - nx, y1 - ny, x1 + nx, y1 + ny, c);
    }
    if (r >= 1.5f) {
      if (havePrev ? (ux * pdx + uy * pdy < 0.97f) : caps) {
        if (vice) viceAACircle(x0, y0, r, c); else gfx->fillCircle(x0, y0, r, c);
      }
      if (caps && i + 2 == n) {
        if (vice) viceAACircle(x1, y1, r, c); else gfx->fillCircle(x1, y1, r, c);
      }
    }
    pdx = ux; pdy = uy; havePrev = true;
  }
}

// 实心多边形（扫描线，奇偶规则）
void mapFillPoly(const float *xs, const float *ys, int n, uint16_t c) {
  if (n < 3) return;
  float ymin = 1e9f, ymax = -1e9f, xmin = 1e9f, xmax = -1e9f;
  for (int i = 0; i < n; i++) {
    ymin = fminf(ymin, ys[i]); ymax = fmaxf(ymax, ys[i]);
    xmin = fminf(xmin, xs[i]); xmax = fmaxf(xmax, xs[i]);
  }
  if (ymax < 0 || ymin >= LCD_HEIGHT || xmax < 0 || xmin >= LCD_WIDTH) return;
  int y0 = max(0, (int)ceilf(ymin - 0.5f)), y1 = min(LCD_HEIGHT - 1, (int)floorf(ymax - 0.5f));
  static float xs_hit[256];
  for (int y = y0; y <= y1; y++) {
    float sy = y + 0.5f;
    int k = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
      float ya = ys[j], yb = ys[i];
      if ((ya <= sy && yb > sy) || (yb <= sy && ya > sy)) {
        if (k < 256) xs_hit[k++] = xs[j] + (sy - ya) * (xs[i] - xs[j]) / (yb - ya);
      }
    }
    for (int a = 1; a < k; a++) {               // 插入排序（交点通常很少）
      float v = xs_hit[a]; int b = a - 1;
      while (b >= 0 && xs_hit[b] > v) { xs_hit[b + 1] = xs_hit[b]; b--; }
      xs_hit[b + 1] = v;
    }
    for (int a = 0; a + 1 < k; a += 2) {
      int xa = max(0, (int)ceilf(xs_hit[a] - 0.5f)), xb = min(LCD_WIDTH - 1, (int)floorf(xs_hit[a + 1] - 0.5f));
      if (xb >= xa) gfx->drawFastHLine(xa, y, xb - xa + 1, c);
    }
  }
}

// 车头箭头（白色，深色描边）
void drawPlayer(float x, float y) {
  // v3.9 车头箭头缩小到原来的约 70%
  const float T[4][2] = {{0, -13}, {-10, 10.5f}, {0, 5}, {10, 10.5f}};
  for (int pass = 0; pass < 2; pass++) {
    float k = pass == 0 ? 1.35f : 1.0f;
    uint16_t c = pass == 0 ? C_BLACK : C_WHITE;
    float yo = pass == 0 ? 1.0f : 0;
    gfx->fillTriangle(x + T[0][0] * k, y + T[0][1] * k + yo, x + T[1][0] * k, y + T[1][1] * k + yo, x + T[2][0] * k, y + T[2][1] * k + yo, c);
    gfx->fillTriangle(x + T[0][0] * k, y + T[0][1] * k + yo, x + T[2][0] * k, y + T[2][1] * k + yo, x + T[3][0] * k, y + T[3][1] * k + yo, c);
  }
}

// ---- v2.7 上下弧形面板：和黑色外圈连成一体 ----
#define CAP_R      213      // 面板填到这个半径（和外圈黑边接上）
#define TOPCAP_Y   98       // 上面板下沿（有测速时 112）
#define BOTCAP_Y   368      // 下面板上沿（离路口 150 米内 334）
#define BOTCAP_Y2  334

// 填充圆内 y0~y1 行（圆的一个弓形）
void fillCapRows(int y0, int y1, uint16_t c) {
  for (int y = max(0, y0); y <= min(LCD_HEIGHT - 1, y1); y++) {
    float dy = y + 0.5f - CX;
    if (fabsf(dy) >= CAP_R) continue;
    float h = sqrtf((float)CAP_R * CAP_R - dy * dy);
    int xa = max(0, (int)(CX - h)), xb = min(LCD_WIDTH - 1, (int)(CX + h));
    gfx->drawFastHLine(xa, y, xb - xa + 1, c);
  }
}

// 面板和地图之间的分隔线（只画到外圈黑边以内）
void capLine(int y, int thick, uint16_t c) {
  for (int k = 0; k < thick; k++) {
    float dy = y + k + 0.5f - CX;
    if (fabsf(dy) >= 211) continue;
    float h = sqrtf(211.0f * 211.0f - dy * dy);
    gfx->drawFastHLine((int)(CX - h), y + k, (int)(2 * h), c);
  }
}

// v2.8 带黑色描边的文字（直接浮在地图上）
void printOutlined(const String &t, int x, int y, int size, uint16_t col) {
  int o = size >= 3 ? 2 : 1;
  // v5.8 只画最外一圈偏移（字的笔画至少 o 像素粗，里面那圈会被盖住，画了也白画）
  for (int dy = -o; dy <= o; dy++)
    for (int dx = -o; dx <= o; dx++)
      if ((dx || dy) && (o < 2 || abs(dx) == o || abs(dy) == o)) printCentered(t, x + dx, y + dy, size, C_BLACK);
  printCentered(t, x, y, size, col);
}
void printOutlined(const char *t, int x, int y, int size, uint16_t col) {
  int o = size >= 3 ? 2 : 1;
  for (int dy = -o; dy <= o; dy++)
    for (int dx = -o; dx <= o; dx++)
      if ((dx || dy) && (o < 2 || abs(dx) == o || abs(dy) == o)) printCentered(t, x + dx, y + dy, size, C_BLACK);
  printCentered(t, x, y, size, col);
}

// 把一块区域调暗（半透明深色底），a = 保留多少原来的亮度（0~255）
void dimRect(int x, int y, int w, int h, int r, int keep) {
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;
  for (int j = max(0, y); j < min(LCD_HEIGHT, y + h); j++) {
    for (int i = max(0, x); i < min(LCD_WIDTH, x + w); i++) {
      int dx = i < x + r ? x + r - i : (i >= x + w - r ? i - (x + w - r - 1) : 0);
      int dy = j < y + r ? y + r - j : (j >= y + h - r ? j - (y + h - r - 1) : 0);
      if (dx * dx + dy * dy > r * r) continue;
      uint16_t &p = fb[j * LCD_WIDTH + i];
      p = blend565(C_BLACK, p, keep);
    }
  }
}

// 车速（浮在顶部）+ 测速牌
void drawMapSpeed(bool over) {
  if (speedKmh < 0) return;
  int sx = hasCam ? CX - 40 : CX;
  char sb[12]; snprintf(sb, sizeof(sb), "%d", speedKmh);
  printOutlined(sb, sx, 44, 4, over ? C_RED : TH.hud);
  printOutlined("km/h", sx, 80, 1, over ? 0xFC10 : TH.hud);
  if (hasCam) {
    const int bx = CX + 52, by = 60;
    aaDisc(bx, by, 27, C_BLACK);
    aaDisc(bx, by, 25, C_SIGNRED);
    aaDisc(bx, by, 19, C_WHITE);
    char lim[8]; snprintf(lim, sizeof(lim), "%d", camLimit);
    int sz = strlen(lim) >= 3 ? 2 : 3;
    printCentered(lim, bx, by - 4 * sz, sz, C_BLACK);
    char db[12]; distTextBuf(camDist, db);
    printOutlined(db, bx, by + 31, 2, over ? 0xFC10 : C_WHITE);
  }
}

// ---- v3.1 红绿灯小图标：竖着的黑色灯箱 + 红黄绿三个灯 ----
void drawTrafficLight(float cx, float cy, float h) {
  float w = h * 0.46f, r = h * 0.13f;
  gfx->fillRoundRect((int)(cx - w / 2 - 1), (int)(cy - h / 2 - 1), (int)w + 2, (int)h + 2, (int)(w / 2.6f), C_BLACK);
  gfx->fillRoundRect((int)(cx - w / 2), (int)(cy - h / 2), (int)w, (int)h, (int)(w / 3), 0x2104);
  aaDisc(cx, cy - h * 0.29f, r, 0xF8A2);     // 红
  aaDisc(cx, cy, r, 0xFDA0);                // 黄
  aaDisc(cx, cy + h * 0.29f, r, 0x3F09);    // 绿
}

// v3.1 底部一行行程信息：到达时间 · 剩余里程 · 红绿灯数（半透明深色底）
void drawTripLine(int y) {
  char eta[8];
  snprintf(eta, sizeof(eta), "%02d:%02d", (tripEta / 100) % 24, tripEta % 100);
  String a = String(eta);
  String b = tripRemainM >= 100000 ? String(tripRemainM / 1000) + "km" : distText(tripRemainM);
  String c = String(tripLights);
  const int CW = 12, GAP = 14, ICON = 10;
  bool lights = tripLights > 0;
  int w = (a.length() + b.length()) * CW + GAP + (lights ? GAP + ICON + 3 + c.length() * CW : 0);
  int x = CX - w / 2;
  dimRect(x - 10, y - 5, w + 20, 26, 12, 75);
  printCentered(a, x + a.length() * CW / 2, y, 2, TH.hud);
  x += a.length() * CW + GAP;
  printCentered(b, x + b.length() * CW / 2, y, 2, C_WHITE);
  x += b.length() * CW + GAP;
  if (lights) {
    drawTrafficLight(x + ICON / 2, y + 7, 18);
    x += ICON + 3;
    printCentered(c, x + c.length() * CW / 2, y, 2, C_WHITE);
  }
}

// v3.3 车道：放在车速下面（离路口 150 米内才显示），比原来大一点；有测速牌时再往下让一点
void drawMapLanes() {
  if (laneN <= 0 || navDir == "S" || navDist > MAP_NEAR_M) return;
  const int H = 30, MAXW = 210;
  int lw = (int)(fminf(36.0f * H / 32, (float)MAXW / laneN) * laneN);
  int y = hasCam ? 114 : 100;
  dimRect(CX - lw / 2 - 9, y - 5, lw + 18, H + 10, 11, 70);
  drawLanesAt(CX, y, MAXW, H);
}

// ---- v3.8 小地图上的“占位框”：先登记已经占用的地方，电子眼图标只放在空位上 ----
#define OBS_MAX 40
int16_t obsX0[OBS_MAX], obsY0[OBS_MAX], obsX1[OBS_MAX], obsY1[OBS_MAX];
int obsN = 0;
void obsAdd(float x0, float y0, float x1, float y1) {
  if (obsN >= OBS_MAX) return;
  obsX0[obsN] = (int16_t)x0; obsY0[obsN] = (int16_t)y0; obsX1[obsN] = (int16_t)x1; obsY1[obsN] = (int16_t)y1;
  obsN++;
}
// 和已占用的地方重叠了多少面积（像素）；from = 从第几个占位框开始算（v4.2）
long obsOverlap(int x0, int y0, int x1, int y1, int from = 0) {
  long sum = 0;
  for (int i = from; i < obsN; i++) {
    int w = min(x1, (int)obsX1[i]) - max(x0, (int)obsX0[i]);
    int h = min(y1, (int)obsY1[i]) - max(y0, (int)obsY0[i]);
    if (w > 0 && h > 0) sum += (long)w * h;
  }
  return sum;
}
bool obsHit(int x0, int y0, int x1, int y1) {
  for (int i = 0; i < obsN; i++)
    if (x0 < obsX1[i] && x1 > obsX0[i] && y0 < obsY1[i] && y1 > obsY0[i]) return true;
  return false;
}

// 本帧在雷达里的电子眼，在屏幕上的实际位置
float camPx[MCAM_MAX], camPy[MCAM_MAX];
bool camShown[MCAM_MAX];

// v3.5 算出电子眼在小地图上的位置（已经开过的 = 在车后面的不画）；v3.8 真正的绘制放到 drawCamIcons()
void drawMapCams(float cx, float cy, float sc, float cs, float sn) {
  for (int i = 0; i < MCAM_MAX; i++) camShown[i] = false;
  if (mapCamN == 0 || mapCamFrame != posFrame) return;
  for (int i = 0; i < mapCamN; i++) {
    float dx = mapCamX[i] - cx, dy = mapCamY[i] - cy;
    float px = CX + sc * (dx * cs - dy * sn), py = MAP_PY - sc * (dx * sn + dy * cs);
    if (py > MAP_PY + 12) continue;                                     // 在车后面：已经过了
    float ex = px - CX, ex2 = py - CX;
    if (ex * ex + ex2 * ex2 > 194.0f * 194.0f) continue;
    camPx[i] = px; camPy[i] = py; camShown[i] = true;
  }
}


// ================= NavMap 段 13（源 MotoNav.cpp 4830-8270 行） =================

// v3.8 电子眼类型图标（32x32，每像素 3 字节：RGB565 低、高、透明度），由 gen.py 生成
#define CAMI_N 26
#define CAMI_S 32
const uint8_t CAM_ICONS[CAMI_N][CAMI_S * CAMI_S * 3] = {
  { // 0 电子眼
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,227,24,18,
    227,24,18,73,74,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,134,49,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,227,24,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    99,8,16,0,0,128,0,0,223,0,0,255,163,16,255,102,41,255,232,57,255,73,66,255,
    73,66,255,232,57,255,102,41,255,163,16,255,0,0,255,0,0,230,0,0,142,66,8,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,85,
    0,0,224,65,8,255,167,49,255,13,91,253,176,115,251,240,115,252,241,115,253,240,115,254,
    240,115,254,241,115,253,240,115,252,176,115,251,13,91,252,167,49,255,65,8,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    102,41,255,78,99,251,241,115,253,208,115,255,175,107,255,143,107,255,143,107,255,143,107,255,
    143,107,255,143,107,255,143,107,255,175,107,255,208,115,255,240,115,254,78,99,251,102,41,255,
    0,0,255,0,0,159,0,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,73,66,251,
    241,115,253,176,115,255,143,107,255,143,107,255,175,107,255,175,107,255,175,107,255,175,107,255,
    175,107,255,175,107,255,175,107,255,175,107,255,143,107,255,143,107,255,176,115,255,240,115,254,
    73,66,251,0,0,255,0,0,189,3,0,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,106,74,250,241,123,255,
    143,107,255,143,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,
    175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,143,107,255,143,107,255,
    241,123,255,106,74,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,81,0,0,255,41,66,250,241,123,254,143,107,255,
    175,107,255,175,107,255,175,107,255,175,107,255,143,107,255,143,107,255,143,107,255,143,107,255,
    175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,
    143,107,255,241,123,255,41,66,250,0,0,255,32,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,36,17,14,0,0,230,69,41,254,241,115,253,143,107,255,175,107,255,
    175,107,255,175,107,255,175,107,255,143,107,255,208,115,255,240,123,255,240,123,255,240,115,255,
    143,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,
    175,107,255,143,107,255,240,115,254,69,41,252,0,0,247,130,8,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,65,0,255,78,99,251,176,115,255,143,107,255,175,107,255,
    175,107,255,176,115,255,143,107,255,212,148,255,158,247,255,190,247,255,190,247,255,190,247,255,
    85,165,255,111,107,255,208,115,255,176,115,255,176,115,255,176,115,255,175,107,255,175,107,255,
    175,107,255,143,107,255,176,115,255,78,99,251,65,0,255,0,0,160,0,0,0,0,0,0,
    0,0,0,199,57,9,0,0,228,167,49,254,241,115,253,143,107,255,175,107,255,175,107,255,
    175,107,255,143,107,255,237,82,255,215,181,255,255,255,255,255,255,255,255,255,255,255,255,255,
    89,206,255,237,90,255,110,99,255,78,99,255,78,99,255,78,99,255,111,107,255,175,107,255,
    175,107,255,175,107,255,143,107,255,241,115,254,167,49,254,0,0,246,162,16,24,0,0,0,
    0,0,0,0,0,70,0,0,255,13,91,253,208,115,255,143,107,255,175,107,255,175,107,255,
    111,107,255,82,132,255,150,173,255,219,222,255,255,255,255,255,255,255,255,255,255,255,255,255,
    93,231,255,183,181,255,215,181,255,215,181,255,248,189,255,248,189,255,114,140,255,111,107,255,
    175,107,255,175,107,255,143,107,255,208,115,255,13,91,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,162,16,254,176,115,251,175,107,255,175,107,255,175,107,255,143,107,255,
    49,132,255,190,247,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,125,239,255,
    158,247,255,255,255,255,255,255,255,255,255,255,93,239,255,93,239,255,255,255,255,147,140,255,
    111,107,255,175,107,255,175,107,255,175,107,255,176,115,251,162,16,254,0,0,173,0,0,0,
    0,0,0,0,0,192,102,41,255,240,115,252,143,107,255,175,107,255,176,115,255,78,99,255,
    183,181,255,255,255,255,223,255,255,255,255,255,255,255,255,93,239,255,147,140,255,143,107,255,
    111,107,255,147,140,255,93,231,255,255,255,255,143,107,255,111,107,255,255,255,255,89,206,255,
    78,99,255,176,115,255,175,107,255,143,107,255,240,115,253,102,41,255,0,0,217,0,0,0,
    0,0,0,0,0,219,232,57,255,241,115,253,143,107,255,175,107,255,176,115,255,78,99,255,
    215,181,255,255,255,255,255,255,255,255,255,255,158,247,255,17,124,255,45,91,255,114,140,255,
    114,140,255,45,91,255,17,124,255,190,247,255,24,190,255,248,189,255,255,255,255,89,206,255,
    78,99,255,176,115,255,175,107,255,143,107,255,240,115,254,232,57,255,0,0,239,162,16,25,
    227,24,18,0,0,234,73,66,255,240,115,254,143,107,255,175,107,255,176,115,255,78,99,255,
    215,181,255,255,255,255,223,255,255,255,255,255,248,189,255,13,91,255,118,173,255,255,255,255,
    255,255,255,118,173,255,13,91,255,248,189,255,255,255,255,255,255,255,255,255,255,89,206,255,
    78,99,255,176,115,255,175,107,255,143,107,255,240,115,254,73,66,255,0,0,250,65,8,43,
    195,24,19,0,0,235,73,66,255,240,115,254,143,107,255,175,107,255,176,115,255,78,99,255,
    215,181,255,255,255,255,223,255,255,255,255,255,53,165,255,46,91,255,60,231,255,255,255,255,
    255,255,255,60,231,255,46,91,255,53,165,255,255,255,255,223,255,255,255,255,255,89,206,255,
    78,99,255,176,115,255,175,107,255,143,107,255,240,115,254,73,66,255,0,0,251,65,8,43,
    73,74,7,0,0,221,232,57,255,241,115,253,143,107,255,175,107,255,176,115,255,78,99,255,
    215,181,255,255,255,255,223,255,255,255,255,255,248,189,255,13,91,255,118,173,255,255,255,255,
    255,255,255,118,173,255,13,91,255,248,189,255,255,255,255,223,255,255,255,255,255,89,206,255,
    78,99,255,176,115,255,175,107,255,143,107,255,240,115,254,232,57,255,0,0,241,130,16,28,
    0,0,0,0,0,196,102,41,255,240,115,252,143,107,255,175,107,255,176,115,255,78,99,255,
    215,181,255,255,255,255,255,255,255,255,255,255,158,247,255,17,124,255,45,91,255,114,140,255,
    114,140,255,45,91,255,17,124,255,158,247,255,255,255,255,255,255,255,255,255,255,89,206,255,
    78,99,255,176,115,255,175,107,255,143,107,255,240,115,253,102,41,255,0,0,220,0,0,0,
    0,0,0,0,0,151,162,16,254,176,115,251,175,107,255,175,107,255,176,115,255,78,99,255,
    215,181,255,255,255,255,223,255,255,255,255,255,255,255,255,61,231,255,147,140,255,111,107,255,
    111,107,255,147,140,255,61,231,255,255,255,255,255,255,255,223,255,255,255,255,255,122,206,255,
    78,99,255,176,115,255,175,107,255,175,107,255,176,115,252,162,16,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,13,91,252,208,115,255,143,107,255,175,107,255,111,107,255,
    114,140,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,191,247,255,
    191,247,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,212,148,255,
    111,99,255,175,107,255,143,107,255,208,115,255,13,91,251,0,0,255,0,0,111,0,0,0,
    0,0,0,36,33,14,0,0,236,167,49,254,241,115,253,143,107,255,175,107,255,175,107,255,
    111,107,255,147,140,255,57,198,255,89,206,255,89,206,255,89,206,255,89,206,255,122,206,255,
    122,206,255,89,206,255,89,206,255,89,206,255,89,206,255,89,198,255,212,148,255,110,99,255,
    175,107,255,175,107,255,143,107,255,241,115,254,167,49,254,0,0,250,97,8,32,0,0,0,
    0,0,0,0,0,0,0,0,143,65,0,255,78,99,251,176,115,255,143,107,255,175,107,255,
    175,107,255,111,107,255,78,99,255,78,99,255,78,99,255,78,99,255,78,99,255,78,99,255,
    78,99,255,78,99,255,78,99,255,78,99,255,78,99,255,78,99,255,111,99,255,175,107,255,
    175,107,255,143,107,255,176,115,255,78,99,251,65,0,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,162,8,23,0,0,239,69,41,253,241,115,253,143,107,255,175,107,255,
    175,107,255,175,107,255,176,115,255,176,115,255,176,115,255,176,115,255,176,115,255,176,115,255,
    176,115,255,176,115,255,176,115,255,176,115,255,176,115,255,176,115,255,175,115,255,175,107,255,
    175,107,255,143,107,255,240,115,254,69,41,252,0,0,253,65,0,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,102,0,0,255,41,66,250,241,123,255,143,107,255,
    175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,
    175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,
    143,107,255,241,123,255,41,66,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,106,74,250,241,123,255,
    143,107,255,143,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,
    175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,175,107,255,143,107,255,143,107,255,
    241,123,255,106,74,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,5,0,6,0,0,190,0,0,255,73,66,250,
    240,115,254,176,115,255,143,107,255,143,107,255,175,107,255,175,107,255,175,107,255,175,107,255,
    175,107,255,175,107,255,175,107,255,175,107,255,143,107,255,143,107,255,176,115,255,240,115,254,
    73,66,250,0,0,255,0,0,212,1,0,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,9,0,0,164,0,0,255,
    102,41,255,78,99,251,240,115,254,208,115,255,175,107,255,143,107,255,143,107,255,143,107,255,
    143,107,255,143,107,255,143,107,255,175,107,255,208,115,255,240,115,254,78,99,252,102,41,254,
    0,0,255,0,0,185,0,0,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,65,8,255,167,49,255,13,91,252,176,115,252,240,115,253,240,115,254,240,115,254,
    240,115,254,240,115,254,240,115,253,176,115,252,13,91,251,167,49,255,65,8,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    33,0,35,0,0,159,0,0,242,0,0,255,163,16,255,102,41,255,232,57,255,73,66,255,
    73,66,255,232,57,255,102,41,255,163,16,255,0,0,255,0,0,248,0,0,172,33,0,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,130,16,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,97,8,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,162,16,24,97,8,42,
    97,8,42,130,16,27,73,74,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 1 监控
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,231,8,18,
    231,8,18,82,34,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,137,25,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,229,8,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    101,0,16,0,0,128,0,0,223,0,0,255,165,8,255,76,17,255,208,17,255,18,26,255,
    18,26,255,208,17,255,76,17,255,165,8,255,0,0,255,0,0,230,0,0,142,67,0,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,85,
    0,0,224,34,0,255,142,17,255,185,34,253,94,43,251,159,43,252,127,43,253,127,43,254,
    127,43,254,127,43,253,159,43,252,94,43,251,217,34,252,142,17,255,34,0,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    75,9,255,27,35,251,159,43,253,127,43,255,94,43,255,61,35,255,61,35,255,61,35,255,
    61,35,255,61,35,255,61,35,255,94,43,255,127,43,255,159,43,254,27,35,251,75,9,255,
    0,0,255,0,0,159,5,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,18,26,251,
    127,43,253,94,43,255,61,35,255,61,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,61,43,255,61,35,255,94,43,255,127,43,254,
    18,26,251,0,0,255,0,0,189,3,0,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,52,26,250,159,43,255,
    61,35,255,61,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,61,43,255,61,35,255,
    159,43,255,52,26,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,33,0,81,0,0,255,18,26,250,159,43,254,61,35,255,
    94,43,255,94,43,255,94,43,255,94,43,255,62,35,255,62,35,255,62,35,255,62,35,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    61,35,255,159,43,255,18,26,250,0,0,255,32,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,134,16,14,0,0,230,42,9,254,159,43,253,61,35,255,94,43,255,
    94,43,255,94,43,255,94,43,255,62,35,255,126,51,255,158,51,255,158,51,255,158,51,255,
    62,35,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,61,35,255,159,43,254,43,9,252,0,0,247,67,8,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,34,0,255,27,35,251,94,43,255,61,43,255,94,43,255,
    94,43,255,94,43,255,62,35,255,158,100,255,159,239,255,159,239,255,159,239,255,159,239,255,
    30,125,255,29,35,255,126,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,61,43,255,94,43,255,27,35,251,34,0,255,0,0,160,0,0,0,0,0,0,
    0,0,0,206,25,9,0,0,228,141,17,254,159,43,253,61,35,255,94,43,255,94,43,255,
    94,43,255,62,35,255,157,2,255,158,149,255,255,255,255,255,255,255,255,255,255,255,255,255,
    63,174,255,157,10,255,253,26,255,253,26,255,221,18,255,221,18,255,29,35,255,94,43,255,
    94,43,255,94,43,255,61,35,255,159,43,254,141,17,254,0,0,246,165,8,24,0,0,0,
    0,0,0,0,0,70,0,0,255,185,34,253,127,43,255,61,43,255,94,43,255,94,43,255,
    29,35,255,254,67,255,94,133,255,191,198,255,255,255,255,255,255,255,255,255,255,255,255,255,
    63,223,255,126,141,255,190,149,255,159,149,255,223,157,255,223,157,255,62,84,255,29,27,255,
    94,43,255,94,43,255,61,43,255,127,43,255,217,34,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,133,0,254,94,43,251,94,43,255,94,43,255,94,43,255,61,35,255,
    254,67,255,191,247,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,127,231,255,
    159,239,255,255,255,255,255,255,255,255,255,255,95,223,255,95,223,255,255,255,255,94,84,255,
    29,27,255,94,43,255,94,43,255,94,43,255,94,43,251,133,0,254,0,0,173,0,0,0,
    0,0,0,0,0,192,75,17,255,159,43,252,61,35,255,94,43,255,94,43,255,253,18,255,
    158,149,255,255,255,255,223,255,255,255,255,255,255,255,255,63,223,255,94,92,255,62,35,255,
    29,35,255,62,84,255,63,223,255,255,255,255,62,35,255,30,35,255,255,255,255,63,174,255,
    253,26,255,94,43,255,94,43,255,61,35,255,127,43,253,75,17,255,0,0,217,0,0,0,
    0,0,0,0,0,219,208,17,255,159,43,253,61,35,255,94,43,255,94,43,255,253,26,255,
    158,149,255,255,255,255,255,255,255,255,255,255,159,239,255,190,59,255,189,18,255,62,84,255,
    62,84,255,189,10,255,190,59,255,159,239,255,255,165,255,223,157,255,255,255,255,63,174,255,
    253,26,255,94,43,255,94,43,255,61,35,255,159,43,254,208,17,255,0,0,239,165,8,25,
    231,8,18,0,0,234,18,26,255,127,43,254,61,35,255,94,43,255,94,43,255,253,26,255,
    158,149,255,255,255,255,223,255,255,255,255,255,223,157,255,157,10,255,62,125,255,255,255,255,
    255,255,255,62,125,255,157,10,255,223,157,255,255,255,255,255,255,255,255,255,255,63,174,255,
    253,26,255,94,43,255,94,43,255,61,35,255,127,43,254,18,26,255,0,0,250,66,0,43,
    198,8,19,0,0,235,18,26,255,127,43,254,61,35,255,94,43,255,94,43,255,253,26,255,
    158,149,255,255,255,255,223,255,255,255,255,255,30,125,255,189,18,255,31,215,255,255,255,255,
    255,255,255,31,215,255,189,18,255,30,125,255,255,255,255,223,247,255,255,255,255,63,174,255,
    253,26,255,94,43,255,94,43,255,61,35,255,127,43,254,18,26,255,0,0,251,66,0,43,
    82,34,7,0,0,221,208,17,255,159,43,253,61,35,255,94,43,255,94,43,255,253,26,255,
    158,149,255,255,255,255,223,255,255,255,255,255,223,157,255,157,10,255,62,125,255,255,255,255,
    255,255,255,62,125,255,157,10,255,223,157,255,255,255,255,223,255,255,255,255,255,63,174,255,
    253,26,255,94,43,255,94,43,255,61,35,255,159,43,254,208,17,255,0,0,241,132,8,28,
    0,0,0,0,0,196,75,17,255,159,43,252,61,35,255,94,43,255,94,43,255,253,26,255,
    158,149,255,255,255,255,255,255,255,255,255,255,159,239,255,190,59,255,189,18,255,62,84,255,
    62,84,255,189,18,255,190,59,255,159,239,255,255,255,255,255,255,255,255,255,255,63,174,255,
    253,26,255,94,43,255,94,43,255,61,35,255,127,43,253,75,17,255,0,0,220,0,0,0,
    0,0,0,0,0,151,133,0,254,94,43,251,94,43,255,94,43,255,94,43,255,253,18,255,
    190,149,255,255,255,255,223,255,255,255,255,255,255,255,255,63,223,255,62,84,255,29,27,255,
    29,27,255,62,84,255,63,223,255,255,255,255,255,255,255,223,255,255,255,255,255,95,182,255,
    253,26,255,94,43,255,94,43,255,94,43,255,94,43,252,133,0,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,217,34,252,127,43,255,61,43,255,94,43,255,29,35,255,
    62,84,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,191,247,255,
    191,247,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,158,100,255,
    29,27,255,94,43,255,61,43,255,127,43,255,217,34,251,0,0,255,0,0,111,0,0,0,
    0,0,0,41,17,14,0,0,236,141,17,254,159,43,253,61,35,255,94,43,255,94,43,255,
    29,27,255,94,92,255,31,174,255,63,174,255,63,174,255,63,174,255,63,174,255,95,182,255,
    95,182,255,63,174,255,63,174,255,63,174,255,63,174,255,63,174,255,158,100,255,29,27,255,
    94,43,255,94,43,255,61,35,255,159,43,254,141,17,254,0,0,250,99,0,32,0,0,0,
    0,0,0,0,0,0,0,0,143,34,0,255,27,35,251,94,43,255,61,43,255,94,43,255,
    94,43,255,29,27,255,253,26,255,253,26,255,253,26,255,253,26,255,253,26,255,253,26,255,
    253,26,255,253,26,255,253,26,255,253,26,255,253,26,255,253,26,255,29,27,255,94,43,255,
    94,43,255,61,43,255,94,43,255,27,35,251,34,0,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,68,8,23,0,0,239,42,9,253,159,43,253,61,35,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,61,35,255,159,43,254,43,9,252,0,0,253,34,0,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,102,0,0,255,18,26,250,159,43,255,61,35,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    61,35,255,159,43,255,18,26,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,52,26,250,159,43,255,
    61,35,255,61,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,61,43,255,61,35,255,
    159,43,255,52,26,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,5,0,6,0,0,190,0,0,255,18,26,250,
    127,43,254,94,43,255,61,35,255,61,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,61,43,255,61,35,255,94,43,255,127,43,254,
    18,26,250,0,0,255,0,0,212,1,0,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,9,0,0,164,0,0,255,
    75,9,255,27,35,251,159,43,254,127,43,255,94,43,255,61,35,255,61,35,255,61,35,255,
    61,35,255,61,35,255,61,35,255,94,43,255,127,43,255,159,43,254,27,35,252,75,9,254,
    0,0,255,0,0,185,1,0,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,34,0,255,142,17,255,217,34,252,94,43,252,127,43,253,127,43,254,127,43,254,
    127,43,254,127,43,254,127,43,253,94,43,252,217,34,251,142,17,255,34,0,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    34,0,35,0,0,159,0,0,242,0,0,255,165,8,255,76,17,255,208,17,255,18,26,255,
    18,26,255,208,17,255,76,17,255,165,8,255,0,0,255,0,0,248,0,0,172,33,0,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,131,8,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,98,0,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,165,8,24,99,0,42,
    99,0,42,132,8,27,82,34,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 2 测速
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,97,56,18,
    97,56,18,36,145,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,72,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,0,40,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,40,16,0,0,128,0,0,223,0,0,255,32,40,255,97,88,255,130,120,255,162,136,255,
    162,136,255,130,120,255,97,88,255,32,40,255,0,0,255,0,0,230,0,0,142,0,24,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,85,
    0,0,224,0,16,255,97,104,255,195,184,253,4,225,251,36,241,252,36,241,253,36,241,254,
    36,241,254,36,241,253,36,241,252,4,225,251,195,184,252,97,104,255,0,16,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    97,80,255,227,208,251,36,241,253,36,233,255,227,224,255,162,216,255,130,216,255,97,216,255,
    97,216,255,130,216,255,162,216,255,227,224,255,36,233,255,36,241,254,227,208,251,97,80,255,
    0,0,255,0,0,159,0,40,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,162,136,251,
    4,241,253,36,225,255,227,216,255,130,216,255,4,225,255,105,226,255,174,235,255,113,244,255,
    113,244,255,174,235,255,105,226,255,4,225,255,130,216,255,227,216,255,36,225,255,4,241,254,
    162,136,251,0,0,255,0,0,189,0,24,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,162,144,250,4,241,255,
    4,217,255,162,216,255,69,225,255,81,236,255,251,254,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,251,254,255,81,236,255,69,225,255,162,216,255,4,217,255,
    4,241,255,162,144,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,81,0,0,255,130,136,250,4,241,254,4,217,255,
    130,216,255,170,234,255,28,255,255,255,255,255,255,255,255,223,255,255,223,255,255,223,255,255,
    223,255,255,223,255,255,223,255,255,255,255,255,255,255,255,28,255,255,170,234,255,130,216,255,
    4,217,255,4,241,255,130,136,250,0,0,255,0,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,48,14,0,0,230,97,80,254,4,241,253,4,217,255,130,216,255,
    77,235,255,255,255,255,255,255,255,223,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,223,255,255,255,255,255,255,255,255,77,235,255,
    130,216,255,4,217,255,4,241,254,97,80,252,0,0,247,0,24,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,0,8,255,227,208,251,36,225,255,162,216,255,170,234,255,
    255,255,255,255,255,255,223,255,255,255,255,255,190,247,255,182,181,255,77,107,255,105,74,255,
    73,74,255,77,107,255,182,181,255,190,247,255,255,255,255,223,255,255,255,255,255,255,255,255,
    170,234,255,162,216,255,36,225,255,227,208,251,0,8,255,0,0,160,0,0,0,0,0,0,
    0,0,0,0,80,9,0,0,228,97,96,254,4,241,253,227,216,255,69,225,255,60,255,255,
    255,255,255,223,255,255,255,255,255,117,173,255,69,41,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,36,33,255,117,173,255,255,255,255,223,255,255,255,255,255,
    60,255,255,69,225,255,227,216,255,4,241,254,97,96,254,0,0,246,0,24,24,0,0,0,
    0,0,0,0,0,70,0,0,255,195,184,253,36,233,255,97,216,255,113,244,255,255,255,255,
    223,255,255,255,255,255,207,123,255,0,0,255,0,0,255,36,33,255,142,115,255,20,165,255,
    20,165,255,142,115,255,69,41,255,0,0,255,0,0,255,207,123,255,255,255,255,223,255,255,
    255,255,255,113,244,255,97,216,255,36,233,255,195,184,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,32,40,254,4,225,251,227,224,255,4,225,255,28,255,255,255,255,255,
    255,255,255,81,140,255,0,0,255,32,0,255,48,132,255,158,247,255,255,255,255,255,255,255,
    255,255,255,255,255,255,24,198,255,65,8,255,0,0,255,0,0,255,81,140,255,255,255,255,
    255,255,255,28,255,255,4,225,255,227,224,255,4,225,251,32,40,254,0,0,173,0,0,0,
    0,0,0,0,0,192,97,88,255,36,241,252,162,216,255,105,234,255,255,255,255,255,255,255,
    121,214,255,0,0,255,0,0,255,52,165,255,255,255,255,255,255,255,223,255,255,190,247,255,
    223,255,255,125,239,255,101,41,255,0,0,255,231,57,255,65,8,255,0,0,255,121,214,255,
    255,255,255,255,255,255,105,234,255,162,216,255,36,241,253,97,88,255,0,0,217,0,0,0,
    0,0,0,0,0,219,130,120,255,36,241,253,130,216,255,207,235,255,255,255,255,255,255,255,
    170,82,255,0,0,255,203,90,255,255,255,255,223,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,170,82,255,0,0,255,105,74,255,223,255,255,235,90,255,0,0,255,170,82,255,
    255,255,255,255,255,255,207,235,255,130,216,255,36,241,254,130,120,255,0,0,239,65,40,25,
    97,56,18,0,0,234,162,136,255,36,241,254,97,216,255,113,236,255,255,255,255,93,239,255,
    97,8,255,0,0,255,121,206,255,255,255,255,223,255,255,255,255,255,223,255,255,243,156,255,
    203,90,255,0,0,255,101,41,255,158,247,255,255,255,255,89,206,255,0,0,255,97,8,255,
    93,239,255,255,255,255,113,236,255,97,216,255,36,241,254,162,136,255,0,0,250,32,16,43,
    97,48,19,0,0,235,162,136,255,36,241,254,97,216,255,113,236,255,255,255,255,24,198,255,
    0,0,255,4,33,255,223,255,255,255,255,255,255,255,255,255,255,255,235,90,255,0,0,255,
    0,0,255,65,8,255,154,214,255,255,255,255,255,255,255,223,255,255,4,33,255,0,0,255,
    24,198,255,255,255,255,113,236,255,97,216,255,36,241,254,162,136,255,0,0,251,32,16,43,
    36,145,7,0,0,221,130,120,255,36,241,253,130,216,255,174,235,255,255,255,255,215,189,255,
    0,0,255,166,49,255,255,255,255,255,255,255,255,255,255,158,247,255,65,8,255,32,0,255,
    65,8,255,0,0,255,28,231,255,255,255,255,255,255,255,255,255,255,166,49,255,0,0,255,
    215,189,255,255,255,255,174,235,255,130,216,255,36,241,254,130,120,255,0,0,241,65,32,28,
    0,0,0,0,0,196,97,88,255,36,241,252,162,216,255,105,226,255,255,255,255,24,198,255,
    0,0,255,4,33,255,223,255,255,255,255,255,255,255,255,255,255,255,73,74,255,0,0,255,
    0,0,255,101,41,255,190,247,255,255,255,255,255,255,255,223,255,255,4,33,255,0,0,255,
    24,198,255,255,255,255,105,226,255,162,216,255,36,241,253,97,88,255,0,0,220,0,0,0,
    0,0,0,0,0,151,32,40,254,4,225,251,227,224,255,4,225,255,60,255,255,125,239,255,
    97,8,255,0,0,255,121,206,255,255,255,255,223,255,255,255,255,255,125,239,255,207,123,255,
    142,115,255,28,231,255,255,255,255,223,255,255,255,255,255,121,206,255,0,0,255,97,8,255,
    125,239,255,60,255,255,4,225,255,227,224,255,4,225,252,32,40,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,195,184,252,36,233,255,130,216,255,81,236,255,255,255,255,
    138,82,255,0,0,255,170,82,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,170,82,255,0,0,255,138,82,255,
    255,255,255,81,236,255,130,216,255,36,233,255,195,184,251,0,0,255,0,0,111,0,0,0,
    0,0,0,0,48,14,0,0,236,97,96,254,4,241,253,227,216,255,36,225,255,93,255,255,
    186,214,255,97,8,255,105,74,255,255,255,255,255,255,255,255,255,255,255,255,255,223,255,255,
    223,255,255,255,255,255,255,255,255,255,255,255,255,255,255,105,74,255,97,8,255,186,214,255,
    93,255,255,36,225,255,227,216,255,4,241,254,97,96,254,0,0,250,0,16,32,0,0,0,
    0,0,0,0,0,0,0,0,143,0,8,255,227,208,251,36,225,255,162,216,255,170,234,255,
    255,255,255,60,231,255,190,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,190,255,255,60,231,255,255,255,255,
    170,234,255,162,216,255,36,225,255,227,208,251,0,8,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,0,32,23,0,0,239,97,80,253,4,241,253,4,217,255,130,216,255,
    77,235,255,255,255,255,255,255,255,223,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,223,255,255,255,255,255,255,255,255,77,235,255,
    130,216,255,4,217,255,4,241,254,97,80,252,0,0,253,0,16,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,102,0,0,255,130,136,250,4,241,255,4,217,255,
    130,216,255,170,226,255,251,254,255,255,255,255,255,255,255,255,255,255,255,255,255,223,255,255,
    223,255,255,255,255,255,255,255,255,255,255,255,255,255,255,251,254,255,170,226,255,130,216,255,
    4,217,255,4,241,255,130,136,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,162,144,250,4,241,255,
    4,217,255,162,216,255,69,225,255,81,236,255,251,254,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,251,254,255,81,236,255,69,225,255,162,216,255,4,217,255,
    4,241,255,162,144,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,40,6,0,0,190,0,0,255,162,136,250,
    4,241,254,36,225,255,227,216,255,130,216,255,4,225,255,105,226,255,174,235,255,113,244,255,
    113,244,255,174,235,255,105,226,255,4,225,255,130,216,255,227,216,255,36,225,255,4,241,254,
    162,136,250,0,0,255,0,0,212,0,8,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,24,9,0,0,164,0,0,255,
    97,80,255,227,208,251,36,241,254,36,233,255,227,224,255,162,216,255,130,216,255,97,216,255,
    97,216,255,130,216,255,162,216,255,227,224,255,36,233,255,36,241,254,227,208,252,97,80,254,
    0,0,255,0,0,185,0,8,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,0,16,255,97,104,255,195,184,252,4,225,252,36,241,253,36,241,254,36,241,254,
    36,241,254,36,241,254,36,241,253,4,225,252,195,192,251,97,104,255,0,16,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,16,35,0,0,159,0,0,242,0,0,255,32,40,255,97,88,255,130,120,255,162,136,255,
    162,136,255,130,120,255,97,88,255,32,40,255,0,0,255,0,0,248,0,0,172,0,8,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,24,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,0,16,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,65,40,24,32,24,42,
    32,24,42,65,32,27,36,145,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 3 闯红灯
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,97,56,18,
    97,56,18,36,145,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,72,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,0,40,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,40,16,0,0,128,0,0,223,0,0,255,32,40,255,97,88,255,130,120,255,162,136,255,
    162,136,255,130,120,255,97,88,255,32,40,255,0,0,255,0,0,230,0,0,142,0,24,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,85,
    0,0,224,0,16,255,97,104,255,195,184,253,4,225,251,4,241,252,4,241,253,4,241,254,
    4,241,254,4,241,253,4,241,252,4,225,251,195,184,252,97,104,255,0,16,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    97,80,255,227,208,251,4,241,253,4,233,255,4,225,255,4,217,255,4,217,255,4,217,255,
    4,217,255,4,217,255,4,217,255,4,225,255,4,233,255,4,241,254,227,208,251,97,80,255,
    0,0,255,0,0,159,0,40,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,162,136,251,
    4,241,253,4,225,255,227,216,255,227,216,255,4,225,255,195,224,255,130,216,255,97,216,255,
    97,216,255,130,216,255,195,224,255,4,225,255,227,216,255,227,216,255,4,225,255,4,241,254,
    162,136,251,0,0,255,0,0,189,0,24,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,162,144,250,4,241,255,
    4,217,255,4,217,255,4,225,255,4,225,255,162,224,255,166,225,255,239,235,255,146,244,255,
    146,244,255,16,236,255,8,226,255,162,224,255,4,225,255,4,225,255,4,217,255,4,217,255,
    4,241,255,162,144,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,81,0,0,255,130,136,250,4,241,254,4,217,255,
    4,225,255,4,225,255,4,225,255,162,224,255,105,226,255,125,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,190,255,255,12,235,255,162,216,255,4,225,255,4,225,255,4,225,255,
    4,217,255,4,241,255,130,136,250,0,0,255,0,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,48,14,0,0,230,97,80,254,4,241,253,4,217,255,4,225,255,
    4,225,255,4,225,255,4,225,255,195,224,255,121,254,255,255,255,255,243,244,255,134,225,255,
    101,225,255,81,236,255,255,255,255,60,255,255,36,225,255,227,224,255,4,225,255,4,225,255,
    4,225,255,4,217,255,4,241,254,97,80,252,0,0,247,0,24,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,0,8,255,227,208,251,4,225,255,4,217,255,4,225,255,
    4,225,255,4,225,255,227,224,255,101,225,255,223,255,255,89,254,255,130,216,255,195,224,255,
    227,224,255,65,216,255,117,245,255,255,255,255,8,226,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,217,255,4,225,255,227,208,251,0,8,255,0,0,160,0,0,0,0,0,0,
    0,0,0,0,80,9,0,0,228,97,96,254,4,241,253,4,217,255,4,225,255,4,225,255,
    4,225,255,4,225,255,227,224,255,69,225,255,255,255,255,113,244,255,130,216,255,36,225,255,
    36,225,255,130,216,255,109,235,255,255,255,255,231,225,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,225,255,4,217,255,4,241,254,97,96,254,0,0,246,0,24,24,0,0,0,
    0,0,0,0,0,70,0,0,255,195,184,253,4,233,255,4,217,255,4,225,255,4,225,255,
    4,225,255,4,225,255,227,224,255,101,225,255,255,255,255,24,246,255,97,216,255,227,216,255,
    4,225,255,32,216,255,20,245,255,255,255,255,231,225,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,225,255,4,217,255,4,233,255,195,184,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,32,40,254,4,225,251,4,225,255,4,225,255,4,225,255,4,225,255,
    4,225,255,4,225,255,227,224,255,101,225,255,125,255,255,255,255,255,81,244,255,69,233,255,
    36,225,255,174,235,255,223,255,255,255,255,255,8,226,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,225,255,4,225,255,4,225,255,4,225,251,32,40,254,0,0,173,0,0,0,
    0,0,0,0,0,192,97,88,255,4,241,252,4,217,255,4,225,255,4,225,255,4,225,255,
    4,225,255,4,225,255,227,224,255,101,225,255,125,255,255,255,255,255,255,255,255,56,222,255,
    24,222,255,255,255,255,255,255,255,255,255,255,8,226,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,225,255,4,225,255,4,217,255,4,241,253,97,88,255,0,0,217,0,0,0,
    0,0,0,0,0,219,130,120,255,4,241,253,227,216,255,4,225,255,4,225,255,4,225,255,
    4,225,255,4,225,255,227,224,255,101,225,255,158,255,255,255,255,255,150,181,255,146,140,255,
    178,140,255,150,181,255,255,255,255,255,255,255,8,226,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,225,255,4,225,255,227,216,255,4,241,254,130,120,255,0,0,239,65,40,25,
    97,56,18,0,0,234,162,136,255,4,241,254,4,217,255,4,225,255,4,225,255,4,225,255,
    4,225,255,4,225,255,227,224,255,101,225,255,190,255,255,28,223,255,113,140,255,178,156,255,
    178,156,255,113,140,255,251,222,255,255,255,255,8,226,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,225,255,4,225,255,4,217,255,4,241,254,162,136,255,0,0,250,32,16,43,
    97,48,19,0,0,235,162,136,255,4,241,254,4,217,255,4,225,255,4,225,255,4,225,255,
    4,225,255,4,225,255,227,224,255,101,225,255,190,255,255,28,223,255,113,140,255,211,156,255,
    211,156,255,113,140,255,251,222,255,255,255,255,8,226,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,225,255,4,225,255,4,217,255,4,241,254,162,136,255,0,0,251,32,16,43,
    36,145,7,0,0,221,130,120,255,4,241,253,227,216,255,4,225,255,4,225,255,4,225,255,
    4,225,255,4,225,255,227,224,255,101,225,255,158,255,255,255,255,255,150,181,255,113,140,255,
    113,140,255,150,181,255,255,255,255,255,255,255,8,226,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,225,255,4,225,255,227,216,255,4,241,254,130,120,255,0,0,241,65,32,28,
    0,0,0,0,0,196,97,88,255,4,241,252,4,217,255,4,225,255,4,225,255,4,225,255,
    4,225,255,4,225,255,227,224,255,101,225,255,125,255,255,255,255,255,255,255,255,219,222,255,
    219,222,255,255,255,255,255,255,255,255,255,255,8,226,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,225,255,4,225,255,4,217,255,4,241,253,97,88,255,0,0,220,0,0,0,
    0,0,0,0,0,151,32,40,254,4,225,251,4,225,255,4,225,255,4,225,255,4,225,255,
    4,225,255,4,225,255,227,224,255,101,225,255,125,255,255,255,255,255,186,214,255,52,165,255,
    52,165,255,186,214,255,255,255,255,255,255,255,8,226,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,225,255,4,225,255,4,225,255,4,225,252,32,40,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,195,184,252,4,233,255,4,217,255,4,225,255,4,225,255,
    4,225,255,4,225,255,227,224,255,101,225,255,190,255,255,125,239,255,146,148,255,146,148,255,
    146,148,255,146,148,255,93,239,255,255,255,255,8,226,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,225,255,4,217,255,4,233,255,195,184,251,0,0,255,0,0,111,0,0,0,
    0,0,0,0,48,14,0,0,236,97,96,254,4,241,253,4,217,255,4,225,255,4,225,255,
    4,225,255,4,225,255,227,224,255,101,225,255,190,255,255,219,214,255,113,140,255,211,156,255,
    211,156,255,113,140,255,186,214,255,255,255,255,8,226,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,225,255,4,217,255,4,241,254,97,96,254,0,0,250,0,16,32,0,0,0,
    0,0,0,0,0,0,0,0,143,0,8,255,227,208,251,4,225,255,4,217,255,4,225,255,
    4,225,255,4,225,255,227,224,255,134,225,255,190,255,255,190,247,255,211,156,255,113,140,255,
    113,140,255,211,156,255,158,247,255,255,255,255,8,226,255,195,224,255,4,225,255,4,225,255,
    4,225,255,4,217,255,4,225,255,227,208,251,0,8,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,0,32,23,0,0,239,97,80,253,4,241,253,4,217,255,4,225,255,
    4,225,255,4,225,255,4,225,255,227,224,255,219,254,255,255,255,255,60,239,255,150,181,255,
    150,181,255,60,239,255,255,255,255,125,255,255,69,225,255,227,224,255,4,225,255,4,225,255,
    4,225,255,4,217,255,4,241,254,97,80,252,0,0,253,0,16,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,102,0,0,255,130,136,250,4,241,255,4,217,255,
    4,225,255,4,225,255,4,225,255,162,216,255,235,234,255,223,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,174,235,255,130,216,255,4,225,255,4,225,255,4,225,255,
    4,217,255,4,241,255,130,136,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,162,144,250,4,241,255,
    4,217,255,4,217,255,4,225,255,4,225,255,162,224,255,73,226,255,211,244,255,85,245,255,
    85,245,255,20,245,255,170,234,255,162,224,255,4,225,255,4,225,255,4,217,255,4,217,255,
    4,241,255,162,144,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,40,6,0,0,190,0,0,255,162,136,250,
    4,241,254,4,225,255,227,216,255,227,216,255,4,225,255,162,224,255,130,216,255,130,216,255,
    130,216,255,130,216,255,162,224,255,4,225,255,4,217,255,227,216,255,4,225,255,4,241,254,
    162,136,250,0,0,255,0,0,212,0,8,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,24,9,0,0,164,0,0,255,
    97,80,255,227,208,251,4,241,254,4,233,255,4,225,255,4,217,255,4,217,255,4,217,255,
    4,217,255,4,217,255,4,217,255,4,225,255,4,233,255,4,241,254,227,208,252,97,80,254,
    0,0,255,0,0,185,0,8,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,0,16,255,97,104,255,195,184,252,4,225,252,4,241,253,4,241,254,4,241,254,
    4,241,254,4,241,254,4,241,253,4,225,252,195,192,251,97,104,255,0,16,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,16,35,0,0,159,0,0,242,0,0,255,32,40,255,97,88,255,130,120,255,162,136,255,
    162,136,255,130,120,255,97,88,255,32,40,255,0,0,255,0,0,248,0,0,172,0,8,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,24,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,0,16,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,65,40,24,32,24,42,
    32,24,42,65,32,27,36,145,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 4 违章拍照
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,224,56,18,
    224,56,18,64,146,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,128,73,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,224,40,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    96,40,16,0,0,128,0,0,223,0,0,255,160,40,255,129,97,255,1,130,255,97,146,255,
    97,146,255,1,130,255,129,97,255,160,40,255,0,0,255,0,0,230,0,0,142,64,24,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,85,
    0,0,224,64,16,255,193,113,255,34,203,253,194,243,251,2,252,252,2,252,253,2,252,254,
    2,252,254,2,252,253,2,252,252,194,243,251,34,203,252,193,113,255,64,16,255,0,0,235,
    32,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    97,89,255,98,219,251,2,252,253,226,251,255,194,243,255,162,235,255,162,235,255,162,235,255,
    162,235,255,162,235,255,162,235,255,194,243,255,226,251,255,2,252,254,65,219,251,32,89,255,
    0,0,255,0,0,159,0,40,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,65,146,251,
    2,252,253,194,243,255,162,235,255,162,235,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,235,255,129,235,255,102,252,255,42,253,254,
    197,146,251,0,0,255,0,0,189,0,24,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,129,162,250,2,252,255,
    162,235,255,162,235,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,162,243,255,195,243,255,124,255,255,255,255,255,
    167,252,255,97,162,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,8,81,0,0,255,65,146,250,2,252,254,162,235,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,195,243,255,96,235,255,18,254,255,255,255,255,143,245,255,
    64,235,255,35,252,255,65,146,250,0,0,255,32,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,49,14,0,0,230,64,81,254,2,252,253,162,235,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,129,235,255,136,244,255,255,255,255,216,254,255,96,235,255,
    161,235,255,162,235,255,2,252,254,64,89,252,0,0,247,128,24,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,64,16,255,98,219,251,194,243,255,162,235,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,161,243,255,25,255,255,255,255,255,143,245,255,169,244,255,
    135,244,255,161,235,255,194,243,255,98,219,251,64,16,255,0,0,160,0,0,0,0,0,0,
    0,0,0,192,113,9,0,0,228,161,105,254,2,252,253,162,235,255,194,243,255,194,243,255,
    194,243,255,194,243,255,162,243,255,129,235,255,129,235,255,128,235,255,161,243,255,194,243,255,
    194,243,255,195,243,255,96,235,255,143,245,255,255,255,255,255,255,255,255,255,255,255,255,255,
    168,244,255,129,243,255,162,235,255,2,252,254,161,105,254,0,0,246,160,40,24,0,0,0,
    0,0,0,0,0,70,0,0,255,34,203,253,226,251,255,162,235,255,194,243,255,194,243,255,
    194,243,255,194,243,255,227,243,255,201,244,255,202,244,255,202,244,255,70,244,255,162,243,255,
    195,243,255,161,243,255,103,244,255,255,255,255,255,255,255,255,255,255,255,255,255,77,245,255,
    96,235,255,195,243,255,162,235,255,226,251,255,34,203,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,160,40,254,194,243,251,194,243,255,194,243,255,194,243,255,194,243,255,
    195,243,255,64,235,255,110,245,255,255,255,255,255,255,255,255,255,255,156,255,255,194,243,255,
    129,235,255,128,235,255,37,244,255,169,244,255,11,245,255,255,255,255,117,254,255,96,235,255,
    195,243,255,194,243,255,194,243,255,194,243,255,194,243,251,160,40,254,0,0,173,0,0,0,
    0,0,0,0,0,192,97,89,255,2,252,252,162,235,255,194,243,255,194,243,255,194,243,255,
    161,235,255,227,243,255,51,254,255,255,255,255,222,255,255,255,255,255,222,255,255,168,244,255,
    70,244,255,103,244,255,103,244,255,161,243,255,44,245,255,157,255,255,194,243,255,162,243,255,
    194,243,255,194,243,255,194,243,255,162,235,255,2,252,253,97,89,255,0,0,217,0,0,0,
    0,0,0,0,0,219,1,130,255,2,252,253,162,235,255,194,243,255,194,243,255,129,235,255,
    142,245,255,222,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,189,255,255,189,255,255,156,255,255,168,244,255,129,235,255,194,243,255,
    194,243,255,194,243,255,194,243,255,162,235,255,2,252,254,1,130,255,0,0,239,160,40,25,
    224,56,18,0,0,234,65,146,255,2,252,254,162,235,255,194,243,255,162,243,255,227,243,255,
    190,255,255,255,255,255,255,255,255,255,255,255,248,254,255,201,244,255,37,244,255,44,245,255,
    157,255,255,190,255,255,128,235,255,84,254,255,223,255,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,162,235,255,2,252,254,65,146,255,0,0,250,64,16,43,
    192,48,19,0,0,235,65,146,255,2,252,254,162,235,255,194,243,255,162,243,255,4,244,255,
    189,255,255,255,255,255,255,255,255,25,255,255,129,235,255,162,243,255,136,244,255,64,235,255,
    70,244,255,189,255,255,143,245,255,25,255,255,223,255,255,4,244,255,161,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,162,235,255,2,252,254,65,146,255,0,0,251,64,16,43,
    64,146,7,0,0,221,1,130,255,2,252,253,162,235,255,194,243,255,162,243,255,4,244,255,
    189,255,255,255,255,255,255,255,255,44,245,255,129,243,255,58,255,255,255,255,255,116,254,255,
    32,235,255,84,254,255,255,255,255,255,255,255,189,255,255,4,244,255,162,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,162,235,255,2,252,254,1,130,255,0,0,241,128,32,28,
    0,0,0,0,0,196,97,89,255,2,252,252,162,235,255,194,243,255,162,243,255,4,244,255,
    189,255,255,255,255,255,255,255,255,168,244,255,4,244,255,255,255,255,255,255,255,124,255,255,
    96,235,255,209,245,255,255,255,255,255,255,255,189,255,255,4,244,255,162,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,162,235,255,2,252,253,97,89,255,0,0,220,0,0,0,
    0,0,0,0,0,151,160,40,254,194,243,251,194,243,255,194,243,255,162,243,255,4,244,255,
    189,255,255,255,255,255,255,255,255,18,254,255,32,235,255,142,245,255,215,254,255,169,244,255,
    64,235,255,25,255,255,255,255,255,255,255,255,189,255,255,4,244,255,162,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,243,252,160,40,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,34,203,252,226,251,255,194,235,255,162,243,255,4,244,255,
    222,255,255,255,255,255,255,255,255,222,255,255,11,245,255,32,235,255,32,235,255,96,235,255,
    241,253,255,255,255,255,223,255,255,255,255,255,222,255,255,4,244,255,162,243,255,194,243,255,
    194,243,255,194,243,255,162,235,255,226,251,255,34,203,251,0,0,255,0,0,111,0,0,0,
    0,0,0,32,73,14,0,0,236,161,105,254,2,252,253,162,235,255,194,243,255,161,243,255,
    249,254,255,255,255,255,255,255,255,255,255,255,255,255,255,90,255,255,182,254,255,157,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,248,254,255,161,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,162,235,255,2,252,254,161,105,254,0,0,250,96,24,32,0,0,0,
    0,0,0,0,0,0,0,0,143,64,16,255,98,219,251,194,243,255,194,235,255,161,243,255,
    37,244,255,18,254,255,116,254,255,116,254,255,116,254,255,150,254,255,182,254,255,149,254,255,
    84,254,255,116,254,255,116,254,255,18,254,255,4,244,255,161,243,255,194,243,255,194,243,255,
    194,243,255,162,235,255,194,243,255,98,219,251,64,16,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,160,32,23,0,0,239,64,81,253,2,252,253,162,235,255,194,243,255,
    161,243,255,96,235,255,96,235,255,96,235,255,96,235,255,96,235,255,96,235,255,96,235,255,
    96,235,255,96,235,255,96,235,255,96,235,255,162,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,162,235,255,2,252,254,64,89,252,0,0,253,64,16,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,102,0,0,255,65,146,250,2,252,255,162,235,255,
    194,243,255,195,243,255,194,243,255,195,243,255,195,243,255,195,243,255,195,243,255,195,243,255,
    195,243,255,195,243,255,194,243,255,195,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    162,235,255,2,252,255,65,146,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,129,162,250,2,252,255,
    162,235,255,162,235,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,162,235,255,162,235,255,
    2,252,255,129,162,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,40,6,0,0,190,0,0,255,65,146,250,
    2,252,254,194,243,255,162,235,255,162,235,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,162,235,255,162,235,255,194,243,255,2,252,254,
    65,146,250,0,0,255,0,0,212,0,8,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,24,9,0,0,164,0,0,255,
    97,89,255,98,219,251,2,252,254,226,251,255,194,243,255,162,235,255,162,235,255,162,235,255,
    162,235,255,162,235,255,162,235,255,194,243,255,226,251,255,2,252,254,98,219,252,97,89,254,
    0,0,255,0,0,185,0,8,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,64,16,255,193,113,255,34,203,252,194,243,252,2,252,253,2,252,254,2,252,254,
    2,252,254,2,252,254,2,252,253,194,243,252,34,203,251,193,113,255,64,16,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,16,35,0,0,159,0,0,242,0,0,255,160,40,255,129,97,255,1,130,255,97,146,255,
    97,146,255,1,130,255,129,97,255,160,40,255,0,0,255,0,0,248,0,0,172,32,8,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,128,24,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,96,16,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,160,40,24,96,24,42,
    96,24,42,128,32,27,64,146,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 5 公交道
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,231,8,18,
    231,8,18,82,34,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,137,25,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,229,8,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    101,0,16,0,0,128,0,0,223,0,0,255,165,8,255,76,17,255,208,17,255,18,26,255,
    18,26,255,208,17,255,76,17,255,165,8,255,0,0,255,0,0,230,0,0,142,67,0,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,85,
    0,0,224,34,0,255,142,17,255,185,34,253,94,43,251,159,43,252,127,43,253,127,43,254,
    127,43,254,127,43,253,159,43,252,94,43,251,217,34,252,142,17,255,34,0,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    75,9,255,27,35,251,159,43,253,127,43,255,94,43,255,61,35,255,61,35,255,61,35,255,
    61,35,255,61,35,255,61,35,255,94,43,255,127,43,255,159,43,254,27,35,251,75,9,255,
    0,0,255,0,0,159,5,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,18,26,251,
    127,43,253,94,43,255,61,35,255,93,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,93,43,255,61,43,255,94,43,255,127,43,254,
    18,26,251,0,0,255,0,0,189,3,0,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,52,26,250,159,43,255,
    61,35,255,93,43,255,94,43,255,29,27,255,253,26,255,253,26,255,253,26,255,253,26,255,
    253,26,255,253,26,255,253,26,255,253,26,255,253,26,255,94,43,255,93,43,255,61,35,255,
    159,43,255,52,26,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,33,0,81,0,0,255,18,26,250,159,43,254,61,35,255,
    94,43,255,62,35,255,29,35,255,190,100,255,158,141,255,158,149,255,158,149,255,158,149,255,
    158,149,255,158,149,255,158,149,255,158,149,255,222,108,255,62,35,255,62,35,255,94,43,255,
    61,35,255,159,43,255,18,26,250,0,0,255,32,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,134,16,14,0,0,230,42,9,254,159,43,253,61,35,255,94,43,255,
    94,43,255,29,35,255,63,174,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,191,198,255,94,43,255,62,35,255,
    94,43,255,61,35,255,159,43,254,43,9,252,0,0,247,67,8,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,34,0,255,27,35,251,94,43,255,61,43,255,94,43,255,
    29,27,255,190,100,255,255,255,255,191,247,255,191,198,255,191,198,255,191,198,255,191,198,255,
    191,198,255,191,198,255,191,198,255,191,198,255,159,239,255,255,255,255,62,133,255,253,26,255,
    94,43,255,61,43,255,94,43,255,27,35,251,34,0,255,0,0,160,0,0,0,0,0,0,
    0,0,0,206,25,9,0,0,228,141,17,254,159,43,253,61,35,255,94,43,255,94,43,255,
    253,26,255,158,149,255,255,255,255,254,116,255,221,18,255,29,35,255,29,27,255,29,27,255,
    29,27,255,29,27,255,29,35,255,221,18,255,126,92,255,255,255,255,63,174,255,253,26,255,
    94,43,255,94,43,255,61,35,255,159,43,254,141,17,254,0,0,246,165,8,24,0,0,0,
    0,0,0,0,0,70,0,0,255,185,34,253,127,43,255,61,43,255,94,43,255,94,43,255,
    253,26,255,158,149,255,255,255,255,94,84,255,29,27,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,222,67,255,255,255,255,63,174,255,253,26,255,
    94,43,255,94,43,255,61,43,255,127,43,255,217,34,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,133,0,254,94,43,251,94,43,255,94,43,255,94,43,255,94,43,255,
    253,26,255,158,149,255,255,255,255,126,92,255,29,27,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,29,35,255,254,75,255,255,255,255,63,174,255,253,26,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,251,133,0,254,0,0,173,0,0,0,
    0,0,0,0,0,192,75,17,255,159,43,252,61,35,255,94,43,255,94,43,255,94,43,255,
    253,26,255,158,149,255,255,255,255,94,92,255,29,35,255,126,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,254,67,255,255,255,255,63,174,255,253,26,255,
    94,43,255,94,43,255,94,43,255,61,35,255,127,43,253,75,17,255,0,0,217,0,0,0,
    0,0,0,0,0,219,208,17,255,159,43,253,61,35,255,94,43,255,94,43,255,94,43,255,
    253,26,255,158,149,255,255,255,255,190,108,255,189,10,255,29,27,255,253,26,255,253,26,255,
    253,26,255,253,26,255,253,26,255,189,10,255,62,84,255,255,255,255,63,174,255,253,26,255,
    94,43,255,94,43,255,94,43,255,61,35,255,159,43,254,208,17,255,0,0,239,165,8,25,
    231,8,18,0,0,234,18,26,255,127,43,254,61,35,255,94,43,255,94,43,255,94,43,255,
    253,26,255,158,149,255,255,255,255,127,231,255,63,174,255,63,174,255,63,174,255,63,174,255,
    63,174,255,63,174,255,63,174,255,31,174,255,95,223,255,255,255,255,63,174,255,253,26,255,
    94,43,255,94,43,255,94,43,255,61,35,255,127,43,254,18,26,255,0,0,250,66,0,43,
    198,8,19,0,0,235,18,26,255,127,43,254,61,35,255,94,43,255,94,43,255,94,43,255,
    253,26,255,158,149,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,63,174,255,253,26,255,
    94,43,255,94,43,255,94,43,255,61,35,255,127,43,254,18,26,255,0,0,251,66,0,43,
    82,34,7,0,0,221,208,17,255,159,43,253,61,35,255,94,43,255,94,43,255,94,43,255,
    253,26,255,158,149,255,255,255,255,255,255,255,95,231,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,95,231,255,255,255,255,255,255,255,63,174,255,253,26,255,
    94,43,255,94,43,255,94,43,255,61,35,255,159,43,254,208,17,255,0,0,241,132,8,28,
    0,0,0,0,0,196,75,17,255,159,43,252,61,35,255,94,43,255,94,43,255,94,43,255,
    253,26,255,158,149,255,255,255,255,223,157,255,29,27,255,222,116,255,255,255,255,255,255,255,
    255,255,255,255,255,255,222,116,255,29,27,255,223,157,255,255,255,255,63,174,255,253,26,255,
    94,43,255,94,43,255,94,43,255,61,35,255,127,43,253,75,17,255,0,0,220,0,0,0,
    0,0,0,0,0,151,133,0,254,94,43,251,94,43,255,94,43,255,94,43,255,94,43,255,
    253,26,255,158,149,255,255,255,255,126,92,255,125,2,255,158,51,255,223,247,255,255,255,255,
    255,255,255,223,247,255,158,51,255,125,2,255,126,100,255,255,255,255,63,174,255,253,26,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,252,133,0,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,217,34,252,127,43,255,61,43,255,94,43,255,94,43,255,
    253,26,255,222,116,255,255,255,255,95,231,255,30,125,255,191,198,255,255,255,255,255,255,255,
    255,255,255,255,255,255,191,198,255,30,125,255,95,231,255,255,255,255,126,141,255,253,26,255,
    94,43,255,94,43,255,61,43,255,127,43,255,217,34,251,0,0,255,0,0,111,0,0,0,
    0,0,0,41,17,14,0,0,236,141,17,254,159,43,253,61,35,255,94,43,255,94,43,255,
    94,43,255,62,35,255,223,206,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,63,223,255,126,51,255,62,35,255,
    94,43,255,94,43,255,61,35,255,159,43,254,141,17,254,0,0,250,99,0,32,0,0,0,
    0,0,0,0,0,0,0,0,143,34,0,255,27,35,251,94,43,255,61,43,255,94,43,255,
    94,43,255,253,18,255,126,141,255,255,255,255,223,255,255,223,206,255,31,174,255,63,174,255,
    63,174,255,31,174,255,159,198,255,223,255,255,255,255,255,31,174,255,253,18,255,94,43,255,
    94,43,255,61,43,255,94,43,255,27,35,251,34,0,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,68,8,23,0,0,239,42,9,253,159,43,253,61,35,255,94,43,255,
    94,43,255,253,26,255,222,157,255,255,255,255,255,255,255,254,116,255,157,10,255,29,27,255,
    29,27,255,189,10,255,94,92,255,255,255,255,255,255,255,95,182,255,253,26,255,94,43,255,
    94,43,255,61,35,255,159,43,254,43,9,252,0,0,253,34,0,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,102,0,0,255,18,26,250,159,43,255,61,35,255,
    94,43,255,29,27,255,222,108,255,255,255,255,223,247,255,94,92,255,61,35,255,126,43,255,
    94,43,255,62,35,255,254,67,255,159,239,255,255,255,255,94,133,255,253,26,255,94,43,255,
    61,35,255,159,43,255,18,26,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,52,26,250,159,43,255,
    61,35,255,61,43,255,62,35,255,254,67,255,222,67,255,62,35,255,94,43,255,94,43,255,
    94,43,255,94,43,255,62,35,255,190,59,255,254,75,255,62,35,255,61,35,255,61,35,255,
    159,43,255,52,26,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,5,0,6,0,0,190,0,0,255,18,26,250,
    127,43,254,94,43,255,61,43,255,29,35,255,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,29,35,255,61,43,255,94,43,255,127,43,254,
    18,26,250,0,0,255,0,0,212,1,0,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,9,0,0,164,0,0,255,
    75,9,255,27,35,251,159,43,254,127,43,255,94,43,255,61,35,255,61,35,255,61,35,255,
    61,35,255,61,35,255,61,35,255,94,43,255,127,43,255,159,43,254,27,35,252,75,9,254,
    0,0,255,0,0,185,1,0,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,34,0,255,142,17,255,217,34,252,94,43,252,127,43,253,127,43,254,127,43,254,
    127,43,254,127,43,254,127,43,253,94,43,252,217,34,251,142,17,255,34,0,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    34,0,35,0,0,159,0,0,242,0,0,255,165,8,255,76,17,255,208,17,255,18,26,255,
    18,26,255,208,17,255,76,17,255,165,8,255,0,0,255,0,0,248,0,0,172,33,0,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,131,8,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,98,0,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,165,8,24,99,0,42,
    99,0,42,132,8,27,82,34,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 6 应急车道
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,64,57,18,
    64,57,18,96,147,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,96,98,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,96,57,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    224,40,16,0,0,128,0,0,223,0,0,255,32,41,255,96,98,255,32,131,255,160,155,255,
    160,155,255,32,131,255,96,98,255,32,41,255,0,0,255,0,0,230,0,0,142,128,24,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,8,85,
    0,0,224,96,16,255,192,114,255,0,213,253,0,254,251,96,254,252,96,254,253,64,254,254,
    64,254,254,96,254,253,96,254,252,0,254,251,0,213,252,192,114,255,96,16,255,0,0,235,
    32,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    64,90,255,128,229,251,96,254,253,32,254,255,224,253,255,192,245,255,192,245,255,192,245,255,
    192,245,255,192,245,255,192,245,255,224,253,255,32,254,255,96,254,254,128,229,251,64,90,255,
    0,0,255,0,0,159,64,41,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,160,155,251,
    96,254,253,0,254,255,192,245,255,224,253,255,224,253,255,224,253,255,224,253,255,32,254,255,
    64,254,255,224,253,255,224,253,255,224,253,255,224,253,255,192,245,255,0,254,255,64,254,254,
    160,155,251,0,0,255,0,0,189,224,24,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,224,163,250,96,254,255,
    192,245,255,224,253,255,224,253,255,224,253,255,224,253,255,224,253,255,32,254,255,32,180,255,
    192,163,255,32,254,255,224,253,255,224,253,255,224,253,255,224,253,255,224,253,255,192,245,255,
    96,254,255,224,163,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,64,8,81,0,0,255,160,155,250,96,254,254,192,245,255,
    224,253,255,224,253,255,224,253,255,224,253,255,224,253,255,0,254,255,96,229,255,0,0,255,
    0,0,255,192,204,255,32,254,255,224,245,255,224,253,255,224,253,255,224,253,255,224,253,255,
    192,245,255,96,254,255,160,155,250,0,0,255,32,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,49,14,0,0,230,32,90,254,96,254,253,192,245,255,224,253,255,
    224,253,255,224,253,255,224,253,255,224,253,255,192,245,255,96,254,255,96,98,255,0,0,255,
    0,0,255,192,73,255,64,254,255,224,253,255,224,253,255,224,253,255,224,253,255,224,253,255,
    224,253,255,192,245,255,96,254,254,32,90,252,0,0,247,128,24,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,96,16,255,128,229,251,0,254,255,224,253,255,224,253,255,
    224,253,255,224,253,255,224,253,255,224,253,255,32,254,255,224,212,255,32,0,255,64,8,255,
    96,16,255,0,0,255,96,188,255,64,254,255,192,245,255,224,253,255,224,253,255,224,253,255,
    224,253,255,224,253,255,0,254,255,128,229,251,96,16,255,0,0,160,0,0,0,0,0,0,
    0,0,0,160,114,9,0,0,228,160,114,254,96,254,253,192,245,255,224,253,255,224,253,255,
    224,253,255,224,253,255,224,253,255,192,245,255,64,254,255,224,81,255,0,0,255,128,147,255,
    32,172,255,0,0,255,64,57,255,0,254,255,224,253,255,224,253,255,224,253,255,224,253,255,
    224,253,255,224,253,255,192,245,255,96,254,254,160,114,254,0,0,246,224,40,24,0,0,0,
    0,0,0,0,0,70,0,0,255,0,213,253,32,254,255,224,253,255,224,253,255,224,253,255,
    224,253,255,224,253,255,192,245,255,64,254,255,96,188,255,0,0,255,160,24,255,128,147,255,
    224,163,255,0,41,255,0,0,255,0,172,255,64,254,255,192,245,255,224,253,255,224,253,255,
    224,253,255,224,253,255,224,253,255,32,254,255,0,213,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,0,41,254,32,254,251,224,253,255,224,253,255,224,253,255,224,253,255,
    224,253,255,224,253,255,224,253,255,32,254,255,64,49,255,0,0,255,160,114,255,32,0,255,
    0,0,255,64,139,255,0,0,255,224,32,255,224,245,255,224,253,255,224,253,255,224,253,255,
    224,253,255,224,253,255,224,253,255,224,253,255,32,254,251,0,41,254,0,0,173,0,0,0,
    0,0,0,0,0,192,64,98,255,96,254,252,192,245,255,224,253,255,224,253,255,224,253,255,
    224,253,255,192,245,255,64,254,255,224,171,255,0,0,255,0,41,255,192,237,255,192,32,255,
    0,0,255,32,221,255,128,57,255,0,0,255,96,147,255,96,254,255,192,245,255,224,253,255,
    224,253,255,224,253,255,224,253,255,192,245,255,96,254,253,64,98,255,0,0,217,0,0,0,
    0,0,0,0,0,219,32,139,255,96,254,253,192,245,255,224,253,255,224,253,255,224,253,255,
    224,253,255,224,253,255,224,245,255,224,32,255,0,0,255,0,172,255,64,254,255,192,32,255,
    0,0,255,96,229,255,160,196,255,0,0,255,160,24,255,160,237,255,0,254,255,224,253,255,
    224,253,255,224,253,255,224,253,255,192,245,255,96,254,254,32,139,255,0,0,239,224,40,25,
    64,57,18,0,0,234,160,155,255,64,254,254,192,245,255,224,253,255,224,253,255,224,253,255,
    192,245,255,96,254,255,96,139,255,0,0,255,96,57,255,32,254,255,224,253,255,224,32,255,
    0,0,255,0,213,255,96,254,255,192,73,255,0,0,255,0,131,255,96,254,255,192,245,255,
    224,253,255,224,253,255,224,253,255,192,245,255,64,254,254,160,155,255,0,0,250,128,16,43,
    64,49,19,0,0,235,160,155,255,64,254,254,192,245,255,224,253,255,224,253,255,224,253,255,
    0,254,255,128,237,255,128,16,255,0,0,255,128,196,255,64,254,255,160,245,255,192,32,255,
    0,0,255,224,212,255,96,254,255,192,204,255,0,0,255,96,16,255,96,229,255,0,254,255,
    224,253,255,224,253,255,224,253,255,192,245,255,64,254,254,160,155,255,0,0,251,128,16,43,
    96,147,7,0,0,221,32,139,255,96,254,253,192,245,255,224,253,255,224,253,255,192,245,255,
    96,254,255,160,114,255,0,0,255,224,81,255,64,254,255,0,254,255,32,221,255,0,0,255,
    0,0,255,32,221,255,0,254,255,64,254,255,32,90,255,0,0,255,128,106,255,64,254,255,
    192,245,255,224,253,255,224,253,255,192,245,255,96,254,254,32,139,255,0,0,241,192,32,28,
    0,0,0,0,0,196,64,98,255,96,254,252,192,245,255,224,253,255,224,253,255,32,254,255,
    32,221,255,64,8,255,32,0,255,0,213,255,32,254,255,192,245,255,64,254,255,160,155,255,
    160,155,255,64,254,255,192,245,255,32,254,255,32,221,255,64,8,255,32,0,255,0,213,255,
    32,254,255,224,253,255,224,253,255,192,245,255,96,254,253,64,98,255,0,0,220,0,0,0,
    0,0,0,0,0,151,0,41,254,32,254,251,224,253,255,224,253,255,192,245,255,64,254,255,
    32,90,255,0,0,255,96,106,255,64,254,255,160,245,255,224,253,255,64,221,255,64,49,255,
    64,49,255,64,221,255,224,253,255,160,245,255,64,254,255,128,106,255,0,0,255,0,82,255,
    64,254,255,192,245,255,224,253,255,224,253,255,0,254,252,0,41,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,0,213,252,32,254,255,192,245,255,64,254,255,192,204,255,
    0,0,255,160,24,255,224,245,255,128,254,255,64,254,255,160,254,255,64,221,255,0,0,255,
    0,0,255,64,221,255,160,254,255,64,254,255,96,254,255,0,254,255,192,32,255,0,0,255,
    160,196,255,64,254,255,192,245,255,32,254,255,0,213,251,0,0,255,0,0,111,0,0,0,
    0,0,0,160,73,14,0,0,236,160,114,254,96,254,253,192,245,255,0,254,255,160,65,255,
    0,0,255,64,57,255,64,131,255,0,123,255,0,131,255,0,131,255,64,131,255,128,65,255,
    128,65,255,64,131,255,0,131,255,0,131,255,0,123,255,64,131,255,64,57,255,0,0,255,
    128,65,255,0,254,255,192,245,255,96,254,254,160,114,254,0,0,250,160,24,32,0,0,0,
    0,0,0,0,0,0,0,0,143,96,16,255,96,229,251,64,254,255,128,196,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,128,196,255,64,254,255,96,229,251,96,16,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,160,32,23,0,0,239,32,90,253,96,254,253,160,237,255,224,122,255,
    192,114,255,0,131,255,0,131,255,0,131,255,0,131,255,0,131,255,0,131,255,0,123,255,
    0,123,255,0,131,255,0,131,255,0,131,255,0,131,255,0,131,255,0,131,255,192,114,255,
    224,122,255,160,237,255,96,254,254,32,90,252,0,0,253,64,16,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,102,0,0,255,160,155,250,128,254,255,32,254,255,
    96,254,255,96,254,255,96,254,255,96,254,255,96,254,255,96,254,255,96,254,255,96,254,255,
    96,254,255,96,254,255,96,254,255,96,254,255,96,254,255,96,254,255,96,254,255,96,254,255,
    32,254,255,128,254,255,160,155,250,0,0,255,32,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,224,163,250,96,254,255,
    192,245,255,192,245,255,192,245,255,192,245,255,192,245,255,192,245,255,192,245,255,192,245,255,
    192,245,255,192,245,255,192,245,255,192,245,255,192,245,255,192,245,255,192,245,255,192,245,255,
    96,254,255,224,163,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,64,41,6,0,0,190,0,0,255,160,155,250,
    64,254,254,0,254,255,192,245,255,224,253,255,224,253,255,224,253,255,224,253,255,224,253,255,
    224,253,255,224,253,255,224,253,255,224,253,255,224,253,255,192,245,255,0,254,255,64,254,254,
    160,155,250,0,0,255,0,0,212,96,8,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,224,24,9,0,0,164,0,0,255,
    64,90,255,128,229,251,96,254,254,32,254,255,224,253,255,192,245,255,192,245,255,192,245,255,
    192,245,255,192,245,255,192,245,255,224,253,255,32,254,255,96,254,254,128,229,252,64,90,254,
    0,0,255,0,0,185,96,8,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,113,
    0,0,242,96,16,255,192,114,255,0,213,252,0,254,252,96,254,253,96,254,254,64,254,254,
    64,254,254,96,254,254,96,254,253,0,254,252,0,213,251,192,114,255,96,16,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    96,16,35,0,0,159,0,0,242,0,0,255,32,41,255,96,98,255,32,131,255,160,155,255,
    160,155,255,32,131,255,96,98,255,32,41,255,0,0,255,0,0,248,0,0,172,64,8,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,192,32,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,160,24,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,224,40,24,128,24,42,
    128,24,42,224,32,27,96,147,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 7 非机动车道
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,231,8,18,
    231,8,18,82,34,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,137,25,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,229,8,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    101,0,16,0,0,128,0,0,223,0,0,255,165,8,255,76,17,255,208,17,255,18,26,255,
    18,26,255,208,17,255,76,17,255,165,8,255,0,0,255,0,0,230,0,0,142,67,0,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,85,
    0,0,224,34,0,255,142,17,255,185,34,253,94,43,251,159,43,252,127,43,253,127,43,254,
    127,43,254,127,43,253,159,43,252,94,43,251,217,34,252,142,17,255,34,0,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    75,9,255,27,35,251,159,43,253,127,43,255,94,43,255,61,35,255,61,35,255,61,35,255,
    61,35,255,61,35,255,61,35,255,94,43,255,127,43,255,159,43,254,27,35,251,75,9,255,
    0,0,255,0,0,159,5,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,18,26,251,
    127,43,253,94,43,255,61,35,255,61,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,61,43,255,61,35,255,94,43,255,127,43,254,
    18,26,251,0,0,255,0,0,189,3,0,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,52,26,250,159,43,255,
    61,35,255,61,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,61,43,255,61,35,255,
    159,43,255,52,26,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,33,0,81,0,0,255,18,26,250,159,43,254,61,35,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    61,35,255,159,43,255,18,26,250,0,0,255,32,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,134,16,14,0,0,230,42,9,254,159,43,253,61,35,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,62,35,255,253,26,255,29,27,255,253,26,255,62,35,255,94,43,255,
    94,43,255,61,35,255,159,43,254,43,9,252,0,0,247,67,8,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,34,0,255,27,35,251,94,43,255,61,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,62,35,255,29,35,255,29,27,255,253,18,255,29,35,255,
    61,35,255,62,35,255,126,51,255,222,108,255,126,92,255,190,108,255,158,51,255,62,35,255,
    94,43,255,61,43,255,94,43,255,27,35,251,34,0,255,0,0,160,0,0,0,0,0,0,
    0,0,0,206,25,9,0,0,228,141,17,254,159,43,253,61,35,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,29,35,255,63,174,255,255,206,255,95,182,255,63,223,255,
    222,116,255,221,18,255,254,116,255,255,255,255,255,255,255,255,255,255,190,108,255,253,26,255,
    94,43,255,94,43,255,61,35,255,159,43,254,141,17,254,0,0,246,165,8,24,0,0,0,
    0,0,0,0,0,70,0,0,255,185,34,253,127,43,255,61,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,29,35,255,63,174,255,95,231,255,63,223,255,31,215,255,
    94,92,255,125,2,255,190,59,255,255,255,255,95,182,255,30,76,255,126,43,255,62,35,255,
    94,43,255,94,43,255,61,43,255,127,43,255,217,34,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,133,0,254,94,43,251,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,221,18,255,94,92,255,223,157,255,158,149,255,
    158,149,255,191,157,255,191,149,255,223,255,255,223,206,255,221,18,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,251,133,0,254,0,0,173,0,0,0,
    0,0,0,0,0,192,75,17,255,159,43,252,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,127,231,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,254,75,255,62,35,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,127,43,253,75,17,255,0,0,217,0,0,0,
    0,0,0,0,0,219,208,17,255,159,43,253,61,35,255,94,43,255,94,43,255,94,43,255,
    62,35,255,253,26,255,253,26,255,189,10,255,158,149,255,255,255,255,255,255,255,126,141,255,
    94,43,255,62,35,255,31,166,255,255,255,255,255,255,255,158,100,255,189,10,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,159,43,254,208,17,255,0,0,239,165,8,25,
    231,8,18,0,0,234,18,26,255,127,43,254,61,35,255,94,43,255,94,43,255,29,27,255,
    158,51,255,30,125,255,191,149,255,62,125,255,223,255,255,31,166,255,63,223,255,31,215,255,
    189,18,255,62,84,255,255,255,255,95,182,255,191,247,255,223,206,255,222,116,255,158,51,255,
    29,27,255,94,43,255,94,43,255,61,35,255,127,43,254,18,26,255,0,0,250,66,0,43,
    198,8,19,0,0,235,18,26,255,127,43,254,61,35,255,94,43,255,29,27,255,94,92,255,
    191,239,255,255,255,255,255,255,255,255,255,255,255,255,255,254,75,255,159,149,255,255,255,255,
    190,59,255,255,206,255,255,255,255,127,231,255,255,255,255,255,255,255,255,255,255,191,247,255,
    94,92,255,29,27,255,94,43,255,61,35,255,127,43,254,18,26,255,0,0,251,66,0,43,
    82,34,7,0,0,221,208,17,255,159,43,253,61,43,255,61,35,255,190,59,255,191,247,255,
    63,223,255,222,67,255,190,108,255,223,255,255,159,239,255,31,215,255,126,92,255,223,255,255,
    255,214,255,255,255,255,255,255,255,95,223,255,223,206,255,159,239,255,94,84,255,31,223,255,
    191,247,255,190,59,255,61,35,255,61,43,255,159,43,254,208,17,255,0,0,241,132,8,28,
    0,0,0,0,0,196,75,17,255,159,43,252,93,43,255,253,26,255,159,149,255,255,255,255,
    254,67,255,221,18,255,63,223,255,127,231,255,191,149,255,255,255,255,191,149,255,95,231,255,
    255,255,255,191,247,255,255,255,255,62,84,255,190,108,255,255,255,255,190,59,255,190,59,255,
    255,255,255,159,149,255,253,26,255,93,43,255,127,43,253,75,17,255,0,0,220,0,0,0,
    0,0,0,0,0,151,133,0,254,94,43,251,94,43,255,253,26,255,127,190,255,191,239,255,
    221,18,255,222,116,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    159,239,255,95,182,255,255,255,255,62,35,255,94,92,255,255,255,255,254,116,255,221,18,255,
    191,239,255,127,190,255,253,26,255,94,43,255,94,43,252,133,0,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,217,34,252,127,43,255,221,18,255,191,157,255,255,255,255,
    158,59,255,62,35,255,222,116,255,30,76,255,94,133,255,255,255,255,159,149,255,126,92,255,
    30,76,255,254,116,255,255,255,255,62,84,255,253,26,255,222,108,255,62,35,255,158,59,255,
    255,255,255,191,157,255,221,18,255,127,43,255,217,34,251,0,0,255,0,0,111,0,0,0,
    0,0,0,41,17,14,0,0,236,141,17,254,159,43,253,29,35,255,254,67,255,255,255,255,
    191,198,255,158,51,255,221,18,255,190,59,255,255,206,255,191,247,255,126,43,255,253,26,255,
    29,27,255,158,51,255,191,247,255,63,223,255,222,67,255,253,18,255,158,51,255,191,198,255,
    255,255,255,254,67,255,29,35,255,159,43,254,141,17,254,0,0,250,99,0,32,0,0,0,
    0,0,0,0,0,0,0,0,143,34,0,255,27,35,251,126,43,255,253,26,255,222,108,255,
    255,255,255,255,255,255,159,239,255,255,255,255,223,247,255,94,92,255,29,27,255,126,43,255,
    94,43,255,29,27,255,94,92,255,223,247,255,255,255,255,159,239,255,255,255,255,255,255,255,
    222,108,255,253,26,255,126,43,255,27,35,251,34,0,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,68,8,23,0,0,239,42,9,253,127,43,253,93,43,255,29,27,255,
    254,67,255,191,149,255,95,182,255,126,141,255,190,59,255,29,27,255,94,43,255,94,43,255,
    94,43,255,94,43,255,29,27,255,190,59,255,126,141,255,95,182,255,191,149,255,254,67,255,
    29,27,255,93,43,255,127,43,254,43,9,252,0,0,253,34,0,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,102,0,0,255,18,26,250,159,43,255,61,43,255,
    29,35,255,253,26,255,253,26,255,253,26,255,62,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,62,35,255,253,26,255,253,26,255,253,26,255,29,35,255,
    61,43,255,159,43,255,18,26,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,52,26,250,159,43,255,
    61,43,255,93,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,93,43,255,61,43,255,
    159,43,255,52,26,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,5,0,6,0,0,190,0,0,255,18,26,250,
    127,43,254,94,43,255,61,35,255,61,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,61,43,255,61,35,255,94,43,255,127,43,254,
    18,26,250,0,0,255,0,0,212,1,0,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,9,0,0,164,0,0,255,
    75,9,255,27,35,251,159,43,254,127,43,255,94,43,255,61,35,255,61,35,255,61,35,255,
    61,35,255,61,35,255,61,35,255,94,43,255,127,43,255,159,43,254,27,35,252,75,9,254,
    0,0,255,0,0,185,1,0,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,34,0,255,142,17,255,217,34,252,94,43,252,127,43,253,127,43,254,127,43,254,
    127,43,254,127,43,254,127,43,253,94,43,252,217,34,251,142,17,255,34,0,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    34,0,35,0,0,159,0,0,242,0,0,255,165,8,255,76,17,255,208,17,255,18,26,255,
    18,26,255,208,17,255,76,17,255,165,8,255,0,0,255,0,0,248,0,0,172,33,0,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,131,8,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,98,0,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,165,8,24,99,0,42,
    99,0,42,132,8,27,82,34,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 8 区间起点
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,103,24,18,
    103,24,18,50,73,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,201,48,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,101,24,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    101,24,16,0,0,128,0,0,223,0,0,255,101,16,255,203,48,255,15,65,255,82,73,255,
    82,73,255,15,65,255,203,48,255,101,16,255,0,0,255,0,0,230,0,0,142,67,16,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,85,
    0,0,224,34,8,255,237,56,255,184,105,253,29,130,251,62,130,252,62,130,253,62,130,254,
    62,130,254,62,130,253,62,130,252,29,130,251,184,105,252,237,56,255,34,8,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    202,40,255,218,113,251,62,130,253,29,130,255,28,122,255,252,121,255,252,121,255,252,121,255,
    252,121,255,252,121,255,252,121,255,28,122,255,29,130,255,62,130,254,218,113,251,202,40,255,
    0,0,255,0,0,159,5,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,49,73,251,
    62,130,253,29,122,255,252,121,255,252,121,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,252,121,255,252,121,255,29,122,255,62,130,254,
    49,73,251,0,0,255,0,0,189,3,0,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,83,81,250,63,130,255,
    252,121,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,252,121,255,
    63,130,255,83,81,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,1,0,81,0,0,255,49,73,250,63,130,254,251,121,255,
    252,121,255,220,121,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    251,121,255,63,130,255,49,73,250,0,0,255,0,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,134,32,14,0,0,230,170,40,254,62,130,253,28,122,255,252,121,255,
    60,130,255,188,138,255,220,121,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,252,121,255,62,130,254,170,40,252,0,0,247,67,16,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,33,8,255,218,113,251,29,122,255,252,121,255,28,122,255,
    191,230,255,191,255,255,93,155,255,188,113,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,252,121,255,29,122,255,218,113,251,33,8,255,0,0,160,0,0,0,0,0,0,
    0,0,0,234,56,9,0,0,228,237,56,254,62,130,253,252,121,255,252,121,255,60,130,255,
    95,247,255,255,255,255,253,163,255,188,113,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,252,121,255,252,121,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,252,121,255,62,130,254,237,56,254,0,0,246,67,16,24,0,0,0,
    0,0,0,0,0,70,0,0,255,183,105,253,29,130,255,28,122,255,28,122,255,252,121,255,
    255,238,255,255,255,255,125,155,255,188,113,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,220,121,255,253,146,255,220,138,255,188,113,255,60,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,252,121,255,29,130,255,184,105,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,101,16,254,29,130,251,28,122,255,28,122,255,28,122,255,28,122,255,
    31,239,255,255,255,255,157,155,255,188,113,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,188,113,255,221,163,255,95,247,255,221,138,255,188,113,255,60,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,29,130,251,101,16,254,0,0,173,0,0,0,
    0,0,0,0,0,192,203,48,255,62,130,252,252,121,255,28,122,255,28,122,255,28,122,255,
    31,239,255,255,255,255,157,155,255,188,113,255,252,121,255,252,121,255,252,121,255,252,121,255,
    252,121,255,156,113,255,125,155,255,255,255,255,127,247,255,29,147,255,188,113,255,60,122,255,
    28,122,255,28,122,255,28,122,255,252,121,255,62,130,253,203,48,255,0,0,217,0,0,0,
    0,0,0,0,0,219,15,65,255,62,130,253,252,121,255,28,122,255,28,122,255,28,122,255,
    31,239,255,255,255,255,125,155,255,220,121,255,188,138,255,92,130,255,124,130,255,124,130,255,
    124,130,255,28,122,255,221,163,255,255,255,255,255,255,255,223,255,255,125,155,255,124,113,255,
    28,122,255,28,122,255,28,122,255,252,121,255,62,130,254,15,65,255,0,0,239,69,16,25,
    103,24,18,0,0,234,81,73,255,62,130,254,252,121,255,28,122,255,28,122,255,28,122,255,
    31,239,255,255,255,255,157,155,255,127,222,255,223,255,255,159,247,255,159,247,255,159,247,255,
    159,247,255,159,247,255,191,255,255,255,255,255,255,255,255,255,255,255,223,255,255,61,172,255,
    220,113,255,28,122,255,28,122,255,252,121,255,62,130,254,81,73,255,0,0,250,34,8,43,
    102,24,19,0,0,235,81,73,255,62,130,254,252,121,255,28,122,255,28,122,255,28,122,255,
    31,239,255,255,255,255,157,155,255,127,222,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,190,180,255,
    220,113,255,28,122,255,28,122,255,252,121,255,62,130,254,81,73,255,0,0,251,34,8,43,
    50,73,7,0,0,221,15,65,255,62,130,253,252,121,255,28,122,255,28,122,255,28,122,255,
    31,239,255,255,255,255,125,155,255,220,121,255,253,146,255,253,138,255,253,146,255,253,146,255,
    253,146,255,188,138,255,61,172,255,255,255,255,255,255,255,223,255,255,253,163,255,124,113,255,
    28,122,255,28,122,255,28,122,255,252,121,255,62,130,254,15,65,255,0,0,241,68,16,28,
    0,0,0,0,0,196,203,48,255,62,130,252,252,121,255,28,122,255,28,122,255,28,122,255,
    31,239,255,255,255,255,157,155,255,188,113,255,220,121,255,220,113,255,220,113,255,220,113,255,
    220,121,255,124,113,255,93,155,255,255,255,255,159,247,255,125,155,255,156,113,255,60,122,255,
    28,122,255,28,122,255,28,122,255,252,121,255,62,130,253,203,48,255,0,0,220,0,0,0,
    0,0,0,0,0,151,101,16,254,29,130,251,28,122,255,28,122,255,28,122,255,28,122,255,
    31,239,255,255,255,255,157,155,255,188,113,255,60,122,255,28,122,255,28,122,255,28,122,255,
    60,122,255,188,113,255,221,163,255,127,247,255,253,138,255,188,113,255,60,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,29,122,252,101,16,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,184,105,252,29,130,255,28,122,255,28,122,255,252,121,255,
    255,238,255,255,255,255,125,155,255,188,113,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,220,121,255,253,146,255,221,138,255,188,113,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,252,121,255,29,130,255,184,105,251,0,0,255,0,0,111,0,0,0,
    0,0,0,134,32,14,0,0,236,237,56,254,62,130,253,252,121,255,252,121,255,60,130,255,
    95,247,255,255,255,255,253,163,255,188,113,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,252,121,255,220,121,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,252,121,255,62,130,254,237,56,254,0,0,250,34,8,32,0,0,0,
    0,0,0,0,0,0,0,0,143,33,8,255,218,113,251,29,122,255,252,121,255,28,122,255,
    191,230,255,191,255,255,93,155,255,188,113,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,252,121,255,29,122,255,218,113,251,33,8,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,68,16,23,0,0,239,170,40,253,62,130,253,28,122,255,252,121,255,
    60,130,255,188,138,255,220,121,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,252,121,255,62,130,254,170,40,252,0,0,253,34,8,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,102,0,0,255,49,73,250,63,130,255,251,121,255,
    252,121,255,220,121,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    251,121,255,63,130,255,49,73,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,83,81,250,63,130,255,
    252,121,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,252,121,255,
    63,130,255,83,81,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,5,0,6,0,0,190,0,0,255,49,73,250,
    62,130,254,29,122,255,252,121,255,252,121,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,252,121,255,252,121,255,29,122,255,62,130,254,
    49,73,250,0,0,255,0,0,212,1,0,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,9,0,0,164,0,0,255,
    202,40,255,218,113,251,62,130,254,29,130,255,28,122,255,252,121,255,252,121,255,252,121,255,
    252,121,255,252,121,255,252,121,255,28,122,255,29,130,255,62,130,254,218,113,252,202,40,254,
    0,0,255,0,0,185,1,0,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,34,8,255,237,56,255,184,105,252,29,122,252,62,130,253,62,130,254,62,130,254,
    62,130,254,62,130,254,62,130,253,29,122,252,184,105,251,237,56,255,34,8,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    34,8,35,0,0,159,0,0,242,0,0,255,101,16,255,203,48,255,15,65,255,82,73,255,
    82,73,255,15,65,255,203,48,255,101,16,255,0,0,255,0,0,248,0,0,172,33,8,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,67,16,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,34,8,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,69,16,24,35,8,42,
    35,8,42,68,16,27,50,73,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 9 区间终点
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,103,24,18,
    103,24,18,50,73,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,201,48,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,101,24,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    101,24,16,0,0,128,0,0,223,0,0,255,101,16,255,203,48,255,15,65,255,82,73,255,
    82,73,255,15,65,255,203,48,255,101,16,255,0,0,255,0,0,230,0,0,142,67,16,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,85,
    0,0,224,34,8,255,237,56,255,184,105,253,29,130,251,62,130,252,62,130,253,62,130,254,
    62,130,254,62,130,253,62,130,252,29,130,251,184,105,252,237,56,255,34,8,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    202,40,255,218,113,251,62,130,253,29,130,255,28,122,255,252,121,255,252,121,255,252,121,255,
    252,121,255,252,121,255,252,121,255,28,122,255,29,130,255,62,130,254,218,113,251,202,40,255,
    0,0,255,0,0,159,5,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,49,73,251,
    62,130,253,29,122,255,252,121,255,252,121,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,252,121,255,252,121,255,29,122,255,62,130,254,
    49,73,251,0,0,255,0,0,189,3,0,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,83,81,250,63,130,255,
    252,121,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,252,121,255,
    63,130,255,83,81,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,1,0,81,0,0,255,49,73,250,63,130,254,251,121,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,220,121,255,252,121,255,
    251,121,255,63,130,255,49,73,250,0,0,255,0,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,134,32,14,0,0,230,170,40,254,62,130,253,252,121,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,220,121,255,188,138,255,60,130,255,
    252,121,255,28,122,255,62,130,254,170,40,252,0,0,247,67,16,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,33,8,255,218,113,251,29,122,255,252,121,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,188,113,255,93,155,255,191,255,255,191,230,255,
    28,122,255,252,121,255,29,122,255,218,113,251,33,8,255,0,0,160,0,0,0,0,0,0,
    0,0,0,234,56,9,0,0,228,237,56,254,62,130,253,252,121,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,252,121,255,252,121,255,
    28,122,255,28,122,255,28,122,255,28,122,255,188,113,255,253,163,255,255,255,255,95,247,255,
    60,130,255,252,121,255,252,121,255,62,130,254,237,56,254,0,0,246,67,16,24,0,0,0,
    0,0,0,0,0,70,0,0,255,183,105,253,29,130,255,252,121,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,220,121,255,253,146,255,220,138,255,
    188,113,255,60,122,255,28,122,255,28,122,255,188,113,255,125,155,255,255,255,255,255,238,255,
    252,121,255,28,122,255,28,122,255,29,130,255,184,105,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,101,16,254,29,130,251,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,188,113,255,221,163,255,95,247,255,
    221,138,255,188,113,255,60,122,255,28,122,255,188,113,255,157,155,255,255,255,255,31,239,255,
    28,122,255,28,122,255,28,122,255,28,122,255,29,130,251,101,16,254,0,0,173,0,0,0,
    0,0,0,0,0,192,203,48,255,62,130,252,252,121,255,28,122,255,28,122,255,252,121,255,
    220,121,255,252,121,255,252,121,255,252,121,255,252,121,255,156,113,255,125,155,255,255,255,255,
    127,247,255,29,147,255,188,113,255,60,130,255,188,113,255,157,155,255,255,255,255,31,239,255,
    28,122,255,28,122,255,28,122,255,252,121,255,62,130,253,203,48,255,0,0,217,0,0,0,
    0,0,0,0,0,219,15,65,255,62,130,253,252,121,255,28,122,255,252,121,255,60,130,255,
    188,138,255,92,130,255,124,130,255,124,130,255,124,130,255,28,122,255,221,163,255,255,255,255,
    255,255,255,223,255,255,125,155,255,156,113,255,220,113,255,157,155,255,255,255,255,31,239,255,
    28,122,255,28,122,255,28,122,255,252,121,255,62,130,254,15,65,255,0,0,239,69,16,25,
    103,24,18,0,0,234,81,73,255,62,130,254,252,121,255,252,121,255,28,122,255,223,230,255,
    223,255,255,159,247,255,159,247,255,159,247,255,159,247,255,159,247,255,191,255,255,255,255,255,
    255,255,255,255,255,255,223,255,255,93,172,255,124,113,255,157,155,255,255,255,255,31,239,255,
    28,122,255,28,122,255,28,122,255,252,121,255,62,130,254,81,73,255,0,0,250,34,8,43,
    102,24,19,0,0,235,81,73,255,62,130,254,252,121,255,252,121,255,28,122,255,223,230,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,190,180,255,124,105,255,157,155,255,255,255,255,31,239,255,
    28,122,255,28,122,255,28,122,255,252,121,255,62,130,254,81,73,255,0,0,251,34,8,43,
    50,73,7,0,0,221,15,65,255,62,130,253,252,121,255,28,122,255,252,121,255,60,122,255,
    253,146,255,253,138,255,253,146,255,253,146,255,253,146,255,188,138,255,61,172,255,255,255,255,
    255,255,255,223,255,255,253,163,255,156,113,255,220,113,255,157,155,255,255,255,255,31,239,255,
    28,122,255,28,122,255,28,122,255,252,121,255,62,130,254,15,65,255,0,0,241,68,16,28,
    0,0,0,0,0,196,203,48,255,62,130,252,252,121,255,28,122,255,28,122,255,252,121,255,
    220,113,255,220,113,255,220,113,255,220,113,255,220,121,255,124,113,255,93,155,255,255,255,255,
    159,247,255,125,155,255,156,113,255,60,130,255,188,113,255,157,155,255,255,255,255,31,239,255,
    28,122,255,28,122,255,28,122,255,252,121,255,62,130,253,203,48,255,0,0,220,0,0,0,
    0,0,0,0,0,151,101,16,254,29,130,251,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,60,122,255,188,113,255,221,163,255,127,247,255,
    253,138,255,188,113,255,60,122,255,28,122,255,188,113,255,157,155,255,255,255,255,31,239,255,
    28,122,255,28,122,255,28,122,255,28,122,255,29,122,252,101,16,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,184,105,252,29,130,255,252,121,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,220,121,255,253,146,255,221,138,255,
    188,113,255,28,122,255,28,122,255,28,122,255,188,113,255,125,155,255,255,255,255,255,238,255,
    252,121,255,28,122,255,28,122,255,29,130,255,184,105,251,0,0,255,0,0,111,0,0,0,
    0,0,0,134,32,14,0,0,236,237,56,254,62,130,253,252,121,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,252,121,255,220,121,255,
    28,122,255,28,122,255,28,122,255,28,122,255,188,113,255,253,163,255,255,255,255,95,247,255,
    60,130,255,252,121,255,252,121,255,62,130,254,237,56,254,0,0,250,34,8,32,0,0,0,
    0,0,0,0,0,0,0,0,143,33,8,255,218,113,251,29,122,255,252,121,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,188,113,255,93,155,255,191,255,255,191,230,255,
    28,122,255,252,121,255,29,122,255,218,113,251,33,8,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,68,16,23,0,0,239,170,40,253,62,130,253,252,121,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,220,121,255,188,138,255,60,130,255,
    252,121,255,28,122,255,62,130,254,170,40,252,0,0,253,34,8,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,102,0,0,255,49,73,250,63,130,255,251,121,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,220,121,255,252,121,255,
    251,121,255,63,130,255,49,73,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,83,81,250,63,130,255,
    252,121,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,28,122,255,252,121,255,
    63,130,255,83,81,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,5,0,6,0,0,190,0,0,255,49,73,250,
    62,130,254,29,122,255,252,121,255,252,121,255,28,122,255,28,122,255,28,122,255,28,122,255,
    28,122,255,28,122,255,28,122,255,28,122,255,252,121,255,252,121,255,29,122,255,62,130,254,
    49,73,250,0,0,255,0,0,212,1,0,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,9,0,0,164,0,0,255,
    202,40,255,218,113,251,62,130,254,29,130,255,28,122,255,252,121,255,252,121,255,252,121,255,
    252,121,255,252,121,255,252,121,255,28,122,255,29,130,255,62,130,254,218,113,252,202,40,254,
    0,0,255,0,0,185,1,0,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,34,8,255,237,56,255,184,105,252,29,122,252,62,130,253,62,130,254,62,130,254,
    62,130,254,62,130,254,62,130,253,29,122,252,184,105,251,237,56,255,34,8,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    34,8,35,0,0,159,0,0,242,0,0,255,101,16,255,203,48,255,15,65,255,82,73,255,
    82,73,255,15,65,255,203,48,255,101,16,255,0,0,255,0,0,248,0,0,172,33,8,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,67,16,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,34,8,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,69,16,24,35,8,42,
    35,8,42,68,16,27,50,73,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 10 流动测速
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,224,56,18,
    224,56,18,64,146,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,128,73,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,224,40,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    96,40,16,0,0,128,0,0,223,0,0,255,160,40,255,129,97,255,1,130,255,97,146,255,
    97,146,255,1,130,255,129,97,255,160,40,255,0,0,255,0,0,230,0,0,142,64,24,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,8,85,
    0,0,224,64,16,255,193,113,255,34,203,253,194,243,251,2,252,252,2,252,253,2,252,254,
    2,252,254,2,252,253,2,252,252,194,243,251,34,203,252,193,113,255,64,16,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    97,89,255,98,219,251,2,252,253,226,251,255,194,243,255,162,235,255,162,235,255,162,235,255,
    162,235,255,162,235,255,162,235,255,194,243,255,226,251,255,2,252,254,98,219,251,97,89,255,
    0,0,255,0,0,159,0,40,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,65,146,251,
    2,252,253,194,243,255,162,235,255,162,235,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,162,235,255,162,235,255,194,243,255,2,252,254,
    65,146,251,0,0,255,0,0,189,0,24,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,129,162,250,2,252,255,
    162,235,255,162,235,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,162,235,255,162,235,255,
    2,252,255,129,162,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,8,81,0,0,255,65,146,250,2,252,254,162,235,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    162,235,255,2,252,255,65,146,250,0,0,255,32,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,49,14,0,0,230,64,81,254,2,252,253,162,235,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,162,243,255,128,235,255,96,235,255,129,235,255,
    129,235,255,96,235,255,128,235,255,162,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,162,235,255,2,252,254,64,89,252,0,0,247,128,24,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,64,16,255,98,219,251,194,243,255,162,235,255,194,243,255,
    194,243,255,194,243,255,194,243,255,128,235,255,195,243,255,234,244,255,51,254,255,182,254,255,
    183,254,255,51,254,255,234,244,255,195,243,255,128,235,255,194,243,255,194,243,255,194,243,255,
    194,243,255,162,235,255,194,243,255,98,219,251,64,16,255,0,0,160,0,0,0,0,0,0,
    0,0,0,192,113,9,0,0,228,161,105,254,2,252,253,162,235,255,194,243,255,194,243,255,
    194,243,255,194,243,255,128,235,255,11,245,255,91,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,91,255,255,11,245,255,128,235,255,194,243,255,194,243,255,
    194,243,255,194,243,255,162,235,255,2,252,254,161,105,254,0,0,246,160,40,24,0,0,0,
    0,0,0,0,0,70,0,0,255,34,203,253,226,251,255,162,235,255,194,243,255,194,243,255,
    194,243,255,129,235,255,241,253,255,255,255,255,255,255,255,123,255,255,18,254,255,77,245,255,
    76,245,255,18,254,255,91,255,255,255,255,255,255,255,255,241,253,255,129,235,255,194,243,255,
    194,243,255,194,243,255,162,235,255,226,251,255,34,203,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,160,40,254,194,243,251,194,243,255,194,243,255,194,243,255,195,243,255,
    96,235,255,175,245,255,255,255,255,255,255,255,208,245,255,195,243,255,96,235,255,96,235,255,
    129,235,255,32,235,255,201,244,255,223,255,255,255,255,255,255,255,255,176,245,255,96,235,255,
    195,243,255,194,243,255,194,243,255,194,243,255,194,243,251,160,40,254,0,0,173,0,0,0,
    0,0,0,0,0,192,97,89,255,2,252,252,162,235,255,194,243,255,194,243,255,129,235,255,
    135,244,255,255,255,255,255,255,255,77,245,255,64,235,255,194,243,255,195,243,255,227,243,255,
    194,243,255,227,243,255,58,255,255,255,255,255,248,254,255,222,255,255,255,255,255,135,244,255,
    129,235,255,194,243,255,194,243,255,162,235,255,2,252,253,97,89,255,0,0,217,0,0,0,
    0,0,0,0,0,219,1,130,255,2,252,253,162,235,255,194,243,255,194,243,255,128,235,255,
    149,254,255,255,255,255,117,254,255,96,235,255,195,243,255,194,243,255,194,243,255,96,235,255,
    96,235,255,150,254,255,255,255,255,182,254,255,97,235,255,117,254,255,255,255,255,149,254,255,
    128,235,255,194,243,255,194,243,255,162,235,255,2,252,254,1,130,255,0,0,239,160,40,25,
    224,56,18,0,0,234,65,146,255,2,252,254,162,235,255,194,243,255,162,243,255,4,244,255,
    190,255,255,255,255,255,136,244,255,129,235,255,195,243,255,162,243,255,195,243,255,77,245,255,
    149,254,255,255,255,255,58,255,255,227,243,255,96,235,255,168,244,255,255,255,255,190,255,255,
    4,244,255,162,243,255,194,243,255,162,235,255,2,252,254,65,146,255,0,0,250,64,16,43,
    192,48,19,0,0,235,65,146,255,2,252,254,162,235,255,194,243,255,129,235,255,169,244,255,
    255,255,255,123,255,255,195,243,255,194,243,255,194,243,255,129,235,255,116,254,255,255,255,255,
    255,255,255,222,255,255,135,244,255,129,235,255,194,243,255,195,243,255,123,255,255,255,255,255,
    169,244,255,129,235,255,194,243,255,162,235,255,2,252,254,65,146,255,0,0,251,64,16,43,
    64,146,7,0,0,221,1,130,255,2,252,253,162,235,255,194,243,255,128,235,255,202,244,255,
    255,255,255,25,255,255,162,243,255,194,243,255,162,243,255,4,244,255,222,255,255,223,255,255,
    223,255,255,255,255,255,70,244,255,161,235,255,194,243,255,162,243,255,25,255,255,255,255,255,
    202,244,255,128,235,255,194,243,255,162,235,255,2,252,254,1,130,255,0,0,241,128,32,28,
    0,0,0,0,0,196,97,89,255,2,252,252,162,235,255,194,243,255,129,235,255,169,244,255,
    255,255,255,124,255,255,195,243,255,194,243,255,194,243,255,161,235,255,183,254,255,255,255,255,
    255,255,255,58,255,255,194,243,255,194,243,255,194,243,255,195,243,255,124,255,255,255,255,255,
    169,244,255,129,235,255,194,243,255,162,235,255,2,252,253,97,89,255,0,0,220,0,0,0,
    0,0,0,0,0,151,160,40,254,194,243,251,194,243,255,194,243,255,162,243,255,4,244,255,
    190,255,255,255,255,255,136,244,255,128,235,255,195,243,255,161,243,255,228,243,255,241,253,255,
    18,254,255,37,244,255,161,235,255,195,243,255,128,235,255,136,244,255,255,255,255,190,255,255,
    4,244,255,162,243,255,194,243,255,194,243,255,194,243,252,160,40,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,34,203,252,226,251,255,162,235,255,194,243,255,128,235,255,
    149,254,255,255,255,255,150,254,255,129,235,255,194,243,255,194,243,255,162,243,255,96,235,255,
    96,235,255,161,243,255,194,243,255,194,243,255,129,235,255,150,254,255,255,255,255,149,254,255,
    128,235,255,194,243,255,162,235,255,226,251,255,34,203,251,0,0,255,0,0,111,0,0,0,
    0,0,0,32,73,14,0,0,236,161,105,254,2,252,253,162,235,255,194,243,255,129,235,255,
    103,244,255,190,255,255,183,254,255,129,235,255,194,243,255,194,243,255,194,243,255,195,243,255,
    195,243,255,194,243,255,194,243,255,194,243,255,129,235,255,183,254,255,190,255,255,103,244,255,
    129,235,255,194,243,255,162,235,255,2,252,254,161,105,254,0,0,250,96,24,32,0,0,0,
    0,0,0,0,0,0,0,0,143,64,16,255,98,219,251,194,243,255,162,235,255,194,243,255,
    161,243,255,37,244,255,195,243,255,162,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,162,243,255,195,243,255,37,244,255,161,243,255,
    194,243,255,162,235,255,194,243,255,98,219,251,64,16,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,160,32,23,0,0,239,64,81,253,2,252,253,162,235,255,194,243,255,
    194,243,255,161,243,255,162,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,162,243,255,161,243,255,194,243,255,
    194,243,255,162,235,255,2,252,254,64,89,252,0,0,253,64,16,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,102,0,0,255,65,146,250,2,252,255,162,235,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    162,235,255,2,252,255,65,146,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,129,162,250,2,252,255,
    162,235,255,162,235,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,194,243,255,162,235,255,162,235,255,
    2,252,255,129,162,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,40,6,0,0,190,0,0,255,65,146,250,
    2,252,254,194,243,255,162,235,255,162,235,255,194,243,255,194,243,255,194,243,255,194,243,255,
    194,243,255,194,243,255,194,243,255,194,243,255,162,235,255,162,235,255,194,243,255,2,252,254,
    65,146,250,0,0,255,0,0,212,0,8,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,24,9,0,0,164,0,0,255,
    97,89,255,98,219,251,2,252,254,226,251,255,194,243,255,162,235,255,162,235,255,162,235,255,
    162,235,255,162,235,255,162,235,255,194,243,255,226,251,255,2,252,254,98,219,252,97,89,254,
    0,0,255,0,0,185,0,8,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,64,16,255,193,113,255,34,203,252,194,243,252,2,252,253,2,252,254,2,252,254,
    2,252,254,2,252,254,2,252,253,194,243,252,34,203,251,193,113,255,64,16,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,16,35,0,0,159,0,0,242,0,0,255,160,40,255,129,97,255,1,130,255,97,146,255,
    97,146,255,1,130,255,129,97,255,160,40,255,0,0,255,0,0,248,0,0,172,32,8,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,128,24,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,96,16,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,160,40,24,96,24,42,
    96,24,42,128,32,27,64,146,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 11 ETC
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,65,1,18,
    65,1,18,100,3,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,131,1,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,225,0,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    225,0,16,0,0,128,0,0,223,0,0,255,225,0,255,4,10,255,165,10,255,38,11,255,
    38,11,255,165,10,255,4,10,255,225,0,255,0,0,255,0,0,230,0,0,142,129,0,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,85,
    0,0,224,64,0,255,100,10,255,40,20,253,42,21,251,106,21,252,106,21,253,74,21,254,
    74,21,254,106,21,253,106,21,252,42,21,251,40,20,252,100,10,255,64,0,255,0,0,235,
    32,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    227,9,255,169,20,251,106,21,253,42,21,255,10,21,255,233,20,255,233,20,255,233,20,255,
    233,20,255,233,20,255,233,20,255,10,21,255,42,21,255,106,21,254,169,20,251,227,9,255,
    0,0,255,0,0,159,64,1,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,6,11,251,
    106,21,253,42,21,255,233,20,255,233,20,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,233,20,255,233,20,255,42,21,255,74,21,254,
    6,11,251,0,0,255,0,0,189,224,0,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,70,11,250,106,21,255,
    233,20,255,233,20,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,233,20,255,233,20,255,
    106,21,255,70,11,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,81,0,0,255,6,11,250,106,21,254,233,20,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    233,20,255,106,21,255,6,11,250,0,0,255,32,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,34,1,14,0,0,230,195,1,254,106,21,253,233,20,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,233,20,255,106,21,254,195,1,252,0,0,247,129,0,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,64,0,255,169,20,251,42,21,255,233,20,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,233,20,255,42,21,255,169,20,251,64,0,255,0,0,160,0,0,0,0,0,0,
    0,0,0,195,1,9,0,0,228,68,10,254,106,21,253,233,20,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,233,20,255,106,21,254,68,10,254,0,0,246,161,0,24,0,0,0,
    0,0,0,0,0,70,0,0,255,40,20,253,42,21,255,233,20,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,233,20,255,233,20,255,10,21,255,
    10,21,255,10,21,255,233,20,255,42,21,255,72,20,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,225,0,254,42,21,251,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,233,20,255,233,20,255,233,20,255,9,21,255,233,20,255,233,20,255,10,21,255,
    9,21,255,233,20,255,233,20,255,10,21,255,200,4,255,10,21,255,43,37,255,233,20,255,
    233,12,255,10,21,255,10,21,255,10,21,255,42,21,251,225,0,254,0,0,173,0,0,0,
    0,0,0,0,0,192,227,9,255,106,21,252,233,20,255,10,21,255,233,12,255,76,45,255,
    124,215,255,189,231,255,222,247,255,222,239,255,241,101,255,156,223,255,189,239,255,156,223,255,
    156,223,255,190,239,255,182,158,255,200,4,255,149,142,255,189,239,255,190,239,255,156,223,255,
    174,77,255,233,12,255,10,21,255,233,20,255,106,21,253,227,9,255,0,0,217,0,0,0,
    0,0,0,0,0,219,165,10,255,106,21,253,233,20,255,10,21,255,233,12,255,108,53,255,
    255,255,255,25,183,255,208,85,255,17,102,255,109,53,255,18,110,255,83,126,255,255,255,255,
    90,207,255,50,110,255,142,69,255,116,134,255,255,255,255,149,142,255,108,53,255,50,118,255,
    175,77,255,233,12,255,10,21,255,233,20,255,106,21,254,165,10,255,0,0,239,225,0,25,
    65,1,18,0,0,234,38,11,255,74,21,254,233,20,255,10,21,255,233,12,255,108,53,255,
    255,255,255,58,199,255,83,126,255,116,134,255,42,29,255,167,4,255,10,21,255,255,255,255,
    182,150,255,134,4,255,233,12,255,190,239,255,123,215,255,200,4,255,233,12,255,200,4,255,
    233,12,255,10,21,255,10,21,255,233,20,255,74,21,254,38,11,255,0,0,250,128,0,43,
    65,1,19,0,0,235,38,11,255,74,21,254,233,20,255,10,21,255,233,12,255,108,53,255,
    255,255,255,190,239,255,123,215,255,123,215,255,76,45,255,233,12,255,75,45,255,255,255,255,
    215,166,255,200,4,255,43,37,255,222,247,255,57,191,255,200,4,255,42,29,255,10,21,255,
    233,20,255,10,21,255,10,21,255,233,20,255,74,21,254,38,11,255,0,0,251,128,0,43,
    100,3,7,0,0,221,165,10,255,106,21,253,233,20,255,10,21,255,233,12,255,76,45,255,
    255,255,255,116,134,255,134,4,255,200,12,255,233,20,255,233,20,255,43,37,255,255,255,255,
    182,158,255,200,4,255,233,12,255,90,207,255,223,247,255,43,37,255,167,4,255,10,21,255,
    43,37,255,233,20,255,10,21,255,233,20,255,106,21,254,165,10,255,0,0,241,193,0,28,
    0,0,0,0,0,196,227,9,255,106,21,252,233,20,255,10,21,255,233,12,255,109,53,255,
    255,255,255,255,255,255,189,231,255,222,239,255,208,93,255,200,4,255,75,45,255,255,255,255,
    247,166,255,200,4,255,233,12,255,142,61,255,222,239,255,222,247,255,24,183,255,157,231,255,
    17,102,255,200,4,255,10,21,255,233,20,255,106,21,253,227,9,255,0,0,220,0,0,0,
    0,0,0,0,0,151,225,0,254,42,21,251,10,21,255,10,21,255,233,20,255,43,37,255,
    18,110,255,50,118,255,51,118,255,51,118,255,108,53,255,233,12,255,10,29,255,50,110,255,
    174,77,255,233,12,255,10,21,255,233,12,255,75,45,255,83,126,255,182,150,255,50,110,255,
    43,29,255,233,20,255,10,21,255,10,21,255,10,21,252,225,0,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,40,20,252,42,21,255,233,20,255,10,21,255,233,20,255,
    200,4,255,200,4,255,200,4,255,200,4,255,233,12,255,10,21,255,233,20,255,200,4,255,
    233,12,255,10,21,255,10,21,255,10,21,255,233,12,255,200,4,255,200,4,255,200,4,255,
    233,20,255,10,21,255,233,20,255,42,21,255,72,20,251,0,0,255,0,0,111,0,0,0,
    0,0,0,34,1,14,0,0,236,68,10,254,106,21,253,233,20,255,10,21,255,10,21,255,
    10,29,255,10,29,255,10,29,255,10,29,255,10,21,255,10,21,255,10,21,255,10,29,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,29,255,10,29,255,10,21,255,
    10,21,255,10,21,255,233,20,255,106,21,254,68,10,254,0,0,250,96,0,32,0,0,0,
    0,0,0,0,0,0,0,0,143,64,0,255,169,20,251,42,21,255,233,20,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,233,20,255,42,21,255,169,20,251,64,0,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,161,0,23,0,0,239,195,1,253,106,21,253,233,20,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,233,20,255,106,21,254,195,1,252,0,0,253,64,0,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,102,0,0,255,6,11,250,106,21,255,233,20,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    233,20,255,106,21,255,6,11,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,70,11,250,106,21,255,
    233,20,255,233,20,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,10,21,255,233,20,255,233,20,255,
    106,21,255,70,11,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,64,1,6,0,0,190,0,0,255,6,11,250,
    74,21,254,42,21,255,233,20,255,233,20,255,10,21,255,10,21,255,10,21,255,10,21,255,
    10,21,255,10,21,255,10,21,255,10,21,255,233,20,255,233,20,255,42,21,255,74,21,254,
    6,11,250,0,0,255,0,0,212,96,0,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,224,0,9,0,0,164,0,0,255,
    227,9,255,169,20,251,106,21,254,42,21,255,10,21,255,233,20,255,233,20,255,233,20,255,
    233,20,255,233,20,255,233,20,255,10,21,255,42,21,255,106,21,254,169,20,252,227,9,254,
    0,0,255,0,0,185,96,0,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,113,
    0,0,242,64,0,255,100,10,255,40,20,252,10,21,252,106,21,253,106,21,254,74,21,254,
    74,21,254,106,21,254,106,21,253,10,21,252,72,20,251,100,10,255,64,0,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    96,0,35,0,0,159,0,0,242,0,0,255,225,0,255,4,10,255,165,10,255,38,11,255,
    38,11,255,165,10,255,4,10,255,225,0,255,0,0,255,0,0,248,0,0,172,64,0,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,129,0,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,96,0,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,225,0,24,128,0,42,
    128,0,42,225,0,27,100,3,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 12 礼让行人
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,231,8,18,
    231,8,18,82,34,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,137,25,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,229,8,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    101,0,16,0,0,128,0,0,223,0,0,255,165,8,255,76,17,255,208,17,255,18,26,255,
    18,26,255,208,17,255,76,17,255,165,8,255,0,0,255,0,0,230,0,0,142,67,0,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,85,
    0,0,224,34,0,255,142,17,255,185,34,253,94,43,251,159,43,252,127,43,253,127,43,254,
    159,43,254,159,43,253,159,43,252,94,43,251,217,34,252,142,17,255,34,0,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    75,9,255,27,35,251,159,43,253,127,43,255,94,43,255,61,35,255,61,35,255,61,43,255,
    253,26,255,253,26,255,253,26,255,93,43,255,127,43,255,159,43,254,27,35,251,75,9,255,
    0,0,255,0,0,159,5,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,18,26,251,
    127,43,253,94,43,255,61,35,255,61,43,255,94,43,255,94,43,255,94,43,255,29,27,255,
    158,100,255,63,174,255,62,133,255,61,35,255,61,35,255,61,43,255,94,43,255,127,43,254,
    18,26,251,0,0,255,0,0,189,3,0,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,52,26,250,159,43,255,
    61,35,255,61,43,255,94,43,255,94,43,255,94,43,255,94,43,255,29,27,255,158,100,255,
    255,255,255,255,255,255,255,255,255,191,149,255,253,26,255,94,43,255,61,43,255,61,35,255,
    159,43,255,52,26,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,33,0,81,0,0,255,18,26,250,159,43,254,61,35,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,253,26,255,95,182,255,
    255,255,255,223,247,255,223,255,255,127,231,255,94,35,255,94,43,255,94,43,255,94,43,255,
    61,35,255,159,43,255,18,26,250,0,0,255,32,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,134,16,14,0,0,230,42,9,254,159,43,253,61,35,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,253,26,255,62,133,255,
    255,255,255,255,255,255,255,255,255,127,182,255,29,27,255,94,43,255,94,43,255,94,43,255,
    94,43,255,61,35,255,159,43,254,43,9,252,0,0,247,67,8,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,34,0,255,27,35,251,94,43,255,61,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,62,35,255,94,43,255,
    31,215,255,223,247,255,63,182,255,158,51,255,62,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,61,43,255,94,43,255,27,35,251,34,0,255,0,0,160,0,0,0,0,0,0,
    0,0,0,206,25,9,0,0,228,141,17,254,159,43,253,61,35,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,29,27,255,158,100,255,
    255,255,255,191,247,255,158,51,255,29,27,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,61,35,255,159,43,254,141,17,254,0,0,246,165,8,24,0,0,0,
    0,0,0,0,0,70,0,0,255,185,34,253,127,43,255,61,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,221,18,255,255,165,255,
    255,255,255,191,247,255,158,59,255,29,35,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,61,43,255,127,43,255,217,34,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,133,0,254,94,43,251,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,62,35,255,253,26,255,158,149,255,255,255,255,
    255,255,255,255,255,255,127,182,255,126,43,255,29,27,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,251,133,0,254,0,0,173,0,0,0,
    0,0,0,0,0,192,75,17,255,159,43,252,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,62,35,255,126,43,255,31,166,255,255,255,255,255,255,255,
    255,255,255,127,231,255,255,255,255,63,223,255,62,84,255,62,35,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,127,43,253,75,17,255,0,0,217,0,0,0,
    0,0,0,0,0,219,208,17,255,159,43,253,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,253,18,255,158,149,255,255,255,255,159,239,255,255,255,255,
    255,255,255,94,84,255,30,125,255,255,255,255,223,206,255,253,26,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,159,43,254,208,17,255,0,0,239,165,8,25,
    231,8,18,0,0,234,18,26,255,127,43,254,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,30,35,255,62,76,255,191,149,255,63,174,255,255,255,255,
    127,231,255,94,43,255,253,26,255,190,100,255,254,75,255,62,35,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,127,43,254,18,26,255,0,0,250,66,0,43,
    198,8,19,0,0,235,18,26,255,127,43,254,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,62,35,255,221,18,255,31,215,255,255,255,255,
    159,239,255,94,92,255,253,26,255,253,26,255,62,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,127,43,254,18,26,255,0,0,251,66,0,43,
    82,34,7,0,0,221,208,17,255,159,43,253,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,29,27,255,126,92,255,255,255,255,255,255,255,
    255,255,255,255,255,255,158,141,255,126,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,159,43,254,208,17,255,0,0,241,132,8,28,
    0,0,0,0,0,196,75,17,255,159,43,252,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,62,35,255,255,214,255,255,255,255,223,157,255,
    62,133,255,255,255,255,255,255,255,63,174,255,29,27,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,127,43,253,75,17,255,0,0,220,0,0,0,
    0,0,0,0,0,151,133,0,254,94,43,251,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,221,18,255,62,125,255,255,255,255,95,231,255,126,51,255,
    189,10,255,254,116,255,255,255,255,63,223,255,62,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,252,133,0,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,217,34,252,127,43,255,61,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,62,35,255,30,76,255,159,239,255,255,255,255,222,116,255,253,26,255,
    94,43,255,158,51,255,191,247,255,223,247,255,222,59,255,62,35,255,94,43,255,94,43,255,
    94,43,255,94,43,255,61,43,255,127,43,255,217,34,251,0,0,255,0,0,111,0,0,0,
    0,0,0,41,17,14,0,0,236,141,17,254,159,43,253,61,35,255,94,43,255,94,43,255,
    94,43,255,126,43,255,253,26,255,158,141,255,255,255,255,255,214,255,29,35,255,94,43,255,
    94,43,255,94,43,255,127,231,255,255,255,255,254,116,255,29,27,255,94,43,255,94,43,255,
    94,43,255,94,43,255,61,35,255,159,43,254,141,17,254,0,0,250,99,0,32,0,0,0,
    0,0,0,0,0,0,0,0,143,34,0,255,27,35,251,94,43,255,93,43,255,62,35,255,
    253,26,255,253,26,255,29,35,255,126,51,255,30,125,255,190,59,255,29,27,255,62,35,255,
    253,26,255,221,18,255,30,125,255,127,190,255,126,43,255,221,18,255,62,35,255,94,43,255,
    94,43,255,61,43,255,94,43,255,27,35,251,34,0,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,68,8,23,0,0,239,42,9,253,159,43,253,29,35,255,222,67,255,
    31,174,255,63,174,255,30,76,255,158,51,255,223,157,255,31,166,255,30,76,255,190,59,255,
    31,174,255,63,174,255,222,67,255,126,43,255,31,166,255,63,174,255,62,84,255,29,35,255,
    94,43,255,61,35,255,159,43,254,43,9,252,0,0,253,34,0,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,102,0,0,255,18,26,250,95,27,255,157,100,255,
    255,255,255,255,255,255,254,116,255,94,84,255,255,255,255,255,255,255,254,116,255,94,84,255,
    255,255,255,255,255,255,30,125,255,94,92,255,255,255,255,255,255,255,94,133,255,253,18,255,
    93,43,255,159,43,255,18,26,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,20,18,250,95,76,255,
    158,198,255,191,198,255,62,84,255,222,59,255,191,198,255,191,198,255,62,84,255,222,59,255,
    191,198,255,191,198,255,62,84,255,222,59,255,191,198,255,191,198,255,94,92,255,29,27,255,
    159,43,255,52,26,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,5,0,6,0,0,190,0,0,255,242,17,250,
    95,35,254,62,35,255,29,35,255,61,35,255,29,27,255,29,27,255,62,35,255,62,35,255,
    29,27,255,29,27,255,62,35,255,61,35,255,29,27,255,29,27,255,62,35,255,159,43,254,
    18,26,250,0,0,255,0,0,212,1,0,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,9,0,0,164,0,0,255,
    75,17,255,27,35,251,159,43,254,127,43,255,94,43,255,93,43,255,61,43,255,61,43,255,
    93,43,255,93,43,255,61,43,255,94,43,255,127,43,255,159,43,254,27,35,252,75,9,254,
    0,0,255,0,0,185,1,0,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,34,0,255,142,17,255,217,34,252,94,43,252,127,43,253,127,43,254,127,43,254,
    127,43,254,159,43,254,127,43,253,94,43,252,217,34,251,142,17,255,34,0,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    34,0,35,0,0,159,0,0,242,0,0,255,165,8,255,76,17,255,208,17,255,18,26,255,
    18,26,255,208,17,255,76,17,255,165,8,255,0,0,255,0,0,248,0,0,172,33,0,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,131,8,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,98,0,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,165,8,24,99,0,42,
    99,0,42,132,8,27,82,34,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 13 违法变道
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,231,8,18,
    231,8,18,82,34,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,137,25,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,229,8,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    101,0,16,0,0,128,0,0,223,0,0,255,165,8,255,76,17,255,208,17,255,18,26,255,
    18,26,255,208,17,255,76,17,255,165,8,255,0,0,255,0,0,230,0,0,142,67,0,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,0,85,
    0,0,224,34,0,255,142,17,255,185,34,253,94,43,251,159,43,252,127,43,253,159,43,254,
    159,43,254,127,43,253,159,43,252,94,43,251,217,34,252,142,17,255,34,0,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    75,9,255,27,35,251,159,43,253,127,43,255,94,43,255,61,35,255,61,43,255,29,27,255,
    29,27,255,61,43,255,61,35,255,94,43,255,127,43,255,159,43,254,27,35,251,75,9,255,
    0,0,255,0,0,159,5,0,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,18,26,251,
    127,43,253,94,43,255,61,35,255,61,43,255,94,43,255,94,43,255,62,35,255,94,84,255,
    62,84,255,62,35,255,94,43,255,93,43,255,61,35,255,30,35,255,94,43,255,127,43,254,
    18,26,251,0,0,255,0,0,189,3,0,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,52,26,250,159,43,255,
    61,35,255,61,43,255,94,43,255,94,43,255,94,43,255,94,43,255,29,35,255,95,223,255,
    127,231,255,62,35,255,94,43,255,94,35,255,93,43,255,218,75,255,62,35,255,61,43,255,
    159,43,255,52,26,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,33,0,81,0,0,255,18,26,250,159,43,254,61,35,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,253,26,255,127,182,255,
    159,239,255,62,35,255,93,43,255,31,19,255,180,132,255,135,246,255,156,59,255,62,35,255,
    61,43,255,159,43,255,18,26,250,0,0,255,32,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,134,16,14,0,0,230,42,9,254,159,43,253,61,35,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,253,26,255,127,182,255,
    159,239,255,61,35,255,62,35,255,187,67,255,104,246,255,197,254,255,142,189,255,31,27,255,
    93,43,255,61,35,255,159,43,254,43,9,252,0,0,247,67,8,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,34,0,255,27,35,251,94,43,255,61,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,253,26,255,127,182,255,
    159,239,255,62,35,255,31,27,255,204,205,255,198,254,255,104,254,255,198,254,255,149,124,255,
    255,18,255,93,43,255,94,43,255,27,35,251,34,0,255,0,0,160,0,0,0,0,0,0,
    0,0,0,206,25,9,0,0,228,141,17,254,159,43,253,61,35,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,253,26,255,127,182,255,
    159,239,255,31,27,255,17,157,255,4,255,255,136,254,255,135,254,255,198,254,255,198,254,255,
    249,83,255,62,35,255,61,43,255,159,43,254,141,17,254,0,0,246,165,8,24,0,0,0,
    0,0,0,0,0,70,0,0,255,185,34,253,127,43,255,61,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,253,26,255,127,182,255,
    159,239,255,93,43,255,211,140,255,49,157,255,135,254,255,135,254,255,48,165,255,242,148,255,
    86,108,255,62,35,255,93,43,255,127,43,255,217,34,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,133,0,254,94,43,251,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,253,26,255,127,182,255,
    159,239,255,93,43,255,191,2,255,211,140,255,229,254,255,11,222,255,31,27,255,255,18,255,
    31,27,255,93,43,255,94,43,255,94,43,255,94,43,251,133,0,254,0,0,173,0,0,0,
    0,0,0,0,0,192,75,17,255,159,43,252,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,253,26,255,127,182,255,
    159,239,255,255,18,255,211,140,255,197,254,255,166,254,255,55,100,255,62,27,255,93,43,255,
    93,43,255,94,43,255,94,43,255,61,35,255,127,43,253,75,17,255,0,0,217,0,0,0,
    0,0,0,0,0,219,208,17,255,159,43,253,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,253,26,255,127,190,255,
    159,231,255,148,124,255,166,254,255,198,254,255,180,132,255,31,27,255,93,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,159,43,254,208,17,255,0,0,239,165,8,25,
    231,8,18,0,0,234,18,26,255,127,43,254,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,93,43,255,254,18,255,63,174,255,
    113,255,255,133,254,255,229,254,255,211,148,255,31,19,255,93,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,127,43,254,18,26,255,0,0,250,66,0,43,
    198,8,19,0,0,235,18,26,255,127,43,254,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,93,43,255,62,27,255,125,51,255,15,206,255,
    165,254,255,135,254,255,149,124,255,31,27,255,93,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,127,43,254,18,26,255,0,0,251,66,0,43,
    82,34,7,0,0,221,208,17,255,159,43,253,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,93,43,255,31,27,255,187,67,255,11,222,255,164,254,255,
    104,246,255,24,92,255,255,18,255,93,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,159,43,254,208,17,255,0,0,241,132,8,28,
    0,0,0,0,0,196,75,17,255,159,43,252,61,35,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,93,43,255,31,27,255,218,75,255,73,238,255,197,254,255,235,254,255,
    251,222,255,63,35,255,93,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,35,255,127,43,253,75,17,255,0,0,220,0,0,0,
    0,0,0,0,0,151,133,0,254,94,43,251,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,93,43,255,31,27,255,249,83,255,104,246,255,229,254,255,108,189,255,158,190,255,
    159,239,255,61,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,94,43,255,94,43,252,133,0,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,217,34,252,127,43,255,61,43,255,94,43,255,94,43,255,
    93,43,255,62,35,255,186,67,255,104,246,255,229,254,255,174,197,255,253,26,255,95,182,255,
    159,239,255,62,35,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,61,43,255,127,43,255,217,34,251,0,0,255,0,0,111,0,0,0,
    0,0,0,41,17,14,0,0,236,141,17,254,159,43,253,61,35,255,94,43,255,94,43,255,
    93,43,255,31,27,255,173,197,255,228,254,255,173,197,255,94,43,255,222,18,255,127,190,255,
    159,239,255,62,35,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,61,35,255,159,43,254,141,17,254,0,0,250,99,0,32,0,0,0,
    0,0,0,0,0,0,0,0,143,34,0,255,27,35,251,94,43,255,61,43,255,93,43,255,
    31,27,255,24,92,255,166,254,255,104,238,255,187,67,255,63,35,255,253,26,255,127,182,255,
    159,239,255,62,35,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,61,43,255,94,43,255,27,35,251,34,0,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,68,8,23,0,0,239,42,9,253,159,43,253,61,35,255,93,43,255,
    31,19,255,242,148,255,4,255,255,110,189,255,31,27,255,125,43,255,253,18,255,127,182,255,
    159,239,255,62,35,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,61,35,255,159,43,254,43,9,252,0,0,253,34,0,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,102,0,0,255,18,26,250,159,43,255,61,43,255,
    31,27,255,86,108,255,198,246,255,180,132,255,31,27,255,125,43,255,253,26,255,127,182,255,
    159,239,255,62,35,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
    61,35,255,159,43,255,18,26,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,52,26,250,159,43,255,
    61,43,255,62,35,255,187,67,255,94,43,255,94,35,255,94,43,255,29,35,255,95,223,255,
    127,231,255,62,35,255,94,43,255,94,43,255,94,43,255,94,43,255,61,43,255,61,35,255,
    159,43,255,52,26,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,5,0,6,0,0,190,0,0,255,18,26,250,
    127,43,254,94,43,255,30,35,255,61,43,255,94,43,255,94,43,255,62,35,255,94,84,255,
    62,84,255,62,35,255,94,43,255,94,43,255,61,43,255,61,35,255,94,43,255,127,43,254,
    18,26,250,0,0,255,0,0,212,1,0,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,9,0,0,164,0,0,255,
    75,9,255,27,35,251,159,43,254,127,43,255,94,43,255,61,35,255,61,43,255,29,27,255,
    29,27,255,61,43,255,61,35,255,94,43,255,127,43,255,159,43,254,27,35,252,75,9,254,
    0,0,255,0,0,185,1,0,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,34,0,255,142,17,255,217,34,252,94,43,252,127,43,253,127,43,254,159,43,254,
    159,43,254,127,43,254,127,43,253,94,43,252,217,34,251,142,17,255,34,0,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    34,0,35,0,0,159,0,0,242,0,0,255,165,8,255,76,17,255,208,17,255,18,26,255,
    18,26,255,208,17,255,76,17,255,165,8,255,0,0,255,0,0,248,0,0,172,33,0,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,131,8,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,98,0,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,165,8,24,99,0,42,
    99,0,42,132,8,27,82,34,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 14 违停
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,97,56,18,
    97,56,18,36,145,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,72,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,0,40,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,40,16,0,0,128,0,0,223,0,0,255,32,40,255,97,88,255,130,120,255,162,136,255,
    162,136,255,130,120,255,97,88,255,32,40,255,0,0,255,0,0,230,0,0,142,0,24,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,85,
    0,0,224,0,16,255,97,104,255,195,184,253,4,225,251,4,241,252,4,241,253,3,241,254,
    3,241,254,4,241,253,4,241,252,4,225,251,195,184,252,97,104,255,0,16,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    97,80,255,227,208,251,36,241,253,3,233,255,194,232,255,226,232,255,4,217,255,37,209,255,
    37,209,255,4,217,255,226,224,255,194,232,255,227,232,255,36,233,254,227,208,251,97,80,255,
    0,0,255,0,0,159,0,40,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,130,136,251,
    4,241,253,3,233,255,194,232,255,37,209,255,238,145,255,150,98,255,250,66,255,28,51,255,
    28,51,255,250,58,255,182,90,255,15,138,255,70,201,255,194,232,255,227,232,255,4,241,254,
    130,136,251,0,0,255,0,0,189,0,24,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,162,144,250,4,241,255,
    227,216,255,5,217,255,16,138,255,60,51,255,127,27,255,127,27,255,94,35,255,94,35,255,
    94,35,255,94,35,255,127,27,255,127,27,255,61,43,255,50,122,255,38,209,255,227,216,255,
    4,241,255,162,144,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,81,0,0,255,130,136,250,4,241,254,4,209,255,
    226,232,255,15,138,255,159,11,255,94,43,255,61,43,255,61,43,255,61,43,255,93,43,255,
    93,43,255,93,43,255,61,43,255,61,43,255,61,43,255,159,11,255,16,130,255,226,232,255,
    4,209,255,4,241,255,130,136,250,0,0,255,0,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,48,14,0,0,230,97,80,254,4,241,253,227,216,255,226,232,255,
    227,224,255,4,217,255,183,90,255,127,27,255,61,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,43,255,127,27,255,183,90,255,4,217,255,227,224,255,
    226,232,255,227,216,255,4,241,254,65,80,252,0,0,247,0,24,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,0,8,255,227,208,251,3,233,255,5,217,255,15,138,255,
    4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,61,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,61,43,255,127,27,255,183,82,255,4,217,255,226,232,255,4,225,255,
    49,130,255,38,201,255,227,232,255,227,208,251,0,8,255,0,0,160,0,0,0,0,0,0,
    0,0,0,0,80,9,0,0,228,97,96,254,36,241,253,194,232,255,16,138,255,159,11,255,
    183,90,255,4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,61,43,255,94,43,255,
    94,43,255,61,43,255,127,27,255,183,82,255,4,217,255,226,232,255,4,217,255,183,90,255,
    159,11,255,83,114,255,194,232,255,4,241,254,97,96,254,0,0,246,0,24,24,0,0,0,
    0,0,0,0,0,70,0,0,255,195,184,253,3,233,255,37,209,255,60,51,255,94,43,255,
    127,27,255,183,82,255,4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,61,43,255,
    61,43,255,127,27,255,183,82,255,4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,
    61,43,255,94,35,255,72,193,255,227,240,255,195,184,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,32,40,254,4,225,251,194,232,255,238,145,255,127,27,255,61,43,255,
    61,43,255,127,27,255,183,82,255,4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,
    127,27,255,183,82,255,4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,61,43,255,
    61,43,255,127,27,255,49,122,255,194,232,255,4,225,251,32,40,254,0,0,173,0,0,0,
    0,0,0,0,0,192,97,88,255,4,241,252,194,232,255,150,98,255,127,27,255,61,43,255,
    94,43,255,61,43,255,127,27,255,183,82,255,4,217,255,226,232,255,4,217,255,216,74,255,
    216,74,255,4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,61,43,255,94,43,255,
    61,43,255,95,35,255,216,74,255,227,224,255,4,241,253,97,88,255,0,0,217,0,0,0,
    0,0,0,0,0,219,130,120,255,4,241,253,4,217,255,250,66,255,94,35,255,61,43,255,
    94,43,255,94,43,255,61,43,255,127,27,255,183,82,255,4,217,255,226,232,255,37,217,255,
    37,217,255,226,232,255,4,217,255,183,82,255,127,27,255,61,43,255,94,43,255,94,43,255,
    93,43,255,94,35,255,60,51,255,38,201,255,3,241,254,130,120,255,0,0,239,65,40,25,
    97,56,18,0,0,234,162,136,255,3,241,254,37,209,255,28,51,255,94,35,255,93,43,255,
    94,43,255,94,43,255,94,43,255,61,43,255,127,27,255,216,74,255,37,217,255,227,224,255,
    227,224,255,37,217,255,216,74,255,127,27,255,61,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,71,193,255,3,241,254,162,136,255,0,0,250,32,16,43,
    97,48,19,0,0,235,162,136,255,3,241,254,37,209,255,28,51,255,94,35,255,93,43,255,
    94,43,255,94,43,255,94,43,255,61,43,255,127,27,255,216,74,255,37,217,255,227,224,255,
    227,224,255,37,217,255,216,74,255,127,27,255,61,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,72,193,255,3,241,254,162,136,255,0,0,251,32,16,43,
    36,145,7,0,0,221,130,120,255,4,241,253,4,217,255,250,58,255,94,35,255,61,43,255,
    94,43,255,94,43,255,61,43,255,127,27,255,183,82,255,4,217,255,226,232,255,37,217,255,
    37,217,255,226,232,255,4,217,255,183,82,255,127,27,255,61,43,255,94,43,255,94,43,255,
    93,43,255,94,35,255,60,51,255,38,201,255,3,241,254,130,120,255,0,0,241,65,32,28,
    0,0,0,0,0,196,97,88,255,4,241,252,226,224,255,182,90,255,127,27,255,61,43,255,
    94,43,255,61,43,255,127,27,255,183,82,255,4,217,255,226,232,255,4,217,255,216,74,255,
    216,74,255,4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,61,43,255,94,43,255,
    61,43,255,95,35,255,217,74,255,227,216,255,4,241,253,97,88,255,0,0,220,0,0,0,
    0,0,0,0,0,151,32,40,254,4,225,251,194,232,255,15,138,255,127,27,255,61,43,255,
    61,43,255,127,27,255,183,82,255,4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,
    127,27,255,183,82,255,4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,61,43,255,
    61,43,255,127,27,255,82,114,255,194,232,255,4,225,252,32,40,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,195,184,252,3,233,255,38,201,255,61,43,255,61,43,255,
    127,27,255,183,82,255,4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,61,43,255,
    61,43,255,127,27,255,183,82,255,4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,
    61,43,255,94,35,255,105,185,255,227,240,255,195,184,251,0,0,255,0,0,111,0,0,0,
    0,0,0,0,48,14,0,0,236,97,96,254,4,241,253,194,232,255,50,122,255,159,11,255,
    183,90,255,4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,61,43,255,94,43,255,
    94,43,255,61,43,255,127,27,255,183,82,255,4,217,255,226,232,255,4,217,255,183,90,255,
    159,11,255,149,98,255,194,224,255,4,241,254,97,96,254,0,0,250,0,16,32,0,0,0,
    0,0,0,0,0,0,0,0,143,0,8,255,227,208,251,227,232,255,38,209,255,16,130,255,
    4,217,255,226,232,255,4,217,255,183,82,255,127,27,255,61,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,61,43,255,127,27,255,183,82,255,4,217,255,227,232,255,4,225,255,
    82,122,255,71,193,255,227,232,255,227,208,251,0,8,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,0,32,23,0,0,239,65,80,253,4,241,253,227,216,255,226,232,255,
    227,224,255,4,225,255,183,90,255,127,27,255,61,43,255,94,43,255,94,43,255,94,43,255,
    94,43,255,94,43,255,94,43,255,61,43,255,127,27,255,183,90,255,4,225,255,227,224,255,
    226,232,255,227,216,255,4,241,254,65,80,252,0,0,253,0,16,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,102,0,0,255,130,136,250,4,241,255,4,209,255,
    226,232,255,49,130,255,159,11,255,61,43,255,61,43,255,61,43,255,93,43,255,94,43,255,
    94,43,255,93,43,255,61,43,255,61,43,255,61,43,255,159,11,255,82,122,255,226,232,255,
    4,217,255,4,241,255,130,136,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,162,144,250,4,241,255,
    227,216,255,38,201,255,83,114,255,94,35,255,127,27,255,95,35,255,94,35,255,94,43,255,
    94,43,255,94,35,255,95,35,255,127,27,255,94,35,255,149,98,255,71,193,255,227,216,255,
    4,241,255,162,144,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,40,6,0,0,190,0,0,255,162,136,250,
    4,241,254,227,232,255,194,232,255,72,193,255,49,122,255,216,74,255,60,51,255,93,43,255,
    94,43,255,60,51,255,217,74,255,82,114,255,105,185,255,194,224,255,227,232,255,4,233,254,
    162,136,250,0,0,255,0,0,212,0,8,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,24,9,0,0,164,0,0,255,
    97,80,255,227,208,251,36,233,254,227,232,255,194,232,255,227,224,255,38,201,255,71,193,255,
    72,193,255,38,201,255,227,216,255,194,232,255,227,240,255,36,241,254,227,200,252,97,80,254,
    0,0,255,0,0,185,0,8,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,0,16,255,97,104,255,195,184,252,4,225,252,4,241,253,3,241,254,3,241,254,
    3,241,254,3,241,254,4,241,253,4,225,252,195,184,251,97,104,255,0,16,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,16,35,0,0,159,0,0,242,0,0,255,32,40,255,97,88,255,130,120,255,162,136,255,
    162,136,255,130,120,255,97,88,255,32,40,255,0,0,255,0,0,248,0,0,172,0,8,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,24,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,0,16,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,65,40,24,32,24,42,
    32,24,42,65,32,27,36,145,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 15 禁止鸣笛
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,97,56,18,
    97,56,18,36,145,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,72,10,0,0,71,0,0,141,0,0,190,0,0,217,0,0,233,
    0,0,233,0,0,220,0,0,195,0,0,149,0,0,80,0,40,16,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,40,16,0,0,128,0,0,223,0,0,255,32,40,255,97,88,255,130,120,255,162,136,255,
    162,136,255,130,120,255,97,88,255,32,40,255,0,0,255,0,0,230,0,0,142,0,24,27,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,85,
    0,0,224,0,16,255,97,104,255,195,184,253,4,225,251,36,241,252,36,241,253,36,241,254,
    36,241,254,36,241,253,36,241,252,4,225,251,195,184,252,97,104,255,0,16,255,0,0,235,
    0,0,105,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,0,0,255,
    97,80,255,227,208,251,36,241,253,36,233,255,227,224,255,162,216,255,130,216,255,97,216,255,
    97,216,255,130,216,255,162,216,255,227,224,255,36,233,255,36,241,254,227,208,251,97,80,255,
    0,0,255,0,0,159,0,40,6,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,166,0,0,255,162,136,251,
    4,241,253,4,225,255,227,216,255,130,216,255,4,225,255,105,226,255,174,235,255,113,244,255,
    113,244,255,174,235,255,105,226,255,4,225,255,130,216,255,227,216,255,36,225,255,4,241,254,
    162,136,251,0,0,255,0,0,189,0,24,8,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,0,0,255,162,144,250,4,241,255,
    4,217,255,195,216,255,69,225,255,81,236,255,251,254,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,251,254,255,81,236,255,69,225,255,162,216,255,4,217,255,
    4,241,255,162,144,250,0,0,255,0,0,166,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,81,0,0,255,130,136,250,4,241,254,4,217,255,
    195,224,255,8,226,255,60,255,255,255,255,255,255,255,255,255,255,255,255,255,255,223,255,255,
    223,255,255,255,255,255,255,255,255,255,255,255,255,255,255,28,255,255,170,234,255,130,216,255,
    4,217,255,4,241,255,130,136,250,0,0,255,0,0,111,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,48,14,0,0,230,97,80,254,4,241,253,4,217,255,195,224,255,
    195,224,255,101,225,255,219,254,255,255,255,255,223,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,77,235,255,
    130,216,255,4,217,255,4,241,254,97,80,252,0,0,247,0,24,31,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,127,0,8,255,227,208,251,4,225,255,195,216,255,8,226,255,
    101,225,255,97,216,255,134,225,255,219,254,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,93,239,255,255,255,255,255,255,255,255,255,255,
    170,234,255,162,216,255,36,225,255,227,208,251,0,8,255,0,0,160,0,0,0,0,0,0,
    0,0,0,0,80,9,0,0,228,97,96,254,4,241,253,227,216,255,36,225,255,93,255,255,
    219,254,255,134,225,255,97,216,255,134,225,255,219,254,255,255,255,255,223,255,255,255,255,255,
    255,255,255,255,255,255,89,206,255,73,74,255,12,99,255,255,255,255,190,255,255,255,255,255,
    60,255,255,69,225,255,227,216,255,4,241,254,97,96,254,0,0,246,0,24,24,0,0,0,
    0,0,0,0,0,70,0,0,255,195,184,253,36,233,255,97,216,255,113,244,255,255,255,255,
    255,255,255,219,254,255,101,225,255,65,216,255,134,225,255,219,254,255,255,255,255,255,255,255,
    154,214,255,138,82,255,0,0,255,0,0,255,235,90,255,255,255,255,255,255,255,223,255,255,
    255,255,255,113,244,255,97,216,255,36,233,255,195,184,251,0,0,255,0,0,100,0,0,0,
    0,0,0,0,0,143,32,40,254,4,225,251,227,224,255,4,225,255,28,255,255,255,255,255,
    223,255,255,255,255,255,93,255,255,40,242,255,130,224,255,166,225,255,215,213,255,12,91,255,
    32,0,255,0,0,255,32,0,255,0,0,255,77,107,255,215,189,255,20,165,255,255,255,255,
    255,255,255,28,255,255,4,225,255,227,224,255,4,225,251,32,40,254,0,0,173,0,0,0,
    0,0,0,0,0,192,97,88,255,36,241,252,162,216,255,105,234,255,255,255,255,255,255,255,
    255,255,255,24,198,255,44,99,255,8,98,255,195,200,255,4,233,255,130,192,255,0,16,255,
    0,0,255,0,0,255,32,0,255,0,0,255,142,115,255,77,107,255,0,0,255,85,173,255,
    255,255,255,255,255,255,105,234,255,162,216,255,36,241,253,97,88,255,0,0,217,0,0,0,
    0,0,0,0,0,219,130,120,255,36,241,253,130,216,255,207,235,255,255,255,255,190,255,255,
    255,255,255,174,115,255,0,0,255,0,0,255,32,32,255,227,200,255,36,241,255,4,209,255,
    32,32,255,0,0,255,32,8,255,0,0,255,12,99,255,223,255,255,69,41,255,195,24,255,
    190,247,255,255,255,255,174,235,255,130,216,255,36,241,254,130,120,255,0,0,239,65,40,25,
    97,56,18,0,0,234,162,136,255,36,241,254,97,216,255,113,244,255,255,255,255,190,255,255,
    255,255,255,16,132,255,32,0,255,32,8,255,0,0,255,32,32,255,227,200,255,4,241,255,
    227,200,255,32,32,255,0,0,255,0,0,255,203,90,255,255,255,255,146,148,255,0,0,255,
    186,222,255,255,255,255,113,236,255,97,216,255,36,241,254,162,136,255,0,0,250,32,16,43,
    97,48,19,0,0,235,162,136,255,36,241,254,97,216,255,113,244,255,255,255,255,190,255,255,
    255,255,255,16,132,255,0,0,255,32,0,255,0,0,255,0,0,255,32,32,255,227,200,255,
    4,241,255,227,200,255,65,32,255,0,0,255,203,90,255,255,255,255,146,148,255,0,0,255,
    186,222,255,255,255,255,113,236,255,97,216,255,36,241,254,162,136,255,0,0,251,32,16,43,
    36,145,7,0,0,221,130,120,255,36,241,253,130,216,255,207,235,255,255,255,255,190,255,255,
    255,255,255,174,115,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,65,40,255,
    227,200,255,4,241,255,4,209,255,0,24,255,235,82,255,223,255,255,69,41,255,195,24,255,
    190,247,255,255,255,255,174,235,255,130,216,255,36,241,254,130,120,255,0,0,241,65,32,28,
    0,0,0,0,0,196,97,88,255,36,241,252,162,216,255,105,234,255,255,255,255,223,255,255,
    255,255,255,182,181,255,8,66,255,40,66,255,40,66,255,138,74,255,32,8,255,0,0,255,
    32,32,255,4,209,255,36,241,255,32,176,255,207,147,255,109,99,255,0,0,255,85,173,255,
    255,255,255,255,255,255,105,234,255,162,216,255,36,241,253,97,88,255,0,0,220,0,0,0,
    0,0,0,0,0,151,32,40,254,4,225,251,227,224,255,4,225,255,28,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,186,214,255,12,99,255,
    32,0,255,0,24,255,227,208,255,4,241,255,231,233,255,113,172,255,85,165,255,255,255,255,
    255,255,255,28,255,255,4,225,255,227,224,255,4,225,252,32,40,255,0,0,180,0,0,0,
    0,0,0,0,0,80,0,0,255,195,184,252,36,233,255,97,216,255,113,244,255,255,255,255,
    223,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    154,214,255,138,74,255,0,24,255,130,192,255,195,232,255,231,233,255,93,255,255,255,255,255,
    255,255,255,113,244,255,97,216,255,36,233,255,195,184,251,0,0,255,0,0,111,0,0,0,
    0,0,0,0,48,14,0,0,236,97,96,254,4,241,253,227,216,255,69,225,255,60,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,56,190,255,170,114,255,227,200,255,130,224,255,101,217,255,251,254,255,
    125,255,255,36,225,255,227,216,255,4,241,254,97,96,254,0,0,250,0,16,32,0,0,0,
    0,0,0,0,0,0,0,0,143,0,8,255,227,208,251,36,225,255,162,216,255,170,234,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,56,230,255,166,225,255,65,216,255,199,225,255,
    73,226,255,162,216,255,4,225,255,227,208,251,0,8,255,0,0,176,0,0,0,0,0,0,
    0,0,0,0,0,0,0,32,23,0,0,239,97,80,253,4,241,253,4,217,255,130,216,255,
    77,235,255,255,255,255,255,255,255,223,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,223,255,255,255,255,255,28,255,255,199,225,255,162,224,255,
    195,224,255,4,217,255,4,241,254,97,80,252,0,0,253,0,16,43,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,102,0,0,255,130,136,250,4,241,255,4,217,255,
    130,216,255,170,234,255,28,255,255,255,255,255,255,255,255,255,255,255,255,255,255,223,255,255,
    223,255,255,255,255,255,255,255,255,255,255,255,255,255,255,60,255,255,73,226,255,195,224,255,
    4,217,255,4,241,255,130,136,250,0,0,255,0,0,133,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,161,0,0,255,162,144,250,4,241,255,
    4,217,255,162,216,255,69,225,255,81,236,255,251,254,255,255,255,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,251,254,255,81,236,255,36,225,255,162,216,255,4,217,255,
    4,241,255,162,144,250,0,0,255,0,0,188,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,40,6,0,0,190,0,0,255,162,136,250,
    4,241,254,36,225,255,227,216,255,130,216,255,4,225,255,105,226,255,174,235,255,113,244,255,
    113,244,255,174,235,255,105,226,255,4,225,255,130,216,255,227,216,255,4,225,255,4,241,254,
    162,136,250,0,0,255,0,0,212,0,8,18,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,24,9,0,0,164,0,0,255,
    97,80,255,227,208,251,36,241,254,36,233,255,227,224,255,162,216,255,130,216,255,97,216,255,
    97,216,255,130,216,255,162,216,255,227,224,255,36,233,255,36,241,254,227,208,252,97,80,254,
    0,0,255,0,0,185,0,8,20,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,113,
    0,0,242,0,16,255,97,104,255,195,184,252,4,225,252,36,241,253,36,241,254,36,241,254,
    36,241,254,36,241,254,36,241,253,4,225,252,195,192,251,97,104,255,0,16,255,0,0,250,
    0,0,134,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,16,35,0,0,159,0,0,242,0,0,255,32,40,255,97,88,255,130,120,255,162,136,255,
    162,136,255,130,120,255,97,88,255,32,40,255,0,0,255,0,0,248,0,0,172,0,8,49,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,24,28,0,0,100,0,0,170,0,0,215,0,0,237,0,0,250,
    0,0,250,0,0,239,0,0,220,0,0,178,0,0,110,0,16,36,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,65,40,24,32,24,42,
    32,24,42,65,32,27,36,145,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 16 急弯
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,60,
    0,0,78,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,197,
    0,0,228,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,72,0,0,255,
    0,0,255,0,0,99,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,210,0,0,255,
    0,0,253,0,0,229,0,0,13,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,100,0,0,255,128,57,252,
    224,81,251,0,0,255,0,0,129,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,231,0,0,253,192,196,253,
    128,229,254,0,0,253,0,0,244,0,8,28,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,133,0,0,255,192,73,252,96,254,255,
    160,254,255,128,106,251,0,0,255,32,0,158,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,30,0,0,245,0,0,253,32,213,254,96,254,255,
    64,254,255,160,229,254,64,8,253,0,0,250,32,0,45,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,162,0,0,255,128,106,251,160,254,255,32,254,255,
    32,254,255,160,254,255,32,131,252,0,0,255,32,0,183,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,51,0,0,254,96,8,253,160,229,255,96,254,255,32,254,255,
    32,254,255,64,254,255,0,246,255,160,24,252,0,0,255,0,0,69,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,189,0,0,255,64,131,252,160,254,255,32,254,255,32,254,255,
    32,254,255,0,246,255,160,254,255,192,155,252,0,0,254,0,0,207,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,32,0,76,0,0,255,192,32,252,32,246,255,64,254,255,32,254,255,128,254,255,
    160,254,255,160,254,255,64,254,255,128,254,255,64,49,252,0,0,255,32,8,94,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,213,0,0,255,0,164,252,160,254,255,32,254,255,128,254,255,224,196,255,
    160,147,255,64,139,255,160,237,255,32,164,255,96,139,253,64,8,254,0,0,225,0,24,10,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,0,104,0,0,255,64,49,252,96,254,255,96,254,255,32,246,255,32,82,255,0,0,255,
    0,0,255,0,0,255,224,122,255,96,98,255,128,16,255,160,24,252,0,0,255,0,0,122,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,17,
    0,0,234,0,0,253,160,196,253,160,254,255,0,246,255,128,65,255,0,0,255,64,8,255,
    64,90,255,128,98,255,128,221,255,128,106,255,0,0,255,0,0,254,0,0,253,0,0,240,
    0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,137,
    0,0,255,0,82,251,128,254,255,160,254,255,32,131,255,0,0,255,192,32,255,32,205,255,
    192,254,255,128,254,255,192,254,255,64,90,255,0,0,255,0,0,255,0,0,252,0,0,255,
    0,0,151,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,33,0,0,245,
    32,0,253,64,221,254,128,254,255,32,254,255,0,41,255,0,0,255,64,172,255,192,254,255,
    0,254,255,0,246,255,128,254,255,96,98,255,0,0,255,64,16,255,64,131,254,96,16,253,
    0,0,251,32,0,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,165,0,0,255,
    192,114,251,160,254,255,64,254,255,192,237,255,96,16,255,0,0,255,96,221,255,96,254,255,
    32,254,255,32,254,255,160,254,255,0,82,255,0,41,255,96,213,255,32,255,255,0,123,252,
    0,0,255,32,0,176,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,8,53,0,0,255,96,16,253,
    192,237,255,64,254,255,64,254,255,192,237,255,64,8,255,32,8,255,160,229,255,96,254,255,
    32,254,255,32,254,255,96,254,255,128,188,255,224,245,255,128,254,255,32,254,255,224,237,255,
    128,24,252,0,0,255,32,8,63,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,192,0,0,255,96,139,252,
    160,254,255,32,254,255,64,254,255,192,237,255,64,8,255,64,8,255,224,245,255,64,254,255,
    32,254,255,64,254,255,32,254,255,128,254,255,64,254,255,32,254,255,32,254,255,160,254,255,
    160,155,252,0,0,255,0,0,201,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,79,0,0,255,224,32,252,64,254,255,
    64,254,255,32,254,255,64,254,255,192,237,255,64,8,255,64,8,255,192,237,255,64,254,255,
    32,254,255,64,254,255,64,254,255,32,254,255,32,254,255,64,254,255,64,254,255,64,254,255,
    64,254,255,0,41,252,0,0,255,32,8,89,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,215,0,0,254,32,172,253,160,254,255,
    32,254,255,32,254,255,64,254,255,192,237,255,64,8,255,64,8,255,192,237,255,64,254,255,
    32,254,255,64,254,255,64,254,255,64,254,255,64,254,255,64,254,255,64,254,255,32,254,255,
    160,254,255,96,180,253,0,0,254,0,0,220,0,0,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,108,0,0,255,96,57,252,128,254,255,32,254,255,
    64,254,255,32,254,255,64,254,255,192,237,255,96,8,255,96,16,255,192,237,255,64,254,255,
    32,254,255,64,254,255,64,254,255,64,254,255,64,254,255,64,254,255,64,254,255,64,254,255,
    32,254,255,128,254,255,128,65,252,0,0,255,32,0,114,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,18,0,0,236,0,0,254,192,196,253,96,254,255,0,254,255,
    32,254,255,32,254,255,32,254,255,192,237,255,0,0,255,0,0,255,96,221,255,64,254,255,
    0,254,255,32,254,255,32,254,255,32,254,255,32,254,255,32,254,255,32,254,255,32,254,255,
    0,254,255,96,254,255,192,196,253,0,0,253,0,0,237,0,0,20,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,141,0,0,255,160,106,251,32,255,255,160,254,255,160,254,255,
    160,254,255,160,254,255,160,254,255,224,254,255,32,172,255,160,155,255,192,254,255,160,254,255,
    160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,
    160,254,255,160,254,255,32,255,255,160,106,251,0,0,255,32,0,145,0,0,0,0,0,0,
    0,0,0,0,0,31,0,0,243,0,0,249,96,98,250,64,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,32,131,251,32,131,251,128,147,251,128,147,251,32,131,251,32,131,251,
    32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,64,131,251,96,98,250,0,0,249,0,0,245,0,0,33,0,0,0,
    0,0,0,0,0,183,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,183,0,0,0,
    0,0,22,0,0,157,0,0,158,0,0,159,0,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,0,0,159,0,0,159,0,0,159,0,0,157,0,0,22,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 17 学校
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,60,
    0,0,78,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,197,
    0,0,228,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,72,0,0,255,
    0,0,255,0,0,99,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,210,0,0,255,
    0,0,253,0,0,229,0,0,13,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,100,0,0,255,128,57,252,
    224,81,251,0,0,255,0,0,129,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,231,0,0,253,192,196,253,
    128,229,254,0,0,253,0,0,244,0,8,28,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,133,0,0,255,192,73,252,96,254,255,
    160,254,255,128,106,251,0,0,255,32,0,158,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,30,0,0,245,0,0,253,32,213,254,96,254,255,
    64,254,255,160,229,254,64,8,253,0,0,250,32,0,45,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,162,0,0,255,128,98,251,160,254,255,32,254,255,
    32,254,255,160,254,255,32,131,252,0,0,255,32,0,183,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,8,51,0,0,254,160,24,253,0,246,255,64,254,255,32,254,255,
    32,254,255,64,254,255,0,246,255,128,24,252,0,0,255,0,0,69,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,189,0,0,255,32,49,252,224,196,255,96,254,255,32,254,255,
    64,254,255,0,254,255,224,254,255,64,180,252,0,0,254,0,0,207,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,76,0,0,255,0,0,252,0,0,255,0,0,255,0,205,255,128,254,255,
    32,254,255,128,254,255,64,213,255,0,164,255,192,32,252,0,0,255,0,0,94,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,213,0,0,255,0,0,252,32,0,255,0,0,255,224,163,255,160,254,255,
    64,254,255,64,213,255,64,8,255,0,0,255,32,0,253,0,0,254,0,0,225,0,0,10,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,0,104,0,0,255,128,65,252,224,114,255,0,0,255,160,65,255,32,246,255,32,254,255,
    160,254,255,192,155,255,0,0,255,32,0,255,0,0,255,224,32,252,0,0,255,0,0,122,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,17,
    0,0,234,0,0,253,64,221,253,64,172,255,0,0,255,224,122,255,224,254,255,32,254,255,
    64,254,255,192,229,255,96,57,255,0,0,255,160,114,255,0,205,254,0,0,253,0,0,240,
    0,8,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,137,
    0,0,255,224,81,251,96,246,255,128,147,255,0,0,255,32,90,255,0,230,255,0,254,255,
    64,254,255,64,255,255,96,139,255,0,0,255,64,172,255,160,255,255,64,90,252,0,0,255,
    32,0,151,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,33,0,0,245,
    64,8,253,192,196,254,96,16,255,0,0,255,0,0,255,32,0,255,0,0,255,128,180,255,
    0,246,255,96,131,255,160,65,255,0,0,255,160,65,255,192,139,255,64,221,254,64,8,253,
    0,0,251,32,0,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,165,0,0,255,
    192,114,251,128,254,255,128,180,255,192,114,255,0,0,255,192,73,255,128,172,255,224,245,255,
    64,221,255,160,24,255,64,8,255,0,0,255,64,8,255,192,32,255,32,246,255,32,131,252,
    0,0,255,32,0,176,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,8,53,0,0,255,96,16,253,
    192,237,255,64,254,255,0,255,255,32,172,255,0,0,255,160,106,255,0,255,255,32,254,255,
    64,254,255,192,254,255,64,131,255,0,0,255,64,139,255,192,254,255,64,254,255,224,237,255,
    128,24,252,0,0,255,32,8,63,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,192,0,0,255,96,139,252,
    160,254,255,0,246,255,160,254,255,96,139,255,0,0,255,32,90,255,128,254,255,32,254,255,
    32,254,255,160,254,255,128,106,255,0,0,255,224,122,255,160,254,255,0,246,255,160,254,255,
    160,155,252,0,0,255,0,0,201,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,79,0,0,255,224,32,252,64,254,255,
    64,254,255,32,254,255,128,254,255,192,73,255,0,0,255,128,16,255,160,237,255,96,254,255,
    64,254,255,32,254,255,224,32,255,0,0,255,224,32,255,32,254,255,64,254,255,32,254,255,
    64,254,255,0,41,252,0,0,255,32,8,89,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,215,0,0,254,32,172,253,160,254,255,
    0,254,255,128,254,255,96,221,255,32,0,255,32,49,255,0,0,255,96,139,255,160,254,255,
    128,254,255,64,180,255,0,0,255,160,65,255,0,0,255,64,172,255,160,254,255,0,254,255,
    160,254,255,96,180,253,0,0,254,0,0,220,0,0,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,108,0,0,255,96,57,252,128,254,255,32,254,255,
    32,254,255,160,254,255,160,106,255,0,0,255,32,213,255,0,41,255,96,16,255,32,254,255,
    96,254,255,0,41,255,64,8,255,192,237,255,32,8,255,32,49,255,128,254,255,32,254,255,
    32,254,255,128,254,255,128,65,252,0,0,255,32,0,114,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,18,0,0,236,0,0,254,192,196,253,96,254,255,0,254,255,
    0,254,255,128,254,255,160,106,255,0,82,255,192,254,255,128,147,255,128,57,255,32,246,255,
    32,246,255,128,57,255,96,139,255,224,254,255,192,114,255,0,82,255,96,254,255,32,254,255,
    0,254,255,96,254,255,192,196,253,0,0,253,0,0,237,0,0,20,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,141,0,0,255,160,106,251,32,255,255,160,254,255,160,254,255,
    160,254,255,160,254,255,224,254,255,224,254,255,160,254,255,192,254,255,224,254,255,192,254,255,
    192,254,255,224,254,255,224,254,255,160,254,255,224,254,255,224,254,255,192,254,255,160,254,255,
    160,254,255,160,254,255,32,255,255,160,106,251,0,0,255,32,0,145,0,0,0,0,0,0,
    0,0,0,0,0,31,0,0,243,0,0,249,96,98,250,64,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,64,131,251,96,98,250,0,0,249,0,0,245,0,0,33,0,0,0,
    0,0,0,0,0,183,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,183,0,0,0,
    0,0,22,0,0,157,0,0,158,0,0,159,0,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,0,0,159,0,0,159,0,0,159,0,0,157,0,0,22,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 18 事故多发
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,60,
    0,0,78,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,197,
    0,0,228,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,72,0,0,255,
    0,0,255,0,0,99,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,210,0,0,255,
    0,0,253,0,0,229,0,0,13,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,100,0,0,255,128,57,252,
    224,81,251,0,0,255,0,0,129,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,231,0,0,253,192,196,253,
    128,229,254,0,0,253,0,0,244,0,8,28,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,133,0,0,255,192,73,252,96,254,255,
    160,254,255,128,106,251,0,0,255,32,0,158,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,30,0,0,245,0,0,253,32,213,254,96,254,255,
    64,254,255,160,229,254,64,8,253,0,0,250,32,0,45,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,162,0,0,255,128,106,251,160,254,255,32,254,255,
    32,254,255,160,254,255,32,131,252,0,0,255,32,0,183,0,0,0,0,0,6,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,51,0,0,254,96,8,253,160,229,255,96,254,255,32,254,255,
    32,254,255,64,254,255,0,246,255,160,24,252,0,0,255,0,0,68,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,189,0,0,255,64,131,252,160,254,255,32,254,255,64,254,255,
    64,254,255,32,254,255,160,254,255,192,163,253,0,0,254,0,0,210,0,0,112,0,0,31,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,32,0,76,0,0,255,192,32,252,32,246,255,64,254,255,32,254,255,64,254,255,
    64,254,255,64,254,255,32,254,255,96,254,255,32,49,255,0,0,255,0,0,255,0,0,153,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,213,0,0,255,0,164,252,128,254,255,0,254,255,32,254,255,32,254,255,
    32,254,255,32,254,255,32,254,255,160,254,255,96,180,255,0,0,250,0,0,251,0,0,160,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,0,104,0,0,255,64,49,252,160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,
    160,254,255,96,254,255,32,254,255,32,254,255,128,254,255,192,73,251,0,0,255,0,0,160,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,17,
    0,0,234,0,0,253,224,204,253,160,180,255,64,90,255,96,98,255,64,98,255,96,98,255,
    96,98,255,32,213,255,96,254,255,0,254,255,160,254,255,0,164,254,0,0,254,0,0,234,
    0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,137,
    0,0,255,0,82,251,96,254,255,64,8,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,192,65,255,160,254,255,0,246,255,160,254,255,224,155,255,0,0,252,0,0,255,
    0,0,151,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,33,0,0,245,
    32,8,253,128,229,254,160,229,255,96,16,255,0,0,255,32,0,255,0,0,255,32,0,255,
    0,0,255,128,57,255,32,246,255,64,254,255,160,254,255,224,155,255,0,0,254,0,0,253,
    0,0,251,0,0,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,165,0,0,255,
    224,114,251,224,114,255,96,16,255,32,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,32,8,255,128,16,255,128,147,255,224,254,255,224,155,255,0,0,255,32,0,252,
    0,0,255,0,0,176,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,8,53,0,0,255,128,16,253,
    96,221,255,64,8,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,64,49,255,0,255,255,64,139,255,0,0,255,224,81,255,
    0,41,252,0,0,255,32,8,63,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,192,0,0,255,128,147,252,
    64,254,255,96,16,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,160,65,255,160,254,255,128,229,255,224,122,255,160,229,255,
    224,163,252,0,0,255,0,0,201,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,79,0,0,255,224,32,252,64,254,255,
    224,245,255,32,8,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,128,57,255,128,254,255,96,254,255,64,213,255,32,254,255,
    96,254,255,0,41,252,0,0,255,32,8,89,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,215,0,0,254,32,172,253,128,254,255,
    96,254,255,192,147,255,96,16,255,0,0,255,32,0,255,96,57,255,192,65,255,224,32,255,
    0,0,255,32,0,255,0,41,255,192,188,255,192,254,255,64,131,255,0,0,255,192,73,255,
    192,254,255,64,180,253,0,0,254,0,0,220,0,0,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,108,0,0,255,96,57,252,128,254,255,32,254,255,
    32,254,255,160,254,255,224,32,255,0,0,255,0,0,255,128,229,255,32,255,255,64,131,255,
    0,0,255,0,0,255,96,139,255,224,254,255,128,254,255,64,139,255,0,0,255,224,73,255,
    96,254,255,128,254,255,128,65,252,0,0,255,32,0,114,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,18,0,0,236,0,0,254,192,196,253,96,254,255,0,254,255,
    0,254,255,64,254,255,0,164,255,0,41,255,224,122,255,32,246,255,32,246,255,128,221,255,
    128,57,255,128,57,255,128,221,255,32,254,255,32,254,255,64,254,255,192,196,255,224,245,255,
    32,254,255,96,254,255,192,196,253,0,0,253,0,0,237,0,0,20,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,141,0,0,255,160,106,251,32,255,255,160,254,255,160,254,255,
    160,254,255,160,254,255,224,254,255,192,254,255,0,255,255,192,254,255,160,254,255,224,254,255,
    224,254,255,224,254,255,224,254,255,160,254,255,160,254,255,160,254,255,0,255,255,192,254,255,
    160,254,255,160,254,255,32,255,255,160,106,251,0,0,255,32,0,145,0,0,0,0,0,0,
    0,0,0,0,0,31,0,0,243,0,0,249,96,98,250,64,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,0,123,251,32,131,251,0,123,251,32,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,0,123,251,32,131,251,
    32,131,251,32,131,251,64,131,251,96,98,250,0,0,249,0,0,245,0,0,33,0,0,0,
    0,0,0,0,0,183,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,183,0,0,0,
    0,0,22,0,0,157,0,0,158,0,0,159,0,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,0,0,159,0,0,159,0,0,159,0,0,157,0,0,22,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 19 注意行人
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,60,
    0,0,78,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,197,
    0,0,228,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,72,0,0,255,
    0,0,255,0,0,99,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,210,0,0,255,
    0,0,253,0,0,229,0,0,13,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,100,0,0,255,128,57,252,
    224,81,251,0,0,255,0,0,129,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,231,0,0,253,192,196,253,
    128,229,254,0,0,253,0,0,244,0,8,28,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,133,0,0,255,192,73,252,96,254,255,
    160,254,255,128,106,251,0,0,255,32,0,158,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,30,0,0,245,0,0,253,32,213,254,96,254,255,
    96,254,255,160,229,254,64,8,253,0,0,250,32,0,45,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,162,0,0,255,128,106,251,160,254,255,96,254,255,
    224,245,255,160,254,255,96,139,252,0,0,255,32,0,183,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,51,0,0,254,96,8,253,160,229,255,160,254,255,224,155,255,
    128,24,255,96,49,255,64,221,255,224,32,252,0,0,255,0,0,69,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,189,0,0,255,64,131,252,192,254,255,224,245,255,64,8,255,
    0,0,255,0,0,255,224,114,255,64,180,252,0,0,254,0,0,207,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,32,0,76,0,0,255,192,32,252,32,246,255,64,254,255,32,254,255,96,57,255,
    0,0,255,0,0,255,64,139,255,192,254,255,0,41,252,0,0,255,32,8,94,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,213,0,0,255,0,164,252,160,254,255,0,254,255,160,254,255,192,155,255,
    0,0,255,96,98,255,64,254,255,160,254,255,96,180,253,0,0,254,0,0,225,0,0,10,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,0,104,0,0,255,64,49,252,96,254,255,32,254,255,0,254,255,224,254,255,96,98,255,
    0,0,255,0,164,255,224,254,255,0,246,255,128,254,255,160,65,252,0,0,255,32,0,122,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,17,
    0,0,234,0,0,253,160,196,253,128,254,255,32,254,255,192,254,255,192,196,255,128,16,255,
    0,0,255,160,65,255,224,237,255,128,254,255,96,254,255,0,205,254,0,0,253,0,0,240,
    0,8,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,137,
    0,0,255,0,82,251,128,254,255,32,254,255,96,254,255,32,172,255,0,0,255,0,0,255,
    64,8,255,0,0,255,128,16,255,160,188,255,64,254,255,160,254,255,64,98,252,0,0,255,
    32,0,151,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,33,0,0,245,
    32,0,253,64,221,254,96,254,255,96,254,255,0,205,255,0,0,255,224,40,255,0,0,255,
    64,49,255,32,205,255,224,32,255,0,123,255,160,254,255,64,254,255,128,229,254,64,8,253,
    0,0,251,32,0,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,165,0,0,255,
    192,114,251,160,254,255,32,254,255,64,254,255,224,245,255,192,188,255,64,139,255,0,0,255,
    64,90,255,32,255,255,0,254,255,64,254,255,64,254,255,32,254,255,160,254,255,0,123,252,
    0,0,255,32,0,176,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,8,53,0,0,255,96,16,253,
    192,237,255,96,254,255,32,254,255,64,254,255,32,254,255,224,254,255,0,82,255,0,0,255,
    96,16,255,64,172,255,160,254,255,32,254,255,64,254,255,32,254,255,64,254,255,224,237,255,
    128,24,252,0,0,255,32,8,63,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,192,0,0,255,96,139,252,
    160,254,255,32,254,255,64,254,255,32,254,255,96,254,255,160,237,255,64,8,255,224,32,255,
    160,24,255,0,0,255,96,139,255,128,254,255,32,254,255,64,254,255,32,254,255,160,254,255,
    160,155,252,0,0,255,0,0,201,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,79,0,0,255,224,32,252,64,254,255,
    64,254,255,64,254,255,64,254,255,32,254,255,160,254,255,160,147,255,0,0,255,0,164,255,
    192,229,255,32,0,255,128,57,255,128,254,255,32,254,255,64,254,255,64,254,255,64,254,255,
    64,254,255,0,41,252,0,0,255,32,8,89,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,215,0,0,254,32,172,253,160,254,255,
    32,254,255,64,254,255,64,254,255,32,254,255,128,254,255,32,41,255,64,8,255,192,237,255,
    160,254,255,192,32,255,32,41,255,128,254,255,32,254,255,64,254,255,64,254,255,32,254,255,
    160,254,255,96,180,253,0,0,254,0,0,220,0,0,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,108,0,0,255,96,57,252,128,254,255,32,254,255,
    64,254,255,64,254,255,32,254,255,128,254,255,128,188,255,0,0,255,64,90,255,160,254,255,
    96,254,255,96,57,255,32,8,255,224,245,255,64,254,255,32,254,255,64,254,255,64,254,255,
    32,254,255,128,254,255,128,65,252,0,0,255,32,0,114,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,18,0,0,236,0,0,254,192,196,253,96,254,255,0,254,255,
    32,254,255,32,254,255,0,246,255,128,254,255,160,147,255,32,0,255,128,188,255,96,254,255,
    96,254,255,32,90,255,64,8,255,160,229,255,64,254,255,0,254,255,32,254,255,32,254,255,
    0,254,255,96,254,255,192,196,253,0,0,253,0,0,237,0,0,20,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,141,0,0,255,160,106,251,32,255,255,160,254,255,160,254,255,
    160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,0,246,255,192,254,255,160,254,255,
    192,254,255,96,254,255,32,254,255,192,254,255,160,254,255,160,254,255,160,254,255,160,254,255,
    160,254,255,160,254,255,32,255,255,160,106,251,0,0,255,32,0,145,0,0,0,0,0,0,
    0,0,0,0,0,31,0,0,243,0,0,249,96,98,250,64,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,64,139,251,32,131,251,32,131,251,
    32,131,251,32,131,251,64,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,64,131,251,96,98,250,0,0,249,0,0,245,0,0,33,0,0,0,
    0,0,0,0,0,183,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,183,0,0,0,
    0,0,22,0,0,157,0,0,158,0,0,159,0,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,0,0,159,0,0,159,0,0,159,0,0,157,0,0,22,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 20 陡坡
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,60,
    0,0,78,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,197,
    0,0,228,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,72,0,0,255,
    0,0,255,0,0,99,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,210,0,0,255,
    0,0,253,0,0,229,0,0,13,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,100,0,0,255,128,57,252,
    224,81,251,0,0,255,0,0,129,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,231,0,0,253,192,196,253,
    128,229,254,0,0,253,0,0,244,0,8,28,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,133,0,0,255,192,73,252,96,254,255,
    160,254,255,128,106,251,0,0,255,32,0,158,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,30,0,0,245,0,0,253,32,213,254,96,254,255,
    64,254,255,160,229,254,64,8,253,0,0,250,32,0,45,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,162,0,0,255,128,106,251,160,254,255,32,254,255,
    32,254,255,160,254,255,32,131,252,0,0,255,32,0,183,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,51,0,0,254,96,8,253,160,229,255,96,254,255,32,254,255,
    32,254,255,64,254,255,0,246,255,160,24,252,0,0,255,0,0,69,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,189,0,0,255,64,131,252,160,254,255,32,254,255,64,254,255,
    64,254,255,32,254,255,160,254,255,224,163,252,0,0,254,0,0,207,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,32,0,76,0,0,255,192,32,252,32,246,255,64,254,255,32,254,255,64,254,255,
    64,254,255,64,254,255,32,254,255,64,254,255,64,49,252,0,0,255,32,0,94,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,213,0,0,254,0,164,252,160,254,255,32,254,255,64,254,255,64,254,255,
    64,254,255,64,254,255,32,254,255,160,254,255,96,98,253,0,0,254,0,0,225,0,0,10,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,0,108,0,0,255,64,49,252,96,254,255,32,254,255,64,254,255,64,254,255,64,254,255,
    64,254,255,64,254,255,32,254,255,128,254,255,64,90,255,0,0,252,0,0,255,0,0,122,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,21,
    0,0,243,32,0,253,32,213,254,160,254,255,0,254,255,64,254,255,64,254,255,64,254,255,
    64,254,255,64,254,255,32,254,255,192,254,255,64,98,255,0,0,254,64,8,253,0,0,240,
    0,0,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,136,
    0,0,255,32,0,251,160,106,255,0,246,255,160,254,255,32,254,255,32,254,255,64,254,255,
    64,254,255,64,254,255,224,245,255,32,238,255,224,73,255,192,32,255,32,90,252,0,0,255,
    0,0,151,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,33,0,0,245,
    0,0,253,32,0,254,0,41,255,160,24,255,224,155,255,160,254,255,128,254,255,32,254,255,
    32,254,255,64,254,255,224,237,255,32,41,255,32,0,255,32,0,255,192,32,254,96,8,253,
    0,0,251,32,0,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,165,0,0,255,
    0,0,251,0,0,255,160,229,255,96,172,255,160,24,255,96,57,255,32,205,255,192,254,255,
    64,254,255,0,254,255,160,254,255,64,172,255,0,0,255,0,0,255,96,180,255,96,139,252,
    0,0,255,32,0,176,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,53,0,0,255,32,8,253,
    32,8,255,0,0,255,64,213,255,224,254,255,32,246,255,192,114,255,64,8,255,128,98,255,
    0,238,255,160,254,255,32,254,255,160,254,255,0,82,255,96,57,255,192,254,255,224,237,255,
    128,24,252,0,0,255,32,8,63,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,192,0,0,255,0,123,252,
    192,32,255,0,0,255,64,221,255,96,254,255,64,254,255,192,254,255,64,213,255,128,65,255,
    128,16,255,224,155,255,128,254,255,160,254,255,160,229,255,96,229,255,64,254,255,160,254,255,
    160,155,252,0,0,255,0,0,201,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,79,0,0,255,0,41,252,192,237,255,
    96,16,255,0,0,255,64,213,255,96,254,255,32,254,255,0,254,255,96,254,255,128,254,255,
    32,164,255,160,24,255,32,41,255,224,196,255,192,254,255,128,254,255,0,254,255,64,254,255,
    64,254,255,0,41,252,0,0,255,32,8,89,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,215,0,0,254,64,180,253,64,254,255,
    96,16,255,0,0,255,160,229,255,192,254,255,128,254,255,160,254,255,128,254,255,128,254,255,
    0,255,255,128,254,255,32,131,255,160,24,255,192,106,255,224,237,255,192,254,255,64,254,255,
    160,254,255,96,180,253,0,0,254,0,0,220,0,0,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,108,0,0,255,96,57,252,128,254,255,192,237,255,
    0,0,255,0,0,255,224,40,255,64,49,255,32,49,255,32,49,255,32,49,255,32,49,255,
    0,41,255,64,49,255,160,65,255,64,8,255,0,0,255,64,8,255,64,131,255,224,237,255,
    32,254,255,128,254,255,128,65,252,0,0,255,32,0,114,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,18,0,0,236,0,0,254,192,196,253,128,254,255,192,237,255,
    128,98,255,64,90,255,224,81,255,224,73,255,224,73,255,224,73,255,224,73,255,224,73,255,
    224,73,255,224,73,255,192,73,255,32,82,255,64,90,255,32,82,255,224,73,255,160,147,255,
    0,246,255,96,254,255,192,196,253,0,0,253,0,0,237,0,0,20,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,141,0,0,255,160,106,251,32,255,255,128,254,255,192,254,255,
    0,255,255,32,255,255,32,255,255,32,255,255,32,255,255,32,255,255,32,255,255,32,255,255,
    32,255,255,32,255,255,32,255,255,32,255,255,32,255,255,32,255,255,32,255,255,224,254,255,
    160,254,255,160,254,255,32,255,255,160,106,251,0,0,255,32,0,145,0,0,0,0,0,0,
    0,0,0,0,0,31,0,0,243,0,0,249,96,98,250,64,131,251,32,131,251,32,131,251,
    0,123,251,0,123,251,0,123,251,0,123,251,0,123,251,0,123,251,0,123,251,0,123,251,
    0,123,251,0,123,251,0,123,251,0,123,251,0,123,251,0,123,251,0,123,251,32,131,251,
    32,131,251,32,131,251,64,131,251,96,98,250,0,0,249,0,0,245,0,0,33,0,0,0,
    0,0,0,0,0,183,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,183,0,0,0,
    0,0,22,0,0,157,0,0,158,0,0,159,0,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,0,0,159,0,0,159,0,0,159,0,0,157,0,0,22,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 21 村庄
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,60,
    0,0,78,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,197,
    0,0,228,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,72,0,0,255,
    0,0,255,0,0,99,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,210,0,0,255,
    0,0,253,0,0,229,0,0,13,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,100,0,0,255,128,57,252,
    224,81,251,0,0,255,0,0,129,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,231,0,0,253,192,196,253,
    128,229,254,0,0,253,0,0,244,0,8,28,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,133,0,0,255,192,73,252,96,254,255,
    160,254,255,128,106,251,0,0,255,32,0,158,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,30,0,0,245,0,0,253,32,213,254,96,254,255,
    64,254,255,160,229,254,64,8,253,0,0,250,32,0,45,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,162,0,0,255,128,106,251,160,254,255,32,254,255,
    32,254,255,160,254,255,32,131,252,0,0,255,32,0,183,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,51,0,0,254,96,8,253,160,229,255,96,254,255,32,254,255,
    32,254,255,64,254,255,0,246,255,160,24,252,0,0,255,0,0,69,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,189,0,0,255,64,131,252,160,254,255,32,254,255,64,254,255,
    64,254,255,32,254,255,160,254,255,224,163,252,0,0,254,0,0,207,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,32,0,76,0,0,255,192,32,252,0,246,255,64,254,255,64,254,255,64,254,255,
    64,254,255,64,254,255,32,254,255,64,254,255,32,49,252,0,0,255,32,8,94,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,213,0,0,255,0,164,252,224,254,255,32,254,255,32,254,255,64,254,255,
    64,254,255,64,254,255,32,254,255,224,254,255,96,180,253,0,0,254,0,0,225,0,0,10,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,0,104,0,0,255,32,49,252,192,254,255,0,205,255,192,237,255,96,254,255,32,254,255,
    64,254,255,32,254,255,96,254,255,192,196,255,128,254,255,160,65,252,0,0,255,32,0,122,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,17,
    0,0,234,0,0,253,224,196,253,224,229,255,64,8,255,0,82,255,160,254,255,32,254,255,
    32,254,255,160,254,255,128,188,255,0,0,255,192,155,255,128,229,254,0,0,253,0,0,240,
    0,8,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,137,
    0,0,255,32,90,251,128,254,255,0,41,255,0,0,255,0,0,255,0,123,255,160,254,255,
    128,254,255,96,221,255,96,16,255,0,0,255,0,0,255,224,204,255,224,114,252,0,0,255,
    32,0,151,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,33,0,0,245,
    64,8,253,32,221,254,224,81,255,0,0,255,0,0,255,0,0,255,0,0,255,224,155,255,
    0,238,255,224,32,255,0,0,255,0,0,255,0,0,255,128,16,255,64,180,254,128,16,253,
    0,0,251,32,0,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,165,0,0,255,
    224,114,251,224,221,255,224,32,255,0,0,255,32,0,255,32,0,255,0,0,255,64,90,255,
    0,189,255,160,24,255,0,0,255,32,0,255,0,0,255,0,0,255,0,164,255,64,139,252,
    0,0,255,32,0,176,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,8,53,0,0,255,96,16,253,
    160,229,255,224,254,255,128,106,255,0,0,255,0,0,255,0,0,255,0,0,255,0,164,255,
    32,255,255,160,65,255,0,0,255,0,0,255,0,0,255,0,0,255,128,229,255,32,246,255,
    128,16,252,0,0,255,32,8,63,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,192,0,0,255,96,139,252,
    160,254,255,128,254,255,64,90,255,0,0,255,64,98,255,192,73,255,0,0,255,192,155,255,
    224,254,255,96,57,255,96,8,255,160,98,255,0,41,255,0,0,255,32,213,255,224,254,255,
    160,147,252,0,0,255,0,0,201,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,79,0,0,255,224,32,252,64,254,255,
    32,254,255,160,254,255,64,90,255,0,0,255,160,254,255,32,213,255,0,0,255,224,155,255,
    0,255,255,32,41,255,64,49,255,128,255,255,64,131,255,0,0,255,96,221,255,96,254,255,
    64,254,255,0,41,252,0,0,255,32,8,89,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,215,0,0,254,32,172,253,160,254,255,
    0,246,255,160,254,255,64,90,255,0,0,255,32,254,255,192,196,255,0,0,255,224,163,255,
    0,255,255,64,49,255,64,49,255,224,254,255,0,123,255,0,0,255,96,221,255,96,254,255,
    128,254,255,96,180,253,0,0,254,0,0,220,0,0,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,108,0,0,255,96,57,252,128,254,255,32,254,255,
    32,254,255,160,254,255,224,73,255,0,0,255,64,254,255,160,196,255,0,0,255,192,155,255,
    0,255,255,192,32,255,192,32,255,32,255,255,192,114,255,0,0,255,96,221,255,96,254,255,
    32,254,255,128,254,255,128,65,252,0,0,255,32,0,114,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,18,0,0,236,0,0,254,192,196,253,96,254,255,0,254,255,
    0,254,255,96,254,255,64,172,255,0,123,255,0,254,255,64,221,255,0,123,255,32,213,255,
    128,254,255,160,155,255,128,147,255,128,254,255,96,180,255,0,123,255,224,245,255,32,254,255,
    0,246,255,96,254,255,192,196,253,0,0,253,0,0,237,0,0,20,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,141,0,0,255,160,106,251,32,255,255,160,254,255,160,254,255,
    160,254,255,160,254,255,0,255,255,32,255,255,160,254,255,192,254,255,32,255,255,224,254,255,
    160,254,255,0,255,255,0,255,255,160,254,255,224,254,255,32,255,255,192,254,255,160,254,255,
    160,254,255,160,254,255,32,255,255,160,106,251,0,0,255,32,0,145,0,0,0,0,0,0,
    0,0,0,0,0,31,0,0,243,0,0,249,96,98,250,64,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,0,123,251,0,123,251,32,131,251,32,131,251,0,123,251,32,131,251,
    32,131,251,0,123,251,0,123,251,32,131,251,0,123,251,0,123,251,32,131,251,32,131,251,
    32,131,251,32,131,251,64,131,251,96,98,250,0,0,249,0,0,245,0,0,33,0,0,0,
    0,0,0,0,0,183,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,183,0,0,0,
    0,0,22,0,0,157,0,0,158,0,0,159,0,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,0,0,159,0,0,159,0,0,159,0,0,157,0,0,22,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 22 窄桥
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,60,
    0,0,78,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,197,
    0,0,228,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,72,0,0,255,
    0,0,255,0,0,99,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,210,0,0,255,
    0,0,253,0,0,229,0,0,13,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,100,0,0,255,128,57,252,
    224,81,251,0,0,255,0,0,129,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,231,0,0,253,192,196,253,
    128,229,254,0,0,253,0,0,244,0,8,28,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,133,0,0,255,192,73,252,96,254,255,
    160,254,255,128,106,251,0,0,255,32,0,158,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,30,0,0,245,0,0,253,32,213,254,96,254,255,
    64,254,255,160,229,254,64,8,253,0,0,250,32,0,45,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,162,0,0,255,128,106,251,160,254,255,32,254,255,
    32,254,255,160,254,255,32,131,252,0,0,255,32,0,183,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,51,0,0,254,96,8,253,160,229,255,96,254,255,32,254,255,
    32,254,255,64,254,255,224,245,255,160,24,252,0,0,255,0,0,69,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,189,0,0,255,64,131,252,0,255,255,64,254,255,64,254,255,
    64,254,255,32,254,255,0,255,255,224,163,252,0,0,254,0,0,207,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,32,0,76,0,0,255,192,32,252,0,246,255,64,172,255,160,229,255,64,254,255,
    64,254,255,192,237,255,64,172,255,32,254,255,32,49,252,0,0,255,32,8,94,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,213,0,0,255,96,188,252,160,196,255,0,0,255,160,114,255,128,254,255,
    160,254,255,96,139,255,0,0,255,160,188,255,192,196,253,0,0,254,0,0,225,0,0,10,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,0,104,0,0,255,32,49,252,224,254,255,224,155,255,0,0,255,192,155,255,128,254,255,
    160,254,255,32,131,255,0,0,255,128,188,255,224,254,255,160,65,252,0,0,255,32,0,122,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,17,
    0,0,234,0,0,253,160,188,253,0,255,255,224,163,255,0,0,255,0,164,255,128,254,255,
    160,254,255,32,131,255,0,0,255,160,196,255,192,254,255,0,205,254,0,0,253,0,0,240,
    0,8,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,137,
    0,0,255,0,82,251,128,254,255,128,254,255,0,164,255,0,0,255,0,164,255,128,254,255,
    160,254,255,32,131,255,0,0,255,192,196,255,96,254,255,128,254,255,64,98,252,0,0,255,
    32,0,151,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,33,0,0,245,
    32,0,253,64,221,254,96,254,255,128,254,255,224,163,255,0,0,255,0,164,255,128,254,255,
    160,254,255,32,131,255,0,0,255,160,188,255,128,254,255,64,254,255,128,229,254,64,8,253,
    0,0,251,32,0,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,165,0,0,255,
    192,114,251,160,254,255,0,246,255,160,254,255,32,172,255,0,0,255,0,164,255,128,254,255,
    160,254,255,32,131,255,0,0,255,224,204,255,128,254,255,0,254,255,160,254,255,0,123,252,
    0,0,255,32,0,176,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,8,53,0,0,255,96,16,253,
    192,237,255,96,254,255,32,254,255,160,254,255,128,106,255,0,0,255,128,106,255,128,254,255,
    160,254,255,0,123,255,0,0,255,64,131,255,192,254,255,0,254,255,64,254,255,224,237,255,
    128,24,252,0,0,255,32,8,63,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,192,0,0,255,96,139,252,
    160,254,255,0,254,255,160,254,255,224,163,255,0,0,255,192,32,255,32,205,255,96,254,255,
    64,254,255,96,221,255,224,32,255,0,0,255,96,180,255,160,254,255,0,254,255,160,254,255,
    160,155,252,0,0,255,0,0,201,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,79,0,0,255,224,32,252,64,254,255,
    32,254,255,128,254,255,32,205,255,64,8,255,64,8,255,32,213,255,160,254,255,32,254,255,
    32,254,255,160,254,255,96,221,255,64,8,255,96,16,255,96,221,255,128,254,255,32,254,255,
    64,254,255,0,41,252,0,0,255,32,8,89,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,215,0,0,254,32,172,253,128,254,255,
    64,254,255,0,246,255,224,40,255,0,0,255,0,164,255,160,254,255,0,254,255,64,254,255,
    64,254,255,32,254,255,160,254,255,96,180,255,0,0,255,0,41,255,32,246,255,64,254,255,
    128,254,255,96,180,253,0,0,254,0,0,220,0,0,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,108,0,0,255,96,57,252,96,254,255,32,254,255,
    32,254,255,96,57,255,0,0,255,160,114,255,160,254,255,32,254,255,64,254,255,64,254,255,
    64,254,255,64,254,255,32,254,255,192,254,255,64,139,255,0,0,255,96,57,255,32,254,255,
    32,254,255,128,254,255,128,65,252,0,0,255,32,0,114,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,18,0,0,236,0,0,254,192,196,253,96,254,255,0,254,255,
    32,246,255,64,49,255,96,57,255,64,254,255,32,254,255,32,254,255,32,254,255,32,254,255,
    32,254,255,32,254,255,32,254,255,32,254,255,64,254,255,96,57,255,32,49,255,32,246,255,
    0,254,255,96,254,255,192,196,253,0,0,253,0,0,237,0,0,20,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,141,0,0,255,160,106,251,32,255,255,160,254,255,160,254,255,
    192,254,255,160,254,255,160,254,255,192,254,255,160,254,255,160,254,255,160,254,255,160,254,255,
    160,254,255,160,254,255,160,254,255,160,254,255,192,254,255,160,254,255,160,254,255,192,254,255,
    160,254,255,160,254,255,32,255,255,160,106,251,0,0,255,32,0,145,0,0,0,0,0,0,
    0,0,0,0,0,31,0,0,243,0,0,249,96,98,250,64,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,64,131,251,96,98,250,0,0,249,0,0,245,0,0,33,0,0,0,
    0,0,0,0,0,183,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,183,0,0,0,
    0,0,22,0,0,157,0,0,158,0,0,159,0,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,0,0,159,0,0,159,0,0,159,0,0,157,0,0,22,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 23 落石
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,60,
    0,0,78,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,197,
    0,0,228,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,72,0,0,255,
    0,0,255,0,0,99,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,210,0,0,255,
    0,0,253,0,0,229,0,0,13,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,100,0,0,255,128,57,252,
    224,81,251,0,0,255,0,0,129,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,231,0,0,253,192,196,253,
    128,229,254,0,0,253,0,0,244,0,8,28,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,133,0,0,255,192,73,252,96,254,255,
    160,254,255,128,106,251,0,0,255,32,0,158,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,30,0,0,245,0,0,253,32,213,254,96,254,255,
    64,254,255,160,229,254,64,8,253,0,0,250,32,0,45,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,162,0,0,255,128,106,251,160,254,255,32,254,255,
    32,254,255,160,254,255,32,131,252,0,0,255,32,0,183,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,51,0,0,254,96,16,253,128,229,255,96,254,255,32,254,255,
    32,254,255,64,254,255,0,246,255,160,24,252,0,0,255,0,0,69,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,189,0,0,255,0,123,252,64,172,255,64,254,255,32,254,255,
    64,254,255,32,254,255,160,254,255,224,155,252,0,0,254,0,0,207,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,32,0,76,0,0,255,32,49,252,64,172,255,32,82,255,160,254,255,32,254,255,
    32,254,255,64,254,255,128,254,255,96,254,255,32,41,252,0,0,255,32,8,94,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,213,0,0,255,128,188,252,32,82,255,192,73,255,192,254,255,0,254,255,
    128,254,255,192,237,255,160,188,255,128,254,255,128,188,253,0,0,254,0,0,225,0,0,10,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,0,104,0,0,255,128,57,252,192,237,255,0,0,255,64,90,255,160,254,255,64,254,255,
    128,221,255,192,32,255,0,0,255,160,73,255,128,254,255,160,65,252,0,0,255,32,0,122,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,17,
    0,0,234,0,0,253,32,213,253,128,139,255,0,0,255,96,98,255,128,254,255,160,254,255,
    224,122,255,0,0,255,64,8,255,0,0,255,192,204,255,96,221,254,0,0,253,0,0,240,
    0,8,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,137,
    0,0,255,0,82,251,96,254,255,160,24,255,0,0,255,96,98,255,128,254,255,96,254,255,
    192,188,255,0,0,255,0,0,255,160,24,255,160,229,255,192,254,255,64,90,252,0,0,255,
    32,0,151,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,33,0,0,245,
    32,0,253,192,229,254,32,172,255,0,0,255,0,0,255,64,98,255,160,254,255,0,254,255,
    128,254,255,160,196,255,32,131,255,192,237,255,128,254,255,128,254,255,192,237,254,64,8,253,
    0,0,251,32,0,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,165,0,0,255,
    160,114,251,224,254,255,32,49,255,0,0,255,0,0,255,64,98,255,160,254,255,32,254,255,
    32,254,255,96,254,255,0,255,255,64,172,255,32,41,255,192,73,255,0,238,255,64,131,252,
    0,0,255,32,0,176,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,8,53,0,0,255,96,8,253,
    32,246,255,224,204,255,0,0,255,32,0,255,0,0,255,64,98,255,160,254,255,0,254,255,
    64,254,255,96,254,255,32,213,255,0,0,255,0,0,255,0,0,255,192,73,255,32,254,255,
    128,24,252,0,0,255,32,8,63,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,192,0,0,255,96,139,252,
    0,255,255,0,82,255,0,0,255,32,8,255,0,0,255,64,98,255,160,254,255,96,254,255,
    0,246,255,96,254,255,0,205,255,0,0,255,32,0,255,0,0,255,192,32,255,192,254,255,
    160,147,252,0,0,255,0,0,201,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,79,0,0,255,224,32,252,96,254,255,
    128,229,255,32,8,255,0,0,255,32,0,255,0,0,255,64,90,255,224,254,255,0,205,255,
    224,32,255,224,40,255,64,213,255,64,90,255,0,0,255,32,8,255,32,164,255,128,254,255,
    32,254,255,0,41,252,0,0,255,32,8,89,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,215,0,0,254,32,172,253,32,255,255,
    0,123,255,0,0,255,32,0,255,32,0,255,0,0,255,64,90,255,32,255,255,32,90,255,
    0,0,255,0,0,255,64,98,255,192,254,255,192,196,255,96,221,255,128,254,255,32,254,255,
    160,254,255,96,180,253,0,0,254,0,0,220,0,0,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,108,0,0,255,96,57,252,128,254,255,224,237,255,
    96,16,255,0,0,255,0,0,255,0,0,255,0,0,255,32,82,255,0,255,255,32,172,255,
    0,0,255,0,0,255,64,172,255,128,254,255,96,254,255,96,254,255,32,254,255,64,254,255,
    32,254,255,128,254,255,128,65,252,0,0,255,32,0,114,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,18,0,0,236,0,0,254,160,196,253,192,254,255,32,172,255,
    192,32,255,224,32,255,192,32,255,224,32,255,192,32,255,192,114,255,96,254,255,64,254,255,
    0,205,255,0,205,255,96,254,255,32,254,255,0,254,255,0,254,255,32,254,255,32,254,255,
    0,254,255,96,254,255,192,196,253,0,0,253,0,0,237,0,0,20,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,141,0,0,255,160,106,251,32,255,255,160,254,255,128,254,255,
    160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,
    224,254,255,224,254,255,160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,
    160,254,255,160,254,255,32,255,255,160,106,251,0,0,255,32,0,145,0,0,0,0,0,0,
    0,0,0,0,0,31,0,0,243,0,0,249,96,98,250,64,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,
    0,123,251,0,123,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,64,131,251,96,98,250,0,0,249,0,0,245,0,0,33,0,0,0,
    0,0,0,0,0,183,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,183,0,0,0,
    0,0,22,0,0,157,0,0,158,0,0,159,0,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,0,0,159,0,0,159,0,0,159,0,0,157,0,0,22,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 24 隧道
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,60,
    0,0,78,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,197,
    0,0,228,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,72,0,0,255,
    0,0,255,0,0,99,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,210,0,0,255,
    0,0,253,0,0,229,0,0,13,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,100,0,0,255,128,57,252,
    224,81,251,0,0,255,0,0,129,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,231,0,0,253,192,196,253,
    128,229,254,0,0,253,0,0,244,0,8,28,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,133,0,0,255,192,73,252,96,254,255,
    160,254,255,128,106,251,0,0,255,32,0,158,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,30,0,0,245,0,0,253,32,213,254,96,254,255,
    64,254,255,160,229,254,64,8,253,0,0,250,32,0,45,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,162,0,0,255,128,106,251,160,254,255,32,254,255,
    32,254,255,160,254,255,32,131,252,0,0,255,32,0,183,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,51,0,0,254,96,8,253,160,229,255,96,254,255,32,254,255,
    32,254,255,64,254,255,0,246,255,160,24,252,0,0,255,0,0,69,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,189,0,0,255,64,131,252,160,254,255,32,254,255,64,254,255,
    64,254,255,32,254,255,160,254,255,224,163,252,0,0,254,0,0,207,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,32,0,76,0,0,255,192,32,252,32,246,255,64,254,255,32,254,255,64,254,255,
    64,254,255,64,254,255,32,254,255,96,254,255,32,49,252,0,0,255,32,8,94,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,213,0,0,255,0,164,252,160,254,255,0,254,255,32,254,255,64,254,255,
    64,254,255,32,254,255,0,254,255,128,254,255,96,180,253,0,0,254,0,0,225,0,0,10,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,0,104,0,0,255,64,49,252,64,254,255,64,254,255,160,254,255,160,254,255,64,254,255,
    64,254,255,160,254,255,160,254,255,64,254,255,96,254,255,160,65,252,0,0,255,32,0,122,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,17,
    0,0,234,0,0,253,160,188,253,0,255,255,224,245,255,0,164,255,96,98,255,0,41,255,
    0,41,255,96,98,255,0,164,255,224,245,255,224,254,255,224,204,254,0,0,253,0,0,240,
    0,8,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,137,
    0,0,255,224,81,251,224,254,255,32,164,255,0,41,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,41,255,32,164,255,0,255,255,64,90,252,0,0,255,
    32,0,151,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,33,0,0,245,
    0,0,253,160,237,254,224,155,255,0,0,255,0,0,255,0,41,255,32,123,255,96,180,255,
    128,180,255,32,123,255,0,41,255,0,0,255,0,0,255,224,155,255,224,245,254,32,8,253,
    0,0,251,32,0,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,165,0,0,255,
    0,123,251,128,229,255,0,0,255,0,0,255,0,123,255,32,246,255,32,255,255,32,123,255,
    0,123,255,32,255,255,32,246,255,0,123,255,0,0,255,0,0,255,128,229,255,96,139,252,
    0,0,255,32,0,176,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,8,53,0,0,255,96,16,253,
    0,246,255,192,73,255,0,0,255,32,90,255,192,254,255,32,254,255,160,254,255,96,98,255,
    160,65,255,160,254,255,32,254,255,192,254,255,32,90,255,0,0,255,192,73,255,32,254,255,
    128,24,252,0,0,255,32,8,63,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,192,0,0,255,160,155,252,
    192,237,255,0,0,255,32,0,255,96,221,255,96,254,255,0,254,255,192,254,255,160,114,255,
    224,73,255,192,254,255,0,254,255,96,254,255,96,221,255,32,0,255,0,0,255,192,237,255,
    224,163,252,0,0,255,0,0,201,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,79,0,0,255,192,32,252,160,254,255,
    224,155,255,0,0,255,96,57,255,96,254,255,32,254,255,32,254,255,192,254,255,160,106,255,
    224,73,255,192,254,255,32,254,255,32,254,255,96,254,255,96,57,255,0,0,255,224,155,255,
    160,254,255,224,32,252,0,0,255,32,8,89,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,215,0,0,254,32,172,253,0,255,255,
    32,131,255,0,0,255,96,98,255,160,254,255,32,254,255,32,254,255,192,254,255,160,106,255,
    224,73,255,192,254,255,32,254,255,32,254,255,160,254,255,128,106,255,0,0,255,0,123,255,
    0,255,255,64,180,253,0,0,254,0,0,220,0,0,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,108,0,0,255,96,57,252,96,254,255,160,254,255,
    0,164,255,0,0,255,96,98,255,160,254,255,32,254,255,32,254,255,192,254,255,128,106,255,
    192,73,255,192,254,255,32,254,255,32,254,255,160,254,255,64,131,255,0,0,255,32,131,255,
    160,254,255,96,254,255,128,65,252,0,0,255,32,0,114,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,18,0,0,236,0,0,254,192,196,253,64,254,255,128,254,255,
    64,139,255,0,0,255,64,90,255,128,254,255,0,246,255,0,246,255,160,254,255,64,90,255,
    32,90,255,160,254,255,0,246,255,0,246,255,128,254,255,128,98,255,0,0,255,32,131,255,
    128,254,255,64,254,255,192,196,253,0,0,253,0,0,237,0,0,20,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,141,0,0,255,160,106,251,32,255,255,128,254,255,192,254,255,
    0,246,255,32,131,255,160,229,255,224,254,255,160,254,255,160,254,255,192,254,255,96,254,255,
    96,254,255,192,254,255,160,254,255,160,254,255,224,254,255,128,229,255,32,131,255,0,246,255,
    192,254,255,128,254,255,32,255,255,160,106,251,0,0,255,32,0,145,0,0,0,0,0,0,
    0,0,0,0,0,31,0,0,243,0,0,249,96,98,250,64,131,251,32,131,251,32,131,251,
    64,131,251,128,147,251,64,139,251,32,131,251,32,131,251,32,131,251,32,131,251,64,131,251,
    32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,64,139,251,128,147,251,64,131,251,
    32,131,251,32,131,251,64,131,251,96,98,250,0,0,249,0,0,245,0,0,33,0,0,0,
    0,0,0,0,0,183,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,183,0,0,0,
    0,0,22,0,0,157,0,0,158,0,0,159,0,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,0,0,159,0,0,159,0,0,159,0,0,157,0,0,22,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
  { // 25 注意
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,60,
    0,0,78,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,197,
    0,0,228,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,72,0,0,255,
    0,0,255,0,0,99,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,210,0,0,255,
    0,0,253,0,0,229,0,0,13,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,100,0,0,255,128,57,252,
    224,81,251,0,0,255,0,0,129,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,15,0,0,231,0,0,253,192,196,253,
    128,229,254,0,0,253,0,0,244,0,8,28,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,133,0,0,255,192,73,252,96,254,255,
    160,254,255,128,106,251,0,0,255,32,0,158,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,8,30,0,0,245,32,0,253,32,213,254,96,254,255,
    64,254,255,160,229,254,64,8,253,0,0,250,32,0,45,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,162,0,0,255,128,106,251,192,254,255,128,254,255,
    128,254,255,192,254,255,32,131,252,0,0,255,32,0,183,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,51,0,0,254,96,8,253,192,229,255,32,246,255,64,98,255,
    96,98,255,32,246,255,32,246,255,160,24,252,0,0,255,0,0,69,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,189,0,0,255,32,131,252,32,255,255,0,164,255,0,0,255,
    0,0,255,224,163,255,0,255,255,192,155,252,0,0,254,0,0,207,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,32,0,76,0,0,255,192,32,252,0,246,255,128,254,255,0,205,255,32,0,255,
    0,0,255,224,155,255,160,254,255,64,254,255,32,49,252,0,0,255,32,8,94,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,213,0,0,255,0,164,252,160,254,255,64,254,255,64,221,255,0,0,255,
    0,0,255,224,163,255,128,254,255,128,254,255,96,180,253,0,0,254,0,0,225,0,0,10,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    32,0,104,0,0,255,64,49,252,96,254,255,32,254,255,96,254,255,64,221,255,0,0,255,
    0,0,255,224,163,255,160,254,255,0,254,255,128,254,255,160,65,252,0,0,255,32,0,122,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,17,
    0,0,234,0,0,253,160,196,253,128,254,255,32,254,255,96,254,255,64,221,255,0,0,255,
    0,0,255,224,163,255,160,254,255,0,254,255,128,254,255,0,205,254,0,0,253,0,0,240,
    0,8,24,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,137,
    0,0,255,0,82,251,160,254,255,32,254,255,32,254,255,96,254,255,64,221,255,0,0,255,
    0,0,255,224,163,255,160,254,255,32,254,255,32,254,255,160,254,255,64,98,252,0,0,255,
    32,0,151,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,33,0,0,245,
    32,0,253,64,221,254,96,254,255,32,254,255,32,254,255,96,254,255,64,221,255,0,0,255,
    0,0,255,224,163,255,160,254,255,32,254,255,32,254,255,96,254,255,128,229,254,64,8,253,
    0,0,251,32,0,40,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,0,165,0,0,255,
    192,114,251,160,254,255,32,254,255,64,254,255,32,254,255,96,254,255,64,221,255,32,0,255,
    0,0,255,224,163,255,160,254,255,32,254,255,64,254,255,32,254,255,160,254,255,0,123,252,
    0,0,255,32,0,176,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,32,8,53,0,0,255,96,16,253,
    192,237,255,96,254,255,32,254,255,64,254,255,32,254,255,160,254,255,0,164,255,0,0,255,
    0,0,255,192,155,255,160,254,255,32,254,255,64,254,255,32,254,255,64,254,255,224,237,255,
    128,24,252,0,0,255,32,8,63,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,192,0,0,255,96,139,252,
    160,254,255,32,254,255,64,254,255,64,254,255,32,254,255,96,254,255,128,221,255,96,57,255,
    96,57,255,128,229,255,96,254,255,32,254,255,64,254,255,64,254,255,32,254,255,160,254,255,
    160,155,252,0,0,255,0,0,201,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,32,0,79,0,0,255,224,32,252,64,254,255,
    64,254,255,64,254,255,64,254,255,64,254,255,64,254,255,32,254,255,160,254,255,128,188,255,
    64,172,255,128,254,255,32,254,255,64,254,255,64,254,255,64,254,255,64,254,255,64,254,255,
    64,254,255,0,41,252,0,0,255,32,8,89,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,215,0,0,254,32,172,253,160,254,255,
    32,254,255,64,254,255,64,254,255,64,254,255,32,254,255,128,254,255,64,172,255,0,0,255,
    0,0,255,128,147,255,160,254,255,32,254,255,64,254,255,64,254,255,64,254,255,32,254,255,
    160,254,255,96,180,253,0,0,254,0,0,220,0,0,8,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,32,0,108,0,0,255,96,57,252,128,254,255,32,254,255,
    64,254,255,64,254,255,64,254,255,64,254,255,32,254,255,160,254,255,64,139,255,0,0,255,
    0,0,255,128,98,255,160,254,255,32,254,255,64,254,255,64,254,255,64,254,255,64,254,255,
    32,254,255,128,254,255,128,65,252,0,0,255,32,0,114,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,18,0,0,236,0,0,254,192,196,253,96,254,255,0,254,255,
    32,254,255,32,254,255,32,254,255,32,254,255,32,254,255,32,254,255,224,237,255,64,90,255,
    0,82,255,160,229,255,64,254,255,32,254,255,32,254,255,32,254,255,32,254,255,32,254,255,
    0,254,255,96,254,255,192,196,253,0,0,253,0,0,237,0,0,20,0,0,0,0,0,0,
    0,0,0,0,0,0,32,0,141,0,0,255,160,106,251,32,255,255,160,254,255,160,254,255,
    160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,192,254,255,0,255,255,
    0,255,255,192,254,255,160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,160,254,255,
    160,254,255,160,254,255,32,255,255,160,106,251,0,0,255,32,0,145,0,0,0,0,0,0,
    0,0,0,0,0,31,0,0,243,0,0,249,96,98,250,64,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,0,123,251,
    0,123,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,32,131,251,
    32,131,251,32,131,251,64,131,251,96,98,250,0,0,249,0,0,245,0,0,33,0,0,0,
    0,0,0,0,0,183,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,
    0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,255,0,0,183,0,0,0,
    0,0,22,0,0,157,0,0,158,0,0,159,0,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,32,0,159,
    32,0,159,32,0,159,32,0,159,0,0,159,0,0,159,0,0,159,0,0,157,0,0,22,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  },
};
#define CAMM_S 20
const uint8_t CAM_MARK[CAMM_S * CAMM_S * 3] = {
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,23,
  0,0,68,0,0,98,0,0,100,0,0,70,0,0,26,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,58,0,0,167,0,0,233,133,0,255,232,8,255,232,8,255,133,0,255,
  0,0,236,0,0,174,0,0,67,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,140,33,0,255,175,17,255,218,34,255,
  94,43,254,127,43,253,127,43,253,94,43,254,218,34,255,175,17,255,33,0,255,0,0,154,
  45,1,7,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,173,
  166,8,255,250,34,253,159,43,252,126,43,254,94,43,255,93,43,255,93,43,255,94,43,255,
  94,43,254,159,43,252,250,34,252,166,8,255,0,0,189,45,1,7,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,140,166,8,255,94,43,249,127,43,255,61,35,255,253,26,255,
  253,26,255,253,26,255,62,35,255,94,43,255,93,43,255,61,43,255,127,43,255,94,43,249,
  166,8,255,0,0,160,0,0,0,0,0,0,0,0,0,32,0,54,33,0,255,251,34,251,
  127,43,254,61,35,255,94,43,255,31,166,255,127,190,255,127,182,255,30,76,255,253,26,255,
  62,35,255,30,35,255,61,43,255,127,43,255,251,34,250,33,0,255,0,0,71,0,0,0,
  0,0,0,0,0,170,142,17,254,159,43,251,61,35,255,30,27,255,62,84,255,255,255,255,
  255,255,255,255,255,255,191,149,255,94,43,255,222,67,255,254,67,255,62,35,255,29,35,255,
  159,43,252,142,17,254,0,0,189,0,0,0,1,0,22,0,0,237,217,34,255,126,43,254,
  61,35,255,63,174,255,223,247,255,223,255,255,255,255,255,127,231,255,95,231,255,255,255,255,
  159,239,255,223,206,255,127,190,255,93,43,255,94,43,255,217,34,255,0,0,246,0,0,36,
  0,0,69,132,0,255,94,43,254,61,35,255,222,67,255,255,255,255,255,255,255,255,255,255,
  94,133,255,62,35,255,62,35,255,158,141,255,31,215,255,30,76,255,255,255,255,30,76,255,
  29,27,255,94,43,253,132,0,255,0,0,89,0,0,100,232,8,255,127,43,252,29,35,255,
  222,67,255,223,247,255,255,255,255,127,182,255,253,26,255,127,190,255,127,182,255,253,26,255,
  95,182,255,191,247,255,255,255,255,30,76,255,29,27,255,159,43,252,232,8,255,0,0,121,
  0,0,101,232,8,255,127,43,252,29,35,255,222,67,255,223,247,255,255,255,255,94,141,255,
  190,59,255,255,255,255,255,255,255,190,59,255,126,141,255,255,255,255,255,255,255,30,76,255,
  29,27,255,159,43,252,232,8,255,0,0,122,0,0,72,132,0,255,94,43,254,29,35,255,
  222,67,255,223,247,255,255,255,255,255,214,255,253,26,255,222,116,255,222,116,255,253,26,255,
  255,214,255,255,255,255,255,255,255,62,84,255,29,27,255,94,43,253,132,0,255,0,0,93,
  1,0,25,0,0,240,217,34,255,94,35,254,190,59,255,255,255,255,255,255,255,255,255,255,
  223,198,255,94,84,255,94,84,255,223,198,255,255,255,255,255,255,255,255,255,255,254,67,255,
  62,35,255,217,34,255,0,0,248,0,0,40,0,0,0,0,0,178,174,17,254,159,43,251,
  29,35,255,94,133,255,31,215,255,31,215,255,95,231,255,95,231,255,95,231,255,95,231,255,
  31,215,255,31,215,255,126,141,255,29,35,255,159,43,252,174,17,254,0,0,196,0,0,0,
  0,0,0,32,0,63,33,0,255,251,34,251,127,43,255,253,26,255,62,35,255,62,35,255,
  62,35,255,62,35,255,62,35,255,62,35,255,62,35,255,62,35,255,253,26,255,127,43,255,
  251,34,250,33,0,255,0,0,80,0,0,0,0,0,0,0,0,0,0,0,155,166,8,255,
  94,43,249,127,43,255,61,43,255,93,43,255,94,43,255,94,43,255,94,43,255,94,43,255,
  93,43,255,61,43,255,127,43,255,94,43,249,166,8,255,0,0,173,0,0,0,0,0,0,
  0,0,0,0,0,0,79,1,6,0,0,189,166,8,255,250,34,252,159,43,252,94,43,254,
  94,43,255,61,43,255,61,43,255,94,43,255,94,43,254,159,43,252,251,34,251,166,8,255,
  0,0,203,135,0,13,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,235,0,8,
  0,0,158,33,0,255,175,17,255,218,34,255,94,43,253,127,43,252,127,43,252,94,43,253,
  218,34,255,175,17,255,33,0,255,0,0,171,134,0,15,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,74,0,0,185,0,0,243,
  133,0,255,232,8,255,232,8,255,133,0,255,0,0,246,0,0,191,0,0,84,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,37,0,0,88,0,0,119,0,0,121,0,0,90,
  0,0,41,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
};

#define CAMI_SPEED 2          // 测速：有限速值时画成带数字的限速牌


// ================= NavMap 段 14（源 MotoNav.cpp 8271-8704 行） =================

// 把一张 n×n 的图标（每像素 RGB565 + 透明度）画在 (中心 x, 中心 y)，按透明度和地图混合
void blitRGBA(const uint8_t *src, int n, int cx, int cy) {
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;
  int x0 = cx - n / 2, y0 = cy - n / 2;
  for (int j = 0; j < n; j++) {
    int y = y0 + j;
    if (y < 0 || y >= LCD_HEIGHT) continue;
    uint16_t *row = fb + y * LCD_WIDTH;
    const uint8_t *p = src + j * n * 3;
    for (int i = 0; i < n; i++, p += 3) {
      uint8_t a = p[2];
      if (!a) continue;
      int x = x0 + i;
      if (x < 0 || x >= LCD_WIDTH) continue;
      uint16_t c = p[0] | (p[1] << 8);
      row[x] = a >= 250 ? c : blend565(row[x], c, a);
    }
  }
}
void blitCamIcon(int id, int cx, int cy) {
  if (id < 0 || id >= CAMI_N) id = 0;
  blitRGBA(CAM_ICONS[id], CAMI_S, cx, cy);
}

// 画一个电子眼：测速且有限速值 = 红圈限速牌（数字是实时的）；其他 = 对应类型的图标
void drawOneCam(int i, float x, float y) {
  if (mapCamTyp[i] == CAMI_SPEED && mapCamSpd[i] > 0) {
    aaDisc(x, y, 15, C_BLACK);
    aaDisc(x, y, 13.6f, C_SIGNRED);
    aaDisc(x, y, 10.2f, C_WHITE);
    String t = String(mapCamSpd[i]);
    printCentered(t, (int)x + (t.length() == 3 ? 0 : 1), (int)y - 3, 1, C_BLACK);
  } else {
    blitCamIcon(mapCamTyp[i], (int)lroundf(x), (int)lroundf(y));
  }
}

static float angDiff(float a, float b) {
  float d = a - b;
  while (d > 180) d -= 360;
  while (d < -180) d += 360;
  return fabsf(d);
}

// v4.1 世界坐标 (wx, wy) 离导航路线最近多远（米）；ux, uy 返回最近那一段路的方向（单位向量）。
//      只看外框在 maxD 以内的路段组，算得很快；没有路线返回很大的数
float routeDistW(float wx, float wy, float maxD, float &ux, float &uy) {
  float best = 1e9f;
  ux = 0; uy = 1;
  if (routeN < 2 || routeFrame != posFrame) return best;
  for (int bi = 0; bi * RB_STEP < routeN - 1; bi++) {
    const int32_t *bb = rbBox + 4 * bi;
    if (wx < bb[0] - maxD || wx > bb[2] + maxD || wy < bb[1] - maxD || wy > bb[3] + maxD) continue;
    int s0 = bi * RB_STEP, e0 = min(routeN - 1, s0 + RB_STEP);
    for (int k = s0; k < e0; k++) {
      float ax = routeX[k], ay = routeY[k], dx = routeX[k + 1] - ax, dy = routeY[k + 1] - ay;
      float l2 = dx * dx + dy * dy;
      float t = l2 > 0 ? ((wx - ax) * dx + (wy - ay) * dy) / l2 : 0;
      t = constrain(t, 0.0f, 1.0f);
      float ex = ax + t * dx - wx, ey = ay + t * dy - wy;
      float d = sqrtf(ex * ex + ey * ey);
      if (d < best && l2 > 0) { best = d; float l = sqrtf(l2); ux = dx / l; uy = dy / l; }
    }
  }
  return best;
}

// v3.8 → v4.1 画所有电子眼：
//   1. 路上（电子眼的实际位置）画一个蓝色小摄像头；挨得很近的几个只画一个
//   2. 类型图标（测速牌、闯红灯、违章拍照……）不放在路上，而是摆在小摄像头的旁边：
//      优先垂直于道路的两侧，图标之间、图标和路线/红绿灯/车速等都不重叠；普通监控只有路上的小摄像头
//   3. 位置选好就锁住不动；转了弯（地图朝向和锁定时差 15 度以上）并且已经转稳了，才重新选一次
void drawCamIcons(float cx, float cy, float sc, float cs, float sn) {
  bool turning = angDiff(viewTargetHdg, viewHdg) > 4 || fabsf(hdgRate) > 8;   // 地图还在跟着转（v6.7 含正在转弯）
  const float MKR = 10, R = 15;                                           // 小摄像头半径、类型图标半径
  const float D = MKR + 3 + R;                                            // 类型图标离小摄像头中心的距离（边缘相隔 3 像素）
  // ---- 1. 路上的小摄像头 ----
  float mkX[MCAM_MAX], mkY[MCAM_MAX];
  int mkN = 0, camMk[MCAM_MAX];
  for (int i = 0; i < mapCamN && i < MCAM_MAX; i++) {
    camMk[i] = -1;
    if (!camShown[i]) continue;
    for (int q = 0; q < mkN; q++)
      if (fabsf(mkX[q] - camPx[i]) < 9 && fabsf(mkY[q] - camPy[i]) < 9) { camMk[i] = q; break; }
    if (camMk[i] < 0) { mkX[mkN] = camPx[i]; mkY[mkN] = camPy[i]; camMk[i] = mkN++; }
  }
  // v4.7 小摄像头和类型图标都是圆的：彼此之间按圆来判断重叠（以前按方框算，斜着放时会误判成“挡住了”，
  //      结果图标被推到远处、拉一根长线）
  float ciX[3 * MCAM_MAX], ciY[3 * MCAM_MAX], ciR[3 * MCAM_MAX];
  int ciN = 0;
  for (int q = 0; q < mkN; q++) {
    bool cam = false;                                                    // v4.8 这一处有没有电子眼（道路安全提示不算）
    for (int i = 0; i < mapCamN && i < MCAM_MAX; i++) if (camMk[i] == q && mapCamTyp[i] < 16) cam = true;
    if (cam) {
      blitRGBA(CAM_MARK, CAMM_S, (int)lroundf(mkX[q]), (int)lroundf(mkY[q]));
      ciX[ciN] = mkX[q]; ciY[ciN] = mkY[q]; ciR[ciN] = MKR; ciN++;
    } else {                                                             // 只有安全提示：路上画个小黄点
      aaDisc(mkX[q], mkY[q], 4.5f, C_BLACK);
      aaDisc(mkX[q], mkY[q], 3, 0xFE40);
      ciX[ciN] = mkX[q]; ciY[ciN] = mkY[q]; ciR[ciN] = 5; ciN++;
    }
  }
  // ---- 2. 旁边的类型图标：v4.3 同一处的几个排成一列（和道路平行），整列放在道路的一侧 ----
  int clN[MCAM_MAX], clCam[MCAM_MAX][MCAM_MAX];
  for (int q = 0; q < mkN; q++) clN[q] = 0;
  for (int i = 0; i < mapCamN && i < MCAM_MAX; i++) {
    if (camMk[i] < 0) continue;
    int t = mapCamTyp[i];
    if (t == 0 || t == 1) continue;                                      // 普通监控：路上的小摄像头就够了
    int q = camMk[i];
    bool dup = false;                                                    // 同一处同一种只标一次
    for (int j = 0; j < clN[q]; j++) if (mapCamTyp[clCam[q][j]] == t) { dup = true; break; }
    if (!dup) clCam[q][clN[q]++] = i;
  }
  // 每一列里按类型排序，顺序固定
  for (int q = 0; q < mkN; q++)
    for (int a = 0; a < clN[q]; a++)
      for (int b = a + 1; b < clN[q]; b++)
        if (mapCamTyp[clCam[q][b]] < mapCamTyp[clCam[q][a]]) { int t = clCam[q][a]; clCam[q][a] = clCam[q][b]; clCam[q][b] = t; }
  // 锁住：这一处的每个电子眼都放过、个数没变、地图没转过弯
  bool keep[MCAM_MAX];
  int order[MCAM_MAX], n = 0;
  for (int q = 0; q < mkN; q++) {
    if (clN[q] == 0) continue;
    bool k = true;
    for (int j = 0; j < clN[q]; j++) {
      int i = clCam[q][j];
      if (mapCamPref[i] <= 0 || mapCamPrefK[i] != clN[q] || !(turning || angDiff(viewHdg, mapCamLockH[i]) < 15)) k = false;
    }
    keep[q] = k;
    order[n++] = q;
  }
  for (int a = 0; a < n; a++)                                            // 锁住的在前，再按离车头的距离
    for (int b = a + 1; b < n; b++) {
      int qa = order[a], qb = order[b];
      float da = fabsf(mkX[qa] - CX) + fabsf(mkY[qa] - MAP_PY) + (keep[qa] ? 0 : 10000);
      float db = fabsf(mkX[qb] - CX) + fabsf(mkY[qb] - MAP_PY) + (keep[qb] ? 0 : 10000);
      if (db < da) { order[a] = qb; order[b] = qa; }
    }
  const float GAP = 2 * R + 3;                                           // 一列里相邻两个图标的间距
  for (int o = 0; o < n; o++) {
    int q = order[o], cnt = clN[q], i0 = clCam[q][0];
    float ax = mkX[q], ay = mkY[q];
    float A = mapCamPrefA[i0];
    int lvl = mapCamPref[i0];
    if (!keep[q]) {
      // 道路在屏幕上的方向（没有路线就当作竖直向上）
      float sdx = 0, sdy = -1, ux, uy;
      if (routeDistW(mapCamX[i0], mapCamY[i0], 60, ux, uy) < 60) { sdx = ux * cs - uy * sn; sdy = -(ux * sn + uy * cs); }
      float perp = atan2f(sdy, sdx) + PI / 2;
      // 候选：道路右侧/左侧，先贴近，再远一列
      long bestScore = 0x7FFFFFFF; float bestA = perp; int bestL = 1;
      for (int c = 0; c < 4; c++) {
        float cA = perp + ((c & 1) ? PI : 0);
        int cL = c < 2 ? 1 : 2;
        float d = cL == 1 ? D : D + GAP;
        float px = cosf(cA), py = sinf(cA), tx = -py, ty = px;
        long score = 0;
        for (int j = 0; j < cnt; j++) {
          float off = (j - (cnt - 1) / 2.0f) * GAP;
          float x = ax + px * d + tx * off, y = ay + py * d + ty * off;
          float ex = x - CX, ey = y - CX;
          if (ex * ex + ey * ey > 186.0f * 186.0f) score += 100000;      // 要整个在雷达里
          score += obsOverlap((int)(x - R - 2), (int)(y - R - 2), (int)(x + R + 2), (int)(y + R + 2));
          for (int m = 0; m < ciN; m++) {                                // 和小摄像头、别的图标：按圆算
            float ddx = x - ciX[m], ddy = y - ciY[m], need = R + ciR[m] + 2;
            float dd2 = ddx * ddx + ddy * ddy;
            if (dd2 < need * need) score += (long)((need - sqrtf(dd2)) * 60) + 1;
          }
          float sx = x - CX, sy = MAP_PY - y, rux, ruy;
          float wx = cx + (sx * cs + sy * sn) / sc, wy = cy + (-sx * sn + sy * cs) / sc;
          if (routeDistW(wx, wy, (R + 4) / sc, rux, ruy) < (R + 4) / sc) score += 600;   // 别压在路线上
        }
        if (score < bestScore) { bestScore = score; bestA = cA; bestL = cL; }
        if (score == 0) break;
      }
      A = bestA; lvl = bestL;
      for (int j = 0; j < cnt; j++) {                                    // 整列一起锁住
        int i = clCam[q][j];
        mapCamPref[i] = lvl; mapCamPrefA[i] = A; mapCamPrefK[i] = cnt; mapCamLockH[i] = viewHdg;
      }
    }
    float d = lvl == 2 ? D + GAP : D;
    float px = cosf(A), py = sinf(A), tx = -py, ty = px;
    if (lvl == 2) {                                                      // 远一列时，细线连到小摄像头
      gfx->drawLine((int)(ax + px * (MKR + 1)), (int)(ay + py * (MKR + 1)), (int)(ax + px * (d - R)), (int)(ay + py * (d - R)), C_BLACK);
    }
    for (int j = 0; j < cnt; j++) {
      float off = (j - (cnt - 1) / 2.0f) * GAP;
      float x = ax + px * d + tx * off, y = ay + py * d + ty * off;
      drawOneCam(clCam[q][j], x, y);
      if (ciN < 3 * MCAM_MAX) { ciX[ciN] = x; ciY[ciN] = y; ciR[ciN] = R; ciN++; }
    }
  }
}

// v3.2 红绿灯直接画在地图上的实际位置（图标始终竖直）；已经开过的不画；
//      下一个红绿灯在 300 米内时，旁边标出距离
void drawMapLights(float cx, float cy, float sc, float cs, float sn) {
  if (lightN == 0 || routeFrame != posFrame) return;
  bool first = true;
  for (int i = 0; i < lightN; i++) {
    if (routeProg >= 0 && lightU[i] <= routeProg + 0.02f) continue;     // 已经过了
    float dx = lightX[i] - cx, dy = lightY[i] - cy;
    float px = CX + sc * (dx * cs - dy * sn), py = MAP_PY - sc * (dx * sn + dy * cs);
    bool isNext = first;
    first = false;
    float ex = px - CX, ey = py - CX;
    if (ex * ex + ey * ey > 196.0f * 196.0f) continue;                  // 雷达外面不画
    drawTrafficLight(px, py - 17, 30);
    obsAdd(px - 9, py - 34, px + 9, py);                                // v3.8 电子眼图标别压住红绿灯
    if (isNext && lightCdSec >= 0 && millis() - lightCdAt < 5000) {     // v7.13.2 红绿灯读秒（灯图标上方）
      char cb[6]; snprintf(cb, sizeof(cb), "%d", (int)lightCdSec);
      printOutlined(cb, (int)px, (int)py - 52, 3, lightCdPhase ? C_GREEN : C_RED);
      obsAdd(px - 26, py - 72, px + 26, py - 38);
    }
    if (isNext && lightDist >= 0) {
      String t = String(lightDist) + "m";
      printOutlined(t, (int)px + 36, (int)py - 25, 2, C_WHITE);
      float tw = t.length() * 12;
      obsAdd(px + 36 - tw / 2 - 3, py - 27, px + 36 + tw / 2 + 3, py - 7);
    }
  }
}

// 转向箭头 + 距离（浮在底部）；离路口 150 米内变大，车道放在半透明深色底上
void drawMapBadge() {
  if (cruiseOn) {                                      // v4.8 巡航：底部只写个“巡航”标记
    printOutlined("CRUISE", CX, 392, 2, C_CYAN);
    return;
  }
  bool isS = navDir == "S";
  bool far = !isS && navDist > STAGE_FAR_M;           // v2.4 远处：先显示直行
  bool nearJ = !isS && navDist <= MAP_NEAR_M;
  bool veryNear = !isS && navDist <= STAGE_NEAR_M;
  uint16_t col = veryNear ? C_ORANGE : C_WHITE;
  char dt[12]; distTextBuf(navDist, dt);        // 栈缓冲：这段每个导航帧都跑，别再走 String 堆分配
  iconHalo = true;
  if (!nearJ) {
    int up = tripOn ? 24 : 0;                          // v3.1 有行程信息时整体上移，下面留一行
    drawBigIconAt((isS || far) ? String("S") : navDir, C_WHITE, 395 - up, 1.0f, CX - 48, 368 - up, 418 - up, 60);
    int sz = strlen(dt) <= 4 ? 4 : 3;
    printOutlined(dt, CX + 46, (sz == 4 ? 378 : 382) - up, sz, C_WHITE);
    if (tripOn) drawTripLine(404);
  } else {
    const int top = 344;                               // v3.3 车道移到了上面，底部只放箭头和距离
    drawBigIconAt(navDir, col, top + 30, 1.0f, CX - 58, top, top + 60, 84);
    printOutlined(dt, CX + 54, top + 14, strlen(dt) <= 4 ? 5 : 4, col);
  }
  iconHalo = false;
}

// ---- v2.9 GTA 雷达元素 ----

// 雷达边缘渐暗：半径 r0~r1 之间越往外越暗（最多保留 maxDark/255 的亮度损失）
// v5.7 渐暗查表：每项 = 像素位置 << 8 | 保留亮度（0~255）
uint32_t *vigLut = nullptr;
int vigN = 0;
int vigLutCap = 0;                 // 已分配容量：几何变大时必须重分配，否则越界
float vigR0 = -1, vigR1 = -1; int vigMax = -1;

bool buildVignette(float r0, float r1, int maxDark) {
  int cap = (int)(PI * (r1 * r1 - r0 * r0)) + 16 * (int)r1 + 64;
  if (!vigLut || cap > vigLutCap) {
    if (vigLut) free(vigLut);
    vigLut = (uint32_t *)ps_malloc(sizeof(uint32_t) * cap);
    vigLutCap = vigLut ? cap : 0;
  }
  if (!vigLut) return false;
  vigN = 0;
  float ro2 = r1 * r1;
  for (int y = (int)(CX - r1); y <= (int)(CX + r1); y++) {
    if (y < 0 || y >= LCD_HEIGHT) continue;
    float dy = y + 0.5f - CX, dy2 = dy * dy;
    if (dy2 >= ro2) continue;
    for (int x = max(0, (int)(CX - r1) - 1); x <= min(LCD_WIDTH - 1, (int)(CX + r1) + 1); x++) {
      float dx = x + 0.5f - CX;
      float r = sqrtf(dx * dx + dy2);
      if (r < r0 || r > r1) continue;
      float k = (r - r0) / (r1 - r0);
      int dark = (int)(k * k * maxDark);
      if (dark <= 0 || vigN >= cap) continue;
      vigLut[vigN++] = ((uint32_t)(y * LCD_WIDTH + x) << 8) | (uint32_t)(255 - dark);
    }
  }
  vigR0 = r0; vigR1 = r1; vigMax = maxDark;
  return true;
}

void radarVignette(float r0, float r1, int maxDark) {
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;
  if ((vigR0 == r0 && vigR1 == r1 && vigMax == maxDark) || buildVignette(r0, r1, maxDark)) {
    for (int i = 0; i < vigN; i++) {                     // 查表：位置 + 保留多少亮度
      uint32_t e = vigLut[i];
      uint16_t &px = fb[e >> 8];
      uint32_t k = (e & 0xFF) + 1, c = px;
      px = (uint16_t)((((c >> 11) * k >> 8) << 11) | ((((c >> 5) & 63) * k >> 8) << 5) | ((c & 31) * k >> 8));
    }
    return;
  }
  // 内存不够时的老办法
  float ro2 = r1 * r1, ri2 = r0 * r0;
  for (int y = (int)(CX - r1); y <= (int)(CX + r1); y++) {
    if (y < 0 || y >= LCD_HEIGHT) continue;
    float dy = y + 0.5f - CX, dy2 = dy * dy;
    if (dy2 >= ro2) continue;
    float ho = sqrtf(ro2 - dy2), hi = dy2 < ri2 ? sqrtf(ri2 - dy2) : -1;
    uint16_t *row = fb + y * LCD_WIDTH;
    for (int side = 0; side < 2; side++) {
      int xa, xb;
      if (hi < 0) { if (side) break; xa = (int)(CX - ho); xb = (int)(CX + ho); }
      else if (side == 0) { xa = (int)(CX - ho); xb = (int)(CX - hi) + 1; }
      else { xa = (int)(CX + hi); xb = (int)(CX + ho) + 1; }
      xa = max(xa, 0); xb = min(xb, LCD_WIDTH - 1);
      for (int x = xa; x <= xb; x++) {
        float dx = x + 0.5f - CX;
        float r = sqrtf(dx * dx + dy2);
        if (r < r0 || r > r1) continue;
        float k = (r - r0) / (r1 - r0);
        int dark = (int)(k * k * maxDark);
        if (dark > 0) row[x] = blend565(row[x], C_BLACK, dark);
      }
    }
  }
}

// 北向标记：雷达边缘的黑底白色 “N”，位置随车头方向转动
void drawNorth(float hdgDeg) {
  float a = hdgDeg * PI / 180;
  float nx = CX - sinf(a) * 196, ny = CX - cosf(a) * 196;
  obsAdd(nx - 14, ny - 14, nx + 14, ny + 14);                          // v3.8
  aaDisc(nx, ny, 13, C_BLACK);
  aaDisc(nx, ny, 11, 0x2945);
  printCentered("N", (int)nx + 1, (int)ny - 7, 2, C_WHITE);
}

// 目的地标记：在雷达里就画在实际位置，在外面就贴着边缘指出方向（和游戏里的标记一样）
void drawDestBlip(float cx, float cy, float sc, float cs, float sn) {
  if (routeN < 2 || routeFrame != posFrame) return;
  float dx = routeX[routeN - 1] - cx, dy = routeY[routeN - 1] - cy;
  float sx = sc * (dx * cs - dy * sn), sy = -sc * (dx * sn + dy * cs);   // 相对车头位置
  float px = CX + sx, py = MAP_PY + sy;
  float ex = px - CX, ey = py - CX, er = sqrtf(ex * ex + ey * ey);
  const float RMAX = styleIdx() == ST_DRAGON ? 183 : 192;
  bool outside = er > RMAX;
  if (outside) { px = CX + ex / er * RMAX; py = CX + ey / er * RMAX; }
  if (styleIdx() == ST_DRAGON) {
    if (outside) DragonRadar::placeEdgeBall(px, py, CX, RMAX,
      [](float x0, float y0, float x1, float y1) { return obsOverlap((int)x0, (int)y0, (int)x1, (int)y1); });
    DragonRadar::drawBall(gfx, (int)px, (int)py);
    obsAdd(px - 21, py - 21, px + 21, py + 21); return;
  }
  if (styleIdx() == ST_COMPASS) { drawCompassBlip(px, py, outside); obsAdd(px - 14, py - 14, px + 14, py + 14); return; }
  aaDisc(px, py, outside ? 9 : 11, C_BLACK);
  aaDisc(px, py, outside ? 7 : 9, TH.blip);
  obsAdd(px - 12, py - 12, px + 12, py + 12);                         // v3.8
  if (!outside) aaDisc(px, py, 3, C_BLACK);
}

// v5.0 路程（米）→ 路线上的位置（段号 + 段内比例）
float routeUAt(float m) {
  if (!routeCum || routeN < 2) return 0;
  if (m <= 0) return 0;
  if (m >= routeCum[routeN - 1]) return (float)(routeN - 1);
  int lo = 0, hi = routeN - 1;
  while (hi - lo > 1) { int mid = (lo + hi) / 2; if (routeCum[mid] <= m) lo = mid; else hi = mid; }
  float seg = routeCum[hi] - routeCum[lo];
  return lo + (seg > 0 ? (m - routeCum[lo]) / seg : 0);
}

// v5.0 路况颜色：缓行黄、拥堵红、严重拥堵深红，盖在导航路线上（已经走过的部分不画）
void drawTraffic(float cx, float cy, float sc, float cs, float sn, float viewR, float w0, float w1, float from) {
  if (trafN == 0 || routeN < 2 || routeFrame != posFrame) return;
  for (int i = 0; i < trafN; i++) {
    float a = fmaxf(routeUAt(trafA[i]), from), b = routeUAt(trafB[i]);
    if (b <= a) continue;
    uint16_t c = trafS[i] >= 4 ? 0x9000 : trafS[i] == 3 ? 0xE8E4 : 0xFDA0;
    drawRouteRange(a, b, cx, cy, sc, cs, sn, viewR, w0, w1, C_ROUTE2, c);
  }
}

// v5.0 地图上方/下方的提示条：半透明深色底 + 文字
void drawBanner(const String &t, int y, uint16_t col) {
  int w = t.length() * 12 + 24;
  dimRect(CX - w / 2, y - 6, w, 28, 13, 60);
  printCentered(t, CX, y, 2, col);
}

// v5.0 重新规划中 / 更快路线 / GPS 信号弱（地图模式和大箭头模式都用）
void drawNavHints(int bannerY) {
  unsigned long now = millis();
  if (rerouteAt && now - rerouteAt > 20000) rerouteAt = 0;        // 最多显示 20 秒
  if (fasterAt && now - fasterAt > 30000) fasterAt = 0;           // 更快路线提示 30 秒
  if (rerouteAt) {
    String dots = String("...").substring(0, 1 + (now / 400) % 3);
    drawBanner("REROUTING" + dots, bannerY, C_ORANGE);
  } else if (fasterAt) {
    drawBanner("FASTER ROUTE  -" + String(fasterMin) + "min", bannerY, C_GREEN);
  }
  if (gpsWeak) {                                                  // 左上角：橙色 “GPS” 加一条斜杠
    const int gx = CX - 136, gy = 150;
    aaDisc(gx, gy, 15, C_BLACK);
    aaDisc(gx, gy, 13, 0x4208);
    printCentered("GPS", gx + 1, gy - 7, 1, C_ORANGE);
    printCentered("GPS", gx, gy - 7, 1, C_ORANGE);
    gfx->drawLine(gx - 9, gy + 7, gx + 9, gy - 7, C_RED);
    gfx->drawLine(gx - 9, gy + 8, gx + 9, gy - 6, C_RED);
  }
}

// v3.8 顶部车速/测速牌/车道、底部箭头和行程信息、车头箭头占的地方（这些后画，电子眼图标要提前让开）
void addHudObstacles() {
  obsAdd(CX - 15, MAP_PY - 18, CX + 15, MAP_PY + 16);                 // 车头箭头
  if (speedKmh >= 0) {
    int sx = hasCam ? CX - 40 : CX;
    obsAdd(sx - 60, 36, sx + 60, 92);                                   // 车速数字 + km/h
    if (hasCam) obsAdd(CX + 52 - 34, 30, CX + 52 + 34, 98);             // 测速牌 + 距离
  }
  if (laneN > 0 && navDir != "S" && navDist <= MAP_NEAR_M) {           // 车道（和 drawMapLanes 一致）
    const int H = 30, MAXW = 210;
    int lw = (int)(fminf(36.0f * H / 32, (float)MAXW / laneN) * laneN);
    int y = hasCam ? 114 : 100;
    obsAdd(CX - lw / 2 - 9, y - 5, CX + lw / 2 + 9, y + H + 5);
  }
  obsAdd(0, 340, LCD_WIDTH, LCD_HEIGHT);                                // 底部箭头、距离、行程信息
}

// 画一整帧小地图
uint32_t mapFrames = 0, mapDrawMsAcc = 0, mapMsAcc = 0, ovlMsAcc = 0;
unsigned long statAt = 0;
String pendingStat = "";


// ================= NavMap 段 15（源 MotoNav.cpp 8712-9229 行） =================


// 快速 atan2（误差约 0.01°），返回 0~1：0 = 正上方，顺时针
static inline float ringAngle(float dx, float dy) {
  float ax = fabsf(dx), ay = fabsf(dy);
  float mx = fmaxf(ax, ay), mn = fminf(ax, ay);
  float a = mx > 0 ? mn / mx : 0, s2 = a * a;
  float r = ((-0.0464964749f * s2 + 0.15931422f) * s2 - 0.327622764f) * s2 * a + a;
  if (ay > ax) r = 1.57079637f - r;
  if (dx < 0) r = 3.14159265f - r;      // 这里的 “x” 是向上的分量，见下面调用
  if (dy < 0) r = -r;
  float t = r / 6.2831853f;
  return t < 0 ? t + 1 : t;
}

// 初版路况提醒环：先用独立外圈验证手机到屏幕的状态链路。
void drawTrafficAlertRing() {
  if (trafficAlertLevel < 3 || !inNav || millis() - trafficAlertAt > 90000UL) return;
  float phase = (millis() % 1400) / 1400.0f;
  float pulse = 0.35f + 0.65f * (0.5f + 0.5f * sinf(phase * 2.0f * PI));
  uint16_t col = blendF(C_RED, C_BLACK, (uint32_t)((1.0f - pulse) * 120.0f));
  int width = 3 + (int)(pulse * 7.0f);
  for (int i = 0; i < width; ++i) gfx->drawCircle(CX, MAP_PY, 232 - i, col);
}

// 外圈参数
struct { uint16_t *fb; bool track; float prog; float ph; uint16_t pcol; bool over; float trip; } RP;   // v7.4 trip：行程环（-1 = 不画）
static inline float bandCov(float r, float rIn, float rOut) {
  float a = fminf(r - rIn + 0.5f, rOut - r + 0.5f);
  return a <= 0 ? 0 : (a >= 1 ? 1 : a);
}

// v6.0 整数版。坐标放大 2 倍取整：ix = 2x+1-W，iy = 2y+1-H，q = ix²+iy² = 4×半径²
#define RQ_MIN  (4 * 209 * 209)              // 半径 209 以内不用管
#define RQ_MAX  (4 * 235 * 235)              // 半径 235 以外整段清零
#define RQ_SH   4
uint16_t *rq2r = nullptr;                    // (q - RQ_MIN) >> 4 → 半径 × 64
int16_t ringInH[LCD_HEIGHT], ringOutH[LCD_HEIGHT];   // 每行：内圈半宽（以内跳过）、外圈半宽（以外清零）
float ringUx = 0, ringUy = -1;               // 进度条末端方向
float tripUx = 0, tripUy = -1;               // v7.4 行程环“走过 / 没走”交界方向

bool ringTablesInit() {
  if (rq2r) return true;
  int n = ((RQ_MAX - RQ_MIN) >> RQ_SH) + 2;
  rq2r = (uint16_t *)heap_caps_malloc(n * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!rq2r) return false;
  for (int k = 0; k < n; k++) rq2r[k] = (uint16_t)(sqrtf((float)(RQ_MIN + (k << RQ_SH) + 8)) * 32.0f);   // √q/2×64
  for (int y = 0; y < LCD_HEIGHT; y++) {
    int iy = 2 * y + 1 - LCD_HEIGHT;
    long iy2 = (long)iy * iy;
    int hin = 0, hout = 0;
    for (int x = LCD_WIDTH / 2; x < LCD_WIDTH; x++) {                // 右半边往外找
      int ix = 2 * x + 1 - LCD_WIDTH;
      long q = (long)ix * ix + iy2;
      if (q < RQ_MIN) hin = x - LCD_WIDTH / 2 + 1;
      if (q < RQ_MAX) hout = x - LCD_WIDTH / 2 + 1;
    }
    ringInH[y] = hin; ringOutH[y] = hout;
  }
  return true;
}

// 一段圆环的覆盖率（0~64）：r64 = 半径×64；带宽 [a, b]
static inline int bandCov64(int r64, int a, int b) {
  int c = min(r64 - (a * 64 - 32), (b * 64 + 32) - r64);
  return c <= 0 ? 0 : (c >= 64 ? 64 : c);
}

static inline void ringPixel(uint16_t *px, int ix, long iy2, int iy) {
  long q = (long)ix * ix + iy2;
  int k = (int)((q - RQ_MIN) >> RQ_SH);
  int r64 = rq2r[k < 0 ? 0 : k];
  uint16_t c = *px;
  int a = bandCov64(r64, 211, 240);
  if (a) c = a >= 64 ? C_BLACK : blendF(c, C_BLACK, a * 4);
  if (RP.track) { a = bandCov64(r64, RING_R - 5, RING_R + 5); if (a) c = a >= 64 ? C_TRACK : blendF(c, C_TRACK, a * 4); }
  if (RP.trip >= 0) {                                  // v7.4 行程环：走过的灰，没走的绿
    a = bandCov64(r64, RING_R - TRIP_H, RING_R + TRIP_H);
    if (a) {
      bool right = ix >= 0;
      bool before = (ix * tripUy - iy * tripUx) >= 0;
      bool done = RP.trip >= 0.9999f ? true : RP.trip <= 0.0001f ? false : (RP.trip <= 0.5f ? (right && before) : (right || before));
      uint16_t tc = done ? TRIP_DONE_C : TRIP_LEFT_C;
      c = a >= 64 ? tc : blendF(c, tc, a * 4);
    }
  }
  if (RP.prog > 0.002f) {
    int h = (int)RP.ph;
    a = bandCov64(r64, RING_R - h, RING_R + h);
    if (a) {
      // 在进度范围内吗：正上方顺时针转到末端方向 (ux, uy)
      bool right = ix >= 0;
      bool before = (ix * ringUy - iy * ringUx) >= 0;
      bool in = RP.prog >= 0.9999f ? true : (RP.prog <= 0.5f ? (right && before) : (right || before));
      if (in) c = a >= 64 ? RP.pcol : blendF(c, RP.pcol, a * 4);
    }
  }
  if (RP.over) { a = bandCov64(r64, 228, 233); if (a) c = a >= 64 ? C_RED : blendF(c, C_RED, a * 4); }
  *px = c;
}

void ringRows(int y0, int y1, int step) {
  uint16_t *fb = RP.fb;
  const int W = LCD_WIDTH, HW = LCD_WIDTH / 2;
  for (int y = y0; y < y1; y += step) {
    uint16_t *row = fb + y * W;
    int iy = 2 * y + 1 - LCD_HEIGHT;
    long iy2 = (long)iy * iy;
    int ho = ringOutH[y], hi = ringInH[y];
    // 圆外两头：整段清零（黑色 = 0）
    if (HW - ho > 0) memset(row, 0, (HW - ho) * 2);
    if (HW + ho < W) memset(row + HW + ho, 0, (W - HW - ho) * 2);
    // 左边一段 [HW-ho, HW-hi)，右边一段 [HW+hi, HW+ho)
    for (int x = HW - ho; x < HW - hi; x++) ringPixel(row + x, 2 * x + 1 - W, iy2, iy);
    for (int x = HW + hi; x < HW + ho; x++) ringPixel(row + x, 2 * x + 1 - W, iy2, iy);
  }
}

// 画外圈：黑边 + 进度环底（track）+ 进度条（prog 0~1，粗 thick）+ 超速红圈
void drawRingLayer(bool track, float prog, int thick, uint16_t pcol, bool over, float trip) {
  RP.fb = gfx->getFramebuffer();
  if (!RP.fb) return;
  RP.track = track; RP.prog = prog > 1 ? 1 : prog; RP.ph = thick / 2.0f; RP.pcol = pcol; RP.over = over;
  RP.trip = trip; tripUx = sinf(trip * 2 * PI); tripUy = -cosf(trip * 2 * PI);
  if (!ringTablesInit()) {                             // 内存不够：退回老办法
    fillRingBand(0, 1, 211, 240, C_BLACK);
    if (trip >= 0) drawTripRing(trip);
    if (track) drawRingTrack();
    if (prog > 0.002f) drawRingProgress(prog, thick, pcol);
    if (over) fillRingBand(0, 1, 228, 233, C_RED);
    return;
  }
  ringUx = sinf(RP.prog * 2 * PI); ringUy = -cosf(RP.prog * 2 * PI);
  parallelRows(ringRows, 0, LCD_HEIGHT);
  if (RP.prog > 0.002f) {                              // 进度条两头的圆头
    float h = RP.ph, a = RP.prog * 2 * PI;
    aaDisc(CX, CX - RING_R, h, pcol);
    aaDisc(CX + RING_R * sinf(a), CX - RING_R * cosf(a), h, pcol);
  }
  if (trip >= 0) drawTripDot(trip);
}

// 边缘渐暗：半径 r0~r1 之间越往外越暗；暗度按 “半径平方” 查表（表放在芯片内部内存）
uint8_t *vigKeep = nullptr;                // 下标 = 半径² - r0²，值 = 保留亮度（1~256 存成 0~255）
int vigK0 = 0, vigK1 = 0;
int vigKeepCap = 0;                         // 已分配容量：几何变大时必须重分配，否则越界
struct { uint16_t *fb; float r0, r1; } VP;

void vignetteRows(int y0, int y1, int step) {
  uint16_t *fb = VP.fb;
  float r0 = VP.r0, r1 = VP.r1, ri2 = r0 * r0, ro2 = r1 * r1;
  for (int y = y0; y < y1; y += step) {
    float dy = y + 0.5f - CX, dy2 = dy * dy;
    if (dy2 >= ro2) continue;
    float ho = sqrtf(ro2 - dy2), hi = dy2 < ri2 ? sqrtf(ri2 - dy2) : -1;
    uint16_t *row = fb + y * LCD_WIDTH;
    for (int side = 0; side < 2; side++) {
      int xa, xb;
      if (hi < 0) { if (side) break; xa = (int)(CX - ho); xb = (int)(CX + ho); }
      else if (side == 0) { xa = (int)(CX - ho); xb = (int)(CX - hi) + 1; }
      else { xa = (int)(CX + hi); xb = (int)(CX + ho) + 1; }
      xa = max(xa, 0); xb = min(xb, LCD_WIDTH - 1);
      for (int x = xa; x <= xb; x++) {
        float dx = x + 0.5f - CX;
        int k = (int)(dx * dx + dy2) - vigK0;
        if (k < 0 || k >= vigK1 - vigK0) continue;
        uint32_t keep = (uint32_t)vigKeep[k] + 1;
        if (keep >= 256) continue;
        uint32_t c = row[x];
        row[x] = (uint16_t)((((c >> 11) * keep >> 8) << 11) | ((((c >> 5) & 63) * keep >> 8) << 5) | ((c & 31) * keep >> 8));
      }
    }
  }
}

void drawVignette(float r0, float r1, int maxDark) {
  VP.fb = gfx->getFramebuffer();
  if (!VP.fb) return;
  if (!vigKeep || vigK0 != (int)(r0 * r0) || vigK1 != (int)(r1 * r1) + 1) {
    vigK0 = (int)(r0 * r0); vigK1 = (int)(r1 * r1) + 1;
    int need = vigK1 - vigK0 + 64;
    if (need > vigKeepCap) {
      if (vigKeep) heap_caps_free(vigKeep);
      vigKeep = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      vigKeepCap = vigKeep ? need : 0;
    }
    if (!vigKeep) { radarVignette(r0, r1, maxDark); return; }
    for (int k = 0; k < vigK1 - vigK0; k++) {
      float r = sqrtf((float)(k + vigK0) + 0.5f);
      float t = (r - r0) / (r1 - r0);
      if (t < 0) t = 0;
      if (t > 1) t = 1;
      int dark = (int)(t * t * maxDark);
      vigKeep[k] = (uint8_t)(255 - dark);
    }
  }
  VP.r0 = r0; VP.r1 = r1;
  parallelRows(vignetteRows, (int)(CX - r1) - 1, (int)(CX + r1) + 2);
}

// ================= v6.1 风格特效 =================
static inline void projXY(float x, float y, float cx, float cy, float sc, float cs, float sn, float &px, float &py) {
  px = CX + sc * ((x - cx) * cs - (y - cy) * sn);
  py = MAP_PY - sc * ((x - cx) * sn + (y - cy) * cs);
}

// 蓝图：跟着地图走的网格，每 50 米一条线
void drawBlueprintGrid(float cx, float cy, float sc, float cs, float sn, float viewR) {
  const float G = 50;
  uint16_t col = nightOn ? 0x1A0F : 0x2AB2;
  float x0 = floorf((cx - viewR) / G) * G, y0 = floorf((cy - viewR) / G) * G;
  for (float g = x0; g <= cx + viewR; g += G) {
    float ax, ay, bx, by;
    projXY(g, cy - viewR, cx, cy, sc, cs, sn, ax, ay); projXY(g, cy + viewR, cx, cy, sc, cs, sn, bx, by);
    gfx->drawLine((int)ax, (int)ay, (int)bx, (int)by, col);
  }
  for (float g = y0; g <= cy + viewR; g += G) {
    float ax, ay, bx, by;
    projXY(cx - viewR, g, cx, cy, sc, cs, sn, ax, ay); projXY(cx + viewR, g, cx, cy, sc, cs, sn, bx, by);
    gfx->drawLine((int)ax, (int)ay, (int)bx, (int)by, col);
  }
}

// 雷达：三圈距离环 + 绕着车转的扫描线（后面拖一串渐暗的线）
void drawRadarSweep() {
  for (int r = 70; r <= 210; r += 70) gfx->drawCircle(CX, MAP_PY, r, 0x0A44);
  float a0 = (millis() % 3200) / 3200.0f * 2 * PI;
  const int TRAIL = 26;
  for (int k = TRAIL - 1; k >= 0; k--) {
    float a = a0 - k * 0.022f;
    uint16_t c = blendF(0x00C1, 0x8FF4, (uint32_t)((TRAIL - k) * 256 / TRAIL));
    gfx->drawLine(CX, MAP_PY, (int)(CX + sinf(a) * 300), (int)(MAP_PY - cosf(a) * 300), c);
  }
}

// 寻宝罗盘：屏幕上一层淡淡的六边形网格（开机第一次用时把网格上的像素位置算好存起来）
uint32_t *hexIdx = nullptr;
int hexN = 0;
bool hexTried = false;
void buildHexGrid() {
  hexTried = true;
  uint8_t *m = (uint8_t *)ps_malloc(LCD_WIDTH * LCD_HEIGHT);
  if (!m) return;
  memset(m, 0, LCD_WIDTH * LCD_HEIGHT);
  const float R = 26, H = R * 1.7320508f;
  auto line = [&](float x0, float y0, float x1, float y1) {
    int n = (int)(fmaxf(fabsf(x1 - x0), fabsf(y1 - y0))) + 1;
    for (int i = 0; i <= n; i++) {
      int x = (int)(x0 + (x1 - x0) * i / n), y = (int)(y0 + (y1 - y0) * i / n);
      if (x >= 0 && x < LCD_WIDTH && y >= 0 && y < LCD_HEIGHT) m[y * LCD_WIDTH + x] = 1;
    }
  };
  for (int col = -1; col * R * 1.5f < LCD_WIDTH + R; col++)
    for (int row = -1; row * H < LCD_HEIGHT + H; row++) {
      float cx = col * R * 1.5f, cy = row * H + ((col & 1) ? H / 2 : 0);
      for (int k = 0; k < 6; k++) {
        float a0 = k * PI / 3, a1 = (k + 1) * PI / 3;
        line(cx + R * cosf(a0), cy + R * sinf(a0), cx + R * cosf(a1), cy + R * sinf(a1));
      }
    }
  int cnt = 0;
  for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) if (m[i]) cnt++;
  hexIdx = (uint32_t *)ps_malloc(sizeof(uint32_t) * (cnt + 1));
  if (hexIdx) {
    for (int y = 0; y < LCD_HEIGHT; y++) {
      float dy = y + 0.5f - CX;
      for (int x = 0; x < LCD_WIDTH; x++) {
        if (!m[y * LCD_WIDTH + x]) continue;
        float dx = x + 0.5f - CX;
        if (dx * dx + dy * dy < 206.0f * 206.0f) hexIdx[hexN++] = y * LCD_WIDTH + x;
      }
    }
  }
  free(m);
}

void drawHexGrid() {
  if (!hexTried) buildHexGrid();
  if (!hexIdx) return;
  uint16_t *fb = gfx->getFramebuffer();
  for (int i = 0; i < hexN; i++) { uint16_t &p = fb[hexIdx[i]]; p = blendF(p, 0x3A4F, 80); }
}

// 寻宝罗盘：外圈刻度（每 5° 一根，东南西北长一点）+ 方位字母，跟着车头转
void drawCompassRose(float hdgDeg) {
  for (int k = 0; k < 72; k++) {
    float a = (k * 5 - hdgDeg) * PI / 180;
    bool major = k % 18 == 0;
    float r0 = 208, r1 = major ? 192 : (k % 2 == 0 ? 199 : 203);
    float sx = sinf(a), cy = -cosf(a);
    uint16_t c = major ? 0xFD80 : 0x7B48;
    gfx->drawLine((int)(CX + sx * r0), (int)(CX + cy * r0), (int)(CX + sx * r1), (int)(CX + cy * r1), c);
    if (major) gfx->drawLine((int)(CX + sx * r0) + 1, (int)(CX + cy * r0), (int)(CX + sx * r1) + 1, (int)(CX + cy * r1), c);
  }
  const char *L[4] = {"N", "E", "S", "W"};
  for (int k = 0; k < 4; k++) {
    float a = (k * 90 - hdgDeg) * PI / 180;
    float x = CX + sinf(a) * 178, y = CX - cosf(a) * 178;
    if (k == 0) obsAdd(x - 10, y - 10, x + 10, y + 10);
    printOutlined(L[k], (int)x, (int)y - 7, 2, k == 0 ? 0xFD80 : 0x7B48);
  }
}

// 寻宝罗盘：目的地 = 琥珀色菱形 + 往外扩的脉冲圈
void drawCompassBlip(float px, float py, bool outside) {
  if (!outside) {
    float ph = (millis() % 1400) / 1400.0f;
    for (int k = 0; k < 2; k++) {
      float t = fmodf(ph + k * 0.5f, 1.0f);
      int r = (int)(12 + t * 20);
      uint16_t c = blendF(0xFD80, TH.block, (uint32_t)(t * 256));
      gfx->drawCircle((int)px, (int)py, r, c);
      gfx->drawCircle((int)px, (int)py, r + 1, c);
    }
  }
  float s = outside ? 8 : 11;
  gfx->fillTriangle(px, py - s - 2, px + s + 2, py, px - s - 2, py, C_BLACK);
  gfx->fillTriangle(px, py + s + 2, px + s + 2, py, px - s - 2, py, C_BLACK);
  gfx->fillTriangle(px, py - s, px + s, py, px - s, py, 0xFD80);
  gfx->fillTriangle(px, py + s, px + s, py, px - s, py, 0xFD80);
}

// v5.8 分步计时（微秒累计，每 2 秒发一次）
#define PROF_N 11
uint32_t profAcc[PROF_N];
uint32_t profT = 0;
static inline void profMark(int i) { uint32_t n = micros(); profAcc[i] += n - profT; profT = n; }

void drawMapFrame() {
  if (!mapAllocOk) return;            // 缓冲不完整（PSRAM 异常）：这一帧画不了地图
  unsigned long t = millis();
  lastMapDraw = t;
  frameIsMap = true;
  // 位置：按车速和转弯速度往前推算（v6.7 沿弧线，再加一点提前量），让画面连续移动、少“慢半拍”
  float cx, cy, hPred;
  predictPose(t, leadMs / 1000.0f, cx, cy, hPred);
  viewTargetHdg = hPred;
  if (corrAt) {                                         // v2.6 平滑修正
    float f = fmaxf(0, 1 - (t - corrAt) / 400.0f);
    cx += corrX * f; cy += corrY * f;
  }
  // 朝向平滑转动，缩放跟车速（越快看得越远）
  if (!viewInit) { viewHdg = hPred; viewInit = true; }
  float dh = hPred - viewHdg;
  while (dh > 180) dh -= 360;
  while (dh < -180) dh += 360;
  // v3.7 按时间平滑转动（和帧率无关）：时间常数 0.35 秒，最快每秒转 70 度。
  //      每一帧之间画面变化更小、更均匀，转弯时的撕裂感会轻很多（这块板子没接屏幕的刷新同步信号，
  //      没法从硬件上完全消除撕裂，只能让相邻两帧尽量接近）
  static unsigned long lastViewAt = 0;
  float vdt = lastViewAt ? (t - lastViewAt) / 1000.0f : 0.05f;
  lastViewAt = t;
  if (vdt > 0.25f) vdt = 0.25f;
  float hstep = dh * (1.0f - expf(-vdt / 0.22f));       // v6.7 0.35 → 0.22 秒：追得更紧
  float hmax = fminf(70.0f * vdt, 3.5f);                // v6.0 每帧最多转 3.5°（防撕裂）
  if (hstep > hmax) hstep = hmax;
  if (hstep < -hmax) hstep = -hmax;
  viewHdg += hstep;
  const bool dragon = styleIdx() == ST_DRAGON;
  bool vecOK = !dragon && mapValid && mapFrame == posFrame;
  bool rasOK = !dragon && rasterValid && rasterFrame == posFrame;
  bool useRaster = rasOK && (!vecOK || rasterAt >= vectorAt);   // v2.5 两种都有时用最新收到的
  static unsigned long mapDiagAt = 0;
  if (t - mapDiagAt >= 5000) {
    mapDiagAt = t;
    Serial.printf("[MAP-DRAW] start inNav=%d page=%d style=%d dragon=%d posFrame=%d raster=%d rasterFrame=%d vector=%d useRaster=%d ageMs=%u size=%dx%d mpp=%.3f building=%d PSRAM=%u\n",
                  inNav, page, styleIdx(), dragon, posFrame, rasterValid, rasterFrame, vecOK, useRaster,
                  rasterValid ? (unsigned)(t - rasterAt) : 0, rasW, rasH, rasMpp, rasterBuildings, ESP.getFreePsram());
  }
  // 图片地图每像素 2 米，放太大会显得粗糙，所以最大放大倍数小一些
  float targetS = constrain(0.85f - posKmh * 0.006f, 0.38f, useRaster ? 0.6f : 0.85f);
  viewScale += (targetS - viewScale) * (1.0f - expf(-vdt / 1.2f));   // v3.7 缩放也按时间平滑
  float sc = viewScale, hs = viewHdg * PI / 180, cs = cosf(hs), sn = sinf(hs);
  float viewR = (CX + (MAP_PY - CX) + 30) / sc;

  float wk = sc / 0.6f;
  profT = micros();
  if (useRaster) drawRasterMap(cx, cy, sc, cs, sn);     // v2.5 天地图图片地图
  else gfx->fillScreen(C_BLOCK);
  profMark(0);
  // 地图块里的要素（文件里已按：水面、绿地、细路 → 粗路 排好序）
  float lx = cx - mapOx, ly = cy - mapOy;
  const float roadW[6] = {0, 15, 12, 10, 7, 5};
  for (int i = 0; i < ((dragon || useRaster) ? 0 : mapFeatN); i++) {
    const int16_t *bx = featBox + 4 * i;
    if (bx[2] < lx - viewR || bx[0] > lx + viewR || bx[3] < ly - viewR || bx[1] > ly + viewR) continue;
    int np = featNp[i];
    if (np > POLY_PTS) np = POLY_PTS;
    const uint8_t *pp = mapShow + featOff[i];
    for (int k = 0; k < np; k++) {
      float dx = rd16(pp + 4 * k) - lx, dy = rd16(pp + 4 * k + 2) - ly;
      tmpX[k] = CX + sc * (dx * cs - dy * sn);
      tmpY[k] = MAP_PY - sc * (dx * sn + dy * cs);
    }
    uint8_t cls = featCls[i];
    if (cls & 0x80) {
      mapFillPoly(tmpX, tmpY, np, (cls & 0x7F) == 11 ? C_WATER : C_PARK);
    } else {
      int c = cls & 0x7F;
      if (c < 1 || c > 5) c = 5;
      mapPolyline(tmpX, tmpY, np, fmaxf(2.0f, roadW[c] * wk), C_ROADM, false);
    }
  }
  {                                                     // v6.1 风格特效（画在路线下面）
    int st = styleIdx();
    if (st == ST_DRAGON) DragonRadar::drawGrid(gfx, LCD_WIDTH, LCD_HEIGHT, nightOn);
    else if (st == ST_BLUEPRINT) drawBlueprintGrid(cx, cy, sc, cs, sn, viewR);
    else if (st == ST_RADAR) drawRadarSweep();
    else if (st == ST_COMPASS) drawHexGrid();
  }
  profMark(1);
  // 导航路线（带深色描边）；v3.0 车已经走过的部分变灰或不画
  // v7.6 巡航没有路线：上一次导航留下的路线、原路线、路况、红绿灯、终点都不画，道路保持原色
  if (!dragon && !cruiseOn) drawGhost(cx, cy, sc, cs, sn, viewR, fmaxf(3, 5 * wk));   // v4.4 原路线（导入的分享路线）
  if (!cruiseOn && routeN > 1 && routeFrame == posFrame) {
    updateRouteProgress(cx, cy);
    float split = routeProg >= 0 ? routeProg : 0;
    float w0 = fmaxf(6, 13 * wk), w1 = fmaxf(4, 9 * wk);
    if (split > 0 && trailMode == 1)
      drawRouteRange(0, split, cx, cy, sc, cs, sn, viewR, w0 * 0.8f, w1 * 0.75f, TH.done2, TH.done);
    drawRouteRange(split, (float)(routeN - 1), cx, cy, sc, cs, sn, viewR, w0, w1, C_ROUTE2, C_ROUTE);
    if (!dragon && curveOn && routeCurv) drawRouteCurved(split, (float)(routeN - 1), cx, cy, sc, cs, sn, viewR, w1);   // v6.1
    if (!dragon) drawTraffic(cx, cy, sc, cs, sn, viewR, w0, w1, split);                       // v5.0 路况颜色
  }
  profMark(2);
  obsN = 0;                                             // v3.8 占位框从头登记
  if (dragon) {
    bool hudCam = hasCam; hasCam = false; // This style hides the camera badge.
    addHudObstacles(); hasCam = hudCam;
  } else addHudObstacles();
  if (!dragon && !cleanOn && !cruiseOn) drawMapLights(cx, cy, sc, cs, sn);   // v3.2 路线上的红绿灯（v4.9 清爽模式不画；v7.6 巡航不画）
  if (!dragon) drawMapCams(cx, cy, sc, cs, sn);                      // v3.5 电子眼
  if (!cruiseOn) drawDestBlip(cx, cy, sc, cs, sn);      // v2.9 目的地标记（v7.6 巡航没有目的地）
  drawPlayer(CX, MAP_PY);
  profMark(3);
  {
    int st = styleIdx();                                // v6.1 纸地图、蓝图、掌机像素不压暗边缘
    if (st != ST_DRAGON && st != ST_PAPER && st != ST_BLUEPRINT && st != ST_PIXEL) drawVignette(178, 211, 150);   // v2.9 雷达边缘渐暗
  }
  profMark(4);
  if (styleIdx() == ST_COMPASS) drawCompassRose(viewHdg);   // v6.1 罗盘刻度
  else if (!dragon) drawNorth(viewHdg);                              // v2.9 北向标记
  if (!dragon && !cleanOn) drawCamIcons(cx, cy, sc, cs, sn);       // v3.8 电子眼图标（v4.9 清爽模式不画）
  profMark(5);
  unsigned long tMapDone = millis();                   // v2.6 统计：画地图用时
  mapMsAcc += tMapDone - t;

  // 边框和信息
  // v4.9 清爽模式：测速牌、超速红圈、底部行程信息（到达时间/里程/红绿灯数）都不显示
  bool saveCam = hasCam, saveTrip = tripOn;
  if (cleanOn || dragon) { hasCam = false; tripOn = false; }
  bool over = hasCam && camLimit > 0 && speedKmh > camLimit;
  {                                                     // v5.9 外圈一次画完
    bool isNear = navDist <= STAGE_NEAR_M;
    // v7.4 平时外圈是行程环；出现转弯倒计时就换成倒计时环（盖掉行程环）
    float tp = ringActive ? -1 : tripFrac();
    drawRingLayer(navDir != "S" && tp < 0, ringActive ? ringShown : 0, isNear ? 14 : 10, isNear ? C_ORANGE : C_YELLOW, over, tp);
    drawTrafficAlertRing();
  }
  profMark(6);
  drawMapSpeed(over);
  profMark(7);
  drawMapLanes();                                       // v3.3 车道（车速下面）
  profMark(8);
  drawMapBadge();
  profMark(9);
  hasCam = saveCam; tripOn = saveTrip;
  drawNavHints(300);                                    // v5.0 重新规划中 / 更快路线 / GPS 信号弱
  drawBattery(CX - 16, 18, true);                         // v6.8 电量（车速上方）；v7.2 带黑底
  profMark(10);
  // v2.9 小地图里不画顶部的蓝牙小圆点（会和 N 标记重叠；能收到地图就说明连着）
  needFlush = true;
  mapFrames++;
  mapDrawMsAcc += millis() - t;
  if (mapDiagAt == t) {
    Serial.printf("[MAP-DRAW] done total_ms=%u raster_ms_avg=%u building_samples=%u flush_pending=%d\n",
                  (unsigned)(millis() - t), mapFrames ? rasMsAcc / mapFrames : 0,
                  (unsigned)rasterBuildingSamples, needFlush);
  }
  ovlMsAcc += millis() - tMapDone;
}

// v2.2 每 2 秒统计一次帧率，发给手机显示
void updateStats() {
  unsigned long now = millis();
  if (now - statAt < 2000) return;
  float secs = (now - statAt) / 1000.0f;
  if (statAt != 0 && mapFrames > 0) {
    uint32_t fc = flushCnt, fm = flushMsAcc;
    pendingStat = "S," + String(mapFrames / secs, 1) + "," + String(mapDrawMsAcc / mapFrames) + "," +
                  String(fc ? fm / fc : 0) + "," + String(qspiMHz) + "," +
                  String(mapMsAcc / mapFrames) + "," + String(ovlMsAcc / mapFrames) + "," + String(fastDev ? 1 : 0) + "," +
                  String(rasMsAcc / mapFrames) + "," + String(helperTask ? 2 : 1);   // v5.7 底图用时、几个核心在画
    String q = "Q";                                    // v5.8 分步用时（0.1 毫秒/帧）
    for (int i = 0; i < PROF_N; i++) q += "," + String(profAcc[i] / 100 / mapFrames);
    pendingProf = q;
  }
  for (int i = 0; i < PROF_N; i++) profAcc[i] = 0;
  statAt = now; mapFrames = 0; mapDrawMsAcc = 0; mapMsAcc = 0; ovlMsAcc = 0; flushCnt = 0; flushMsAcc = 0; rasMsAcc = 0;
}

void updateRingTarget() {
  bool hasRing = (navDir != "S") && navDist <= STAGE_FAR_M;
  float target = hasRing ? (float)(STAGE_FAR_M - navDist) / STAGE_FAR_M : 0;
  if (navDir != ringDir || !ringActive || !hasRing) ringShown = target;  // 换路口/刚进入：直接到位
  ringTarget = target;
  ringActive = hasRing;
  ringDir = navDir;
}

void animateRing() {
  if (!inNav || !ringActive || staleShown) return;
  float diff = ringTarget - ringShown;
  if (fabsf(diff) < 0.0005f) return;
  ringShown += diff * 0.35f;
  if (fabsf(ringTarget - ringShown) < 0.002f) ringShown = ringTarget;
  navDirty = true;
}


// ================= NavMap 段 16（源 MotoNav.cpp 9230-9281 行） =================

// 按当前全部状态画一整帧导航画面
void drawNavFrame() {
  onWaitScreen = false;                                 // v3.6 离开首页，猫猫不再动
  if (mapModeActive()) { drawMapFrame(); return; }      // v2.0 小地图
  viewInit = false;
  if (cruiseOn) {                                       // v4.8 巡航但还没有地图：只显示车速和测速
    gfx->fillScreen(C_BLACK);
    bool over = hasCam && camLimit > 0 && speedKmh > camLimit;
    if (over) fillRingBand(0, 1, 228, 233, C_RED);
    drawStatus();
    drawSpeedBlock(over);
    printCentered("CRUISE", CX, 300, 4, C_CYAN);
    needFlush = true;
    return;
  }
  gfx->fillScreen(C_BLACK);
  bool isS = (navDir == "S");
  bool isFar = !isS && navDist > STAGE_FAR_M;
  bool lanes = laneN > 0;
  bool over = hasCam && camLimit > 0 && speedKmh > camLimit;
  float laneK = lanes ? 0.85f : 1.0f;
  float iconCy = lanes ? ICON_CY_LANE : ICON_CY;

  // 边缘：进度环 / 超速红边
  float tp = ringActive ? -1 : tripFrac();              // v7.4 平时外圈是行程环，转弯倒计时时让位
  if (tp >= 0) drawTripRing(tp);
  else if (!isS) drawRingTrack();
  if (ringActive) {
    bool isNear = navDist <= STAGE_NEAR_M;
    drawRingProgress(ringShown, isNear ? 14 : 10, isNear ? C_ORANGE : C_YELLOW);
  }
  if (over) fillRingBand(0, 1, 228, 233, C_RED);

  drawStatus();
  drawSpeedBlock(over);

  // 中间图标
  // v1.8.2 图标可用区域：车速下方 ~ 车道（或距离数字）上方，各留 8 像素
  float iconTop = ICON_TOP - 20, iconBot = (lanes ? LANES_Y : DIST_Y) - 8;
  if (isFar)     drawPreview(navDir, iconCy, 0.66f * laneK, iconTop, iconBot);
  else if (isS)  drawBigIcon("S", C_WHITE, iconCy, laneK * 1.2f, iconTop, iconBot);
  else           drawBigIcon(navDir, navDist <= STAGE_NEAR_M ? C_ORANGE : C_WHITE, iconCy, laneK * 1.2f, iconTop, iconBot);

  // 车道、距离、路名、底部信息
  if (lanes) drawLanes(LANES_Y);
  char ndb[12]; distTextBuf(navDist, ndb);
  printCentered(ndb, CX, lanes ? DIST_Y_LANE : DIST_Y, lanes ? 7 : 8, C_WHITE);
  drawTextImg(0, lanes ? ROAD_Y_LANE : ROAD_Y, 0xDD);
  drawTextImg(1, BOTTOM_Y, 0x99);                      // v1.7.3：常驻显示
  drawNavHints(128);                                    // v5.0
  needFlush = true;
}
// v7.3 开始新一趟导航 / 清屏时清零路程进度（原是 MotoNav.cpp 里的 progReset 宏）
void navProgReset() {
  progDone = 0; progLastRm = -1; progLastAt = 0;
  tripProgress.reset();
}

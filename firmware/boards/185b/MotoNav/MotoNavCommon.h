// MotoNav 主文件和 NavMap 等拆分模块共用的类型、常量和绘图接口。
// 拆自 MotoNav.cpp（v7.13.1）；全局变量本体仍在 MotoNav.cpp，导航/地图状态见 NavMap.h。
#pragma once

#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "driver/spi_master.h"
#include "TripProgress.h"
#include "HomeAnimation.h"   // onWaitScreen / sliding / waitSt 等首页动画状态

// ===== 颜色（RGB565）=====
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GREEN  0x07E0
#define C_GREY   0x7BEF
#define C_CYAN   0x07FF
#define C_RED    0xF800
#define C_DIM    0x6B4D   // #6B6B6B 预告、非推荐车道用的灰色
#define C_TRACK  0x2945   // #2A2A2A 进度环底色
#define C_YELLOW 0xF623   // #F5C518
#define C_ORANGE 0xFBC3   // #FF7A1A
#define C_SEP    0x31A6   // #333333 车道分隔线
#define C_SIGNRED 0xE104  // #E02020 限速牌红圈

// ===== 屏幕几何：466x466，圆心 (233, 233) =====
#define CX 233
#define RING_R 222

// ===== 布局（y 坐标）=====
#define SPEED_Y      50    // 车速数字顶部
#define ICON_CY      188   // 没有车道时，图标中心（v1.8.2 起只作初始位置，最终按可用区域自动摆放）
#define ICON_CY_LANE 172   // 有车道时，图标中心
#define ICON_TOP     120   // v1.8.2 图标可用区域上沿（车速 km/h 下方）
#define ICON_MAX_W   250   // 图标最大宽度
#define LANES_Y      238   // 车道行顶部
#define DIST_Y       250   // 没有车道时，距离数字顶部（8 号字）
#define DIST_Y_LANE  258   // 有车道时，距离数字顶部（7 号字）
#define ROAD_Y       334   // 没有车道时，路名图片顶部
#define ROAD_Y_LANE  334   // 有车道时，路名图片顶部
#define BOTTOM_Y     372   // 底部信息图片顶部（常驻显示）
#define TEXT_R       200   // 文字必须在这个半径以内（圆环内沿约 215）

// 页面从左到右：音乐、胎压、主页/导航、IMU、天气。
// 使用连续的内部编号，便于滑动动画和边界判断；不要把 PAGE_MUSIC 当成主页。
enum MotoNavPage : uint8_t {
  PAGE_MUSIC = 0,
  PAGE_TPMS = 1,
  PAGE_HOME = 2,
  PAGE_IMU = 3,
  PAGE_WEATHER = 4,
};

// 先画在内存里的“画布”上，画完再一次性推到屏幕（需要 PSRAM：OPI PSRAM）
// v2.2：可以切换画布内存的画布，用来做双缓冲
void flushLogicalFrame(const uint16_t *fb);
class SwapCanvas : public Arduino_Canvas {
public:
  SwapCanvas(int16_t w, int16_t h, Arduino_G *out) : Arduino_Canvas(w, h, out) {}
  void setFramebuffer(uint16_t *f) { _framebuffer = f; }
  void flush(bool force_flush=false) override { if(_framebuffer)flushLogicalFrame(_framebuffer); }
};
extern SwapCanvas *gfx;

extern bool needFlush;
extern bool frameIsMap;           // v2.2 这一帧是小地图（整帧重画，可以异步刷屏）
extern int qspiMHz;               // v2.3 屏幕数据线速度
extern spi_device_handle_t fastDev;   // v2.6 快速刷屏设备（updateStats 里报告用没用快速刷屏）
extern volatile uint32_t flushCnt, flushMsAcc;   // v2.2 刷屏耗时统计
extern int page;                  // 当前页面（MotoNavPage）
extern bool staleShown;           // 是否正在显示“数据中断”提示

// ===== 抗锯齿矢量图标（实现在 MotoNav.cpp）=====
#define PRIM_MAX   192      // v3.4 车道也用这套（多条车道图形较多）
#define ICON_BUF_W 360
#define ICON_BUF_H 360
struct Prim {
  uint8_t type;            // 0 = 线段（起点平头、终点圆头），1 = 三角形，2 = 圆
  uint16_t col;
  float ax, ay, bx, by, cx, cy, r;
  float x0, y0, x1, y1;    // 外框
};
extern Prim prims[PRIM_MAX];
extern int primN;
extern bool recIcon;       // true = 正在记录图标，thickLine/arrowHead/iconCircle 只记录不直接画
extern bool iconHalo;      // v2.8 图标外加黑色描边（浮在地图上时用）

void iconBegin();
void iconEnd();
void iconFit(float top, float bottom, float maxW);
void iconFitAt(float cx, float top, float bottom, float maxW);
float primAlpha(const struct Prim &p, float px, float py);   // 一个像素中心被图形覆盖的比例 0~1
void iconCircle(float x, float y, float r, uint16_t c);
void thickLine(float x0, float y0, float x1, float y1, float w, uint16_t c);
void arrowHead(float x, float y, float deg, float s, uint16_t c);

// ===== 基础绘图工具（实现在 MotoNav.cpp）=====
int16_t rnd(float v);
void printCentered(const String &s, int x, int y, int size, uint16_t color);
void printCentered(const char *s, int x, int y, int size, uint16_t color);
void distTextBuf(int m, char *out);            // out 至少 12 字节；distText 的栈缓冲版
String distText(int m);
uint16_t blend565(uint16_t a, uint16_t b, int t);   // 两个 RGB565 颜色按 t（0~255）混合（实现在 NavMap.cpp）
void aaDisc(float cx, float cy, float rad, uint16_t c);   // 抗锯齿实心圆（实现在 NavMap.cpp）

// ===== 车道（实现在 NavMap.cpp）=====
void drawLanesAt(float centerX, float y0, float maxW, float h);

// ===== 状态栏（实现在 MotoNav.cpp）=====
void drawStatus();
void drawBattery(int x0, int y0, bool backing = false);
void drawTextImg(int slot, int y0, uint8_t tint);
void drawWaiting(int st);

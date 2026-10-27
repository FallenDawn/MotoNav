// Arduino_GFX 桩实现：画布图元 + 经典 5x7 字体 + 面板推屏
#include "Arduino_GFX_Library.h"
#include "sim_state.h"
#include <cstring>
#include <cstdlib>

// 经典 Adafruit 5x7 字体（来自 vendored GFX 库的 glcdfont.h，static 数组单 TU 可见）
#include <glcdfont.h>

Arduino_Canvas::Arduino_Canvas(int16_t w, int16_t h, Arduino_G *output)
    : _w(w), _h(h), _max_x(w - 1), _max_y(h - 1), _output(output) {}

Arduino_Canvas::~Arduino_Canvas() { free(_framebuffer); }

bool Arduino_Canvas::begin(int32_t speed) {
  (void)speed;
  if (!_framebuffer) _framebuffer = (uint16_t *)calloc((size_t)_w * _h, 2);
  return _framebuffer != nullptr;
}
void Arduino_Canvas::setFramebuffer(uint16_t *f) { _framebuffer = f; }
uint16_t *Arduino_Canvas::getFramebuffer() { return _framebuffer; }
void Arduino_Canvas::flush(bool force) { (void)force; }   // SwapCanvas 覆盖了 flush

void Arduino_Canvas::drawPixel(int16_t x, int16_t y, uint16_t color) {
  if (!_framebuffer || x < 0 || y < 0 || x > _max_x || y > _max_y) return;
  _framebuffer[(int32_t)y * _w + x] = color;
}

void Arduino_Canvas::fillScreen(uint16_t color) {
  if (!_framebuffer) return;
  for (int32_t i = 0; i < (int32_t)_w * _h; i++) _framebuffer[i] = color;
}

void Arduino_Canvas::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  for (int16_t j = y; j < y + h; j++)
    for (int16_t i = x; i < x + w; i++) drawPixel(i, j, color);
}

void Arduino_Canvas::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  drawFastHLine(x, y, w, color);
  drawFastHLine(x, y + h - 1, w, color);
  drawFastVLine(x, y, h, color);
  drawFastVLine(x + w - 1, y, h, color);
}

void Arduino_Canvas::drawFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) {
  fillRect(x, y, w, 1, color);
}
void Arduino_Canvas::drawFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) {
  fillRect(x, y, 1, h, color);
}

void Arduino_Canvas::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color) {
  // Bresenham（Adafruit 风格，含陡线处理）
  bool steep = abs((int)y1 - y0) > abs((int)x1 - x0);
  if (steep) { int16_t t; t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
  if (x0 > x1) {
    int16_t t; t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t;
  }
  int16_t dx = x1 - x0, dy = abs(y1 - y0);
  int16_t err = dx / 2;
  int16_t ystep = y0 < y1 ? 1 : -1;
  for (; x0 <= x1; x0++) {
    if (steep) drawPixel(y0, x0, color); else drawPixel(x0, y0, color);
    err -= dy;
    if (err < 0) { y0 += ystep; err += dx; }
  }
}

void Arduino_Canvas::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2,
                                  int16_t y2, uint16_t color) {
  // Adafruit-GFX 扫描线算法
  int16_t a, b, y, last;
  if (y0 > y1) { int16_t t; t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
  if (y1 > y2) { int16_t t; t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
  if (y0 > y1) { int16_t t; t = y0; y0 = y1; y1 = t; t = x0; x0 = x1; x1 = t; }
  if (y0 == y2) {
    a = x0; b = x0;
    if (x1 < a) a = x1; else if (x1 > b) b = x1;
    if (x2 < a) a = x2; else if (x2 > b) b = x2;
    drawFastHLine(a, y0, b - a + 1, color);
    return;
  }
  int16_t dx01 = x1 - x0, dy01 = y1 - y0, dx02 = x2 - x0, dy02 = y2 - y0, dx12 = x2 - x1,
          dy12 = y2 - y1;
  int32_t sa = 0, sb = 0;
  if (y1 == y2) last = y1;
  else last = y1 - 1;
  for (y = y0; y <= last; y++) {
    a = x0 + sa / dy01;
    b = x0 + sb / dy02;
    sa += dx01;
    sb += dx02;
    if (a > b) { int16_t t; t = a; a = b; b = t; }
    drawFastHLine(a, y, b - a + 1, color);
  }
  sa = dx12 * (y - y1);
  sb = dx02 * (y - y0);
  for (; y <= y2; y++) {
    a = x1 + sa / dy12;
    b = x0 + sb / dy02;
    sa += dx12;
    sb += dx02;
    if (a > b) { int16_t t; t = a; a = b; b = t; }
    drawFastHLine(a, y, b - a + 1, color);
  }
}

void Arduino_Canvas::drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
  int16_t f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
  while (x < y) {
    if (f >= 0) { y--; ddy += 2; f += ddy; }
    x++; ddx += 2; f += ddx;
    drawPixel(x0 + x, y0 + y, color);
    drawPixel(x0 - x, y0 + y, color);
    drawPixel(x0 + x, y0 - y, color);
    drawPixel(x0 - x, y0 - y, color);
    drawPixel(x0 + y, y0 + x, color);
    drawPixel(x0 - y, y0 + x, color);
    drawPixel(x0 + y, y0 - x, color);
    drawPixel(x0 - y, y0 - x, color);
  }
}

void Arduino_Canvas::fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color) {
  drawFastVLine(x0, y0 - r, 2 * r + 1, color);
  int16_t f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
  while (x < y) {
    if (f >= 0) { y--; ddy += 2; f += ddy; }
    x++; ddx += 2; f += ddx;
    drawFastHLine(x0 - x, y0 + y, 2 * x + 1, color);
    drawFastHLine(x0 - x, y0 - y, 2 * x + 1, color);
    drawFastHLine(x0 - y, y0 + x, 2 * y + 1, color);
    drawFastHLine(x0 - y, y0 - x, 2 * y + 1, color);
  }
}

void Arduino_Canvas::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                                   uint16_t color) {
  int16_t maxR = (w < h ? w : h) / 2;
  if (r > maxR) r = maxR;
  drawFastHLine(x + r, y, w - 2 * r, color);
  drawFastHLine(x + r, y + h - 1, w - 2 * r, color);
  drawFastVLine(x, y + r, h - 2 * r, color);
  drawFastVLine(x + w - 1, y + r, h - 2 * r, color);
  int16_t f = 1 - r, ddx = 1, ddy = -2 * r, px = 0, py = r;
  while (px < py) {
    if (f >= 0) { py--; ddy += 2; f += ddy; }
    px++; ddx += 2; f += ddx;
    drawPixel(x + r - px, y + r - py, color);
    drawPixel(x + w - r - 1 + px, y + r - py, color);
    drawPixel(x + r - px, y + h - r - 1 + py, color);
    drawPixel(x + w - r - 1 + px, y + h - r - 1 + py, color);
  }
}

void Arduino_Canvas::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r,
                                   uint16_t color) {
  int16_t maxR = (w < h ? w : h) / 2;
  if (r > maxR) r = maxR;
  fillRect(x + r, y, w - 2 * r, h, color);
  // 上下两条边（Adafruit fillCircleHelper 的 corners=1/2 各管两侧半圆）
  fillCircleHelper(x + w - r - 1, y + r, r, 1, h - 2 * r - 1, color);
  fillCircleHelper(x + r, y + r, r, 2, h - 2 * r - 1, color);
}

// 圆角四角 1/4 圆填充（Adafruit 算法，corners=1 右侧 / 2 左侧）
void Arduino_Canvas::fillCircleHelper(int16_t x0, int16_t y0, int16_t r, int16_t corners,
                                      int16_t delta, uint16_t color) {
  int16_t f = 1 - r, ddx = 1, ddy = -2 * r, x = 0, y = r;
  while (x < y) {
    if (f >= 0) { y--; ddy += 2; f += ddy; }
    x++; ddx += 2; f += ddx;
    if (corners & 1) {
      drawFastVLine(x0 + x, y0 - y, 2 * y + delta + 1, color);
      drawFastVLine(x0 + y, y0 - x, 2 * x + delta + 1, color);
    }
    if (corners & 2) {
      drawFastVLine(x0 - x, y0 - y, 2 * y + delta + 1, color);
      drawFastVLine(x0 - y, y0 - x, 2 * x + delta + 1, color);
    }
  }
}

void Arduino_Canvas::setTextSize(uint8_t s) { _textSizeX = _textSizeY = s ? s : 1; }
void Arduino_Canvas::setTextColor(uint16_t color) { _textFg = color; _textBg = color; _textOpaque = false; }
void Arduino_Canvas::setTextColor(uint16_t fg, uint16_t bg) {
  _textFg = fg; _textBg = bg; _textOpaque = true;
}
void Arduino_Canvas::setCursor(int16_t x, int16_t y) { _cursorX = x; _cursorY = y; }

void Arduino_Canvas::drawChar(int16_t x, int16_t y, uint8_t c, uint16_t fg, uint16_t bg,
                              uint8_t size) {
  for (uint8_t i = 0; i < 6; i++) {
    uint8_t line = (i < 5) ? pgm_read_byte(&font[c * 5 + i]) : 0;
    // glcdfont 每列最低位对应顶部像素，与固件 Arduino_GFX 的位序一致。
    for (int8_t j = 0; j < 8; j++, line >>= 1) {
      if (line & 1 || (_textOpaque && bg != fg)) {
        if (size == 1) drawPixel(x + i, y + j, (line & 1) ? fg : bg);
        else fillRect(x + i * size, y + j * size, size, size, (line & 1) ? fg : bg);
      }
    }
  }
}

size_t Arduino_Canvas::write(uint8_t c) {
  if (c == '\n') {
    _cursorY += _textSizeY * 8;
    _cursorX = 0;
  } else if (c == '\r') {
    _cursorX = 0;
  } else {
    drawChar(_cursorX, _cursorY, c, _textFg, _textBg, _textSizeX);
    _cursorX += _textSizeX * 6;
  }
  return 1;
}
size_t Arduino_Canvas::print(const String &s) { return print(s.c_str()); }
size_t Arduino_Canvas::print(const char *s) {
  size_t n = 0;
  while (s && *s) n += write((uint8_t)*s++);
  return n;
}
size_t Arduino_Canvas::print(char c) { return write((uint8_t)c); }
size_t Arduino_Canvas::print(int v) { char b[16]; snprintf(b, sizeof(b), "%d", v); return print(b); }
size_t Arduino_Canvas::print(unsigned int v) { char b[16]; snprintf(b, sizeof(b), "%u", v); return print(b); }
size_t Arduino_Canvas::print(long v) { char b[24]; snprintf(b, sizeof(b), "%ld", v); return print(b); }
size_t Arduino_Canvas::print(unsigned long v) { char b[24]; snprintf(b, sizeof(b), "%lu", v); return print(b); }
size_t Arduino_Canvas::print(double v, int digits) {
  char b[32];
  snprintf(b, sizeof(b), "%.*f", digits, v);
  return print(b);
}

void Arduino_Canvas::draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w,
                                        int16_t h) {
  if (!_framebuffer) return;
  for (int16_t j = 0; j < h; j++)
    for (int16_t i = 0; i < w; i++) drawPixel(x + i, y + j, bitmap[j * w + i]);
}

// ---- 面板推屏（无快速刷屏时的回退路径）：直接落到 SDL 物理缓冲 ----
void Arduino_ST77916::draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w,
                                         int16_t h) {
  (void)x; (void)y;
  if (w != _w || h != _h) return;   // 固件只整屏推
  std::lock_guard<std::mutex> lk(sim_phys_mu);
  memcpy(sim_phys, bitmap, (size_t)w * h * 2);
}

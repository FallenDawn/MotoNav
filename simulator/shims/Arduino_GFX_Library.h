// PC/SDL 模拟器桩：Arduino_GFX 图形库。
// 只实现 MotoNav 固件用到的子集；Canvas 的绘制全部落到 RGB565 内存帧缓冲。
// 固件自研的抗锯齿图元（aaDisc/Prim/PortScale）都在固件源码里，不经这里。
#pragma once
#include "Arduino.h"
#include "esp_shim.h"
#include "driver/spi_master.h"

class Arduino_G {
public:
  virtual ~Arduino_G() {}
};

// batchOperation 字节流操作码（与真实 Arduino_DataBus.h 一致）
typedef enum {
  BEGIN_WRITE,
  WRITE_COMMAND_8,
  WRITE_COMMAND_16,
  WRITE_COMMAND_BYTES,
  WRITE_DATA_8,
  WRITE_DATA_16,
  WRITE_BYTES,
  WRITE_C8_D8,
  WRITE_C8_D16,
  WRITE_C8_BYTES,
  WRITE_C16_D16,
  END_WRITE,
  DELAY,
} spi_operation_type_t;

class Arduino_DataBus {
public:
  virtual ~Arduino_DataBus() {}
  virtual bool begin(int32_t speed = 0, int8_t dataMode = -1) { (void)speed; (void)dataMode; return true; }
  virtual void beginWrite() {}
  virtual void endWrite() {}
  virtual void writeCommand(uint8_t c) { (void)c; }
  virtual void writeCommand16(uint16_t c) { (void)c; }
  virtual void writeCommandBytes(uint8_t *data, uint32_t len) { (void)data; (void)len; }
  virtual void write(uint8_t d) { (void)d; }
  virtual void write16(uint16_t d) { (void)d; }
  virtual void writeC8D8(uint8_t c, uint8_t d) { (void)c; (void)d; }
  virtual void writeC8D16(uint8_t c, uint16_t d) { (void)c; (void)d; }
  virtual void writeC8D16D16(uint8_t c, uint16_t d1, uint16_t d2) { (void)c; (void)d1; (void)d2; }
  virtual void batchOperation(const uint8_t *ops, size_t len) { (void)ops; (void)len; }
  virtual void sendCommand(uint8_t c) { (void)c; }
};

class Arduino_ESP32QSPI : public Arduino_DataBus {
public:
  Arduino_ESP32QSPI(int cs, int sck, int d0, int d1, int d2, int d3, bool shared = true) {
    (void)cs; (void)sck; (void)d0; (void)d1; (void)d2; (void)d3; (void)shared;
  }
};

// ---- 内存画布（对应 Arduino_Canvas：begin 分配帧缓冲，所有图元写进缓冲）----
class Arduino_Canvas : public Arduino_G {
public:
  Arduino_Canvas(int16_t w, int16_t h, Arduino_G *output);
  virtual ~Arduino_Canvas();

  bool begin(int32_t speed = 0);
  virtual void setFramebuffer(uint16_t *f);
  virtual uint16_t *getFramebuffer();
  virtual void flush(bool force = false);

  void drawPixel(int16_t x, int16_t y, uint16_t color);
  void fillScreen(uint16_t color);
  void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color);
  void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
  void drawFastHLine(int16_t x, int16_t y, int16_t h, uint16_t color);
  void drawFastVLine(int16_t x, int16_t y, int16_t w, uint16_t color);
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1, int16_t x2, int16_t y2,
                    uint16_t color);
  void drawCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
  void fillCircle(int16_t x0, int16_t y0, int16_t r, uint16_t color);
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
  void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint16_t color);
  void draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h);

  void setTextSize(uint8_t s);
  void setTextColor(uint16_t color);
  void setTextColor(uint16_t fg, uint16_t bg);
  void setCursor(int16_t x, int16_t y);
  size_t write(uint8_t c);
  size_t print(const String &s);
  size_t print(const char *s);
  size_t print(char c);
  size_t print(int v);
  size_t print(unsigned int v);
  size_t print(long v);
  size_t print(unsigned long v);
  size_t print(double v, int digits = 2);

  int16_t width() const { return _w; }
  int16_t height() const { return _h; }

protected:
  void drawChar(int16_t x, int16_t y, uint8_t c, uint16_t fg, uint16_t bg, uint8_t size);
  void fillCircleHelper(int16_t x0, int16_t y0, int16_t r, int16_t corners, int16_t delta,
                        uint16_t color);
  int16_t _w, _h, _max_x, _max_y;
  Arduino_G *_output;
  uint16_t *_framebuffer = nullptr;
  uint8_t _textSizeX = 1, _textSizeY = 1;
  uint16_t _textFg = 0xFFFF, _textBg = 0x0000;
  bool _textOpaque = false;
  int16_t _cursorX = 0, _cursorY = 0;
};

// ---- ST77916 面板（Board185BPanel 的基类）：推屏方法落到 SDL 物理缓冲 ----
class Arduino_ST77916 : public Arduino_G {
public:
  Arduino_ST77916(Arduino_DataBus *bus, int rst, int rotation, bool ips, int16_t w, int16_t h)
      : _bus(bus), _w(w), _h(h) {
    (void)rst; (void)rotation; (void)ips;
  }
  bool begin(int32_t speed = 0) { (void)speed; return true; }   // 面板初始化在模拟器上跳过
  void startWrite() {}
  void endWrite() {}
  void writeAddrWindow(int16_t x, int16_t y, int16_t w, int16_t h) { (void)x; (void)y; (void)w; (void)h; }
  void writePixels(const uint16_t *pixels, uint32_t count) { (void)pixels; (void)count; }
  void draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t *bitmap, int16_t w, int16_t h);
  void displayOff() {}
  virtual void tftInit() {}

protected:
  Arduino_DataBus *_bus;
  int16_t _w, _h;
};

// PC/SDL 模拟器桩：Arduino 核心 API。实现见 arduino_shim.cpp。
// min/max 与 ESP32 核心一致：不用宏，用 std:: 模板（mixed-type 调用编译期即报错）。
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdbool.h>
#include <algorithm>
using std::min;
using std::max;
using std::abs;
using std::isinf;
using std::isnan;

#include "WString.h"
#include "pgmspace.h"

#define IRAM_ATTR
typedef uint8_t byte;
typedef bool boolean;

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define CHANGE 1
#define FALLING 2
#define RISING 3

#ifndef PI
#define PI 3.1415926535897932384626433832795f
#endif
#define DEG_TO_RAD 0.017453292519943295f
#define RAD_TO_DEG 57.29577951308232f

#define constrain(x, l, h) ((x) < (l) ? (l) : ((x) > (h) ? (h) : (x)))
#define sq(x) ((x) * (x))

long map(long x, long inMin, long inMax, long outMin, long outMax);

// ---- 时间 ----
unsigned long millis();
unsigned long micros();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void yield();

// ---- GPIO（模拟器上都是空操作）----
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t value);
int digitalRead(uint8_t pin);
void attachInterrupt(uint8_t pin, void (*isr)(), int mode);
void detachInterrupt(uint8_t pin);

// ---- LEDC 背光 PWM：空操作 ----
bool ledcAttach(uint8_t pin, uint32_t freq, uint8_t resolution);
void ledcWrite(uint8_t pin, uint32_t duty);
void ledcDetach(uint8_t pin);

// ---- 伪随机 ----
void randomSeed(unsigned long seed);
long random(long howbig);
long random(long howsmall, long howbig);

// ---- ESP 对象 ----
class EspShim {
public:
  uint32_t getFreeHeap();
  uint32_t getFreePsram();
  void restart();   // 打印提示后退出模拟器
};
extern EspShim ESP;

// ---- 串口（stdout）----
class SerialShim {
public:
  void begin(unsigned long baud) { (void)baud; }
  void setTxTimeoutMs(long ms) { (void)ms; }
  size_t printf(const char *fmt, ...);
  size_t print(const String &s);
  size_t print(const char *s);
  size_t print(char c);
  size_t print(int v, int base = 10);
  size_t print(unsigned int v, int base = 10);
  size_t print(long v, int base = 10);
  size_t print(unsigned long v, int base = 10);
  size_t print(double v, int digits = 2);
  size_t print(bool v);
  size_t println();
  size_t println(const String &s);
  size_t println(const char *s);
  size_t println(char c);
  size_t println(int v, int base = 10);
  size_t println(unsigned int v, int base = 10);
  size_t println(long v, int base = 10);
  size_t println(unsigned long v, int base = 10);
  size_t println(double v, int digits = 2);
  size_t println(bool v);
};
extern SerialShim Serial;

// ---- FreeRTOS 任务/信号量（真线程实现）----
#include "freertos/FreeRTOS.h"

// ---- PSRAM / heap_caps（PC 上全是普通 malloc）----
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_INTERNAL 4
#define MALLOC_CAP_8BIT 8
#define MALLOC_CAP_32BIT 16
void *heap_caps_malloc(size_t size, int caps);
void *heap_caps_calloc(size_t n, size_t size, int caps);
void *heap_caps_aligned_alloc(size_t align, size_t size, int caps);
void heap_caps_free(void *p);
static inline void *ps_malloc(size_t n) { return malloc(n); }
static inline void *ps_calloc(size_t n, size_t size) { return calloc(n, size); }

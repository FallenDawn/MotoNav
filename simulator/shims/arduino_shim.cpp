// Arduino 核心 API 的 PC 实现
#include "Arduino.h"
#include "esp_shim.h"
#include "esp_system_sleep.h"
#include "Wire.h"
#include <chrono>
#include <cstdio>
#include <cstdarg>
#include <thread>
#include <map>

// ---- 时钟 ----
static auto sim_start = std::chrono::steady_clock::now();
unsigned long millis() {
  return (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - sim_start)
      .count();
}
unsigned long micros() {
  return (unsigned long)std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now() - sim_start)
      .count();
}
void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
void delayMicroseconds(uint32_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
void yield() {}

long map(long x, long inMin, long inMax, long outMin, long outMax) {
  return (x - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

// ---- GPIO ----
static std::map<int, void (*)()> g_isrs;
void pinMode(uint8_t pin, uint8_t mode) { (void)pin; (void)mode; }
void digitalWrite(uint8_t pin, uint8_t value) { (void)pin; (void)value; }
int digitalRead(uint8_t pin) { (void)pin; return 0; }
void attachInterrupt(uint8_t pin, void (*isr)(), int mode) { (void)mode; g_isrs[pin] = isr; }
void detachInterrupt(uint8_t pin) { g_isrs.erase(pin); }

// LEDC
bool ledcAttach(uint8_t pin, uint32_t freq, uint8_t res) { (void)pin; (void)freq; (void)res; return true; }
void ledcWrite(uint8_t pin, uint32_t duty) { (void)pin; (void)duty; }
void ledcDetach(uint8_t pin) { (void)pin; }

// ---- 随机 ----
void randomSeed(unsigned long seed) { srand((unsigned)seed); }
long random(long howbig) { return howbig <= 0 ? 0 : rand() % howbig; }
long random(long howsmall, long howbig) {
  if (howbig <= howsmall) return howsmall;
  return howsmall + rand() % (howbig - howsmall);
}

// ---- ESP ----
uint32_t EspShim::getFreeHeap() { return 200 * 1024 * 1024; }
uint32_t EspShim::getFreePsram() { return 8 * 1024 * 1024; }
void EspShim::restart() {
  printf("[SIM] ESP.restart() called - exiting (re-run the simulator to 'reboot')\n");
  exit(3);
}

// ---- 串口 ----
SerialShim Serial;
EspShim ESP;

// ---- stdlib_noniso 声明但 ROM 提供、PC 上缺的转换函数（extern "C"）----
extern "C" char *utoa(unsigned value, char *result, int base) {
  if (base < 2 || base > 36) { *result = 0; return result; }
  char *ptr = result, *ptr1 = result, tmp_char;
  unsigned tmp_value;
  do { tmp_value = value; value /= (unsigned)base; *ptr++ = "zyxwvutsrqponmlkjihgfedcba9876543210123456789abcdefghijklmnopqrstuvwxyz"[35 + (int)(tmp_value - value * (unsigned)base)]; } while (value);
  *ptr-- = 0;
  while (ptr1 < ptr) { tmp_char = *ptr; *ptr = *ptr1; *ptr1 = tmp_char; ptr--; ptr1++; }
  return result;
}
extern "C" char *itoa(int value, char *result, int base) {
  char *p = result;
  if (base == 10 && value < 0) { *p++ = '-'; value = -value; }
  utoa((unsigned)value, p, base);
  return result;
}
size_t SerialShim::printf(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vprintf(fmt, ap);
  va_end(ap);
  fflush(stdout);
  return (size_t)(n < 0 ? 0 : n);
}
size_t SerialShim::print(const String &s) { return printf("%s", s.c_str()); }
size_t SerialShim::print(const char *s) { return printf("%s", s); }
size_t SerialShim::print(char c) { return printf("%c", c); }
size_t SerialShim::print(int v, int base) {
  if (base == 16) return printf("%X", v);
  return printf("%d", v);
}
size_t SerialShim::print(unsigned int v, int base) {
  if (base == 16) return printf("%X", v);
  return printf("%u", v);
}
size_t SerialShim::print(long v, int base) {
  if (base == 16) return printf("%lX", (unsigned long)v);
  return printf("%ld", v);
}
size_t SerialShim::print(unsigned long v, int base) {
  if (base == 16) return printf("%lX", v);
  return printf("%lu", v);
}
size_t SerialShim::print(double v, int digits) {
  return printf("%.*f", digits, v);
}
size_t SerialShim::print(bool v) { return printf("%d", (int)v); }
size_t SerialShim::println() { return printf("\n"); }
size_t SerialShim::println(const String &s) { return print(s) + printf("\n"); }
size_t SerialShim::println(const char *s) { return print(s) + printf("\n"); }
size_t SerialShim::println(char c) { return print(c) + printf("\n"); }
size_t SerialShim::println(int v, int base) { return print(v, base) + printf("\n"); }
size_t SerialShim::println(unsigned int v, int base) { return print(v, base) + printf("\n"); }
size_t SerialShim::println(long v, int base) { return print(v, base) + printf("\n"); }
size_t SerialShim::println(unsigned long v, int base) { return print(v, base) + printf("\n"); }
size_t SerialShim::println(double v, int digits) { return print(v, digits) + printf("\n"); }
size_t SerialShim::println(bool v) { return print(v) + printf("\n"); }

// ---- FreeRTOS 任务/信号量 ----
SemaphoreHandle_t xSemaphoreCreateBinary() { return new SimSemaphore(); }
SemaphoreHandle_t xSemaphoreCreateMutex() { return new SimSemaphore(); }
void xSemaphoreGive(SemaphoreHandle_t s) {
  if (!s) return;
  std::lock_guard<std::mutex> lk(s->m);
  s->count = 1;
  s->cv.notify_one();
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
  if (!s) return pdTRUE;
  std::unique_lock<std::mutex> lk(s->m);
  if (ticks == portMAX_DELAY) {
    s->cv.wait(lk, [&] { return s->count > 0; });
    s->count = 0;
    return pdTRUE;
  }
  bool got = s->cv.wait_for(lk, std::chrono::milliseconds(ticks), [&] { return s->count > 0; });
  if (got) { s->count = 0; return pdTRUE; }
  return pdFALSE;
}

struct SimTask { TaskFunction_t fn; void *arg; std::string name; };
static void sim_task_body(SimTask *t) {
  t->fn(t->arg);
  delete t;
}
static BaseType_t sim_task_create(TaskFunction_t fn, const char *name, void *arg,
                                  TaskHandle_t *handle) {
  auto *t = new SimTask{fn, arg, name ? name : "task"};
  std::thread th(sim_task_body, t);
  th.detach();
  if (handle) *handle = (TaskHandle_t)1;   // 非空占位：固件只拿它判断任务是否在跑
  return pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *handle, BaseType_t core) {
  (void)stack; (void)prio; (void)core;
  return sim_task_create(fn, name, arg, handle);
}
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg,
                       UBaseType_t prio, TaskHandle_t *handle) {
  (void)stack; (void)prio;
  return sim_task_create(fn, name, arg, handle);
}
void vTaskDelete(TaskHandle_t task) { (void)task; }
void vTaskDelay(TickType_t ticks) { delay(ticks); }
void taskYIELD() {}

std::recursive_mutex *sim_portmux() {
  static std::recursive_mutex m;
  return &m;
}

// ---- heap_caps / PSRAM ----
void *heap_caps_malloc(size_t size, int caps) { (void)caps; return malloc(size); }
void *heap_caps_calloc(size_t n, size_t size, int caps) { (void)caps; return calloc(n, size); }
void *heap_caps_aligned_alloc(size_t align, size_t size, int caps) {
  (void)align; (void)caps; return malloc(size);
}
void heap_caps_free(void *p) { free(p); }

// ---- esp 杂项 ----
uint32_t esp_random() { return (uint32_t)rand() << 16 ^ (uint32_t)millis(); }
int esp_reset_reason() { return 1; }
void esp_restart() { ESP.restart(); }
esp_sleep_source_t esp_sleep_get_wakeup_cause() { return ESP_SLEEP_WAKEUP_UNDEFINED; }
void esp_deep_sleep_start() {
  printf("[SIM] deep sleep requested (idle power off) - exiting\n");
  exit(0);
}
esp_err_t esp_deep_sleep(uint64_t us) { (void)us; esp_deep_sleep_start(); return ESP_OK; }
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us) { (void)us; return ESP_OK; }
esp_err_t esp_sleep_disable_wakeup_source(int source) { (void)source; return ESP_OK; }
esp_err_t esp_sleep_enable_ext0_wakeup(int gpio, int level) { (void)gpio; (void)level; return ESP_OK; }
esp_err_t esp_read_mac(uint8_t *mac, int type) {
  (void)type;
  if (mac) { mac[0]=0x24; mac[1]=0x6F; mac[2]=0x28; mac[3]=0xA1; mac[4]=0xB2; mac[5]=0xC3; }
  return ESP_OK;
}
esp_err_t esp_efuse_mac_get_default(uint8_t *mac) { return esp_read_mac(mac, 0); }

// ---- I2C ----
TwoWire Wire;

// ---- 触摸 ISR ----
void sim_call_touch_isr() {
  auto it = g_isrs.find(4);   // TP_INT = 4（pin_config.h）
  if (it != g_isrs.end()) it->second();
}

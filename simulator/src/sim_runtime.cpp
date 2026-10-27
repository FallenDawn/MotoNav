// 桌面/网页版共用的运行时：全局状态、触摸馈送、合成滑动、事件处理、截图。
// sim_main.cpp（桌面 main）和 web_main.cpp（Emscripten）都链接这里。
#include "sim_state.h"
#include <SDL.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

uint16_t sim_phys[360 * 360];
std::mutex sim_phys_mu;
std::atomic<bool> sim_running{true};
std::atomic<bool> sim_touch_down{false};
std::atomic<int> sim_touch_x{0}, sim_touch_y{0};
int sim_window_scale = 2;

void sim_call_touch_isr();       // arduino_shim.cpp

// 固件入口（HOST_SIM 钩子块提供）
extern void hostSimSetLink(bool conn, bool auth);
extern void hostSimInjectRx(const uint8_t *data, size_t n);
extern void setup();
extern void loop();

// ---- 键盘注入的合成触摸拖动（页面切换）----
struct SwipeInject {
  bool active = false;
  int step = 0;
  int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
} g_swipe;

void feed_touch(bool down, int px, int py) {
  if (down && !sim_touch_down.load()) sim_call_touch_isr();   // 触发 onTouchIrq
  sim_touch_down = down;
  sim_touch_x = px;
  sim_touch_y = py;
}

static void tick_swipe() {
  if (!g_swipe.active) return;
  if (g_swipe.step == 0) {
    feed_touch(true, g_swipe.x0, g_swipe.y0);
  } else {
    int t = g_swipe.step;
    int x = g_swipe.x0 + (g_swipe.x1 - g_swipe.x0) * t / 6;
    int y = g_swipe.y0 + (g_swipe.y1 - g_swipe.y0) * t / 6;
    feed_touch(true, x, y);
    if (t >= 6) {
      feed_touch(false, x, y);
      g_swipe.active = false;
      return;
    }
  }
  g_swipe.step++;
}

void start_swipe(int dir) {
  g_swipe.active = true;
  g_swipe.step = 0;
  if (dir > 0) { g_swipe.x0 = 300; g_swipe.y0 = 180; g_swipe.x1 = 60; g_swipe.y1 = 180; }
  else { g_swipe.x0 = 60; g_swipe.y0 = 180; g_swipe.x1 = 300; g_swipe.y1 = 180; }
}

void send_text(const char *s) { hostSimInjectRx((const uint8_t *)s, strlen(s)); }

// ---- SDL 事件处理（桌面/Web 共用）----
void sim_handle_event(const SDL_Event &e) {
  if (e.type == SDL_QUIT) sim_running = false;
  else if (e.type == SDL_KEYDOWN) {
    switch (e.key.keysym.sym) {
      case SDLK_ESCAPE: sim_running = false; break;
      case SDLK_RIGHT: start_swipe(1); break;
      case SDLK_LEFT: start_swipe(-1); break;
      case SDLK_c: {
        static bool on = false;
        on = !on;
        hostSimSetLink(on, on);
        printf("[SIM] link %s\n", on ? "ON (connected+authed)" : "OFF");
        break;
      }
      case SDLK_n: send_text("#n,1"); break;
      case SDLK_t: send_text("#t,1"); break;
      case SDLK_s: sim_write_bmp("screenshot.bmp"); break;
    }
  } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEMOTION ||
             e.type == SDL_MOUSEBUTTONUP) {
    if (e.button.which == SDL_TOUCH_MOUSEID) return;
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    int ww = 0, wh = 0;
    SDL_GetWindowSize(SDL_GetWindowFromID(e.button.window), &ww, &wh);
    int px = mx * 360 / (ww ? ww : 1);
    int py = my * 360 / (wh ? wh : 1);
    if (px < 0) px = 0; if (px > 359) px = 359;
    if (py < 0) py = 0; if (py > 359) py = 359;
    feed_touch(e.type != SDL_MOUSEBUTTONUP, px, py);
  }
}

// 360x360 RGB565 → 24bit BMP（桌面截图；Web 版用页面上的下载按钮）
void sim_write_bmp(const char *path) {
  std::lock_guard<std::mutex> lk(sim_phys_mu);
#pragma pack(push, 1)
  struct { char type[2]{'B','M'}; uint32_t size; uint16_t r1{0}, r2{0}; uint32_t off{54}; } fh;
  struct { uint32_t hsize{40}; int32_t w{360}; int32_t h{360}; uint16_t planes{1}; uint16_t bpp{24};
           uint32_t comp{0}; uint32_t imgsize; int32_t xppm{2835}, yppm{2835}; uint32_t c0{0}, c1{0}; } ih;
#pragma pack(pop)
  int rowBytes = (360 * 3 + 3) & ~3;
  ih.imgsize = rowBytes * 360;
  fh.size = 54 + ih.imgsize;
  FILE *f = fopen(path, "wb");
  if (!f) return;
  fwrite(&fh, 1, sizeof(fh), f);
  fwrite(&ih, 1, sizeof(ih), f);
  std::vector<unsigned char> row(rowBytes, 0);
  for (int y = 359; y >= 0; y--) {
    for (int x = 0; x < 360; x++) {
      uint16_t p = sim_phys[y * 360 + x];
      row[x * 3] = (unsigned char)((((p & 0x001F) << 3) | ((p & 0x001F) >> 2)));      // B
      row[x * 3 + 1] = (unsigned char)(((p & 0x07E0) >> 3) | ((p & 0x07E0) >> 9));    // G
      row[x * 3 + 2] = (unsigned char)(((p & 0xF800) >> 8) | ((p & 0xF800) >> 13));   // R
    }
    fwrite(row.data(), 1, rowBytes, f);
  }
  fclose(f);
  printf("[SIM] %s written\n", path);
}

// Web 版数据注入：UDP 不可用，用 WebSocket（EM_JS 轮询队列，主循环每帧取一次）。
// 消息格式与 UDP 版一致：手机协议文本行 / 二进制原样 / '!' 开头的模拟器控制指令。
#include "sim_state.h"
#include <emscripten.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern void hostSimSetLink(bool conn, bool auth);
extern void hostSimInjectRx(const uint8_t *data, size_t n);
void sim_set_battery(int pct, bool chg);
void sim_set_imu(float ax, float ay, float az, float gx, float gy, float gz);
void sim_web_download_shot();   // web_main.cpp

// ---- JS 侧：WebSocket 收到的消息推进全局队列，C 侧轮询取出 ----
EM_JS(void, sim_ws_start, (int port), {
  Module.simWsQueue = [];
  Module.simWsReady = false;
  function connect() {
    var ws = new WebSocket('ws://127.0.0.1:' + port + '/sim');
    ws.binaryType = 'arraybuffer';
    ws.onopen = function() { Module.simWsReady = true; console.log('[SIM-WS] connected to bridge :' + port); };
    ws.onmessage = function(ev) {
      if (typeof ev.data === 'string') { Module.simWsQueue.push(new TextEncoder().encode(ev.data)); }
      else { Module.simWsQueue.push(new Uint8Array(ev.data)); }
    };
    ws.onclose = function() { Module.simWsReady = false; setTimeout(connect, 2000); };
  }
  connect();
});

// 弹出一条消息到 buf；返回长度（0 = 没有）
EM_JS(int, sim_ws_pop, (char *buf, int maxlen), {
  var q = Module.simWsQueue;
  if (!q || q.length === 0) return 0;
  var msg = q.shift();
  var n = Math.min(msg.length, maxlen);
  for (var i = 0; i < n; i++) setValue(buf + i, msg[i], 'i8');
  return n;
});

static void handle_msg(const uint8_t *d, size_t n) {
  std::string s((const char *)d, n);
  if (!s.empty() && s[0] == '!') {
    float ax, ay, az;
    int conn, pct, chg;
    if (sscanf(s.c_str(), "!conn %d", &conn) == 1) hostSimSetLink(conn != 0, conn != 0);
    else if (sscanf(s.c_str(), "!auth %d", &conn) == 1) hostSimSetLink(true, conn != 0);
    else if (sscanf(s.c_str(), "!bat %d %d", &pct, &chg) >= 1) sim_set_battery(pct, chg != 0);
    else if (sscanf(s.c_str(), "!imu %f %f %f", &ax, &ay, &az) == 3) sim_set_imu(ax, ay, az, 0, 0, 0);
    else if (s.rfind("!shot", 0) == 0) sim_web_download_shot();
    return;
  }
  hostSimInjectRx(d, n);
}

void sim_net_start(int port) {
  sim_ws_start(port);
}

// 主循环每帧调用：取走队列里的所有消息
void sim_net_poll() {
  static uint8_t buf[4096];
  for (;;) {
    int n = sim_ws_pop((char *)buf, sizeof(buf));
    if (n <= 0) break;
    // 按 \n 分行（一个消息可带多条指令）
    uint8_t *start = buf;
    for (int i = 0; i <= n; i++) {
      if (i == n || buf[i] == '\n') {
        if (i > 0 && buf[i - 1] == '\r') buf[i - 1] = 0; else buf[i] = 0;
        size_t len = strlen((const char *)start);
        if (len) handle_msg(start, len);
        start = buf + i + 1;
      }
    }
  }
}

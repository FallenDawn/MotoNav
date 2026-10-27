// UDP 注入线程：把手机协议包（文本指令或二进制大块）投进固件的 RX 队列，
// 控制指令以 '!' 开头（模拟器专用，不进固件）。
#include "sim_state.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef int socklen_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
typedef int SOCKET;
#endif

extern void hostSimSetLink(bool conn, bool auth);
extern void hostSimInjectRx(const uint8_t *data, size_t n);
void sim_write_bmp(const char *path);   // sim_main.cpp
void sim_set_battery(int pct, bool chg);   // periph_shim.cpp
void sim_set_imu(float ax, float ay, float az, float gx, float gy, float gz);

static void handle_line(char *line) {
  if (line[0] == '!') {
    // 模拟器控制指令：!conn 1/0  !auth 1/0  !bat 78 1  !imu ax ay az
    float ax, ay, az;
    int conn, pct, chg;
    if (sscanf(line, "!conn %d", &conn) == 1) { hostSimSetLink(conn != 0, conn != 0); }
    else if (sscanf(line, "!auth %d", &conn) == 1) { hostSimSetLink(true, conn != 0); }
    else if (sscanf(line, "!bat %d %d", &pct, &chg) >= 1) { sim_set_battery(pct, chg != 0); }
    else if (sscanf(line, "!imu %f %f %f", &ax, &ay, &az) == 3) {
      sim_set_imu(ax, ay, az, 0, 0, 0);
    }
    else if (!strncmp(line, "!shot", 5)) sim_write_bmp("screenshot.bmp");
    else if (!strcmp(line, "!quit")) exit(0);
    return;
  }
  hostSimInjectRx((const uint8_t *)line, strlen(line));
}

void sim_net_start(int port) {
  std::thread([port]() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { printf("[NET] socket failed\n"); return; }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0) {
      printf("[NET] bind UDP %d failed\n", port);
      return;
    }
    printf("[NET] listening on UDP %d (phone protocol + !sim commands)\n", port);
    char buf[4096];
    for (;;) {
      sockaddr_in from{};
      socklen_t flen = sizeof(from);
      int n = recvfrom(sock, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &flen);
      if (n <= 0) continue;
      buf[n] = 0;
      // 按 \n 分行（一个数据报可带多条指令），去掉回车
      char *start = buf;
      for (int i = 0; i <= n; i++) {
        if (buf[i] == '\n' || i == n) {
          buf[i] = 0;
          size_t len = strlen(start);
          while (len && (start[len - 1] == '\r')) start[--len] = 0;
          if (len) handle_line(start);
          start = buf + i + 1;
        }
      }
    }
  }).detach();
}

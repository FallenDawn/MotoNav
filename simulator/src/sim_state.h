// 模拟器共享状态：物理帧缓冲、触摸馈送、运行标志。shim 层和 SDL 主程序共用。
#pragma once
#include <stdint.h>
#include <mutex>
#include <atomic>

// 物理 360x360 RGB565 帧（PortScale 缩放后的最终画面）
extern uint16_t sim_phys[360 * 360];
extern std::mutex sim_phys_mu;

// 模拟器主循环运行标志
extern std::atomic<bool> sim_running;

// 触摸馈送（物理面板 360x360 坐标，PortScale::touch 会换算到逻辑画布）
extern std::atomic<bool> sim_touch_down;
extern std::atomic<int> sim_touch_x, sim_touch_y;

// 触摸中断：SDL 按下鼠标时触发固件注册的 ISR（onTouchIrq）
void sim_call_touch_isr();

// 窗口缩放（1 = 360px 原大）
extern int sim_window_scale;

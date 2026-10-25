# MotoNav 高保真 SDL 桌面模拟器方案

## 目标与定位

用**真实固件渲染代码**在 PC 上跑一个 SDL2 桌面模拟器，替代现有 ui-editor 网页 mockup：
- 逐像素一致的预览（编译的是 `firmware/boards/185b/MotoNav/` 的真实源码，不是重写）
- 真实 setup()/loop() 状态机全部生效：页面滑动、开机动画、主题切换、日夜模式
- 数据用真实 BLE 协议（`R,300`/`#v,42`/`#W`...）经 UDP 注入——**真实 Android App 可以直接驱动模拟器**，实现固件+Android 双侧联调预览

架构：`固件源码（不改或近零改动） + shim 层（Arduino/ESP-IDF/BLE/GFX 桩） + SDL2 窗口`，通过 CMake 直接引用 `../firmware/boards/185b/MotoNav/*.cpp` 和 `firmware/libraries/miniz`。

## 已验证的关键事实

- 固件只调用 23 个 `gfx->` 方法，全部是简单图元，shim 半天可写完
- `flushLogicalFrame()`（MotoNav.cpp:836）中的 PortScale 缩放是纯 CPU 数学，可在 PC 运行；最后的 QSPI `fastPush` stub 成 SDL 上屏 → **零固件改动拿到全保真画面**
- 不定义 `CONFIG_NIMBLE_ENABLED` 可让 NimBLE 专属代码自然编译排除
- setup() 中 IMU/触摸/电量初始化均有失败兜底（`imuOK=false` 路径），shim 让它们"优雅失败"即可
- 项目已有构建时代码生成先例（select_src.py），BootAudioData.h 生成逻辑可复用

## 目录结构

```
simulator/
  CMakeLists.txt          # 引用固件源码 + shim include 路径置顶
  README.md               # 构建运行说明
  shims/                  # 桩层（include 路径最优先，覆盖 ESP 头文件名）
    Arduino.h             # millis/delay/String(移植 Arduino WString)/PROGMEM/map...
    Arduino_GFX_Library.h # Arduino_Canvas 子集：23 个方法 → 466×466 RGB565 缓冲
    glcdfont.c            # 直接复用 vendor 库里的 5×7 字体数据
    driver/spi_master.h   # spi_device_transmit → SDL 上屏（拦截 fastPush）
    driver/{i2s,rtc_io}.h, esp_*.h, host/ble_*.h   # 空/最小桩
    Wire.h, TouchDrv.hpp, GaugeDrv.hpp, SensorQMI8658.hpp  # 假传感器
    BLE*.h                # BLEDevice/BLEServer 等空壳类
    Preferences.h         # JSON 文件存取
    SPIFFS.h              # 挂载本地目录（背景图 motonav_bg.bin）
    FreeRTOS 桩           # xTaskCreatePinnedToCore→std::thread, 信号量→mutex/condvar
    ps_malloc/heap_caps   # → malloc
  src/
    main.cpp              # SDL 主循环，调用固件 setup()/loop()
    net_feed.cpp          # UDP 服务器：收手机协议包 → 注入 rxq（与 RxCB::onWrite 同路径）
    input.cpp             # 鼠标→触摸(合成 CST9217 事件)、键盘→切页/日夜/主题/IMU 假数据
    sim_glue.cpp          # 与固件全局变量对接（见"固件改动"）
  tools/
    demo_drive.py         # 脚本化导航会话（PIL 合成中文 TextImg 4bit 灰度位图、地图块）
    gen_boot_audio.py     # 复用 select_src.py 逻辑生成 BootAudioData.h（或要求先跑过一次固件构建）
```

## 实施阶段与验收里程碑

**阶段 0：基础设施**（半天）
- 强烈建议先 `git init`（当前项目无版本控制，大改动前必须留退路）
- CMake 工程 + vcpkg 安装 SDL2，空窗口跑通

**阶段 1：shim 层让固件源码编译通过**（1-2 天）
- 全部 shim 头文件 + GFX 23 方法实现（画进 RGB565 内存缓冲，含 5×7 字体）
- **M1 验收**：185b 三个 .cpp + miniz 在 PC 编译零错误（固件改动 ≤30 行）

**阶段 2：显示管线**（1 天）
- spi shim 拦截推屏 → 真实 PortScale 输出 360×360 → SDL 纹理放大显示；加截图热键
- **M2 验收**：开机品牌动画 → 首页等待画面（像素骑手动画+状态栏）正确渲染

**阶段 3：输入与数据**（1-2 天）
- 鼠标拖动 → 触摸滑动切页（走真实 touchPoll/dragMove 路径）；键盘快捷键
- UDP 协议注入 + demo_drive.py 演示脚本（不用手机也能看导航大图/小地图/车道/天气/音乐全页面）
- IMU 假数据发生器、电量模拟
- **M3 验收**：完整导航会话在模拟器里回放，小地图主题（12 套×日夜）可切换

**阶段 4：Android 侧联动**（1-2 天）
- Android App 加"开发者选项 → 模拟器直连"：把所有 BLE 下行包镜像到 UDP（sim 接收），并接收 sim 的上行通知（触摸/电量）→ 真实 App 端到端驱动模拟器
- Android App 加"演示模式"（ canned 位置/路线回放），配合 Android Studio 模拟器预览 App 自身 UI
- **M4 验收**：手机 App 操作 → PC 模拟器画面实时变化

**阶段 5：收尾**（半天）
- 旧 `ui-editor/` 移到 `tools/attic/ui-editor/`（无 git 不直接删），清掉垃圾文件 1.cmd
- 更新根 README 的编辑器章节为模拟器说明

## 预期固件改动（诚实声明）

目标为零， realistically ≤30 行：主要是 sim 访问 `rxq`（若为 static 需要一个 `#ifdef HOST_SIM` 暴露钩子）、`esp_deep_sleep` 桩防模拟器"关机"。全部用 `#ifdef HOST_SIM` 包裹，不影响设备构建。

## 主要风险与对策

- **String/任务并发**：WString 直接移植 Arduino 官方实现（纯 C++ 可移植）；FreeRTOS 任务用 std::thread + mutex，若出竞态可临时改单线程步进
- **AppleMedia/HID 残余耦合**：`bleConnected=false` 时这些 tick 大多自守卫；个别需要桩返回"未连接"
- **多板支持**：先做 185b（活跃板），175/C3 的单体 MotoNav.cpp 后续用同一 CMake 模式加 target

## 交付物

1. `simulator/` SDL 桌面模拟器（跑真实固件渲染，鼠标键盘可交互）
2. `simulator/tools/demo_drive.py` 脚本化演示（含 PIL 合成中文位图）
3. Android App：UDP 直连镜像 + 演示模式
4. 旧 ui-editor 归档、README 更新
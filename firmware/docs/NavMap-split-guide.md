# NavMap 拆分迁移指南（175 / esp32c3_128x160）

把 `MotoNav.cpp` 里高德地图/导航相关代码拆成独立 `NavMap.cpp` 的操作说明。
185b 板已完成（v7.13.1，2026-10），本文记录做法和所有坑，供 175 和 esp32c3_128x160 照做。

---

## 一、185b 拆分结果

| 文件 | 行数 | 职责 |
|---|---|---|
| `MotoNav.cpp` | 10076 → 3456 | 通信协议（handleText/handleMessage）、setup/loop、BLE 与配对、开机、触摸换页、天气/音乐/IMU/来电/到达特效页、文字图片接收 |
| `NavMap.cpp`（新） | 6681 | 地图/导航全部实现（其中约 3400 行是 CAM_ICONS/CAM_MARK 像素数据） |
| `NavMap.h`（新） | 117 | 导航状态变量 extern + 对外接口声明 |
| `MotoNavCommon.h`（新） | 111 | 画布 SwapCanvas、颜色/几何宏、Prim 图元系统、公共绘图函数声明 |

拆分原则（三板通用）：

1. **代码逐行搬移，不改逻辑**。唯一的改写是几处机械调整（见第六节）。
2. **全局状态留在声明处语义不变**：导航状态变量的*定义*随状态段搬进 NavMap.cpp，
   通过 NavMap.h 的 extern 给主文件用；画布/图元等公共变量的定义留在 MotoNav.cpp。
3. **不引入 setter/包装层**：主文件的 handleText 继续直接读写这些全局量。

拆什么、不拆什么（判断标准）：

- **搬走**：导航状态（方向/距离/车速/车道/行程/巡航/电子眼…）、地图主题与大块数据接收
  （handleBulk）、天地图栅格解码与双核渲染、路线解析与走过的路线、小地图整帧绘制
  （drawMapFrame）、大箭头画面（drawNavFrame）、进度环系统、车道绘制、电子眼/红绿灯/
  路况绘制、雷达暗角、风格特效、帧率统计。
- **留下**：handleText（协议总入口）、handleMessage、loop/setup、BLE、开机、触摸、
  各非导航页面、drawTextImg/handleBitmap（文字图片）。

---

## 二、准备

1. **备份**整个板子目录（`cp -r boards/175 boards/175.bak`）。本仓库不是 git 仓库。
2. **先编译一次记录基线**，拆完对比 Flash/RAM 应基本一致（±1KB 内正常，段边界可能差几行注释）：

   ```
   cd firmware
   pio run -e esp32c3_128x160        # c3 板
   pio run -e esp32s3-185b           # 185b（已完成，作对照）
   ```

3. **175 板注意：platformio.ini 里目前没有 175 的环境**（`scripts/select_src.py` 里有
   `"esp32s3-175": "175"` 映射，但 ini 缺对应 env，现在根本编不了）。迁移前先补：

   ```ini
   [env:esp32s3-175]
   board = esp32-s3-devkitc1-n16r8   ; 按 1.75 寸板实际芯片确认（应同为 S3R8）
   build_flags =
       ${env.build_flags}
       -DMOTONAV_BOARD_175=1
       -I boards/175/MotoNav
   lib_deps =
       ${env.lib_deps}
   ```

---

## 三、第一步：创建两个头文件

直接从 `boards/185b/MotoNav/` 复制 `MotoNavCommon.h` 和 `NavMap.h` 到目标板目录，然后按下面的差异清单改。

### MotoNavCommon.h 差异

| 项 | 175 / c3 的改法 |
|---|---|
| `#include "HomeAnimation.h"` | **删掉**。两板都没有这个头文件（没有骑手 GIF 动画模块），改为手写三行 extern（见下） |
| `extern bool onWaitScreen; extern bool sliding; extern int waitSt;` | **必须加**（原从 HomeAnimation.h 来）。两板这三个变量在自己 MotoNav.cpp 里内联定义，主文件定义保留不动，NavMap.cpp 靠 extern 使用 |
| `enum MotoNavPage` | 可删可留。两板用裸数字（`page == 0` 主页），枚举用不上；留着也无害 |
| `extern spi_device_handle_t fastDev;` 等其余内容 | 照抄（两板都有 fastDev、flushCnt 等） |

### NavMap.h 差异

NavMap.h 的每个 extern 都要在目标板 MotoNav.cpp 里 grep 确认存在，**没有的删掉**。已核实的差异：

- 两板都**没有** v7.13.2 红绿灯倒计时：删 `lightCdSec / lightCdPhase / lightCdAt` 三个 extern。
- c3 板没有 IdlePowerOff 模块（无该头文件）：如头里出现相关项则删（185b 版没写，无需动）。
- 其余（themeIdx/curveOn/nightOn/trailMode/bld3dOn/mapOn、posX 系列、traf 系列、MCAM 数组、
  ringActive、pendingAck 等）两板都有，照抄。
- `STAGE_FAR_M / STAGE_NEAR_M` 两个 define 照抄（主文件顶部有同值定义，重复定义同值宏合法；
  想干净也可以把主文件那份删掉）。
- `THEME_N` define 照抄（主文件 handleText 里 `t < THEME_N` 要用，定义已随小地图段搬走）。
- `MAP_RX_LOG` 宏：**两板的 handleBulk 都没有这个日志宏**（老分支），不用搬，头文件里也不用加。

---

## 四、第二步：按段搬代码

用锚点文本定位（行号仅供快速跳转，**以锚点为准**）。操作方式随意：编辑器手工剪切，
或写个 python/sed 脚本按行号切段（185b 是脚本切的）。每段从主文件剪切，按表顺序粘进
NavMap.cpp。**段内一行不改**（除第六节列出的机械调整）。

### 175 板（MotoNav.cpp 共 9726 行）

| # | 起锚点（含） | 止锚点（不含） | 行号 | 内容 |
|---|---|---|---|---|
| 1 | `// ===== 导航与行车状态 =====` | `// ===== 文字图片（手机渲染好的` | 353–396 | 导航状态变量。**370–371 两行（idlePowerOffTimer / powerOffPending）留下不搬** |
| 2 | `float min4(` | （max4 行，共 2 行） | 1078–1079 | min4/max4 |
| 3 | `void bentStroke(` | `// ================= 圆环` | 1109–1277 | bentStroke、大图标、远阶段预告 |
| 4 | `// ================= 圆环` | `// ================= v6.8 电池（AXP2101` | 1278–1496 | 圆环 LUT、aaDisc、blend565、fillRingBand、行程环、drawRedRing |
| 5 | `// ================= 车速和限速牌` | `// ================= 文字图片` | 1564–1753 | drawSpeedBlock、高德车道码、drawLanesAt |
| 6 | `// ---- 进度环动画 ----` | （4 行） | 3120–3123 | ringShown/ringTarget/ringActive/ringDir |
| 7 | `// ================= v2.0 小地图` | `String pendingStat2` | 3125–4116 | 主题表、Bulk/handleBulk、栅格地图、罪恶都市轮廓、3D 楼块、路线解析、背景图 |
| 8 | `bool mapModeActive()` | CAM 图标注释行（`#define CAMI_N` 的上一行） | 4212–4564 | 地图画图工具、电子眼占位框、红绿灯、车速/车道浮层、drawMapCams |
| 9 | CAM 图标注释行 | `#define CAMI_SPEED` 行（含） | 4565–8004 | CAM_ICONS / CAM_MARK 像素数据 |
| 10 | `void blitRGBA(` | `// ---- v5.9 外圈和边缘渐暗` | 8005–8434 | blitRGBA、电子眼图标摆放、雷达暗角、北向/终点标记、路况、提示条、HUD 占位 |
| 11 | `// ---- v5.9 外圈和边缘渐暗` | `int hexVal(char c)` | 8435–9010 | blendF、外圈整数渲染、风格特效、罗盘、drawMapFrame、统计、环动画、drawNavFrame |

### esp32c3_128x160 板（MotoNav.cpp 共 9732 行）

| # | 起锚点（含） | 止锚点（不含） | 行号 | 内容 |
|---|---|---|---|---|
| 1 | `// ===== 导航与行车状态 =====` | `// ===== 文字图片（手机渲染好的` | 376–418 | 同上（此板无 idlePowerOffTimer，整段搬） |
| 2 | `float min4(` | （2 行） | 967–968 | min4/max4 |
| 3 | `void bentStroke(` | `// ================= 圆环` | 998–1166 | 同 175 段 3 |
| 4 | `// ================= 圆环` | `// ================= 1.85B 电量计 BQ27220` | 1167–1419 | 同 175 段 4 |
| 5 | `// ================= 车速和限速牌` | `// ================= 文字图片` | 1472–1675 | 同 175 段 5 |
| 6 | `// ---- 进度环动画 ----` | （4 行） | 3024–3027 | 同 175 段 6 |
| 7 | `// ================= v2.0 小地图` | `String pendingStat2` | 3029–4165 | 同 175 段 7 |
| 8 | `bool mapModeActive()` | CAM 图标注释行 | 4247–4608 | 同 175 段 8 |
| 9 | CAM 图标注释行 | `#define CAMI_SPEED` 行（含） | 4609–8048 | CAM 像素数据 |
| 10 | `void blitRGBA(` | `// ---- v5.9 外圈和边缘渐暗` | 8049–8489 | 同 175 段 10 |
| 11 | `// ---- v5.9 外圈和边缘渐暗` | `int hexVal(char c)` | 8490–9122 | 同 175 段 11 |

> 锚点对不上的情况：先 `grep -n` 锚点确认行号再切；两板和 185b 是同源衍生，段落顺序一致，
> 只是行号平移和个别注释措辞不同（比如电量计注释 175 是 AXP2101、c3 抄成了 BQ27220）。

段 1 里有两行要注意：

- `#include "TripProgress.h"` 随段搬走（落在 NavMap.cpp 中部，合法；也可上提到头部）。
- `#define progReset() ...` 宏行随段搬走后**直接删掉**，由第七节的 navProgReset 函数替代。

---

## 五、第三步：组装 NavMap.cpp

文件头部（照 185b 版，includes 按板增减）：

```cpp
// 导航与小地图模块……拆自 MotoNav.cpp。共享状态见 NavMap.h，公共工具见 MotoNavCommon.h。
#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include <math.h>
#include <SPIFFS.h>
#include <miniz.h>            // 罪恶都市轮廓：仅接收地图时解压
#include "DragonRadar.h"
#include "MotoNavCommon.h"
#include "NavMap.h"
```

手写三样：

1. **blendF 声明**（原文件小地图段里的 `static inline uint16_t blendF(...);` 前置声明随段
   搬进来了，把 `static inline` 去掉即可；定义在段 11 里，同样去掉 `static inline`）。
2. **drawCompassBlip 前置声明**：`void drawCompassBlip(float px, float py, bool outside);`
   （drawDestBlip 在它前面用到）。
3. **文件末尾追加 navProgReset**（替代原 progReset 宏）：

```cpp
// v7.3 开始新一趟导航 / 清屏时清零路程进度（原是 MotoNav.cpp 里的 progReset 宏）
void navProgReset() {
  progDone = 0; progLastRm = -1; progLastAt = 0;
  tripProgress.reset();
}
```

**c3 板特有**：c3 无 PSRAM，主文件顶部把 `ps_malloc` 宏映射到内部堆
（`static inline void *motonavAlloc(...)` + `#define ps_malloc motonavAlloc`，约 250–253 行，
带 #if 分支）。NavMap.cpp 大量调用 ps_malloc，**把这几行（含 #if/#else/#endif）复制到
NavMap.cpp 的 include 之后**。static inline 函数每个编译单元一份，重复无害。

段与段之间建议像 185b 那样插一行 `// ================= NavMap 段 NN（源 xx-xx 行） =================`
注释，方便以后对照三块板。

---

## 六、第四步：NavMap.cpp 内的机械调整（共 3 处）

| 位置 | 原文 | 改为 | 原因 |
|---|---|---|---|
| 段 4 blend565 定义 | `static inline uint16_t blend565(...)` | `uint16_t blend565(...)` | 主文件天气/音乐/特效页也用；MotoNavCommon.h 已声明 |
| 段 7 blendF 前置声明 | `static inline uint16_t blendF(uint16_t a, uint16_t b, uint32_t t);` | `uint16_t blendF(uint16_t a, uint16_t b, uint32_t t);` | 同文件后部有定义，去 static 才能被别处链接（若只在本文件用则可不改，185b 为统一改了） |
| 段 11 blendF 定义 | `static inline uint16_t blendF(...) {` | `uint16_t blendF(...) {` | 同上 |

---

## 七、第五步：主文件机械调整

按出现顺序，全部是 185b 踩过的坑：

1. **插入头文件**：`#include <esp_mac.h>` 行（175 在 273 行、c3 在 276 行）之后加：

   ```cpp
   #include "MotoNavCommon.h"
   #include "NavMap.h"
   ```

2. **删 SwapCanvas 类定义**（175 约 291、c3 约 311）。类已在头文件里；
   `SwapCanvas *gfx = new SwapCanvas(...)` 这一行**保留**。

3. **删 Prim 结构定义**（175 约 869、c3 约 760）。结构体和 `PRIM_MAX` 宏已在头文件；
   紧随其后的 `Prim prims[PRIM_MAX]; int primN = 0; bool recIcon = false;` 三行定义**保留**
   （iconBuf/iconX/iconY/iconW/iconH/iconHash 也保留）。

4. **drawBattery 定义去默认参数**：`void drawBattery(int x0, int y0, bool backing = false) {`
   → `void drawBattery(int x0, int y0, bool backing) {`。默认参数已在头文件声明里，
   两处都有会报 "default argument given for parameter"。

5. **progReset 改名**：主文件剩 2 处调用（"C" 清屏指令、到达后又开始新导航），
   `progReset()` → `navProgReset()`。宏定义已随段 1 搬走并删除。

6. **enum MotoNavPage**（如主文件有）：删掉，枚举在 MotoNavCommon.h。175/c3 用裸数字则跳过。

7. 主文件顶部原有的 `void drawCompassBlip(...)` / `void drawLanesAt(...)` /
   `void drawBackground()` 前置声明**保留不动**（与头文件重复声明合法）。

---

## 八、验证

1. 编译：

   ```
   pio run -e esp32c3_128x160     # 或补好 env 后 pio run -e esp32s3-175
   ```

   期望：SUCCESS，无 multiple definition / undefined reference；和第二节记录的基线对比
   Flash/RAM 用量应基本持平。

2. 常见报错速查（185b 实际遇到的全集）：

   | 报错 | 原因 | 修法 |
   |---|---|---|
   | `multiple definition of 'class SwapCanvas' / 'struct Prim' / enum` | 主文件和头文件都定义了 | 第六节 2、3 条：删主文件里的定义，保留变量定义行 |
   | `default argument given for parameter 3 of drawBattery` | 默认参数写在定义上 | 第七节 4 |
   | `undefined reference to 'navProgReset()'` | 只改名没定义函数 | 第五节手写第 3 样 |
   | `'iconFit' / 'primAlpha' / 'drawLanesAt' was not declared` | NavMap.cpp 用到主文件的 static 函数 | 确认 MotoNavCommon.h 里有这三个声明（185b 版已带） |
   | `'THEME_N' was not declared`（handleText 里） | 定义随小地图段搬走 | NavMap.h 加 `#define THEME_N 12` |
   | `'MAP_RX_LOG' was not declared`（如有） | 同上 | NavMap.h 加同名空宏（175/c3 大概率没有，跳过） |
   | 链接期 `undefined reference to blendF/blend565` | 定义还是 static | 第六节 |

3. 功能冒烟（烧录实机）：

   开机动画 → 待机页 → 手机连接 → 开始导航（大箭头）→ 进小地图 → `#t,3`~`#t,11` 切
   风格 → `#n,1` 夜间 → 电子眼/红绿灯显示 → `#z,1` 巡航 → `#w,1` 清爽 → `#M1` 行程环 →
   断开重连 → 到达特效。重点看地图帧率统计（App 上的 "S,…" 报文）和拆分前一致。

---

## 九、收尾

- 删掉临时脚本/备份（确认编译和冒烟都过了再删）。
- 三块板的 NavMap 段注释里都保留了源行号，以后对照 diff 三块板时从锚点进，不要信旧行号。
- 后续如果把天气/音乐/IMU 等页面也拆出去，照同样套路：状态定义随段走、NavMap.h 式的
  extern 头文件、主文件只留协议和 loop。

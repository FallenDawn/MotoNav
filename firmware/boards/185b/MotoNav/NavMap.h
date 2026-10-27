// 导航与小地图模块：手机（高德 App 桥接）发来的定位、路线、地图块、图片地图、
// 电子眼、红绿灯、路况等数据的接收、解析和渲染（实现见 NavMap.cpp）。
// 拆自 MotoNav.cpp（v7.13.1）。跨文件使用的状态都放在这里；主循环和消息解析
//（MotoNav.cpp 的 handleText/loop）直接读写这些全局状态。
#pragma once

#include "MotoNavCommon.h"

// ===== 导航与行车状态（写入口：handleText，见 MotoNav.cpp）=====
extern bool inNav;                 // 是否处于导航中
extern String navDir;              // 当前方向
extern int navDist;                // 当前距离（米）
extern float navAngle;             // v1.8 路口实际转弯角度；NAN = 没有，用标准角度
extern bool navDirty;              // 导航画面需要重画

extern int speedKmh;               // 车速，-1 表示还没有
extern int camLimit, camDist;
extern bool hasCam;                // 前方是否有测速
extern int laneN;                  // 车道数，0 表示不显示
extern uint8_t laneBg[8], laneFr[8];   // 每条车道的背景方向码、推荐方向码（0xF = 不推荐）

extern bool tripOn;
extern bool cruiseOn;              // v4.8 巡航模式
extern bool cleanOn;               // v4.9 清爽模式
extern int tripRemainM, tripEta, tripLights;   // v3.1 行程信息（"#e"）

// 初版实时路况提醒：手机根据高德路线计算后发送 #TR,等级,前方距离,覆盖长度
extern int trafficAlertLevel, trafficAlertDist, trafficAlertSpan;
extern unsigned long trafficAlertAt;
// v7.13.5 前方拥堵提醒是否该显示：等级够 + 90 秒内刷新过。
// nearOnly = true 时再加 1.5km 距离门槛（地图页外圈、JAM banner 用）；
// 大箭头页没有地图和路线，外圈是唯一提示，不受距离限制
bool trafficAlertOn(bool nearOnly = true);

// v7.3 路程进度
extern bool progOn;
extern long progDone;              // 这趟已经走掉的里程（米）
extern int progLastRm;             // 上一次收到的剩余里程
extern unsigned long progLastAt;
void navProgReset();               // 原 progReset 宏：开始新一趟时清零进度

extern int lightDist;              // 前方红绿灯距离（米），-1 = 不显示
// v7.13.2 红绿灯倒计时（"#GL,相位,秒"）
extern volatile int lightCdSec;
extern volatile uint8_t lightCdPhase;
extern volatile unsigned long lightCdAt;

// v3.5 小地图上的电子眼（"#c,坐标系,x,y,限速,类型,..."）
#define MCAM_MAX 10                // v4.8 6 → 10（电子眼 + 道路安全提示）
extern int mapCamN, mapCamFrame;
extern int32_t mapCamX[MCAM_MAX], mapCamY[MCAM_MAX];
extern int mapCamSpd[MCAM_MAX];
extern int mapCamTyp[MCAM_MAX];    // v3.8 图标编号（手机按高德类型换算好，0~15）
extern int8_t mapCamPref[MCAM_MAX];
extern float mapCamPrefA[MCAM_MAX];
extern int8_t mapCamPrefK[MCAM_MAX];
extern float mapCamLockH[MCAM_MAX];

// ===== 地图风格与开关 =====
#define THEME_N 12                 // 配色主题数（表在 NavMap.cpp）
extern int themeIdx;               // "#t,0~11" 配色主题
extern bool curveOn;               // v6.1 弯道急缓上色
extern bool nightOn;               // v3.0 夜间配色（"#n,1/0"）
extern int trailMode;              // v3.0 走过的路线：1 变灰，2 擦除（"#r,1/2"）
extern bool bld3dOn;               // v7.13 3D 楼块显示开关（"#B3D,0/1"）
extern bool mapOn;                 // "#m,1/0" 小地图开/关
extern bool forceArrow;            // v7.13.6 "#m,2" 强制大箭头页（地图数据照收，只切显示）

// ===== 当前位置（"#p"，写入口 handleText）=====
extern int posFrame;
extern float posX, posY, posHdg, posKmh;
extern unsigned long posAt;
extern float corrX, corrY;         // v2.6 位置偏差（米）
extern unsigned long corrAt;
extern float hdgRate;              // v6.7 正在转弯的速度（度/秒）
extern int leadMs;                 // v6.7 预判提前量（毫秒）
void predictPose(unsigned long t, float lead, float &x, float &y, float &h);

// v4.4 原路线（导入的高德分享路线）
extern int ghostN;

// v5.0 路况 / 提示条
#define TRAF_MAX 40
extern int trafN;
extern int32_t trafA[TRAF_MAX], trafB[TRAF_MAX];
extern uint8_t trafS[TRAF_MAX];
extern unsigned long rerouteAt, fasterAt;
extern int fasterMin;
extern bool gpsWeak;

// 进度环（转弯倒计时）
extern bool ringActive;
extern String ringDir;

// 待发给手机的确认与统计（发送在 MotoNav.cpp 的 sendPendingAck/loop）
extern String pendingAck;                 // "K,类型,编号" 大块数据收齐确认
extern bool pendingBuildingCapability;    // "#GB?" → CAP,BLD,2
extern String pendingStat;                // v2.2 帧率统计（loop 里转交 pendingStat2）
extern String pendingProf;                // v5.8 分步用时（定义在 MotoNav.cpp）

extern TripProgress tripProgress;

// ===== 供 setup / loop / 消息解析调用的接口 =====
void buildRingLut();               // v1.8.3 抗锯齿圆环查找表（开机一次）
void mapInit();                    // v2.0 小地图缓冲分配（开机一次）
void helperInit();                 // v5.7 第二个核心帮忙画底图
void loadBackground();             // 独立圆屏背景图（SPIFFS）
void drawBackground();
void drawRedRing();                // 屏幕边缘的红圈（断线、数据中断）
void handleBulk(const uint8_t *d, int n);   // 'M' 大块数据（地图块/路线/原路线/背景）
// 接收链路已确认，默认关闭详细分片日志；主文件和 NavMap.cpp 共用
#define MAP_RX_LOG(...) do {} while (0)
bool mapModeActive();              // 现在是否显示小地图
float tripFrac();                  // v7.4 路程进度 0~1；不显示时 -1
void updateRingTarget();           // 收到新导航指令时更新倒计时环目标
void animateRing();                // 倒计时环动画（loop 每圈调用）
void updateStats();                // v2.2 每 2 秒统计帧率发给手机
void drawNavFrame();               // 按当前全部状态画一整帧导航画面（小地图或大箭头）

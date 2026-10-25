// v7.8 新增音乐页：导航左滑天气，右滑音乐；音乐左滑返回，上下滑也可查看天气；上一首/播放暂停/下一首，中文歌名和歌手。
// MotoNav 圆屏固件：蓝牙接收导航指令，显示箭头、距离和各种行车信息
// 板子：微雪 ESP32-S3-Touch-LCD-1.85B（360x360，466x466 逻辑画布）
// 配套手机 App：v2.0 及以上
//
// 版本记录
//   v1.0  蓝牙接收指令，显示箭头和距离
//   v1.1  改用内存画布绘图，修复箭头不显示
//   v1.2  修复串口打印卡住导致的约 4 秒延迟
//   v1.3  断线保护：蓝牙断开时不再停留在旧箭头；导航中超过 6 秒收不到数据，
//         屏幕边缘亮红圈并提示 NO DATA；支持心跳指令 "P"
//   v1.3.1 修复编译错误（drawWaiting 的参数类型）
//   v1.4  所有箭头按外框居中显示；NO DATA 提示移到距离数字下方
//   v1.5  三阶段转向提示（远：直行线+灰色预告；中：大箭头+黄色进度环；近：橙色）
//   v1.6  拐弯处改为圆角；远处预告竖条居中；进度环连续平滑、动画追赶
//   v1.7  新增：顶部车速、电子眼限速牌（超速变红）、车道提示、
//         中文路名和底部“剩余里程·到达时间·红绿灯数”（由手机渲染成图片发来）、
//         亮度调节；蓝牙改为二进制消息队列，支持最长 512 字节的数据包
//   v1.7.1 整体排版上移，路名和底部信息离外圈更远；文字图片太宽时自动缩小，
//         保证不碰到圆环；有车道时距离数字稍小一号（配套 App v1.7.1）
//   v1.7.2 车道里的左转、右转小图标改为直角样式（竖着上去，再横着拐出去）
//   v1.7.3 底部“剩余里程·到达时间·红绿灯数”常驻显示（出现车道时也不再隐藏）
//   v1.8  转弯箭头角度跟随路口实际角度（手机算好发来），锐角弯显示锐角箭头
//         （配套 App v1.8；旧 App 不发角度时照旧用标准角度）
//   v1.8.1 大图标和远处预告改用“抗锯齿矢量绘制”：转弯处是真正连续的圆弧，边缘平滑无锯齿、
//         无拼接痕迹；小角度转弯的弯也更圆润。图标画好后缓存，画面刷新时直接复制，不增加耗时
//   v1.8.2 图标自动适应可用区域：不管箭头是什么角度（缓弯的箭头特别高），都整体缩放并居中到
//         “车速下方 ~ 车道/距离上方”之间，不再挡住 km/h
//   v1.8.3 进度环、超速红圈、断线红圈、状态点、限速牌改为抗锯齿绘制，边缘平滑无毛边
//         （开机时预先算好圆环上每个像素的半径和角度，每帧只查表，速度很快）
//   v2.0  小地图（罪恶都市风格）：手机发来附近的道路/绿地/水面（地图块）和导航路线，
//         圆屏画成车头朝上、跟着转的小地图；左下角小圆牌显示转向和距离，上方显示车速。
//         离路口 150 米内自动切回大箭头画面。新增蓝牙通知通道，收齐地图后回确认给手机。
//         （配套 App v2.0）
//   v2.1  小地图一直显示，路口前不再跳回大箭头画面：离路口 150 米内左下角圆牌放大、
//         边框变橙色，车道图标也显示在圆牌里；100 米内箭头和进度环变橙色
//   v2.2  提高小地图帧率：双缓冲 + 双核并行（一个核画下一帧，另一个核同时把上一帧推到屏幕）；
//         取消 60 毫秒的帧间隔限制；道路只在拐弯处补圆角，少画很多圆。
//         每 2 秒把帧率、画图耗时、刷屏耗时发给手机，App 上可以看到（配套 App v2.0.1）
//   v2.3  减轻画面撕裂：屏幕数据线速度可在 App 里切换 40 / 60 / 80 MHz（保存在圆屏里，切换后自动重启）。
//         速度越快，每帧推到屏幕的时间越短，撕裂越轻；如果花屏就切回 40。（配套 App v2.1）
//   v2.4  小地图左下角圆牌：离路口 500 米以外显示直行箭头（和大箭头画面的远阶段一致），
//         500 米以内才显示转向箭头，不再几公里外就提示转弯（配套 App v2.2：测速提醒提前）
//   v2.5  支持天地图“图片地图”（大块数据类型 3）：手机把天地图底图归类成 街区/道路/绿地/水面 四色、
//         压缩后发来，圆屏每帧按车头方向旋转显示；线条地图（OpenStreetMap）照常支持，
//         两种都有时显示最新收到的那种（配套 App v2.3）
//   v2.6  提高帧率：
//         1. 快速刷屏：自己驱动屏幕数据线，一边整理下一段像素一边传上一段（原来是排队一段一段来），
//            可在 App 里关掉（花屏时用）；
//         2. 图片地图按 16×16 小块存放，旋转取像素时内存命中率高很多；
//         3. 整圈的圆环（黑边、底环、红边）改为按行填充，不再逐点查表；
//         4. 新位置到达时，和推算位置之间的偏差在 0.4 秒内平滑修正，地图不再“跳一下”；
//         5. 帧率统计里多报“画地图”“画边框信息”各用多少毫秒（配套 App v2.4）
//   v2.7  小地图信息区重新设计：去掉左下角突兀的小圆牌和车速胶囊，改成和黑色外圈连成一体的
//         上下两块“弧形面板”——上面显示车速/测速，下面显示转向箭头和距离；
//         离路口 150 米内下面板升高、分隔线变橙色，并显示车道
//   v2.8  按要求改回整屏地图：去掉上下面板，车速、转向箭头、距离直接浮在地图上，
//         带黑色描边保证看得清；车道放在一条半透明深色底上（地图仍然透得过来）
//   v2.9  更有 GTA 味：
//         1. 地图配色主题：罪恶都市（默认，暖沙色街区、青绿水面、粉紫路线、青色车速）/
//            GTA5（深色地面、浅色道路、紫色路线）/ 原版，可在 App 里切换（"#t,0/1/2"）；
//         2. 雷达边缘的“N”北向标记，跟着车头方向转；
//         3. 目的地标记：在雷达范围内显示在地图上，范围外贴在雷达边缘指出方向；
//         4. 雷达边缘一圈渐暗阴影，更像游戏里的圆形雷达（配套 App v2.5）
//   v3.0  1. 走过的路线变灰（或直接擦掉，App 里选），只有前方的路线是鲜艳的颜色；
//         2. 夜间配色：三套主题都有夜间版（地面压暗、道路变浅色线条、路线保持鲜艳），
//            手机按当地日出日落自动切换，也可以固定白天/夜间（配套 App v2.8）
//   v3.1  小地图底部加一行行程信息：到达时间 · 剩余里程 · 红绿灯数（离路口 150 米内让位给车道，自动隐藏）；
//         前方 300 米内有红绿灯时，左侧显示红绿灯图标和距离（配套 App v3.1）
//   v3.2  红绿灯直接画在小地图上它实际的位置（只画还没开过的），下一个灯在 300 米内时旁边标距离；
//         去掉 v3.1 左侧的红绿灯面板（配套 App v3.3）
//   v3.3  小地图的车道提示从底部移到车速下面，并放大一点（不再和底部的 N 标记、外圈挤在一起）
//   v3.4  车道图标抗锯齿：边缘平滑，左转/右转/掉头的拐角改成圆弧（大箭头界面的车道也一样）
//   v3.5  电子眼画在小地图上的实际位置：测速点是带限速数字的小限速牌，其他拍照点是相机图标；
//         开过以后自动消失（配套 App v3.4）
//   v3.6  首页加一只像素猫猫：连上手机时睁眼、摇尾巴、偶尔眨眼；没连上手机时闭眼睡觉，头上飘 z
//   v3.7  转弯更顺：小地图转向改成按时间平滑转动（最快每秒 70 度），缩放也按时间平滑，
//         相邻两帧变化更小更均匀，撕裂感更轻（这块板子没接屏幕刷新同步信号，撕裂无法完全消除）
//   v3.8  小地图上的电子眼按类型画不同图标（测速牌、监控、闯红灯、违章拍照、公交道、应急车道、
//         非机动车道、区间起点/终点、流动测速、ETC、礼让行人、违法变道、违停、禁止鸣笛）；
//         图标优先画在实际位置，和别的东西（另一个电子眼、红绿灯、车头、北向标记、车速、底部信息）
//         重叠时自动挪到旁边的空位，并用细线连回实际位置
//   v3.9  电子眼图标挪开时紧贴在实际位置旁边（不再拉长线，实际位置画个小白点）；车头箭头缩小到约 70%
//   v4.0  电子眼图标位置选好后锁住不动，转弯转稳后才重新选一次；停车/暂停后一直保持地图画面
//         （以前 5 秒收不到位置就切回大箭头画面）
//   v4.1  路上只画蓝色小摄像头（代替小白点）和红绿灯；类型图标（测速牌、违章拍照……）一律摆在
//         小摄像头旁边，优先放在道路两侧，不压路线，图标之间留空隙不重叠
//   v4.2  修复两个已锁住位置的类型图标叠在一起：锁住的图标和别的电子眼图标重叠时重新选位置
//   v4.3  同一处有好几个电子眼时，类型图标排成一列（和道路平行）整列放在道路一侧，间距固定不会重叠；
//         整列一起锁住（v4.2 的“重叠就重选”去掉了，它会让图标来回跳）；两侧都放不下时放到远一列，用细线连回
//   v4.4  导入高德分享路线导航时，原路线画成一条橙色细线（在导航路线下面，走错路时才露出来）；
//         新增大块数据类型 4 = 原路线，"#o" = 不显示原路线
//   v4.5  蓝牙允许 2M 模式和最长空中数据包，手机请求时直接用（大块数据传得更快）
//   v4.6  首页的猫猫换成原创像素女骑手和小摩托：连上手机时围巾马尾飘动、眨眼、挥手；没连上时车灯熄灭、打瞌睡
//   v4.7  修复类型图标被推到远处、拉一根长线：小摄像头和图标之间改按圆形判断重叠（以前按方框，斜放时误判）
//   v4.8  巡航模式（"#z,1/0"）：没有目的地时也显示小地图、车速、电子眼和限速；
//         道路安全提示图标（急弯、学校、事故多发、注意行人、陡坡、村庄、窄桥、落石、隧道、注意，编号 16~25），
//         路上画小黄点、图标摆在旁边；电子眼最多 10 个；修复文字消息超过 63 字节被截断（电子眼多了会丢）
//   v4.9  清爽模式（"#w,1/0"）：小地图照常显示，去掉红绿灯、电子眼、安全提示、测速牌、超速红圈和底部行程信息，
//         只留地图、路线、车速、转弯箭头/距离、车道和进度环
//   v5.0  路况颜色（"#u"：缓行黄、拥堵红、严重拥堵深红）；走错路时显示 REROUTING（"#y,1/0"）；
//         发现更快路线时显示 FASTER ROUTE -X min（"#h,分钟"）；GPS 信号弱时左上角亮 GPS 图标（"#x,1/0"）
//   v5.1  来电显示（"#i,1,号码" / "#i,0"，名字是图片栏 2 的文字图片）：来电时整屏显示是谁，接听或挂断后回到原画面
//   v5.2  触屏接电话：来电画面上按住屏幕任意位置 0.8 秒（绿圈外一圈白色进度走满）就接听，
//         通过通知通道告诉手机 "T,A"；按下时也报一次坐标 "T,P,x,y"（以后做左右按钮用）。
//         触摸芯片 CST9217（SensorLib 库），只在来电时读取
//   v5.3  天气页：在圆屏上左右滑（上下滑也行）切换到天气，再滑一下回来。手机用高德天气查当地实况和
//         4 天预报发来（"#W,图标,温度,更新时间,天数,{星期,图标,最高,最低}…"；地名·天气、风·湿度是
//         图片栏 3、4 的文字图片）。导航中看天气 15 秒后、或快到路口（200 米内）自动回到导航画面。
//         触摸改为一直监听（触摸芯片有动静才读），切到天气页时向手机要一次数据 "T,W"
//   v5.4  换页滑动动画：跟着手指方向，旧画面滑出、新画面滑入（约 0.3 秒，先快后慢）；
//         自动回导航时按原路滑回去。触摸方向和画面相反的话，改 TOUCH_FLIP_X / TOUCH_FLIP_Y
//   v5.5  换页改成“跟手”：手指按住拖动时新页面跟着手指一点点滑进来，松手后按速度顺势滑完；
//         拖得不够远、也没甩出去就弹回原页面。新页面盖在上面，旧页面慢慢往后退（视差）、
//         渐渐变暗，交界处有一道阴影；动画用两块画布轮流推屏，更顺滑
//   v5.5.1 触摸左右方向反了：TOUCH_FLIP_X 改为 1
//   v5.6  天气页下面一排改成接下来 5 个小时：几点、图标、温度，会下雨时显示降雨概率（蓝色）。
//         "#W,图标,温度,更新时间,H,个数,{几点,图标,温度,降雨%}…"；手机查不到逐小时时发 D（按天，和以前一样）
//   v5.7  小地图提速：
//         1) 天地图底图（每帧最费时的逐像素计算）改成两个核心一起画：主核心画偶数行，另一个核心画奇数行。
//            另一个核心平时只负责推屏，推屏大部分时间在等数据线传完，正好空着
//         2) 雷达边缘渐暗：开机时把每个像素的暗度算好存表，每帧只查表（原来每个像素都要开平方、做 3 次除法）
//         3) 帧率统计里多报一项“底图用时”，方便看瓶颈在哪
//   v5.8  实测 8.9 帧：底图 42 毫秒、边框和信息 53 毫秒。
//         1) 帮忙画底图的任务优先级提到推屏任务之上（原来推屏一直占着那个核心，帮忙的任务排不上，
//            主核心画完自己那一半只能干等）
//         2) 描边文字（车速、距离）原来要把字重复画 24 遍，改成 16 遍，效果一样
//         3) 每帧各步骤分别计时，每 2 秒发一份 "Q,…" 给手机（单位 0.1 毫秒），找出真正慢的地方
//   v5.9  按实测分步用时继续提速（13 帧时：外圈 22.7 毫秒、边缘渐暗 10.3 毫秒）：
//         1) 外圈（黑边 + 进度环底 + 进度条 + 超速红圈）原来分 4 次画，进度条还要把一圈圈像素在屏幕上
//            来回跳着访问（很费外挂内存）。改成一次从上到下逐行算出最终颜色，两个核心各画一半的行
//         2) 边缘渐暗也改成逐行、两个核心一起画，暗度按“半径平方”查一张放在芯片内部内存里的小表
//         3) 混色改用乘法加移位，不再做除法
//   v6.0  v5.9 的外圈反而更慢（48 毫秒）：这块芯片的浮点开平方、除法是软件算的，很慢，而外圈每个像素都开了平方。
//         1) 外圈改成纯整数：半径用“半径平方 → 半径”的小查表（放芯片内部内存），进度条的角度判断改成
//            两次乘法（叉乘）；圆外角落直接整段清零
//         2) 两个核心分活改成“上半屏 / 下半屏”，不再隔行交错（交错时两个核心抢同一块缓存，互相拖慢）
//         3) 转弯撕裂：屏幕没有接刷新同步信号，帧率低时相邻两帧角度差太大就会看到明显的错位。
//            现在每帧最多转 3.5°（帧率高时不影响，帧率低时画面转得稍慢一点，但不再撕得厉害）
//   v6.1  新增 8 种地图风格（"#t,3"~"#t,10"）：
//           3 赛博霓虹  4 复古纸地图（夜间配色时自动变成霓虹）  5 极简黑白  6 蓝图（跟着地图走的网格）
//           7 掌机像素（底图按 3×3 大像素画，还更快）  8 雷达（转动的扫描线 + 距离圈）
//           9 寻宝罗盘（六边形网格、跟着车头转的罗盘刻度、目的地菱形 + 脉冲圈）
//           10 卫星图（手机把天地图卫星照片压成 16 色发来，地图数据里自带调色板）
//         另加“弯道急缓上色”开关（v6.2 起是 "#Q,1/0"）：前方路线按弯急程度涂成绿 / 黄 / 红，任何风格都能叠加
//   v6.1.1 赛博霓虹的道路青色调暗一点，让品红路线更跳
//   v6.2  圆屏方向：整个画面可以转 0° / 90° / 180° / 270°（"#R,0~3"，存在板子里，开机就按这个方向），
//         装车时线从哪边出来都行。转向在推屏时顺手完成（不多花画图时间）；触摸方向跟着一起转。
//         修复：v6.1 的弯道上色用了 "#q"，和“屏幕数据线速度”撞车，一直开不了 → 改成 "#Q,1/0"
//   v6.4  圆屏方向支持任意角度（"#D角度"，0~359，没有逗号，旧固件会忽略它）：App 上转圆盘，圆屏实时跟着转。
//         90° 的整数倍照旧走原来的快速通道；其他角度推屏时逐像素按角度取样（只算圆里面的像素），
//         帧率会略低一点。手指滑动方向也按角度换算。角度停下 1.5 秒后才存进板子（拖动时不反复写闪存）。
//         （配套安卓 App v6.3；Mac 上 iOS 那边做的 v6.3 固件用的指令可能不同，以这一版为准再对齐）
//   v6.5  蓝牙配对验证：陌生手机连上也发不了任何指令。
//         - 第一次连接时圆屏显示 6 位配对码，在手机弹出的配对框里输入；配好后手机被记住（最多 3 台），以后自动连、全程加密
//         - 只有“开机 2 分钟内”或“还没有配对过任何手机”时才显示配对码；其他时候有陌生手机来配对，
//           圆屏显示 LOCKED，配对必然失败（要加新手机：重启圆屏，2 分钟内配对）
//         - 连上 60 秒还没通过验证的连接自动断开，不会占着圆屏让你自己的手机连不上
//         - 通过验证后通知手机 "U,1"（App 收到后把所有状态重发一遍）；"#UNPAIR" 清除记住的所有手机
//   v6.6  修："#UNPAIR" 把正在用的手机也忘了，可手机那边还记着配对 → 两边对不上，再也连不上。
//         改成只忘掉“别的”手机，正在连着的这台保留（丢手机 / 换手机时用新手机点一下就行）
//   v6.7  小地图跟得更紧（减少“慢半拍”）：
//         1. 转弯预判：用前后两次定位算出正在以多快的速度转弯，两次定位之间地图按这个速度继续转，
//            位置也沿着弯道推算（原来只会沿直线推、朝向要等下一次定位才变）
//         2. 提前量：画面显示“再过一点点时间”的位置和朝向，抵消 GPS 和传输的延迟（"#L毫秒"，0~800，默认 300；
//            App 里可选 关 / 0.3 秒 / 0.6 秒）
//         3. 地图朝向追得更快（时间常数 0.35 → 0.22 秒）
//         4. 不认识的 "#..." 指令一律忽略（以前会被误当成导航方向）
//   v6.8  电量显示（接了锂电池时）：板子上的 AXP2101 电源芯片读电量、是否在充电、是否插着 USB。
//         圆屏顶部（蓝牙小圆点右边 / 小地图车速上方）画一个小电池和百分比：>50% 绿、20~50% 黄、<20% 红，
//         充电时青色带小闪电；低于 10% 没在充电时闪烁。没接电池就不画。
//         电量变化或每分钟发给手机一次："V,百分比,是否充电,是否插USB,电压mV"
//   v6.9  来电按住接听从 0.8 秒缩短到 0.2 秒（骑车时手离开车把的时间越短越安全）；
//         提示文字改成 "TAP & HOLD"，白点进度圈照旧（0.2 秒走满）
//   v7.0  蓝牙名带上自己硬件地址的最后 4 位（例 "MotoNav-3A7F"），几块圆屏在一起不会混；
//         这 4 位和手机蓝牙设置里看到的地址末尾一样。首页和配对页都显示这个名字
//   v7.1  到达特效：到达目的地时约 7 秒庆祝动画（放射光芒、冲击波、烟花、飘动的终点旗、ARRIVED 霓虹字、
//         行程统计逐字打出）；到达途经点 3 秒短版。点一下屏幕提前结束。
//         新指令（不带逗号，旧固件会忽略）："#O米/分钟/最高车速" 行程统计（在 "A,0" 前发），"#J" 到达途经点
//   v7.2  修：首页电量图标开机闪一下就没了（骑手动画每次重画都把它擦掉）→ 骑手画完补画蓝牙点和电量；
//         电量变化时只重画状态栏，不再整屏重画；偶尔读不到电池不会马上隐藏（连续 3 次才算没接电池）；
//         小地图上电量图标加黑色底，浅色地图上也看得清
//   v7.3  路程进度：屏幕左边沿一条细弧（在转弯进度环里面），从左下往左上走满 = 到达；白点是你现在的位置，
//         上端粉点是终点。用 "#e" 里的剩余里程自己算：每次剩余变少就记进“已走”，进度 = 已走 /（已走 + 剩余），
//         偏航重新规划也不会归零。"#M1" / "#M0" 开关（不带逗号，旧固件忽略；默认开）
//   v7.4  路程进度改到最外圈：一开始是一整圈绿环，从正上方顺时针，走过的部分变灰，交界处一个小白点；
//         快到路口出现转弯倒计时环时，倒计时环盖掉行程环，转完弯再回到行程环。去掉 v7.3 左边那条细弧
//   v7.5  行程环改细：8 像素 → 4 像素
//   v7.6  巡航模式不画路线：以前会把上一次导航留下的路线（粉色 / 走过变灰）、弯道上色、路况、红绿灯、终点标记画出来，
//         现在巡航时道路都保持地图原色
//   v7.7  圆屏转到 0/90/180/270 以外的角度时字发糊：推屏时改成双线性插值（取周围 4 个点按距离混合），
//         斜着转的字和线条边缘平滑、粗细均匀
//
// ---------------- 通信协议（手机 → 圆屏，每条消息一个蓝牙数据包） ----------------
//   R,300 / L,120 / S,800 / SL / SR / U / A   导航：方向,距离(米)
//   L,120,-135            v1.8：可选第三项 = 路口实际转弯角度（负左正右，单位度）
//   C                     清屏回到待机
//   P                     心跳
//   #v,42                 车速 km/h
//   #k,60,300 / #k        前方测速：限速,距离(米)；只有 #k 表示没有测速
//   #j,0033F0             车道：每条车道两个十六进制字符（背景方向码、推荐方向码，F=不推荐）；只有 #j 表示隐藏
//   #b,200                屏幕亮度 0~255
//   'B' + 二进制头 + 数据   文字图片（4 位灰度），见 handleBitmap()
//   #p,坐标系,x分米,y分米,朝向,车速   v2.0 当前位置（平面坐标，x 向东 y 向北）
//   #m,1 / #m,0           v2.0 小地图开 / 关
//   #q,40 / #q,60 / #q,80 v2.3 屏幕数据线速度（MHz），保存后重启生效
//   #f,1 / #f,0           v2.6 快速刷屏 开 / 关，保存后重启生效
//   #t,0 / #t,1 / #t,2    v2.9 地图配色：0 原版，1 罪恶都市，2 GTA5（v6.1 增加 3~10，见版本记录）
//   #Q,1 / #Q,0           v6.2 弯道急缓上色 开 / 关（v6.1 误用了 #q）
//   #R,0~3[,角度]         v7.10.1 兼容四档方向，带第二项时按精确角度旋转
//   #D角度                v6.4 圆屏方向任意角度 0~359（例 "#D37"；故意不带逗号，旧固件收到会忽略）
//   #UNPAIR               v6.5 忘掉配对过的手机（v6.6 起保留正在连着的这台）
//   #L毫秒                v6.7 小地图预判提前量 0~800（例 "#L300"；不带逗号，旧固件会忽略）
//   #n,1 / #n,0           v3.0 夜间配色 开 / 关
//   #r,1 / #r,2           v3.0 走过的路线：1 变灰，2 擦除
//   #e,12300,1432,8 / #e  v3.1 行程信息：剩余米,到达时间(时分),红绿灯数；只有 #e 表示隐藏
//   #g,120 / #g           v3.1 前方红绿灯距离（米）；只有 #g 表示隐藏
//   #c,坐标系,x,y,限速,图标,... / #c   v3.5 前方电子眼位置（米），最多 6 个；只有 #c 表示清空
//                         v3.8 最后一项是图标编号 0~15（手机按高德类型换算好，见 CAM_ICONS）
//   'M' + 二进制头 + 数据   v2.0 大块数据（1 = 地图块，2 = 导航路线，3 = 天地图图片地图，4 = 原路线 v4.4），见 handleBulk()
//   #o                    v4.4 不显示原路线
//   #z,1 / #z,0           v4.8 巡航模式开 / 关
//   #w,1 / #w,0           v4.9 清爽模式开 / 关
//   #i,1,号码 / #i,0      v5.1 来电显示开始 / 结束（名字用 'B' 图片栏 2）
//   #u,起,止,状态,...      v5.0 路况（米，离路线起点）；#y,1/0 重新规划中；#h,分钟 / #h 更快路线；#x,1/0 GPS 弱
//   #HM,0/1               v7.9 App 音乐控制 / HID 媒体按键（保存到圆屏；互斥）
//   HID,模式,可发按键        v7.9 HID 就绪状态；CAP,HID,1 能力通知
//   #Z,有会话,播放中,按钮掩码   v7.8 音乐状态（1=上一首，2=播放暂停，4=下一首）；图片栏 5/6 为歌名/歌手
// ---------------- 圆屏 → 手机（通知） ----------------
//   T,M,Q / T,M,P / T,M,T / T,M,N   v7.8 请求音乐状态 / 上一首 / 播放暂停 / 下一首
//   K,类型,编号           v2.0 这一份大块数据已收齐
//   U,1                   v6.5 这台手机通过了配对验证（连上后发 3 次）
//   V,电量,充电,USB,mV    v6.8 圆屏电池（电量 -1 = 没接电池）

#define FW_VERSION "v7.13.1-185B"

// 调试时想在串口监视器里看到每条收到的文字指令，把 0 改成 1
#define DEBUG_SERIAL 0

// 导航中超过这么久没收到任何消息（含心跳），就提示数据中断
#define STALE_MS 6000

// 三阶段的分界距离（米），可按需要调整
#define STAGE_FAR_M  500   // 超过这个距离：远处预告画面
#define STAGE_NEAR_M 100   // 小于等于这个距离：橙色警示

#include <Arduino.h>
#include "Arduino_GFX_Library.h"
#include "pin_config.h"
#include <Wire.h>
#include "TouchDrv.hpp"
#include "GaugeDrv.hpp"
#include "sensor/imu/qmi8658/SensorQMI8658.hpp"
#include <math.h>
#include "PortScale.h"
#include "Board185BPanel.h"     // v5.2 触摸屏（SensorLib 库，微雪示例里带的）
#include "HomeAnimation.h"
void drawBackground();
#include <Preferences.h>
#include <SPIFFS.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <driver/rtc_io.h>
#include "IdlePowerOff.h"
#include <driver/spi_master.h>
#include <BLEDevice.h>
#include "TpmsScanner.h"                       // TPMS 胎压传感器 BLE 广播接收（数据层）
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <BLEHIDDevice.h>
#include "HidMediaKeys.h"
#include "AppleMediaClient.h"
#include "ScreenRotationCommand.h"
#include <BLESecurity.h>       // v6.5 配对验证
#include <miniz.h>            // 罪恶都市轮廓：仅接收地图时解压
#include "BootAnimation.h"
#include "BootAudioData.h"
#include <es8311.h>
#include <driver/i2s.h>
#include "DragonRadar.h"
int bootBrand = 0;
String pendingBootAck;
#include <esp_mac.h>           // v7.0 读蓝牙地址，拼蓝牙名
#include "MotoNavCommon.h"
#include "NavMap.h"
#if defined(CONFIG_NIMBLE_ENABLED)
#include "host/ble_gap.h"     // v4.5 蓝牙 2M 模式
#include "host/ble_store.h"   // v6.5 记住的手机
#endif

// ===== 屏幕（和官方 HelloWorld 示例一样的初始化方式）=====
// v2.6 第 7 个参数 true = 总线可共享（快速刷屏要和它轮流使用数据线）
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3, true);
Board185BPanel *panel = new Board185BPanel(bus);
PortScale portScale;
bool bootAudioActive = false;
volatile bool bootAudioFinished = false;
es8311_handle_t bootEs8311 = nullptr;
size_t bootAudioOffset = 78; // 44-byte RIFF header plus the WAV metadata chunk
uint32_t bootAudioDueAt = 0;
TaskHandle_t bootAudioTaskHandle = nullptr;
struct TextImg;    // Arduino 自动生成的函数声明需要先认识文字图片类型。
void flushLogicalFrame(const uint16_t *fb);
void drawLanesAt(float centerX, float y0, float maxW, float h);
void drawPageDots();
void weatherTap();
void drawCompassBlip(float px, float py, bool outside);
bool backlightPWM=false;

void stopBootAudio() {
  bootAudioActive = false;
  i2s_driver_uninstall(I2S_NUM_0);
  pinMode(PA, OUTPUT);
  digitalWrite(PA, LOW);
}

void bootAudioTask(void *) {
  static uint8_t chunk[4096];
  while (bootAudioOffset < motonav_boot_wav_len) {
    const size_t n = min(sizeof(chunk), motonav_boot_wav_len - bootAudioOffset);
    memcpy_P(chunk, motonav_boot_wav + bootAudioOffset, n);
    size_t written = 0;
    if (i2s_write(I2S_NUM_0, chunk, n, &written, portMAX_DELAY) != ESP_OK || !written) break;
    bootAudioOffset += written;
  }
  vTaskDelay(pdMS_TO_TICKS(250));
  bootAudioTaskHandle = nullptr;
  bootAudioActive = false;
  bootAudioFinished = true;
  vTaskDelete(nullptr);
}

static bool initBootEs8311() {
  if (bootEs8311) return true;
  bootEs8311 = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  if (!bootEs8311) {
    return false;
  }
  const es8311_clock_config_t clock = {
    .mclk_inverted = false,
    .sclk_inverted = false,
    .mclk_from_mclk_pin = true,
    .mclk_frequency = 8000 * 256,
    .sample_frequency = 8000,
  };
  esp_err_t err = es8311_init(bootEs8311, &clock, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if (err != ESP_OK) {
    return false;
  }
  // 开机提示音单独使用较低音量，避免刚启动时声音过大。
  err = es8311_voice_volume_set(bootEs8311, 80, nullptr);
  return err == ESP_OK;
}

void startBootAudio() {
  if (bootAudioActive || bootAudioTaskHandle) return;
  if (!initBootEs8311()) {
    return;
  }
  pinMode(PA, OUTPUT);
  digitalWrite(PA, HIGH);
  const i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 8000, .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, .dma_buf_count = 8,
    .dma_buf_len = 512, .use_apll = false, .tx_desc_auto_clear = true,
    .fixed_mclk = 8000 * 256
  };
  const i2s_pin_config_t pins = {
    .mck_io_num = I2S_MCK_IO, .bck_io_num = I2S_BCK_IO,
    .ws_io_num = I2S_WS_IO, .data_out_num = I2S_DO_IO, .data_in_num = I2S_DI_IO
  };
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK ||
      i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
    stopBootAudio();
    return;
  }
  bootAudioOffset = 78;
  bootAudioActive = true;
  bootAudioFinished = false;
  if (xTaskCreatePinnedToCore(bootAudioTask, "boot-audio", 4096, nullptr, 4,
                              &bootAudioTaskHandle, 0) != pdPASS) {
    stopBootAudio();
    bootAudioTaskHandle = nullptr;
  }
}

void boardBrightness(uint8_t value) {
  if(backlightPWM)ledcWrite(LCD_BL,value);
  else digitalWrite(LCD_BL,value>0?HIGH:LOW);
}
// 先画在内存里的“画布”上，画完再一次性推到屏幕（需要 PSRAM：OPI PSRAM）
// SwapCanvas 类在 MotoNavCommon.h
SwapCanvas *gfx = new SwapCanvas(LCD_WIDTH, LCD_HEIGHT, panel);
bool needFlush = false;
bool frameIsMap = false;          // v2.2 这一帧是小地图（整帧重画，可以异步刷屏）
int qspiMHz = 40;                 // v2.3 屏幕数据线速度
bool fastFlushOn = true;          // v2.6 快速刷屏


// ===== 颜色（RGB565）=====
#define C_BLACK  0x0000
#define C_WHITE  0xFFFF
#define C_GREEN  0x07E0
#define C_GREY   0x7BEF
#define C_CYAN   0x07FF
#define C_RED    0xF800
#define C_DIM    0x6B4D   // #6B6B6B 预告、非推荐车道用的灰色
#define C_TRACK  0x2945   // #2A2A2A 进度环底色
#define C_YELLOW 0xF623   // #F5C518
#define C_ORANGE 0xFBC3   // #FF7A1A
#define C_SEP    0x31A6   // #333333 车道分隔线
#define C_SIGNRED 0xE104  // #E02020 限速牌红圈

// ===== 屏幕几何：466x466，圆心 (233, 233) =====
#define CX 233
#define RING_R 222

// ===== 布局（y 坐标）=====
#define SPEED_Y      50    // 车速数字顶部
#define ICON_CY      188   // 没有车道时，图标中心（v1.8.2 起只作初始位置，最终按可用区域自动摆放）
#define ICON_CY_LANE 172   // 有车道时，图标中心
#define ICON_TOP     120   // v1.8.2 图标可用区域上沿（车速 km/h 下方）
#define ICON_MAX_W   250   // 图标最大宽度
#define LANES_Y      238   // 车道行顶部
#define DIST_Y       250   // 没有车道时，距离数字顶部（8 号字）
#define DIST_Y_LANE  258   // 有车道时，距离数字顶部（7 号字）
#define ROAD_Y       334   // 没有车道时，路名图片顶部
#define ROAD_Y_LANE  334   // 有车道时，路名图片顶部
#define BOTTOM_Y     372   // 底部信息图片顶部（常驻显示）
#define TEXT_R       200   // 文字必须在这个半径以内（圆环内沿约 215）

// ===== 蓝牙：手机端必须用同样的名字和 UUID =====
#define DEVICE_NAME  "MotoNav"
String bleName = DEVICE_NAME;        // v7.0 实际蓝牙名 "MotoNav-XXXX"（XXXX = 蓝牙地址最后两个字节），开机时算好
#define SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   // v2.0 圆屏 → 手机通知
BLECharacteristic *txChar = nullptr;

volatile bool bleConnected = false;

// ===== 接收队列：蓝牙线程收到的每个数据包原样放进队列，主循环再取出处理 =====
#define RXQ_SLOTS 48   // v2.0 加大，接收地图大块数据时不丢包
#define RXQ_MAX   512
struct RxMsg { uint16_t len; uint8_t data[RXQ_MAX]; };
RxMsg rxq[RXQ_SLOTS];
volatile int rxHead = 0, rxTail = 0;
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// ===== 导航与行车状态 =====
bool shownConn = false;       // 屏幕上反映的连接状态
unsigned long lastRxMs = 0;   // 最近一次收到任何消息的时间
bool staleShown = false;      // 是否正在显示“数据中断”提示
bool navDirty = false;        // 导航画面需要重画

IdlePowerOffTimer idlePowerOffTimer = {0, false};
volatile bool powerOffPending = false;
// v7.3 路程进度

// ===== 文字图片（手机渲染好的 4 位灰度图）=====
#define IMG_SLOTS 13           // 0 = 路名，1 = 底部信息，2 = 来电人名字，3/4 = 天气地名和风湿度，5/6 = 音乐歌名和歌手
#define IMG_MAX   8192        // 每张图最多字节数
struct TextImg {
  uint8_t *show;              // 正在显示的图
  uint16_t showW; uint8_t showH;
  uint8_t *recv;              // 正在接收的图
  int recvId; uint16_t recvW; uint8_t recvH; uint8_t recvParts; uint32_t recvMask;
};
TextImg imgs[IMG_SLOTS];
bool imgAllocWarned[IMG_SLOTS] = {};
AppleMediaClient appleMedia;
volatile uint32_t mediaConnectionEpoch = 0;
uint32_t lyricImageGeneration = 0; uint16_t lyricImageTitleRevision = 0, lyricImageArtistRevision = 0;
bool musicLyrics = false;                  // #ML,0/1; never persist an enabled default
bool musicDirty = true;
bool musicHave = false, musicPlaying = false;
int musicActions = 0;
int musicPressed = -1, musicAck = 0;
unsigned long musicAckUntil = 0;
unsigned long musicSparkUntil = 0;
int musicSparkButton = -1;
// HID adds a second GATT service to the existing authenticated navigation connection.
bool hidMusicMode = false;                  // persisted by #HM,0/1; Android explicitly selects 0
BLEHIDDevice *mediaHid = nullptr;
BLECharacteristic *mediaInput = nullptr;
volatile bool hidSubscribed = false, hidSuspended = false, hidReportMode = true;
volatile uint16_t hidSubscriber = 0xFFFF;
volatile bool hidNotifyAccepted = false;
volatile uint32_t hidSessionEpoch = 0;
uint32_t hidSeenEpoch = 0;
bool hidReadyShown = false, hidReleaseOnReady = true;
String pendingHidStatus, pendingLyricsStatus, pendingMediaStatus;
unsigned long hidInitialReleaseAt = 0;
HidMediaKeys::Pulses hidPulses;
unsigned long hidTapAt = 0;
void hidMediaTick();
bool hidMediaReady();
bool wxDirty = false;                 // v5.3 天气页要重画

// ================= v6.5 蓝牙配对验证 =================
#define PAIR_WINDOW_MS  120000UL      // 开机 2 分钟内可以配对新手机
#define AUTH_TIMEOUT_MS 60000UL       // 连上 60 秒还没通过验证就断开
volatile bool bleAuthed = false;      // 当前连接已加密 + 通过配对码验证
volatile uint16_t bleConnHandle = 0xFFFF;
volatile unsigned long bleConnAt = 0, bleAuthAt = 0;
volatile uint32_t pairCode = 0;       // 要显示的配对码
volatile bool pairReq = false;        // 有新的配对请求（蓝牙线程 → 主循环）
volatile bool pairLocked = false;     // 不在允许配对的时间：不显示配对码
volatile bool pairDone = false;       // 配对结束（成功或失败）
volatile bool pairOk = false;
int authNotifies = 0;                 // "U,1" 已经发了几次

int bondCount() {
  int n = 0;
#if defined(CONFIG_NIMBLE_ENABLED)
  ble_store_util_count(BLE_STORE_OBJ_TYPE_OUR_SEC, &n);
#endif
  return n;
}
bool pairingAllowed() { return millis() < PAIR_WINDOW_MS || bondCount() == 0; }

class SecCB : public BLESecurityCallbacks {
  uint32_t onPassKeyRequest() { return 0; }
  // 圆屏要显示配对码（每次配对随机生成）：只在允许配对的时间里真的显示出来
  void onPassKeyNotify(uint32_t k) { pairCode = k; pairLocked = !pairingAllowed(); pairDone = false; pairReq = true; }
  bool onSecurityRequest() { return true; }
  bool onConfirmPIN(uint32_t pin) { return false; }
  bool onAuthorizationRequest(uint16_t connHandle, uint16_t attrHandle, bool isRead) { return true; }
#if defined(CONFIG_NIMBLE_ENABLED)
  void onAuthenticationComplete(ble_gap_conn_desc *d) {
    bool ok = d && d->sec_state.encrypted && d->sec_state.authenticated;
    if (ok) { bleAuthed = true; bleAuthAt = millis(); authNotifies = 0; }
    pairOk = ok;
    pairDone = true;
  }
#endif
};

class HidInputCB : public BLECharacteristicCallbacks {
public:
#if defined(CONFIG_NIMBLE_ENABLED)
  void onSubscribe(BLECharacteristic *, ble_gap_conn_desc *desc, uint16_t value) override {
    hidSubscriber = desc ? desc->conn_handle : 0xFFFF;
    // HID hosts normally enable notifications (bit 0), but a few Android
    // Bluetooth stacks use indications (bit 1) for the same input report.
    // Treat either CCCD mode as subscribed; rejecting bit 1 leaves the phone
    // showing "connected" while media controls stay disabled.
    hidSubscribed = (value & 3) != 0; ++hidSessionEpoch;
  }
#endif
  void onStatus(BLECharacteristic *, Status status, uint32_t) override {
    hidNotifyAccepted = status == SUCCESS_NOTIFY;
  }
};
class HidControlCB : public BLECharacteristicCallbacks {
public:
  void onWrite(BLECharacteristic *c) override {
    if (c->getLength() == 1 && c->getData()[0] <= 1) { hidSuspended = c->getData()[0] == 0; ++hidSessionEpoch; }
  }
};
class HidProtocolCB : public BLECharacteristicCallbacks {
public:
  void onWrite(BLECharacteristic *c) override {
    if (c->getLength() == 1 && c->getData()[0] <= 1) { hidReportMode = c->getData()[0] == 1; ++hidSessionEpoch; }
  }
};
// Gate every report on this connection's pairing, host subscription and report mode.
bool hidMediaLinkReady() {
  // Android may keep the paired HID host state but omit a fresh CCCD
  // callback after the navigation GATT reconnect. The report characteristic
  // is still valid on this authenticated link, so do not block media keys on
  // that callback; otherwise the UI falls back to "OPEN MUSIC APP" forever.
  return mediaInput && bleConnected && bleAuthed && hidSubscribed &&
         (hidSubscriber == 0xFFFF || hidSubscriber == bleConnHandle) && !hidSuspended && hidReportMode;
}
bool hidMediaReady() { return hidMediaLinkReady() && !hidReleaseOnReady && hidSeenEpoch == hidSessionEpoch; }
bool hidMediaNotify(uint16_t usage) {
  if (!hidMediaLinkReady()) return false;
  uint8_t report[2]; HidMediaKeys::encode(usage, report);
  hidNotifyAccepted = false;
  mediaInput->setValue(report, sizeof(report)); mediaInput->notify();
  return hidNotifyAccepted;
}
void hidMediaTick() {
  static bool resetWritten = false;
  uint32_t epoch = hidSessionEpoch;
  if (hidSeenEpoch != epoch) {
    hidPulses.reset(); hidReleaseOnReady = true; resetWritten = false; hidSeenEpoch = epoch;
  }
  bool linkReady = hidMediaLinkReady();
  if (!linkReady) {
    if (!resetWritten) {
      hidPulses.reset(); hidReleaseOnReady = true;
      // 尽力把零报告 notify 出去：手机侧 HID host 在 suspend 等状态下会保持最后一份报告，
      // 不补发 release 媒体键就等效“长按”；断链时 notify 无接收者，是安全空操作
      if (mediaInput) { const uint8_t zero[2] = {0, 0}; mediaInput->setValue(zero, sizeof(zero)); mediaInput->notify(); }
      resetWritten = true;
    }
  } else if (hidReleaseOnReady) {
    resetWritten = false;
    if (!hidInitialReleaseAt || millis() - hidInitialReleaseAt >= 50) {
      hidInitialReleaseAt = millis();
      if (hidMediaNotify(0)) hidReleaseOnReady = false;
    }
  } else {
    if (!hidMusicMode) hidPulses.cancelQueued();
    HidMediaKeys::Pulses::Report report;
    if (hidPulses.poll(millis(), report)) {
      bool sent = hidMediaNotify(report.usage);
      hidPulses.submitted(report, millis(), sent);
      if (!report.release || !sent) {
        musicAck = sent ? 6 : 8; musicAckUntil = millis() + 2500; musicDirty = true;
      }
    }
  }
  bool ready = hidMediaReady();
  static int lastMode = -1;
  if (ready != hidReadyShown || lastMode != (int)hidMusicMode) {
    hidReadyShown = ready; lastMode = hidMusicMode;
    pendingHidStatus = String("HID,") + (hidMusicMode ? 1 : 0) + "," + (ready ? 1 : 0);
    musicDirty = true;
  }
}
void initMediaHid(BLEServer *server) {
  mediaHid = new BLEHIDDevice(server);
  mediaHid->manufacturer()->setValue("MotoNav");
  // DIY/private device identity, not a borrowed keyboard vendor/product ID.
  const uint8_t identity[7] = {0x01, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x01};
  mediaHid->deviceInfo()->getCharacteristic(BLEUUID((uint16_t)0x2A50))->setValue(identity, sizeof(identity));
  mediaHid->hidInfo(0, 0x02); // Normally Connectable, no remote wake claim
  mediaHid->reportMap(const_cast<uint8_t *>(HidMediaKeys::REPORT_MAP), sizeof(HidMediaKeys::REPORT_MAP));
  mediaInput = mediaHid->inputReport(HidMediaKeys::REPORT_ID);
  mediaInput->setCallbacks(new HidInputCB());
  const uint8_t zero[2] = {0, 0}; mediaInput->setValue(zero, sizeof(zero));
  mediaHid->hidControl()->setCallbacks(new HidControlCB());
  mediaHid->protocolMode()->setCallbacks(new HidProtocolCB());
  mediaHid->setBatteryLevel(100); // updated from the existing fuel gauge in the loop
  mediaHid->startServices();
}

class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer *s) {
    bleConnected = true; ++mediaConnectionEpoch;
    bleAuthed = false;
    hidSubscribed = false; hidSubscriber = 0xFFFF; hidSuspended = false; hidReportMode = true; ++hidSessionEpoch;
    bleConnAt = millis();
    authNotifies = 0;
  }
#if defined(CONFIG_NIMBLE_ENABLED)
  void onConnect(BLEServer *s, ble_gap_conn_desc *desc) {
    if (!desc) return;
    bleConnHandle = desc->conn_handle;
    // Keep navigation responsive while the phone is also using HID/AMS.
    ble_gap_upd_params params{};
    params.itvl_min = 12;             // 15 ms
    params.itvl_max = 24;             // 30 ms
    params.latency = 0;
    params.supervision_timeout = 600; // 6 s
    params.min_ce_len = 0;
    params.max_ce_len = 0;
    const int paramRc = ble_gap_update_params(desc->conn_handle, &params);
    Serial.printf("BLE connected handle=%u encrypted=%d authenticated=%d param_rc=%d\n",
                  desc->conn_handle, desc->sec_state.encrypted,
                  desc->sec_state.authenticated, paramRc);
    // NimBLE may resume an existing bond without calling
    // onAuthenticationComplete again.  The connection descriptor already
    // contains the authoritative encryption/authentication state, so accept
    // an encrypted bonded reconnect immediately instead of waiting for the
    // auth timeout (which used to require deleting and re-pairing the phone).
    if (desc->sec_state.encrypted && desc->sec_state.authenticated) {
      bleAuthed = true;
      bleAuthAt = millis();
      authNotifies = 0;
    }
  }
#endif
  void onDisconnect(BLEServer *s) {
    Serial.println("BLE disconnected");
    bleConnected = false; ++mediaConnectionEpoch;
    bleAuthed = false;
    bleConnHandle = 0xFFFF;
    hidSubscribed = false; hidSubscriber = 0xFFFF; ++hidSessionEpoch;
    if (!powerOffPending) BLEDevice::startAdvertising();
  }
};

// v2.6 保险：屏幕相关设置在蓝牙线程里直接处理（即使主循环被卡住也能改回来）
void saveAndRestart(const char *key, int val, bool isBool) {
  Preferences pr;
  pr.begin("motonav", false);
  if (isBool) pr.putBool(key, val != 0); else pr.putInt(key, val);
  pr.end();
  delay(100);
  ESP.restart();
}

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *c) {
    if (powerOffPending) return;
    size_t n = c->getLength();
    if (n == 0) return;
    const uint8_t *d0 = c->getData();
    if (n >= 4 && n < 12 && d0[0] == '#' && d0[2] == ',' && (d0[1] == 'f' || d0[1] == 'q')) {
      char num[12];
      size_t ln = n - 3;
      memcpy(num, d0 + 3, ln);
      num[ln] = 0;
      int v = atoi(num);
      if (d0[1] == 'f' && (v != 0) != fastFlushOn) saveAndRestart("fastf", v, true);
      if (d0[1] == 'q' && (v == 40 || v == 60 || v == 80) && v != qspiMHz) saveAndRestart("qspi", v, false);
      return;
    }
    if (n > RXQ_MAX) return;   // 超规格包整包丢弃：截断会让残缺分片被记成“已收”，解析出损坏数据；App 超时会重发
    portENTER_CRITICAL(&mux);
    int next = (rxHead + 1) % RXQ_SLOTS;
    if (next == rxTail) rxTail = (rxTail + 1) % RXQ_SLOTS;   // 队列满：丢掉最旧的一条
    rxq[rxHead].len = n;
    memcpy(rxq[rxHead].data, c->getData(), n);
    rxHead = next;
    portEXIT_CRITICAL(&mux);
  }
};

// ---- v2.2 双缓冲：核心 0 上的刷屏任务 ----
uint16_t *fbBuf[2] = {nullptr, nullptr};
int fbCur = 0;                    // 画布当前用的是哪一块
SemaphoreHandle_t flushReq = nullptr, flushDone = nullptr;
uint16_t *volatile flushSrc = nullptr;
volatile uint32_t flushMsAcc = 0, flushCnt = 0;
volatile uint32_t flushLastMs = 0, flushMaxMs = 0, flushTimeouts = 0;

// ---- v2.6 快速刷屏：自己往屏幕数据线上排队发送，两块缓冲轮流用（一块在传、一块在准备）----
#define FAST_PIX 4096                     // 每段像素数（8KB）
spi_device_handle_t fastDev = nullptr;
uint8_t *fastBuf[2] = {nullptr, nullptr};

int screenRot = 0;                        // v6.2 圆屏方向：0 / 1 / 2 / 3 = 0° / 90° / 180° / 270°
int screenDeg = 0;                        // v6.4 圆屏方向（度，0~359）
bool rotFree = false;                     // v6.4 不是 90° 的整数倍：推屏时逐像素按角度取样
int32_t rotCos = 65536, rotSin = 0;       // v6.4 角度的 cos / sin（×65536）
float rotCosF = 1, rotSinF = 0;
unsigned long rotSaveAt = 0;              // v6.4 角度停下后过一会儿再存进板子
int16_t rotSpan[LCD_HEIGHT];              // v6.4 每一行圆里面的半宽（圆外面的像素直接涂黑）

// v6.4 设定圆屏角度。save = true：稍后存进板子
void setScreenDeg(int d, bool save) {
  d = ((d % 360) + 360) % 360;
  screenDeg = d;
  rotFree = (d % 90) != 0;
  screenRot = rotFree ? 0 : d / 90;
  float a = d * (float)M_PI / 180.0f;
  rotCosF = cosf(a); rotSinF = sinf(a);
  rotCos = (int32_t)lroundf(rotCosF * 65536.0f);
  rotSin = (int32_t)lroundf(rotSinF * 65536.0f);
  if (save) rotSaveAt = millis() + 1500;
}

void initRotSpan() {
  const int c = LCD_WIDTH / 2;
  for (int y = 0; y < LCD_HEIGHT; y++) {
    int dy = y - c;
    int r2 = c * c - dy * dy;
    rotSpan[y] = r2 > 0 ? (int16_t)sqrtf((float)r2) : 0;
  }
}

// v6.4 停下 1.5 秒后把角度存起来（loop 里调用）
void saveRotIfDue() {
  if (!rotSaveAt || (long)(millis() - rotSaveAt) < 0) return;
  rotSaveAt = 0;
  Preferences pr;
  pr.begin("motonav", false);
  pr.putInt("rotd", screenDeg);
  pr.putInt("rot", screenRot);
  pr.end();
}

bool fastFlushInit() {
  spi_device_interface_config_t dc;
  memset(&dc, 0, sizeof(dc));
  dc.command_bits = 8;
  dc.address_bits = 24;
  dc.mode = 0;
  dc.clock_source = SPI_CLK_SRC_DEFAULT;
  dc.clock_speed_hz = qspiMHz * 1000000;
  dc.spics_io_num = -1;                   // 片选自己控制（和屏幕库一样）
  dc.flags = SPI_DEVICE_HALFDUPLEX;
  dc.queue_size = 2;
  if (spi_bus_add_device(SPI2_HOST, &dc, &fastDev) != ESP_OK) { fastDev = nullptr; return false; }
  for (int i = 0; i < 2; i++) {
    fastBuf[i] = (uint8_t *)heap_caps_aligned_alloc(16, FAST_PIX * 2, MALLOC_CAP_DMA);
    if (!fastBuf[i]) {
      for (int j = 0; j < i; j++) { heap_caps_free(fastBuf[j]); fastBuf[j] = nullptr; }
      spi_bus_remove_device(fastDev);                  // 别让没用的设备占着总线队列
      fastDev = nullptr;
      return false;
    }
  }
  return true;
}

// v7.7 RGB565 拆成 0x0GGRRBB 形式（绿、红、蓝之间留空），一次乘法就能三个颜色一起按权重混合
static inline uint32_t rgbSpread(uint16_t c) { return ((uint32_t)c | ((uint32_t)c << 16)) & 0x07E0F81Fu; }

void fastPush(const uint16_t *fb) {
  panel->startWrite();panel->writeAddrWindow(0,0,PANEL_WIDTH,PANEL_HEIGHT);panel->endWrite();
  spi_device_acquire_bus(fastDev,portMAX_DELAY);digitalWrite(LCD_CS,LOW);
  static spi_transaction_ext_t tr[2];
  uint32_t sent=0,total=PANEL_WIDTH*PANEL_HEIGHT;int bi=0,inflight=0;bool first=true;
  while(sent<total) {
    uint32_t count=min((uint32_t)FAST_PIX,total-sent);
    if(inflight==2){spi_transaction_t* result;spi_device_get_trans_result(fastDev,&result,portMAX_DELAY);inflight--;}
    uint16_t* out=(uint16_t*)fastBuf[bi];
    int y=sent/PANEL_WIDTH,x=sent%PANEL_WIDTH;
    for(uint32_t i=0;i<count;i++) {
      uint16_t pixel=portScale.pixel(fb,x,y,screenRot,rotFree,rotCos,rotSin);
      out[i]=(uint16_t)((pixel>>8)|(pixel<<8));
      if(++x==PANEL_WIDTH){x=0;y++;}
    }
    spi_transaction_ext_t& t=tr[bi];memset(&t,0,sizeof(t));
    if(first){t.base.flags=SPI_TRANS_MODE_QIO;t.base.cmd=0x32;t.base.addr=0x002C00;first=false;}
    else t.base.flags=SPI_TRANS_MODE_QIO|SPI_TRANS_VARIABLE_CMD|SPI_TRANS_VARIABLE_ADDR|SPI_TRANS_VARIABLE_DUMMY;
    t.base.tx_buffer=fastBuf[bi];t.base.length=count*16;
    spi_device_queue_trans(fastDev,(spi_transaction_t*)&t,portMAX_DELAY);
    inflight++;bi^=1;sent+=count;
  }
  while(inflight){spi_transaction_t* result;spi_device_get_trans_result(fastDev,&result,portMAX_DELAY);inflight--;}
  digitalWrite(LCD_CS,HIGH);spi_device_release_bus(fastDev);
}

void flushLogicalFrame(const uint16_t* fb) {
  if(!fb)return;
  if(fastDev){fastPush(fb);return;}
  // 标准刷屏和低内存路径也走相同缩放，不能把 466 像素直接写到 360 像素面板。
  static uint16_t* physical=nullptr;
  if(!physical)physical=(uint16_t*)ps_malloc(PANEL_WIDTH*PANEL_HEIGHT*2);
  if(physical) {
    for(int y=0;y<PANEL_HEIGHT;y++)for(int x=0;x<PANEL_WIDTH;x++)physical[y*PANEL_WIDTH+x]=portScale.pixel(fb,x,y,screenRot,rotFree,rotCos,rotSin);
    panel->draw16bitRGBBitmap(0,0,physical,PANEL_WIDTH,PANEL_HEIGHT);
  }else {
    uint16_t row[PANEL_WIDTH];
    for(int y=0;y<PANEL_HEIGHT;y++) {
      for(int x=0;x<PANEL_WIDTH;x++)row[x]=portScale.pixel(fb,x,y,screenRot,rotFree,rotCos,rotSin);
      panel->draw16bitRGBBitmap(0,y,row,PANEL_WIDTH,1);
    }
  }
}

void flushTask(void *arg) {
  for (;;) {
    xSemaphoreTake(flushReq, portMAX_DELAY);
    uint32_t t0 = millis();
    static uint32_t flushDiagAt = 0;
    if (t0 - flushDiagAt >= 5000) {
      flushDiagAt = t0;
#if 0
      Serial.printf("[LCD-FLUSH] begin fast=%d qspi=%d heap=%u psram=%u\n",
                    fastDev != nullptr, qspiMHz, ESP.getFreeHeap(), ESP.getFreePsram());
#endif
    }
    flushLogicalFrame(flushSrc);
    uint32_t elapsed = millis() - t0;
    flushLastMs = elapsed;
    if (elapsed > flushMaxMs) flushMaxMs = elapsed;
    flushMsAcc += elapsed; flushCnt++;
    if (elapsed >= 500 || t0 - flushDiagAt < 100) {
#if 0
      Serial.printf("[LCD-FLUSH] done ms=%u count=%u max=%u\n",
                    (unsigned)elapsed, (unsigned)flushCnt, (unsigned)flushMaxMs);
#endif
    }
    xSemaphoreGive(flushDone);
  }
}

// v2.6 等刷屏完成；超过 3 秒还没完成，说明快速刷屏在这块板子上不工作：关掉它并重启
void flushWait() {
  if (xSemaphoreTake(flushDone, pdMS_TO_TICKS(3000)) == pdTRUE) return;
  flushTimeouts++;
#if 0
  Serial.printf("[LCD-FLUSH] TIMEOUT last=%u max=%u count=%u timeouts=%u fast=%d qspi=%d\n",
                (unsigned)flushLastMs, (unsigned)flushMaxMs, (unsigned)flushCnt,
                (unsigned)flushTimeouts, fastDev != nullptr, qspiMHz);
#endif
  if (fastFlushOn) saveAndRestart("fastf", 0, true);
  xSemaphoreTake(flushDone, portMAX_DELAY);
}

// 异步刷屏：把刚画好的这一块交给核心 0 推到屏幕，画布换到另一块继续画下一帧
void flushAsync() {
  if (!fbBuf[1]) { gfx->flush(); return; }
  flushWait();                                    // 等上一帧推完
  flushSrc = fbBuf[fbCur];
  xSemaphoreGive(flushReq);
  fbCur ^= 1;
  gfx->setFramebuffer(fbBuf[fbCur]);
}

// 同步刷屏（普通画面用，等推完再返回）
void flushSync() {
  if (!fbBuf[1]) { gfx->flush(); return; }
  flushWait();
  flushSrc = fbBuf[fbCur];
  xSemaphoreGive(flushReq);
  flushWait();                                    // 等这一帧推完
  xSemaphoreGive(flushDone);
}

void initDoubleBuffer() {
  fbBuf[0] = gfx->getFramebuffer();
  fbBuf[1] = (uint16_t *)heap_caps_aligned_alloc(16, LCD_WIDTH * LCD_HEIGHT * 2, MALLOC_CAP_SPIRAM);
  if (!fbBuf[1]) return;
  memcpy(fbBuf[1], fbBuf[0], LCD_WIDTH * LCD_HEIGHT * 2);
  if (fastFlushOn) fastFlushInit();                // v2.6 快速刷屏
  flushReq = xSemaphoreCreateBinary();
  flushDone = xSemaphoreCreateBinary();
  xSemaphoreGive(flushDone);
  xTaskCreatePinnedToCore(flushTask, "flush", 4096, nullptr, 2, nullptr, 0);
}

// ================= 基础画图工具 =================

int16_t rnd(float v) { return (int16_t)lroundf(v); }

// ================= v1.8.1 抗锯齿矢量图标 =================
// 画大图标时先把“线段 / 三角形 / 圆”记录下来，再逐像素按到图形边缘的距离算覆盖率，
// 边缘自然过渡（抗锯齿），线段之间的连接也没有缝。结果缓存起来，图标不变就直接复制。
// Prim 结构、prims/primN/recIcon 在 MotoNavCommon.h
Prim prims[PRIM_MAX];
int primN = 0;
bool recIcon = false;      // true = 正在记录图标，thickLine/arrowHead/iconCircle 只记录不直接画
#define ICON_BUF_W 360
#define ICON_BUF_H 360
uint16_t *iconBuf = nullptr;
int iconX = 0, iconY = 0, iconW = 0, iconH = 0;
uint32_t iconHash = 0;

void addPrim(const struct Prim &p) { if (primN < PRIM_MAX) prims[primN++] = p; }

void iconBegin() { primN = 0; recIcon = true; }

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// 一个像素中心 (px,py) 被图形覆盖的比例 0~1
float primAlpha(const struct Prim &p, float px, float py) {
  if (p.type == 0) {
    float dx = p.bx - p.ax, dy = p.by - p.ay;
    float len2 = dx * dx + dy * dy;
    float qx = px - p.ax, qy = py - p.ay;
    if (len2 < 1e-4f) return clamp01(p.r - sqrtf(qx * qx + qy * qy) + 0.5f);
    float h = (qx * dx + qy * dy) / len2;
    if (h < 0) {                                   // 起点：平头
      float len = sqrtf(len2);
      float perp = fabsf(qx * dy - qy * dx) / len;
      return clamp01(p.r - perp + 0.5f) * clamp01(0.5f + h * len);
    }
    if (h > 1) h = 1;                              // 终点：圆头
    float ex = qx - dx * h, ey = qy - dy * h;
    return clamp01(p.r - sqrtf(ex * ex + ey * ey) + 0.5f);
  }
  if (p.type == 1) {
    float vx[3] = {p.ax, p.bx, p.cx}, vy[3] = {p.ay, p.by, p.cy};
    float area = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vy[1] - vy[0]) * (vx[2] - vx[0]);
    float sgn = area > 0 ? 1.0f : -1.0f;
    float m = 1e9f;
    for (int i = 0; i < 3; i++) {
      int j = (i + 1) % 3;
      float ex = vx[j] - vx[i], ey = vy[j] - vy[i];
      float len = sqrtf(ex * ex + ey * ey);
      if (len < 1e-4f) return 0;
      float d = sgn * ((px - vx[i]) * ey - (py - vy[i]) * ex) / len;   // 到这条边的距离（里面为负）
      if (-d < m) m = -d;
    }
    return clamp01(m + 0.5f);
  }
  float qx = px - p.ax, qy = py - p.ay;
  return clamp01(p.r - sqrtf(qx * qx + qy * qy) + 0.5f);
}

bool iconHalo = false;              // v2.8 图标外加黑色描边（浮在地图上时用）
#define HALO_R 3

void iconRender() {
  float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
  for (int i = 0; i < primN; i++) {
    bx0 = fminf(bx0, prims[i].x0); by0 = fminf(by0, prims[i].y0);
    bx1 = fmaxf(bx1, prims[i].x1); by1 = fmaxf(by1, prims[i].y1);
  }
  int mg = iconHalo ? HALO_R + 1 : 0;
  iconX = (int)floorf(bx0) - 1 - mg; iconY = (int)floorf(by0) - 1 - mg;
  if (iconX < 0) iconX = 0;
  if (iconY < 0) iconY = 0;
  iconW = (int)ceilf(bx1) + 2 + mg - iconX; iconH = (int)ceilf(by1) + 2 + mg - iconY;
  if (iconX + iconW > LCD_WIDTH)  iconW = LCD_WIDTH - iconX;
  if (iconY + iconH > LCD_HEIGHT) iconH = LCD_HEIGHT - iconY;
  if (iconW > ICON_BUF_W) iconW = ICON_BUF_W;
  if (iconH > ICON_BUF_H) iconH = ICON_BUF_H;
  if (iconW <= 0 || iconH <= 0) { iconW = iconH = 0; return; }
  for (int y = 0; y < iconH; y++) {
    float py = iconY + y + 0.5f;
    for (int x = 0; x < iconW; x++) {
      float px = iconX + x + 0.5f;
      float r = 0, g = 0, b = 0;                  // 背景是黑色，按顺序叠加每个图形
      for (int i = 0; i < primN; i++) {
        const Prim &p = prims[i];
        if (px < p.x0 || px > p.x1 || py < p.y0 || py > p.y1) continue;
        float a = primAlpha(p, px, py);
        if (a <= 0) continue;
        float cr = ((p.col >> 11) & 0x1F) / 31.0f, cg = ((p.col >> 5) & 0x3F) / 63.0f, cb = (p.col & 0x1F) / 31.0f;
        r += (cr - r) * a; g += (cg - g) * a; b += (cb - b) * a;
      }
      iconBuf[y * ICON_BUF_W + x] = ((uint16_t)lroundf(r * 31) << 11) | ((uint16_t)lroundf(g * 63) << 5) | (uint16_t)lroundf(b * 31);
    }
  }
  // v2.8 描边：图形周围 HALO_R 像素以内的空白处涂成接近黑色（纯黑 0 表示透明，所以用 0x0841）
  if (iconHalo) {
    for (int y = 0; y < iconH; y++) {
      for (int x = 0; x < iconW; x++) {
        if (iconBuf[y * ICON_BUF_W + x]) continue;
        bool near = false;
        for (int dy = -HALO_R; dy <= HALO_R && !near; dy++) {
          int yy = y + dy;
          if (yy < 0 || yy >= iconH) continue;
          for (int dx = -HALO_R; dx <= HALO_R; dx++) {
            int xx = x + dx;
            if (xx < 0 || xx >= iconW || dx * dx + dy * dy > HALO_R * HALO_R + 1) continue;
            uint16_t v = iconBuf[yy * ICON_BUF_W + xx];
            if (v && v != 0x0841) { near = true; break; }
          }
        }
        if (near) iconBuf[y * ICON_BUF_W + x] = 0x0841;
      }
    }
  }
}

// v1.8.2：把记录好的图标整体缩放、居中到 [top, bottom] 区域里（只缩小不放大）
void iconFitAt(float cx, float top, float bottom, float maxW) {
  if (primN == 0) return;
  float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
  for (int i = 0; i < primN; i++) {
    bx0 = fminf(bx0, prims[i].x0 + 1); by0 = fminf(by0, prims[i].y0 + 1);
    bx1 = fmaxf(bx1, prims[i].x1 - 1); by1 = fmaxf(by1, prims[i].y1 - 1);
  }
  float w = bx1 - bx0, h = by1 - by0;
  if (w <= 0 || h <= 0) return;
  float sc = fminf(1.0f, fminf((bottom - top) / h, maxW / w));
  float ox = (bx0 + bx1) / 2, oy = (by0 + by1) / 2;
  float nx = cx, ny = (top + bottom) / 2;
  for (int i = 0; i < primN; i++) {
    Prim &p = prims[i];
    p.ax = nx + (p.ax - ox) * sc; p.ay = ny + (p.ay - oy) * sc;
    p.bx = nx + (p.bx - ox) * sc; p.by = ny + (p.by - oy) * sc;
    p.cx = nx + (p.cx - ox) * sc; p.cy = ny + (p.cy - oy) * sc;
    p.r *= sc;
    p.x0 = nx + (p.x0 + 1 - ox) * sc - 1; p.y0 = ny + (p.y0 + 1 - oy) * sc - 1;
    p.x1 = nx + (p.x1 - 1 - ox) * sc + 1; p.y1 = ny + (p.y1 - 1 - oy) * sc + 1;
  }
}

void iconFit(float top, float bottom, float maxW) { iconFitAt(CX, top, bottom, maxW); }

// 结束记录：图标和上次一样就直接用缓存，否则重新计算；然后复制到画布
void iconEnd() {
  recIcon = false;
  if (!iconBuf || primN == 0) return;
  uint32_t h = 2166136261u;                      // FNV 哈希，判断图标有没有变化
  const uint8_t *bytes = (const uint8_t *)prims;
  for (size_t i = 0; i < sizeof(Prim) * primN; i++) { h ^= bytes[i]; h *= 16777619u; }
  h ^= primN;
  h ^= iconHalo ? 0x5A5A5A5Au : 0;
  if (h != iconHash) { iconRender(); iconHash = h; }
  uint16_t *fb = gfx->getFramebuffer();
  if (!fb) return;
  for (int y = 0; y < iconH; y++) {
    const uint16_t *src = iconBuf + y * ICON_BUF_W;
    uint16_t *dst = fb + (iconY + y) * LCD_WIDTH + iconX;
    for (int x = 0; x < iconW; x++) if (src[x]) dst[x] = src[x];
  }
}

void iconCircle(float x, float y, float r, uint16_t c) {
  if (recIcon) {
    Prim p; memset(&p, 0, sizeof(p));
    p.type = 2; p.col = c; p.ax = x; p.ay = y; p.r = r;
    p.x0 = x - r - 1; p.y0 = y - r - 1; p.x1 = x + r + 1; p.y1 = y + r + 1;
    addPrim(p);
    return;
  }
  gfx->fillCircle(x, y, r, c);
}

// 有宽度的线（终点带圆头）
void thickLine(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  if (recIcon) {
    Prim p; memset(&p, 0, sizeof(p));
    float r = w / 2;
    p.type = 0; p.col = c; p.ax = x0; p.ay = y0; p.bx = x1; p.by = y1; p.r = r;
    p.x0 = fminf(x0, x1) - r - 1; p.y0 = fminf(y0, y1) - r - 1;
    p.x1 = fmaxf(x0, x1) + r + 1; p.y1 = fmaxf(y0, y1) + r + 1;
    addPrim(p);
    return;
  }
  float dx = x1 - x0, dy = y1 - y0;
  float len = sqrtf(dx * dx + dy * dy);
  if (len < 1) return;
  float nx = -dy / len * w / 2, ny = dx / len * w / 2;
  gfx->fillTriangle(x0 + nx, y0 + ny, x0 - nx, y0 - ny, x1 + nx, y1 + ny, c);
  gfx->fillTriangle(x0 - nx, y0 - ny, x1 - nx, y1 - ny, x1 + nx, y1 + ny, c);
  gfx->fillCircle(x1, y1, w / 2, c);
}

// 箭头头部；deg：0 = 朝上，-90 = 朝左，90 = 朝右，180 = 朝下
void arrowHead(float x, float y, float deg, float s, uint16_t c) {
  float a = deg * PI / 180;
  float dx = sinf(a), dy = -cosf(a);
  float px = cosf(a), py = sinf(a);
  if (recIcon) {
    Prim p; memset(&p, 0, sizeof(p));
    p.type = 1; p.col = c;
    p.ax = x + dx * s;           p.ay = y + dy * s;
    p.bx = x + px * s * 0.8f;    p.by = y + py * s * 0.8f;
    p.cx = x - px * s * 0.8f;    p.cy = y - py * s * 0.8f;
    p.x0 = fminf(fminf(p.ax, p.bx), p.cx) - 1; p.y0 = fminf(fminf(p.ay, p.by), p.cy) - 1;
    p.x1 = fmaxf(fmaxf(p.ax, p.bx), p.cx) + 1; p.y1 = fmaxf(fmaxf(p.ay, p.by), p.cy) + 1;
    addPrim(p);
    return;
  }
  gfx->fillTriangle(x + dx * s, y + dy * s,
                    x + px * s * 0.8f, y + py * s * 0.8f,
                    x - px * s * 0.8f, y - py * s * 0.8f, c);
}


// 居中打印一行字（自带字体每个字符宽 6*size 像素）
void printCentered(const String &s, int x, int y, int size, uint16_t color) {
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x - (int)s.length() * 6 * size / 2, y);
  gfx->print(s);
}
// 每帧都会调的路径用这个重载：字符串字面量和栈缓冲不再构造 String（省一次堆分配/帧）
void printCentered(const char *s, int x, int y, int size, uint16_t color) {
  if (!s) return;
  gfx->setTextSize(size);
  gfx->setTextColor(color);
  gfx->setCursor(x - (int)strlen(s) * 6 * size / 2, y);
  gfx->print(s);
}

void distTextBuf(int m, char *out) {           // out 至少 12 字节；distText 的栈缓冲版
  if (m >= 1000) snprintf(out, 12, "%.1fkm", m / 1000.0);
  else snprintf(out, 12, "%dm", m);
}

String distText(int m) {
  char b[12]; distTextBuf(m, b);
  return String(b);
}



// 顶部小圆点：绿 = 蓝牙已连接，灰 = 未连接
// ================= 1.85B 电量计 BQ27220（只读取，不改写充电或电池参数）=================
GaugeBQ27220 batteryGauge;
bool pmuOK=false;
int batPct=-1;
bool batChg=false,usbIn=false;
int batMv=0;
unsigned long batAt=0,batSentAt=0;
String pendingBat="";
void batInit() {
  pmuOK=batteryGauge.begin(Wire,IIC_SDA,IIC_SCL);
  Serial.println(pmuOK?"BQ27220 ok":"BQ27220 not found (navigation continues)");
}

// 小电池图标：x0,y0 = 左上角（电池身 24x12，右边小凸头，再右边是百分比数字）
void drawBattery(int x0, int y0, bool backing) {
  if (batPct < 0) return;                                  // 没接电池不画
  if (backing) {                                           // v7.2 小地图上垫一块黑底
    int tw = String(batPct).length() * 6;
    gfx->fillRoundRect(x0 - 4, y0 - 3, 24 + 2 + 5 + tw + 7, 12 + 6, 6, C_BLACK);
  }
  uint16_t c = batChg ? C_CYAN : batPct > 50 ? C_GREEN : batPct >= 20 ? 0xFE60 : C_RED;
  if (!batChg && batPct < 10 && (millis() / 600) % 2) c = 0x4208;   // 快没电：闪
  const int W = 24, H = 12;
  gfx->drawRoundRect(x0, y0, W, H, 3, c);
  gfx->drawRoundRect(x0 + 1, y0 + 1, W - 2, H - 2, 2, c);
  gfx->fillRect(x0 + W, y0 + 3, 2, H - 6, c);
  int fw = (W - 6) * constrain(batPct, 0, 100) / 100;
  if (fw > 0) gfx->fillRect(x0 + 3, y0 + 3, fw, H - 6, c);
  if (batChg) {                                            // 小闪电
    int bx = x0 + W / 2, by = y0 + H / 2;
    gfx->fillTriangle(bx + 1, by - 5, bx - 3, by + 1, bx, by + 1, C_BLACK);
    gfx->fillTriangle(bx - 1, by + 5, bx + 3, by - 1, bx, by - 1, C_BLACK);
  }
  gfx->setTextSize(1);
  gfx->setTextColor(c);
  gfx->setCursor(x0 + W + 5, y0 + 2);
  gfx->print(String(batPct));
}

void drawStatus() {
  gfx->fillRect(CX - 9, 21, 9 + 14 + 24 + 2 + 5 + 18 + 4, 19, C_BLACK);   // v7.2 先擦掉旧的（只重画状态栏时用）
  aaDisc(CX, 30, 7, bleConnected ? C_GREEN : C_GREY);
  drawBattery(CX + 14, 24);                               // v6.8 电量（蓝牙小圆点右边）
  needFlush = true;
}

static void riderFillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  gfx->fillRect(x, y, w, h, color);
}

static void riderPrintText(const char *text, int x, int y, int size, uint16_t color) {
  printCentered(String(text), x, y, size, color);
}

static void riderDrawStatusBar() {
  drawStatus();
}


// ================= 文字图片 =================

// 把 4 位灰度图画到 (中心 x, 顶部 y)，tint = 最亮处的灰度 0~255
// v1.7.1：如果图片超出半径 TEXT_R 的圆，就整体等比缩小（按面积取平均，缩小后依然平滑）
static inline uint8_t imgNib(const TextImg &im, int bpr, int x, int y) {
  uint8_t b = im.show[y * bpr + (x >> 1)];
  return (x & 1) ? (b & 0x0F) : (b >> 4);
}

static inline uint16_t greyColor(uint8_t a, uint8_t tint) {      // a = 0~15
  uint8_t g = (uint16_t)(a * 17) * tint / 255;
  return ((g >> 3) << 11) | ((g >> 2) << 5) | (g >> 3);
}

void drawTextImg(int slot, int y0, uint8_t tint) {
  TextImg &im = imgs[slot];
  if (!im.show || im.showW == 0 || im.showH == 0) return;
  int bpr = (im.showW + 1) / 2;

  // 算缩放比例：图片四个角都要落在半径 TEXT_R 的圆里
  float s = 1.0f;
  for (int it = 0; it < 4; it++) {
    float h = im.showH * s;
    float dy = fmaxf(fabsf(y0 - CX), fabsf(y0 + h - CX));
    float half = dy < TEXT_R ? sqrtf((float)TEXT_R * TEXT_R - dy * dy) : 0;
    if (im.showW * s / 2 <= half) break;
    s = 2 * half / im.showW;
  }
  if (s < 0.5f) s = 0.5f;

  if (s >= 0.999f) {                       // 不用缩小：原样画
    int x0 = CX - im.showW / 2;
    for (int y = 0; y < im.showH; y++)
      for (int x = 0; x < im.showW; x++) {
        uint8_t a = imgNib(im, bpr, x, y);
        if (a) gfx->drawPixel(x0 + x, y0 + y, greyColor(a, tint));
      }
    return;
  }

  int dw = (int)(im.showW * s), dh = (int)(im.showH * s);
  if (dw < 1 || dh < 1) return;
  int x0 = CX - dw / 2;
  float inv = 1.0f / s;
  for (int dy = 0; dy < dh; dy++) {
    int sy0 = (int)(dy * inv), sy1 = (int)((dy + 1) * inv);
    if (sy1 <= sy0) sy1 = sy0 + 1;
    if (sy1 > im.showH) sy1 = im.showH;
    for (int dx = 0; dx < dw; dx++) {
      int sx0 = (int)(dx * inv), sx1 = (int)((dx + 1) * inv);
      if (sx1 <= sx0) sx1 = sx0 + 1;
      if (sx1 > im.showW) sx1 = im.showW;
      int sum = 0, cnt = 0;
      for (int y = sy0; y < sy1; y++)
        for (int x = sx0; x < sx1; x++) { sum += imgNib(im, bpr, x, y); cnt++; }
      uint8_t a = cnt ? (sum + cnt / 2) / cnt : 0;
      if (a) gfx->drawPixel(x0 + dx, y0 + dy, greyColor(a, tint));
    }
  }
}

// 'B' slot id part parts wLo wHi h offLo offHi + 数据
bool ensureTextImgBuffers(TextImg &im, int slot) {
  if (!im.show) im.show = (uint8_t *)ps_malloc(IMG_MAX);
  if (!im.recv) im.recv = (uint8_t *)ps_malloc(IMG_MAX);
  if (im.show && im.recv) return true;
  if (!imgAllocWarned[slot]) {
    imgAllocWarned[slot] = true;
    Serial.printf("bitmap: slot %d buffer allocation failed (show=%p recv=%p)\n",
                  slot, (void *)im.show, (void *)im.recv);
  }
  return false;
}

void handleBitmap(const uint8_t *d, int n) {
  if (n < 10) return;
  int slot = d[1], id = d[2], part = d[3], parts = d[4];
  int w = d[5] | (d[6] << 8), h = d[7];
  int off = d[8] | (d[9] << 8);
  if (slot >= IMG_SLOTS) return;
  TextImg &im = imgs[slot];
  if (w == 0 || h == 0 || parts == 0) {          // 空图：清掉这一栏
    im.showW = 0; im.showH = 0; im.recvId = -1;
    navDirty = true;
    if (slot == 3 || slot == 4) wxDirty = true;
    if (slot == 5 || slot == 6 || slot == 12) musicDirty = true;
    return;
  }
  if (slot == 12 && (!musicLyrics || !hidMusicMode || !appleMedia.available ||
      lyricImageGeneration != appleMedia.generation || lyricImageTitleRevision != appleMedia.values[0].revision || lyricImageArtistRevision != appleMedia.values[1].revision)) return;
  int total = ((w + 1) / 2) * h;
  if (total > IMG_MAX || parts > 32) return;
  if (part >= parts || off >= total) return;
  if (!ensureTextImgBuffers(im, slot)) return;
  if (id != im.recvId || w != im.recvW || h != im.recvH || parts != im.recvParts) {
    // 新的一张图（或同 id 但几何变了）：重新开始接收，避免沿用旧 mask 把残图拼起来
    im.recvId = id; im.recvW = w; im.recvH = h; im.recvParts = parts; im.recvMask = 0;
  }
  int len = n - 10;
  if (off + len > total) len = total - off;
  if (len > 0) memcpy(im.recv + off, d + 10, len);
  im.recvMask |= (1UL << part);
  uint32_t full = (parts >= 32) ? 0xFFFFFFFFUL : ((1UL << parts) - 1);
  if ((im.recvMask & full) == full) {             // 收齐了：换成显示用的图
    uint8_t *t = im.show; im.show = im.recv; im.recv = t;
    im.showW = im.recvW; im.showH = im.recvH;
    im.recvId = -1;
    navDirty = true;
    if (slot == 3 || slot == 4) wxDirty = true;
    if (slot == 5 || slot == 6 || slot == 12) musicDirty = true;                // 音乐文字图片收齐后刷新
  }
}

// ================= 整屏画面 =================

#define WAIT_READY  0
#define WAIT_NOLINK 1
#define WAIT_LOST   2

// ================= v4.6 首页像素骑手（代替 v3.6 的猫猫） =================
// 原创的像素女骑手 + 小摩托：连上手机时围巾和马尾随风飘、偶尔眨眼、每隔几秒挥挥手；
// 没连上时摩托车灯熄灭、她闭眼打瞌睡，头上飘 z
// v4.6 首页像素骑手（原创角色）：40×26，每格放大 RIDER_S 倍；由 rider2.py 生成
// 帧：stand0、stand1、blink0、blink1、wave0、wave1、sleep
// 页面槽位：负一屏音乐，第一屏主页/导航，第二屏 IMU，第三屏天气（枚举在 MotoNavCommon.h）。
int page = PAGE_HOME;
unsigned long pageAt = 0;

// QMI8658 6 轴融合。没有磁力计时 yaw 只能表示相对方向，不能提供绝对罗盘北方。
SensorQMI8658 imu;
bool imuOK = false;
float imuRoll=0, imuPitch=0, imuYaw=0, imuG=1, imuAx=0, imuAy=0, imuAz=0;
float imuLinearG=0;
float imuPeakG=0;                  // v7.13.3 G 值表：重置以来的峰值（点屏幕清零）
float imuBiasRoll=0, imuBiasPitch=0, imuBiasYaw=0;
float imuGyroBiasX=0, imuGyroBiasY=0, imuGyroBiasZ=0;
uint32_t imuLastMs=0;
uint32_t imuBootMs=0, imuStillSince=0;
uint16_t imuCalSamples=0;
float imuCalX=0, imuCalY=0, imuCalZ=0;
bool imuFilterStarted=false;

static float imuWrap180(float deg) {
  while (deg > 180.0f) deg -= 360.0f;
  while (deg < -180.0f) deg += 360.0f;
  return deg;
}

static const char *imuDirection() {
  const float h = imuWrap180(imuYaw - imuBiasYaw);
  if (h >= -45.0f && h < 45.0f) return "FORWARD";
  if (h >= 45.0f && h < 135.0f) return "RIGHT";
  if (h >= -135.0f && h < -45.0f) return "LEFT";
  return "BACK";
}

// 用户每次开机后点一下，把当前姿态作为 roll/pitch/yaw 零点。
void imuResetReference() {
  imuBiasRoll = imuRoll;
  imuBiasPitch = imuPitch;
  imuBiasYaw = imuYaw;
  imuPeakG = 0;                    // v7.13.3 峰值一并清零
  imuStillSince = millis();
}

void imuTick(){
  if(!imuOK) return;
  AccelerometerData a; GyroscopeData g;
  if(!imu.readAccel(a) || !imu.readGyro(g)) return;
  const uint32_t now = millis();
  const float dt = imuLastMs ? constrain((now - imuLastMs) * 0.001f, 0.001f, 0.05f) : 0.01f;
  imuLastMs = now;
  if (!imuBootMs) imuBootMs = now;

  imuAx=a.mps2.x/9.80665f;
  imuAy=a.mps2.y/9.80665f;
  imuAz=a.mps2.z/9.80665f;
  imuG=sqrtf(imuAx*imuAx+imuAy*imuAy+imuAz*imuAz);
  const float ar=atan2f(imuAy,imuAz)*57.2957795f;
  const float ap=atan2f(-imuAx,sqrtf(imuAy*imuAy+imuAz*imuAz))*57.2957795f;
  const bool stationaryRaw = fabsf(imuG - 1.0f) < 0.15f &&
                             fabsf(g.dps.x) < 3.0f && fabsf(g.dps.y) < 3.0f &&
                             fabsf(g.dps.z) < 3.0f;
  imuLinearG = stationaryRaw ? 0.0f : fabsf(imuG - 1.0f);

  // 开机前 3 秒只在确认静止时采集 gyro 零偏，避免把手拿起时的动作写进校准值。
  if (now - imuBootMs < 3000 && stationaryRaw) {
    imuCalX += g.dps.x; imuCalY += g.dps.y; imuCalZ += g.dps.z;
    if (imuCalSamples < 60000) imuCalSamples++;
    if (imuCalSamples >= 8) {
      imuGyroBiasX = imuCalX / imuCalSamples;
      imuGyroBiasY = imuCalY / imuCalSamples;
      imuGyroBiasZ = imuCalZ / imuCalSamples;
    }
  }

  if (!imuFilterStarted) {
    imuRoll = ar;
    imuPitch = ap;
    imuYaw = 0;
    imuFilterStarted = true;
  }

  float wx = g.dps.x - imuGyroBiasX;
  float wy = g.dps.y - imuGyroBiasY;
  float wz = g.dps.z - imuGyroBiasZ;
  if (stationaryRaw) {
    if (!imuStillSince) imuStillSince = now;
    // 静止超过 0.5 s 后慢速跟踪温漂，避免 yaw 在桌面上继续累积。
    if (now - imuStillSince > 500) {
      imuGyroBiasX += (g.dps.x - imuGyroBiasX) * 0.002f;
      imuGyroBiasY += (g.dps.y - imuGyroBiasY) * 0.002f;
      imuGyroBiasZ += (g.dps.z - imuGyroBiasZ) * 0.002f;
      wx = g.dps.x - imuGyroBiasX;
      wy = g.dps.y - imuGyroBiasY;
      wz = g.dps.z - imuGyroBiasZ;
    }
  } else {
    imuStillSince = 0;
  }
  if (fabsf(wz) < 0.35f) wz = 0;

  // 加速度只在接近 1 g 时校正 roll/pitch，急加速/刹车时避免把线性加速度当重力。
  const float accelWeight = fabsf(imuG - 1.0f) < 0.20f ? 0.04f : 0.0f;
  const float gyroWeight = 1.0f - accelWeight;
  // v7.13.4 真倾角：稳态压弯时合力正好沿车身竖直轴，atan2(ay,az) 会把倾角读成 ~0。
  // 用去重力后的侧向比力把加速度基准补偿回来（tan(修正)=侧向比力/g），
  // 静止时补偿量为 0，稳态压弯时恰好补回真实倾角，加减速（纵向）不影响 roll。
  float arCorr = ar;
  {
    const float phR = imuRoll * 0.017453293f;
    const float latAcc = imuAy * cosf(phR) - imuAz * sinf(phR);   // 侧向比力（去重力，g）
    if (fabsf(latAcc) > 0.015f) arCorr = ar + atan2f(-latAcc, fabsf(imuAz));
  }
  imuRoll = gyroWeight * (imuRoll + wx * dt) + accelWeight * arCorr;
  imuPitch = gyroWeight * (imuPitch + wy * dt) + accelWeight * ap;
  imuYaw = imuWrap180(imuYaw + wz * dt);
}
// v7.13.3 G 值表（gg 图）：黑底 + 蓝色分段弧刻度（1.0g 内圈 / 1.5g 外圈）+ 黄绿 G 矢量圆点 + 中央合 G 数字。
// 加速度先按 roll/pitch 去重力，得到水平系的侧向/纵向 G（压弯时比 |g|-1 准）；点屏幕重置零点和峰值。
static void imuArcSeg(float cx, float cy, float r, float a0, float a1, uint16_t c) {
  const float D = 0.017453293f;
  float px = cx + cosf(a0 * D) * r, py = cy + sinf(a0 * D) * r;
  for (float a = a0 + 3; a <= a1 + 0.01f; a += 3) {
    float nx = cx + cosf(a * D) * r, ny = cy + sinf(a * D) * r;
    gfx->drawLine((int)px, (int)py, (int)nx, (int)ny, c);
    px = nx; py = ny;
  }
}
void drawImuPage(){
  onWaitScreen = false;
  gfx->fillScreen(C_BLACK);
  const uint16_t C_CHART = 0xCEA0;   // 黄绿（点 / 外圈光晕）
  const uint16_t C_GBLUE = 0x3CFF;   // 刻度蓝
  if(!imuOK){ printCentered("QMI8658 NOT FOUND",CX,190,2,C_RED); drawPageDots(); needFlush=true; return; }
  const float cyG = 233;                           // v7.13.4 仪表几何居中（画布中心）
  // ---- 去重力：水平系侧向 / 纵向加速度（g）。轴约定与 roll/pitch 一致：
  //      X = 前后（pitch 绕 Y），Y = 左右（roll 绕 X）。yLat = 侧向，xLon = 纵向 ----
  const float ph = imuWrap180(imuRoll - imuBiasRoll) * 0.017453293f;
  const float th = imuWrap180(imuPitch - imuBiasPitch) * 0.017453293f;
  const float xLon = imuAx * cosf(th) + imuAz * sinf(th);
  const float z1 = -imuAx * sinf(th) + imuAz * cosf(th);
  const float yLat = imuAy * cosf(ph) - z1 * sinf(ph);
  static float sLat = 0, sLon = 0;                 // 显示平滑
  sLat += (yLat - sLat) * 0.35f;
  sLon += (xLon - sLon) * 0.35f;
  float mag = sqrtf(sLat * sLat + sLon * sLon);
  if (mag > imuPeakG) imuPeakG = mag;
  // ---- 刻度：内圈 1.0g（r118）外圈 1.5g（r176），四个方向留缺口 ----
  for (int k = 0; k < 2; k++)
    for (int s = 0; s < 4; s++) {
      const float a0 = 14 + s * 90, a1 = 76 + s * 90;
      imuArcSeg(CX, cyG, (k ? 176 : 118), a0, a1, C_GBLUE);
      imuArcSeg(CX, cyG, (k ? 176 : 118) - 1, a0, a1, C_GBLUE);   // 2 像素粗
    }
  for (int s = 0; s < 4; s++) {                    // 0.5g 刻度短线（轴向）
    const float a = s * 90 * 0.017453293f;
    gfx->drawLine((int)(CX + cosf(a) * 52), (int)(cyG + sinf(a) * 52),
                  (int)(CX + cosf(a) * 64), (int)(cyG + sinf(a) * 64), C_GBLUE);
  }
  // ---- 外圈光晕：厚度随 G 增长（静止 4px → 满表 10px），亮度同步增强，外缘亮内缘暗 ----
  const int glowT = constrain(4 + (int)(mag * 4.0f), 4, 10);
  const int glowC = constrain(60 + (int)(mag * 150), 60, 255);
  for (int i = 0; i < glowT; i++)
    gfx->drawCircle(CX, cyG, 205 - i, blend565(C_BLACK, C_CHART, constrain(glowC - i * 14, 30, 255)));
  // ---- 方向标注（内外圈之间的缺口带）+ 量程小字 ----
  printCentered("FRONT", CX, cyG - 162, 2, C_GBLUE);
  printCentered("REAR",  CX, cyG + 148, 2, C_GBLUE);
  printCentered("LEFT",  CX - 150, cyG - 8, 2, C_GBLUE);
  printCentered("RIGHT", CX + 150, cyG - 8, 2, C_GBLUE);
  printCentered("0.5", CX, cyG - 70, 1, 0x630C);
  printCentered("1.0", CX, cyG - 128, 1, 0x630C);
  // ---- 中央合 G 数字（黄点画在数字上面，所以数字先画）----
  const uint16_t vc = mag < 0.5f ? C_WHITE : (mag < 1.0f ? C_ORANGE : C_RED);
  char b[24];
  snprintf(b, sizeof(b), "%.2fG", mag);
  printCentered(b, CX, cyG - 24, 5, vc);
  snprintf(b, sizeof(b), "LEAN %+5.1f deg", imuWrap180(imuRoll - imuBiasRoll));
  printCentered(b, CX, cyG + 34, 2, C_GREY);
  // ---- G 矢量圆点（限幅 1.5g 内；最后画，浮在数字上面。sLat = 侧向，sLon = 纵向）----
  float dl = sqrtf(sLat * sLat + sLon * sLon);
  if (dl > 1.5f) { sLat *= 1.5f / dl; sLon *= 1.5f / dl; }
  aaDisc(CX + sLat * 118.0f, cyG - sLon * 118.0f, 17, C_CHART);
  drawPageDots(); needFlush=true;
}

void drawPageDots() {
  if (page == PAGE_HOME && inNav) return;
  const int pageOrder[] = {PAGE_MUSIC, PAGE_HOME, PAGE_IMU, PAGE_WEATHER};
  for (int i = 0; i < 4; i++) {
    aaDisc(CX + (i - 1.5f) * 20, 444, 4, page == pageOrder[i] ? C_WHITE : 0x4208);
  }
}

void drawWaiting(int st) {
  waitSt = st;
  if (sliding) return;                                                 // v5.5 正在滑动：滑完再画
  if (page == PAGE_MUSIC) { onWaitScreen = false; musicDirty = true; return; }
  if (page == PAGE_IMU || page == PAGE_WEATHER) { onWaitScreen = false; wxDirty = (page == PAGE_WEATHER); return; }
  gfx->fillScreen(C_BLACK);
  drawBackground();
  onWaitScreen = true; waitSt = st;          // v3.6 首页猫猫 → v4.6 像素骑手
  riderPrevF = -1; riderPrevZ = -2;
  catForce = true;
  animateidle();
  printCentered("MotoNav", CX, 240, 6, C_CYAN);
  if (st == WAIT_READY) {
    printCentered("ready", CX, 305, 3, C_GREEN);
  } else if (st == WAIT_NOLINK) {
    printCentered("no link", CX, 305, 3, C_GREY);
  } else {
    drawRedRing();
    printCentered("LINK LOST", CX, 305, 4, C_RED);
  }
  printCentered(bleName, CX, 342, 2, C_GREY);                          // v7.0 这块圆屏的蓝牙名
  printCentered(String("fw ") + FW_VERSION + "  " + String(qspiMHz) + "MHz", CX, 380, 2, C_GREY);
  drawStatus();
  drawPageDots();
  needFlush = true;
}

// ================= v6.5 配对画面 =================
bool pairOn = false;
unsigned long pairShowAt = 0;

void drawPairScreen() {
  onWaitScreen = false;
  frameIsMap = false;
  gfx->fillScreen(C_BLACK);
  printCentered("PAIRING", CX, 120, 3, C_CYAN);
  if (pairLocked) {
    printCentered("LOCKED", CX, 205, 5, C_RED);
    printCentered("restart this screen,", CX, 290, 2, C_GREY);
    printCentered("then pair within 2 min", CX, 315, 2, C_GREY);
  } else {
    char b[8];
    snprintf(b, sizeof(b), "%06lu", (unsigned long)pairCode);
    printCentered(b, CX, 200, 7, C_WHITE);
    printCentered("enter this code", CX, 295, 2, C_GREY);
    printCentered("on your phone", CX, 320, 2, C_GREY);
  }
  printCentered(bleName, CX, 372, 2, C_CYAN);                            // v7.0 确认连的是这块

  needFlush = true;
}

// ================= v7.1 到达特效 =================
// 到达目的地（"A,0"）：约 7 秒的庆祝动画——粉色放射光芒慢慢转、冲击波圆环、几轮彩色烟花、
// 中间一面飘动的黑白格终点旗、"ARRIVED" 霓虹字，手机发了行程统计（"#O米/分钟/最高车速"）就逐字打出来。
// 到达途经点（"#J"）：约 3 秒的短版（青色，"WAYPOINT"）。点一下屏幕提前结束，之后回到原来的画面。
#define C_PINK   0xF973   // #FF2E9A 罪恶都市粉
#define FX_DEST_MS 7000
#define FX_WP_MS   3000
#define FX_PN      180
#define CCY 233                // 屏幕中心 y（圆屏 466×466）
bool fxOn = false, fxWp = false;
unsigned long fxAt = 0, fxDur = 0;
bool touchEat = false;                 // 点掉特效的那一下：不当成滑动换页
long tripM = -1; int tripMin = -1, tripMaxKmh = -1; unsigned long tripAt = 0;   // "#O" 行程统计
struct FxP { int16_t x, y, vx, vy; uint16_t t0; uint16_t col; };
FxP fxP[FX_PN];
int fxPN = 0;

static inline float frand(float a, float b) { return a + (b - a) * (esp_random() & 0xFFFF) / 65535.0f; }

void startArriveFx(bool wp) {
  fxOn = true; fxWp = wp;
  fxAt = millis();
  fxDur = wp ? FX_WP_MS : FX_DEST_MS;
  // 烟花：几轮，每轮从一个点炸开
  const uint16_t pal[5] = { C_PINK, C_CYAN, C_YELLOW, 0x07F0, C_WHITE };
  const int burstsD[6] = { 0, 650, 1300, 2200, 3200, 4300 };
  const int burstsW[2] = { 0, 700 };
  int nb = wp ? 2 : 6;
  int per = FX_PN / nb;
  fxPN = 0;
  for (int b = 0; b < nb; b++) {
    float bx = CX + frand(-110, 110), by = (wp ? 200 : 150) + frand(-50, 50);
    uint16_t c1 = wp ? C_CYAN : pal[esp_random() % 5], c2 = wp ? C_WHITE : pal[esp_random() % 5];
    for (int i = 0; i < per && fxPN < FX_PN; i++) {
      float a = frand(0, 2 * PI), sp = frand(70, 230);
      FxP &p = fxP[fxPN++];
      p.x = (int16_t)bx; p.y = (int16_t)by;
      p.vx = (int16_t)(cosf(a) * sp); p.vy = (int16_t)(sinf(a) * sp - 40);
      p.t0 = (uint16_t)((wp ? burstsW[b] : burstsD[b]) + frand(0, 60));
      p.col = (i & 1) ? c1 : c2;
    }
  }
}

void stopArriveFx() { fxOn = false; }

// 一圈 w 像素粗的圆环
void fxRing(float r, int w, uint16_t c) {
  for (int k = 0; k < w; k++) if (r + k > 0) gfx->drawCircle(CX, CCY, (int)(r + k), c);
}

// 飘动的黑白格旗：(px, py) 是旗杆顶，s = 缩放，t = 毫秒
void fxFlag(float px, float py, float s, unsigned long t, uint16_t light) {
  const int COLS = 8, ROWS = 5;
  float cell = 13 * s;
  thickLine(px, py - 6 * s, px, py + 150 * s, 5 * s, 0xC618);            // 旗杆
  aaDisc(px, py - 8 * s, 5 * s, C_YELLOW);                               // 杆头
  for (int c = 0; c < COLS; c++) {
    float k = (c + 1) / (float)COLS;                                     // 越往外飘得越厉害
    float yo = sinf(t * 0.008f - c * 0.75f) * 7 * s * k;
    float x = px + 3 * s + c * cell;
    float y = py + yo;
    gfx->fillRect((int)x, (int)(y - 2), (int)ceilf(cell) + (c == COLS - 1 ? 2 : 0), (int)(ROWS * cell + 4), light);
    for (int r = 0; r < ROWS; r++)
      if ((r + c) & 1) gfx->fillRect((int)x, (int)(y + r * cell), (int)ceilf(cell), (int)ceilf(cell), C_BLACK);
  }
}

// 霓虹字：外面一圈暗色描边当光晕
void fxNeonText(const String &s, int y, int size, uint16_t col, uint16_t glow) {
  for (int dx = -2; dx <= 2; dx += 2)
    for (int dy = -2; dy <= 2; dy += 2)
      if (dx || dy) printCentered(s, CX + dx, y + dy, size, glow);
  printCentered(s, CX, y, size, col);
}

void drawArriveFx(unsigned long t) {
  onWaitScreen = false;
  frameIsMap = false;
  gfx->fillScreen(C_BLACK);
  uint16_t mc = fxWp ? C_CYAN : C_PINK;
  float fade = (t + 600 > fxDur) ? (fxDur - t) / 600.0f : 1.0f;          // 最后 0.6 秒淡出
  if (fade < 0) fade = 0;
  int fadeA = 255 - (int)(fade * 255);                                   // 和黑色混合的比例

  // 1. 放射光芒（慢慢转）
  {
    float rot = t * 0.00025f;
    uint16_t rc = blend565(blend565(mc, C_BLACK, 205), C_BLACK, fadeA);
    for (int i = 0; i < 16; i++) {
      float a0 = rot + i * (2 * PI / 16), a1 = a0 + 0.11f;
      gfx->fillTriangle(CX, CCY, CX + cosf(a0) * 260, CCY + sinf(a0) * 260,
                        CX + cosf(a1) * 260, CCY + sinf(a1) * 260, rc);
    }
  }
  // 2. 冲击波圆环
  for (int k = 0; k < 3; k++) {
    float ph = ((long)t - k * 260) / 900.0f;
    if (ph < 0 || ph > 1) continue;
    uint16_t c = blend565((k & 1) ? C_CYAN : mc, C_BLACK, (int)(ph * 255));
    fxRing(30 + ph * 210, 4, c);
  }
  // 3. 外圈霓虹跑马灯
  {
    float base = t * 0.0016f;
    for (int i = 0; i < 24; i++) {
      float a0 = base + i * (2 * PI / 24), a1 = a0 + 0.14f;
      uint16_t c = blend565((i & 1) ? C_CYAN : mc, C_BLACK, fadeA);
      thickLine(CX + cosf(a0) * 222, CCY + sinf(a0) * 222, CX + cosf(a1) * 222, CCY + sinf(a1) * 222, 5, c);
    }
  }
  // 4. 旗子：弹出来（先放大一点再回到正常大小）
  {
    float pt = t / 450.0f;
    float s = pt >= 1 ? 1.0f : (pt < 0.7f ? pt / 0.7f * 1.15f : 1.15f - (pt - 0.7f) / 0.3f * 0.15f);
    if (fxWp) s *= 0.75f;
    if (s > 0.05f) {
      float fw = (3 + 8 * 13) * s;
      fxFlag(CX - fw / 2, (fxWp ? 120 : 92) + (1 - s) * 60, s, t, blend565(C_WHITE, C_BLACK, fadeA));
    }
  }
  // 5. 烟花粒子（带重力，越到后面越暗越小）
  {
    const float LIFE = 1.7f, G = 170;
    for (int i = 0; i < fxPN; i++) {
      float dt = ((long)t - fxP[i].t0) / 1000.0f;
      if (dt < 0 || dt > LIFE) continue;
      float x = fxP[i].x + fxP[i].vx * dt;
      float y = fxP[i].y + fxP[i].vy * dt + 0.5f * G * dt * dt;
      float lf = dt / LIFE;
      int r = lf < 0.4f ? 4 : (lf < 0.75f ? 3 : 2);
      uint16_t c = blend565(fxP[i].col, C_BLACK, (int)(lf * lf * 230));
      gfx->fillRect((int)x - r / 2, (int)y - r / 2, r + 1, r + 1, c);
      if (dt < 0.35f) {                                                  // 刚炸开时带一小段拖尾
        float tx = x - fxP[i].vx * 0.04f, ty = y - (fxP[i].vy + G * dt) * 0.04f;
        gfx->drawLine((int)tx, (int)ty, (int)x, (int)y, blend565(c, C_BLACK, 120));
      }
    }
  }
  // 6. 文字
  if (t > 250) {
    uint16_t tc = (t / 180) % 2 ? C_WHITE : mc;                        // 前 1.5 秒闪烁，之后定住
    if (t > 1500) tc = C_WHITE;
    tc = blend565(tc, C_BLACK, fadeA);
    uint16_t gc = blend565(blend565(mc, C_BLACK, 110), C_BLACK, fadeA);
    if (fxWp) fxNeonText("WAYPOINT", 268, 4, tc, gc);
    else      fxNeonText("ARRIVED", 262, 5, tc, gc);
  }
  if (!fxWp && t > 900) {
    String l1 = "", l2 = "";
    bool haveTrip = tripM >= 0 && millis() - tripAt < 30000;
    if (haveTrip) {
      l1 = (tripM >= 1000 ? String(tripM / 1000.0, 1) + " km" : String(tripM) + " m") + "  " + String(tripMin) + " min";
      if (tripMaxKmh > 0) l2 = "MAX " + String(tripMaxKmh) + " km/h";
    } else {
      l1 = "RIDE SAFE";
    }
    int n = (t - 900) / 45;                                              // 逐字打出来
    String s1 = l1.substring(0, min(n, (int)l1.length()));
    printCentered(s1, CX, 328, 3, blend565(C_CYAN, C_BLACK, fadeA));
    int n2 = n - (int)l1.length() - 4;
    if (l2.length() && n2 > 0) printCentered(l2.substring(0, min(n2, (int)l2.length())), CX, 366, 2, blend565(C_YELLOW, C_BLACK, fadeA));
  }
  needFlush = true;
}

// ================= v5.1 来电显示 =================
// 手机来电时整屏显示：上面 “INCOMING CALL”，中间绿色电话图标带往外扩散的波纹，下面是名字（手机渲染好的
// 文字图片，图片栏 2）和号码；接听或挂断后回到原来的画面（地图 / 大箭头 / 首页）
bool callOn = false;
String callNum = "";
unsigned long callAt = 0, callSeenAt = 0;

// v5.2 触屏接电话：按住任意位置 HOLD_MS 毫秒 → 发 "T,A" 给手机（v6.9 800 → 200）
#define HOLD_MS 200
TouchDrvCST816 touch;
bool touchOK = false;
unsigned long touchHoldAt = 0, touchLastSeen = 0;
float touchHold = 0;                 // 0~1 按住的进度
bool callAnswerSent = false;
unsigned long callAnswerAt = 0;
String pendingProf = "";             // v5.8 分步用时
String pendingEvt = "";              // 待发给手机的触摸事件

// v5.3 触摸芯片一有动静就拉低中断脚；平时只看这个标记，不用一直读 I2C
volatile bool touchIrq = false;
void IRAM_ATTR onTouchIrq() { touchIrq = true; }

void touchInit() {
  touch.setPins(TP_RESET, TP_INT);
  touchOK = touch.begin(Wire, CST816_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  Serial.println(touchOK ? "touch ok" : "touch not found");
  if (touchOK) { touch.disableAutoSleep();attachInterrupt(TP_INT, onTouchIrq, FALLING); }   // v5.3
}

bool touchDown = false;
int16_t touchX0 = 0, touchY0 = 0, touchX1 = 0, touchY1 = 0;
void dragMove(int dx, int dy, unsigned long now);   // v5.5 跟手换页
void touchPointToCanvas(int &x, int &y);
int musicButtonAt(int x, int y);
void musicTap(int x, int y);
void dragEnd();
void dragAbort();

// 读触摸：按下 → 报坐标；来电时按住够久 → 接听；拖动 → 跟手换页（v5.5）
// 手指离开超过 0.15 秒才算松开（防止读数偶尔丢一下）
void touchPoll() {
  static unsigned long lastPoll = 0;
  unsigned long now = millis();
  if (!touchOK) return;
  if (!touchIrq && !touchDown && !callOn) return;
  if (now - lastPoll < 12) return;
  lastPoll = now;
  touchIrq = false;
  if (callAnswerSent && now - callAnswerAt > 4000) callAnswerSent = false;   // 4 秒还在响：允许再按一次
  int16_t x[5] = {0}, y[5] = {0};
  uint8_t n = touch.getPoint(x, y, 1);
  if(n>0) {
    x[0]=PortScale::touch(x[0]); y[0]=PortScale::touch(y[0]);
    int tx = x[0], ty = y[0];
    touchPointToCanvas(tx, ty); x[0] = (int16_t)tx; y[0] = (int16_t)ty;
  }
  if (n > 0) {
    if (!touchDown) {
      touchDown = true;
      touchHoldAt = now;
      touchX0 = touchX1 = x[0]; touchY0 = touchY1 = y[0];
      pendingEvt = "T,P," + String(x[0]) + "," + String(y[0]);
      if (page == PAGE_MUSIC && !callOn && !pairOn) {
        musicPressed = musicButtonAt(x[0], y[0]);
        if (musicPressed >= 0) { musicSparkButton = musicPressed; musicSparkUntil = now + 520; }
        // Serial.printf("MUSIC touch down x=%d y=%d button=%d\n", x[0], y[0], musicPressed);
        musicDirty = true;
      }
      if (fxOn && !callOn) { stopArriveFx(); touchEat = true; }          // v7.1 点一下结束到达特效
    }
    touchX1 = x[0]; touchY1 = y[0];
    touchLastSeen = now;
    if (callOn || pairOn || touchEat || fxOn) dragAbort();
    else dragMove(touchX1 - touchX0, touchY1 - touchY0, now);
  } else if (touchDown && now - touchLastSeen > 80) {
    touchDown = false;
    touchHoldAt = 0;
    if (callOn || pairOn || touchEat || fxOn) dragAbort();
    else {
      if (page == PAGE_MUSIC && abs(touchX1 - touchX0) < 60 && abs(touchY1 - touchY0) < 60) musicTap(touchX0, touchY0);
      if (page == PAGE_WEATHER && abs(touchX1 - touchX0) < 36 && abs(touchY1 - touchY0) < 36) weatherTap();
      if (page == PAGE_IMU && abs(touchX1 - touchX0) < 36 && abs(touchY1 - touchY0) < 36) imuResetReference();
      dragEnd();
    }
    touchEat = false;
    if (musicPressed >= 0) { musicPressed = -1; musicDirty = true; }
  }
  // 来电：按住不动才算（滑动不算）
  bool still = abs(touchX1 - touchX0) < 60 && abs(touchY1 - touchY0) < 60;
  touchHold = (callOn && touchDown && still) ? min(1.0f, (now - touchHoldAt) / (float)HOLD_MS) : 0;
  if (touchHold >= 1 && !callAnswerSent) {
    callAnswerSent = true;
    callAnswerAt = now;
    touchHold = 0;
    pendingEvt = "T,A";
  }
}

// 电话听筒：一段拱形的圆点串当手柄，两头各一个大点当听筒和话筒，整体斜过来（常见的电话图标）
void drawHandset(float cx, float cy, float r, uint16_t c) {
  const float ROT = -35 * PI / 180, cr = cosf(ROT), sr = sinf(ROT);
  auto dot = [&](float x, float y, float rr) {
    aaDisc(cx + x * cr - y * sr, cy + x * sr + y * cr, rr, c);
  };
  for (int a = 25; a <= 155; a += 5) {
    float rad = a * PI / 180;
    dot(cosf(rad) * r, -sinf(rad) * r * 0.55f + r * 0.25f, r * 0.22f);
  }
  for (int k = 0; k < 2; k++) {
    float rad = (k ? 155 : 25) * PI / 180;
    dot(cosf(rad) * r + (k ? -1 : 1) * r * 0.04f, -sinf(rad) * r * 0.55f + r * 0.25f + r * 0.21f, r * 0.32f);
  }
}

void drawCallScreen() {
  onWaitScreen = false;
  frameIsMap = false;
  gfx->fillScreen(C_BLACK);
  unsigned long t = millis() - callAt;
  const float icy = 168;
  // 往外扩散的两圈波纹
  for (int k = 0; k < 2; k++) {
    float ph = fmodf(t / 1400.0f + k * 0.5f, 1.0f);
    float r = 52 + ph * 110;
    uint16_t c = blend565(C_GREEN, C_BLACK, (int)(ph * 255));
    for (int w = 0; w < 3; w++) gfx->drawCircle(CX, (int)icy, (int)r + w, c);
  }
  aaDisc(CX, icy, 50, 0x2DC6);
  aaDisc(CX, icy, 46, C_GREEN);
  // 听筒轻轻左右晃
  float wob = ((t / 120) % 4 < 2) ? -2.5f : 2.5f;
  drawHandset(CX + wob, icy, 26, C_WHITE);
  printCentered("INCOMING CALL", CX, 58, 2, C_GREEN);
  bool hasName = imgs[2].show && imgs[2].showW > 0;
  if (hasName) {
    drawTextImg(2, 252, 0xFF);
    if (callNum.length()) printCentered(callNum, CX, 318, 2, C_GREY);
  } else if (callNum.length()) {
    printCentered(callNum, CX, 262, callNum.length() > 12 ? 3 : 4, C_WHITE);
  } else {
    printCentered("UNKNOWN", CX, 262, 4, C_WHITE);
  }
  // v5.2 触屏接听提示；按住时绿圈外面一圈白点从正上方顺时针走满
  if (touchOK) {
    if (callAnswerSent) printCentered("ANSWERING...", CX, 392, 2, C_GREEN);
    else printCentered("TAP & HOLD", CX, 392, 2, C_GREY);      // v6.9
    if (touchHold > 0 && !callAnswerSent) {
      int steps = (int)(touchHold * 72);
      for (int k = 0; k <= steps; k++) {
        float a = k * 5 * PI / 180;
        aaDisc(CX + sinf(a) * 64, icy - cosf(a) * 64, 4, C_WHITE);
      }
    }
  }
  needFlush = true;
}

// ================= v5.3 天气页 =================
// 图标编号（手机按高德的天气文字归类）：0 晴 1 多云 2 阴 3 雨 4 大雨 5 雷雨 6 雪 7 雾霾 8 雨夹雪 9 沙尘 10 夜晴 11 夜多云
bool wxHave = false;
int wxIcon = 0, wxTemp = 0;
String wxTime = "";
int wxN = 0;
bool wxHourly = false;                 // v5.6 下面一排是逐小时（H）还是按天（D）
int wxWk[6], wxFi[6], wxFh[6], wxFl[6];   // 按天：星期/图标/最高/最低；逐小时：几点/图标/温度/降雨%
unsigned long wxAt = 0, wxAskAt = 0;

#define WX_SUN   0xFE60
#define WX_CLOUD 0xDEFB
#define WX_DARK  0x8C71
#define WX_RAIN  0x4D7F
#define WX_DUST  0xC4A8

// 粗线：沿线段画一串抗锯齿圆点
void wxLine(float x0, float y0, float x1, float y1, float w, uint16_t c) {
  float len = sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
  int n = max(1, (int)(len / 1.5f));
  for (int i = 0; i <= n; i++) aaDisc(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, w / 2, c);
}

void wxSun(float cx, float cy, float r) {
  for (int k = 0; k < 8; k++) {
    float a = k * PI / 4;
    wxLine(cx + cosf(a) * r * 1.35f, cy + sinf(a) * r * 1.35f, cx + cosf(a) * r * 1.75f, cy + sinf(a) * r * 1.75f, max(2.0f, r * 0.18f), WX_SUN);
  }
  aaDisc(cx, cy, r, WX_SUN);
}

void wxMoon(float cx, float cy, float r) {
  aaDisc(cx, cy, r, WX_SUN);
  aaDisc(cx + r * 0.45f, cy - r * 0.3f, r * 0.82f, C_BLACK);
}

// 云：三个圆 + 圆角底座
void wxCloud(float cx, float cy, float s, uint16_t c) {
  aaDisc(cx - s * 0.5f, cy + s * 0.08f, s * 0.42f, c);
  aaDisc(cx + s * 0.05f, cy - s * 0.2f, s * 0.58f, c);
  aaDisc(cx + s * 0.55f, cy + s * 0.12f, s * 0.38f, c);
  int r = (int)(s * 0.25f);
  gfx->fillRoundRect((int)(cx - s * 0.92f), (int)(cy + s * 0.02f), (int)(s * 1.84f), (int)(s * 0.5f), r, c);
}

// 画一个天气图标；s ≈ 图标半宽
void drawWxIcon(int id, float cx, float cy, float s) {
  switch (id) {
    case 0: wxSun(cx, cy, s * 0.55f); break;
    case 10: wxMoon(cx, cy, s * 0.7f); break;
    case 1: case 11:
      if (id == 1) wxSun(cx + s * 0.3f, cy - s * 0.3f, s * 0.42f); else wxMoon(cx + s * 0.3f, cy - s * 0.35f, s * 0.5f);
      wxCloud(cx - s * 0.12f, cy + s * 0.2f, s * 0.78f, WX_CLOUD);
      break;
    case 2: wxCloud(cx, cy, s * 0.95f, WX_CLOUD); break;
    case 3: case 4: case 8: {
      int drops = id == 4 ? 4 : 3;
      for (int k = 0; k < drops; k++) {
        float x = cx + (k - (drops - 1) / 2.0f) * s * 0.42f;
        if (id == 8 && k % 2) aaDisc(x - s * 0.08f, cy + s * 0.72f, s * 0.11f, C_WHITE);
        else wxLine(x, cy + s * 0.5f, x - s * 0.16f, cy + s * 0.92f, max(2.0f, s * 0.1f), WX_RAIN);
      }
      wxCloud(cx, cy - s * 0.2f, s * 0.9f, id == 4 ? WX_DARK : WX_CLOUD);
      break;
    }
    case 5: {
      wxCloud(cx, cy - s * 0.25f, s * 0.9f, WX_DARK);
      float x = cx, y = cy + s * 0.2f;           // 闪电
      gfx->fillTriangle((int)(x + s * 0.12f), (int)y, (int)(x - s * 0.22f), (int)(y + s * 0.42f), (int)(x + s * 0.02f), (int)(y + s * 0.42f), WX_SUN);
      gfx->fillTriangle((int)(x + s * 0.1f), (int)(y + s * 0.3f), (int)(x - s * 0.12f), (int)(y + s * 0.85f), (int)(x + s * 0.2f), (int)(y + s * 0.3f), WX_SUN);
      break;
    }
    case 6:
      wxCloud(cx, cy - s * 0.2f, s * 0.9f, WX_CLOUD);
      for (int k = 0; k < 3; k++) aaDisc(cx + (k - 1) * s * 0.42f, cy + s * 0.68f + (k % 2) * s * 0.18f, s * 0.11f, C_WHITE);
      break;
    case 7: case 9:
      for (int k = 0; k < 4; k++) {
        float y = cy - s * 0.45f + k * s * 0.3f, off = (k % 2) * s * 0.18f;
        wxLine(cx - s * 0.75f + off, y, cx + s * 0.6f + off, y, max(3.0f, s * 0.16f), id == 9 ? WX_DUST : WX_DARK);
      }
      break;
    default: wxCloud(cx, cy, s * 0.95f, WX_CLOUD);
  }
}

// 温度大字 + 度数小圆圈
void drawTemp(int t, int cx, int y, int size, uint16_t c) {
  String v = String(t);
  int w = v.length() * 6 * size;
  printCentered(v, cx - size * 2, y, size, c);
  float ox = cx - size * 2 + w / 2.0f + size * 1.6f, oy = y + size * 2.5f;
  for (int a = 0; a < 360; a += 12) aaDisc(ox + cosf(a * PI / 180) * size * 1.3f, oy + sinf(a * PI / 180) * size * 1.3f, max(1.5f, size * 0.32f), c);
}

const char *WX_WEEK[] = {"", "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"};

// QWeather extension: no strings in BLE packets; Chinese summaries use image slots 7-11.
int wxDetail = 0, wxAQI = -1, wxAlerts = -1, wxLevel = 0, wxRainN = 0;
int wxRain[24];
unsigned long wxAlertTag = 0;
bool wxExtra = false;
void weatherTap() { if (wxHave && wxExtra) { wxDetail = (wxDetail + 1) % 3; wxDirty = true; pageAt = millis(); } }
void parseWeatherExtra(const String &m) {
  char buffer[240]; m.toCharArray(buffer, sizeof(buffer));
  char *save = nullptr; strtok_r(buffer, ",", &save);
  int values[29], n = 0; char *token;
  while ((token = strtok_r(nullptr, ",", &save)) && n < 29) values[n++] = atoi(token);
  if (n < 5 || values[3] < 0 || values[3] > 24 || n < 5 + values[3]) return;
  wxAQI = values[0]; wxAlerts = values[1]; wxLevel = values[2]; wxRainN = values[3];
  unsigned long tag = (unsigned long) values[4];
  if (wxAlerts > 0 && tag != wxAlertTag && page == PAGE_WEATHER) wxDetail = 2;
  wxAlertTag = tag;
  for (int i = 0; i < wxRainN; ++i) wxRain[i] = values[5+i];
  wxExtra = true; wxDirty = true;
}
bool drawWeatherExtra() {
  if (!wxExtra || wxDetail == 0) return false;
  gfx->fillScreen(C_BLACK);
  if (wxDetail == 1) {
    printCentered("RAIN / AIR", CX, 55, 3, C_CYAN);
    drawTextImg(7, 105, 0xFF);
    int peak = 0;
    for (int i=0; i<wxRainN; ++i) peak = max(peak, wxRain[i]);
    if (wxRainN > 0) {
      for (int i=0; i<wxRainN; ++i) {
        int x = 95 + i * 276 / wxRainN;
        int height = wxRain[i] < 0 ? 2 : wxRain[i] * 72 / max(1, peak);
        gfx->fillRect(x, 225-height, max(2, 260/wxRainN), max(2,height), wxRain[i] < 0 ? C_GREY : C_CYAN);
      }
      printCentered("NEXT 2H  MAX " + String(peak / 100.0f, 2) + " mm/5m", CX, 240, 2, C_GREY);
    } else printCentered("RAIN DATA UNAVAILABLE", CX, 190, 2, C_GREY);
    uint16_t airColor = wxAQI < 0 ? C_GREY : C_CYAN;
    drawTextImg(8, 285, 0xFF);
    printCentered(wxAQI < 0 ? "AQI --" : "AQI " + String(wxAQI), CX, 335, 3, airColor);
  } else {
    printCentered(wxAlerts > 0 ? "WEATHER ALERT" : "ALERT STATUS", CX, 65, 3, wxLevel >= 3 ? C_ORANGE : C_CYAN);
    drawTextImg(9, 125, 0xFF);
    drawTextImg(10, 200, 0xDC);
    drawTextImg(11, 270, 0xDC);
    if (wxAlerts > 1) printCentered(String(wxAlerts) + " ALERTS - DETAILS ON PHONE", CX, 330, 1, C_GREY);
    printCentered("MAY BE DELAYED - CHECK OFFICIAL", CX, 365, 1, C_GREY);
  }
  printCentered("QWEATHER - TAP TO SWITCH", CX, 400, 1, C_GREY);
  drawPageDots(); needFlush = true;
  return true;
}

void drawWeather() {
  if (wxHave && drawWeatherExtra()) return;
  onWaitScreen = false;
  frameIsMap = false;
  gfx->fillScreen(C_BLACK);
  if (!wxHave) {
    drawWxIcon(1, CX, 190, 60);
    printCentered(bleConnected ? "LOADING..." : "NO PHONE", CX, 290, 3, C_GREY);
    printCentered("WEATHER", CX, 70, 2, C_GREY);
    drawPageDots();
    needFlush = true;
    return;
  }
  // 地名 · 天气
  drawTextImg(3, 56, 0xFF);
  // 大图标 + 温度
  drawWxIcon(wxIcon, CX - 78, 165, 58);
  drawTemp(wxTemp, CX + 82, 135, 8, C_WHITE);
  // 风 · 湿度
  drawTextImg(4, 232, 0xC0);
  if (wxHourly) {
    if (wxN <= 0) {
      printCentered("HOURLY UNAVAILABLE", CX, 322, 2, C_GREY);
      printCentered("CHECK PHONE WEATHER DETAILS", CX, 354, 1, C_GREY);
    }
    // v5.6 接下来几个小时：几点 / 图标 / 温度 / 降雨概率（≥10% 才显示）
    for (int i = 0; i < wxN && i < 6; i++) {
      int x = CX + (int)((i - (wxN - 1) / 2.0f) * 76);
      char hh[8]; snprintf(hh, sizeof(hh), "%02d:00", wxWk[i]);
      printCentered(hh, x, 280, 2, C_GREY);
      drawWxIcon(wxFi[i], x, 322, 19);
      String t = String(wxFh[i]);
      printCentered(t, x - 4, 348, 2, C_WHITE);
      float ox = x - 4 + t.length() * 6 + 3, oy = 351;      // 小度数圈
      for (int a = 0; a < 360; a += 30) aaDisc(ox + cosf(a * PI / 180) * 2.6f, oy + sinf(a * PI / 180) * 2.6f, 0.9f, C_WHITE);
      if (wxFl[i] >= 10) printCentered(String(wxFl[i]) + "%", x, 372, 2, WX_RAIN);
    }
  } else {
    // 按天（手机查不到逐小时时）
    for (int i = 0; i < wxN && i < 4; i++) {
      int x = CX + (int)((i - (wxN - 1) / 2.0f) * 92);
      String lb = i == 0 ? String("TODAY") : String(WX_WEEK[wxWk[i] >= 1 && wxWk[i] <= 7 ? wxWk[i] : 0]);
      printCentered(lb, x, 282, 2, i == 0 ? C_CYAN : C_GREY);
      drawWxIcon(wxFi[i], x, 328, 20);
      printCentered(String(wxFh[i]) + "/" + String(wxFl[i]), x, 360, 2, C_WHITE);
    }
  }
  if (wxTime.length()) printCentered("QWEATHER " + wxTime + (wxAlerts > 0 ? " ! TAP" : " TAP"), CX, 406, 1, 0x5AEB);
  drawPageDots();
  needFlush = true;
}

// "#W,图标,温度,更新时间,H/D,个数,{…4 个数…}..."
void parseWeather(const String &m) {
  char buf[220];
  m.toCharArray(buf, sizeof(buf));
  char *save = nullptr;
  char *tok = strtok_r(buf, ",", &save);           // "#W"
  String f[32]; int n = 0;
  while ((tok = strtok_r(nullptr, ",", &save)) && n < 32) f[n++] = tok;
  if (n < 5) return;
  wxIcon = f[0].toInt(); wxTemp = f[1].toInt(); wxTime = f[2];
  wxHourly = f[3] == "H";
  wxN = min(wxHourly ? 6 : 4, (int)f[4].toInt());
  for (int i = 0; i < wxN; i++) {
    int b = 5 + i * 4;
    if (b + 3 >= n) { wxN = i; break; }
    wxWk[i] = f[b].toInt(); wxFi[i] = f[b + 1].toInt(); wxFh[i] = f[b + 2].toInt(); wxFl[i] = f[b + 3].toInt();
  }
  wxHave = true;
  wxAt = millis();
  wxDirty = true;
}

// ---- v5.5 跟手换页 ----
// 触摸坐标和画面方向相反时改成 1（拖动时画面往手指反方向走就是反了）
#define DRAG_START  16        // 手指移动超过这么多像素才开始拖
uint16_t *slideOld = nullptr, *slideNew = nullptr;
int16_t chordHalf[LCD_HEIGHT];   // 每一行在圆里的半宽（圆外不用算）
bool chordReady = false;
int lastSlideSx = -1, lastSlideSy = 0;      // 上次进天气页的方向（自动回去时反着滑）
void drawNavFrame();

// v7.8 音乐页：歌名/歌手图片栏 5/6；#Z,有会话,播放中,按钮位掩码。
void drawMusic() {
  gfx->fillScreen(C_BLACK);
  onWaitScreen = false;
  frameIsMap = false;
  printCentered("MUSIC", CX, 65, 4, C_CYAN);
  bool linked = bleConnected && bleAuthed;
  if (hidMusicMode && linked && !appleMedia.available && !musicHave) {
    printCentered("PHONE MEDIA KEYS", CX, 158, 2, C_WHITE);
    printCentered("OPEN MUSIC APP ON PHONE", CX, 210, 2, C_GREY);
  } else if (linked) {
    drawTextImg(5, 158, 0xFF);
    drawTextImg(6, 210, 0xBB);
  } else printCentered("PHONE OFFLINE", CX, 170, 3, C_GREY);
  bool showLyrics = linked && hidMusicMode && musicLyrics && appleMedia.available && imgs[12].showW > 0;
  if (showLyrics) drawTextImg(12, 242, 0xDD);
  const char *feedback[] = {"", "SENT", "PHONE RECEIVED", "DONE", "NO RESPONSE", "NO PLAYER", "HID SENT", "HID NOT READY", "HID SEND FAILED", "AMS SENT", "AMS SEND FAILED", "AMS QUEUED"};
  String state = linked && musicHave ? (musicPlaying ? "PLAYING" : "PAUSED") : "NO PLAYER";
  if (hidMusicMode) {
    state = !hidMediaReady() && !musicHave ? (appleMedia.controlMask() ? "AMS MEDIA READY" : "PAIR HID IN PHONE SETTINGS") : !appleMedia.available ? (musicHave ? (musicPlaying ? "PLAYING" : "PAUSED") : "HID READY / AMS UNAVAILABLE")
      : !appleMedia.playback.known ? "STATE UNKNOWN" : appleMedia.playback.state == 1 ? "PLAYING" : appleMedia.playback.state == 0 ? "PAUSED" : "SEEKING";
  }
  if (musicAck > 0 && musicAck <= 11) state = feedback[musicAck];
  printCentered(state, CX, showLyrics ? 272 : 257, 2, musicAck == 4 ? C_ORANGE : C_GREY);
  for (int i = 0; i < 3; i++) {
    int x = CX + (i - 1) * 100, y = 335;
    bool enabled = hidMusicMode ? (hidMediaReady() || appleMedia.controlReady(i)) : linked && musicHave;
    uint16_t c = enabled ? C_WHITE : C_GREY;
    aaDisc(x, y, 43, enabled && musicPressed == i ? 0x0470 : enabled ? 0x19A7 : 0x1082);
    if (i == 1) {
      if (hidMusicMode && !appleMedia.playback.known) {
        gfx->fillTriangle(x - 19, y - 15, x - 19, y + 15, x + 1, y, c);
        gfx->fillRect(x + 7, y - 15, 5, 30, c); gfx->fillRect(x + 16, y - 15, 5, 30, c);
      } else if (musicPlaying) {
        gfx->fillRect(x - 13, y - 15, 8, 30, c);
        gfx->fillRect(x + 5, y - 15, 8, 30, c);
      } else gfx->fillTriangle(x - 10, y - 17, x - 10, y + 17, x + 17, y, c);
    } else {
      int sign = i == 0 ? -1 : 1;
      gfx->fillTriangle(x - sign * 12, y - 15, x - sign * 12, y + 15, x + sign * 10, y, c);
      gfx->fillRect(x + sign * 15 - 3, y - 15, 6, 30, c);
    }
  }
  // Short two-spark confirmation, independent of the button press state.
  if (musicSparkButton >= 0 && (long)(musicSparkUntil - millis()) > 0) {
    int sx = CX + (musicSparkButton - 1) * 100, sy = 335;
    float phase = 1.0f - (musicSparkUntil - millis()) / 420.0f;
    int d = 43 + (int)(phase * 15);
    gfx->drawLine(sx - d, sy - d / 2, sx - d - 10, sy - d / 2 - 12, C_YELLOW);
    gfx->drawLine(sx + d, sy + d / 2, sx + d + 10, sy + d / 2 + 12, C_YELLOW);
  }
  if (hidMusicMode && appleMedia.available) {
    double duration = AppleMediaData::duration(appleMedia.values[3].text.c_str());
    if (duration > 0 && appleMedia.playback.known) {
      double elapsed = appleMedia.playback.elapsed + appleMedia.playback.rate * (millis() - appleMedia.playbackAt) / 1000.0;
      int sec = (int)constrain(elapsed, 0.0, duration), total = (int)duration;
      char time[32]; snprintf(time, sizeof(time), "%d:%02d / %d:%02d", sec/60, sec%60, total/60, total%60);
      printCentered(time, CX, 392, 2, C_GREY);
    }
  } else printCentered("SWIPE LEFT TO RETURN", CX, 402, 2, C_GREY);
  drawPageDots();
  needFlush = true;
}

// 拖动状态
bool dragging = false;
int dragTarget = 0, dragSign = 1;
bool dragHoriz = true;
float dragP = 0, dragV = 0;                 // 新页面已经进来多少像素；速度（像素/毫秒）
unsigned long dragT = 0;

uint16_t *shownFrame() {
  if (fbBuf[1] && flushSrc) return flushSrc;
  return gfx->getFramebuffer();
}

static inline uint16_t dim565(uint16_t c, uint32_t k) {   // k: 0~32，32 = 原样
  uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
  return (uint16_t)(((r * k >> 5) << 11) | ((g * k >> 5) << 5) | (b * k >> 5));
}

// 画一个页面到画布（不改当前页）
void renderPage(int p) {
  int keep = page;
  bool ks = sliding;
  page = p; sliding = false;
  if (p == PAGE_MUSIC) drawMusic();
  else if (p == PAGE_IMU) drawImuPage();
  else if (p == PAGE_WEATHER) drawWeather();
  else if (inNav) drawNavFrame();
  else drawWaiting(bleConnected ? WAIT_READY : WAIT_NOLINK);
  page = keep; sliding = ks;
}

// 准备滑动：存下屏幕上的旧画面，在画布上画好目标页面存起来
bool slidePrepare(int target) {
  const size_t BYTES = (size_t)LCD_WIDTH * LCD_HEIGHT * 2;
  if (!slideOld) slideOld = (uint16_t *)heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
  if (!slideNew) slideNew = (uint16_t *)heap_caps_malloc(BYTES, MALLOC_CAP_SPIRAM);
  if (!slideOld || !slideNew) return false;
  if (!chordReady) {
    for (int y = 0; y < LCD_HEIGHT; y++) {
      float d = y + 0.5f - LCD_HEIGHT / 2.0f, r = LCD_WIDTH / 2.0f + 1;
      chordHalf[y] = d * d < r * r ? (int16_t)ceilf(sqrtf(r * r - d * d)) : 0;
    }
    chordReady = true;
  }
  if (fbBuf[1]) { flushWait(); xSemaphoreGive(flushDone); }   // 等正在推的那帧推完，旧画面才完整
  memcpy(slideOld, shownFrame(), BYTES);
  renderPage(target);
  memcpy(slideNew, gfx->getFramebuffer(), BYTES);
  sliding = true;
  needFlush = false; frameIsMap = false;
  return true;
}

// 合成一帧：新页面已经进来 p 像素（盖在上面），旧页面往同方向退 30%、变暗，交界处一道阴影
void slideCompose(int p) {
  const int W = LCD_WIDTH, H = LCD_HEIGHT;
  if (p < 0) p = 0;
  if (p > (dragHoriz ? W : H)) p = dragHoriz ? W : H;
  uint16_t *fb = gfx->getFramebuffer();
  int full = dragHoriz ? W : H;
  int q = p * 3 / 10;                                   // 旧页面退后的距离
  uint32_t k0 = 32 - (uint32_t)(p * 13 / full);         // 旧页面整体亮度：100% → 约 60%
  const int SH = 22;                                    // 阴影宽度
  if (dragHoriz) {
    static uint8_t colK[LCD_WIDTH];
    for (int x = 0; x < W; x++) {
      int dist = dragSign > 0 ? x - p : (W - p - 1) - x;   // 离交界多远（旧页面这一侧）
      uint32_t k = k0;
      if (p > 0 && dist < SH) k = k * (14 + dist * 18 / SH) / 32;
      colK[x] = (uint8_t)k;
    }
    for (int y = 0; y < H; y++) {
      int h = chordHalf[y];
      if (!h) continue;
      int xa = max(0, W / 2 - h), xb = min(W, W / 2 + h);
      uint16_t *row = fb + (size_t)y * W;
      const uint16_t *o = slideOld + (size_t)y * W, *n = slideNew + (size_t)y * W;
      if (dragSign > 0) {                               // 新页面从左边进来
        int a = xa, b = min(p, xb);
        if (b > a) memcpy(row + a, n + (W - p) + a, (b - a) * 2);
        for (int x = max(p, xa); x < xb; x++) row[x] = colK[x] >= 32 ? o[x - q] : dim565(o[x - q], colK[x]);
      } else {                                          // 新页面从右边进来
        int a = max(W - p, xa), b = xb;
        if (b > a) memcpy(row + a, n + (a - (W - p)), (b - a) * 2);
        for (int x = xa; x < min(W - p, xb); x++) row[x] = colK[x] >= 32 ? o[x + q] : dim565(o[x + q], colK[x]);
      }
      // 新页面前沿一道细亮边，看得出是一张“卡片”盖过来
      if (p > 1 && p < W - 1) {
        int ex = dragSign > 0 ? p - 1 : W - p;
        if (ex >= xa && ex < xb) row[ex] = 0x4A69;
        int ex2 = ex + (dragSign > 0 ? -1 : 1);
        if (ex2 >= xa && ex2 < xb) row[ex2] = 0x2945;
      }
    }
  } else {
    for (int y = 0; y < H; y++) {
      int h = chordHalf[y];
      if (!h) continue;
      int xa = max(0, W / 2 - h), xb = min(W, W / 2 + h);
      uint16_t *row = fb + (size_t)y * W;
      bool isNew = dragSign > 0 ? y < p : y >= H - p;
      if (isNew) {
        int sy = dragSign > 0 ? y + (H - p) : y - (H - p);
        memcpy(row + xa, slideNew + (size_t)sy * W + xa, (xb - xa) * 2);
        int edge = dragSign > 0 ? p - 1 : H - p;                // 前沿细亮边
        if (p > 1 && p < H - 1 && (y == edge || y == edge + (dragSign > 0 ? -1 : 1))) {
          uint16_t ec = y == edge ? 0x4A69 : 0x2945;
          for (int x = xa; x < xb; x++) row[x] = ec;
        }
      } else {
        int sy = dragSign > 0 ? y - q : y + q;
        int dist = dragSign > 0 ? y - p : (H - p - 1) - y;
        uint32_t k = k0;
        if (p > 0 && dist < SH) k = k * (14 + dist * 18 / SH) / 32;
        const uint16_t *o = slideOld + (size_t)sy * W;
        for (int x = xa; x < xb; x++) row[x] = k >= 32 ? o[x] : dim565(o[x], k);
      }
    }
  }
  flushAsync();                                         // 两块画布轮流：推这一帧的同时画下一帧
  needFlush = false; frameIsMap = false;
}

// 换页完成
void slideCommit() {
  const size_t BYTES = (size_t)LCD_WIDTH * LCD_HEIGHT * 2;
  sliding = false; dragging = false;
  page = dragTarget;
  pageAt = millis();
  memcpy(gfx->getFramebuffer(), slideNew, BYTES);
  needFlush = true; frameIsMap = false;
  if (page == PAGE_MUSIC) {
    onWaitScreen = false; musicDirty = true; pendingEvt = "T,M,Q";
  } else if (page == PAGE_WEATHER) {
    lastSlideSx = dragHoriz ? dragSign : 0; lastSlideSy = dragHoriz ? 0 : dragSign;
    onWaitScreen = false;
    wxDirty = false;
    if (!wxHave || millis() - wxAt > 15UL * 60 * 1000) { pendingEvt = "T,W"; wxAskAt = millis(); }
  } else {
    if (inNav) navDirty = true;
    else { riderPrevF = -1; riderPrevZ = -2; catForce = true; }
  }
}

// 没换成：回到原页面
void slideCancel() {
  sliding = false; dragging = false;
  renderPage(page);
  if (page == PAGE_MUSIC) musicDirty = true;
  else if (page == PAGE_WEATHER) wxDirty = false;
  else if (inNav) navDirty = true;
  else { onWaitScreen = true; catForce = true; }
  needFlush = true;
}

// 从 p0 顺势滑到 p1（先快后慢）；ms = 时长
void slideAnimate(float p0, float p1, int ms) {
  unsigned long t0 = millis();
  for (;;) {
    float t = (millis() - t0) / (float)ms;
    if (t >= 1) break;
    float e = 1 - (1 - t) * (1 - t) * (1 - t);         // ease-out
    slideCompose((int)(p0 + (p1 - p0) * e));
  }
  slideCompose((int)p1);
}

// 触摸向量统一换算：先修正芯片坐标的镜像，再按刷屏的逆旋转映射到画布。
// 位移和相对屏幕中心的点都使用这一个函数，避免旋转后点按与手势方向不一致。
void touchVectorToCanvas(float &dx, float &dy) {
  float ox = dx, oy = dy;
  if (rotFree) { dx = ox * rotCosF + oy * rotSinF; dy = -ox * rotSinF + oy * rotCosF; }
  else if (screenRot == 1) { dx = oy; dy = -ox; }
  else if (screenRot == 2) { dx = -ox; dy = -oy; }
  else if (screenRot == 3) { dx = -oy; dy = ox; }
}

// Convert an absolute CST816S point into the logical canvas coordinate
// system. The 185B panel is physically mirrored on both axes; applying that
// correction here keeps taps, swipes, music buttons and weather controls in
// the same coordinate system. Rotation is then applied exactly like the
// framebuffer output, using the inverse transform for touch input.
void touchPointToCanvas(int &x, int &y) {
  float dx = (float)x - CX, dy = (float)y - CX;
  // CST816S on the 185B reports both axes opposite to the LCD image.
  dx = -dx; dy = -dy;
  float ox = dx, oy = dy;
  if (rotFree) { dx = ox * rotCosF + oy * rotSinF; dy = -ox * rotSinF + oy * rotCosF; }
  else if (screenRot == 1) { dx = oy; dy = -ox; }
  else if (screenRot == 2) { dx = -ox; dy = -oy; }
  else if (screenRot == 3) { dx = -oy; dy = ox; }
  x = constrain((int)lroundf(CX + dx), 0, LCD_WIDTH - 1);
  y = constrain((int)lroundf(CX + dy), 0, LCD_HEIGHT - 1);
}

// 手指在拖：(dx, dy) = 从按下到现在的位移
void dragMove(int dx, int dy, unsigned long now) {
  // 位移向量已在 touchPoll 里随触点一起变换到画布系（touchPointToCanvas），
  // 这里不能再转一次，否则旋转后被转两遍（90°→180°反向，任意角→2θ 偏转）。
  // vx 取反：实机左右滑动方向与页面切换方向的对应关系（0° 行为与旧版一致）。
  float vx = -(float)dx, vy = dy;
  dx = (int)lroundf(vx); dy = (int)lroundf(vy);
  if (!dragging) {
    int threshold = page == PAGE_MUSIC ? 36 : DRAG_START;
    if (abs(dx) < threshold && abs(dy) < threshold) return;
    dragHoriz = abs(dx) >= abs(dy);
    int d = dragHoriz ? dx : dy;
    dragSign = d > 0 ? 1 : -1;
    if (page == PAGE_MUSIC) { if (!dragHoriz || dragSign > 0) return; dragTarget = PAGE_HOME; }
    else if (page == PAGE_WEATHER) { if (!dragHoriz || dragSign < 0) return; dragTarget = PAGE_IMU; }
    else if (dragHoriz && page == PAGE_HOME) dragTarget = dragSign < 0 ? PAGE_IMU : PAGE_MUSIC;
    else if (dragHoriz && page == PAGE_IMU) dragTarget = dragSign < 0 ? PAGE_WEATHER : PAGE_HOME;
    else return;
    if (!slidePrepare(dragTarget)) return;
    dragging = true;
    dragP = 0; dragV = 0; dragT = now;
  }
  float p = (dragHoriz ? dx : dy) * dragSign;
  if (p < 0) p = 0;
  float dt = now - dragT;
  if (dt > 0) dragV = dragV * 0.5f + ((p - dragP) / dt) * 0.5f;
  dragP = p; dragT = now;
  slideCompose((int)p);
}

// 松手：拖过 30% 或者往前甩出去 → 顺势滑完；否则弹回
void dragEnd() {
  if (!dragging) return;
  int full = dragHoriz ? LCD_WIDTH : LCD_HEIGHT;
  bool go = dragP > full * 0.3f || (dragV > 0.35f && dragP > 24);
  if (dragV < -0.35f) go = false;                       // 往回甩：取消
  float target = go ? full : 0;
  float left = fabsf(target - dragP);
  int ms = 140 + (int)(260 * left / full);
  if (go && dragV > 0.5f) ms = max(140, (int)(ms * 0.75f));   // 甩得快就滑得快
  slideAnimate(dragP, target, ms);
  if (go) slideCommit(); else slideCancel();
}

// 来电等情况：直接放弃拖动
void dragAbort() {
  if (!dragging) return;
  sliding = false; dragging = false;
  if (page == PAGE_MUSIC) musicDirty = true;
  else if (page == PAGE_WEATHER) wxDirty = true;
  else if (page == PAGE_IMU) drawImuPage();
  else if (inNav) navDirty = true;
}

// 将触摸点换算到画布坐标，按钮使用较大的点按范围。
int musicButtonAt(int x, int y) {
  // Music page only: the CST816S Y direction is still reversed for the
  // button hit test. Keep the global touch/swipe coordinate system unchanged.
  y = LCD_HEIGHT - 1 - y;
  for (int i = 0; i < 3; i++) {
    int bx = CX + (i - 1) * 100;
    if ((x - bx) * (x - bx) + (y - 335) * (y - 335) <= 72 * 72) {
      // The 185B touch panel's absolute X axis is mirrored relative to the
      // music artwork, while the normalized swipe vector is already correct.
      // Keep this correction local to the music controls: left artwork is
      // Previous and right artwork is Next.
      return 2 - i;
    }
  }
  return -1;
}

void musicTap(int x, int y) {
  if (dragging || page != PAGE_MUSIC || callOn || pairOn || !bleConnected || !bleAuthed) return;
  int button = musicButtonAt(x, y);
  Serial.printf("MUSIC tap x=%d y=%d button=%d hidReady=%d\n", x, y, button, hidMediaReady());
  if (hidMusicMode) {
    if (button < 0) return;
    musicSparkButton = button; musicSparkUntil = millis() + 420; musicDirty = true;
    bool hidReady = hidMediaReady();
    if (!hidReady && !appleMedia.controlReady(button)) {
      // Queue the HID pulse even while the host is finishing enumeration;
      // hidMediaTick will submit it as soon as the report subscription is
      // ready. Do not redirect Android controls through the companion app.
      hidPulses.enqueue(button);
      musicAck = 7; musicAckUntil = millis() + 1200; return;
    }
    if (hidTapAt && millis() - hidTapAt < 120) return;
    hidTapAt = millis();
    if (!hidReady) {
      musicAck = appleMedia.control(button) ? 11 : 10; musicAckUntil = millis() + 2500; musicDirty = true; return;
    }
    if (!hidPulses.enqueue(button)) { musicAck = 8; musicAckUntil = millis() + 2500; musicDirty = true; }
    else Serial.printf("MUSIC HID queued button=%d usage=0x%04X\n", button, HidMediaKeys::usageForButton(button));
    return; // no T,M,P/T/N: do not also control Apple Music through the App
  }
  if (button < 0 || !musicHave) {
    pendingEvt = String("T,M,H,") + x + "," + y + "," + button;
    return;
  }
  pendingEvt = String("T,M,") + (button == 0 ? "P" : button == 1 ? "T" : "N") + "," + x + "," + y + "," + button;
  musicAck = 1; musicAckUntil = millis() + 2500; musicDirty = true;
}

// 自动换页（导航中看天气超时 / 快到路口）：整段动画
void showPageAnimated(int target, int sx, int sy) {
  if (page == target) return;
  dragHoriz = sx != 0 || sy == 0;
  dragSign = dragHoriz ? (sx ? sx : 1) : sy;
  dragTarget = target;
  if (!slidePrepare(target)) { page = target; renderPage(target); needFlush = true; return; }
  dragging = true;
  slideAnimate(0, dragHoriz ? LCD_WIDTH : LCD_HEIGHT, 420);
  slideCommit();
}

// 音乐页在路口前回导航，其余时候保持显示，便于连续切歌。
void musicTick() {
  if (musicSparkButton >= 0 && (long)(millis() - musicSparkUntil) >= 0) {
    musicSparkButton = -1; musicDirty = true;
  }
  if (musicSparkButton >= 0 && page == PAGE_MUSIC) musicDirty = true;
  if (musicAckUntil && (long)(millis() - musicAckUntil) >= 0) {
    if (musicAck == 1) { musicAck = 4; musicAckUntil = millis() + 2500; }
    else { musicAck = 0; musicAckUntil = 0; }
    musicDirty = true;
  }
  if (page != PAGE_MUSIC || callOn || pairOn || sliding) return;
  if (inNav && !cruiseOn && navDir != "S" && navDir.length() && navDist > 0 && navDist < 200 && millis() - pageAt > 3000) {
    showPageAnimated(PAGE_HOME, -1, 0);
    return;
  }
  if (musicDirty) { musicDirty = false; drawMusic(); }
}

// 每圈调用：天气页要不要重画、要不要回导航
void weatherTick() {
  if (page != PAGE_WEATHER || callOn || sliding) return;
  unsigned long now = millis();
  // 导航中：看 15 秒自动回去；快到路口也立刻回去
  if (inNav && (now - pageAt > 15000 || (navDir != "S" && navDir.length() && navDist > 0 && navDist < 200))) {
    showPageAnimated(PAGE_HOME, -lastSlideSx, -lastSlideSy);   // 按进来的方向反着滑回去
    return;
  }
  // 还没数据或数据旧了：30 秒再要一次
  if (bleConnected && (!wxHave || now - wxAt > 15UL * 60 * 1000) && now - wxAskAt > 30000) { pendingEvt = "T,W"; wxAskAt = now; }
  if (wxDirty) { wxDirty = false; drawWeather(); }
}

void drawStaleOverlay() {
  drawRedRing();
  printCentered("NO DATA", CX, 412, 3, C_RED);
  needFlush = true;
}


String pendingStat2 = "";
// v6.8 每 5 秒读一次；有变化就重画、通知手机
void batPoll() {
  if (!pmuOK) return;
  unsigned long now = millis();
  if (batAt && now - batAt < 5000) return;
  batAt = now;
  if(!batteryGauge.refresh())return;
  int voltage=batteryGauge.getVoltage();
  bool present=batteryGauge.getBatteryStatus().isBatteryPresent() && voltage>=2000 && voltage<=5000;
  int pct=present?(int)batteryGauge.getStateOfCharge():-1;
  if(pct>100)pct=-1;
  if(present)batMv=voltage;else batMv=0;
  static int missCnt=0;
  if(pct<0){if(batPct>=0 && ++missCnt<3){pct=batPct;present=true;}}
  else missCnt=0;
  bool chg=present && batteryGauge.isCharging();
  // BQ27220 没有独立 VBUS 状态；只能在确认充电时报告外部供电。
  bool usb=chg;
  bool changed = pct != batPct || chg != batChg || usb != usbIn;
  batPct = pct; batChg = chg; usbIn = usb;
  if (mediaHid && changed && pct >= 0) mediaHid->setBatteryLevel((uint8_t)constrain(pct, 0, 100));
  if (changed) {
    if (onWaitScreen && page == PAGE_HOME && !sliding) drawStatus();        // v7.2 只重画状态栏
    else if (inNav) navDirty = true;
  }
  if (changed || now - batSentAt > 60000) {
    batSentAt = now;
    pendingBat = "V," + String(batPct) + "," + String(batChg ? 1 : 0) + "," + String(usbIn ? 1 : 0) + "," + String(batMv);
  }
}

void sendPendingAck() {
  if (!txChar || !bleConnected || !bleAuthed) return;    // v6.5 没通过验证的手机什么都不发
  // v6.5 告诉手机“验证通过了”：连上后 0.3 / 1.5 / 4 秒各发一次（手机订阅通知可能比加密晚一点）
  static const uint16_t uAt[3] = {300, 1500, 4000};
  if (authNotifies < 3 && millis() - bleAuthAt > uAt[authNotifies]) {
    authNotifies++;
    txChar->setValue((uint8_t *)"U,1", 3);
    txChar->notify();
    txChar->setValue((uint8_t *)"CAP,HID,1", 9); txChar->notify();
    txChar->setValue((uint8_t *)"CAP,LYR,1", 9); txChar->notify();
    txChar->setValue((uint8_t *)"CAP,BG,1", 9); txChar->notify();
    txChar->setValue((uint8_t *)"CAP,BLD,1", 9); txChar->notify();
    pendingHidStatus = String("HID,") + (hidMusicMode ? 1 : 0) + "," + (hidMediaReady() ? 1 : 0);
    appleMedia.resend();
    pendingLyricsStatus = String("ML,") + (musicLyrics ? 1 : 0);
    pendingMediaStatus = String("AMC,") + appleMedia.controlMask();
  }
  if (pendingLyricsStatus.length()) { txChar->setValue((uint8_t *)pendingLyricsStatus.c_str(), pendingLyricsStatus.length()); txChar->notify(); pendingLyricsStatus = ""; }
  if (pendingMediaStatus.length()) { txChar->setValue((uint8_t *)pendingMediaStatus.c_str(), pendingMediaStatus.length()); txChar->notify(); pendingMediaStatus = ""; }
  uint8_t amsFrame[139]; size_t amsSize = 0;
  if (appleMedia.nextFrame(amsFrame, amsSize, millis())) { txChar->setValue(amsFrame, amsSize); txChar->notify(); }
  if (pendingHidStatus.length()) {
    txChar->setValue((uint8_t *)pendingHidStatus.c_str(), pendingHidStatus.length());
    txChar->notify(); pendingHidStatus = "";
  }
  if (pendingBootAck.length()) {
    txChar->setValue((uint8_t *)pendingBootAck.c_str(), pendingBootAck.length());
    txChar->notify(); pendingBootAck = "";
  }
  if (pendingBuildingCapability) {
    txChar->setValue((uint8_t *)"CAP,BLD,2", 9); txChar->notify(); pendingBuildingCapability=false;
  }
  if (pendingAck.length()) {
    if (pendingAck.startsWith("K,1,") || pendingAck.startsWith("K,3,"))
      MAP_RX_LOG("[MAP-RX] ACK notify %s\n", pendingAck.c_str());
    txChar->setValue((uint8_t *)pendingAck.c_str(), pendingAck.length());
    txChar->notify();
    pendingAck = "";
  }
  if (pendingStat2.length()) {
    txChar->setValue((uint8_t *)pendingStat2.c_str(), pendingStat2.length());
    txChar->notify();
    pendingStat2 = "";
  }
  if (pendingProf.length()) {                         // v5.8 分步用时
    txChar->setValue((uint8_t *)pendingProf.c_str(), pendingProf.length());
    txChar->notify();
    pendingProf = "";
  }
  if (pendingBat.length()) {                          // v6.8 电量
    txChar->setValue((uint8_t *)pendingBat.c_str(), pendingBat.length());
    txChar->notify();
    pendingBat = "";
  }
  if (pendingEvt.length()) {                          // v5.2 触摸事件
    txChar->setValue((uint8_t *)pendingEvt.c_str(), pendingEvt.length());
    txChar->notify();
    pendingEvt = "";
  }
}

// ---- v5.9 外圈和边缘渐暗：逐行、两个核心一起画 ----
static inline uint16_t blendF(uint16_t a, uint16_t b, uint32_t t) {   // t: 0~256，256 = 全是 b
  uint32_t ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  uint32_t br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  uint32_t r = (ar * (256 - t) + br * t) >> 8, g = (ag * (256 - t) + bg * t) >> 8, bl = (ab * (256 - t) + bb * t) >> 8;
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

void resetNavState() {
  if (idlePowerOffNavigationActive(true, inNav, cruiseOn, navDir == "A"))
    idlePowerOffReset(&idlePowerOffTimer, millis());
  inNav = false;
  tripOn = false; lightDist = -1;                       // v3.1
  lightCdSec = -1;                                      // v7.13.2 倒计时随导航状态清掉
  mapCamN = 0;                                          // v3.5
  ghostN = 0;                                           // v4.4
  cruiseOn = false;                                     // v4.8
  trafN = 0; rerouteAt = 0; fasterAt = 0; gpsWeak = false;   // v5.0
  trafficAlertLevel = 0; trafficAlertDist = 0; trafficAlertSpan = 0; trafficAlertAt = 0;
  speedKmh = -1;
  hasCam = false;
  laneN = 0;
  ringActive = false; ringDir = "";
  for (int i = 0; i < 3; i++) { imgs[i].showW = 0; imgs[i].showH = 0; imgs[i].recvId = -1; }   // v5.3 天气栏 3、4 留着
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0xF;
}

// 处理文字类消息
void handleText(const String &m) {
  if (m.startsWith("#LY,")) {
    unsigned long gen = 0; unsigned title = 0, artist = 0;
    if (sscanf(m.c_str(), "#LY,%lu,%u,%u", &gen, &title, &artist) == 3 && gen == appleMedia.generation &&
        title == appleMedia.values[0].revision && artist == appleMedia.values[1].revision) {
      lyricImageGeneration = gen; lyricImageTitleRevision = title; lyricImageArtistRevision = artist;
    } else { lyricImageGeneration = 0; imgs[12].showW = 0; imgs[12].showH = 0; imgs[12].recvId = -1; }
    return;
  }
  if (m == "#ML,0" || m == "#ML,1") {
    bool wasEnabled = musicLyrics;
    musicLyrics = m == "#ML,1";
    if (!musicLyrics) {
      lyricImageGeneration = 0; imgs[12].showW = 0; imgs[12].showH = 0; imgs[12].recvId = -1; imgs[12].recvMask = 0; imgs[12].recvParts = 0;
      // Older companion renderers may have put lyrics in the artist bitmap.
      // Ask for a fresh title/artist once per connection or on an actual OFF transition.
      static uint32_t refreshedEpoch = 0xffffffff;
      if (wasEnabled || refreshedEpoch != mediaConnectionEpoch) {
        refreshedEpoch = mediaConnectionEpoch;
        imgs[6].showW = 0; imgs[6].showH = 0; imgs[6].recvId = -1; pendingEvt = "T,M,Q";
      }
    }
    pendingLyricsStatus = String("ML,") + (musicLyrics ? 1 : 0);
    Serial.printf("MEDIA LYRICS enabled=%d\n", musicLyrics);
    musicDirty = true; return;
  }
  if (m == "#HM,0" || m == "#HM,1") {
    bool enabled = m == "#HM,1";
    if (enabled != hidMusicMode) {
      hidMusicMode = enabled; hidPulses.cancelQueued();
      Preferences pr; pr.begin("motonav", false); pr.putBool("mediahid", enabled); pr.end();
    }
    if (!hidMusicMode) { musicLyrics = false; imgs[12].showW = 0; imgs[12].showH = 0; imgs[12].recvId = -1; }
    musicAck = 0; musicAckUntil = 0; musicDirty = true;
    pendingHidStatus = String("HID,") + (hidMusicMode ? 1 : 0) + "," + (hidMediaReady() ? 1 : 0);
    return;
  }
  // A dedicated command: no navigation state or screen invalidation.
  if (m.startsWith("#BOOT,")) {
    int id = BootAnimation::parse(m.c_str() + 6);
    if (id < 0) return;
    bool saved = true;
    if (id != bootBrand) {
      Preferences pr;
      saved = pr.begin("motonav", false);
      if (saved) {
        saved = pr.putInt("bootbrand", id) == sizeof(int32_t);
        pr.end();
      }
      if (saved) bootBrand = id;
    }
    pendingBootAck = "BOOT," + String(saved ? bootBrand : -1);
    return;
  }
  if (m.startsWith("#A,")) {
    int mode = m.substring(3).toInt();
    if (mode >= 0 && mode <= 2 && mode != riderAnimMode) {
      riderAnimMode = mode;
      Preferences pr;
      if (pr.begin("motonav", false)) { pr.putInt("rideranim", mode); pr.end(); }
      riderPrevF = -1; riderPrevZ = -2; catForce = true;
    }
    return;
  }
  if (m == "#AMS?") { appleMedia.resend(); return; }
  if (m == "#GB?") { pendingBuildingCapability=true; return; }
  if (m.length() == 6 && m.startsWith("#B3D,")) {        // v7.13 3D 楼块显示开关（App 设置页）
    bool on = m[5] == '1';
    if (on != bld3dOn) {
      bld3dOn = on;
      Preferences pr; pr.begin("motonav", false); pr.putBool("bld3d", on); pr.end();
      navDirty = true;                                   // 下一帧按新模式重画
    }
    return;
  }
  if (m.startsWith("#ZACK,")) {
    if (hidMusicMode) return;
    musicAck = constrain(m.substring(6).toInt(), 0, 5);
    musicAckUntil = millis() + 2500; musicDirty = true; return;
  }
  if (m.startsWith("#Z,")) {
    int have = 0, play = 0, actions = 0;
    if (sscanf(m.c_str(), "#Z,%d,%d,%d", &have, &play, &actions) == 3) {
      // Keep companion-app metadata as a fallback when HID is enabled. AMS
      // is not present on Android, so suppressing #Z here leaves the song
      // title/buttons permanently blank even though the app has the session.
      musicHave = have != 0; musicPlaying = play != 0; musicActions = actions & 7; musicDirty = true;
    }
    return;
  }
  if (m == "P") return;                                 // 心跳
  if (m == "C") { stopArriveFx(); navProgReset(); resetNavState(); drawWaiting(WAIT_READY); return; }

  if (m.startsWith("#v,")) { speedKmh = m.substring(3).toInt(); navDirty = true; return; }
  if (m.startsWith("#k")) {
    int c1 = m.indexOf(','), c2 = m.indexOf(',', c1 + 1);
    if (c1 > 0 && c2 > c1) {
      hasCam = true;
      camLimit = m.substring(c1 + 1, c2).toInt();
      camDist = m.substring(c2 + 1).toInt();
    } else {
      hasCam = false;
    }
    navDirty = true;
    return;
  }
  if (m.startsWith("#j")) {
    laneN = 0;
    if (m.length() > 3) {
      String s = m.substring(3);
      int n = s.length() / 2;
      if (n > 8) n = 8;
      for (int i = 0; i < n; i++) { laneBg[i] = hexVal(s[2 * i]); laneFr[i] = hexVal(s[2 * i + 1]); }
      laneN = n;
    }
    navDirty = true;
    return;
  }
  if (m.startsWith("#p,")) {                             // v2.0 位置
    int fr = 0, hd = 0, kmh = 0; long xd = 0, yd = 0;
    if (sscanf(m.c_str(), "#p,%d,%ld,%ld,%d,%d", &fr, &xd, &yd, &hd, &kmh) == 5) {
      // v2.6 记下“按旧数据推算的当前位置”和新位置的偏差，画面在 0.4 秒内平滑修正过去
      unsigned long nowMs = millis();
      // v6.7 转弯速度：和上一次定位的朝向差 / 时间（太慢、隔太久、变化太离谱就当没在转）
      float newRate = 0;
      if (fr == posFrame && posAt) {
        float fdt = (nowMs - posAt) / 1000.0f;
        float dhd = hd - posHdg;
        while (dhd > 180) dhd -= 360;
        while (dhd < -180) dhd += 360;
        if (fdt > 0.3f && fdt < 2.5f && kmh >= 6 && fabsf(dhd) < 120) newRate = constrain(dhd / fdt, -45.0f, 45.0f);
      }
      if (fr == posFrame && posAt) {
        float px, py, ph;
        predictPose(nowMs, 0, px, py, ph);              // v6.7 按旧数据（含转弯）推算的现在位置
        float ex = px - xd / 10.0f, ey = py - yd / 10.0f;
        float rem = corrAt ? fmaxf(0, 1 - (nowMs - corrAt) / 400.0f) : 0;   // 上一次还没修正完的部分
        ex += corrX * rem; ey += corrY * rem;
        if (ex * ex + ey * ey < 60 * 60) { corrX = ex; corrY = ey; corrAt = nowMs; }
        else { corrX = corrY = 0; corrAt = 0; }
      }
      posFrame = fr; posX = xd / 10.0f; posY = yd / 10.0f; posHdg = hd; posKmh = kmh; posAt = nowMs;
      hdgRate = hdgRate * 0.3f + newRate * 0.7f;          // v6.7 稍微平滑一下，GPS 朝向偶尔抖一下不至于乱转
      if (inNav) navDirty = true;
    }
    return;
  }
  if (m.startsWith("#q,")) {                             // v2.3 屏幕数据线速度：保存并重启
    int q = m.substring(3).toInt();
    if ((q == 40 || q == 60 || q == 80) && q != qspiMHz) {
      Preferences pr;
      pr.begin("motonav", false);
      pr.putInt("qspi", q);
      pr.end();
      delay(100);
      ESP.restart();
    }
    return;
  }
  if (m.startsWith("#f,")) {                             // v2.6 快速刷屏开关：保存并重启
    bool on = m.substring(3).toInt() != 0;
    if (on != fastFlushOn) {
      Preferences pr;
      pr.begin("motonav", false);
      pr.putBool("fastf", on);
      pr.end();
      delay(100);
      ESP.restart();
    }
    return;
  }
  if (m.startsWith("#t,")) {                             // v2.9 地图配色主题
    int t = m.substring(3).toInt();
    if (t >= 0 && t < THEME_N) { themeIdx = t; navDirty = true; }   // v7.8.11 0~11
    return;
  }
  if (m.startsWith("#Q,")) { curveOn = m.substring(3).toInt() != 0; navDirty = true; return; }   // v6.2 弯道上色
  if (m.startsWith("#R,") || m.startsWith("#D")) {
    // iOS sends #R,nearestQuarter,exactDegrees; Android sends #R then #Ddegrees.
    // Preserve legacy #R,quarter while applying the exact angle when supplied.
    int d;
    if (ScreenRotationCommand::parse(m.c_str(), d) && d != screenDeg) {
      setScreenDeg(d, true); navDirty = true; needFlush = true;
    }
    return;
  }
  if (m.startsWith("#L")) {                              // v6.7 预判提前量："#L300"
    leadMs = constrain(m.substring(2).toInt(), 0, 800);
    return;
  }
  if (m == "#UNPAIR") {                                  // v6.5 忘掉配对过的手机；v6.6 只忘“别的”手机，正在用的这台保留
#if defined(CONFIG_NIMBLE_ENABLED)
    ble_addr_t keep;
    bool haveKeep = false;
    ble_gap_conn_desc d;
    if (bleConnHandle != 0xFFFF && ble_gap_conn_find(bleConnHandle, &d) == 0) { keep = d.peer_id_addr; haveKeep = true; }
    ble_addr_t peers[8];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, 8) == 0) {
      for (int i = 0; i < n; i++) {
        if (haveKeep && ble_addr_cmp(&peers[i], &keep) == 0) continue;
        ble_gap_unpair(&peers[i]);
      }
    }
#endif
    return;
  }
  if (m.startsWith("#e")) {                              // v3.1 行程信息：剩余米,到达时间HHMM,红绿灯数
    int rm = 0, eta = 0, lt = 0;
    if (sscanf(m.c_str(), "#e,%d,%d,%d", &rm, &eta, &lt) == 3) {
      // v7.3 路程进度：剩余变少就算走掉了（一次少太多的当成重新规划，不算）
      unsigned long nowMs = millis();
      if (progLastRm >= 0) {
        long d = (long)progLastRm - rm;
        long cap = 200 + (long)((nowMs - progLastAt) / 1000) * 60;     // 最快按 60 米/秒算
        if (d > 0 && d <= cap) progDone += d;
      }
      progLastRm = rm; progLastAt = nowMs;
      tripRemainM = rm; tripEta = eta; tripLights = lt; tripOn = true;
      long total=progDone+tripRemainM;
      if(total>0)tripProgress.sample(progDone/(float)total,nowMs);
    }
    else tripOn = false;
    navDirty = true;
    return;
  }
  if (m.startsWith("#c")) {                              // v3.5 小地图上的电子眼位置；只有 #c 表示清空
    int oldN = mapCamN;
    int32_t oldX[MCAM_MAX], oldY[MCAM_MAX]; int oldT[MCAM_MAX]; int8_t oldP[MCAM_MAX], oldK[MCAM_MAX]; float oldH[MCAM_MAX], oldA[MCAM_MAX];
    for (int k = 0; k < oldN; k++) { oldX[k] = mapCamX[k]; oldY[k] = mapCamY[k]; oldT[k] = mapCamTyp[k]; oldP[k] = mapCamPref[k]; oldH[k] = mapCamLockH[k]; oldA[k] = mapCamPrefA[k]; oldK[k] = mapCamPrefK[k]; }
    mapCamN = 0;
    const char *q = m.c_str() + 2;
    if (*q == ',') {
      char *e;
      mapCamFrame = strtol(q + 1, &e, 10); q = e;
      while (*q == ',' && mapCamN < MCAM_MAX) {
        long v[4]; bool ok = true;
        for (int k = 0; k < 4; k++) {
          if (*q != ',') { ok = false; break; }
          v[k] = strtol(q + 1, &e, 10); q = e;
        }
        if (!ok) break;
        mapCamX[mapCamN] = v[0]; mapCamY[mapCamN] = v[1]; mapCamSpd[mapCamN] = v[2]; mapCamTyp[mapCamN] = v[3];
        // v3.8 同一个电子眼（坐标一样）沿用上次的图标位置和锁定朝向（v4.0）
        int8_t pref = -1;
        float lockH = 0, prefA = 0; int8_t prefK = 0;
        for (int k = 0; k < oldN; k++)
          if (oldX[k] == v[0] && oldY[k] == v[1] && oldT[k] == v[3]) { pref = oldP[k]; lockH = oldH[k]; prefA = oldA[k]; prefK = oldK[k]; break; }
        mapCamPref[mapCamN] = pref; mapCamLockH[mapCamN] = lockH; mapCamPrefA[mapCamN] = prefA; mapCamPrefK[mapCamN] = prefK;
        mapCamN++;
      }
    }
    navDirty = true;
    return;
  }
  if (m.startsWith("#g")) {                              // v3.1 前方红绿灯距离；只有 #g 表示隐藏
    int d = -1;
    lightDist = (sscanf(m.c_str(), "#g,%d", &d) == 1) ? d : -1;
    navDirty = true;
    return;
  }
  if (m.startsWith("#GL,")) {                            // v7.13.2 红绿灯倒计时：0=红 1=绿，秒 0..180
    int p = 0, s = -1;
    if (sscanf(m.c_str(), "#GL,%d,%d", &p, &s) == 2 && s >= 0 && s <= 180) {
      lightCdPhase = p ? 1 : 0;
      lightCdSec = s;
      lightCdAt = millis();
      navDirty = true;
    }
    return;
  }
  if (m.startsWith("#n,")) { nightOn = m.substring(3).toInt() != 0; navDirty = true; return; }   // v3.0 夜间配色
  if (m == "#o") { ghostN = 0; navDirty = true; return; }   // v4.4 不显示原路线
  if (m == "#WXRESET") { wxHave = false; wxExtra = false; wxDetail = 0; wxDirty = true; return; }
  if (m.startsWith("#WX,")) { parseWeatherExtra(m); return; }
  if (m.startsWith("#W,")) { parseWeather(m); return; }   // v5.3 天气
  if (m.startsWith("#i,")) {                             // v5.1 来电："#i,1,号码" / "#i,0"
    bool on = m.charAt(3) == '1';
    if (on) {
      if (!callOn) { callAt = millis(); callAnswerSent = false; touchHoldAt = millis(); touchHold = 0; }
      callSeenAt = millis();
      int c2 = m.indexOf(',', 4);
      callNum = c2 > 0 ? m.substring(c2 + 1) : "";
      if (callNum.length() > 16) callNum = callNum.substring(0, 16);
    }
    callOn = on;
    if (!on) navDirty = true;
    return;
  }
  if (m.startsWith("#y,")) {                             // v5.0 重新规划中
    rerouteAt = m.substring(3).toInt() ? max(1UL, millis()) : 0;
    if (rerouteAt) fasterAt = 0;
    navDirty = true; return;
  }
  if (m.startsWith("#h")) {                              // v5.0 更快路线：省几分钟；只有 #h 表示收起
    if (m.length() > 3) { fasterMin = m.substring(3).toInt(); fasterAt = max(1UL, millis()); } else fasterAt = 0;
    navDirty = true; return;
  }
  if (m.startsWith("#x,")) { gpsWeak = m.substring(3).toInt() != 0; navDirty = true; return; }   // v5.0 GPS 信号弱
  if (m.startsWith("#u")) {                              // v5.0 路况：起,止,状态,...；只有 #u 表示清空
    trafN = 0;
    const char *q = m.c_str() + 2;
    while (*q == ',' && trafN < TRAF_MAX) {
      char *e; long a = strtol(q + 1, &e, 10); q = e; if (*q != ',') break;
      long b = strtol(q + 1, &e, 10); q = e; if (*q != ',') break;
      long st = strtol(q + 1, &e, 10); q = e;
      trafA[trafN] = a; trafB[trafN] = b; trafS[trafN] = (uint8_t)st; trafN++;
    }
    navDirty = true; return;
  }
  if (m.startsWith("#TR")) {                             // 初版前方实时路况提醒
    int level = 0, dist = 0, span = 0;
    if (sscanf(m.c_str(), "#TR,%d,%d,%d", &level, &dist, &span) == 3) {
      trafficAlertLevel = constrain(level, 0, 4);
      trafficAlertDist = max(0, dist);
      trafficAlertSpan = max(0, span);
      trafficAlertAt = millis();
      Serial.printf("[TRAFFIC] level=%d ahead_m=%d span_m=%d\n", trafficAlertLevel, trafficAlertDist, trafficAlertSpan);
      navDirty = true;
    }
    return;
  }
  if (m.startsWith("#w,")) { cleanOn = m.substring(3).toInt() != 0; navDirty = true; return; }   // v4.9 清爽模式
  if (m.startsWith("#z,")) {                             // v4.8 巡航模式开关
    cruiseOn = m.substring(3).toInt() != 0;
    if (cruiseOn) { inNav = true; navDir = "S"; navDist = 0; ringActive = false; tripOn = false; laneN = 0; }
    navDirty = true;
    return;
  }
  if (m.startsWith("#r,")) {                             // v3.0 走过的路线：1 变灰，2 擦除
    int r = m.substring(3).toInt();
    if (r == 1 || r == 2) { trailMode = r; navDirty = true; }
    return;
  }
  if (m.startsWith("#m,")) { mapOn = m.substring(3).toInt() != 0; navDirty = true; return; }
  if (m.startsWith("#b,")) {
    int b = constrain(m.substring(3).toInt(), 5, 255);
    if (flushDone) {                                    // v2.2 刷屏任务在用总线时先等它推完
      xSemaphoreTake(flushDone, portMAX_DELAY);
      boardBrightness(b);
      xSemaphoreGive(flushDone);
    } else {
      boardBrightness(b);
    }
    return;
  }

  if (m.startsWith("#O")) {                              // v7.1 行程统计 "#O米/分钟/最高车速"
    long a = -1; int b = -1, c = -1;
    if (sscanf(m.c_str() + 2, "%ld/%d/%d", &a, &b, &c) >= 2) { tripM = a; tripMin = b; tripMaxKmh = c; tripAt = millis(); }
    return;
  }
  if (m == "#M1" || m == "#M0") { progOn = m[2] == '1'; navDirty = true; return; }   // v7.3 路程进度开关
  if (m == "#J") { if (page == PAGE_HOME) startArriveFx(true); return; }   // v7.1 到达途经点
  if (m.startsWith("#")) return;                        // v6.7 不认识的 "#..." 指令：忽略（别当成导航方向）
  int comma = m.indexOf(',');                           // 导航：方向,距离
  if (comma > 0) {
    String prevDir = navDir;
    navDir = m.substring(0, comma);
    // v7.1 导航中 → 到达：放特效（重连补发的 "A,0"、圆屏刚开机收到的都不放）
    if (navDir == "A" && prevDir.length() && prevDir != "A" && page == PAGE_HOME) startArriveFx(false);
    if (prevDir == "A" && navDir != "A") navProgReset();       // v7.3 到达后又开始新的导航：进度从头算
    else if (navDir != "A" && fxOn) stopArriveFx();
    int comma2 = m.indexOf(',', comma + 1);
    if (comma2 > 0) {
      navDist = m.substring(comma + 1, comma2).toInt();
      navAngle = m.substring(comma2 + 1).toFloat();
    } else {
      navDist = m.substring(comma + 1).toInt();
      navAngle = NAN;
    }
    inNav = true;
    if (navDir != "A") idlePowerOffReset(&idlePowerOffTimer, millis());
    updateRingTarget();
    navDirty = true;
  }
}

void handleMessage(const uint8_t *d, int n) {
  lastRxMs = millis();
  if (staleShown) { staleShown = false; navDirty = true; }   // 收到任何消息，清掉数据中断提示
  if (d[0] == 'B') { handleBitmap(d, n); return; }
  if (d[0] == 'M') { handleBulk(d, n); return; }     // v2.0 地图块 / 路线
  char buf[512];                                      // v4.8 64 → 512：电子眼列表长了会被截断
  int len = n < 511 ? n : 511;
  memcpy(buf, d, len);
  buf[len] = 0;
  String m = String(buf);
  m.trim();
#if DEBUG_SERIAL
  Serial.println("recv: " + m);
#endif
  handleText(m);
}

// v7.0 蓝牙名 = "MotoNav-" + 蓝牙地址最后两个字节
void makeBleName() {
  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) return;
  char b[20];
  snprintf(b, sizeof(b), "%s-%02X%02X", DEVICE_NAME, mac[4], mac[5]);
  bleName = b;
}

void setup() {
  makeBleName();                                       // v7.0 先算好蓝牙名（首页要显示）
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // 没打开串口监视器时打印不等待（v1.2 的修复）
  delay(100);
  Serial.printf("[BOOT] serial alive board=185B heap=%u psram=%u\n", ESP.getFreeHeap(), ESP.getFreePsram());
  Serial.printf("Reset reason=%d, sleep wake=%d\n", (int)esp_reset_reason(),
                (int)esp_sleep_get_wakeup_cause());
  // Release the backlight RTC hold left by idle deep sleep.
  rtc_gpio_hold_dis((gpio_num_t)LCD_BL);
  rtc_gpio_deinit((gpio_num_t)LCD_BL);
  pinMode(LCD_BL,OUTPUT);digitalWrite(LCD_BL,LOW);
  backlightPWM=ledcAttach(LCD_BL,20000,8);
  Wire.begin(IIC_SDA, IIC_SCL);
  imuOK=imu.begin(Wire,0x6B,IIC_SDA,IIC_SCL); if(!imuOK) imuOK=imu.begin(Wire,0x6A,IIC_SDA,IIC_SCL);
  if(imuOK){ imu.configAccel(SensorQMI8658::AccelRange::FS_8G,SensorQMI8658::AccelODR::ODR_112Hz); imu.configGyro(SensorQMI8658::GyroRange::FS_512DPS,SensorQMI8658::GyroODR::ODR_112Hz); imu.enableAccel(); imu.enableGyro(); }
  touchInit();                                         // v5.2 触摸屏
  batInit();                                           // v6.8 电源芯片（电量）

  // v2.3 读取保存的屏幕数据线速度（默认 80MHz）
  {
    Preferences pr;
    pr.begin("motonav", true);
    qspiMHz = pr.getInt("qspi", 40);
    riderAnimMode = constrain(pr.getInt("rideranim", 2), 0, 2);
    screenRot = pr.getInt("rot", 0) & 3;                            // v6.2 圆屏方向
    setScreenDeg(pr.getInt("rotd", screenRot * 90), false);         // v6.4 任意角度
    bootBrand = BootAnimation::valid(pr.getInt("bootbrand", 0));
    hidMusicMode = pr.getBool("mediahid", false);
    fastFlushOn = pr.getBool("fastf", true);
    bld3dOn = pr.getBool("bld3d", true);                 // v7.13 3D 楼块（App 可用 #B3D,0 关成 2D 俯视）
    pr.end();
    if (qspiMHz != 40 && qspiMHz != 60 && qspiMHz != 80) qspiMHz = 80;
  }
  if (!gfx->begin(qspiMHz * 1000000)) { Serial.println("gfx->begin() failed; stopping before framebuffer use");while(true)delay(1000); }
  riderBindRenderer(riderFillRect, riderPrintText, riderDrawStatusBar);
  boardBrightness(200);

  iconBuf = (uint16_t *)ps_malloc(ICON_BUF_W * ICON_BUF_H * 2);   // v1.8.1 图标缓存
  buildRingLut();                                                   // v1.8.3 抗锯齿圆环查找表
  mapInit();                                                        // v2.0 小地图缓冲
  if (!SPIFFS.begin(true)) Serial.println("[BG] SPIFFS mount failed");
  loadBackground();                                                 // v8.5 user background, falls back to built-in screen
  // Text images are allocated on first use, so unused slots do not consume PSRAM
  // during startup and a failed allocation can be handled without a reboot.
  for (int i = 0; i < IMG_SLOTS; i++) {
    imgs[i] = {};
    imgs[i].recvId = -1;
  }

  // Begin with black instead of briefly displaying the waiting page before the intro.
  gfx->fillScreen(0);
  gfx->flush();
  initRotSpan();                                                    // v6.4 任意角度用
  initDoubleBuffer();                                               // v2.2 双缓冲 + 核心 0 刷屏
  helperInit();                                                     // v5.7 另一个核心帮忙画底图

  if (bootBrand != BootAnimation::Off && gfx->getFramebuffer()) {
    uint32_t bootAt = millis();
    while (millis() - bootAt < BootAnimation::Duration) {
      uint32_t frameAt = millis();
      BootAnimation::render(gfx->getFramebuffer(), frameAt - bootAt, bootBrand);
      flushSync();
      uint32_t spent = millis() - frameAt;
      delay(spent < 33 ? 33 - spent : 1);
    }
  }
  drawWaiting(WAIT_NOLINK);
  flushSync();
  needFlush = false;
  bootAudioDueAt = millis() + 500;                  // 主页和蓝牙初始化稳定后再播提示音

  BLEDevice::init(bleName);                    // v7.0 名字带地址后 4 位；名字放不进广播包时库会自动放到扫描应答里
  BLEDevice::setMTU(517);     // 允许手机协商大数据包，用来传文字图片
#if defined(CONFIG_NIMBLE_ENABLED)
  // v4.5 允许蓝牙 2M 模式（速度翻倍，手机请求时直接同意）和最长空中数据包（251 字节）
  ble_gap_set_prefered_default_le_phy(BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK,
                                      BLE_GAP_LE_PHY_1M_MASK | BLE_GAP_LE_PHY_2M_MASK);
  ble_gap_write_sugg_def_data_len(251, 2120);
#endif
  // v6.5 配对验证：圆屏显示 6 位随机配对码（每次配对都换），手机输入；要绑定 + 防中间人 + 安全连接
  {
    BLESecurity *sec = new BLESecurity();                 // 构造时设好“交换加密密钥和身份密钥”（手机换地址也认得出）
    sec->setCapability(ESP_IO_CAP_OUT);                   // 圆屏只能显示，不能输入
    sec->setAuthenticationMode(true, true, true);         // 绑定、防中间人、安全连接
    sec->setPassKey(false);                               // 随机配对码
    // Keep the passkey stable across reconnects. Regenerating it on every
    // connection leaves Android bonded while the peripheral expects a new key.
    sec->regenPassKeyOnConnect(false);
    BLEDevice::setSecurityCallbacks(new SecCB());
  }
  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  BLEService *svc = server->createService(SERVICE_UUID);
  BLECharacteristic *rx = svc->createCharacteristic(
    RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
#if defined(CONFIG_NIMBLE_ENABLED)
             | BLECharacteristic::PROPERTY_WRITE_ENC | BLECharacteristic::PROPERTY_WRITE_AUTHEN   // v6.5 必须配对过才能写
#endif
  );
  rx->setCallbacks(new RxCB());
  txChar = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);   // v2.0
  txChar->addDescriptor(new BLE2902());
  svc->start();
  initMediaHid(server);
  appleMedia.begin();
  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->addServiceUUID(BLEUUID((uint16_t)0x1812));
  adv->setAppearance(GENERIC_HID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  tpmsBegin();                                 // TPMS 扫描和外设角色共存（NimBLE 多角色）
  idlePowerOffReset(&idlePowerOffTimer, millis());
  Serial.println(String("MotoNav ready, firmware ") + FW_VERSION);
}

// Power down before destroying BLE objects: the host can still dispatch events
// referencing those objects. Shutdown does not need to reclaim their memory.
void idlePowerOffShutdown() {
  powerOffPending = true;
  Serial.println("No navigation for 5 minutes: powering off");
  stopBootAudio();
  const bool displayReady = !flushDone ||
      xSemaphoreTake(flushDone, pdMS_TO_TICKS(3000)) == pdTRUE;
  if (displayReady) {
    boardBrightness(0);
    panel->displayOff();
  }
  // 1.85B has a BQ27220 fuel gauge and a separate hardware PWR latch.
  // It has no AXP2101 software power-off register. Do not write 0x34.
  // Enter deep sleep without a timer; BOOT wakes, PWR switches supply.
  Serial.println("1.85B: entering deep sleep (BOOT to wake, PWR for hardware power off)");
  if(backlightPWM) { ledcDetach(LCD_BL);backlightPWM=false; }
  rtc_gpio_init((gpio_num_t)LCD_BL);
  rtc_gpio_set_direction((gpio_num_t)LCD_BL, RTC_GPIO_MODE_OUTPUT_ONLY);
  rtc_gpio_set_level((gpio_num_t)LCD_BL, 0);
  rtc_gpio_hold_en((gpio_num_t)LCD_BL);
  pinMode(PA,OUTPUT);digitalWrite(PA,LOW);
  // Stop the host/controller without deleting live GATT objects. Deep sleep
  // discards RAM anyway. Keep objects valid until pending callbacks have ended.
  tpmsEnd();                                          // TPMS：停扫描
#if defined(CONFIG_NIMBLE_ENABLED)
  nimble_port_stop();
#elif defined(CONFIG_BLUEDROID_ENABLED)
  esp_bluedroid_disable();
#endif
#if CONFIG_BT_CONTROLLER_ENABLED
  esp_bt_controller_disable();
#endif
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  rtc_gpio_init(GPIO_NUM_0);
  rtc_gpio_set_direction(GPIO_NUM_0, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(GPIO_NUM_0);
  rtc_gpio_pulldown_dis(GPIO_NUM_0);
  // EXT0 is level-triggered: arming it while BOOT is held low causes an
  // immediate wake/reboot. Require 100 ms released, with a bounded wait.
  uint32_t releasedAt = millis(), waitAt = releasedAt;
  while (millis() - waitAt < 1500 && millis() - releasedAt < 100) {
    if (!rtc_gpio_get_level(GPIO_NUM_0)) releasedAt = millis();
    delay(10);
  }
  if (millis() - releasedAt >= 100 && rtc_gpio_get_level(GPIO_NUM_0)) {
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);
  } else {
    Serial.println("BOOT is low; leave wake disabled to avoid instant restart (use PWR to power cycle)");
  }
  esp_deep_sleep_start();
}

void loop() {
  if (bootAudioFinished) {
    bootAudioFinished = false;
    stopBootAudio();
  }
  if (bootAudioDueAt && (int32_t)(millis() - bootAudioDueAt) >= 0) {
    bootAudioDueAt = 0;
    startBootAudio();
  }
  imuTick();
  if(page == PAGE_IMU && !sliding && !callOn && !pairOn){ static uint32_t imuDrawAt=0; if(millis()-imuDrawAt>=33){imuDrawAt=millis(); drawImuPage();}}   // v7.13.3 80ms→33ms：面板 80MHz QSPI 可到 ~30fps，原来是被这里的节流卡住的
  hidMediaTick();                                      // press/release without blocking navigation
  static uint32_t lyricConnectionEpoch = 0;
  if (lyricConnectionEpoch != mediaConnectionEpoch) {
    lyricConnectionEpoch = mediaConnectionEpoch; musicLyrics = false; lyricImageGeneration = 0;
    imgs[12].showW = 0; imgs[12].showH = 0; imgs[12].recvId = -1; musicDirty = true;
  }
  appleMedia.tick(bleConnected && bleAuthed && hidMusicMode, bleConnHandle, mediaConnectionEpoch, millis());
  static int lastControlMask = -1;
  int controlMask = appleMedia.controlMask();
  if (lastControlMask != controlMask) { lastControlMask = controlMask; pendingMediaStatus = String("AMC,") + controlMask; musicDirty = true; }
  if (appleMedia.controlAck) { musicAck = appleMedia.controlAck > 0 ? 9 : 10; appleMedia.controlAck = 0; musicAckUntil = millis() + 2500; musicDirty = true; }
  if (appleMedia.changed) {
    appleMedia.changed = false; musicDirty = true;
    static uint32_t lyricGeneration = 0; static uint16_t lyricTitleRevision = 0, lyricArtistRevision = 0;
    if (lyricGeneration != appleMedia.generation || lyricTitleRevision != appleMedia.values[0].revision || lyricArtistRevision != appleMedia.values[1].revision) {
      lyricImageGeneration = 0;
      lyricGeneration = appleMedia.generation; lyricTitleRevision = appleMedia.values[0].revision; lyricArtistRevision = appleMedia.values[1].revision;
      imgs[12].showW = 0; imgs[12].showH = 0; imgs[12].recvId = -1;
    }
    if (hidMusicMode && appleMedia.available) {
      // Android supplies metadata through #Z/images; AMS is unavailable
      // there. Do not erase the Android song state every time AMS retries.
      musicHave = appleMedia.values[0].text.length();
      musicPlaying = appleMedia.playback.known && appleMedia.playback.state == 1;
    }
  }
  static unsigned long mediaDrawAt = 0;
  if (hidMusicMode && appleMedia.available && millis() - mediaDrawAt >= 1000) { mediaDrawAt = millis(); musicDirty = true; }
  saveRotIfDue();                                      // v6.4
  // v6.5 连上 60 秒还没通过验证（不是自己的手机）：断开，让自己的手机能连
#if defined(CONFIG_NIMBLE_ENABLED)
  if (bleConnected && !bleAuthed && bleConnHandle != 0xFFFF && millis() - bleConnAt > AUTH_TIMEOUT_MS) {
    ble_gap_terminate(bleConnHandle, BLE_ERR_REM_USER_CONN_TERM);
    bleConnAt = millis();                              // 别每圈都断
  }
#endif
  // 1. 连接状态变化（先处理）；v6.5 通过验证才算连上
  bool conn = bleConnected && bleAuthed;
  if (conn != shownConn) {
    shownConn = conn;
    if (conn) {
      lastRxMs = millis();
      drawWaiting(WAIT_READY);
    } else {
      bool wasNav = inNav;
      resetNavState();
      staleShown = false;
      drawWaiting(wasNav ? WAIT_LOST : WAIT_NOLINK);   // 导航途中断开 → 红色 LINK LOST
    }
  }

  // 2. 取出队列里的所有消息逐条处理（最多 30 条，避免卡住画面）
  if (!conn) {                                     // 刚断开：丢掉残余消息，别让它们把 LINK LOST 画面盖掉
    portENTER_CRITICAL(&mux);
    rxTail = rxHead;
    portEXIT_CRITICAL(&mux);
  }
  static RxMsg local;
  for (int i = 0; i < 30; i++) {
    bool got = false;
    portENTER_CRITICAL(&mux);
    if (rxTail != rxHead) {
      local.len = rxq[rxTail].len;
      memcpy(local.data, rxq[rxTail].data, local.len);
      rxTail = (rxTail + 1) % RXQ_SLOTS;
      got = true;
    }
    portEXIT_CRITICAL(&mux);
    if (!got) break;
    handleMessage(local.data, local.len);
  }

  updateStats();
  if (pendingStat.length()) { pendingStat2 = pendingStat; pendingStat = ""; }
  sendPendingAck();                                    // v2.0 地图收齐的确认 / v2.2 帧率

  // 3. 进度环动画；小地图模式下连续刷新（v2.2 不再限制帧间隔）
  animateRing();
  // 地图帧原本连续刷新；大箭头画面也按现有帧节奏推进行程环。
  static unsigned long tripDrawAt=0;
  static float tripDrawValue=-1;
  float tripValue=tripFrac();
  if(inNav && progOn && tripOn && !cruiseOn && !ringActive && !staleShown &&
     tripValue>=0 && tripValue!=tripDrawValue && millis()-tripDrawAt>=33) {
    tripDrawAt=millis();tripDrawValue=tripValue;navDirty=true;
  }
  if(tripValue<0)tripDrawValue=-1;
  static bool wasMap = false;
  bool mapNow = inNav && mapModeActive();
  if (mapNow != wasMap) { wasMap = mapNow; navDirty = true; }   // 进出地图模式时整帧重画
  if (mapNow) navDirty = true;

  touchPoll();                                         // v5.2 按住接听 / v5.3 滑动换页
  batPoll();                                           // v6.8 电量
  tpmsTick(conn);                                      // TPMS 胎压：解析广播 + 扫描健康检查
  // 4. 需要时重画导航画面
  // v6.5 配对：整屏显示配对码，配好 / 失败 / 60 秒 / 断开后回到原来的画面
  if (pairReq) {
    pairReq = false;
    if (sliding) dragAbort();
    pairOn = true; pairShowAt = millis();
    drawPairScreen();
  }
  if (pairOn && (pairDone || millis() - pairShowAt > AUTH_TIMEOUT_MS || !bleConnected)) {
    pairOn = false; pairDone = false;
    if (page == PAGE_MUSIC) musicDirty = true;
    else if (page == PAGE_WEATHER) wxDirty = true;
    else if (page == PAGE_IMU) drawImuPage();
    else if (inNav) navDirty = true;
    else drawWaiting(conn ? WAIT_READY : WAIT_NOLINK);
  }
  // v5.1 来电：整屏显示（约 12 帧/秒的动画），结束后回到原来的画面
  static bool wasCall = false;
  if (callOn && (millis() - callSeenAt > 120000 || !conn)) callOn = false;      // 2 分钟没消息 / 断开：收起
  if (callOn && sliding) dragAbort();                  // v5.5 拖到一半来电：放弃换页
  if (callOn) {
    static unsigned long lastCallDraw = 0;
    if (millis() - lastCallDraw > 80) { lastCallDraw = millis(); drawCallScreen(); }
    wasCall = true;
  } else if (wasCall) {
    wasCall = false;
    if (page == PAGE_MUSIC) musicDirty = true;
    else if (page == PAGE_WEATHER) wxDirty = true;
    else if (page == PAGE_IMU) drawImuPage();
    else if (inNav) navDirty = true;
    else drawWaiting(conn ? WAIT_READY : WAIT_NOLINK);
  }

  // v7.1 到达特效：来电、配对优先；放完 / 被点掉后回到原来的画面
  static bool wasFx = false;
  if (fxOn && (millis() - fxAt > fxDur || !conn)) fxOn = false;
  if (fxOn) {
    static unsigned long lastFxDraw = 0;
    if (!callOn && !pairOn && page == PAGE_HOME && !sliding && millis() - lastFxDraw >= 33) {
      lastFxDraw = millis();
      drawArriveFx(millis() - fxAt);
    }
    wasFx = true;
  } else if (wasFx) {
    wasFx = false;
    if (!callOn && !pairOn) {
      if (page == PAGE_MUSIC) musicDirty = true;
      else if (page == PAGE_WEATHER) wxDirty = true;
      else if (page == PAGE_IMU) drawImuPage();
      else if (inNav) navDirty = true;
      else drawWaiting(conn ? WAIT_READY : WAIT_NOLINK);
    }
  }

  musicTick();
  weatherTick();                                       // v5.3 天气页
  if (inNav && navDirty && !fxOn && !callOn && !pairOn && page == PAGE_HOME && !sliding) {
    drawNavFrame();
    navDirty = false;
  }

  // 5. 导航中长时间收不到任何消息 → 数据中断提示
  if (inNav && conn && !fxOn && !callOn && !pairOn && page == PAGE_HOME && !sliding && !staleShown && millis() - lastRxMs > STALE_MS) {
    staleShown = true;
    drawStaleOverlay();
  }

  animateidle();                                        // v3.6 首页猫猫动画

  // 6. 有变化才把画布推到屏幕（小地图帧异步推送，其他画面同步推送）
  if (needFlush && !sliding) {
    if (frameIsMap) flushAsync(); else flushSync();
    needFlush = false;
    frameIsMap = false;
  }
  // Idle phone traffic (including HID/AMS) does not count as navigation.
  const bool navigating = idlePowerOffNavigationActive(
      bleConnected && bleAuthed, inNav, cruiseOn, navDir == "A");
  const bool interacting = pairOn || pairReq || callOn || touchDown || dragging || sliding;
  if (idlePowerOffDue(&idlePowerOffTimer, millis(), navigating, interacting)) {
    bool queued;
    portENTER_CRITICAL(&mux);
    queued = rxTail != rxHead;
    portEXIT_CRITICAL(&mux);
    if (!queued) idlePowerOffShutdown();
  }
  delay(mapNow ? 1 : 10);
}

package com.example.motonav

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import org.json.JSONArray
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.util.concurrent.Executors
import kotlin.math.cos
import kotlin.math.roundToInt
import kotlin.math.sin
import kotlin.math.sqrt

/**
 * 版本：跟随 App 版本，见 MainActivity.kt 里的 APP_VERSION（v2.0 新增）
 *
 * 小地图数据：
 * - 从 OpenStreetMap（Overpass 接口）下载当前位置附近约 2.4 公里见方的道路、绿地、水面，
 *   换算成高德坐标，简化压缩后发给圆屏（"地图块"）。快开出这一块时自动下载下一块（向前方偏移）。
 * - 导航路线（高德规划的路线坐标）单独打包发给圆屏，画成紫色。
 * - 每次定位把当前位置、朝向、车速发给圆屏："#p,坐标系,x分米,y分米,朝向,车速"。
 * - 圆屏收齐一个地图块/路线后会回一条确认；没确认的 8 秒后重发，重连后也会重发。
 * - v2.3 地图来源可选：天地图（图片地图，国内数据全，见 TdtRaster）/ OpenStreetMap（线条地图）/
 *   自动（天地图优先，失败换 OpenStreetMap）。坐标原点按所在区域固定（不再每次启动随机），
 *   App 重启后圆屏上已有的地图还能继续用。
 * - v2.8 夜间配色（"#n,1/0"，自动模式按日出日落切换）、走过的路线变灰/擦除（"#r,1/2"）。
 * - v4.8 清爽模式 "#w,1/0"（小地图保留，去掉红绿灯、电子眼、安全提示、测速牌和行程信息，配合固件 v4.9）
 * - v4.5 原路线（导入的高德分享路线）：大块数据类型 4，圆屏画成橙色细线；"#o" 不显示（配合固件 v4.4）
 * - v5.9 风格扩到 11 种（"#t,0~10"），弯道急缓上色 "#Q,1/0"（v6.1 修正，原来误用 #q；配合固件 v6.2）。
 * - v6.1 圆屏方向 "#R,0~3"（0°/90°/180°/270°）
 * - v6.3 圆屏方向任意角度 "#D角度"（配合固件 v6.4），同时照发 "#R" 给旧固件
 * - v7.2 预判提前量 "#L毫秒"（配合固件 v6.7）
 * - v8.0 路程进度条开关 "#M1" / "#M0"（配合固件 v7.3；默认开）；v8.1 固件 v7.4 起改成最外圈的行程环
 *   卫星图风格：改下天地图卫星照片（范围小一些：半边 700 米、每像素 3.5 米），压成 16 色连同调色板发给圆屏
 */
object MapFeeder {
    private const val PREFS = "motonav"
    private const val HALF = 1200.0            // 地图块半边长（米）
    private const val REFRESH = 650.0          // 离地图块中心超过这个距离就准备下一块
    private const val AHEAD = 450.0            // 新地图块中心往前方偏移，前面的路多下载一些
    const val MAX_BYTES = 190_000              // 圆屏地图缓冲 200KB，留余量
    const val MAX_FEAT = 7000
    private const val RESEND_MS = 8000L

    const val TYPE_CHUNK = 1
    const val TYPE_ROUTE = 2
    const val TYPE_RASTER = 3                  // v2.3 天地图图片地图
    const val TYPE_GHOST = 4                   // v4.5 原路线

    // v2.3 地图来源
    const val SRC_AUTO = 0
    const val SRC_TDT = 1
    const val SRC_OSM = 2
    const val SRC_AMAP = 3
    @Volatile var source = SRC_AUTO
        private set
    @Volatile var previewRaw: android.graphics.Bitmap? = null   // 天地图原图（预览）
    @Volatile var previewCls: android.graphics.Bitmap? = null   // 归类上色后（预览）
    @Volatile var previewVer = 0
    // v2.5 地图配色主题：0 原版，1 罪恶都市（默认），2 GTA5；v5.9 增加 3~10
    @Volatile var theme = 1
        private set
    const val THEME_SAT = 10
    const val THEME_DRAGON = MapStylePolicy.DRAGON_RADAR
    fun routeOnly() = !MapStylePolicy.usesBasemap(theme)
    val THEME_ORDER = listOf(1, 2, 0, 3, 4, 5, 6, 7, 8, 9, 10, THEME_DRAGON)   // 列表里的顺序
    @Volatile var curve = false                                  // v5.9 弯道急缓上色
        private set
    private const val SAT_HALF = 700.0                           // v5.9 卫星图地图块半边长（米）
    private const val SAT_REFRESH = 330.0
    private const val SAT_AHEAD = 260.0
    private var chunkSat = false                                 // 当前地图块是不是卫星图
    private var chunkType = TYPE_CHUNK

    // v2.8 夜间配色：0 自动（日出日落），1 固定白天，2 固定夜间
    const val NIGHT_AUTO = 0
    const val NIGHT_OFF = 1
    const val NIGHT_ON = 2
    @Volatile var nightMode = NIGHT_AUTO
        private set
    @Volatile var nightNow = false               // 现在圆屏用的是不是夜间配色
        private set
    private var nightSent = -1                   // 上次发给圆屏的值（-1 = 还没发）
    // v2.8 走过的路线：1 变灰，2 擦除
    const val TRAIL_GREY = 1
    const val TRAIL_ERASE = 2
    @Volatile var trail = TRAIL_GREY
        private set

    private val handler = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadExecutor()

    @Volatile var enabled = true
        private set
    @Volatile var clean = false                // v4.8 清爽模式：地图保留，去掉红绿灯、电子眼等
        private set
    @Volatile var status = "（还没开始）"
    @Volatile var chunkInfo = ""
    @Volatile var routeInfo = ""

    private var ctx: Context? = null
    private var frame: MapGeo.Frame? = null

    // 当前地图块
    private var chunkCx = 0.0
    private var chunkCy = 0.0
    private var haveChunk = false
    @Volatile private var fetching = false
    private var lastFetchFail = 0L
    private var chunkId = 0
    private var chunkBytes: ByteArray? = null
    private var compactVectorBytes: ByteArray? = null
    private var compactRasterBytes: ByteArray? = null
    private var chunkFallbackLevel = 0
    private var chunkRetryCount = 0
    private var fallbackPreparing = false
    private var transferLabel = ""
    private var deliveryBytes = 0
    private var deliveryCx = 0.0
    private var deliveryCy = 0.0
    private var deliveryHalf = HALF
    private var chunkAcked = false
    private var chunkSentAt = 0L
    private var chunkDeliveryId = -1
    private var chunkDeliveryStart = 0L
    private var chunkBaseBytes: ByteArray? = null
    private var chunkBuildingsComplete = false
    @Volatile private var requestSerial = 0
    private var baseInstalledSerial = -1
    private var haveConfirmedChunk = false
    private var confirmedCx = 0.0
    private var confirmedCy = 0.0
    private var confirmedSat = false
    private var chunkHalf = HALF
    private var confirmedHalf = HALF
    private var offlineKey = ""
    private var offlineMaps: RouteOfflineMaps? = null
    private var preparedRouteKey=""
    private var preparedRouteCoords: List<DoubleArray> = emptyList()
    @Volatile var buildingEnabled=true
        private set
    @Volatile var building3dEnabled=true      // v7.13 圆屏 3D 楼块显示（#B3D,0/1）；关 = 俯视平铺
        private set
    @Volatile private var smallDisplayRasterCap=0
    fun onSmallDisplayCapability(cap: Int) {
        smallDisplayRasterCap=if(cap<=0) 0 else cap.coerceIn(1024,8*1024)
        if(smallDisplayRasterCap==0) return
        val current=chunkBytes ?: return
        if(chunkType==TYPE_RASTER && !chunkAcked && current.size>smallDisplayRasterCap) {
            compactRasterBytes=null
            chunkFallbackLevel=0
            BleLink.cancelBulk(TYPE_RASTER)
            sendChunk()
        }
    }
    fun buildingMode()=source==SRC_AMAP && buildingEnabled && BleLink.buildingsSupported
    fun setBuildingEnabled(value: Boolean) {
        buildingEnabled=value
        ctx?.getSharedPreferences(PREFS,Context.MODE_PRIVATE)?.edit()?.putBoolean("map_buildings",value)?.apply()
        onBuildingCapability()
    }
    /** v7.13 3D 楼块开关：保存后立刻下发；重连后 onLinkReady 会再推一次，保证和圆屏一致 */
    fun setBuilding3d(value: Boolean) {
        building3dEnabled=value
        ctx?.getSharedPreferences(PREFS,Context.MODE_PRIVATE)?.edit()?.putBoolean("map_bld3d",value)?.apply()
        BleLink.send(if (value) "#B3D,1" else "#B3D,0")
    }
    fun onBuildingCapability() {
        if(source!=SRC_AMAP || routeOnly())return
        clearOfflineRoute();requestSerial++;fetching=false
        haveChunk=false;lastFetchFail=0L
        val context=ctx
        if(context!=null && preparedRouteCoords.size>=2)prepareRoute(context,preparedRouteKey,preparedRouteCoords)
    }
    private var routeProgress = -1.0
    fun offlineStatus(): String {
        val status=offlineMaps?.status ?: ""
        return if (source==SRC_AMAP) (if (status.isEmpty()) "高德楼块实验" else status)+"\n"+(if(!buildingEnabled) "俯视楼块已关闭" else if(BleLink.buildingsSupported) (if(BleLink.buildingsHeights) (if(building3dEnabled) "圆屏支持楼块（3D 显示）" else "圆屏支持楼块（当前俯视 2D）") else "圆屏支持俯视楼块") else "旧固件按普通地图显示；楼块需实验固件")+"\n"+AmapSdkRaster.state else status
    }

    /** 规划页选定路线后就开始下载，不必等车辆出发。 */
    fun prepareRoute(context: Context, key: String, coords: List<DoubleArray>) {
        load(context)
        if (coords.size < 2) return
        if (frame == null) {
            val lat=coords[0][0]; val lng=coords[0][1]
            val la=Math.round(lat*10)/10.0; val lo=Math.round(lng*10)/10.0
            val id=((Math.round(la*10)*7919+Math.round(lo*10)*104729) and 0x7FFF).toInt() or 1
            frame=MapGeo.Frame(la,lo,id)
        }
        preparedRouteKey=key;preparedRouteCoords=coords.map { it.clone() }
        if (!enabled || source == SRC_OSM || satMode() || routeOnly()) return
        if (offlineKey == key && offlineMaps != null) return
        val f=frame ?: return
        val xy=DoubleArray(coords.size*2)
        for (i in coords.indices) { xy[2*i]=f.x(coords[i][1]); xy[2*i+1]=f.y(coords[i][0]) }
        offlineMaps?.close(); offlineMaps=null; offlineKey=""; routeProgress=-1.0
        try {
            val plan=RouteMapPlan(xy)
            offlineKey=key
            offlineMaps=RouteOfflineMaps(context.applicationContext,f,plan,
                foregroundBusy={ fetching || !enabled }, onReady={ node ->
                    handler.post {
                        if (offlineKey!=key || offlineMaps?.plan!==plan || !enabled || routeOnly() || fetching || (haveChunk && !chunkAcked)) return@post
                        val progress=if (havePose) plan.progress(poseX,poseY,routeProgress) else 0.0
                        val onRoute=plan.at(progress)
                        if (havePose && kotlin.math.hypot(poseX-onRoute.x,poseY-onRoute.y)>=400) return@post
                        val wanted=plan.nodes[plan.target(progress,!haveConfirmedChunk || !havePose || poseKmh<4)]
                        if (wanted!==node || (haveChunk && chunkType==TYPE_RASTER &&
                            kotlin.math.abs(chunkCx-node.x)<1 && kotlin.math.abs(chunkCy-node.y)<1 &&
                            (!buildingMode() || chunkBuildingsComplete || offlineMaps?.read(node)==null))) return@post
                        fetchChunk(f,node.x,node.y,node)
                    }
                }, useAmap=source==SRC_AMAP,buildings=buildingMode())
            offlineMaps?.start()
        } catch (e: Exception) { status="沿线离线地图准备失败：${e.message}" }
    }

    fun screenMapStatus(): String {
        val maps=offlineMaps ?: return ""
        if (!BleLink.isConnected) return "圆屏未连接"
        val first=maps.plan.nodes.first()
        return if (haveConfirmedChunk && confirmedSat==false &&
            maxOf(kotlin.math.abs(first.x-confirmedCx),kotlin.math.abs(first.y-confirmedCy))<500)
            "圆屏起点底图已就绪" else "圆屏起点底图准备中"
    }

    private fun clearOfflineRoute() { offlineMaps?.close(); offlineMaps=null; offlineKey=""; routeProgress=-1.0 }
    private var baseBuildSeconds = 4.0
    private var baseDeliverySeconds = 1.0
    private var poseX = 0.0
    private var poseY = 0.0
    private var poseKmh = 0
    private var havePose = false

    private fun measuredPreparationSeconds(): Double {
        val rate = BleLink.lastRateKBs.takeIf { it > 0 } ?: 12.0
        val transfer = (chunkBaseBytes?.size ?: 64_000) / 1024.0 / rate
        return baseBuildSeconds + maxOf(transfer, baseDeliverySeconds)
    }

    private fun basePreparationSeconds(): Double {
        val measured = measuredPreparationSeconds()
        // 没有沿线离线包的巡航模式，仍按实测耗时留出准备余量。
        return maxOf(8.0, measured*1.5+3.0)
    }

    // 当前路线
    private var routeKey = ""
    private var routeId = 0
    private var routeBytes: ByteArray? = null
    private var routeAcked = false
    private var routeSentAt = 0L
    // v4.5 原路线
    private var ghostCoords: List<DoubleArray>? = null    // [纬度, 经度]，还没有坐标系时先存着
    private var ghostId = 0
    private var ghostBytes: ByteArray? = null
    private var ghostAcked = false
    private var ghostSentAt = 0L

    private fun now() = SystemClock.elapsedRealtime()

    fun load(context: Context) {
        if (ctx != null) return
        ctx = context.applicationContext
        val sp = ctx!!.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        enabled = sp.getBoolean("map_on", true)
        buildingEnabled=sp.getBoolean("map_buildings",true)
        building3dEnabled=sp.getBoolean("map_bld3d",true)
        source = sp.getInt("map_src", SRC_AUTO)
        // 试用版首次覆盖安装时选高德；保留原来源，用户之后的选择不再被覆盖。
        if (!sp.getBoolean("amap_trial_837",false)) {
            sp.edit().putInt("amap_trial_previous_source",source).putBoolean("amap_trial_837",true)
                .putInt("map_src",SRC_AMAP).apply()
            source=SRC_AMAP
        }
        theme = sp.getInt("map_theme", 1)
        nightMode = sp.getInt("map_night", NIGHT_AUTO)
        trail = sp.getInt("map_trail", TRAIL_GREY)
        clean = sp.getBoolean("clean_mode", false)
        curve = sp.getBoolean("map_curve", false)
        screenRot = sp.getInt("screen_rot", 0) and 3
        screenDeg = sp.getInt("screen_deg", screenRot * 90)
        leadMs = sp.getInt("lead_ms", 300)
        progress = sp.getBoolean("trip_prog", true)
        if (!MapStylePolicy.isValid(theme)) theme = 1
    }

    /** v5.9 弯道急缓上色（v6.1 起 "#Q,1/0"；原来的 "#q" 和屏幕数据线速度撞了） */
    fun setCurve(on: Boolean) {
        curve = on
        ctx?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.edit()?.putBoolean("map_curve", on)?.apply()
        BleLink.send(if (on) "#Q,1" else "#Q,0")
    }

    // v6.1 圆屏方向：0 / 1 / 2 / 3 = 0° / 90° / 180° / 270°（"#R,n"，圆屏会记住）
    @Volatile var screenRot = 0
        private set
    fun setScreenRot(r: Int) = setScreenDeg((r and 3) * 90, true)

    /**
     * v6.3 圆屏方向任意角度（0~359，顺时针）。先发 "#R,最近的90°档"（旧固件按这个转），
     * 再发 "#D角度"（固件 v6.4 起按任意角度转；故意不带逗号，旧固件会忽略）。
     * save = false：拖动圆盘过程中，只发给圆屏不存手机设置
     */
    @Volatile var screenDeg = 0
        private set
    fun setScreenDeg(d: Int, save: Boolean) {
        screenDeg = ((d % 360) + 360) % 360
        screenRot = ((screenDeg + 45) / 90) % 4
        if (save) ctx?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.edit()
            ?.putInt("screen_rot", screenRot)?.putInt("screen_deg", screenDeg)?.apply()
        BleLink.send("#R,$screenRot")
        BleLink.send("#D$screenDeg")
    }

    /**
     * v7.2 预判提前量（毫秒，0 = 关）：圆屏的小地图和手机算的距离都显示“再过这么一点时间”的样子，
     * 抵消 GPS 定位本身的滞后和蓝牙传输时间，看起来不再“慢半拍”。"#L毫秒"（固件 v6.7 起；不带逗号，旧固件忽略）
     */
    @Volatile var leadMs = 300
        private set
    fun setLead(ms: Int) {
        leadMs = ms.coerceIn(0, 800)
        ctx?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.edit()?.putInt("lead_ms", leadMs)?.apply()
        BleLink.send("#L$leadMs")
    }

    /** 现在要不要下卫星图：选了卫星图风格就用天地图卫星照片（v5.9.1 不管地图来源选的是什么） */
    private fun satMode() = theme == THEME_SAT

    /** v8.0 圆屏左边沿的路程进度条（"#M1" / "#M0"，固件 v7.3 起；不带逗号，旧固件忽略） */
    @Volatile var progress = true
        private set
    fun setProgress(on: Boolean) {
        progress = on
        ctx?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.edit()?.putBoolean("trip_prog", on)?.apply()
        BleLink.send(if (on) "#M1" else "#M0")
    }

    /** v4.8 清爽模式开关（"#w,1/0"） */
    fun setClean(on: Boolean) {
        clean = on
        ctx?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.edit()?.putBoolean("clean_mode", on)?.apply()
        BleLink.send(if (on) "#w,1" else "#w,0")
    }

    fun setNightMode(m: Int) {
        nightMode = m
        ctx?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.edit()?.putInt("map_night", m)?.apply()
        applyNight()
    }

    /** 算出现在该不该用夜间配色，变了就告诉圆屏（亮度检查时每分钟调用一次） */
    fun applyNight() {
        val n = when (nightMode) {
            NIGHT_ON -> true
            NIGHT_OFF -> false
            else -> !Brightness.isDaytimeNow()
        }
        nightNow = n
        val v = if (n) 1 else 0
        if (v != nightSent && BleLink.isConnected) {
            nightSent = v
            BleLink.send("#n,$v")
        }
    }

    fun setTrail(t: Int) {
        trail = t
        ctx?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.edit()?.putInt("map_trail", t)?.apply()
        BleLink.send("#r,$t")
    }

    fun setTheme(t: Int) {
        if (!MapStylePolicy.isValid(t)) return
        val wasSat = satMode()
        val wasRouteOnly = routeOnly()
        theme = t
        AmapCaptureHost.refresh()
        ctx?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.edit()?.putInt("map_theme", t)?.apply()
        BleLink.send("#t,$t")
        if (wasSat != satMode() || wasRouteOnly != routeOnly()) {
            clearOfflineRoute()
            requestSerial++ // In-flight captures/results must never install an old basemap.
            fetching = false
            haveChunk = false; haveConfirmedChunk = false; lastFetchFail = 0L
            chunkBytes = null; chunkBaseBytes = null; chunkAcked = true
            BleLink.cancelBulk(TYPE_CHUNK, TYPE_RASTER)
            if (routeOnly()) {
                BleLink.cancelBulk(TYPE_GHOST)
                status = "龙珠雷达：仅传输导航路线与位置，无底图"
            } else {
                sendGhost()
                ctx?.let { if (preparedRouteCoords.size >= 2) prepareRoute(it,preparedRouteKey,preparedRouteCoords) }
            }
        }
    }

    fun themeName(t: Int = theme) = when (t) {
        0 -> "原版"; 2 -> "GTA5"; 3 -> "赛博霓虹"; 4 -> "复古纸地图"; 5 -> "极简黑白"; 6 -> "蓝图"
        7 -> "掌机像素"; 8 -> "雷达"; 9 -> "寻宝罗盘"; 10 -> "卫星图"; THEME_DRAGON -> "龙珠雷达"; else -> "罪恶都市"
    }

    fun themeDesc(t: Int = theme) = when (t) {
        0 -> "浅色地面、深色道路"
        2 -> "深色地面、浅色道路、紫色路线"
        3 -> "黑底青色道路、品红路线，夜里最酷"
        4 -> "米黄纸色，白天太阳下最清楚；夜间配色时自动换成霓虹"
        5 -> "黑底灰路，只有路线是白色，最不分心"
        6 -> "深蓝底 + 跟着地图走的网格"
        7 -> "老掌机的 4 色绿屏，大像素，帧率更高"
        8 -> "墨绿单色 + 转动的扫描线和距离圈"
        9 -> "六边形网格、罗盘刻度、目的地菱形脉冲"
        10 -> "天地图卫星照片（数据大，范围小一些）"
        THEME_DRAGON -> "绿色十字网格，无底图，仅导航路线；四星球标记终点（不支持低性能主控）"
        else -> "暖沙色街区、粉紫路线（默认）"
    }

    fun setSource(src: Int) {
        clearOfflineRoute()
        source = src
        AmapCaptureHost.refresh()
        requestSerial++
        fetching = false
        ctx?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.edit()?.putInt("map_src", src)?.apply()
        handler.post { haveChunk = false; lastFetchFail = 0L }      // 下次定位时按新来源重新下载
    }

    /** v6.2 在 App 里改了 key：马上按新 key 重新下载地图（不用等 15 秒的失败冷却） */
    fun onKeysChanged() {
        clearOfflineRoute()
        requestSerial++
        fetching = false
        handler.post { haveChunk = false; lastFetchFail = 0L }
    }

    fun sourceName(src: Int = source) = when (src) { SRC_TDT -> "天地图"; SRC_OSM -> "OpenStreetMap"; SRC_AMAP -> "高德无字（试用）"; else -> "自动" }

    fun setEnabled(on: Boolean) {
        enabled = on
        AmapCaptureHost.refresh()
        ctx?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.edit()?.putBoolean("map_on", on)?.apply()
        BleLink.send(if (on) "#m,1" else "#m,0")
    }

    /** 新的一次导航：路线清空（地图块保留，位置附近的地图还能用） */
    fun onNaviStart() {
        handler.post { routeKey = "" }
    }

    // ================= 位置 =================

    private var lastBearing: Float? = null       // v4.3 最后一次正常行驶时的朝向

    /** 每次定位调用（主线程）：lat/lng 为高德坐标，bearing 为朝向（度，正北 = 0，顺时针） */
    fun onLocation(lat: Double, lng: Double, bearing: Float, kmh: Int) {
        var f = frame
        if (f == null) {
            // v2.3 坐标原点取所在区域（0.1 度网格）的固定点，编号也由它决定：App 重启后保持不变
            val la = Math.round(lat * 10) / 10.0; val lo = Math.round(lng * 10) / 10.0
            val id = ((Math.round(la * 10) * 7919 + Math.round(lo * 10) * 104729) and 0x7FFF).toInt() or 1
            f = MapGeo.Frame(la, lo, id)
            frame = f
        }
        val x = f.x(lng); val y = f.y(lat)
        poseX = x; poseY = y; poseKmh = kmh; havePose = true
        // v4.3 停车（车速很低）时 GPS 朝向会乱跳，圆屏地图跟着乱转：沿用最后一次正常行驶时的朝向
        val hdg = if (kmh >= 4 || lastBearing == null) bearing.also { if (kmh >= 4) lastBearing = it } else lastBearing!!
        BleLink.send("#p,${f.id},${(x * 10).roundToInt()},${(y * 10).roundToInt()},${hdg.roundToInt()},$kmh")
        if (ghostCoords != null && ghostBytes == null) encodeGhost()     // v4.5 有坐标系了，原路线可以发了
        if (!enabled) return

        if (routeOnly()) { checkResend(); return }

        // 需要新的地图块？
        val cx = if (haveConfirmedChunk) confirmedCx else chunkCx
        val cy = if (haveConfirmedChunk) confirmedCy else chunkCy
        val sat = if (haveConfirmedChunk) confirmedSat else chunkSat
        val half = if (haveConfirmedChunk) confirmedHalf else chunkHalf
        val offline=offlineMaps?.takeIf { !satMode() && source!=SRC_OSM }
        var target: RouteMapPlan.Node? = null
        if (offline!=null) {
            routeProgress=offline.plan.progress(x,y,routeProgress)
            val onRoute=offline.plan.at(routeProgress)
            val index=offline.plan.target(routeProgress,!haveConfirmedChunk || kmh<4)
            offline.update(index)
            if (kotlin.math.hypot(x-onRoute.x,y-onRoute.y)<400) target=offline.plan.nodes[index]
        }
        val wanted=target
        val needs=if (wanted!=null) !haveChunk || chunkType!=TYPE_RASTER ||
            kotlin.math.abs(wanted.x-chunkCx)>1 || kotlin.math.abs(wanted.y-chunkCy)>1
            else !haveChunk || MapCoveragePolicy.shouldRefresh(x-cx,y-cy,bearing,kmh,sat,basePreparationSeconds(),half)
        // 已发出的普通图必须完整确认；下载后续地图不能冲掉正在传输的数据。
        if (!fetching && (!haveChunk || chunkAcked) && needs && (lastFetchFail==0L || now()-lastFetchFail>5000)) {
            val a=Math.toRadians(bearing.toDouble())
            val ahead=if (kmh>5) (if (satMode()) SAT_AHEAD else AHEAD) else 0.0
            fetchChunk(f,wanted?.x ?: (x+sin(a)*ahead),wanted?.y ?: (y+cos(a)*ahead),wanted)
        }
        checkResend()
    }

    // ================= 地图块 =================

    private fun fetchChunk(f: MapGeo.Frame, cx: Double, cy: Double, offlineNode: RouteMapPlan.Node? = null) {
        if (routeOnly() || !enabled) return
        fetching = true
        val startedAt = now()
        val serial = ++requestSerial
        status = "正在下载附近地图…"
        val m = HALF + 150
        // 自动来源退回 OSM 时仍用原来的范围及前移量，不能沿用高速大图的中心。
        val fallbackCx = cx
        val fallbackCy = cy
        // 方框四角（高德坐标）→ WGS-84，给 OpenStreetMap 查询用
        val sw = MapGeo.gcjToWgs(f.lat(fallbackCy - m), f.lng(fallbackCx - m))
        val ne = MapGeo.gcjToWgs(f.lat(fallbackCy + m), f.lng(fallbackCx + m))
        val src = source
        val c = ctx
        val sat = satMode()
        val rasterHalf = HALF
        val mpp = TdtRaster.MPP
        val zoom = 16
        val offline=offlineMaps
        val knownBuildings=if(src==SRC_AMAP && buildingMode() && !chunkSat)chunkBytes else null
        worker.execute {
          if (serial != requestSerial || routeOnly()) return@execute
          try {
            var bytes: ByteArray? = null
            var type = TYPE_CHUNK
            var err = ""
            var info = ""
            var compactVector: ByteArray? = null
            var preparedBase: ByteArray? = null
            // 0. v5.9 卫星图
            if (sat && c != null) {
                try {
                    val r = TdtRaster.buildSat(c, f, cx, cy, SAT_HALF, shouldCancel={ serial!=requestSerial || routeOnly() })
                    bytes = r.bytes; type = TYPE_RASTER
                    if (r.previewRaw!=null) { previewRaw = r.previewRaw; previewCls = r.previewCls; previewVer++ }
                    info = "天地图卫星图 ${r.tiles} 张瓦片（缓存 ${r.fromCache}），${r.bytes.size / 1024} KB"
                } catch (e: Exception) {
                    err = "卫星图：" + (e.message ?: e.javaClass.simpleName)
                }
            }
            // 1. 天地图（自动 / 天地图模式）
            if (serial == requestSerial && !routeOnly() && bytes == null && src != SRC_OSM && c != null && (!sat || src!=SRC_AMAP)) {
                try {
                    val cached=if (offlineNode!=null) offline?.read(offlineNode) ?: offline?.readBase(offlineNode) else null
                    val r = if (cached!=null) TdtRaster.Result(cached,0,0,null,null) else
                        if (src==SRC_AMAP) AmapSdkRaster.build(f,cx,cy,rasterHalf,makePreview=true,
                            shouldCancel={ serial!=requestSerial },includeBuildings=buildingMode())
                        else TdtRaster.build(c, f, cx, cy, rasterHalf, vice=false, mpp=mpp, zoom=zoom, makePreview=true,
                            shouldCancel={ serial!=requestSerial || routeOnly() })
                    // 缓存只保存这块真正完整的取图；旧楼块拼到普通图不冒充完成。
                    if (offlineNode!=null && cached==null) try { offline?.store(offlineNode,r.bytes) } catch (_: Exception) { }
                    val carried=if(src==SRC_AMAP && buildingMode())BuildingContinuity.merge(r.bytes,knownBuildings)else null
                    val base=carried?.bytes ?: r.bytes
                    run {
                        preparedBase = base
                        val buildSeconds = (now() - startedAt) / 1000.0
                        handler.post {
                            if (serial != requestSerial || src != source || sat != satMode() || routeOnly()) return@post
                            baseBuildSeconds = buildSeconds.coerceIn(0.2, 60.0)
                            baseInstalledSerial = serial
                            haveChunk = true; chunkSat = false
                            chunkCx = cx; chunkCy = cy; chunkType = TYPE_RASTER
                            chunkHalf = rasterHalf
                            chunkBaseBytes = base; chunkBytes = base
                            compactVectorBytes = null; compactRasterBytes = null
                            chunkFallbackLevel = 0; chunkRetryCount = 0; fallbackPreparing = false; transferLabel = ""
                            chunkBuildingsComplete=carried?.complete ?: RasterPacket.isBuilding(base)
                            chunkId = (chunkId + 1) and 0xFF; chunkAcked = false
                            chunkInfo = "地图块 #$chunkId：${if (src==SRC_AMAP) "高德无字" else "天地图"}${if(chunkBuildingsComplete) "楼块底图" else "普通底图"}，${base.size / 1024} KB，覆盖 2.4 公里" + (if (cached!=null) "（离线包）" else "") + (if((carried?.retained ?: 0)>0) "（保留重叠楼块）" else "")
                            status = "普通底图已生成，正在传到圆屏"
                            sendChunk()
                        }
                    }
                    bytes = r.bytes; type = TYPE_RASTER
                    if (r.previewRaw!=null) { previewRaw = r.previewRaw; previewCls = r.previewCls; previewVer++ }
                    info = if (src==SRC_AMAP) "高德 SDK 无字底图，${r.bytes.size/1024} KB" else "天地图 ${r.tiles} 张瓦片（缓存 ${r.fromCache}），${r.bytes.size / 1024} KB"
                } catch (e: Exception) {
                    err = (if (src==SRC_AMAP) "高德：" else "天地图：") + (e.message ?: e.javaClass.simpleName)
                    val fallback = preparedBase
                    if (fallback != null) { bytes = fallback; type = TYPE_RASTER; info = "天地图普通底图，平滑暂不可用" }
                }
            }
            // 2. OpenStreetMap（OSM 模式，或自动模式下天地图失败）
            if (serial == requestSerial && !routeOnly() && bytes == null && src != SRC_TDT && src!=SRC_AMAP) {
                try {
                    val json = OsmFetch.query(sw[0], sw[1], ne[0], ne[1])
                    val feats = OsmFetch.parse(json, f)
                    bytes = MapGeo.encodeChunk(f.id, fallbackCx, fallbackCy, HALF, feats, MAX_BYTES, MAX_FEAT)
                    compactVector = MapGeo.encodeChunk(f.id, fallbackCx, fallbackCy, 450.0, feats, 15_000, 350)
                    type = TYPE_CHUNK
                    info = "OpenStreetMap ${feats.size} 个要素，${bytes.size / 1024} KB"
                } catch (e: Exception) {
                    err += (if (err.isNotEmpty()) "；" else "") + "OSM：" + (e.message ?: e.javaClass.simpleName)
                }
            }
            handler.post {
                if (serial != requestSerial || src != source || sat != satMode() || routeOnly()) return@post
                fetching = false
                if (baseInstalledSerial==serial) {
                    if (chunkAcked) status="圆屏已收到普通底图"
                    return@post
                }
                val b = bytes
                if (b == null) {
                    lastFetchFail = now()
                    status = "地图下载失败（5 秒后重试）：$err"
                    return@post
                }
                haveChunk = true
                chunkSat = sat && info.startsWith("天地图卫星图")
                chunkCx = if (type == TYPE_CHUNK) fallbackCx else cx
                chunkCy = if (type == TYPE_CHUNK) fallbackCy else cy
                chunkId = (chunkId + 1) and 0xFF
                chunkBytes = b
                compactVectorBytes = compactVector
                compactRasterBytes = null
                chunkFallbackLevel = 0
                chunkRetryCount = 0
                fallbackPreparing = false
                transferLabel = ""
                chunkBaseBytes = b
                chunkBuildingsComplete=false
                baseBuildSeconds = ((now() - startedAt) / 1000.0).coerceIn(0.2, 60.0)
                chunkType = type
                chunkHalf = if (chunkSat) SAT_HALF else HALF
                chunkAcked = false
                status = "地图已下载" + (if (err.isNotEmpty()) "（$err）" else "")
                chunkInfo = "地图块 #$chunkId：$info"
                sendChunk()
            }
          } catch (_: OutOfMemoryError) {
            handler.post { if (serial == requestSerial && !routeOnly()) { fetching=false; lastFetchFail=now(); status="手机内存不足，保留圆屏当前底图，稍后重试" } }
          }
        }
    }

    private fun sendChunk() {
        val b = chunkBytes ?: return
        if (!enabled || routeOnly()) return
        if(chunkType==TYPE_RASTER && smallDisplayRasterCap>0 && compactRasterBytes==null && chunkFallbackLevel==0 && b.size>smallDisplayRasterCap) {
            if(fallbackPreparing) return
            fallbackPreparing=true
            val id=chunkId; val cap=smallDisplayRasterCap
            worker.execute {
                val compact=RasterPacket.compactForSmallDisplay(b,cap,128,chunkCx,chunkCy)
                    ?: RasterPacket.compactForSmallDisplay(b,cap,64,chunkCx,chunkCy)
                handler.post {
                    fallbackPreparing=false
                    if(id!=chunkId || chunkAcked || smallDisplayRasterCap==0) return@post
                    if(compact!=null) {
                        compactRasterBytes=compact
                        chunkFallbackLevel=1
                        val w=u16(compact,10); val h=u16(compact,12); val mm=u16(compact,14) and 0x7fff
                        transferLabel="C3 中心兼容图 ${w}×${h}，覆盖 ${w*mm/1000} 米，${compact.size} B"
                        chunkInfo=transferLabel
                        sendChunk()
                    } else {
                        status="兼容地图压缩失败，改发原始地图"
                        smallDisplayRasterCap=0
                        sendChunk()
                    }
                }
            }
            status="正在生成低内存固件兼容中心地图…"
            return
        }
        val data = compactRasterBytes ?: if (chunkFallbackLevel > 0 && chunkType == TYPE_CHUNK) compactVectorBytes else null
        val outgoing = data ?: b
        deliveryBytes = outgoing.size
        deliveryCx = chunkCx; deliveryCy = chunkCy; deliveryHalf = chunkHalf
        if (chunkType == TYPE_RASTER && data != null && outgoing.size >= 16) {
            val w = u16(outgoing,10); val h = u16(outgoing,12); val mm = u16(outgoing,14) and 0x7fff
            deliveryCx = i32(outgoing,2) + w * mm / 2000.0
            deliveryCy = i32(outgoing,6) - h * mm / 2000.0
            deliveryHalf = minOf(w,h) * mm / 2000.0
        } else if (chunkType == TYPE_CHUNK && data != null) {
            deliveryHalf = 450.0
        }
        chunkSentAt = now()
        if (chunkDeliveryId != chunkId) { chunkDeliveryId = chunkId; chunkDeliveryStart = chunkSentAt }
        BleLink.sendBulk(chunkType, chunkId, if(chunkType==TYPE_RASTER && !buildingMode()) RasterPacket.forLegacy(outgoing) else outgoing)
        if (chunkFallbackLevel > 0) status = "$transferLabel，正在发送兼容地图"
        else status = "完整地图已交给蓝牙发送队列，等待圆屏确认"
    }

    private fun u16(b: ByteArray,p: Int)=(b[p].toInt() and 255) or ((b[p+1].toInt() and 255) shl 8)
    private fun i32(b: ByteArray,p: Int)=u16(b,p) or (u16(b,p+2) shl 16)

    private fun retryChunkForCompatibility() {
        if (fallbackPreparing || chunkAcked) return
        val original=chunkBytes ?: return
        if (chunkFallbackLevel==0 && chunkType==TYPE_CHUNK && compactVectorBytes!=null) {
            chunkFallbackLevel=1
            transferLabel="旧固件兼容：中心 900 米矢量地图，${compactVectorBytes!!.size/1024} KB"
            sendChunk()
            return
        }
        if (chunkType==TYPE_RASTER && chunkFallbackLevel<2) {
            // Only a device that explicitly advertises the C3 small-display
            // capability may receive a cropped 128/64 px fallback. A missing
            // ACK on 185B must retry/stop, never replace its full 1200 px map.
            if (smallDisplayRasterCap <= 0) {
                status="完整地图暂未确认，保留完整地图等待重试"
                chunkSentAt=Long.MAX_VALUE
                return
            }
            fallbackPreparing=true
            val id=chunkId
            val targetX=if(havePose)poseX else chunkCx
            val targetY=if(havePose)poseY else chunkCy
            val level=chunkFallbackLevel
            status="设备尚未确认完整图片地图，正在生成兼容中心地图…"
            worker.execute {
                val wide=if(level==0)RasterPacket.compactForSmallDisplay(original,8*1024,128,targetX,targetY)else null
                val compact=wide ?: RasterPacket.compactForSmallDisplay(original,8*1024,64,targetX,targetY)
                val selected=if(wide!=null)1 else if(compact!=null)2 else 0
                handler.post {
                    fallbackPreparing=false
                    if(chunkId!=id || chunkAcked) return@post
                    if(compact==null) {
                        status="旧固件兼容地图生成失败：地图数据格式无法裁剪"
                        chunkSentAt=Long.MAX_VALUE
                        return@post
                    }
                    compactRasterBytes=compact
                    chunkFallbackLevel=selected
                    val w=u16(compact,10);val mm=u16(compact,14) and 0x7fff
                    transferLabel="旧固件兼容：中心 ${w*mm/1000} 米图片地图，${compact.size/1024} KB"
                    sendChunk()
                }
            }
            return
        }
        if (chunkRetryCount < 2) {
            chunkRetryCount++
            sendChunk()
            return
        }
        val sent=BleLink.bulkTransferStatus(chunkType,chunkId)
        status="圆屏仍未确认地图（${sent?.submitted ?: 0}/${sent?.parts ?: 0} 个蓝牙分片已提交；已尝试完整图和兼容图），将保留当前屏幕内容"
        chunkSentAt=Long.MAX_VALUE
    }

    // ================= 路线 =================

    /** 每次收到导航信息时调用（主线程），路线变了（新规划/偏航重算）就重新发送 */
    fun onRoute(pathKey: String, coords: List<DoubleArray>, lights: List<DoubleArray> = emptyList()): Boolean {
        ctx?.let { prepareRoute(it,pathKey,coords) }
        val f = frame ?: return false             // 还没有定位，稍后再发
        if (pathKey == routeKey) return true
        if (coords.size < 2) return false
        routeKey = pathKey
        val pts = DoubleArray(coords.size * 2)
        for (i in coords.indices) { pts[2 * i] = f.x(coords[i][1]); pts[2 * i + 1] = f.y(coords[i][0]) }
        worker.execute {
            // v3.3 红绿灯位置（高德坐标 → 平面坐标）一起打包
            val lp = DoubleArray(lights.size * 2)
            for (i in lights.indices) { lp[2 * i] = f.x(lights[i][1]); lp[2 * i + 1] = f.y(lights[i][0]) }
            val b = MapGeo.encodeRoute(f.id, pts, 20000, lp)
            handler.post {
                routeId = (routeId + 1) and 0xFF
                routeBytes = b
                routeAcked = false
                routeInfo = "路线 #$routeId：${coords.size} 个点、${lights.size} 个红绿灯 → ${b.size / 1024} KB"
                sendRoute()
            }
        }
        return true
    }

    private fun sendRoute() {
        val b = routeBytes ?: return
        if (!enabled) return
        routeSentAt = now()
        BleLink.sendBulk(TYPE_ROUTE, routeId, b)
    }

    // ================= v4.5 原路线 =================

    /** 导航开始时调用（主线程）：把导入的分享路线发给圆屏画成细线；null = 不显示 */
    fun setGhost(coords: List<DoubleArray>?) {
        handler.post {
            ghostCoords = coords
            ghostBytes = null; ghostAcked = false
            if (coords == null) { BleLink.send("#o"); return@post }
            encodeGhost()
        }
    }

    private fun encodeGhost() {
        val f = frame ?: return
        val c = ghostCoords ?: return
        if (c.size < 2) return
        val pts = DoubleArray(c.size * 2)
        for (i in c.indices) { pts[2 * i] = f.x(c[i][1]); pts[2 * i + 1] = f.y(c[i][0]) }
        ghostBytes = MapGeo.encodeRoute(f.id, pts, 5000)
        ghostId = (ghostId + 1) and 0xFF
        ghostAcked = false
        sendGhost()
    }

    private fun sendGhost() {
        val b = ghostBytes ?: return
        if (!enabled || routeOnly()) return
        ghostSentAt = now()
        BleLink.sendBulk(TYPE_GHOST, ghostId, b)
    }

    // ================= 确认与重发 =================

    /** 圆屏回复 "K,类型,编号"：这一份收齐了（蓝牙线程调用） */
    fun onAck(type: Int, id: Int) {
        handler.post {
            if (!routeOnly() && type == chunkType && id == chunkId) {
                if (!chunkAcked) baseDeliverySeconds = ((now() - chunkDeliveryStart) / 1000.0).coerceIn(0.2, 60.0)
                chunkAcked = true
                chunkCx=deliveryCx; chunkCy=deliveryCy; chunkHalf=deliveryHalf
                haveConfirmedChunk = true; confirmedCx = deliveryCx; confirmedCy = deliveryCy; confirmedSat = chunkSat
                confirmedHalf = deliveryHalf
                status = "圆屏已收到普通底图"
                // 规划页停车等待时也能接着准备起点底图，不依赖下一次行驶定位。
                val maps=offlineMaps
                if (maps!=null && enabled && !fetching) {
                    val progress=if (havePose) maps.plan.progress(poseX,poseY,routeProgress) else 0.0
                    val point=maps.plan.at(progress)
                    val node=maps.plan.nodes[maps.plan.target(progress,!havePose || poseKmh<4)]
                    if ((!havePose || kotlin.math.hypot(poseX-point.x,poseY-point.y)<400) &&
                        (kotlin.math.abs(node.x-chunkCx)>1 || kotlin.math.abs(node.y-chunkCy)>1 ||
                            (buildingMode() && !chunkBuildingsComplete && maps.read(node)!=null)))
                        fetchChunk(maps.frame,node.x,node.y,node)
                }
            }
            if (type == TYPE_ROUTE && id == routeId) routeAcked = true
            if (type == TYPE_GHOST && id == ghostId) ghostAcked = true
        }
    }

    /** 蓝牙重新连上：圆屏可能重启过，全部重发 */
    fun onLinkReady() {
        BleLink.sendEvent("#GB?")
        handler.post {
            BleLink.send(if (building3dEnabled) "#B3D,1" else "#B3D,0")   // v7.13 3D 楼块开关（旧固件忽略）
            BleLink.send(if (enabled) "#m,1" else "#m,0")
            BleLink.send("#t,$theme")                    // v2.5 配色主题
            BleLink.send("#r,$trail")                    // v2.8 走过的路线
            BleLink.send(if (clean) "#w,1" else "#w,0")   // v4.8 清爽模式
            BleLink.send(if (curve) "#Q,1" else "#Q,0")   // v5.9 弯道急缓上色（v6.1 改用 #Q）
            BleLink.send("#R,$screenRot")                // v6.1 圆屏方向
            BleLink.send("#D$screenDeg")                 // v6.3 任意角度（固件 v6.4）
            BleLink.send("#L$leadMs")                    // v7.2 预判提前量（固件 v6.7）
            BleLink.send(if (progress) "#M1" else "#M0")  // v8.0 路程进度条（固件 v7.3）
            nightSent = -1; applyNight()                 // v2.8 夜间配色
            haveConfirmedChunk = false
            chunkDeliveryId = -1
            chunkAcked = false; routeAcked = false; ghostAcked = false
            sendChunk(); sendRoute(); sendGhost()
        }
    }

    private fun checkResend() {
        if (!BleLink.isConnected || BleLink.bulkPending()) return
        if (chunkBytes != null && !chunkAcked && !fallbackPreparing && now() - chunkSentAt > RESEND_MS) retryChunkForCompatibility()
        if (routeBytes != null && !routeAcked && now() - routeSentAt > RESEND_MS) sendRoute()
        if (ghostBytes != null && !ghostAcked && now() - ghostSentAt > RESEND_MS) sendGhost()
    }

    /** v3.4 高德坐标 → 小地图平面坐标（米）；还没有坐标系时返回 null */
    fun toFrame(lat: Double, lng: Double): IntArray? = frame?.let { intArrayOf(it.x(lng).roundToInt(), it.y(lat).roundToInt()) }
    fun frameId(): Int? = frame?.id

    private fun transferDetail(): String {
        if (!haveChunk || chunkAcked || chunkBytes==null) return ""
        if (fallbackPreparing) return status
        if (chunkSentAt==Long.MAX_VALUE) return status
        BleLink.bulkTransferError(chunkType,chunkId)?.let { return "地图暂未发送：$it；设备固件没有收到地图" }
        val t=BleLink.bulkTransferStatus(chunkType,chunkId)
        if (t==null) return "地图已生成 ${deliveryBytes/1024} KB；尚未提交蓝牙分片，检查连接和 MTU 协商"
        val pct=if(t.parts==0)0 else t.submitted*100/t.parts
        val phase=if(t.queued>0) "手机已提交蓝牙 ${t.submitted}/${t.parts} 片（$pct%），队列剩余 ${t.queued} 片" else
            "全部 ${t.parts} 片（${t.bytes/1024} KB）已提交蓝牙，等待圆屏接收、解析并确认"
        val attempt="第 ${chunkRetryCount+1} 次发送"
        val fallback=if(transferLabel.isNotEmpty()) "；$transferLabel" else "；首选完整地图，超时后自动尝试低性能主控兼容地图"
        return "$phase；$attempt；已等待 ${(now()-chunkSentAt).coerceAtLeast(0)/1000} 秒$fallback"
    }

    // A usable connected link is sufficient for map delivery. Some Android
    // versions miss the standalone U,1 notification even though navigation
    // and map packets are already accepted by the screen.
    fun loadingStatus(): String = MapLoadStatus.text(enabled,routeOnly(),BleLink.isConnected,
        BleLink.verified || BleLink.isConnected,
        frame!=null,haveChunk && chunkBytes!=null && !chunkAcked,
        BleLink.bulkProgress(chunkType,chunkId) ?: -1,fetching,
        lastFetchFail>0 && (status.contains("失败") || status.contains("内存不足")),haveConfirmedChunk && chunkAcked,
        transferDetail())

    fun infoText(): String =
        if (!enabled) "已关闭" else if (routeOnly()) "龙珠雷达：绿色网格，无底图下载与传输\n$routeInfo${if (routeAcked) "（圆屏已确认）" else ""}" else "来源：${sourceName()}　${if(chunkAcked) status else loadingStatus()}\n$chunkInfo${if (chunkAcked) "（圆屏已确认）" else ""}\n" +
            "$routeInfo${if (routeAcked) "（圆屏已确认）" else ""}\n" + offlineStatus() + "\n" + coverageInfo()

    private fun coverageInfo(): String {
        if (!haveConfirmedChunk || !havePose) return "地图覆盖：等待圆屏确认"
        val distance = maxOf(kotlin.math.abs(poseX - confirmedCx), kotlin.math.abs(poseY - confirmedCy))
        val margin = (confirmedHalf - MapCoveragePolicy.viewRadius(150) - distance).roundToInt()
        return "已确认覆盖 ${(confirmedHalf * 2).roundToInt()} 米，可见余量 $margin 米\n" +
            "普通图构建 ${(baseBuildSeconds * 10).roundToInt() / 10.0} 秒，确认 ${(baseDeliverySeconds * 10).roundToInt() / 10.0} 秒，准备预留 ${basePreparationSeconds().roundToInt()} 秒"
    }
}

/** 从 OpenStreetMap 的 Overpass 接口下载并解析道路、绿地、水面 */
object OsmFetch {
    private val SERVERS = listOf(
        "https://overpass-api.de/api/interpreter",
        "https://overpass.kumi.systems/api/interpreter",
        "https://maps.mail.ru/osm/tools/overpass/api/interpreter"
    )

    fun query(s: Double, w: Double, n: Double, e: Double): String {
        val bb = "%.6f,%.6f,%.6f,%.6f".format(java.util.Locale.US, s, w, n, e)
        val q = """
            [out:json][timeout:25];
            (
              way["highway"~"^(motorway|trunk|primary|secondary|tertiary|unclassified|residential|living_street|service|motorway_link|trunk_link|primary_link|secondary_link|tertiary_link)$"]($bb);
              way["leisure"~"^(park|garden|pitch|golf_course|nature_reserve)$"]($bb);
              way["landuse"~"^(grass|forest|meadow|recreation_ground|village_green|park|reservoir|basin)$"]($bb);
              way["natural"~"^(wood|scrub|grassland|water)$"]($bb);
              way["waterway"="riverbank"]($bb);
              relation["natural"="water"]($bb);
              relation["leisure"="park"]($bb);
              relation["landuse"~"^(forest|grass)$"]($bb);
            );
            out geom;
        """.trimIndent()
        var last: Exception? = null
        for (srv in SERVERS) {
            try {
                val c = URL(srv).openConnection() as HttpURLConnection
                c.connectTimeout = 10_000
                c.readTimeout = 40_000
                c.requestMethod = "POST"
                c.doOutput = true
                c.setRequestProperty("User-Agent", "MotoNav/2.0 (personal motorcycle display)")
                c.setRequestProperty("Content-Type", "application/x-www-form-urlencoded")
                c.outputStream.use { it.write(("data=" + URLEncoder.encode(q, "UTF-8")).toByteArray()) }
                if (c.responseCode != 200) throw Exception("HTTP ${c.responseCode}")
                return c.inputStream.bufferedReader().use { it.readText() }
            } catch (ex: Exception) {
                last = ex
            }
        }
        throw last ?: Exception("无法连接地图服务器")
    }

    /** 根据标签决定类别：返回 Pair(类别, 是否面)，不需要的返回 null */
    private fun classify(tags: JSONObject?): Pair<Int, Boolean>? {
        if (tags == null) return null
        val hw = tags.optString("highway", "")
        if (hw.isNotEmpty()) {
            val c = when (hw) {
                "motorway", "trunk" -> 1
                "primary" -> 2
                "secondary", "tertiary" -> 3
                "residential", "unclassified", "living_street" -> 4
                "service", "motorway_link", "trunk_link", "primary_link", "secondary_link", "tertiary_link" -> 5
                else -> return null
            }
            if (tags.optString("area", "") == "yes") return null
            return Pair(c, false)
        }
        val natural = tags.optString("natural", "")
        val landuse = tags.optString("landuse", "")
        if (natural == "water" || tags.optString("waterway", "") == "riverbank" ||
            landuse == "reservoir" || landuse == "basin") return Pair(MapGeo.CLS_WATER, true)
        if (tags.has("leisure") || landuse.isNotEmpty() || natural.isNotEmpty()) return Pair(MapGeo.CLS_PARK, true)
        return null
    }

    private fun geom(arr: JSONArray?, f: MapGeo.Frame): DoubleArray? {
        if (arr == null || arr.length() < 2) return null
        val out = DoubleArray(arr.length() * 2)
        var n = 0
        for (i in 0 until arr.length()) {
            val p = arr.optJSONObject(i) ?: continue
            val g = MapGeo.wgsToGcj(p.getDouble("lat"), p.getDouble("lon"))
            out[2 * n] = f.x(g[1]); out[2 * n + 1] = f.y(g[0]); n++
        }
        return if (n >= 2) out.copyOf(n * 2) else null
    }

    fun parse(json: String, f: MapGeo.Frame): List<MapGeo.Feature> {
        val els = JSONObject(json).optJSONArray("elements") ?: return emptyList()
        val out = ArrayList<MapGeo.Feature>()
        for (i in 0 until els.length()) {
            val el = els.optJSONObject(i) ?: continue
            val cls = classify(el.optJSONObject("tags")) ?: continue
            when (el.optString("type")) {
                "way" -> {
                    val g = geom(el.optJSONArray("geometry"), f) ?: continue
                    if (cls.second && g.size < 6) continue
                    out.add(MapGeo.Feature(cls.first, cls.second, g))
                }
                "relation" -> {
                    if (!cls.second) continue
                    val mem = el.optJSONArray("members") ?: continue
                    val parts = ArrayList<DoubleArray>()
                    for (j in 0 until mem.length()) {
                        val m = mem.optJSONObject(j) ?: continue
                        if (m.optString("type") != "way") continue
                        val role = m.optString("role", "")
                        if (role != "outer" && role != "") continue
                        geom(m.optJSONArray("geometry"), f)?.let { parts.add(it) }
                    }
                    for (r in MapGeo.joinRings(parts)) if (r.size >= 6) out.add(MapGeo.Feature(cls.first, true, r))
                }
            }
        }
        return out
    }
}

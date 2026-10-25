package com.example.motonav

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.CancellationException
import java.util.concurrent.Callable
import java.util.concurrent.Executors
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.roundToInt
import kotlin.math.tan

/**
 * 版本：跟随 App 版本，见 MainActivity.kt 里的 APP_VERSION（v2.3 新增；v5.2 key 改从 local.properties 读，v6.5 只用 App 里填的；v5.9 新增卫星图）
 *
 * 天地图图片地图：
 * 1. 下载当前位置附近的天地图矢量底图小图片（不带文字标注的 vec_w 图层，16 级，每张 256 像素），
 *    下载过的存在手机缓存里，下次经过直接用。
 * 2. 按我们的坐标（高德坐标、北朝上、每像素 2 米）重新取样拼成一大张，
 *    同时把天地图的 WGS-84 坐标换算成高德坐标，保证和导航路线对齐。
 * 3. 把每个像素的颜色归成 4 类（街区 / 道路 / 绿地 / 水面），压缩后发给圆屏。
 */
object TdtRaster {
    /** v6.5 天地图 key：只用 App 里「Key 设置」填的；每次现读，改了马上生效 */
    private fun key(ctx: Context): String {
        val k = Keys.tdt(ctx)
        if (k.isEmpty()) throw Exception("没填天地图 key：到首页「Key 设置」里填")
        return k
    }
    private const val Z = 16                  // 瓦片级别：16 级约 2 米/像素
    const val MPP = 2.0                       // 输出每像素多少米

    class SmoothInput(val cleaned: ByteArray, val fallback: ByteArray, val size: Int)
    class Result(val bytes: ByteArray, val tiles: Int, val fromCache: Int,
                 val previewRaw: Bitmap?, val previewCls: Bitmap?, val smooth: Boolean = false, val contourVersion: Int = 0,
                 val deferredSmooth: SmoothInput? = null)

    private val pool = Executors.newFixedThreadPool(6)
    private val offlinePool = Executors.newFixedThreadPool(2)
    private val tileWrites = java.util.concurrent.atomic.AtomicInteger()
    private val tileCleanupLock = Any()

    private fun trimTiles(ctx: Context) {
        if (tileWrites.incrementAndGet()%64!=0) return
        synchronized(tileCleanupLock) {
            val files=File(ctx.filesDir,"tdt_tiles").walkTopDown().filter { it.isFile && it.extension=="png" }.toList()
            var total=files.sumOf { it.length() }
            val limit=256L*1024*1024
            if (total>limit) for (file in files.sortedBy { it.lastModified() }) {
                val size=file.length()
                if (file.delete()) total-=size
                if (total<=limit) break
            }
        }
    }

    private fun tileUrl(ctx: Context, x: Int, y: Int, layer: String, zoom: Int) =
        "https://t${(x + y) and 7}.tianditu.gov.cn/DataServer?T=$layer&x=$x&y=$y&l=$zoom&tk=${key(ctx)}"

    /** 下载一张瓦片（先查缓存），返回 256×256 像素；失败抛异常。layer：vec_w 矢量底图 / img_w 卫星照片（v5.9） */
    private fun tile(ctx: Context, x: Int, y: Int, cacheHit: IntArray, layer: String = "vec_w", zoom: Int = Z): IntArray {
        val f = File(ctx.filesDir, (if (layer == "vec_w") "tdt_tiles" else "tdt_$layer") + "/$zoom/$x/$y.png")
        var bmp: Bitmap? = null
        if (f.isFile) try {
            val bytes = f.readBytes()
            bmp = BitmapFactory.decodeByteArray(bytes,0,bytes.size)
            if (bmp?.width != 256 || bmp?.height != 256) { bmp?.recycle(); bmp=null; f.delete() }
            else synchronized(cacheHit) { cacheHit[0]++ }
        } catch (_: Exception) { f.delete() }
        if (bmp == null) {
            if (Thread.currentThread().isInterrupted) throw CancellationException()
            val c=URL(tileUrl(ctx,x,y,layer,zoom)).openConnection() as HttpURLConnection
            try {
                c.connectTimeout=8000; c.readTimeout=15000
                c.setRequestProperty("User-Agent","MotoNav/2.3.2 (Android)")
                val code=c.responseCode
                if (code!=200 || !(c.contentType ?: "").startsWith("image")) throw java.io.IOException("天地图请求失败：HTTP $code")
                val body=c.inputStream.use { it.readBytes() }
                bmp=BitmapFactory.decodeByteArray(body,0,body.size)
                if (bmp?.width!=256 || bmp?.height!=256) { bmp?.recycle(); throw java.io.IOException("瓦片图片不完整") }
                // 完整解码后原子替换缓存；前台和离线下载同时访问也不会读到半张图片。
                f.parentFile?.mkdirs()
                val tmp=File.createTempFile("tile-",".tmp",f.parentFile)
                try { tmp.writeBytes(body); synchronized(f.path.intern()) { if (!tmp.renameTo(f) && !f.exists()) throw java.io.IOException("瓦片缓存写入失败") } }
                finally { tmp.delete() }
                trimTiles(ctx)
            } finally { c.disconnect() }
        }
        val image=bmp ?: throw java.io.IOException("瓦片图片解析失败")
        val px = IntArray(256 * 256)
        val w = minOf(256, image.width); val h = minOf(256, image.height)
        image.getPixels(px, 0, 256, 0, 0, w, h)
        image.recycle()
        return px
    }

    // 经纬度（WGS-84）→ 16 级全球像素坐标（Web 墨卡托）
    private fun mx(lng: Double, zoom: Int = Z) = (lng + 180.0) / 360.0 * (256.0 * (1 shl zoom))
    private fun my(lat: Double, zoom: Int = Z): Double {
        val r = Math.toRadians(lat)
        return (1.0 - ln(tan(r) + 1.0 / cos(r)) / PI) / 2.0 * (256.0 * (1 shl zoom))
    }

    /** 生成以 (cx, cy)（平面米坐标）为中心、半边长 half 米的图片地图 */
    fun build(ctx: Context, f: MapGeo.Frame, cx: Double, cy: Double, half: Double, vice: Boolean = false,
              mpp: Double = MPP, zoom: Int = Z, deferSmooth: Boolean = false, makePreview: Boolean = true, background: Boolean = false, shouldCancel: () -> Boolean = { false },
              onBaseReady: ((ByteArray) -> Unit)? = null): Result {
        fun checkCancelled() { if (Thread.currentThread().isInterrupted || shouldCancel()) throw CancellationException() }
        checkCancelled()
        val n = (2 * half / mpp).roundToInt()
        val x0 = (cx - half).roundToInt()             // 左上角（米）
        val y0 = (cy + half).roundToInt()
        // 高德坐标和天地图坐标（WGS-84）的偏移：几公里内几乎不变，取中心点的值
        val gLat = f.lat(cy); val gLng = f.lng(cx)
        val w = MapGeo.gcjToWgs(gLat, gLng)
        val dLat = gLat - w[0]; val dLng = gLng - w[1]
        // 每一列、每一行对应的全球像素坐标（经度只和列有关，纬度只和行有关）
        val colPx = DoubleArray(n) { i -> mx(f.lng(x0 + (i + 0.5) * mpp) - dLng, zoom) }
        val rowPx = DoubleArray(n) { j -> my(f.lat(y0 - (j + 0.5) * mpp) - dLat, zoom) }
        val tx0 = floor(colPx[0] / 256).toInt(); val tx1 = floor(colPx[n - 1] / 256).toInt()
        val ty0 = floor(rowPx[0] / 256).toInt(); val ty1 = floor(rowPx[n - 1] / 256).toInt()
        val tw = tx1 - tx0 + 1; val th = ty1 - ty0 + 1
        if (tw * th > 120) throw Exception("需要的瓦片太多（${tw * th}）")
        // 并行下载全部瓦片
        val cacheHit = IntArray(1)
        val jobs = ArrayList<java.util.concurrent.Future<IntArray>>()
        val executor=if (background) offlinePool else pool
        val tiles: List<IntArray>
        try {
            for (ty in ty0..ty1) for (tx in tx0..tx1) {
                checkCancelled()
                jobs.add(executor.submit(Callable { checkCancelled(); tile(ctx,tx,ty,cacheHit,zoom=zoom) }))
            }
            tiles=jobs.map { checkCancelled(); it.get() }
        } finally { jobs.forEach { if (!it.isDone) it.cancel(true) } }
        checkCancelled()
        // 重新取样
        val raw = IntArray(n * n)
        for (j in 0 until n) {
            checkCancelled()
            val py = rowPx[j]
            val ty = floor(py / 256).toInt() - ty0
            val iy = (py - (ty + ty0) * 256.0).toInt().coerceIn(0, 255)
            for (i in 0 until n) {
                val pxx = colPx[i]
                val tx = floor(pxx / 256).toInt() - tx0
                val ix = (pxx - (tx + tx0) * 256.0).toInt().coerceIn(0, 255)
                raw[j * n + i] = tiles[ty * tw + tx][iy * 256 + ix]
            }
        }
        // 颜色归类
        val bg = MapGeo.backgroundLum(raw)
        val idx = ByteArray(n * n)
        val cache = HashMap<Int, Int>()
        for (k in raw.indices) {
            val c = raw[k] or (0xFF shl 24)
            idx[k] = cache.getOrPut(c) { MapGeo.classifyPixel(c, bg) }.toByte()
        }
        checkCancelled()
        val cleaned = MapGeo.cleanRaster(idx, n, n)
        val idx2 = MapGeo.dilateRoads(cleaned, n, n)   // v2.3.2 先清理杂点再加粗道路
        val base = MapGeo.encodeRaster(f.id, x0, y0, n, n, (mpp * 1000).roundToInt(), idx2)
        if (base.size > 480_000) throw Exception("普通底图超过圆屏缓冲")
        // 先交付完整普通地图，轮廓计算和传送不再挡住底图覆盖。
        checkCancelled()
        onBaseReady?.invoke(base)
        if (!makePreview && !vice) return Result(base,tw*th,cacheHit[0],null,null)
        val bytes = if (vice && !deferSmooth) try { ViceRaster.append(base, cleaned, n, n, idx2) }
            catch (_: OutOfMemoryError) { base } else base
        if (bytes.size > 480_000) throw Exception("压缩后 ${bytes.size / 1024} KB，超过圆屏缓冲")
        // 预览图（给 App 界面看，方便调颜色归类）
        val pv = 300
        val pr = IntArray(pv * pv); val pc = IntArray(pv * pv)
        val pal = intArrayOf(0xFFDCDCDC.toInt(), 0xFF303030.toInt(), 0xFF5BBF5B.toInt(), 0xFF4A90D9.toInt())
        for (j in 0 until pv) for (i in 0 until pv) {
            val k = (j * n / pv) * n + (i * n / pv)
            pr[j * pv + i] = raw[k] or (0xFF shl 24)
            pc[j * pv + i] = pal[idx2[k].toInt() and 3]
        }
        val bRaw = Bitmap.createBitmap(pr, pv, pv, Bitmap.Config.ARGB_8888)
        val bCls = Bitmap.createBitmap(pc, pv, pv, Bitmap.Config.ARGB_8888)
        return Result(bytes, tw * th, cacheHit[0], bRaw, bCls, bytes !== base,
            if (bytes === base) 0 else bytes[base.size + 3].toInt() and 255,
            if (vice && deferSmooth) SmoothInput(cleaned, idx2, n) else null)
    }

    // ================= v5.9 卫星图 =================
    private const val SAT_MPP = 3.5                 // 卫星图每像素 3.5 米（范围小一些，数据量才发得动）

    /**
     * 卫星图：下载天地图卫星照片（img_w），按我们的坐标重新取样，稍微模糊一下（去掉噪点，压缩得更小），
     * 再挑出最有代表性的 16 种颜色（k 均值），每个像素换成最接近的那种，连同调色板一起发给圆屏。
     */
    fun buildSat(ctx: Context, f: MapGeo.Frame, cx: Double, cy: Double, half: Double,
                 shouldCancel: () -> Boolean = { false }): Result {
        fun checkCancelled() { if (Thread.currentThread().isInterrupted || shouldCancel()) throw CancellationException() }
        checkCancelled()
        val n = (2 * half / SAT_MPP).roundToInt()
        val x0 = (cx - half).roundToInt()
        val y0 = (cy + half).roundToInt()
        val gLat = f.lat(cy); val gLng = f.lng(cx)
        val w = MapGeo.gcjToWgs(gLat, gLng)
        val dLat = gLat - w[0]; val dLng = gLng - w[1]
        val colPx = DoubleArray(n) { i -> mx(f.lng(x0 + (i + 0.5) * SAT_MPP) - dLng) }
        val rowPx = DoubleArray(n) { j -> my(f.lat(y0 - (j + 0.5) * SAT_MPP) - dLat) }
        val tx0 = floor(colPx[0] / 256).toInt(); val tx1 = floor(colPx[n - 1] / 256).toInt()
        val ty0 = floor(rowPx[0] / 256).toInt(); val ty1 = floor(rowPx[n - 1] / 256).toInt()
        val tw = tx1 - tx0 + 1; val th = ty1 - ty0 + 1
        if (tw * th > 64) throw Exception("需要的卫星瓦片太多（${tw * th}）")
        val cacheHit = IntArray(1)
        val jobs = ArrayList<java.util.concurrent.Future<IntArray>>()
        for (ty in ty0..ty1) for (tx in tx0..tx1) jobs.add(pool.submit(Callable {
            checkCancelled()
            tile(ctx, tx, ty, cacheHit, "img_w")
        }))
        val tiles = jobs.map { it.get() }
        checkCancelled()
        // 取样（每个输出像素取对应位置周围 2×2 的平均，比只取一个点平滑）
        val R = IntArray(n * n); val G = IntArray(n * n); val B = IntArray(n * n)
        fun px(gx: Double, gy: Double): Int {
            val tx = floor(gx / 256).toInt() - tx0; val ty = floor(gy / 256).toInt() - ty0
            val ix = (gx - (tx + tx0) * 256.0).toInt().coerceIn(0, 255)
            val iy = (gy - (ty + ty0) * 256.0).toInt().coerceIn(0, 255)
            return tiles[ty.coerceIn(0, th - 1) * tw + tx.coerceIn(0, tw - 1)][iy * 256 + ix]
        }
        val stepX = if (n > 1) (colPx[n - 1] - colPx[0]) / (n - 1) / 2 else 0.0
        val stepY = if (n > 1) (rowPx[n - 1] - rowPx[0]) / (n - 1) / 2 else 0.0
        for (j in 0 until n) for (i in 0 until n) {
            var r = 0; var g = 0; var b = 0
            for (k in 0 until 4) {
                val c = px(colPx[i] + (if (k and 1 == 0) -stepX else stepX) / 2, rowPx[j] + (if (k < 2) -stepY else stepY) / 2)
                r += (c shr 16) and 255; g += (c shr 8) and 255; b += c and 255
            }
            R[j * n + i] = r / 4; G[j * n + i] = g / 4; B[j * n + i] = b / 4
        }
        // 3×3 模糊
        fun blur(a: IntArray): IntArray {
            val o = IntArray(n * n)
            for (j in 0 until n) for (i in 0 until n) {
                var sum = 0; var cnt = 0
                for (dj in -1..1) for (di in -1..1) {
                    val jj = j + dj; val ii = i + di
                    if (jj in 0 until n && ii in 0 until n) { sum += a[jj * n + ii]; cnt++ }
                }
                o[j * n + i] = sum / cnt
            }
            return o
        }
        val r1 = blur(R); val g1 = blur(G); val b1 = blur(B)
        // k 均值找 16 种颜色：先按亮度排序均匀取 16 个起点，迭代 10 轮
        val rnd = java.util.Random(7)
        val S = minOf(6000, n * n)
        val sIdx = IntArray(S) { rnd.nextInt(n * n) }
        val sorted = sIdx.sortedBy { r1[it] * 3 + g1[it] * 6 + b1[it] }
        val K = 16
        val cr = DoubleArray(K) { r1[sorted[(it * 2 + 1) * S / (2 * K)]].toDouble() }
        val cg = DoubleArray(K) { g1[sorted[(it * 2 + 1) * S / (2 * K)]].toDouble() }
        val cb = DoubleArray(K) { b1[sorted[(it * 2 + 1) * S / (2 * K)]].toDouble() }
        fun nearest(r: Int, g: Int, b: Int): Int {
            var best = 0; var bd = Double.MAX_VALUE
            for (k in 0 until K) {
                val dr = r - cr[k]; val dg = g - cg[k]; val db = b - cb[k]
                val d = dr * dr * 2 + dg * dg * 4 + db * db * 3
                if (d < bd) { bd = d; best = k }
            }
            return best
        }
        repeat(10) {
            val sr = DoubleArray(K); val sg = DoubleArray(K); val sb = DoubleArray(K); val sc = IntArray(K)
            for (p in sIdx) { val k = nearest(r1[p], g1[p], b1[p]); sr[k] += r1[p].toDouble(); sg[k] += g1[p].toDouble(); sb[k] += b1[p].toDouble(); sc[k]++ }
            for (k in 0 until K) if (sc[k] > 0) { cr[k] = sr[k] / sc[k]; cg[k] = sg[k] / sc[k]; cb[k] = sb[k] / sc[k] }
        }
        val idx = ByteArray(n * n) { p -> nearest(r1[p], g1[p], b1[p]).toByte() }
        val pal = IntArray(K) { k ->
            val r = cr[k].roundToInt().coerceIn(0, 255); val g = cg[k].roundToInt().coerceIn(0, 255); val b = cb[k].roundToInt().coerceIn(0, 255)
            ((r shr 3) shl 11) or ((g shr 2) shl 5) or (b shr 3)
        }
        val bytes = MapGeo.encodeRaster(f.id, x0, y0, n, n, (SAT_MPP * 1000).roundToInt(), idx, pal)
        if (bytes.size > 480_000) throw Exception("卫星图压缩后 ${bytes.size / 1024} KB，超过圆屏缓冲")
        // 预览
        val pv = 300
        val pr = IntArray(pv * pv); val pc = IntArray(pv * pv)
        for (j in 0 until pv) for (i in 0 until pv) {
            val k = (j * n / pv) * n + (i * n / pv)
            pr[j * pv + i] = (0xFF shl 24) or (R[k] shl 16) or (G[k] shl 8) or B[k]
            val q = idx[k].toInt()
            pc[j * pv + i] = (0xFF shl 24) or (cr[q].roundToInt().coerceIn(0, 255) shl 16) or (cg[q].roundToInt().coerceIn(0, 255) shl 8) or cb[q].roundToInt().coerceIn(0, 255)
        }
        val bRaw = Bitmap.createBitmap(pr, pv, pv, Bitmap.Config.ARGB_8888)
        val bCls = Bitmap.createBitmap(pc, pv, pv, Bitmap.Config.ARGB_8888)
        return Result(bytes, tw * th, cacheHit[0], bRaw, bCls)
    }
}

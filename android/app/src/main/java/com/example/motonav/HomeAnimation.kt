package com.example.motonav

import android.content.Context

/** 首页动画选择，和 185B 固件通过 #A,n 同步。 */
object HomeAnimation {
    private const val PREFS = "motonav"
    private const val KEY = "home_animation"
    private var context: Context? = null
    @Volatile var selected: Int = 2
        private set

    val labels = arrayOf("原版骑手", "镜像弹跳", "星之卡比")

    fun load(ctx: Context) {
        if (context != null) return
        context = ctx.applicationContext
        selected = context!!.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getInt(KEY, 2).coerceIn(0, labels.lastIndex)
    }

    fun select(index: Int) {
        selected = index.coerceIn(0, labels.lastIndex)
        context?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)?.edit()
            ?.putInt(KEY, selected)?.apply()
        BleLink.send("#A,$selected")
    }

    fun sync() { BleLink.send("#A,$selected") }

    fun summary(): String = labels[selected]
}

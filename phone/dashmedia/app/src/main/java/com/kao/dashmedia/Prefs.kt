package com.kao.dashmedia

import android.content.Context

/**
 * The small amount of state that has to outlive the process.
 *
 * Go edition kills background processes readily, so "where was I" cannot live
 * in memory. Written on every change rather than at shutdown, because there
 * may not be a shutdown.
 */
class Prefs(context: Context) {

    private val p = context.getSharedPreferences("dashmedia", Context.MODE_PRIVATE)

    /** What the queue was built from, so a restart can rebuild it. */
    var source: Source?
        get() = Source.decode(p.getString(K_SOURCE, null))
        set(v) = p.edit().putString(K_SOURCE, v?.encode()).apply()

    /** 0 off, 1 low, 2 medium, 3 high. See PlayerService.applyBoost. */
    var boost: Int
        get() = p.getInt(K_BOOST, 0)
        set(v) = p.edit().putInt(K_BOOST, v).apply()

    /** Trim the silence at the start of a track. Only the start. */
    var trimSilence: Boolean
        get() = p.getBoolean(K_TRIM, true)
        set(v) = p.edit().putBoolean(K_TRIM, v).apply()

    var shuffle: Boolean
        get() = p.getBoolean(K_SHUFFLE, false)
        set(v) = p.edit().putBoolean(K_SHUFFLE, v).apply()

    /** MediaStore id of the track that was playing, or -1. */
    var resumeId: Long
        get() = p.getLong(K_ID, -1L)
        set(v) = p.edit().putLong(K_ID, v).apply()

    var resumePositionMs: Long
        get() = p.getLong(K_POS, 0L)
        set(v) = p.edit().putLong(K_POS, v).apply()

    fun saveResume(id: Long, positionMs: Long) {
        p.edit().putLong(K_ID, id).putLong(K_POS, positionMs).apply()
    }

    private companion object {
        const val K_SOURCE = "source"
        const val K_BOOST = "boost"
        const val K_TRIM = "trim_silence"
        const val K_SHUFFLE = "shuffle"
        const val K_ID = "resume_id"
        const val K_POS = "resume_pos"
    }
}

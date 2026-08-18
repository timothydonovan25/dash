package com.kao.dashmedia

/** Formatting shared by the lists and the now-playing clock. */
object Fmt {

    /**
     * Milliseconds as m:ss, or h:mm:ss once it runs past an hour.
     *
     * A negative or unknown duration prints as --:--; ExoPlayer reports
     * C.TIME_UNSET as a large negative number before an item is ready, and
     * "-9223372036854:47" on a row would be quite a thing to see.
     */
    fun clock(ms: Long): String {
        if (ms < 0) return "--:--"
        val total = ms / 1000
        val h = total / 3600
        val m = (total % 3600) / 60
        val s = total % 60
        return if (h > 0) "%d:%02d:%02d".format(h, m, s) else "%d:%02d".format(m, s)
    }
}

package com.kao.dashmedia

/** One timed line. An empty [text] is a real thing: an instrumental gap. */
data class LrcLine(val timeMs: Long, val text: String)

/**
 * A parsed .lrc file.
 *
 * [lines] is sorted by time and already has any `[offset:]` applied, so
 * nothing downstream has to remember that offsets exist.
 */
data class Lrc(
    val lines: List<LrcLine>,
    val title: String? = null,
    val artist: String? = null,
    val album: String? = null,
) {
    val isEmpty: Boolean get() = lines.isEmpty()

    /**
     * Index of the line that should be showing at [positionMs], or -1 before
     * the first one.
     *
     * Binary search rather than a scan: this is called on every progress tick,
     * and a linear walk of a long lyric would be work done sixty times a
     * minute for no reason.
     */
    fun indexAt(positionMs: Long): Int {
        var lo = 0
        var hi = lines.size - 1
        var found = -1
        while (lo <= hi) {
            val mid = (lo + hi) ushr 1
            if (lines[mid].timeMs <= positionMs) {
                found = mid
                lo = mid + 1
            } else {
                hi = mid - 1
            }
        }
        return found
    }
}

/**
 * The .lrc format, which is loose enough in the wild that most of this file is
 * about tolerating what real files actually contain.
 *
 * Pure Kotlin, tested on the JVM: a lyric that is a second out is obvious to a
 * listener and invisible to a compiler, so the arithmetic is worth pinning
 * down away from a device.
 */
object LrcParser {

    /** `[mm:ss]`, `[mm:ss.xx]` or `[mm:ss.xxx]`, with `:` also seen as the point. */
    private val TIME = Regex("""\[(\d{1,3}):([0-5]?\d)(?:[.:](\d{1,3}))?]""")

    /** `[ar:...]` and friends. The key is letters, which is what separates it
     *  from a time tag. */
    private val META = Regex("""\[([a-zA-Z]+):([^]]*)]""")

    fun parse(text: String?): Lrc {
        if (text.isNullOrBlank()) return Lrc(emptyList())

        var title: String? = null
        var artist: String? = null
        var album: String? = null
        var offsetMs = 0L
        val out = ArrayList<LrcLine>(64)

        // Strip a BOM; plenty of these files are written by Windows tools.
        for (raw in text.removePrefix("﻿").lineSequence()) {
            val line = raw.trimEnd('\r', '\n')
            if (line.isBlank()) continue

            val times = TIME.findAll(line).toList()
            if (times.isEmpty()) {
                // No timestamp, so it can only be metadata. Anything else is a
                // stray line and is dropped rather than shown untimed.
                val m = META.find(line) ?: continue
                val value = m.groupValues[2].trim()
                when (m.groupValues[1].lowercase()) {
                    "ti" -> title = value.ifEmpty { null }
                    "ar" -> artist = value.ifEmpty { null }
                    "al" -> album = value.ifEmpty { null }
                    "offset" -> offsetMs = value.removePrefix("+").trim().toLongOrNull() ?: 0L
                }
                continue
            }

            // The text is whatever follows the last timestamp. A line may carry
            // several -- a chorus is written once and pointed at repeatedly.
            val body = line.substring(times.last().range.last + 1).trim()
            for (t in times) out.add(LrcLine(millis(t), body))
        }

        // `[offset:+500]` means the words arrive earlier by that much, so it
        // comes off the timestamps. Clamped at zero: a large offset must not
        // push the opening line to a position that can never be reached.
        val shifted =
            if (offsetMs == 0L) out
            else out.map { it.copy(timeMs = (it.timeMs - offsetMs).coerceAtLeast(0L)) }

        // Files are not reliably in order, and a chorus written once and
        // referenced three times never is.
        return Lrc(shifted.sortedBy { it.timeMs }, title, artist, album)
    }

    private fun millis(m: MatchResult): Long {
        val min = m.groupValues[1].toLong()
        val sec = m.groupValues[2].toLong()
        val frac = m.groupValues[3]
        val fracMs = when (frac.length) {
            0 -> 0L
            1 -> frac.toLong() * 100      // tenths
            2 -> frac.toLong() * 10       // centiseconds, the usual case
            else -> frac.toLong()         // already milliseconds
        }
        return min * 60_000L + sec * 1_000L + fracMs
    }
}

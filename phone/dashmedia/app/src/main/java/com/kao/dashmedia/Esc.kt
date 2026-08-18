package com.kao.dashmedia

/**
 * Delimiting user text in a flat string, safely.
 *
 * Tag and playlist names are typed by people and read off files, so every
 * obvious separator occurs in real data -- "AC/DC", "Nine Inch Nails: Broken",
 * "a|b". The unit separator is unlikely rather than impossible, so values are
 * escaped as well and the delimiter genuinely cannot appear in one.
 *
 * Shared by [Source], which stores what the queue was built from, and [Store],
 * which stores playlists and play counts.
 */
object Esc {

    /** ASCII unit separator. */
    const val SEP = "\u001F"

    fun join(vararg parts: String): String = parts.joinToString(SEP) { escape(it) }

    fun split(s: String): List<String> = s.split(SEP).map { unescape(it) }

    /** Backslash escapes itself and the separator; nothing else is touched. */
    fun escape(v: String): String = v.replace("\\", "\\\\").replace(SEP, "\\s")

    fun unescape(v: String): String {
        if (v.indexOf('\\') < 0) return v
        val sb = StringBuilder(v.length)
        var i = 0
        while (i < v.length) {
            val c = v[i]
            if (c == '\\' && i + 1 < v.length) {
                when (v[i + 1]) {
                    '\\' -> { sb.append('\\'); i += 2 }
                    's' -> { sb.append(SEP); i += 2 }
                    else -> { sb.append(c); i++ }
                }
            } else {
                sb.append(c); i++
            }
        }
        return sb.toString()
    }
}

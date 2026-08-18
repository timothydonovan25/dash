package com.kao.dashmedia

/**
 * What the queue was built from -- "whatever you're in".
 *
 * The middle softkey toggles repeat and shuffle over the current queue rather
 * than re-scoping it, so nothing rebuilds the queue behind your back, and the
 * only thing that has to survive a restart is which list you picked.
 *
 * Pure Kotlin, and encoded as one string because SharedPreferences is a flat
 * map and a playlist named "artist:Gojira" should not be able to impersonate
 * an artist.
 */
sealed interface Source {

    object All : Source
    data class Artist(val name: String) : Source
    data class Album(val artist: String, val title: String) : Source
    data class Playlist(val id: Long, val name: String) : Source

    fun encode(): String = when (this) {
        is All -> "all"
        is Artist -> join("artist", name)
        is Album -> join("album", artist, title)
        is Playlist -> join("playlist", id.toString(), name)
    }

    /** How a header names it. */
    fun label(): String = when (this) {
        is All -> ""
        is Artist -> Browse.display(name)
        is Album -> Browse.display(title)
        is Playlist -> name
    }

    companion object {
        /**
         * ASCII unit separator: unlikely in a tag, unlike the colon, slash and
         * pipe that all occur in real names. Unlikely is not the same as
         * impossible, though, so values are escaped as well -- see [esc].
         */
        private const val SEP = "\u001F"

        private fun join(vararg parts: String): String =
            parts.joinToString(SEP) { esc(it) }

        /**
         * A playlist name is typed by a person and can contain anything at
         * all, including this separator -- so the values are escaped rather
         * than trusted. Without this, a playlist called "artist<sep>Gojira"
         * came back decoded as "artist", which is the sort of bug that only
         * ever shows up in someone else's library.
         */
        private fun esc(v: String): String =
            v.replace("\\", "\\\\").replace(SEP, "\\s")

        private fun unesc(v: String): String {
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

        fun decode(s: String?): Source? {
            if (s.isNullOrEmpty()) return null
            val parts = s.split(SEP).map { unesc(it) }
            return when (parts.getOrNull(0)) {
                "all" -> All
                "artist" -> parts.getOrNull(1)?.let { Artist(it) }
                "album" -> {
                    val a = parts.getOrNull(1)
                    val t = parts.getOrNull(2)
                    if (a != null && t != null) Album(a, t) else null
                }
                "playlist" -> {
                    val id = parts.getOrNull(1)?.toLongOrNull()
                    if (id != null) Playlist(id, parts.getOrNull(2).orEmpty()) else null
                }
                else -> null
            }
        }
    }
}

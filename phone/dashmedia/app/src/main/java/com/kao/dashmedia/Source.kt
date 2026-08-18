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

    /** A read-only MediaStore playlist, made by some other app. */
    data class Playlist(val id: Long, val name: String) : Source

    /** One of ours, which can be edited. */
    data class Owned(val id: Long, val name: String) : Source

    /** The automatic lists. Contents depend on when you ask. */
    object RecentAdded : Source
    object RecentPlayed : Source
    object MostPlayed : Source

    fun encode(): String = when (this) {
        is All -> "all"
        is Artist -> Esc.join("artist", name)
        is Album -> Esc.join("album", artist, title)
        is Playlist -> Esc.join("playlist", id.toString(), name)
        is Owned -> Esc.join("owned", id.toString(), name)
        is RecentAdded -> "recent_added"
        is RecentPlayed -> "recent_played"
        is MostPlayed -> "most_played"
    }

    /** How a header names it. */
    fun label(): String = when (this) {
        is All -> ""
        is Artist -> Browse.display(name)
        is Album -> Browse.display(title)
        is Playlist -> name
        is Owned -> name
        is RecentAdded -> ""
        is RecentPlayed -> ""
        is MostPlayed -> ""
    }

    companion object {

        fun decode(s: String?): Source? {
            if (s.isNullOrEmpty()) return null
            val parts = Esc.split(s)
            return when (parts.getOrNull(0)) {
                "all" -> All
                "recent_added" -> RecentAdded
                "recent_played" -> RecentPlayed
                "most_played" -> MostPlayed
                "owned" -> {
                    val id = parts.getOrNull(1)?.toLongOrNull()
                    if (id != null) Owned(id, parts.getOrNull(2).orEmpty()) else null
                }
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

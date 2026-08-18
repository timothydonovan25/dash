package com.kao.dashmedia

/**
 * Grouping a flat MediaStore list into artist -> album -> track.
 *
 * Pure, so the awkward parts are testable: untagged files, an album title
 * shared by two artists, and sort order that ignores case without collapsing
 * two genuinely different names.
 */
/** The little Browse needs to know about a track in order to group it. */
interface Tagged {
    val artist: String
    val album: String
}

object Browse {

    /** Shown for a track whose tag is empty. Never used as a key. */
    const val UNKNOWN = "Unknown"

    fun display(s: String): String = if (s.isBlank()) UNKNOWN else s

    /** Every artist, once, case-insensitively sorted. Untagged sorts last. */
    fun <T : Tagged> artists(all: List<T>): List<String> =
        all.map { it.artist }.distinct().sortedWith(byName)

    /** The albums an artist has tracks on. */
    fun <T : Tagged> albums(all: List<T>, artist: String): List<String> =
        all.filter { it.artist == artist }.map { it.album }.distinct().sortedWith(byName)

    /**
     * One album by one artist. The artist has to match as well as the title:
     * "Greatest Hits" is not one album shared by everyone who has released one.
     */
    fun <T : Tagged> tracksOf(all: List<T>, artist: String, album: String): List<T> =
        all.filter { it.artist == artist && it.album == album }

    fun <T : Tagged> tracksBy(all: List<T>, artist: String): List<T> =
        all.filter { it.artist == artist }

    /**
     * Blank last, then case-insensitive. Falling back to a case-sensitive
     * compare keeps "AC/DC" and "ac/dc" as two stable entries rather than an
     * order that changes between runs.
     */
    private val byName = Comparator<String> { a, b ->
        when {
            a.isBlank() && b.isBlank() -> 0
            a.isBlank() -> 1
            b.isBlank() -> -1
            else -> a.compareTo(b, ignoreCase = true).let {
                if (it != 0) it else a.compareTo(b)
            }
        }
    }
}

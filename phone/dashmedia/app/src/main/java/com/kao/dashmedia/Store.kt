package com.kao.dashmedia

/** A playlist this app owns, and can therefore edit. */
data class OwnedList(val id: Long, val name: String, val trackIds: List<Long>)

/** What we know about how much a track gets played. */
data class Stat(val plays: Int, val lastPlayedMs: Long)

/**
 * The on-disk format for playlists and listening stats.
 *
 * App-owned rather than MediaStore: an app on API 30 may only modify playlists
 * it created, and `MediaStore.Audio.Playlists` is deprecated from 31 anyway.
 * Keeping our own means full edit rights today and nothing to rewrite later,
 * at the price of these playlists being invisible to other apps.
 *
 * One record per line, so a corrupted or half-written line costs one playlist
 * rather than the file. Pure Kotlin, so the format is testable without a
 * device -- which is the whole reason the parsing lives here and not in the
 * class that touches the filesystem.
 */
object StoreCodec {

    fun encodeLists(lists: List<OwnedList>): String =
        lists.joinToString("\n") { l ->
            Esc.join(l.id.toString(), l.name, l.trackIds.joinToString(","))
        }

    fun decodeLists(text: String?): List<OwnedList> {
        if (text.isNullOrBlank()) return emptyList()
        return text.lineSequence().mapNotNull { line ->
            if (line.isBlank()) return@mapNotNull null
            val p = Esc.split(line)
            val id = p.getOrNull(0)?.toLongOrNull() ?: return@mapNotNull null
            val name = p.getOrNull(1) ?: return@mapNotNull null
            // A playlist with no tracks is legal: you make it before you fill it.
            val ids = p.getOrNull(2).orEmpty()
                .split(",")
                .mapNotNull { it.trim().toLongOrNull() }
            OwnedList(id, name, ids)
        }.toList()
    }

    fun encodeStats(stats: Map<Long, Stat>): String =
        stats.entries.joinToString("\n") { (id, s) ->
            Esc.join(id.toString(), s.plays.toString(), s.lastPlayedMs.toString())
        }

    fun decodeStats(text: String?): Map<Long, Stat> {
        if (text.isNullOrBlank()) return emptyMap()
        val out = HashMap<Long, Stat>()
        for (line in text.lineSequence()) {
            if (line.isBlank()) continue
            val p = Esc.split(line)
            val id = p.getOrNull(0)?.toLongOrNull() ?: continue
            val plays = p.getOrNull(1)?.toIntOrNull() ?: continue
            val last = p.getOrNull(2)?.toLongOrNull() ?: continue
            out[id] = Stat(plays, last)
        }
        return out
    }
}

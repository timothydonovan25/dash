package com.kao.dashmedia

import android.content.Context
import android.util.Log
import java.io.File

/**
 * The filesystem half of [StoreCodec] -- everything this app owns rather than
 * borrows from MediaStore.
 *
 * Two files in app-private storage: the playlists you can edit, and how often
 * each track has been played. Both are small enough to hold in memory and
 * rewrite whole, which avoids every partial-update problem a bigger scheme
 * would bring, and both are written through a temporary file so a process
 * killed mid-write loses the change rather than the file.
 */
class Stash(context: Context) {

    private val listsFile = File(context.filesDir, "playlists.txt")
    private val statsFile = File(context.filesDir, "stats.txt")

    var lists: List<OwnedList> = StoreCodec.decodeLists(read(listsFile))
        private set

    private var stats: MutableMap<Long, Stat> =
        StoreCodec.decodeStats(read(statsFile)).toMutableMap()

    // ---- playlists -------------------------------------------------------

    fun create(name: String, trackIds: List<Long> = emptyList()): OwnedList {
        // Ids only have to be unique among ours, and time is a cheap way to be.
        val id = (lists.maxOfOrNull { it.id } ?: 0L).coerceAtLeast(
            System.currentTimeMillis() / 1000
        ) + 1
        val made = OwnedList(id, name, trackIds)
        lists = lists + made
        saveLists()
        return made
    }

    fun rename(id: Long, name: String) {
        lists = lists.map { if (it.id == id) it.copy(name = name) else it }
        saveLists()
    }

    fun delete(id: Long) {
        lists = lists.filterNot { it.id == id }
        saveLists()
    }

    fun addTo(id: Long, trackId: Long) {
        // Duplicates allowed: the same song twice in a playlist is a choice.
        lists = lists.map { if (it.id == id) it.copy(trackIds = it.trackIds + trackId) else it }
        saveLists()
    }

    fun setTracks(id: Long, trackIds: List<Long>) {
        lists = lists.map { if (it.id == id) it.copy(trackIds = trackIds) else it }
        saveLists()
    }

    fun find(id: Long): OwnedList? = lists.firstOrNull { it.id == id }

    // ---- stats -----------------------------------------------------------

    fun played(trackId: Long, nowMs: Long = System.currentTimeMillis()) {
        val old = stats[trackId]
        stats[trackId] = Stat((old?.plays ?: 0) + 1, nowMs)
        saveStats()
    }

    fun plays(trackId: Long): Int = stats[trackId]?.plays ?: 0

    fun lastPlayed(trackId: Long): Long = stats[trackId]?.lastPlayedMs ?: 0L

    fun anyPlays(): Boolean = stats.isNotEmpty()

    // ---- io --------------------------------------------------------------

    private fun saveLists() = write(listsFile, StoreCodec.encodeLists(lists))

    private fun saveStats() = write(statsFile, StoreCodec.encodeStats(stats))

    private fun read(f: File): String? =
        runCatching { if (f.exists()) f.readText() else null }
            .onFailure { Log.w(TAG, "could not read ${f.name}", it) }
            .getOrNull()

    private fun write(f: File, text: String) {
        runCatching {
            val tmp = File(f.parentFile, "${f.name}.tmp")
            tmp.writeText(text)
            // Rename is the atomic step: either the old file or the new one,
            // never half of either.
            if (!tmp.renameTo(f)) {
                f.writeText(text)
                tmp.delete()
            }
        }.onFailure { Log.w(TAG, "could not write ${f.name}", it) }
    }

    private companion object {
        const val TAG = "Stash"
    }
}

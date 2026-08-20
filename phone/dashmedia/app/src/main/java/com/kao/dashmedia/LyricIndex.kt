package com.kao.dashmedia

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import java.util.concurrent.Executors

/**
 * Every lyric in the library, keyed for searching.
 *
 * Searching by lyric means testing each line against a new query on every
 * keypress. Converting text to keypad digits is the expensive part of that, so
 * it happens once per line here and the query is matched against the result --
 * an indexOf per term rather than a re-parse of 456 files.
 *
 * Built in the background on first use and kept for the life of the process,
 * for the same reason the library is: rebuilding it every time the search
 * screen opens would be a second of nothing happening, repeatedly.
 */
object LyricIndex {

    private const val TAG = "LyricIndex"

    /** A line that matched, and the song it came from. */
    class Hit(val track: Track, val line: String)

    private class Doc(val track: Track, val lines: List<String>, val keyed: List<String>)

    @Volatile
    var ready = false
        private set

    @Volatile
    private var building = false

    @Volatile
    private var docs: List<Doc> = emptyList()

    private val io = Executors.newSingleThreadExecutor { r ->
        Thread(r, "lyric-index").apply { isDaemon = true }
    }

    private val main = Handler(Looper.getMainLooper())

    /**
     * Build it if it is not built. [onReady] runs on the main thread when the
     * index becomes usable -- immediately if it already was.
     */
    fun ensureBuilt(context: Context, onReady: () -> Unit) {
        if (ready) { onReady(); return }
        if (building) return
        building = true
        val app = context.applicationContext
        val library = PlayerService.library
        io.execute {
            val t0 = SystemClock.uptimeMillis()
            val built = ArrayList<Doc>(library.size)
            for (t in library) {
                val lrc = Lyrics.loadBlocking(t)
                if (lrc.isEmpty) continue
                // Instrumental gaps are real lines but have no words in them,
                // and an empty line can never match anything.
                val lines = lrc.lines.map { it.text }.filter { it.isNotBlank() }
                if (lines.isEmpty()) continue
                built.add(Doc(t, lines, lines.map { T9.keyLine(it) }))
            }
            val ms = SystemClock.uptimeMillis() - t0
            main.post {
                docs = built
                ready = true
                building = false
                Log.i(TAG, "indexed ${built.size} lyrics in ${ms}ms")
                onReady()
            }
        }
    }

    /**
     * Songs with a line matching [query], and the line that matched.
     *
     * All of a query's terms must land in the *same* line: a song is not a
     * lyric match because one word is in the first verse and another in the
     * last, and the line shown under the result has to be a line that actually
     * says what was asked for.
     */
    fun search(query: String, limit: Int): List<Hit> {
        if (!ready || query.isBlank()) return emptyList()
        val out = ArrayList<Hit>(limit)
        for (d in docs) {
            for (i in d.keyed.indices) {
                if (T9.keyedMatches(query, d.keyed[i])) {
                    out.add(Hit(d.track, d.lines[i]))
                    break          // one hit per song: the first line that says it
                }
            }
            if (out.size >= limit) break
        }
        return out
    }

    /** Forget it, for the rescan in settings. */
    fun clear() {
        docs = emptyList()
        ready = false
    }
}

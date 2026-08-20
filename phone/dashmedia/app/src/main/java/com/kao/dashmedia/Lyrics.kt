package com.kao.dashmedia

import android.content.Context
import android.os.Build
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.util.LruCache
import java.io.File
import java.util.concurrent.Executors

/**
 * Finding and loading the .lrc sitting next to a track.
 *
 * A .lrc is not a media file, so MediaStore will not hand one over no matter
 * what is granted through it -- which is why this reads the path directly and
 * why the app asks for all-files access. Everything about *interpreting* the
 * file is in [LrcParser], which is pure and tested; this half is the
 * filesystem and the cache.
 */
object Lyrics {

    private const val TAG = "Lyrics"

    /** Small: one lyric is on screen, and a couple either side is generous. */
    private val cache = object : LruCache<Long, Lrc>(6) {}

    /** Tracks with no .lrc, so a missing file is not looked for repeatedly. */
    private val misses = HashSet<Long>()

    private val io = Executors.newSingleThreadExecutor { r ->
        Thread(r, "lyrics").apply { isDaemon = true }
    }

    private val main = Handler(Looper.getMainLooper())

    /** Whether the app can actually read a file that is not a media file. */
    fun canRead(): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager()

    fun cached(trackId: Long): Lrc? = cache.get(trackId)

    /**
     * Load the lyric for [track]. [onLoaded] runs on the main thread, possibly
     * before this returns, and receives an empty [Lrc] when there is none.
     */
    fun load(context: Context, track: Track, onLoaded: (Lrc) -> Unit) {
        cache.get(track.id)?.let { onLoaded(it); return }
        if (track.id in misses || track.path.isEmpty()) { onLoaded(EMPTY); return }
        io.execute {
            val text = runCatching { read(track.path) }
                .onFailure { Log.w(TAG, "could not read lyrics for ${track.title}", it) }
                .getOrNull()
            val lrc = LrcParser.parse(text)
            main.post {
                if (lrc.isEmpty) misses.add(track.id) else cache.put(track.id, lrc)
                onLoaded(lrc)
            }
        }
    }

    /**
     * Both spellings people use: `Song.lrc` beside `Song.mp3`, and the
     * `Song.mp3.lrc` that some taggers write.
     */
    private fun read(audioPath: String): String? {
        val f = File(audioPath)
        val stem = f.name.substringBeforeLast('.', f.name)
        for (candidate in listOf(File(f.parentFile, "$stem.lrc"), File("$audioPath.lrc"))) {
            if (candidate.isFile) return candidate.readText()
        }
        return null
    }

    /**
     * A synchronous read, for the lyric index.
     *
     * Never call this from the main thread: it opens and parses a file. The
     * callback version above exists for everything else.
     */
    fun loadBlocking(track: Track): Lrc {
        if (track.path.isEmpty()) return EMPTY
        return LrcParser.parse(runCatching { read(track.path) }.getOrNull())
    }

    /** Forget everything, for the rescan in settings. */
    fun clear() {
        cache.evictAll()
        misses.clear()
    }

    private val EMPTY = Lrc(emptyList())
}

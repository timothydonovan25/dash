package com.kao.dashmedia

import android.content.Context
import android.graphics.Bitmap
import android.net.Uri
import android.os.Handler
import android.os.Looper
import android.util.LruCache
import android.util.Size
import java.util.concurrent.Executors

/**
 * Album art, loaded off the main thread and remembered for a little while.
 *
 * `loadThumbnail` is the sanctioned route on API 29 and up -- the old
 * `content://media/external/audio/albumart` path is deprecated and was never
 * reliable. It reads and decodes a file, so it never happens on the main
 * thread; a 2 GB Go device notices things like that.
 *
 * The cache is deliberately tiny. Only one cover is on screen at a time, and
 * a few neighbours are worth keeping for skipping back and forth; anything
 * more is memory spent on pictures nobody is looking at.
 */
object Art {

    private const val CACHE_ENTRIES = 8

    private val cache = object : LruCache<Long, Bitmap>(CACHE_ENTRIES) {}

    /** Ids known to have no art, so a missing cover is not looked up forever. */
    private val misses = HashSet<Long>()

    private val io = Executors.newSingleThreadExecutor { r ->
        Thread(r, "art").apply { isDaemon = true }
    }

    private val main = Handler(Looper.getMainLooper())

    fun cached(trackId: Long): Bitmap? = cache.get(trackId)

    /**
     * Hand back the cover for [trackId] at [uri]. [onLoaded] runs on the main
     * thread, possibly before this returns if the bitmap is already cached,
     * and receives null when the track has no art.
     */
    fun load(context: Context, trackId: Long, uri: Uri, px: Int, onLoaded: (Bitmap?) -> Unit) {
        cache.get(trackId)?.let { onLoaded(it); return }
        if (trackId in misses) { onLoaded(null); return }
        val app = context.applicationContext
        io.execute {
            val bmp = runCatching {
                app.contentResolver.loadThumbnail(uri, Size(px, px), null)
            }.getOrNull()
            main.post {
                if (bmp != null) cache.put(trackId, bmp) else misses.add(trackId)
                onLoaded(bmp)
            }
        }
    }
}

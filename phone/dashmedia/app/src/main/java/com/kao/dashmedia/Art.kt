package com.kao.dashmedia

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Handler
import android.os.Looper
import android.util.LruCache
import android.util.Size
import java.io.File
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

    /** What a cover file is called, in the order worth trying. */
    private val COVER_NAMES = listOf(
        "cover.jpg", "cover.jpeg", "cover.png",
        "folder.jpg", "folder.jpeg", "folder.png",
        "front.jpg", "album.jpg",
    )

    /**
     * A cover sitting in the album folder.
     *
     * `loadThumbnail` only knows about art embedded in the file, and plenty of
     * rips have none -- The Way of All Flesh has not a single APIC frame in it.
     * A cover.jpg beside the tracks is the other convention, and all-files
     * access (already needed for .lrc) makes it readable.
     */
    private fun folderArt(audioPath: String, px: Int): Bitmap? {
        if (audioPath.isEmpty()) return null
        val dir = File(audioPath).parentFile ?: return null
        val file = COVER_NAMES.asSequence()
            .map { File(dir, it) }
            .firstOrNull { it.isFile } ?: return null
        return runCatching {
            // Measure first: album art is routinely 3000x3000, and decoding
            // that whole thing for a 72dp thumbnail would cost 36 MB and a
            // visible pause on a 2 GB phone.
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeFile(file.path, bounds)
            var sample = 1
            while (bounds.outWidth / (sample * 2) >= px) sample *= 2
            BitmapFactory.decodeFile(
                file.path,
                BitmapFactory.Options().apply { inSampleSize = sample },
            )
        }.getOrNull()
    }

    /**
     * Hand back the cover for [track] -- embedded if the file has any, else a
     * cover file in its folder. [onLoaded] runs on the main thread, possibly
     * before this returns if the bitmap is already cached, and receives null
     * when there is no art either way.
     */
    fun load(context: Context, track: Track, px: Int, onLoaded: (Bitmap?) -> Unit) {
        val trackId = track.id
        cache.get(trackId)?.let { onLoaded(it); return }
        if (trackId in misses) { onLoaded(null); return }
        val app = context.applicationContext
        io.execute {
            val bmp = runCatching {
                app.contentResolver.loadThumbnail(track.uri, Size(px, px), null)
            }.getOrNull() ?: folderArt(track.path, px)
            main.post {
                if (bmp != null) cache.put(trackId, bmp) else misses.add(trackId)
                onLoaded(bmp)
            }
        }
    }
}

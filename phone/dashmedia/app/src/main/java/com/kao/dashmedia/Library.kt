package com.kao.dashmedia

import android.content.Context
import android.net.Uri
import android.provider.MediaStore

/** One playable file on the phone. */
data class Track(
    val id: Long,
    val uri: Uri,
    val title: String,
    val artist: String,
    val album: String,
    val durationMs: Long,
)

/**
 * The on-device library, read through MediaStore.
 *
 * API 30 is scoped-storage-only, so this is the sanctioned way in and it needs
 * READ_EXTERNAL_STORAGE and nothing more -- no MANAGE_EXTERNAL_STORAGE, which
 * would be a much bigger ask for a much smaller gain.
 */
object Library {

    fun tracks(context: Context): List<Track> {
        val cols = arrayOf(
            MediaStore.Audio.Media._ID,
            MediaStore.Audio.Media.TITLE,
            MediaStore.Audio.Media.ARTIST,
            MediaStore.Audio.Media.ALBUM,
            MediaStore.Audio.Media.DURATION,
        )
        val out = ArrayList<Track>(256)
        context.contentResolver.query(
            MediaStore.Audio.Media.EXTERNAL_CONTENT_URI,
            cols,
            "${MediaStore.Audio.Media.IS_MUSIC} != 0",
            null,
            "${MediaStore.Audio.Media.ARTIST} ASC, " +
                "${MediaStore.Audio.Media.ALBUM} ASC, " +
                "${MediaStore.Audio.Media.TRACK} ASC",
        )?.use { c ->
            val idI = c.getColumnIndexOrThrow(MediaStore.Audio.Media._ID)
            val tI = c.getColumnIndexOrThrow(MediaStore.Audio.Media.TITLE)
            val arI = c.getColumnIndexOrThrow(MediaStore.Audio.Media.ARTIST)
            val alI = c.getColumnIndexOrThrow(MediaStore.Audio.Media.ALBUM)
            val dI = c.getColumnIndexOrThrow(MediaStore.Audio.Media.DURATION)
            while (c.moveToNext()) {
                val id = c.getLong(idI)
                out.add(
                    Track(
                        id = id,
                        uri = Uri.withAppendedPath(
                            MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, id.toString()
                        ),
                        title = c.getString(tI) ?: "",
                        // MediaStore says "<unknown>" for a file with no tag.
                        // Do not pass that on to a dashboard.
                        artist = clean(c.getString(arI)),
                        album = clean(c.getString(alI)),
                        durationMs = c.getLong(dI),
                    )
                )
            }
        }
        return out
    }

    private fun clean(s: String?): String =
        if (s == null || s == MediaStore.UNKNOWN_STRING) "" else s
}

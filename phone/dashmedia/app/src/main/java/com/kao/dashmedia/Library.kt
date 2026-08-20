package com.kao.dashmedia

import android.content.Context
import android.net.Uri
import android.provider.MediaStore

/** One playable file on the phone. */
data class Track(
    val id: Long,
    val uri: Uri,
    val title: String,
    override val artist: String,
    override val album: String,
    val durationMs: Long,
    /** Seconds since epoch, from MediaStore. Drives "recently added". */
    val addedSec: Long,
    /**
     * The file's path on disk.
     *
     * DATA is deprecated and scoped storage is the reason, but a .lrc sitting
     * beside the mp3 is not a media file and cannot be reached through
     * MediaStore at all -- so with all-files access granted, the path is the
     * only way to find it.
     */
    val path: String,
) : Tagged

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
            MediaStore.Audio.Media.DATE_ADDED,
            @Suppress("DEPRECATION") MediaStore.Audio.Media.DATA,
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
            val addI = c.getColumnIndexOrThrow(MediaStore.Audio.Media.DATE_ADDED)
            @Suppress("DEPRECATION")
            val pathI = c.getColumnIndexOrThrow(MediaStore.Audio.Media.DATA)
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
                        addedSec = c.getLong(addI),
                        path = c.getString(pathI) ?: "",
                    )
                )
            }
        }
        return out
    }

    private fun clean(s: String?): String =
        if (s == null || s == MediaStore.UNKNOWN_STRING) "" else s

    /**
     * The user's playlists.
     *
     * MediaStore.Audio.Playlists is deprecated from API 31 onwards, and this
     * app targets 30 on a phone running 30, where it is the only way to see
     * playlists made by other apps -- and there are twelve of them on this
     * handset. If the app is ever retargeted, this is the first thing that
     * will stop working.
     */
    @Suppress("DEPRECATION")
    fun playlists(context: Context): List<Playlist> {
        val cols = arrayOf(MediaStore.Audio.Playlists._ID, MediaStore.Audio.Playlists.NAME)
        val out = ArrayList<Playlist>(16)
        context.contentResolver.query(
            MediaStore.Audio.Playlists.EXTERNAL_CONTENT_URI,
            cols,
            null,
            null,
            "${MediaStore.Audio.Playlists.NAME} COLLATE NOCASE ASC",
        )?.use { c ->
            val idI = c.getColumnIndexOrThrow(MediaStore.Audio.Playlists._ID)
            val nI = c.getColumnIndexOrThrow(MediaStore.Audio.Playlists.NAME)
            while (c.moveToNext()) out.add(Playlist(c.getLong(idI), c.getString(nI) ?: ""))
        }
        return out
    }

    /**
     * A playlist's tracks, in the order they were put there.
     *
     * Resolved against [byId] rather than re-querying every column: the
     * library is already in memory, and a member that is not in it -- deleted
     * since, or not audio -- is simply dropped rather than becoming a row that
     * cannot play.
     */
    @Suppress("DEPRECATION")
    fun playlistTracks(context: Context, playlistId: Long, byId: Map<Long, Track>): List<Track> {
        val uri = MediaStore.Audio.Playlists.Members.getContentUri("external", playlistId)
        val out = ArrayList<Track>(64)
        context.contentResolver.query(
            uri,
            arrayOf(MediaStore.Audio.Playlists.Members.AUDIO_ID),
            null,
            null,
            "${MediaStore.Audio.Playlists.Members.PLAY_ORDER} ASC",
        )?.use { c ->
            val aI = c.getColumnIndexOrThrow(MediaStore.Audio.Playlists.Members.AUDIO_ID)
            while (c.moveToNext()) byId[c.getLong(aI)]?.let { out.add(it) }
        }
        return out
    }
}

/** A MediaStore playlist. Count is filled in lazily by the browse screen. */
data class Playlist(val id: Long, val name: String)

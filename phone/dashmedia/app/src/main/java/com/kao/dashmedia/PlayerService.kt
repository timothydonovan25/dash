package com.kao.dashmedia

import android.os.Handler
import android.os.Looper
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService

/**
 * Playback, and the owner of the link to the dash.
 *
 * Those two live together because of Android Go: background processes are cut
 * aggressively, and a MediaSessionService is a foreground service whenever
 * something is playing. Tying the BLE link to it means the link is alive
 * exactly when there is anything worth sending.
 *
 * Two possible sources of truth feed the dash, and only one is used at a time:
 *
 *  - [MediaWatcher], when notification access is granted. It sees every app,
 *    including this one, so it is strictly better and wins.
 *  - the [Player.Listener] below, when it is not. Our own playback still
 *    reaches the dash; Spotify does not.
 */
class PlayerService : MediaSessionService() {

    private lateinit var player: ExoPlayer
    private var session: MediaSession? = null

    private val handler = Handler(Looper.getMainLooper())
    private val feed = Feed()
    private lateinit var link: DashLink
    private var watcher: MediaWatcher? = null

    /** Last thing the settings screen was told, for display only. */
    @Volatile
    var status: String = "idle"
        private set

    override fun onCreate() {
        super.onCreate()

        player = ExoPlayer.Builder(this).build()
        session = MediaSession.Builder(this, player).build()

        link = DashLink(this, feed) { s ->
            status = s
            handler.post { listeners.forEach { it(s) } }
        }
        link.start()

        if (MediaWatcher.hasAccess(this)) {
            watcher = MediaWatcher(this, handler) { publish(it) }.also { it.start() }
        } else {
            player.addListener(ownPlaybackListener)
        }
    }

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo): MediaSession? = session

    override fun onDestroy() {
        watcher?.stop()
        link.stop()
        session?.run {
            player.release()
            release()
        }
        session = null
        super.onDestroy()
    }

    /** Everything the dash learns goes through here. */
    private fun publish(s: Snapshot) {
        link.push(feed.update(s))
    }

    private val ownPlaybackListener = object : Player.Listener {
        override fun onMediaMetadataChanged(mediaMetadata: MediaMetadata) = emitOwn()
        override fun onIsPlayingChanged(isPlaying: Boolean) = emitOwn()
        override fun onPlaybackStateChanged(playbackState: Int) = emitOwn()
    }

    private fun emitOwn() {
        val m = player.mediaMetadata
        publish(
            Snapshot(
                title = m.title?.toString().orEmpty(),
                artist = m.artist?.toString() ?: m.albumArtist?.toString().orEmpty(),
                album = m.albumTitle?.toString().orEmpty(),
                state = when {
                    player.isPlaying -> PlayState.Playing
                    player.playbackState == Player.STATE_IDLE ||
                        player.playbackState == Player.STATE_ENDED -> PlayState.Stopped
                    else -> PlayState.Paused
                },
            )
        )
    }

    companion object {
        /** Status text for the settings screen. Main thread only. */
        val listeners = ArrayList<(String) -> Unit>()

        /** MediaStore rows to something ExoPlayer will play. */
        fun items(tracks: List<Track>): List<MediaItem> = tracks.map { t ->
            MediaItem.Builder()
                .setUri(t.uri)
                .setMediaId(t.id.toString())
                .setMediaMetadata(
                    MediaMetadata.Builder()
                        .setTitle(t.title)
                        .setArtist(t.artist)
                        .setAlbumTitle(t.album)
                        .build()
                )
                .build()
        }
    }
}

package com.kao.dashmedia

import android.os.Handler
import android.os.Looper
import android.media.audiofx.BassBoost
import android.media.audiofx.LoudnessEnhancer
import androidx.annotation.OptIn
import androidx.media3.common.AudioAttributes
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.Player
import androidx.media3.common.util.UnstableApi
import androidx.media3.exoplayer.DefaultRenderersFactory
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.audio.AudioSink
import androidx.media3.exoplayer.audio.DefaultAudioSink
import androidx.media3.session.MediaSession
import androidx.media3.session.MediaSessionService

/**
 * Playback, the library, the queue, and the link to the dash.
 *
 * These live together because of Android Go: background processes are cut
 * aggressively, and a MediaSessionService is a foreground service whenever
 * something is playing, so tying the BLE link to it means the link is alive
 * exactly when there is anything worth sending.
 *
 * The screens reach this through [instance] rather than through media3 custom
 * commands. Everything here is one process on one phone, and a session
 * command round-trip to change a queue we already own would be ceremony for
 * its own sake. The controller is still what plays, pauses and seeks -- only
 * the things media3 has no vocabulary for (the library, the sleep timer)
 * come through here.
 */
class PlayerService : MediaSessionService() {

    private lateinit var player: ExoPlayer
    private var session: MediaSession? = null

    private val handler = Handler(Looper.getMainLooper())
    private val feed = Feed()
    private lateinit var link: DashLink
    private lateinit var prefs: Prefs
    private var watcher: MediaWatcher? = null


    /** What the current queue was built from. Null before anything is queued. */
    var source: Source? = null
        private set


    /** Wall-clock ms at which the sleep timer fires, or 0 for off. */
    var sleepAtMs: Long = 0L
        private set


    /** The in-flight queue fill, if any. */
    private var fill: Runnable? = null

    private val trimmer = LeadingSilenceProcessor()

    private var bass: BassBoost? = null
    private var loudness: LoudnessEnhancer? = null

    /** Set by the sleep menu: stop when the current track ends. */
    var sleepAtEndOfTrack: Boolean = false
        private set

    @Volatile
    var linkStatus: String = "starting"
        private set

    override fun onCreate() {
        val tStart = android.os.SystemClock.uptimeMillis()
        super.onCreate()
        instance = this
        prefs = Prefs(this)

        trimmer.enabled = prefs.trimSilence
        player = buildPlayer().apply {
            // The queue always loops. The middle softkey chooses whether it
            // loops in order or shuffled, which is all shuffleModeEnabled is.
            repeatMode = Player.REPEAT_MODE_ALL
            shuffleModeEnabled = prefs.shuffle
            // handleAudioFocus: without it the music plays straight through an
            // incoming call, which on a phone is not a preference.
            setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(C.USAGE_MEDIA)
                    .setContentType(C.AUDIO_CONTENT_TYPE_MUSIC)
                    .build(),
                /* handleAudioFocus= */ true,
            )
        }
        session = MediaSession.Builder(this, player).build()

        link = DashLink(this, feed) { s ->
            linkStatus = s
            handler.post { listeners.toList().forEach { it() } }
        }
        link.start()

        if (MediaWatcher.hasAccess(this)) {
            watcher = MediaWatcher(this, handler) { publish(it) }.also { it.start() }
        } else {
            player.addListener(ownPlaybackListener)
        }
        player.addListener(bookkeeping)

        loadLibrary()
        handler.post(ticker)
        android.util.Log.i("Timing", "service onCreate total=" +
            "${android.os.SystemClock.uptimeMillis() - tStart}ms")
    }

    /**
     * A player with the leading-silence trimmer spliced into its audio sink.
     * That is the only reason a custom renderers factory exists here.
     */
    @OptIn(UnstableApi::class)
    private fun buildPlayer(): ExoPlayer {
        val renderers = object : DefaultRenderersFactory(this) {
            override fun buildAudioSink(
                context: android.content.Context,
                enableFloatOutput: Boolean,
                enableAudioTrackPlaybackParams: Boolean,
            ): AudioSink = DefaultAudioSink.Builder(context)
                .setAudioProcessors(arrayOf(trimmer))
                .build()
        }
        return ExoPlayer.Builder(this, renderers).build()
    }

    override fun onGetSession(controllerInfo: MediaSession.ControllerInfo): MediaSession? = session

    override fun onDestroy() {
        saveResume()
        releaseEffects()
        handler.removeCallbacksAndMessages(null)
        watcher?.stop()
        link.stop()
        session?.run {
            player.release()
            release()
        }
        session = null
        instance = null
        super.onDestroy()
    }

    // ---- library and queue ----------------------------------------------

    private fun loadLibrary() {
        // A MediaSessionService is stopped as soon as nothing is bound to it
        // and nothing is playing -- which happens every time the screen sleeps
        // -- and it used to re-read the whole library on the way back. The
        // cache is on the companion so it outlives the service and only a
        // process death pays for it again.
        if (libraryReady) {
            restoreResume()
            notifyUi()
            return
        }
        // Off the main thread: it is a third of a second of ContentResolver on
        // this phone, and the browse list cannot draw until it lands.
        Thread {
            val t0 = android.os.SystemClock.uptimeMillis()
            val lib = Library.tracks(this)
            val index = lib.associateBy { it.id }
            // Counted here rather than on demand: it is a query per playlist,
            // and the browse list wants all of them at once.
            val lists = Library.playlists(this)
            val counts = lists.associate {
                it.id to Library.playlistTracks(this, it.id, index).size
            }
            val ms = android.os.SystemClock.uptimeMillis() - t0
            handler.post {
                library = lib
                byId = index
                playlists = lists
                playlistCounts = counts
                libraryReady = true
                android.util.Log.i("Timing",
                    "query=${ms}ms rows=${lib.size} playlists=${lists.size}")
                restoreResume()
                notifyUi()
            }
        }.also { it.name = "library" }.start()
    }

    /** Play [tracks] starting at [index]. Used by the browse screens. */
    fun playFrom(tracks: List<Track>, index: Int, from: Source) {
        source = from
        prefs.source = from
        setQueue(tracks, index, 0L, play = true)
    }

    /** The tracks a [Source] stands for, resolved against the loaded library. */
    fun tracksFor(s: Source): List<Track> = when (s) {
        is Source.All -> library
        is Source.Artist -> Browse.tracksBy(library, s.name)
        is Source.Album -> Browse.tracksOf(library, s.artist, s.title)
        is Source.Playlist -> Library.playlistTracks(this, s.id, byId)
    }

    fun shuffling(): Boolean = player.shuffleModeEnabled

    fun trimSilence(): Boolean = trimmer.enabled

    fun setTrimSilence(on: Boolean) {
        trimmer.enabled = on
        prefs.trimSilence = on
        notifyUi()
    }

    fun boost(): Int = prefs.boost

    /**
     * Bass and loudness, for wind noise.
     *
     * Helmet speakers at speed lose the bottom of the range first, and the
     * phone's own volume ceiling is the limit you hit before the music is
     * loud enough. Both effects are system AudioEffects on the player's
     * session.
     *
     * They are also the part most likely to be missing or refused on any given
     * handset, so a failure here disables the effect and leaves the music
     * playing rather than taking the app down with it.
     */
    fun setBoost(level: Int) {
        prefs.boost = level.coerceIn(0, 3)
        applyBoost()
        notifyUi()
    }

    private fun applyBoost() {
        val level = prefs.boost
        val id = player.audioSessionId
        if (id == C.AUDIO_SESSION_ID_UNSET) return
        releaseEffects()
        if (level <= 0) return
        // strength 0..1000, gain in millibels.
        val strength = shortArrayOf(0, 350, 650, 1000)[level]
        val gainMb = intArrayOf(0, 300, 700, 1200)[level]
        try {
            bass = BassBoost(0, id).apply {
                if (strengthSupported) setStrength(strength)
                enabled = true
            }
        } catch (e: Exception) {
            android.util.Log.w(TAG, "bass boost unavailable", e)
            bass = null
        }
        try {
            loudness = LoudnessEnhancer(id).apply {
                setTargetGain(gainMb)
                enabled = true
            }
        } catch (e: Exception) {
            android.util.Log.w(TAG, "loudness enhancer unavailable", e)
            loudness = null
        }
    }

    private fun releaseEffects() {
        runCatching { bass?.release() }
        runCatching { loudness?.release() }
        bass = null
        loudness = null
    }

    /** Sleep when the current track ends, rather than after a fixed time. */
    fun setSleepAtEndOfTrack() {
        handler.removeCallbacks(sleep)
        sleepAtMs = 0L
        sleepAtEndOfTrack = true
        notifyUi()
    }

    /**
     * Queue [tracks] at [index] without paying for all of them first.
     *
     * `setMediaItems` builds a media source per entry, which is 2.3 seconds for
     * a 522-track library on this phone -- and it was being paid at startup
     * merely to restore one song. So the chosen track goes in alone, which is
     * immediate, and the rest is added around it afterwards in chunks. Adding
     * to a queue does not disturb what is already playing, and the looper gets
     * a turn between batches, so neither the audio nor the UI stalls.
     */
    private fun setQueue(tracks: List<Track>, index: Int, positionMs: Long, play: Boolean) {
        cancelFill()
        if (tracks.isEmpty()) return
        val i = index.coerceIn(0, tracks.size - 1)
        player.setMediaItems(items(listOf(tracks[i])), 0, positionMs)
        player.prepare()
        if (play) player.play()
        if (tracks.size > 1) {
            // What plays next matters more than what played before, so the
            // tail goes in first.
            fillOutward(before = tracks.subList(0, i), after = tracks.subList(i + 1, tracks.size))
        }
    }

    private fun fillOutward(before: List<Track>, after: List<Track>) {
        var ai = 0
        var bi = before.size
        val step = 40
        val r = object : Runnable {
            override fun run() {
                when {
                    ai < after.size -> {
                        val end = minOf(ai + step, after.size)
                        player.addMediaItems(items(after.subList(ai, end)))
                        ai = end
                    }
                    bi > 0 -> {
                        val start = maxOf(0, bi - step)
                        // Inserting ahead of the current item shifts its index;
                        // ExoPlayer keeps track of that itself.
                        player.addMediaItems(0, items(before.subList(start, bi)))
                        bi = start
                    }
                    else -> {
                        fill = null
                        android.util.Log.i("Timing", "queue filled to ${player.mediaItemCount}")
                        return
                    }
                }
                handler.post(this)
            }
        }
        fill = r
        handler.post(r)
    }

    private fun cancelFill() {
        fill?.let { handler.removeCallbacks(it) }
        fill = null
    }

    private fun currentTrackId(): Long? =
        player.currentMediaItem?.mediaId?.toLongOrNull()

    // ---- sleep timer -----------------------------------------------------

    /** [minutes] of 0 cancels. */
    fun setSleepTimer(minutes: Int) {
        handler.removeCallbacks(sleep)
        sleepAtEndOfTrack = false
        sleepAtMs = if (minutes <= 0) 0L else System.currentTimeMillis() + minutes * 60_000L
        if (sleepAtMs != 0L) handler.postDelayed(sleep, minutes * 60_000L)
        notifyUi()
    }

    private val sleep = Runnable {
        player.pause()
        sleepAtMs = 0L
        notifyUi()
    }

    // ---- resume ----------------------------------------------------------

    private fun restoreResume() {
        val id = prefs.resumeId
        if (id < 0 || library.isEmpty()) return
        val src = prefs.source ?: Source.All
        val tracks = tracksFor(src)
        val idx = tracks.indexOfFirst { it.id == id }
        if (idx < 0) return
        source = src
        val t0 = android.os.SystemClock.uptimeMillis()
        // Queued and seeked but deliberately not started. Coming back to a
        // phone that is suddenly playing out loud is worse than pressing OK.
        setQueue(tracks, idx, prefs.resumePositionMs, play = false)
        android.util.Log.i("Timing", "resume=${android.os.SystemClock.uptimeMillis() - t0}ms")
    }

    private fun saveResume() {
        val id = currentTrackId() ?: return
        prefs.saveResume(id, player.currentPosition.coerceAtLeast(0L))
    }

    private val ticker = object : Runnable {
        override fun run() {
            if (player.isPlaying) saveResume()
            handler.postDelayed(this, 5_000)
        }
    }

    private val bookkeeping = object : Player.Listener {
        override fun onMediaItemTransition(mediaItem: MediaItem?, reason: Int) {
            // "Sleep after this track" lands here, at the moment the next one
            // starts. Pausing at position zero means what you come back to is
            // the start of the next track rather than a second of it already
            // gone.
            if (sleepAtEndOfTrack && reason == Player.MEDIA_ITEM_TRANSITION_REASON_AUTO) {
                sleepAtEndOfTrack = false
                player.pause()
                player.seekTo(0L)
            }
            saveResume()
            notifyUi()
        }

        override fun onAudioSessionIdChanged(audioSessionId: Int) {
            // The effects bind to a session id that does not exist until the
            // player has one, and changes if the sink is rebuilt.
            applyBoost()
        }

        override fun onIsPlayingChanged(isPlaying: Boolean) = notifyUi()

        /**
         * The middle softkey flips this through the MediaController, so the
         * change arrives here rather than being handed in. Saving it from the
         * player means the stored value is whatever the player actually has.
         */
        override fun onShuffleModeEnabledChanged(shuffleModeEnabled: Boolean) {
            prefs.shuffle = shuffleModeEnabled
            notifyUi()
        }
    }

    // ---- the dash --------------------------------------------------------

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

    private fun notifyUi() {
        handler.post { listeners.toList().forEach { it() } }
    }

    companion object {
        private const val TAG = "PlayerService"

        /**
         * The library, cached for the life of the process rather than the life
         * of the service. See [loadLibrary].
         */
        @Volatile
        @JvmStatic
        var library: List<Track> = emptyList()
            private set

        @Volatile
        @JvmStatic
        var playlists: List<Playlist> = emptyList()
            private set

        @Volatile
        @JvmStatic
        var playlistCounts: Map<Long, Int> = emptyMap()
            private set

        /** False until the MediaStore query has come back, so a list can say so. */
        @Volatile
        @JvmStatic
        var libraryReady = false
            private set

        /** Index into [library] by MediaStore id, for resolving playlist members. */
        @Volatile
        private var byId: Map<Long, Track> = emptyMap()

        /** Single process, single phone. See the class comment. */
        @Volatile
        @JvmStatic
        var instance: PlayerService? = null
            private set

        /** Screens register to redraw their chrome. Main thread only. */
        val listeners = ArrayList<() -> Unit>()

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

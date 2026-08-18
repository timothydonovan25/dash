package com.kao.dashmedia

import android.content.ComponentName
import android.content.Context
import android.media.MediaMetadata
import android.media.session.MediaController
import android.media.session.MediaSessionManager
import android.media.session.PlaybackState
import android.os.Handler
import android.provider.Settings
import android.util.Log

/**
 * Turns whatever is playing on the phone into a [Snapshot].
 *
 * This is what makes the dash useful for more than our own library: Spotify,
 * a podcast app, anything that publishes a MediaSession shows up on the
 * handlebars. It needs notification-listener access ([NotifListener]) and
 * degrades to nothing without it -- our own playback reaches the dash by a
 * different route ([PlayerService] pushes its own session directly), so the
 * app is still worth having before the grant.
 */
class MediaWatcher(
    private val context: Context,
    private val handler: Handler,
    private val onSnapshot: (Snapshot) -> Unit,
) {

    companion object {
        private const val TAG = "MediaWatcher"

        /** Has the user granted notification access to this app? */
        fun hasAccess(context: Context): Boolean {
            val flat = Settings.Secure.getString(
                context.contentResolver, "enabled_notification_listeners"
            ) ?: return false
            val me = context.packageName
            return flat.split(':').any {
                ComponentName.unflattenFromString(it)?.packageName == me
            }
        }
    }

    private val manager: MediaSessionManager? =
        context.getSystemService(Context.MEDIA_SESSION_SERVICE) as? MediaSessionManager

    private val component = ComponentName(context, NotifListener::class.java)

    private var watched: MediaController? = null

    private val controllerCallback = object : MediaController.Callback() {
        override fun onMetadataChanged(metadata: MediaMetadata?) = emit()
        override fun onPlaybackStateChanged(state: PlaybackState?) = emit()
        override fun onSessionDestroyed() { rescan(null) }
    }

    private val sessionsCallback =
        MediaSessionManager.OnActiveSessionsChangedListener { list -> rescan(list) }

    fun start() {
        if (!hasAccess(context)) {
            Log.i(TAG, "no notification access; only our own playback reaches the dash")
            return
        }
        val m = manager ?: return
        try {
            m.addOnActiveSessionsChangedListener(sessionsCallback, component, handler)
            rescan(m.getActiveSessions(component))
        } catch (e: SecurityException) {
            // The grant can be revoked while we run. Not fatal, and not worth
            // taking the player down for.
            Log.w(TAG, "session access refused", e)
        }
    }

    fun stop() {
        manager?.removeOnActiveSessionsChangedListener(sessionsCallback)
        watched?.unregisterCallback(controllerCallback)
        watched = null
    }

    /**
     * Pick one session out of however many exist. Preference is whatever is
     * actually playing; a paused session only wins if nothing is.
     */
    private fun rescan(list: List<MediaController>?) {
        val sessions = list ?: try {
            manager?.getActiveSessions(component)
        } catch (e: SecurityException) {
            null
        } ?: emptyList()

        val pick = sessions.firstOrNull {
            it.playbackState?.state == PlaybackState.STATE_PLAYING
        } ?: sessions.firstOrNull()

        if (pick?.sessionToken == watched?.sessionToken) {
            emit()
            return
        }
        watched?.unregisterCallback(controllerCallback)
        watched = pick
        watched?.registerCallback(controllerCallback, handler)
        emit()
    }

    private fun emit() {
        val c = watched
        if (c == null) {
            onSnapshot(Snapshot())
            return
        }
        val m = c.metadata
        onSnapshot(
            Snapshot(
                title = m?.getString(MediaMetadata.METADATA_KEY_TITLE).orEmpty(),
                artist = m?.getString(MediaMetadata.METADATA_KEY_ARTIST)
                    ?: m?.getString(MediaMetadata.METADATA_KEY_ALBUM_ARTIST).orEmpty(),
                album = m?.getString(MediaMetadata.METADATA_KEY_ALBUM).orEmpty(),
                state = when (c.playbackState?.state) {
                    PlaybackState.STATE_PLAYING,
                    PlaybackState.STATE_BUFFERING -> PlayState.Playing
                    PlaybackState.STATE_PAUSED -> PlayState.Paused
                    else -> PlayState.Stopped
                },
            )
        )
    }
}

package com.kao.dashmedia.ui

import android.app.Activity
import android.content.ComponentName
import android.media.AudioManager
import android.media.session.MediaSessionManager
import android.os.Bundle
import android.os.SystemClock
import android.view.KeyEvent
import androidx.core.content.ContextCompat
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import com.kao.dashmedia.MediaWatcher
import com.kao.dashmedia.NotifListener
import com.kao.dashmedia.PlayerService

/**
 * The side button, as a play/pause key.
 *
 * The XP3900's programmable keys do not deliver a keycode to anything -- the
 * Programmable Keys settings screen binds each one to an *app*, which is why
 * one of them opens DashMedia and the other opened the torch. So the only way
 * to put play/pause on a side button is to be an app that, when launched,
 * toggles playback and gets out of the way.
 *
 * Hence a second launcher entry. It has no interface, appears for a fraction
 * of a second, and costs one extra icon in the app drawer, which is the price
 * of the mechanism rather than a choice.
 *
 * It prefers dispatching a media key, so the button governs whatever is
 * actually playing -- Spotify and podcasts included -- and falls back to
 * waking our own player when nothing holds a session, so pressing it cold
 * resumes where you left off instead of doing nothing.
 */
class PlayPauseActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (dispatchToActiveSession()) {
            done()
        } else {
            wakeOwnPlayer()
        }
    }

    /**
     * True if something was listening. A media key goes to whichever session
     * is active, which is what makes this a play/pause button rather than a
     * DashMedia button.
     */
    private fun dispatchToActiveSession(): Boolean {
        if (!hasActiveSession()) return false
        val audio = getSystemService(AudioManager::class.java) ?: return false
        val now = SystemClock.uptimeMillis()
        return runCatching {
            for (action in intArrayOf(KeyEvent.ACTION_DOWN, KeyEvent.ACTION_UP)) {
                audio.dispatchMediaKeyEvent(
                    KeyEvent(now, now, action, KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, 0)
                )
            }
            true
        }.getOrDefault(false)
    }

    /**
     * Whether any app holds a media session right now.
     *
     * Needs notification-listener access, which this app already asks for so
     * that other players reach the dash. Without it, assume nothing is playing
     * and go straight to our own player -- worse for Spotify, still correct
     * for us.
     */
    private fun hasActiveSession(): Boolean {
        if (!MediaWatcher.hasAccess(this)) return false
        val mgr = getSystemService(MediaSessionManager::class.java) ?: return false
        return runCatching {
            mgr.getActiveSessions(ComponentName(this, NotifListener::class.java)).isNotEmpty()
        }.getOrDefault(false)
    }

    /** Nothing is playing anywhere: start ours, which resumes the last track. */
    private fun wakeOwnPlayer() {
        val token = SessionToken(this, ComponentName(this, PlayerService::class.java))
        val future = MediaController.Builder(this, token).buildAsync()
        future.addListener({
            runCatching {
                val c = future.get()
                if (c.isPlaying) c.pause() else {
                    c.prepare()
                    c.play()
                }
                MediaController.releaseFuture(future)
            }
            done()
        }, ContextCompat.getMainExecutor(this))
    }

    private fun done() {
        finish()
        // No slide-in for a window nobody is meant to see.
        @Suppress("DEPRECATION")
        overridePendingTransition(0, 0)
    }
}

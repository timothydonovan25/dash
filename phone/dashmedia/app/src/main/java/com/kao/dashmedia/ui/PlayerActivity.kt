package com.kao.dashmedia.ui

import android.content.ComponentName
import android.os.Bundle
import android.util.Log
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import com.google.common.util.concurrent.ListenableFuture
import com.kao.dashmedia.PlayerService

/**
 * Holds the connection to [PlayerService] for the screens that need it.
 *
 * The controller is bound in onStart and dropped in onStop, so a screen left
 * open on a phone in a pocket is not holding the service up.
 */
abstract class PlayerActivity : AppCompatActivity() {

    protected var controller: MediaController? = null
        private set

    private var future: ListenableFuture<MediaController>? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        volumeControlStream = android.media.AudioManager.STREAM_MUSIC
    }

    override fun onStart() {
        super.onStart()
        val token = SessionToken(this, ComponentName(this, PlayerService::class.java))
        val f = MediaController.Builder(this, token).buildAsync()
        future = f
        f.addListener({
            // The future completes even when the bind failed, and get() then
            // throws. A phone with no service to connect to is not a crash.
            try {
                if (!f.isCancelled) {
                    controller = f.get()
                    controller?.let { onController(it) }
                }
            } catch (e: Exception) {
                Log.w("PlayerActivity", "no session", e)
            }
        }, ContextCompat.getMainExecutor(this))
    }

    override fun onStop() {
        future?.let { MediaController.releaseFuture(it) }
        future = null
        controller = null
        super.onStop()
    }

    /** Called on the main thread once the controller is live. */
    protected open fun onController(c: MediaController) {}
}

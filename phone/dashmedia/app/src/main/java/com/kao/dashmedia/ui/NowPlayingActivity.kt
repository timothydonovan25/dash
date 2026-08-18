package com.kao.dashmedia.ui

import android.content.Intent
import android.os.Bundle
import android.view.KeyEvent
import android.widget.TextView
import androidx.media3.common.Player
import androidx.media3.session.MediaController
import com.kao.dashmedia.R

/**
 * What is playing, and whether the dash knows about it.
 *
 * The D-pad is the transport: left is previous, right is next, centre is
 * play/pause. Those are the same three commands the reserved control
 * characteristic would carry from the handlebars, which is not a coincidence.
 */
class NowPlayingActivity : PlayerActivity() {

    private lateinit var title: TextView
    private lateinit var artist: TextView
    private lateinit var album: TextView
    private lateinit var state: TextView
    private lateinit var link: TextView

    private val listener = object : Player.Listener {
        override fun onEvents(player: Player, events: Player.Events) = draw()
    }

    private val onStatus: (String) -> Unit = { s -> link.text = s }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_now_playing)
        title = findViewById(R.id.title)
        artist = findViewById(R.id.artist)
        album = findViewById(R.id.album)
        state = findViewById(R.id.state)
        link = findViewById(R.id.link)
    }

    override fun onStart() {
        super.onStart()
        com.kao.dashmedia.PlayerService.listeners.add(onStatus)
    }

    override fun onStop() {
        com.kao.dashmedia.PlayerService.listeners.remove(onStatus)
        controller?.removeListener(listener)
        super.onStop()
    }

    override fun onController(c: MediaController) {
        c.addListener(listener)
        draw()
    }

    private fun draw() {
        val c = controller ?: return
        val m = c.mediaMetadata
        title.text = m.title ?: getString(R.string.nothing_playing)
        artist.text = m.artist ?: ""
        album.text = m.albumTitle ?: ""
        state.setText(if (c.isPlaying) R.string.playing else R.string.paused)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val c = controller ?: return super.onKeyDown(keyCode, event)
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT -> { c.seekToPreviousMediaItem(); return true }
            KeyEvent.KEYCODE_DPAD_RIGHT -> { c.seekToNextMediaItem(); return true }
            KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER -> {
                if (c.isPlaying) c.pause() else c.play()
                return true
            }
            KeyEvent.KEYCODE_0 -> {
                startActivity(Intent(this, SettingsActivity::class.java))
                return true
            }
        }
        return super.onKeyDown(keyCode, event)
    }
}

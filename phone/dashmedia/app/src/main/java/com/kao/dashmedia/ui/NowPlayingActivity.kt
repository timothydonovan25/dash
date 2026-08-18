package com.kao.dashmedia.ui

import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.KeyEvent
import android.view.View
import android.widget.ImageView
import android.widget.ProgressBar
import android.widget.TextView
import androidx.media3.common.Player
import androidx.media3.session.MediaController
import com.kao.dashmedia.Art
import com.kao.dashmedia.Fmt
import com.kao.dashmedia.Keys
import com.kao.dashmedia.PlayerService
import com.kao.dashmedia.R

/**
 * What is playing, with a position you can move.
 *
 * The D-pad is the transport. Left and right skip on a tap and scrub on a
 * hold, which is the only way to get both onto a keypad that has four
 * directions and no spare keys -- and it matches how every feature phone has
 * behaved for twenty years, so it needs no explaining.
 */
class NowPlayingActivity : PlayerActivity() {

    private lateinit var title: TextView
    private lateinit var artist: TextView
    private lateinit var album: TextView
    private lateinit var elapsed: TextView
    private lateinit var total: TextView
    private lateinit var bar: ProgressBar
    private lateinit var state: TextView
    private lateinit var art: ImageView

    /** Which track the cover on screen belongs to, so a late load is ignored. */
    private var artFor: Long = -1L

    private val ui = Handler(Looper.getMainLooper())

    /**
     * How fast a held left/right moves through the track, in playback seconds
     * per real second of holding.
     *
     * Not a fixed step per key repeat: Android repeats a held key about every
     * 50 ms, so any fixed step is multiplied by twenty and a 5 s step scrubs at
     * a hundred times speed. Rate times elapsed time is the same however the
     * device chooses to repeat.
     */
    private val scrubRate = 8

    /** After [accelerateAfterMs] of holding, this rate takes over. */
    private val scrubRateFast = 32
    private val accelerateAfterMs = 2_000L

    /** Set while a hold is scrubbing, so the key-up knows it was not a tap. */
    private var scrubbed = false

    /** uptime of the last scrub step, for the elapsed-time calculation. */
    private var lastScrubMs = 0L

    /** uptime the current hold began, for deciding when to accelerate. */
    private var holdStartMs = 0L

    private val listener = object : Player.Listener {
        override fun onEvents(player: Player, events: Player.Events) = draw()
    }

    private val tick = object : Runnable {
        override fun run() {
            drawPosition()
            ui.postDelayed(this, 500)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_now_playing)
        title = findViewById(R.id.title)
        artist = findViewById(R.id.artist)
        album = findViewById(R.id.album)
        elapsed = findViewById(R.id.elapsed)
        total = findViewById(R.id.total)
        bar = findViewById(R.id.bar)
        state = findViewById(R.id.state)
        art = findViewById(R.id.art)
    }

    override fun onController(c: MediaController) {
        c.addListener(listener)
        draw()
    }

    override fun onResume() {
        super.onResume()
        ui.post(tick)
    }

    override fun onPause() {
        ui.removeCallbacks(tick)
        super.onPause()
    }

    override fun onStop() {
        controller?.removeListener(listener)
        super.onStop()
    }

    override fun onSoftRight() {
        // Right softkey swaps back to the library rather than stacking another
        // copy of it on top of this one.
        finish()
    }

    override fun softLabels() =
        Triple(getString(R.string.k_settings), modeLabel(), getString(R.string.k_library))

    override fun onChrome() = draw()

    private fun draw() {
        val c = controller ?: return
        val m = c.mediaMetadata
        title.text = m.title ?: getString(R.string.nothing_playing)
        artist.text = m.artist ?: ""
        album.text = m.albumTitle ?: ""
        // A playback error is worth more than the word "Paused", which is what
        // the player looks like from the outside when a file will not open.
        val err = PlayerService.instance?.lastError
        if (err != null) {
            state.text = err
            state.setTextColor(getColor(R.color.accent))
        } else {
            state.setText(if (c.isPlaying) R.string.playing else R.string.paused)
            state.setTextColor(getColor(R.color.text))
        }
        drawArt()
        drawPosition()
    }

    private fun drawPosition() {
        val c = controller ?: return
        val dur = c.duration
        val pos = c.currentPosition.coerceAtLeast(0L)
        // duration is C.TIME_UNSET (a large negative) until the item is ready.
        val known = dur > 0
        bar.max = if (known) (dur / 1000).toInt() else 1
        bar.progress = if (known) (pos / 1000).toInt().coerceAtMost(bar.max) else 0
        elapsed.text = clock(pos)
        total.text = if (known) clock(dur) else "--:--"
    }

    private fun clock(ms: Long): String = Fmt.clock(ms)

    private fun drawArt() {
        val id = controller?.currentMediaItem?.mediaId?.toLongOrNull() ?: return
        if (id == artFor && art.drawable != null) return
        artFor = id
        val track = PlayerService.library.firstOrNull { it.id == id }
        if (track == null) { art.visibility = View.GONE; return }
        Art.load(this, id, track.uri, 144) { bmp ->
            // The track may have moved on while the file was being decoded.
            if (artFor != id) return@load
            if (bmp == null) {
                art.visibility = View.GONE
            } else {
                art.setImageBitmap(bmp)
                art.visibility = View.VISIBLE
            }
        }
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        // Before the controller check: the queue is reachable even if the
        // session has not bound yet, and a key that does nothing on a phone
        // with no touchscreen reads as a broken app.
        if (keyCode == KeyEvent.KEYCODE_DPAD_DOWN) {
            startActivity(Intent(this, QueueActivity::class.java))
            return true
        }
        val c = controller ?: return super.onKeyDown(keyCode, event)
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT -> {
                // Nothing happens on the way down. A tap and the first instant
                // of a hold are the same event, and only the key-up can tell
                // them apart -- deciding here is what made a press skip before
                // you had a chance to hold it.
                val dir = if (keyCode == KeyEvent.KEYCODE_DPAD_LEFT) -1 else 1
                val now = android.os.SystemClock.uptimeMillis()
                if (event.repeatCount == 0) {
                    scrubbed = false
                    lastScrubMs = now
                    holdStartMs = now
                } else {
                    // Slow enough to land on a chorus for the first couple of
                    // seconds, then fast enough to cross a long track.
                    val rate = if (now - holdStartMs < accelerateAfterMs) scrubRate
                               else scrubRateFast
                    seekBy(dir * (now - lastScrubMs) * rate)
                    lastScrubMs = now
                    scrubbed = true
                }
            }
            else -> {
                if (Keys.isSelect(event)) {
                    if (c.isPlaying) c.pause() else c.play()
                } else {
                    return super.onKeyDown(keyCode, event)
                }
            }
        }
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        val c = controller ?: return super.onKeyUp(keyCode, event)
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_LEFT -> {
                if (!scrubbed) c.seekToPreviousMediaItem()
                scrubbed = false
                return true
            }
            KeyEvent.KEYCODE_DPAD_RIGHT -> {
                if (!scrubbed) c.seekToNextMediaItem()
                scrubbed = false
                return true
            }
        }
        return super.onKeyUp(keyCode, event)
    }

    private fun seekBy(deltaMs: Long) {
        val c = controller ?: return
        val dur = if (c.duration > 0) c.duration else Long.MAX_VALUE
        c.seekTo((c.currentPosition + deltaMs).coerceIn(0L, dur))
        drawPosition()
    }
}

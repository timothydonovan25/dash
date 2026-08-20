package com.kao.dashmedia.ui

import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.provider.Settings
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.media3.common.Player
import androidx.media3.session.MediaController
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.kao.dashmedia.Keys
import com.kao.dashmedia.Lrc
import com.kao.dashmedia.Lyrics
import com.kao.dashmedia.PlayerService
import com.kao.dashmedia.Prefs
import com.kao.dashmedia.R

/**
 * Lyrics that follow the song.
 *
 * Reached with D-pad up from Now Playing, mirroring the queue on down. The
 * position is polled five times a second and the current line found by binary
 * search on that tick -- often enough that the polling interval is not itself
 * mistaken for the lyric being late.
 *
 * Following is suspended while you scroll by hand and resumes when the song
 * moves on, because a list that yanks itself back every half second is
 * unusable if you wanted to read ahead.
 */
class LyricsActivity : PlayerActivity() {

    private lateinit var list: RecyclerView
    private lateinit var empty: TextView
    private lateinit var title: TextView

    private var lrc: Lrc = Lrc(emptyList())
    private var current = -1
    private var loadedFor = -1L

    /** Set while the user is scrolling; cleared when the line changes. */
    private var following = true

    private val ui = Handler(Looper.getMainLooper())

    private val tick = object : Runnable {
        override fun run() {
            follow()
            ui.postDelayed(this, 200)
        }
    }

    private val listener = object : Player.Listener {
        override fun onEvents(player: Player, events: Player.Events) = reload()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_lyrics)
        list = findViewById(R.id.list)
        empty = findViewById(R.id.empty)
        title = findViewById(R.id.title)
        list.layoutManager = LinearLayoutManager(this)
        list.adapter = Adapter()
    }

    override fun onController(c: MediaController) {
        c.addListener(listener)
        reload()
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

    override fun onSoftRight() = finish()

    override fun softLabels() =
        Triple(getString(R.string.k_settings), modeLabel(), getString(R.string.k_playing))

    override fun onChrome() = reload()

    private fun reload() {
        val c = controller ?: return
        val id = c.currentMediaItem?.mediaId?.toLongOrNull() ?: return
        title.text = c.mediaMetadata.title ?: ""
        if (id == loadedFor) return
        loadedFor = id
        lrc = Lrc(emptyList())
        current = -1
        list.adapter?.notifyDataSetChanged()

        val track = PlayerService.library.firstOrNull { it.id == id }
        if (track == null) { showEmpty(R.string.lyrics_none); return }
        if (!Lyrics.canRead()) { showEmpty(R.string.lyrics_no_access); return }

        Lyrics.load(this, track) { loaded ->
            // The song may have changed while the file was being read.
            if (loadedFor != id) return@load
            lrc = loaded
            following = true
            list.adapter?.notifyDataSetChanged()
            if (loaded.isEmpty) showEmpty(R.string.lyrics_none) else empty.visibility = View.GONE
            follow()
        }
    }

    private fun showEmpty(msg: Int) {
        empty.setText(msg)
        empty.visibility = View.VISIBLE
    }

    private fun follow() {
        val c = controller ?: return
        if (lrc.isEmpty) return
        val at = lrc.indexAt(c.currentPosition, Prefs(this).lyricOffsetMs)
        if (at == current) return
        val previous = current
        current = at
        // A new line is the moment to take the list back, however far the
        // reader wandered.
        following = true
        if (previous >= 0) list.adapter?.notifyItemChanged(previous)
        if (at >= 0) {
            list.adapter?.notifyItemChanged(at)
            centre(at)
        }
    }

    /** Put the current line a third of the way down rather than at the top. */
    private fun centre(index: Int) {
        if (!following) return
        val lm = list.layoutManager as? LinearLayoutManager ?: return
        lm.scrollToPositionWithOffset(index, list.height / 3)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN -> {
                // Reading ahead: stop dragging the list back until the song
                // reaches the next line.
                following = false
                return super.onKeyDown(keyCode, event)
            }
        }
        val c = controller ?: return super.onKeyDown(keyCode, event)
        if (Keys.isSelect(event)) {
            if (c.isPlaying) c.pause() else c.play()
            return true
        }
        // Nowhere else to grant it from: the system screen is the only way.
        if (keyCode == KeyEvent.KEYCODE_0 && !Lyrics.canRead()) {
            startActivity(Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION))
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    private inner class Adapter : RecyclerView.Adapter<Holder>() {
        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
            Holder(LayoutInflater.from(parent.context).inflate(R.layout.item_lyric, parent, false))

        override fun getItemCount() = lrc.lines.size

        override fun onBindViewHolder(holder: Holder, position: Int) {
            val line = lrc.lines[position]
            // An instrumental gap is a real entry, and showing it blank is
            // what makes the highlight visibly leave the previous line.
            holder.text.text = line.text
            val now = position == current
            holder.text.setTextColor(getColor(if (now) R.color.accent else R.color.dim))
            holder.text.textSize = if (now) 15f else 13f
        }
    }

    private class Holder(v: View) : RecyclerView.ViewHolder(v) {
        val text: TextView = v.findViewById(R.id.line)
    }
}

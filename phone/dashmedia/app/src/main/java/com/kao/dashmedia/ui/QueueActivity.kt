package com.kao.dashmedia.ui

import android.os.Bundle
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.media3.common.MediaItem
import androidx.media3.session.MediaController
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.kao.dashmedia.Keys
import com.kao.dashmedia.R

/**
 * What is coming, and the ability to change it.
 *
 * Reordering on a keypad cannot be a drag, so the number keys do it: this is
 * the one screen where * and # earn their place, and the hints are on screen
 * because nobody would guess.
 *
 *   OK   play this one now
 *   *    move up        #  move down
 *   0    remove
 */
class QueueActivity : PlayerActivity() {

    private lateinit var list: RecyclerView
    private lateinit var empty: TextView

    private var items: List<MediaItem> = emptyList()
    private var current = 0

    /** Which row the D-pad is on. Tracked here because the rows are rebuilt. */
    private var focus = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_queue)
        list = findViewById(R.id.list)
        empty = findViewById(R.id.empty)
        list.layoutManager = LinearLayoutManager(this)
        list.adapter = Adapter()
    }

    override fun onController(c: MediaController) = reload()

    override fun onChrome() = reload()

    override fun onSoftRight() = finish()

    override fun softLabels() =
        Triple(getString(R.string.k_settings), modeLabel(), getString(R.string.k_playing))

    private fun reload() {
        val c = controller ?: return
        items = (0 until c.mediaItemCount).map { c.getMediaItemAt(it) }
        current = c.currentMediaItemIndex
        focus = focus.coerceIn(0, maxOf(0, items.size - 1))
        empty.visibility = if (items.isEmpty()) View.VISIBLE else View.GONE
        list.adapter?.notifyDataSetChanged()
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        val c = controller ?: return super.onKeyDown(keyCode, event)
        if (items.isEmpty()) return super.onKeyDown(keyCode, event)
        when (keyCode) {
            KeyEvent.KEYCODE_DPAD_UP -> { move(-1); return true }
            KeyEvent.KEYCODE_DPAD_DOWN -> { move(1); return true }
            KeyEvent.KEYCODE_STAR -> { shift(-1); return true }
            KeyEvent.KEYCODE_POUND -> { shift(1); return true }
            KeyEvent.KEYCODE_0 -> { remove(); return true }
        }
        if (Keys.isSelect(event)) {
            c.seekTo(focus, 0L)
            c.play()
            finish()
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    private fun move(delta: Int) {
        focus = (focus + delta).coerceIn(0, items.size - 1)
        list.scrollToPosition(focus)
        list.adapter?.notifyDataSetChanged()
    }

    private fun shift(delta: Int) {
        val c = controller ?: return
        val to = focus + delta
        if (to !in items.indices) return
        c.moveMediaItem(focus, to)
        focus = to
        reload()
        list.scrollToPosition(focus)
    }

    private fun remove() {
        val c = controller ?: return
        c.removeMediaItem(focus)
        reload()
    }

    private inner class Adapter : RecyclerView.Adapter<Holder>() {
        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
            Holder(LayoutInflater.from(parent.context).inflate(R.layout.item_row, parent, false))

        override fun getItemCount() = items.size

        override fun onBindViewHolder(holder: Holder, position: Int) {
            val m = items[position].mediaMetadata
            holder.label.text = m.title ?: ""
            holder.sub.text = m.artist ?: ""
            // Focus is drawn rather than held by the views: the list is
            // rebuilt on every reorder, and a real focus would jump about.
            val mark = position == focus
            holder.itemView.setBackgroundResource(
                if (mark) R.color.focus else android.R.color.transparent
            )
            holder.label.setTextColor(
                getColor(if (position == current) R.color.accent else R.color.text)
            )
        }
    }

    private class Holder(v: View) : RecyclerView.ViewHolder(v) {
        val label: TextView = v.findViewById(R.id.label)
        val sub: TextView = v.findViewById(R.id.sub)
    }
}

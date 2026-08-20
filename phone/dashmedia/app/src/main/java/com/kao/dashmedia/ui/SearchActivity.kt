package com.kao.dashmedia.ui

import android.content.Intent
import android.os.Bundle
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.TextView
import androidx.media3.session.MediaController
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.kao.dashmedia.Browse
import com.kao.dashmedia.Fmt
import com.kao.dashmedia.PlayerService
import com.kao.dashmedia.R
import com.kao.dashmedia.Source
import com.kao.dashmedia.T9
import com.kao.dashmedia.Track

/**
 * Search, the way phones did it before touchscreens.
 *
 * 4-6-5 finds Gojira. There is no keyboard on this handset and no room for
 * one, and 522 tracks is a great many D-pad presses, so the number keys are
 * the search. Pressing any of them in the browse list lands here with that
 * digit already typed.
 *
 * The matching itself is [T9], which is pure and tested; this screen is the
 * keypad and the list.
 */
class SearchActivity : PlayerActivity() {

    companion object {
        const val EXTRA_SEED = "seed"

        /** Enough to find anything; more than anyone scrolls on a keypad. */
        private const val MAX_RESULTS = 60
    }

    private lateinit var query: TextView
    private lateinit var hint: TextView
    private lateinit var list: RecyclerView
    private lateinit var empty: TextView

    private var digits = ""
    private var results: List<Track> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_search)
        query = findViewById(R.id.query)
        hint = findViewById(R.id.hint)
        list = findViewById(R.id.list)
        empty = findViewById(R.id.empty)
        list.layoutManager = LinearLayoutManager(this)
        list.adapter = Adapter()
        digits = intent.getStringExtra(EXTRA_SEED).orEmpty()
    }

    override fun onController(c: MediaController) = search()

    override fun onSoftRight() {
        startActivity(Intent(this, NowPlayingActivity::class.java))
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_0) {
            // 0 is space, as it has been on every keypad. It costs the ability
            // to search for a literal zero -- no more finding "2004" by its
            // year -- which is the trade every phone has always made.
            if (digits.isNotEmpty() && !digits.endsWith(" ")) {
                digits += " "
                search()
            }
            return true
        }
        if (keyCode in KeyEvent.KEYCODE_1..KeyEvent.KEYCODE_9) {
            digits += ('0' + (keyCode - KeyEvent.KEYCODE_0))
            search()
            return true
        }
        when (keyCode) {
            KeyEvent.KEYCODE_BACK -> {
                // Back deletes a digit before it leaves, which is what every
                // phone with a keypad has always done.
                if (digits.isNotEmpty()) {
                    digits = digits.dropLast(1)
                    search()
                    return true
                }
            }
            KeyEvent.KEYCODE_STAR -> { queueFocused(next = true); return true }
            KeyEvent.KEYCODE_POUND -> { queueFocused(next = false); return true }
        }
        return super.onKeyDown(keyCode, event)
    }

    private fun search() {
        val lib = PlayerService.library
        results = if (digits.isEmpty()) emptyList() else lib.asSequence()
            .filter { T9.matchesAny(digits, it.title, it.artist, it.album) }
            .take(MAX_RESULTS)
            .toList()
        query.text = digits
        hint.text = T9.hint(digits)
        empty.visibility = if (results.isEmpty()) View.VISIBLE else View.GONE
        empty.setText(if (digits.isEmpty()) R.string.search_prompt else R.string.search_none)
        list.adapter?.notifyDataSetChanged()
        drawChrome()
    }

    private fun focused(): Track? {
        val v = list.focusedChild ?: return null
        return results.getOrNull(list.getChildAdapterPosition(v))
    }

    private fun queueFocused(next: Boolean) {
        val t = focused() ?: return
        val svc = PlayerService.instance ?: return
        if (next) svc.playNext(listOf(t)) else svc.addToQueue(listOf(t))
        toast(resources.getQuantityString(
            if (next) R.plurals.queued_next else R.plurals.queued_end, 1, 1))
    }

    private fun play(index: Int) {
        // The queue becomes the results, so next and previous stay inside what
        // you searched for.
        PlayerService.instance?.playFrom(results, index, Source.All)
        startActivity(Intent(this, NowPlayingActivity::class.java))
    }

    private inner class Adapter : RecyclerView.Adapter<Holder>() {
        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
            Holder(LayoutInflater.from(parent.context).inflate(R.layout.item_row, parent, false))

        override fun getItemCount() = results.size

        override fun onBindViewHolder(holder: Holder, position: Int) {
            val t = results[position]
            holder.label.text = t.title
            holder.sub.text = "${Browse.display(t.artist)}  ${Fmt.clock(t.durationMs)}"
            holder.itemView.setOnClickListener { play(holder.bindingAdapterPosition) }
        }
    }

    private class Holder(v: View) : RecyclerView.ViewHolder(v) {
        val label: TextView = v.findViewById(R.id.label)
        val sub: TextView = v.findViewById(R.id.sub)
    }
}

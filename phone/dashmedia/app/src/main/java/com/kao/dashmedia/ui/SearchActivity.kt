package com.kao.dashmedia.ui

import android.content.Intent
import android.graphics.Typeface
import android.os.Bundle
import android.text.SpannableString
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.text.style.StyleSpan
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
import com.kao.dashmedia.LyricIndex
import com.kao.dashmedia.PlayerService
import com.kao.dashmedia.R
import com.kao.dashmedia.Source
import com.kao.dashmedia.T9
import com.kao.dashmedia.Track

/**
 * Search, the way phones did it before touchscreens.
 *
 * 4-6-5 finds Gojira, and 0 is a space. There is no keyboard on this handset
 * and no room for one, so the number keys are the search.
 *
 * Results come in sections, in the order you are most likely to have meant:
 * songs by title, then playlists, then artists, then songs by something they
 * say. A lyric match carries the line that matched, because "this song" is not
 * an answer to "the one that goes like this".
 *
 * Titles, playlists and artists are matched against lists already in memory
 * and are instant. Lyrics are not: that is [LyricIndex], built once in the
 * background, and its section appears when it is ready rather than holding
 * everything else up.
 */
class SearchActivity : PlayerActivity() {

    companion object {
        const val EXTRA_SEED = "seed"

        // Enough to be worth scrolling, not so many that the sections below
        // are unreachable on a keypad.
        private const val MAX_SONGS = 20
        private const val MAX_PLAYLISTS = 8
        private const val MAX_ARTISTS = 8
        private const val MAX_LYRICS = 20
    }

    private sealed interface Row {
        class Header(val title: String) : Row
        class Song(val track: Track, val within: List<Track>, val at: Int) : Row
        class Opens(val label: String, val sub: String, val open: Source) : Row
        class Lyric(
            val track: Track,
            val line: String,
            val query: String,
            val within: List<Track>,
            val at: Int,
        ) : Row
    }

    private lateinit var query: TextView
    private lateinit var hint: TextView
    private lateinit var list: RecyclerView
    private lateinit var empty: TextView

    private var digits = ""
    private var rows: List<Row> = emptyList()

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
            // 0 is space, as it has been on every keypad. It costs searching
            // for a literal zero, which is the trade every phone has made.
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
                // Back deletes a character before it leaves, which is what
                // every phone with a keypad has always done.
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

    // ---- the search ------------------------------------------------------

    private fun search() {
        val q = digits
        query.text = q
        hint.text = T9.hint(q)

        if (q.isBlank()) {
            rows = emptyList()
            showEmpty(R.string.search_prompt)
            list.adapter?.notifyDataSetChanged()
            drawChrome()
            return
        }

        val built = ArrayList<Row>(16)
        built += songSection(q)
        built += playlistSection(q)
        built += artistSection(q)
        rows = built
        list.adapter?.notifyDataSetChanged()
        empty.visibility = if (rows.isEmpty()) View.VISIBLE else View.GONE
        if (rows.isEmpty()) showEmpty(R.string.search_none)
        drawChrome()

        // Lyrics last and asynchronously: the index takes several seconds to
        // build the first time, and everything above is already useful.
        if (LyricIndex.ready) {
            appendLyrics(q)
        } else {
            // Deliberately not `q`. The build takes long enough that the query
            // has almost always moved on by the time it lands -- guarding on
            // the generation here meant the lyric section simply never
            // appeared for anything typed while the index was building, which
            // is every search on the first run.
            LyricIndex.ensureBuilt(this) { appendLyrics(digits) }
        }
    }

    private fun appendLyrics(q: String) {
        val hits = LyricIndex.search(q, MAX_LYRICS)
        if (hits.isEmpty()) return
        val tracks = hits.map { it.track }
        val more = ArrayList<Row>(hits.size + 1)
        more.add(Row.Header(getString(R.string.sec_by_lyric)))
        hits.forEachIndexed { i, h -> more.add(Row.Lyric(h.track, h.line, q, tracks, i)) }
        rows = rows + more
        empty.visibility = View.GONE
        list.adapter?.notifyDataSetChanged()
    }

    private fun songSection(q: String): List<Row> {
        val hits = PlayerService.library.asSequence()
            .filter { T9.matchesAny(q, it.title) }
            .take(MAX_SONGS)
            .toList()
        if (hits.isEmpty()) return emptyList()
        return listOf(Row.Header(getString(R.string.sec_songs))) +
            hits.mapIndexed { i, t -> Row.Song(t, hits, i) }
    }

    private fun playlistSection(q: String): List<Row> {
        val svc = PlayerService.instance
        val out = ArrayList<Row>()
        for (l in svc?.stash?.lists.orEmpty()) {
            if (!T9.matchesAny(q, l.name)) continue
            out.add(Row.Opens(l.name, count(l.trackIds.size), Source.Owned(l.id, l.name)))
            if (out.size >= MAX_PLAYLISTS) break
        }
        for (pl in PlayerService.playlists) {
            if (out.size >= MAX_PLAYLISTS) break
            if (!T9.matchesAny(q, pl.name)) continue
            val n = PlayerService.playlistCounts[pl.id] ?: 0
            out.add(Row.Opens(pl.name, getString(R.string.read_only, count(n)),
                Source.Playlist(pl.id, pl.name)))
        }
        if (out.isEmpty()) return emptyList()
        return listOf(Row.Header(getString(R.string.sec_playlists_r))) + out
    }

    private fun artistSection(q: String): List<Row> {
        val lib = PlayerService.library
        val out = ArrayList<Row>()
        for (a in Browse.artists(lib)) {
            if (a.isBlank() || !T9.matchesAny(q, a)) continue
            out.add(Row.Opens(Browse.display(a), count(Browse.tracksBy(lib, a).size),
                Source.Artist(a)))
            if (out.size >= MAX_ARTISTS) break
        }
        if (out.isEmpty()) return emptyList()
        return listOf(Row.Header(getString(R.string.sec_artists_r))) + out
    }

    private fun count(n: Int) = resources.getQuantityString(R.plurals.tracks, n, n)

    /**
     * Mark the words the query matched.
     *
     * Whole words, not matched prefixes: half a word in a different colour
     * reads as a rendering fault rather than an answer. The ranges come from
     * [T9.matchRanges], which indexes the original text -- folding for accents
     * changes a string's length, so indices taken from folded text would point
     * at the wrong characters.
     */
    private fun highlight(line: String, q: String): CharSequence {
        val ranges = T9.matchRanges(q, line)
        if (ranges.isEmpty()) return line
        val out = SpannableString(line)
        val colour = getColor(R.color.accent)
        for (r in ranges) {
            val end = r.last + 1
            out.setSpan(ForegroundColorSpan(colour), r.first, end,
                Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
            out.setSpan(StyleSpan(Typeface.BOLD), r.first, end,
                Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
        }
        return out
    }

    private fun showEmpty(msg: Int) {
        empty.setText(msg)
        empty.visibility = View.VISIBLE
    }

    // ---- acting on a row -------------------------------------------------

    private fun focusedRow(): Row? {
        val v = list.focusedChild ?: return null
        return rows.getOrNull(list.getChildAdapterPosition(v))
    }

    private fun queueFocused(next: Boolean) {
        val t = when (val r = focusedRow()) {
            is Row.Song -> r.track
            is Row.Lyric -> r.track
            else -> null
        } ?: return
        val svc = PlayerService.instance ?: return
        if (next) svc.playNext(listOf(t)) else svc.addToQueue(listOf(t))
        toast(resources.getQuantityString(
            if (next) R.plurals.queued_next else R.plurals.queued_end, 1, 1))
    }

    private fun open(row: Row) {
        when (row) {
            is Row.Header -> Unit
            // The queue becomes what you searched, so next and previous stay
            // inside the results rather than wandering off into the library.
            is Row.Song -> play(row.within, row.at)
            is Row.Lyric -> play(row.within, row.at)
            // An artist or a playlist is a place, not a track: go there rather
            // than start playing something you have not chosen yet.
            is Row.Opens -> startActivity(
                Intent(this, BrowseActivity::class.java)
                    .putExtra(BrowseActivity.EXTRA_OPEN, row.open.encode())
            )
        }
    }

    private fun play(within: List<Track>, at: Int) {
        PlayerService.instance?.playFrom(within, at, Source.All)
        startActivity(Intent(this, NowPlayingActivity::class.java))
    }

    // ---- list ------------------------------------------------------------

    private inner class Adapter : RecyclerView.Adapter<RecyclerView.ViewHolder>() {

        override fun getItemViewType(position: Int) =
            if (rows[position] is Row.Header) 0 else 1

        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): RecyclerView.ViewHolder {
            val inflater = LayoutInflater.from(parent.context)
            return if (viewType == 0) {
                HeaderHolder(inflater.inflate(R.layout.item_header, parent, false))
            } else {
                ItemHolder(inflater.inflate(R.layout.item_row, parent, false))
            }
        }

        override fun getItemCount() = rows.size

        override fun onBindViewHolder(holder: RecyclerView.ViewHolder, position: Int) {
            when (val r = rows[position]) {
                is Row.Header -> (holder as HeaderHolder).title.text = r.title
                else -> {
                    val h = holder as ItemHolder
                    val (label, sub) = when (r) {
                        is Row.Song -> r.track.title to
                            "${Browse.display(r.track.artist)}  ${Fmt.clock(r.track.durationMs)}"
                        is Row.Opens -> r.label to (r.sub as CharSequence)
                        // The line that matched, under the song it came from:
                        // "this song" is not an answer to "the one that goes
                        // like this". The words that matched are marked, so it
                        // is clear which part of the line answered the query
                        // rather than leaving it to be re-read.
                        is Row.Lyric -> r.track.title to highlight(r.line, r.query)
                        is Row.Header -> "" to ("" as CharSequence)
                    }
                    h.label.text = label
                    h.sub.text = sub
                    h.sub.visibility = if (sub.isEmpty()) View.GONE else View.VISIBLE
                    h.itemView.setOnClickListener {
                        rows.getOrNull(h.bindingAdapterPosition)?.let { open(it) }
                    }
                }
            }
        }
    }

    private class ItemHolder(v: View) : RecyclerView.ViewHolder(v) {
        val label: TextView = v.findViewById(R.id.label)
        val sub: TextView = v.findViewById(R.id.sub)
    }

    private class HeaderHolder(v: View) : RecyclerView.ViewHolder(v) {
        val title: TextView = v.findViewById(R.id.header)
    }
}

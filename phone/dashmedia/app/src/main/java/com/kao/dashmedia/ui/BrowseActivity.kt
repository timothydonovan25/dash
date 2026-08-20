package com.kao.dashmedia.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.text.InputType
import android.view.KeyEvent
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.EditText
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.media3.session.MediaController
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.kao.dashmedia.Browse
import com.kao.dashmedia.Fmt
import com.kao.dashmedia.OwnedList
import com.kao.dashmedia.PlayerService
import com.kao.dashmedia.R
import com.kao.dashmedia.Source
import com.kao.dashmedia.Track

/**
 * The library, in two sections that left and right move between.
 *
 * ARTISTS goes artist then album then track. PLAYLISTS holds the automatic
 * lists, the playlists this app owns and can edit, and the read-only ones from
 * MediaStore -- an app on API 30 may only modify playlists it created, so ours
 * and theirs are genuinely different things and the list says which is which.
 *
 * The keypad does the work a touchscreen would:
 *
 *   2-9   start searching
 *   0     options for the highlighted row
 *   *     play next          #  add to the end of the queue
 *
 * Digits go to search rather than to actions, because search is the thing you
 * reach for most and a keypad phone has trained everyone to expect it.
 */
class BrowseActivity : PlayerActivity() {

    companion object {
        /**
         * Open straight at an artist or a playlist, as an encoded [Source].
         *
         * Search hands artists and playlists back here rather than growing its
         * own version of these screens, and Source already knows how to name
         * one of them in a string -- with tests -- so nothing new is invented.
         */
        const val EXTRA_OPEN = "open"
    }

    private class Row(
        val label: String,
        val sub: String,
        val go: () -> Unit,
        /** What this row stands for, for the queue keys. Null for a header. */
        val tracks: (() -> List<Track>)? = null,
        val options: (() -> Unit)? = null,
    )

    private enum class Section { Artists, Playlists }

    private lateinit var list: RecyclerView
    private lateinit var empty: TextView
    private lateinit var title: TextView

    private var rows: List<Row> = emptyList()

    private var section = Section.Artists

    /** Where we are inside the current section. Null means its top level. */
    private var artist: String? = null
    private var album: String? = null
    private var playlist: com.kao.dashmedia.Playlist? = null
    private var owned: OwnedList? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_browse)
        list = findViewById(R.id.list)
        empty = findViewById(R.id.empty)
        title = findViewById(R.id.title)
        list.layoutManager = LinearLayoutManager(this)
        list.adapter = Adapter()
        openFrom(intent)
        ensurePermissions()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        openFrom(intent)
        rebuild()
    }

    /** Jump to wherever a deep link points, before the first draw. */
    private fun openFrom(intent: Intent) {
        val src = Source.decode(intent.getStringExtra(EXTRA_OPEN)) ?: return
        artist = null; album = null; playlist = null; owned = null
        when (src) {
            is Source.Artist -> { section = Section.Artists; artist = src.name }
            is Source.Owned -> {
                section = Section.Playlists
                owned = PlayerService.instance?.stash?.find(src.id)
            }
            is Source.Playlist -> {
                section = Section.Playlists
                playlist = PlayerService.playlists.firstOrNull { it.id == src.id }
            }
            else -> Unit
        }
    }

    override fun onController(c: MediaController) = rebuild()

    override fun onChrome() {
        // The library arrives on a background thread, so the first draw is
        // usually empty and this is what fills it in.
        if (rows.isEmpty()) rebuild()
    }

    override fun onSoftRight() {
        startActivity(Intent(this, NowPlayingActivity::class.java))
    }

    private fun atTop(): Boolean = artist == null && playlist == null && owned == null

    // ---- keys ------------------------------------------------------------

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        // Left and right change section, but only at the top of one: doing it
        // from three levels deep would throw away where you were silently.
        if (atTop() && (keyCode == KeyEvent.KEYCODE_DPAD_LEFT ||
                keyCode == KeyEvent.KEYCODE_DPAD_RIGHT)
        ) {
            section = if (section == Section.Artists) Section.Playlists else Section.Artists
            album = null
            rebuild()
            return true
        }
        // Any letter key starts a search, seeded with what was pressed.
        if (keyCode in KeyEvent.KEYCODE_2..KeyEvent.KEYCODE_9) {
            val digit = ('0' + (keyCode - KeyEvent.KEYCODE_0)).toString()
            startActivity(
                Intent(this, SearchActivity::class.java).putExtra(SearchActivity.EXTRA_SEED, digit)
            )
            return true
        }
        when (keyCode) {
            KeyEvent.KEYCODE_0 -> { focusedRow()?.options?.invoke(); return true }
            KeyEvent.KEYCODE_STAR -> { queueFocused(next = true); return true }
            KeyEvent.KEYCODE_POUND -> { queueFocused(next = false); return true }
        }
        return super.onKeyDown(keyCode, event)
    }

    /**
     * Which row the D-pad is on. The RecyclerView owns focus, so it is asked
     * rather than tracked -- tracking it separately drifts the moment the list
     * is rebuilt under you.
     */
    private fun focusedRow(): Row? {
        val v = list.focusedChild ?: return null
        return rows.getOrNull(list.getChildAdapterPosition(v))
    }

    private fun queueFocused(next: Boolean) {
        val r = focusedRow() ?: return
        val t = r.tracks?.invoke() ?: return
        if (t.isEmpty()) return
        val svc = PlayerService.instance ?: return
        if (next) svc.playNext(t) else svc.addToQueue(t)
        toast(resources.getQuantityString(
            if (next) R.plurals.queued_next else R.plurals.queued_end, t.size, t.size))
    }

    private fun ensurePermissions() {
        val want = arrayOf(
            Manifest.permission.READ_EXTERNAL_STORAGE,
            // Not for location. On API 30 a BLE scan silently returns nothing
            // without it.
            Manifest.permission.ACCESS_FINE_LOCATION,
        ).filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (want.isNotEmpty()) requestPermissions(want.toTypedArray(), 1)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        rebuild()
    }

    // ---- levels ----------------------------------------------------------

    // Process-scoped, so it survives the service being stopped when the
    // screen sleeps -- see PlayerService.loadLibrary.
    private fun library(): List<Track> = PlayerService.library

    override fun onResume() {
        super.onResume()
        // A playlist may have been edited from another screen.
        rebuild()
    }

    private fun rebuild() {
        val lib = library()
        rows = when {
            owned != null -> ownedTracks(owned!!)
            playlist != null -> playlistTracks(playlist!!)
            section == Section.Playlists -> playlistRoot()
            artist == null -> artistRoot(lib)
            album == null -> albumsOf(lib, artist!!)
            else -> tracksOf(lib, artist!!, album!!)
        }
        title.text = when {
            owned != null -> owned!!.name
            playlist != null -> playlist!!.name
            !atTop() && album != null -> Browse.display(album!!)
            !atTop() -> Browse.display(artist!!)
            // The arrows are the only hint that left and right do anything.
            else -> getString(
                R.string.section_header,
                getString(
                    if (section == Section.Artists) R.string.sec_artists
                    else R.string.sec_playlists
                ),
            )
        }
        val none = rows.isEmpty()
        empty.visibility = if (none) View.VISIBLE else View.GONE
        if (none) {
            empty.setText(
                when {
                    checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
                        != PackageManager.PERMISSION_GRANTED -> R.string.need_storage
                    // The query runs off the main thread, so an empty list
                    // usually means "not yet" rather than "none".
                    !PlayerService.libraryReady -> R.string.loading
                    owned != null -> R.string.playlist_empty
                    else -> R.string.no_music
                }
            )
        }
        list.adapter?.notifyDataSetChanged()
        drawChrome()
    }

    private fun artistRoot(lib: List<Track>): List<Row> {
        if (lib.isEmpty()) return emptyList()
        val out = ArrayList<Row>(lib.size / 8 + 1)
        out.add(Row(getString(R.string.all_tracks), count(lib.size),
            go = { play(lib, 0, Source.All) }, tracks = { lib }))
        for (a in Browse.artists(lib)) {
            val mine = { Browse.tracksBy(lib, a) }
            out.add(Row(Browse.display(a), count(Browse.tracksBy(lib, a).size),
                go = { artist = a; album = null; rebuild() }, tracks = mine))
        }
        return out
    }

    private fun albumsOf(lib: List<Track>, a: String): List<Row> {
        val mine = Browse.tracksBy(lib, a)
        val out = ArrayList<Row>()
        out.add(Row(getString(R.string.all_by_artist), count(mine.size),
            go = { play(mine, 0, Source.Artist(a)) }, tracks = { mine }))
        for (al in Browse.albums(lib, a)) {
            val theirs = { Browse.tracksOf(lib, a, al) }
            out.add(Row(Browse.display(al), count(Browse.tracksOf(lib, a, al).size),
                go = { album = al; rebuild() }, tracks = theirs))
        }
        return out
    }

    private fun tracksOf(lib: List<Track>, a: String, al: String): List<Row> {
        val mine = Browse.tracksOf(lib, a, al)
        return mine.mapIndexed { i, t -> trackRow(t, mine, i, Source.Album(a, al)) }
    }

    private fun trackRow(t: Track, of: List<Track>, i: Int, from: Source): Row = Row(
        label = t.title,
        sub = "${Browse.display(t.artist)}  ${Fmt.clock(t.durationMs)}",
        go = { play(of, i, from) },
        tracks = { listOf(t) },
        options = { trackOptions(t, i) },
    )

    // ---- playlists -------------------------------------------------------

    private fun playlistRoot(): List<Row> {
        val svc = PlayerService.instance
        val out = ArrayList<Row>()
        out.add(Row(getString(R.string.new_playlist), "", go = { promptNewPlaylist() }))

        fun auto(labelRes: Int, src: Source) {
            val tracks = svc?.tracksFor(src) ?: emptyList()
            if (tracks.isEmpty()) return
            out.add(Row(getString(labelRes), count(tracks.size),
                go = { play(tracks, 0, src) }, tracks = { tracks }))
        }
        auto(R.string.recently_added, Source.RecentAdded)
        auto(R.string.recently_played, Source.RecentPlayed)
        auto(R.string.most_played, Source.MostPlayed)

        for (l in PlayerService.instance?.stash?.lists.orEmpty()) {
            out.add(Row(l.name, count(l.trackIds.size),
                go = { owned = l; rebuild() },
                tracks = { svc?.tracksFor(Source.Owned(l.id, l.name)) ?: emptyList() },
                options = { ownedOptions(l) }))
        }
        for (pl in PlayerService.playlists) {
            out.add(Row(
                pl.name,
                getString(R.string.read_only, count(PlayerService.playlistCounts[pl.id] ?: 0)),
                go = { playlist = pl; rebuild() },
                tracks = { svc?.tracksFor(Source.Playlist(pl.id, pl.name)) ?: emptyList() },
                options = { readOnlyOptions(pl) },
            ))
        }
        return out
    }

    private fun playlistTracks(pl: com.kao.dashmedia.Playlist): List<Row> {
        val svc = PlayerService.instance ?: return emptyList()
        val tracks = svc.tracksFor(Source.Playlist(pl.id, pl.name))
        return tracks.mapIndexed { i, t -> trackRow(t, tracks, i, Source.Playlist(pl.id, pl.name)) }
    }

    private fun ownedTracks(l: OwnedList): List<Row> {
        val svc = PlayerService.instance ?: return emptyList()
        val src = Source.Owned(l.id, l.name)
        val tracks = svc.tracksFor(src)
        return tracks.mapIndexed { i, t ->
            Row(
                label = t.title,
                sub = "${Browse.display(t.artist)}  ${Fmt.clock(t.durationMs)}",
                go = { play(tracks, i, src) },
                tracks = { listOf(t) },
                options = { ownedTrackOptions(l, t, i) },
            )
        }
    }

    private fun promptNewPlaylist(fill: List<Long> = emptyList()) {
        val field = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_WORDS
            setSingleLine()
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.new_playlist)
            .setView(field)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                val name = field.text.toString().trim().ifEmpty { getString(R.string.untitled) }
                PlayerService.instance?.stash?.create(name, fill)
                rebuild()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun ownedOptions(l: OwnedList) {
        menu(l.name, R.string.opt_rename, R.string.opt_delete) { which ->
            when (which) {
                0 -> promptRename(l)
                1 -> confirmDelete(l)
            }
        }
    }

    private fun promptRename(l: OwnedList) {
        val field = EditText(this).apply { setText(l.name); setSingleLine() }
        AlertDialog.Builder(this)
            .setTitle(R.string.opt_rename)
            .setView(field)
            .setPositiveButton(android.R.string.ok) { _, _ ->
                val name = field.text.toString().trim()
                if (name.isNotEmpty()) PlayerService.instance?.stash?.rename(l.id, name)
                rebuild()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    private fun confirmDelete(l: OwnedList) {
        AlertDialog.Builder(this)
            .setTitle(getString(R.string.delete_q, l.name))
            .setPositiveButton(R.string.opt_delete) { _, _ ->
                PlayerService.instance?.stash?.delete(l.id)
                rebuild()
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    /** The only thing you can do to someone else's playlist is take a copy. */
    private fun readOnlyOptions(pl: com.kao.dashmedia.Playlist) {
        val svc = PlayerService.instance ?: return
        menu(pl.name, R.string.opt_import) {
            val ids = svc.tracksFor(Source.Playlist(pl.id, pl.name)).map { it.id }
            svc.stash.create(getString(R.string.copy_of, pl.name), ids)
            toast(getString(R.string.imported, pl.name))
            rebuild()
        }
    }

    private fun trackOptions(t: Track, index: Int) {
        menu(t.title, R.string.opt_next, R.string.opt_queue, R.string.opt_add_to) { which ->
            val svc = PlayerService.instance ?: return@menu
            when (which) {
                0 -> svc.playNext(listOf(t))
                1 -> svc.addToQueue(listOf(t))
                2 -> choosePlaylistFor(t)
            }
        }
    }

    private fun ownedTrackOptions(l: OwnedList, t: Track, index: Int) {
        menu(
            t.title,
            R.string.opt_next, R.string.opt_queue, R.string.opt_add_to,
            R.string.opt_up, R.string.opt_down, R.string.opt_remove,
        ) { which ->
            val svc = PlayerService.instance ?: return@menu
            val ids = l.trackIds.toMutableList()
            when (which) {
                0 -> svc.playNext(listOf(t))
                1 -> svc.addToQueue(listOf(t))
                2 -> choosePlaylistFor(t)
                3 -> if (index > 0) {
                    ids.add(index - 1, ids.removeAt(index)); svc.stash.setTracks(l.id, ids)
                }
                4 -> if (index < ids.size - 1) {
                    ids.add(index + 1, ids.removeAt(index)); svc.stash.setTracks(l.id, ids)
                }
                5 -> {
                    ids.removeAt(index); svc.stash.setTracks(l.id, ids)
                }
            }
            owned = svc.stash.find(l.id)
            rebuild()
        }
    }

    private fun choosePlaylistFor(t: Track) {
        val svc = PlayerService.instance ?: return
        val lists = svc.stash.lists
        val labels = (lists.map { it.name } + getString(R.string.new_playlist)).toTypedArray()
        AlertDialog.Builder(this)
            .setTitle(R.string.opt_add_to)
            .setItems(labels) { _, which ->
                if (which < lists.size) {
                    svc.stash.addTo(lists[which].id, t.id)
                    toast(getString(R.string.added_to, lists[which].name))
                    rebuild()
                } else {
                    promptNewPlaylist(listOf(t.id))
                }
            }
            .show()
    }

    /** A short options list. AlertDialog will not show a message and items. */
    private fun menu(title: String, vararg labels: Int, onPick: (Int) -> Unit) {
        AlertDialog.Builder(this)
            .setTitle(title)
            .setItems(labels.map { getString(it) }.toTypedArray()) { _, which -> onPick(which) }
            .show()
    }

    private fun count(n: Int) = resources.getQuantityString(R.plurals.tracks, n, n)

    private fun play(tracks: List<Track>, index: Int, from: Source) {
        PlayerService.instance?.playFrom(tracks, index, from)
        startActivity(Intent(this, NowPlayingActivity::class.java))
    }

    @Suppress("DEPRECATION")
    override fun onBackPressed() {
        // Back walks up the tree before it leaves the app.
        when {
            owned != null -> { owned = null; rebuild() }
            playlist != null -> { playlist = null; rebuild() }
            album != null -> { album = null; rebuild() }
            artist != null -> { artist = null; rebuild() }
            else -> super.onBackPressed()
        }
    }

    // ---- list ------------------------------------------------------------

    private inner class Adapter : RecyclerView.Adapter<Holder>() {
        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int) =
            Holder(LayoutInflater.from(parent.context).inflate(R.layout.item_row, parent, false))

        override fun getItemCount() = rows.size

        override fun onBindViewHolder(holder: Holder, position: Int) {
            val r = rows[position]
            holder.label.text = r.label
            holder.sub.text = r.sub
            holder.sub.visibility = if (r.sub.isEmpty()) View.GONE else View.VISIBLE
            holder.itemView.setOnClickListener {
                rows.getOrNull(holder.bindingAdapterPosition)?.go?.invoke()
            }
            holder.itemView.setOnLongClickListener {
                rows.getOrNull(holder.bindingAdapterPosition)?.options?.invoke()
                true
            }
        }
    }

    private class Holder(v: View) : RecyclerView.ViewHolder(v) {
        val label: TextView = v.findViewById(R.id.label)
        val sub: TextView = v.findViewById(R.id.sub)
    }
}

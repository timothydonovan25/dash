package com.kao.dashmedia.ui

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
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
import com.kao.dashmedia.Library
import com.kao.dashmedia.PlayerService
import com.kao.dashmedia.R
import com.kao.dashmedia.Source
import com.kao.dashmedia.Track

/**
 * The library, in two sections that left and right move between.
 *
 * ARTISTS goes artist then album then track; PLAYLISTS goes playlist then
 * track. A flat list of 522 songs is one D-pad press from the song above it
 * and five hundred from anything else, so everything here is about making the
 * second press worth more than the first.
 *
 * Left and right only change section at the top of one, because changing
 * section from three levels deep would throw away where you were without
 * saying so. Below the top, Back is the way out.
 */
class BrowseActivity : PlayerActivity() {

    private class Row(val label: String, val sub: String, val go: () -> Unit)

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

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_browse)
        list = findViewById(R.id.list)
        empty = findViewById(R.id.empty)
        title = findViewById(R.id.title)
        list.layoutManager = LinearLayoutManager(this)
        list.adapter = Adapter()
        ensurePermissions()
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

    private fun atTop(): Boolean = artist == null && playlist == null

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (atTop() && (keyCode == KeyEvent.KEYCODE_DPAD_LEFT ||
                keyCode == KeyEvent.KEYCODE_DPAD_RIGHT)
        ) {
            section = if (section == Section.Artists) Section.Playlists else Section.Artists
            album = null
            rebuild()
            return true
        }
        return super.onKeyDown(keyCode, event)
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

    private fun rebuild() {
        val lib = library()
        rows = when {
            playlist != null -> playlistTracks(playlist!!)
            section == Section.Playlists -> playlistRoot()
            artist == null -> artistRoot(lib)
            album == null -> albumsOf(lib, artist!!)
            else -> tracksOf(lib, artist!!, album!!)
        }
        title.text = when {
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
                    // The query runs off the main thread now, so an empty list
                    // usually means "not yet" rather than "none".
                    !PlayerService.libraryReady -> R.string.loading
                    section == Section.Playlists -> R.string.no_playlists
                    else -> R.string.no_music
                }
            )
        }
        list.adapter?.notifyDataSetChanged()
        list.scrollToPosition(0)
        drawChrome()
    }

    private fun artistRoot(lib: List<Track>): List<Row> {
        if (lib.isEmpty()) return emptyList()
        val out = ArrayList<Row>(lib.size / 8 + 1)
        out.add(Row(getString(R.string.all_tracks), count(lib.size)) {
            play(lib, 0, Source.All)
        })
        for (a in Browse.artists(lib)) {
            val n = Browse.tracksBy(lib, a).size
            out.add(Row(Browse.display(a), count(n)) { artist = a; album = null; rebuild() })
        }
        return out
    }

    private fun albumsOf(lib: List<Track>, a: String): List<Row> {
        val mine = Browse.tracksBy(lib, a)
        val out = ArrayList<Row>()
        out.add(Row(getString(R.string.all_by_artist), count(mine.size)) {
            play(mine, 0, Source.Artist(a))
        })
        for (al in Browse.albums(lib, a)) {
            val n = Browse.tracksOf(lib, a, al).size
            out.add(Row(Browse.display(al), count(n)) { album = al; rebuild() })
        }
        return out
    }

    private fun tracksOf(lib: List<Track>, a: String, al: String): List<Row> {
        val mine = Browse.tracksOf(lib, a, al)
        return mine.mapIndexed { i, t ->
            Row(t.title, Browse.display(t.artist)) { play(mine, i, Source.Album(a, al)) }
        }
    }

    private fun playlistRoot(): List<Row> =
        PlayerService.playlists.map { pl ->
            Row(pl.name, count(PlayerService.playlistCounts[pl.id] ?: 0)) {
                playlist = pl
                rebuild()
            }
        }

    private fun playlistTracks(pl: com.kao.dashmedia.Playlist): List<Row> {
        val svc = PlayerService.instance ?: return emptyList()
        val tracks = svc.tracksFor(Source.Playlist(pl.id, pl.name))
        return tracks.mapIndexed { i, t ->
            Row(t.title, Browse.display(t.artist)) {
                play(tracks, i, Source.Playlist(pl.id, pl.name))
            }
        }
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
            holder.itemView.setOnClickListener {
                rows.getOrNull(holder.bindingAdapterPosition)?.go?.invoke()
            }
        }
    }

    private class Holder(v: View) : RecyclerView.ViewHolder(v) {
        val label: TextView = v.findViewById(R.id.label)
        val sub: TextView = v.findViewById(R.id.sub)
    }
}

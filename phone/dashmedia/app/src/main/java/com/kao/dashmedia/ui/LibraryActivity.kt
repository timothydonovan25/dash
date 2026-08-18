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
import androidx.recyclerview.widget.LinearLayoutManager
import androidx.recyclerview.widget.RecyclerView
import com.kao.dashmedia.Library
import com.kao.dashmedia.PlayerService
import com.kao.dashmedia.R
import com.kao.dashmedia.Track

/**
 * The library, as a single scrolling list.
 *
 * No touchscreen and 240x320: the list is the whole interface. Artist/album
 * grouping would cost two more screens of D-pad navigation to reach a song,
 * so the list is flat and sorted, and every row is focusable.
 */
class LibraryActivity : PlayerActivity() {

    private lateinit var list: RecyclerView
    private lateinit var empty: TextView
    private var tracks: List<Track> = emptyList()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_library)
        list = findViewById(R.id.list)
        empty = findViewById(R.id.empty)
        list.layoutManager = LinearLayoutManager(this)
        list.adapter = Adapter()

        // No startForegroundService here: connecting a MediaController binds
        // PlayerService, and media3 promotes it to the foreground itself when
        // playback starts. Starting it by hand means owing Android a
        // startForeground() within five seconds that we cannot honour before
        // there is anything to play.
        ensurePermissions()
    }

    override fun onResume() {
        super.onResume()
        load()
    }

    private fun ensurePermissions() {
        val want = arrayOf(
            Manifest.permission.READ_EXTERNAL_STORAGE,
            // Not for location. On API 30 a BLE scan silently returns nothing
            // without it.
            Manifest.permission.ACCESS_FINE_LOCATION,
        ).filter {
            checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED
        }
        if (want.isNotEmpty()) requestPermissions(want.toTypedArray(), 1)
    }

    override fun onRequestPermissionsResult(
        requestCode: Int,
        permissions: Array<out String>,
        grantResults: IntArray,
    ) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        load()
    }

    private fun load() {
        if (checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE)
            != PackageManager.PERMISSION_GRANTED
        ) {
            empty.setText(R.string.need_storage)
            empty.visibility = View.VISIBLE
            return
        }
        tracks = Library.tracks(this)
        empty.visibility = if (tracks.isEmpty()) View.VISIBLE else View.GONE
        if (tracks.isEmpty()) empty.setText(R.string.no_music)
        list.adapter?.notifyDataSetChanged()
    }

    private fun play(index: Int) {
        val c = controller ?: return
        c.setMediaItems(PlayerService.items(tracks), index, 0L)
        c.prepare()
        c.play()
        startActivity(Intent(this, NowPlayingActivity::class.java))
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        // The right softkey / menu goes to what is playing without hunting for
        // it, which is the one thing you might want with gloves on.
        if (keyCode == KeyEvent.KEYCODE_MENU) {
            startActivity(Intent(this, NowPlayingActivity::class.java))
            return true
        }
        if (keyCode == KeyEvent.KEYCODE_0) {
            startActivity(Intent(this, SettingsActivity::class.java))
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    private inner class Adapter : RecyclerView.Adapter<Row>() {
        override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): Row =
            Row(LayoutInflater.from(parent.context)
                .inflate(R.layout.item_track, parent, false))

        override fun getItemCount(): Int = tracks.size

        override fun onBindViewHolder(holder: Row, position: Int) {
            val t = tracks[position]
            holder.title.text = t.title
            holder.sub.text = if (t.artist.isEmpty()) t.album else t.artist
            holder.itemView.setOnClickListener { play(holder.bindingAdapterPosition) }
        }
    }

    private class Row(v: View) : RecyclerView.ViewHolder(v) {
        val title: TextView = v.findViewById(R.id.title)
        val sub: TextView = v.findViewById(R.id.sub)
    }
}

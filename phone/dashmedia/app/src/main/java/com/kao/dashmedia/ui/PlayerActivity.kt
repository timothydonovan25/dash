package com.kao.dashmedia.ui

import android.Manifest
import android.content.ComponentName
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.provider.Settings
import android.util.Log
import android.view.KeyEvent
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.media3.session.MediaController
import androidx.media3.session.SessionToken
import com.google.common.util.concurrent.ListenableFuture
import com.kao.dashmedia.DashLink
import com.kao.dashmedia.Keys
import com.kao.dashmedia.LyricIndex
import com.kao.dashmedia.Lyrics
import com.kao.dashmedia.MediaWatcher
import com.kao.dashmedia.PlayMode
import com.kao.dashmedia.Prefs
import com.kao.dashmedia.PlayerService
import com.kao.dashmedia.R

/**
 * What every screen shares: the controller, the chrome strips, and the three
 * softkeys.
 *
 * The softkey scheme is fixed across the whole app, because on a keypad the
 * only thing worse than too few controls is controls that move:
 *
 *   left    settings
 *   middle  cycle the repeat scope (all / album / artist)
 *   right   swap between the library and what is playing
 */
abstract class PlayerActivity : AppCompatActivity() {

    protected var controller: MediaController? = null
        private set

    private var future: ListenableFuture<MediaController>? = null

    private val redraw: () -> Unit = { drawChrome(); onChrome() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        volumeControlStream = android.media.AudioManager.STREAM_MUSIC
    }

    override fun onStart() {
        super.onStart()
        PlayerService.listeners.add(redraw)
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
                    drawChrome()
                }
            } catch (e: Exception) {
                Log.w("PlayerActivity", "no session", e)
            }
        }, ContextCompat.getMainExecutor(this))
    }

    override fun onStop() {
        PlayerService.listeners.remove(redraw)
        future?.let { MediaController.releaseFuture(it) }
        future = null
        controller = null
        super.onStop()
    }

    /** Called on the main thread once the controller is live. */
    protected open fun onController(c: MediaController) {}

    /** Labels for the three softkeys, left to right. */
    protected open fun softLabels(): Triple<String, String, String> =
        Triple(getString(R.string.k_settings), modeLabel(), getString(R.string.k_playing))

    /** What the queue is doing now, read from the player rather than remembered. */
    protected fun modeLabel(): String {
        val c = controller ?: return ""
        return getString(
            when (PlayMode.of(c.repeatMode, c.shuffleModeEnabled)) {
                PlayMode.RepeatAll -> R.string.repeat
                PlayMode.RepeatOne -> R.string.repeat_one
                PlayMode.Shuffle -> R.string.shuffle
            }
        )
    }

    // ---- chrome ----------------------------------------------------------

    /**
     * The top strip carries the dash link, which is the one piece of state
     * that matters on the bike and belongs on every screen rather than buried
     * in a menu. The bottom strip says what the three keys do, because
     * nothing on this phone is discoverable by looking at it.
     */
    protected fun drawChrome() {
        val svc = PlayerService.instance
        findViewById<TextView>(R.id.strip_link)?.text = svc?.linkStatus ?: ""
        findViewById<TextView>(R.id.strip_scope)?.text =
            if (controller == null) "" else modeLabel()
        val (l, m, r) = softLabels()
        findViewById<TextView>(R.id.soft_left)?.text = l
        findViewById<TextView>(R.id.soft_middle)?.text = m
        findViewById<TextView>(R.id.soft_right)?.text = r
    }

    /**
     * A screen's own refresh on a service event. Called alongside [drawChrome],
     * never from inside it -- a screen whose refresh redraws the chrome would
     * otherwise recurse until the stack ran out.
     */
    protected open fun onChrome() {}

    // ---- keys ------------------------------------------------------------

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (event.action == KeyEvent.ACTION_DOWN) {
            when {
                Keys.isSoftLeft(event) -> { openSettings(); return true }
                Keys.isSoftMiddle(event) -> { toggleMode(); return true }
                Keys.isSoftRight(event) -> { onSoftRight(); return true }
            }
        }
        // Swallow the up event too, or the launcher sees a stray release.
        if (event.action == KeyEvent.ACTION_UP &&
            (Keys.isSoftLeft(event) || Keys.isSoftMiddle(event) || Keys.isSoftRight(event))
        ) {
            return true
        }
        return super.dispatchKeyEvent(event)
    }

    /** Right softkey. Browse goes to Now Playing; Now Playing comes back. */
    protected abstract fun onSoftRight()

    private fun toggleMode() {
        val c = controller ?: return
        val next = PlayMode.of(c.repeatMode, c.shuffleModeEnabled).next()
        // Both properties, every time: leaving shuffle on while switching to
        // repeat-one would come back as shuffle the moment repeat-one is left.
        c.repeatMode = next.repeatMode
        c.shuffleModeEnabled = next.shuffle
        drawChrome()
    }

    // ---- settings --------------------------------------------------------

    private fun openSettings() {
        val svc = PlayerService.instance
        val access = if (MediaWatcher.hasAccess(this)) R.string.on else R.string.off
        val items = arrayOf(
            getString(R.string.menu_sleep, sleepSummary()),
            getString(R.string.menu_boost, getString(boostLabel(svc?.boost() ?: 0))),
            getString(
                R.string.menu_trim,
                getString(if (svc?.trimSilence() != false) R.string.on else R.string.off),
            ),
            getString(R.string.menu_access, getString(access)),
            getString(R.string.menu_lyric_timing, timingSummary()),
            getString(
                R.string.menu_files,
                getString(if (Lyrics.canRead()) R.string.on else R.string.off),
            ),
            getString(R.string.menu_rescan),
            getString(R.string.menu_about),
        )
        AlertDialog.Builder(this)
            .setTitle(R.string.settings)
            .setItems(items) { _, which ->
                when (which) {
                    0 -> openSleep()
                    1 -> openBoost()
                    2 -> {
                        PlayerService.instance?.let { it.setTrimSilence(!it.trimSilence()) }
                        drawChrome()
                    }
                    3 -> startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
                    4 -> openLyricTiming()
                    5 -> startActivity(
                        Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION)
                    )
                    6 -> {
                        // The library is cached for the life of the process,
                        // which is what makes coming back instant and what
                        // stops new music from ever showing up. Lyrics are
                        // cached the same way and for the same reason.
                        PlayerService.instance?.rescan()
                        Lyrics.clear()
                        LyricIndex.clear()
                        toast(getString(R.string.rescanning))
                    }
                    7 -> openAbout()
                }
            }
            .show()
    }

    private fun sleepSummary(): String {
        val svc = PlayerService.instance
        if (svc?.sleepAtEndOfTrack == true) return getString(R.string.sleep_end)
        val at = svc?.sleepAtMs ?: 0L
        if (at == 0L) return getString(R.string.off)
        val mins = ((at - System.currentTimeMillis()) / 60_000L).coerceAtLeast(0L) + 1
        return getString(R.string.n_min, mins)
    }

    private fun openSleep() {
        val labels = arrayOf(
            getString(R.string.off),
            getString(R.string.sleep_end),
            getString(R.string.n_min, 15L),
            getString(R.string.n_min, 30L),
            getString(R.string.n_min, 60L),
        )
        AlertDialog.Builder(this)
            .setTitle(R.string.sleep_timer)
            .setItems(labels) { _, which ->
                val svc = PlayerService.instance
                when (which) {
                    0 -> svc?.setSleepTimer(0)
                    1 -> svc?.setSleepAtEndOfTrack()
                    else -> svc?.setSleepTimer(intArrayOf(0, 0, 15, 30, 60)[which])
                }
                drawChrome()
            }
            .show()
    }

    /**
     * Lyric timing, in words rather than signs.
     *
     * "+0.3" is ambiguous -- earlier or later? -- and the answer matters when
     * you are nudging by ear, so the menu says which. Steps are fine near zero
     * because that is where the correction usually lands: the screens poll the
     * position a few times a second, so a small lag is expected and a large one
     * means the file is wrong, not the player.
     */
    private val timingSteps = intArrayOf(-500, -250, -100, 0, 100, 200, 300, 500, 750, 1000)

    private fun timingLabel(ms: Int): String = when {
        ms == 0 -> getString(R.string.timing_in_time)
        ms > 0 -> getString(R.string.timing_earlier, seconds(ms))
        else -> getString(R.string.timing_later, seconds(-ms))
    }

    /** 250 -> "0.25", 100 -> "0.1", 1000 -> "1". No trailing noughts. */
    private fun seconds(ms: Int): String =
        "%.2f".format(ms / 1000f).trimEnd('0').trimEnd('.')

    private fun timingSummary(): String =
        timingLabel(Prefs(this).lyricOffsetMs)

    private fun openLyricTiming() {
        val prefs = Prefs(this)
        val current = timingSteps.indexOfFirst { it == prefs.lyricOffsetMs }
        AlertDialog.Builder(this)
            .setTitle(R.string.menu_lyric_timing_title)
            .setSingleChoiceItems(
                timingSteps.map { timingLabel(it) }.toTypedArray(),
                current,
            ) { dialog, which ->
                prefs.lyricOffsetMs = timingSteps[which]
                dialog.dismiss()
                drawChrome()
            }
            .show()
    }

    private fun boostLabel(level: Int): Int = when (level) {
        1 -> R.string.boost_low
        2 -> R.string.boost_med
        3 -> R.string.boost_high
        else -> R.string.off
    }

    private fun openBoost() {
        val labels = (0..3).map { getString(boostLabel(it)) }.toTypedArray()
        // No setMessage here. AlertDialog treats a message and a list as
        // alternatives, not as things to stack: setting both shows the message
        // and silently drops every option, which is exactly what it did.
        AlertDialog.Builder(this)
            .setTitle(R.string.boost)
            .setItems(labels) { _, which ->
                PlayerService.instance?.setBoost(which)
                drawChrome()
            }
            .show()
    }

    /**
     * From the package manager rather than BuildConfig.
     *
     * Generating BuildConfig means a Java compile step, and AGP 8.5 on JDK 21
     * cannot run one -- it fails transforming core-for-system-modules.jar. The
     * version is already recorded in the manifest, so asking for it there
     * avoids the toolchain problem entirely. If a Java source ever does appear
     * in this project, that incompatibility is what will greet it.
     */
    private fun versionName(): String =
        runCatching { packageManager.getPackageInfo(packageName, 0).versionName }
            .getOrNull() ?: "?"

    /**
     * Mostly useful when something is missing and you want to know which layer
     * to blame -- the permission, the query, or the link.
     */
    private fun openAbout() {
        val svc = PlayerService.instance
        val yes = getString(R.string.on)
        val no = getString(R.string.off)
        fun granted(p: String) =
            if (checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED) yes else no
        val body = buildString {
            append(getString(R.string.about_version, versionName()))
            append("\n")
            append(getString(R.string.about_library,
                PlayerService.library.size, PlayerService.playlists.size))
            append("\n\n")
            append(getString(R.string.about_storage,
                granted(Manifest.permission.READ_EXTERNAL_STORAGE)))
            append("\n")
            append(getString(R.string.about_location,
                granted(Manifest.permission.ACCESS_FINE_LOCATION)))
            append("\n")
            append(getString(R.string.about_files, if (Lyrics.canRead()) yes else no))
            append("\n")
            append(getString(R.string.about_notif,
                if (MediaWatcher.hasAccess(this@PlayerActivity)) yes else no))
            append("\n\n")
            append(getString(R.string.about_link, svc?.linkStatus ?: "-"))
            append("\n")
            append(getString(R.string.about_uuid, DashLink.SERVICE.toString()))
        }
        // A plain setMessage does not scroll, and on a 320 px screen the last
        // two lines were simply unreachable. A ScrollView costs nothing and
        // means this dialog can keep growing.
        val text = TextView(this).apply {
            setText(body)
            textSize = 12f
            setPadding(16, 12, 16, 12)
            setTextColor(getColor(R.color.text))
        }
        AlertDialog.Builder(this)
            .setTitle(R.string.menu_about)
            .setView(android.widget.ScrollView(this).apply { addView(text) })
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    protected fun toast(s: String) {
        android.widget.Toast.makeText(this, s, android.widget.Toast.LENGTH_SHORT).show()
    }
}

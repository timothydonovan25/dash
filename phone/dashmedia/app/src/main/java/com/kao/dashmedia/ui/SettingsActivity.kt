package com.kao.dashmedia.ui

import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import com.kao.dashmedia.MediaWatcher
import com.kao.dashmedia.PlayerService
import com.kao.dashmedia.R

/**
 * The two grants that are easy to get wrong, and the link status.
 *
 * Notification access is what lets other apps' music reach the dash, and it is
 * buried several D-pad presses deep in Settings, so there is a button for it.
 * If you have adb to hand it is one line instead:
 *   adb shell cmd notification allow_listener com.kao.dashmedia/.NotifListener
 */
class SettingsActivity : AppCompatActivity() {

    private lateinit var access: TextView
    private lateinit var link: TextView

    private val onStatus: (String) -> Unit = { s -> link.text = s }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_settings)
        access = findViewById(R.id.access)
        link = findViewById(R.id.link)
        findViewById<Button>(R.id.grant).setOnClickListener {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }
    }

    override fun onStart() {
        super.onStart()
        PlayerService.listeners.add(onStatus)
    }

    override fun onStop() {
        PlayerService.listeners.remove(onStatus)
        super.onStop()
    }

    override fun onResume() {
        super.onResume()
        access.setText(
            if (MediaWatcher.hasAccess(this)) R.string.access_on else R.string.access_off
        )
    }
}

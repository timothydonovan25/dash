package com.kao.dashmedia

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanFilter
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.content.pm.PackageManager
import android.os.Handler
import android.os.HandlerThread
import android.os.ParcelUuid
import android.util.Log
import java.util.ArrayDeque
import java.util.UUID

/**
 * The BLE half. The dash is the peripheral and advertises "Dash Media"; this
 * is the central that connects in and writes lines to it.
 *
 * Three things here are not style choices, they are the difference between a
 * link that works and one that silently drops most of what it is given:
 *
 *  1. MTU. Android's default ATT MTU is 23, i.e. 20 payload bytes. The dash
 *     asks for 256 but only the central can start the exchange, so nothing
 *     longer than 20 bytes survives until [BluetoothGatt.requestMtu] has come
 *     back. Writes wait for it.
 *  2. One operation in flight. `BluetoothGatt` allows exactly one outstanding
 *     request, write-without-response included; a second `writeCharacteristic`
 *     before `onCharacteristicWrite` returns false and the line is gone. Hence
 *     the queue.
 *  3. ACCESS_FINE_LOCATION. On API 30 a BLE scan without it returns no results
 *     and no error at all.
 */
class DashLink(
    private val context: Context,
    private val feed: Feed,
    private val onStatus: (String) -> Unit = {},
) {

    companion object {
        private const val TAG = "DashLink"

        /** §11.1. The dash advertises this so the scan can filter on it. */
        val SERVICE: UUID = UUID.fromString("6e5f0001-b5a3-f393-e0a9-e50e24dcca9e")

        /** Feed: write-no-response, one line per write, UTF-8. */
        val CHAR_FEED: UUID = UUID.fromString("6e5f0002-b5a3-f393-e0a9-e50e24dcca9e")

        /**
         * Reserved: dash -> phone transport control, notify. Nothing subscribes
         * to it yet; see docs/phone-media-app.md. Kept here so the number is
         * written down in exactly one place when it does get built.
         */
        val CHAR_CTRL: UUID = UUID.fromString("6e5f0003-b5a3-f393-e0a9-e50e24dcca9e")

        private const val MTU = 256

        /** Deep enough for a track change plus a beat; anything more is stale. */
        private const val QUEUE_MAX = 32

        private const val RETRY_MIN_MS = 2_000L
        private const val RETRY_MAX_MS = 30_000L
        private const val BEAT_TICK_MS = 2_000L
    }

    private val thread = HandlerThread("dashlink").apply { start() }
    private val handler = Handler(thread.looper)

    private val queue = ArrayDeque<ByteArray>()
    private var busy = false
    private var ready = false

    private var gatt: BluetoothGatt? = null
    private var scanning = false
    private var retryMs = RETRY_MIN_MS
    private var running = false

    private val adapter: BluetoothAdapter?
        get() = (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    fun hasPermissions(): Boolean =
        context.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) ==
            PackageManager.PERMISSION_GRANTED

    fun start() = handler.post {
        if (running) return@post
        running = true
        handler.postDelayed(beat, BEAT_TICK_MS)
        scan()
    }

    fun stop() = handler.post {
        running = false
        handler.removeCallbacks(beat)
        handler.removeCallbacks(retry)
        stopScan()
        teardown()
    }

    /** Hand lines to the link. Safe from any thread. */
    fun push(lines: List<ByteArray>) {
        if (lines.isEmpty()) return
        handler.post {
            for (l in lines) {
                // Drop the oldest rather than the newest: if the link is
                // stalled, what is playing now beats what was playing then.
                while (queue.size >= QUEUE_MAX) queue.poll()
                queue.add(l)
            }
            pump()
        }
    }

    private val beat = object : Runnable {
        override fun run() {
            push(feed.tick())
            if (running) handler.postDelayed(this, BEAT_TICK_MS)
        }
    }

    private val retry = Runnable { if (running) scan() }

    // ---- connection ------------------------------------------------------

    @SuppressLint("MissingPermission")
    private fun scan() {
        val a = adapter ?: run { status("no bluetooth adapter"); return }
        if (!a.isEnabled) { status("bluetooth off"); backoff(); return }
        if (!hasPermissions()) { status("location permission needed to scan"); return }
        val scanner = a.bluetoothLeScanner ?: run { status("no scanner"); backoff(); return }
        if (scanning) return
        scanning = true
        status("scanning for dash")
        scanner.startScan(
            listOf(ScanFilter.Builder().setServiceUuid(ParcelUuid(SERVICE)).build()),
            ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(),
            scanCallback,
        )
    }

    @SuppressLint("MissingPermission")
    private fun stopScan() {
        if (!scanning) return
        scanning = false
        adapter?.bluetoothLeScanner?.stopScan(scanCallback)
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            handler.post {
                if (gatt != null) return@post
                stopScan()
                connect(result.device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            handler.post {
                scanning = false
                status("scan failed ($errorCode)")
                backoff()
            }
        }
    }

    @SuppressLint("MissingPermission")
    private fun connect(device: BluetoothDevice) {
        status("connecting to ${device.address}")
        // autoConnect = false for the first attempt: it is much faster, and we
        // already know the device is in range because we just saw it advertise.
        gatt = device.connectGatt(context, false, callback, BluetoothDevice.TRANSPORT_LE)
    }

    private val callback = object : BluetoothGattCallback() {

        @SuppressLint("MissingPermission")
        override fun onConnectionStateChange(g: BluetoothGatt, gattStatus: Int, newState: Int) {
            handler.post {
                if (newState == BluetoothProfile.STATE_CONNECTED) {
                    retryMs = RETRY_MIN_MS
                    status("connected, discovering")
                    g.discoverServices()
                } else {
                    status("disconnected ($gattStatus)")
                    teardown()
                    backoff()
                }
            }
        }

        @SuppressLint("MissingPermission")
        override fun onServicesDiscovered(g: BluetoothGatt, gattStatus: Int) {
            handler.post {
                if (g.getService(SERVICE)?.getCharacteristic(CHAR_FEED) == null) {
                    status("dash media service not found")
                    g.disconnect()
                    return@post
                }
                // Nothing may be written before this returns; see (1) above.
                if (!g.requestMtu(MTU)) {
                    status("requestMtu refused; writing at default MTU")
                    open()
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, gattStatus: Int) {
            handler.post {
                Log.i(TAG, "mtu=$mtu status=$gattStatus")
                open()
            }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            c: BluetoothGattCharacteristic,
            gattStatus: Int,
        ) {
            handler.post {
                busy = false
                pump()
            }
        }
    }

    private fun open() {
        if (ready) return
        ready = true
        status("linked")
        push(feed.onConnect())
    }

    @SuppressLint("MissingPermission")
    private fun teardown() {
        gatt?.let {
            it.disconnect()
            it.close()
        }
        gatt = null
        ready = false
        busy = false
        queue.clear()
        feed.onDisconnect()
    }

    private fun backoff() {
        if (!running) return
        handler.removeCallbacks(retry)
        handler.postDelayed(retry, retryMs)
        retryMs = (retryMs * 2).coerceAtMost(RETRY_MAX_MS)
    }

    // ---- the write queue -------------------------------------------------

    @SuppressLint("MissingPermission")
    private fun pump() {
        if (busy || !ready) return
        val g = gatt ?: return
        val c = g.getService(SERVICE)?.getCharacteristic(CHAR_FEED) ?: return
        val line = queue.poll() ?: return
        c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        @Suppress("DEPRECATION")
        c.value = line
        @Suppress("DEPRECATION")
        if (!g.writeCharacteristic(c)) {
            // Put it back and wait for the next event rather than spinning.
            queue.addFirst(line)
            return
        }
        busy = true
        Log.d(TAG, "-> " + String(line, Charsets.UTF_8))
    }

    private fun status(s: String) {
        Log.i(TAG, s)
        onStatus(s)
    }
}

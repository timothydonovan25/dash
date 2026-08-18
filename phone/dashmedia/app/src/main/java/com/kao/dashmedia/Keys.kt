package com.kao.dashmedia

import android.view.KeyEvent

/**
 * The XP3900's keypad, in one place.
 *
 * The three softkeys under the screen are the whole navigation scheme on a
 * phone with no touchscreen, and two of them do not exist in the SDK.
 * /system/usr/keylayout/soc_matrix_keypad_0.kl on the device maps them to
 * MULTIFUNC_CENTER and MULTIFUNC_RIGHT, which are Sonim additions to the
 * framework, so KeyEvent has no constant to use and the numbers below were
 * read off the device by logging dispatchKeyEvent:
 *
 *   left    scan 139 -> keyCode 82   KEYCODE_MENU
 *   middle  scan 102 -> keyCode 294  KEYCODE_MULTIFUNC_CENTER
 *   right   scan 355 -> keyCode 295  KEYCODE_MULTIFUNC_RIGHT
 *
 * If this app is ever run on another handset those two will be wrong, which
 * is why the scan codes are checked as well: a keypad that reports the same
 * physical key still works even if the framework numbers it differently.
 */
object Keys {

    const val SOFT_LEFT = KeyEvent.KEYCODE_MENU     // 82
    const val SOFT_MIDDLE = 294                     // KEYCODE_MULTIFUNC_CENTER
    const val SOFT_RIGHT = 295                      // KEYCODE_MULTIFUNC_RIGHT

    private const val SCAN_LEFT = 139
    private const val SCAN_MIDDLE = 102
    private const val SCAN_RIGHT = 355

    /** OK in the middle of the D-pad. Reported as ENTER, not DPAD_CENTER. */
    fun isSelect(e: KeyEvent): Boolean =
        e.keyCode == KeyEvent.KEYCODE_ENTER ||
            e.keyCode == KeyEvent.KEYCODE_DPAD_CENTER ||
            e.keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER

    fun isSoftLeft(e: KeyEvent): Boolean = e.keyCode == SOFT_LEFT || e.scanCode == SCAN_LEFT

    fun isSoftMiddle(e: KeyEvent): Boolean = e.keyCode == SOFT_MIDDLE || e.scanCode == SCAN_MIDDLE

    fun isSoftRight(e: KeyEvent): Boolean = e.keyCode == SOFT_RIGHT || e.scanCode == SCAN_RIGHT
}

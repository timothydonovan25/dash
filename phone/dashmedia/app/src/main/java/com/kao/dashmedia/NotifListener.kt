package com.kao.dashmedia

import android.service.notification.NotificationListenerService

/**
 * Does nothing, and has to exist.
 *
 * `MediaSessionManager.getActiveSessions()` will only talk to a caller that
 * owns an enabled notification listener -- it is the permission gate for
 * reading other apps' playback. We never look at a single notification.
 *
 * Enable it with:
 *   adb shell cmd notification allow_listener com.kao.dashmedia/.NotifListener
 * which is worth knowing on a phone whose Settings you navigate with a D-pad.
 */
class NotifListener : NotificationListenerService()

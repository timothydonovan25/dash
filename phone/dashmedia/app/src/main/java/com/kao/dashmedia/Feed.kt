package com.kao.dashmedia

/**
 * The wire protocol to the dash, and the decision of what is worth sending.
 *
 * This file has no Android imports on purpose, for the same reason
 * `components/core/` has no ESP headers: the protocol is the part that has to
 * be right, and it is only cheap to test if it does not need a device. See
 * `docs/system-architecture.md` §11.1 for the contract this implements.
 */

/** Tag `S`. The dash reads these as raw digits, not as an enum ordinal. */
enum class PlayState(val tag: Char) {
    Stopped('0'),
    Playing('1'),
    Paused('2'),
}

/** Everything the dash knows about what is playing. */
data class Snapshot(
    val title: String = "",
    val artist: String = "",
    val album: String = "",
    val state: PlayState = PlayState.Stopped,
)

object Proto {
    /** §11.1: the dash truncates to 59 bytes, leaving room for a NUL in `char[60]`. */
    const val MAX_VALUE_BYTES = 59

    const val TAG_TITLE = 'T'
    const val TAG_ARTIST = 'A'

    /** aLbum. `A` was already artist. */
    const val TAG_ALBUM = 'L'
    const val TAG_STATE = 'S'

    /**
     * Cut to [max] UTF-8 bytes without splitting a character.
     *
     * The dash truncates too, so this is belt and braces -- but it truncates
     * at a byte count and cannot know where a character started. Doing it here
     * as well is the difference between a title that stops early and one that
     * ends in a replacement glyph.
     */
    fun truncate(s: String, max: Int = MAX_VALUE_BYTES): String {
        val bytes = s.toByteArray(Charsets.UTF_8)
        if (bytes.size <= max) return s
        var end = max
        // 10xxxxxx is a UTF-8 continuation byte, so `end` is mid-character.
        // Walk back to the first byte that starts one.
        while (end > 0 && (bytes[end].toInt() and 0xC0) == 0x80) end--
        return String(bytes, 0, end, Charsets.UTF_8)
    }

    /**
     * Strip anything that would look like a line break or a control code.
     *
     * The end of the write is the end of the line, so an embedded newline
     * cannot split a record -- but it would still land in the middle of a
     * `char[60]` that a screen is about to draw.
     */
    fun sanitize(s: String): String =
        s.map { if (it.code < 0x20 || it.code == 0x7F) ' ' else it }
            .joinToString("")
            .trim()

    /**
     * One line: tag, space, value. An empty value is legal and is how a field
     * gets cleared -- a track with no album sends `"L "`.
     */
    fun line(tag: Char, value: String): ByteArray =
        "$tag ${truncate(sanitize(value))}".toByteArray(Charsets.UTF_8)

    fun all(s: Snapshot): List<ByteArray> = listOf(
        line(TAG_TITLE, s.title),
        line(TAG_ARTIST, s.artist),
        line(TAG_ALBUM, s.album),
        line(TAG_STATE, s.state.tag.toString()),
    )
}

/**
 * Holds what the dash is believed to be showing, and emits only the lines
 * needed to make that true again.
 *
 * A [MediaController] callback fires on every position update, which changes
 * none of our four fields; without this the link would carry a write a second
 * for a song that has not changed.
 *
 * The caller owns the transport. [update], [onConnect] and [tick] return lines
 * to write; nothing here touches Bluetooth.
 */
class Feed(private val nowMs: () -> Long = { System.currentTimeMillis() }) {

    /** How often `S` is re-sent while connected, as a liveness beat. */
    var beatMs: Long = 10_000

    private var current = Snapshot()

    /**
     * What the dash holds, or null when it holds nothing. Null is both
     * "never connected" and "disconnected": §11.1 requires the dash to zero
     * `Media` on disconnect, so after a drop it genuinely knows nothing and
     * the whole snapshot has to go again.
     */
    private var mirror: Snapshot? = null

    private var lastBeatMs = 0L

    /** New truth from the phone. Returns what the dash still needs to hear. */
    fun update(s: Snapshot): List<ByteArray> {
        current = s
        val held = mirror ?: return emptyList()
        if (held == s) return emptyList()
        val out = ArrayList<ByteArray>(4)
        if (held.title != s.title) out.add(Proto.line(Proto.TAG_TITLE, s.title))
        if (held.artist != s.artist) out.add(Proto.line(Proto.TAG_ARTIST, s.artist))
        if (held.album != s.album) out.add(Proto.line(Proto.TAG_ALBUM, s.album))
        if (held.state != s.state) {
            out.add(Proto.line(Proto.TAG_STATE, s.state.tag.toString()))
            lastBeatMs = nowMs()
        }
        mirror = s
        return out
    }

    /** The link came up. Everything goes, because the dash starts at zero. */
    fun onConnect(): List<ByteArray> {
        mirror = current
        lastBeatMs = nowMs()
        return Proto.all(current)
    }

    /** The link went away. The dash zeroes itself; stop pretending otherwise. */
    fun onDisconnect() {
        mirror = null
    }

    /** Call on a timer. Re-sends `S` occasionally so a dropped write heals. */
    fun tick(): List<ByteArray> {
        val held = mirror ?: return emptyList()
        val now = nowMs()
        if (now - lastBeatMs < beatMs) return emptyList()
        lastBeatMs = now
        return listOf(Proto.line(Proto.TAG_STATE, held.state.tag.toString()))
    }

    /** For the settings screen, so a human can see what the dash was told. */
    fun snapshot(): Snapshot = current

    fun connected(): Boolean = mirror != null
}

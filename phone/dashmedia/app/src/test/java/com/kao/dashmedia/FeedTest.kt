package com.kao.dashmedia

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The protocol tests. These run on the JVM -- no device, no emulator -- which
 * is the whole reason Feed.kt has no Android imports.
 *
 *   ./gradlew :app:testDebugUnitTest
 */
class FeedTest {

    private fun str(b: ByteArray) = String(b, Charsets.UTF_8)
    private fun strs(l: List<ByteArray>) = l.map { str(it) }

    // ---- Proto -----------------------------------------------------------

    @Test
    fun `line is tag space value`() {
        assertEquals("T The Devil In I", str(Proto.line('T', "The Devil In I")))
        assertEquals("S 1", str(Proto.line('S', "1")))
    }

    @Test
    fun `empty value still emits a line so the field can be cleared`() {
        // A track with no album has to overwrite the previous track's album.
        assertEquals("L ", str(Proto.line('L', "")))
    }

    @Test
    fun `long values stop at 59 bytes`() {
        val long = "a".repeat(200)
        val out = str(Proto.line('T', long))
        assertEquals(2 + 59, out.toByteArray(Charsets.UTF_8).size)
    }

    @Test
    fun `truncation never splits a character`() {
        // Two-byte characters: 30 of them is 60 bytes, one over the limit, so
        // the cut lands mid-character unless it is walked back.
        val s = "é".repeat(30)
        val cut = Proto.truncate(s)
        assertEquals(29, cut.length)
        assertEquals(58, cut.toByteArray(Charsets.UTF_8).size)
        assertTrue(s.startsWith(cut))
    }

    @Test
    fun `four byte characters survive truncation`() {
        val s = "x".repeat(56) + "🎸"   // 56 + 4 = 60 bytes
        val cut = Proto.truncate(s)
        assertEquals(56, cut.toByteArray(Charsets.UTF_8).size)
    }

    @Test
    fun `control characters are flattened`() {
        // The end of the write is the end of the line, so an embedded newline
        // cannot break framing -- but it would still land in a char[60] that a
        // screen is about to draw.
        assertEquals("T one two", str(Proto.line('T', "one\ntwo")))
        assertEquals("T one two", str(Proto.line('T', "  one\ttwo  ")))
    }

    // ---- Feed ------------------------------------------------------------

    private val song = Snapshot("Vermilion", "Slipknot", "Vol. 3", PlayState.Playing)

    @Test
    fun `nothing is sent before the link is up`() {
        val f = Feed()
        assertEquals(emptyList<String>(), strs(f.update(song)))
    }

    @Test
    fun `connect sends the whole snapshot`() {
        val f = Feed()
        f.update(song)
        assertEquals(
            listOf("T Vermilion", "A Slipknot", "L Vol. 3", "S 1"),
            strs(f.onConnect()),
        )
    }

    @Test
    fun `an unchanged update sends nothing`() {
        val f = Feed()
        f.update(song)
        f.onConnect()
        // This is the case that matters: a MediaController callback fires on
        // every position update, and none of them change our four fields.
        assertEquals(emptyList<String>(), strs(f.update(song)))
        assertEquals(emptyList<String>(), strs(f.update(song.copy())))
    }

    @Test
    fun `only the changed fields go`() {
        val f = Feed()
        f.update(song)
        f.onConnect()
        val next = song.copy(title = "Duality", state = PlayState.Paused)
        assertEquals(listOf("T Duality", "S 2"), strs(f.update(next)))
    }

    @Test
    fun `reconnect resends everything because the dash zeroed itself`() {
        val f = Feed()
        f.update(song)
        f.onConnect()
        f.onDisconnect()
        // Changes made while away are remembered but not sent.
        val next = song.copy(title = "Duality")
        assertEquals(emptyList<String>(), strs(f.update(next)))
        assertEquals(
            listOf("T Duality", "A Slipknot", "L Vol. 3", "S 1"),
            strs(f.onConnect()),
        )
    }

    @Test
    fun `state beats after the interval and not before`() {
        var now = 0L
        val f = Feed { now }
        f.update(song)
        f.onConnect()
        now += 9_000
        assertEquals(emptyList<String>(), strs(f.tick()))
        now += 2_000
        assertEquals(listOf("S 1"), strs(f.tick()))
        assertEquals(emptyList<String>(), strs(f.tick()))
    }

    @Test
    fun `a state change resets the beat`() {
        var now = 0L
        val f = Feed { now }
        f.update(song)
        f.onConnect()
        now += 9_000
        assertEquals(listOf("S 2"), strs(f.update(song.copy(state = PlayState.Paused))))
        now += 2_000
        // The beat is redundant right after a real change; it should wait.
        assertEquals(emptyList<String>(), strs(f.tick()))
    }

    @Test
    fun `nothing beats while disconnected`() {
        var now = 0L
        val f = Feed { now }
        f.update(song)
        now += 60_000
        assertEquals(emptyList<String>(), strs(f.tick()))
    }
}

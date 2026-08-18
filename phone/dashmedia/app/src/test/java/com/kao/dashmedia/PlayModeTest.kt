package com.kao.dashmedia

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class PlayModeTest {

    @Test
    fun `the key cycles all, one, shuffle, and comes back round`() {
        assertEquals(PlayMode.RepeatOne, PlayMode.RepeatAll.next())
        assertEquals(PlayMode.Shuffle, PlayMode.RepeatOne.next())
        assertEquals(PlayMode.RepeatAll, PlayMode.Shuffle.next())
    }

    @Test
    fun `three presses return to where you started`() {
        var m = PlayMode.RepeatAll
        repeat(3) { m = m.next() }
        assertEquals(PlayMode.RepeatAll, m)
    }

    @Test
    fun `each mode maps to a player state`() {
        assertEquals(PlayMode.REPEAT_ALL, PlayMode.RepeatAll.repeatMode)
        assertFalse(PlayMode.RepeatAll.shuffle)

        // Repeat one still loops the queue setting-wise; the repeat mode is
        // what makes it a single track.
        assertEquals(PlayMode.REPEAT_ONE, PlayMode.RepeatOne.repeatMode)
        assertFalse(PlayMode.RepeatOne.shuffle)

        assertEquals(PlayMode.REPEAT_ALL, PlayMode.Shuffle.repeatMode)
        assertTrue(PlayMode.Shuffle.shuffle)
    }

    @Test
    fun `setting a mode and reading it back gives the same mode`() {
        // The round trip that matters: the label on screen is derived from the
        // player, not from what we last asked for.
        for (m in PlayMode.entries) {
            assertEquals(m, PlayMode.of(m.repeatMode, m.shuffle))
        }
    }

    @Test
    fun `repeat one wins over shuffle`() {
        // Both can be set at once by anything holding the session. With one
        // track looping, the order of the rest is not observable, so calling
        // it shuffle would describe something invisible.
        assertEquals(PlayMode.RepeatOne, PlayMode.of(PlayMode.REPEAT_ONE, shuffle = true))
    }

    @Test
    fun `repeat off is reported as repeat all`() {
        // This app never sets it, but the session is not exclusively ours, and
        // a queue that silently stops is not one of the three offered states.
        assertEquals(PlayMode.RepeatAll, PlayMode.of(PlayMode.REPEAT_OFF, shuffle = false))
        assertEquals(PlayMode.Shuffle, PlayMode.of(PlayMode.REPEAT_OFF, shuffle = true))
    }

    @Test
    fun `a stored name comes back, and junk does not`() {
        for (m in PlayMode.entries) assertEquals(m, PlayMode.byName(m.name))
        assertNull(PlayMode.byName(null))
        assertNull(PlayMode.byName(""))
        // A preference written by the two-state build, before repeat one existed.
        assertNull(PlayMode.byName("Repeat"))
    }
}

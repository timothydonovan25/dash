package com.kao.dashmedia

import org.junit.Assert.assertEquals
import org.junit.Test

class FmtTest {

    @Test
    fun `minutes and seconds`() {
        assertEquals("0:00", Fmt.clock(0))
        assertEquals("0:07", Fmt.clock(7_000))
        assertEquals("4:54", Fmt.clock(294_000))
    }

    @Test
    fun `seconds are zero padded but minutes are not`() {
        assertEquals("1:05", Fmt.clock(65_000))
        assertEquals("10:05", Fmt.clock(605_000))
    }

    @Test
    fun `an hour gains a field`() {
        assertEquals("1:00:00", Fmt.clock(3_600_000))
        assertEquals("2:03:04", Fmt.clock(7_384_000))
    }

    @Test
    fun `an unknown duration does not print as a huge negative`() {
        // ExoPlayer's C.TIME_UNSET reaches here before an item is ready.
        assertEquals("--:--", Fmt.clock(Long.MIN_VALUE + 1))
        assertEquals("--:--", Fmt.clock(-1))
    }

    @Test
    fun `sub-second durations round down rather than vanish`() {
        assertEquals("0:00", Fmt.clock(999))
        assertEquals("0:01", Fmt.clock(1_000))
    }
}

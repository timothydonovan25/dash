package com.kao.dashmedia

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The playlist file is the only thing in this app a user can lose, so the
 * format is tested harder than anything else: round trips, names full of
 * delimiters, and files that have been damaged.
 */
class StoreCodecTest {

    private val lists = listOf(
        OwnedList(1, "chug", listOf(861, 956, 864)),
        OwnedList(2, "empty for now", emptyList()),
        OwnedList(3, "a|b:c/d", listOf(7)),
    )

    @Test
    fun `playlists round trip`() {
        assertEquals(lists, StoreCodec.decodeLists(StoreCodec.encodeLists(lists)))
    }

    @Test
    fun `a name containing the delimiter survives`() {
        val awkward = listOf(OwnedList(9, "one${Esc.SEP}two", listOf(1, 2)))
        assertEquals(awkward, StoreCodec.decodeLists(StoreCodec.encodeLists(awkward)))
    }

    @Test
    fun `a name containing a backslash survives`() {
        val awkward = listOf(OwnedList(9, "AC\\DC \\s", listOf(1)))
        assertEquals(awkward, StoreCodec.decodeLists(StoreCodec.encodeLists(awkward)))
    }

    @Test
    fun `an empty playlist keeps its name and its emptiness`() {
        val one = listOf(OwnedList(4, "new", emptyList()))
        val back = StoreCodec.decodeLists(StoreCodec.encodeLists(one))
        assertEquals("new", back[0].name)
        assertTrue(back[0].trackIds.isEmpty())
    }

    @Test
    fun `track order is preserved, including duplicates`() {
        // The same song twice in a playlist is a legitimate thing to want.
        val one = listOf(OwnedList(5, "x", listOf(3, 1, 3, 2)))
        assertEquals(listOf(3L, 1L, 3L, 2L), StoreCodec.decodeLists(StoreCodec.encodeLists(one))[0].trackIds)
    }

    @Test
    fun `a damaged line costs one playlist, not the file`() {
        // Half-written last line, as a power cut would leave it.
        val text = StoreCodec.encodeLists(lists) + "\n4"
        val back = StoreCodec.decodeLists(text)
        assertEquals(lists, back)
    }

    @Test
    fun `junk lines are skipped rather than throwing`() {
        val text = "not-a-number\tnonsense\n\n" + StoreCodec.encodeLists(lists.take(1))
        assertEquals(listOf(lists[0]), StoreCodec.decodeLists(text))
    }

    @Test
    fun `nothing decodes to nothing`() {
        assertTrue(StoreCodec.decodeLists(null).isEmpty())
        assertTrue(StoreCodec.decodeLists("").isEmpty())
        assertTrue(StoreCodec.decodeLists("   ").isEmpty())
    }

    @Test
    fun `stats round trip`() {
        val stats = mapOf(
            10L to Stat(3, 1_700_000_000_000),
            11L to Stat(0, 0),
        )
        assertEquals(stats, StoreCodec.decodeStats(StoreCodec.encodeStats(stats)))
    }

    @Test
    fun `a stat line missing a field is dropped, the rest survive`() {
        val good = StoreCodec.encodeStats(mapOf(10L to Stat(3, 99)))
        val back = StoreCodec.decodeStats("12${Esc.SEP}5\n$good")
        assertEquals(mapOf(10L to Stat(3, 99)), back)
    }

    @Test
    fun `a timestamp beyond int range survives`() {
        // Milliseconds since epoch does not fit in an Int, and truncating it
        // would put every play in 1970.
        val stats = mapOf(1L to Stat(1, 1_760_000_000_000))
        assertEquals(stats, StoreCodec.decodeStats(StoreCodec.encodeStats(stats)))
    }
}

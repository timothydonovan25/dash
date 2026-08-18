package com.kao.dashmedia

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The queue source has to survive a restart, and it is stored as one string in
 * SharedPreferences, so the encoding is the thing worth testing: a tag that
 * looks like another kind of source must not become one.
 */
class SourceTest {

    /** The separator Esc delimits with. Named, so it is visible. */
    private val sep = Esc.SEP

    private fun round(s: Source): Source? = Source.decode(s.encode())

    @Test
    fun `every source survives a round trip`() {
        val cases = listOf(
            Source.All,
            Source.Artist("Slipknot"),
            Source.Album("Slipknot", "Vol. 3"),
            Source.Playlist(1347, "chug"),
        )
        for (c in cases) assertEquals(c, round(c))
    }

    @Test
    fun `tags containing punctuation are not delimiters`() {
        // The obvious separators all occur in real tags.
        assertEquals(Source.Artist("AC/DC"), round(Source.Artist("AC/DC")))
        assertEquals(
            Source.Album("Nine Inch Nails", "Broken: The EP"),
            round(Source.Album("Nine Inch Nails", "Broken: The EP")),
        )
        assertEquals(Source.Playlist(9, "a|b:c/d"), round(Source.Playlist(9, "a|b:c/d")))
    }

    @Test
    fun `a playlist cannot impersonate an artist`() {
        // The whole reason the type tag comes first and the separator is a
        // control character.
        val sneaky = Source.Playlist(1, "artist${sep}Gojira")
        assertEquals(sneaky, round(sneaky))
    }

    @Test
    fun `an empty artist round trips, because untagged files exist`() {
        assertEquals(Source.Artist(""), round(Source.Artist("")))
        assertEquals(Source.Album("", ""), round(Source.Album("", "")))
    }

    @Test
    fun `unicode survives`() {
        assertEquals(Source.Artist("Mötley Crüe"), round(Source.Artist("Mötley Crüe")))
        assertEquals(Source.Playlist(3, "🎸 riffs"), round(Source.Playlist(3, "🎸 riffs")))
    }

    @Test
    fun `a name containing the escape character survives too`() {
        // Escaping the separator introduces a second special character, and a
        // backslash in a playlist name is a good deal more likely than a
        // control code.
        assertEquals(Source.Playlist(2, "AC\\DC"), round(Source.Playlist(2, "AC\\DC")))
        assertEquals(Source.Artist("back\\\\slash"), round(Source.Artist("back\\\\slash")))
        assertEquals(Source.Artist("trailing\\"), round(Source.Artist("trailing\\")))
        assertEquals(Source.Album("a\\s", "b\\s"), round(Source.Album("a\\s", "b\\s")))
    }

    @Test
    fun `nothing stored decodes to nothing`() {
        assertNull(Source.decode(null))
        assertNull(Source.decode(""))
    }

    @Test
    fun `junk decodes to nothing rather than throwing`() {
        // A preference file written by an older build, or a corrupted one.
        assertNull(Source.decode("nonsense"))
        assertNull(Source.decode("artist"))
        assertNull(Source.decode("album${sep}only-one-part"))
        assertNull(Source.decode("playlist${sep}not-a-number"))
    }

    @Test
    fun `a playlist with no stored name still decodes`() {
        assertEquals(Source.Playlist(7, ""), Source.decode("playlist${sep}7"))
    }
}

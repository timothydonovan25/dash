package com.kao.dashmedia

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class T9Test {

    @Test
    fun `the first letters of a title match it`() {
        // g-o-j = 4-6-5
        assertTrue(T9.matches("465", "Gojira"))
        assertTrue(T9.matches("4", "Gojira"))
        assertFalse(T9.matches("466", "Gojira"))
    }

    @Test
    fun `any word matches, not only the first`() {
        // Otherwise a song is only findable if you remember how its title opens.
        assertTrue(T9.matches("3", "The Devil In I"))       // Devil
        assertTrue(T9.matches("222", "Gojira - Backbone"))  // Backbone
        assertTrue(T9.matches("86", "From Mars To Sirius"))  // To
        assertTrue(T9.matches("747487", "From Mars To Sirius"))  // s-i-r-i-u-s
        assertFalse(T9.matches("46", "From Mars To Sirius"))  // no word starts g/h/i + m/n/o
    }

    @Test
    fun `a match must start a word, not appear in the middle`() {
        // "one" inside "Backbone" should not match, or every query matches
        // nearly everything and the search is useless.
        assertFalse(T9.matches("663", "Backbone"))
    }

    @Test
    fun `punctuation separates words`() {
        assertTrue(T9.matches("22", "AC/DC"))   // ac
        assertTrue(T9.matches("32", "AC/DC"))   // dc
        assertTrue(T9.matches("7", "Rock 'n' Roll"))
    }

    @Test
    fun `accents are reachable from a keypad that cannot type them`() {
        assertEquals("motley crue", T9.fold("Mötley Crüe"))
        assertTrue(T9.matches("66853", "Mötley Crüe"))
        assertTrue(T9.matches("2783", "Mötley Crüe"))   // crue
    }

    @Test
    fun `digits in a title match themselves`() {
        assertTrue(T9.matches("5", "5 Minutes Alone"))
        assertTrue(T9.matches("2004", "2004 - Vol 3"))
    }

    @Test
    fun `case does not matter`() {
        assertTrue(T9.matches("52", "LANDMVRKS"))
        assertTrue(T9.matches("52", "landmvrks"))
    }

    @Test
    fun `an empty query matches everything`() {
        // Nothing typed is not the same as nothing found.
        assertTrue(T9.matches("", "anything"))
        assertTrue(T9.matchesAny(""))
    }

    @Test
    fun `matching across fields finds a song by its artist`() {
        assertTrue(T9.matchesAny("465", "Backbone", "Gojira", "From Mars To Sirius"))
        assertTrue(T9.matchesAny("6277", "Backbone", "Gojira", "From Mars To Sirius"))
        assertFalse(T9.matchesAny("999", "Backbone", "Gojira", "From Mars To Sirius"))
    }

    @Test
    fun `a space narrows the search instead of starting a new one`() {
        // 0 is the space key. 465 is g-o-j, 2 is a-b-c.
        assertTrue(T9.matchesAny("465", "Backbone", "Gojira", "From Mars To Sirius"))
        assertTrue(T9.matchesAny("465 222", "Backbone", "Gojira", "From Mars To Sirius"))
        // Both terms must land somewhere; 999 lands nowhere.
        assertFalse(T9.matchesAny("465 999", "Backbone", "Gojira", "From Mars To Sirius"))
    }

    @Test
    fun `terms may come from different fields`() {
        // The search people actually type: artist then title. Requiring one
        // field to satisfy both would fail it.
        assertTrue(T9.matchesAny("465 222", "Backbone", "Gojira", "From Mars"))
    }

    @Test
    fun `terms need not be in order or adjacent`() {
        assertTrue(T9.matches("747487 6277", "From Mars To Sirius"))
        assertTrue(T9.matches("3766 747487", "From Mars To Sirius"))
    }

    @Test
    fun `a trailing space changes nothing until something follows it`() {
        // Typing 0 then thinking about it should not empty the results.
        assertTrue(T9.matches("465 ", "Gojira"))
        assertTrue(T9.matches(" ", "anything"))
    }

    @Test
    fun `a keyed line matches the same things a plain one does`() {
        // The lyric index keys every line once and matches against that, so it
        // has to agree with the ordinary matcher or searching lyrics would find
        // different songs than searching titles.
        val text = "Everyone is doing their best"
        val keyed = T9.keyLine(text)
        assertTrue(T9.keyedMatches("3", keyed))            // everyone, doing -> 3...
        assertTrue(T9.keyedMatches("38", keyed))           // everyone -> 38379663
        assertTrue(T9.keyedMatches("2378", keyed))         // best
        assertFalse(T9.keyedMatches("999", keyed))
    }

    @Test
    fun `a keyed line requires every term, as word starts`() {
        val keyed = T9.keyLine("Everyone is doing their best")
        // Both terms present, in any order.
        assertTrue(T9.keyedMatches("2378 3646", keyed))    // best + doing
        assertTrue(T9.keyedMatches("3646 2378", keyed))
        // One term absent fails the whole line.
        assertFalse(T9.keyedMatches("2378 999", keyed))
        // Mid-word does not count.
        assertFalse(T9.keyedMatches("478", keyed))
    }

    @Test
    fun `an empty query matches no line, unlike an empty search`() {
        // Nothing typed should not make every lyric in the library a hit.
        assertFalse(T9.keyedMatches("", T9.keyLine("anything at all")))
    }

    @Test
    fun `the hint draws the boundary`() {
        assertEquals("ghi mno jkl · abc", T9.hint("465 2"))
    }

    @Test
    fun `words handles empty and symbol-only text`() {
        assertEquals(emptyList<String>(), T9.words(""))
        assertEquals(emptyList<String>(), T9.words("--- !!! ---"))
    }

    @Test
    fun `the hint says what was typed`() {
        assertEquals("ghi mno jkl", T9.hint("465"))
        assertEquals("", T9.hint(""))
    }
}

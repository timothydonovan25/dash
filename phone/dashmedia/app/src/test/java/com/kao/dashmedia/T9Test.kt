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

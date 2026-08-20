package com.kao.dashmedia

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The .lrc format is a convention rather than a specification, so most of
 * these are about what real files contain rather than what they should.
 */
class LrcTest {

    private fun times(l: Lrc) = l.lines.map { it.timeMs }
    private fun texts(l: Lrc) = l.lines.map { it.text }

    @Test
    fun `plain timestamped lines`() {
        val lrc = LrcParser.parse(
            """
            [00:12.00]First
            [00:15.30]Second
            [01:02.50]Third
            """.trimIndent()
        )
        assertEquals(listOf(12_000L, 15_300L, 62_500L), times(lrc))
        assertEquals(listOf("First", "Second", "Third"), texts(lrc))
    }

    @Test
    fun `fraction digits are read as tenths, centiseconds or milliseconds`() {
        // All three appear in the wild; two digits is the common one.
        val lrc = LrcParser.parse("[00:01.5]a\n[00:02.50]b\n[00:03.500]c")
        assertEquals(listOf(1_500L, 2_500L, 3_500L), times(lrc))
    }

    @Test
    fun `a timestamp with no fraction is whole seconds`() {
        assertEquals(listOf(9_000L), times(LrcParser.parse("[00:09]a")))
    }

    @Test
    fun `minutes past an hour keep counting`() {
        // A long track, or a DJ set tagged as one file.
        assertEquals(listOf(4_230_000L), times(LrcParser.parse("[70:30.00]a")))
    }

    @Test
    fun `one line can carry several timestamps`() {
        // A chorus is written once and pointed at repeatedly.
        val lrc = LrcParser.parse("[00:20.00][01:10.00][02:00.00]Chorus")
        assertEquals(listOf(20_000L, 70_000L, 120_000L), times(lrc))
        assertEquals(listOf("Chorus", "Chorus", "Chorus"), texts(lrc))
    }

    @Test
    fun `lines are sorted even when the file is not`() {
        val lrc = LrcParser.parse("[00:30.00]late\n[00:10.00]early\n[00:20.00]middle")
        assertEquals(listOf("early", "middle", "late"), texts(lrc))
    }

    @Test
    fun `metadata is read and kept off the lyric list`() {
        val lrc = LrcParser.parse(
            """
            [ti:Backbone]
            [ar:Gojira]
            [al:From Mars To Sirius]
            [by:someone]
            [00:01.00]words
            """.trimIndent()
        )
        assertEquals("Backbone", lrc.title)
        assertEquals("Gojira", lrc.artist)
        assertEquals("From Mars To Sirius", lrc.album)
        assertEquals(listOf("words"), texts(lrc))
    }

    @Test
    fun `a positive offset makes the words arrive earlier`() {
        val lrc = LrcParser.parse("[offset:+500]\n[00:10.00]a")
        assertEquals(listOf(9_500L), times(lrc))
    }

    @Test
    fun `a negative offset makes them arrive later`() {
        val lrc = LrcParser.parse("[offset:-500]\n[00:10.00]a")
        assertEquals(listOf(10_500L), times(lrc))
    }

    @Test
    fun `an offset cannot push a line before the start of the track`() {
        // Otherwise the first line sits at a position playback never reaches.
        val lrc = LrcParser.parse("[offset:+9000]\n[00:01.00]a")
        assertEquals(listOf(0L), times(lrc))
    }

    @Test
    fun `an instrumental gap is a real line, not a dropped one`() {
        // It is what moves the highlight off the previous line during a solo.
        val lrc = LrcParser.parse("[00:10.00]words\n[00:20.00]\n[00:40.00]more")
        assertEquals(listOf("words", "", "more"), texts(lrc))
    }

    @Test
    fun `untimed text is dropped rather than shown at the wrong moment`() {
        val lrc = LrcParser.parse("a stray line\n[00:05.00]real")
        assertEquals(listOf("real"), texts(lrc))
    }

    @Test
    fun `windows line endings and a byte order mark survive`() {
        val lrc = LrcParser.parse("﻿[ti:X]\r\n[00:01.00]a\r\n[00:02.00]b\r\n")
        assertEquals("X", lrc.title)
        assertEquals(listOf("a", "b"), texts(lrc))
    }

    @Test
    fun `nothing in, nothing out`() {
        assertTrue(LrcParser.parse(null).isEmpty)
        assertTrue(LrcParser.parse("").isEmpty)
        assertTrue(LrcParser.parse("   \n  \n").isEmpty)
        assertNull(LrcParser.parse("").title)
    }

    // ---- lookup ----------------------------------------------------------

    private val song = LrcParser.parse(
        "[00:10.00]one\n[00:20.00]two\n[00:30.00]three"
    )

    @Test
    fun `before the first line nothing is highlighted`() {
        // An intro should not light up the first line for ten seconds.
        assertEquals(-1, song.indexAt(0))
        assertEquals(-1, song.indexAt(9_999))
    }

    @Test
    fun `a line is current from its own timestamp until the next`() {
        assertEquals(0, song.indexAt(10_000))
        assertEquals(0, song.indexAt(19_999))
        assertEquals(1, song.indexAt(20_000))
        assertEquals(2, song.indexAt(30_000))
    }

    @Test
    fun `the last line stays current to the end of the track`() {
        assertEquals(2, song.indexAt(999_999))
    }

    @Test
    fun `a positive correction shows lines earlier`() {
        // What you reach for when the words feel late: at 9.8s, a 300ms
        // correction is already showing the line timed at 10s.
        assertEquals(-1, song.indexAt(9_800))
        assertEquals(0, song.indexAt(9_800, offsetMs = 300))
    }

    @Test
    fun `a negative correction holds lines back`() {
        assertEquals(0, song.indexAt(10_000))
        assertEquals(-1, song.indexAt(10_000, offsetMs = -300))
    }

    @Test
    fun `the correction means the same as a file's own offset tag`() {
        // [offset:+500] shifts the file's timestamps earlier; a +500
        // correction at lookup does the same to the same song. One word, one
        // meaning, whichever end it arrives from.
        val fromFile = LrcParser.parse("[offset:+500]\n[00:10.00]a")
        val fromSetting = LrcParser.parse("[00:10.00]a")
        assertEquals(0, fromFile.indexAt(9_500))
        assertEquals(0, fromSetting.indexAt(9_500, offsetMs = 500))
    }

    @Test
    fun `a correction cannot look before the start of the lyric`() {
        assertEquals(-1, song.indexAt(0, offsetMs = -5_000))
    }

    @Test
    fun `lookup on an empty lyric does not explode`() {
        assertEquals(-1, LrcParser.parse("").indexAt(1_000))
    }

    @Test
    fun `repeated timestamps resolve to one of them, not out of range`() {
        val dup = LrcParser.parse("[00:10.00]a\n[00:10.00]b")
        val i = dup.indexAt(10_000)
        assertTrue(i in dup.lines.indices)
        assertEquals(10_000L, dup.lines[i].timeMs)
    }
}

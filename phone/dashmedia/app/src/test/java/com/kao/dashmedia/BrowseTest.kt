package com.kao.dashmedia

import org.junit.Assert.assertEquals
import org.junit.Test

class BrowseTest {

    private data class Song(
        val title: String,
        override val artist: String,
        override val album: String,
    ) : Tagged

    private val lib = listOf(
        Song("Vermilion", "Slipknot", "Vol. 3"),
        Song("Backbone", "Gojira", "From Mars To Sirius"),
        Song("Left Behind", "Slipknot", "Iowa"),
        Song("Untagged", "", ""),
        Song("Hits A", "Queen", "Greatest Hits"),
        Song("Hits B", "Slipknot", "Greatest Hits"),
        Song("lowercase", "acdc", "Back In Black"),
        Song("uppercase", "ACDC", "Back In Black"),
    )

    @Test
    fun `artists are distinct and case-insensitively sorted with untagged last`() {
        assertEquals(
            listOf("ACDC", "acdc", "Gojira", "Queen", "Slipknot", ""),
            Browse.artists(lib),
        )
    }

    @Test
    fun `albums belong to the artist that has tracks on them`() {
        assertEquals(listOf("Greatest Hits", "Iowa", "Vol. 3"), Browse.albums(lib, "Slipknot"))
        assertEquals(listOf("Greatest Hits"), Browse.albums(lib, "Queen"))
    }

    @Test
    fun `a shared album title does not merge two artists`() {
        assertEquals(listOf("Hits A"), Browse.tracksOf(lib, "Queen", "Greatest Hits").map { it.title })
        assertEquals(listOf("Hits B"), Browse.tracksOf(lib, "Slipknot", "Greatest Hits").map { it.title })
    }

    @Test
    fun `case differences stay two entries rather than shuffling between runs`() {
        // Same album title, two spellings of the artist: two rows, stable order.
        assertEquals(listOf("uppercase"), Browse.tracksOf(lib, "ACDC", "Back In Black").map { it.title })
        assertEquals(listOf("lowercase"), Browse.tracksOf(lib, "acdc", "Back In Black").map { it.title })
    }

    @Test
    fun `everything by one artist across albums`() {
        assertEquals(
            listOf("Vermilion", "Left Behind", "Hits B"),
            Browse.tracksBy(lib, "Slipknot").map { it.title },
        )
    }

    @Test
    fun `a blank tag displays as Unknown but stays blank as a key`() {
        assertEquals("Unknown", Browse.display(""))
        assertEquals("Slipknot", Browse.display("Slipknot"))
        assertEquals(listOf("Untagged"), Browse.tracksBy(lib, "").map { it.title })
    }
}

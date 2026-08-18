package com.kao.dashmedia

import java.text.Normalizer

/**
 * Search on a numeric keypad.
 *
 * The phone has 0-9 and no letters, so searching means what it meant on every
 * phone before touchscreens: 4-6-5 finds Gojira because g, o and j live on
 * those keys. Matching is at word starts, so "backbone" is reachable by
 * typing its own first letters rather than only by the start of the title.
 *
 * Pure Kotlin. The whole point of a search is that it finds the right thing,
 * which is a property you can check on a JVM in a millisecond and cannot
 * reasonably check by hand on a 522-track phone.
 */
object T9 {

    private val KEYS: Map<Char, Char> = buildMap {
        fun put(digit: Char, letters: String) = letters.forEach { put(it, digit) }
        put('2', "abc")
        put('3', "def")
        put('4', "ghi")
        put('5', "jkl")
        put('6', "mno")
        put('7', "pqrs")
        put('8', "tuv")
        put('9', "wxyz")
    }

    /** What each key writes, for the on-screen hint. */
    val LETTERS: Map<Char, String> = mapOf(
        '2' to "abc", '3' to "def", '4' to "ghi", '5' to "jkl",
        '6' to "mno", '7' to "pqrs", '8' to "tuv", '9' to "wxyz",
    )

    /**
     * Lowercase and strip accents, so "Mötley" is reachable from a keypad that
     * cannot type an umlaut.
     */
    fun fold(s: String): String =
        Normalizer.normalize(s.lowercase(), Normalizer.Form.NFD)
            .replace(DIACRITICS, "")

    private val DIACRITICS = "\\p{Mn}+".toRegex()

    /** The digit a character sits on, or null if it is not typeable. */
    fun key(c: Char): Char? = when {
        c.isDigit() -> c
        else -> KEYS[c]
    }

    /**
     * The words of [text] as digit strings.
     *
     * Anything that is not a letter or digit separates words, so "AC/DC" is
     * two words and both are searchable.
     */
    fun words(text: String): List<String> {
        val out = ArrayList<String>(4)
        val sb = StringBuilder()
        for (c in fold(text)) {
            val k = key(c)
            if (k == null) {
                if (sb.isNotEmpty()) { out.add(sb.toString()); sb.setLength(0) }
            } else {
                sb.append(k)
            }
        }
        if (sb.isNotEmpty()) out.add(sb.toString())
        return out
    }

    /**
     * Does [query] -- a string of digits -- match [text]?
     *
     * True for an empty query: no search typed yet is not the same as no
     * results.
     */
    fun matches(query: String, text: String): Boolean {
        if (query.isEmpty()) return true
        return words(text).any { it.startsWith(query) }
    }

    /** Match against several fields at once: title, artist, album. */
    fun matchesAny(query: String, vararg fields: String): Boolean {
        if (query.isEmpty()) return true
        return fields.any { matches(query, it) }
    }

    /** "465" -> "ghi mno jkl", for showing what has been typed. */
    fun hint(query: String): String =
        query.mapNotNull { LETTERS[it] }.joinToString(" ")
}

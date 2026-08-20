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
     * Does [query] match [text]?
     *
     * The query is digits, and 0 is a space separating terms. Every term has
     * to begin some word; they need not be adjacent or in order, which is what
     * makes typing a second word narrow the search rather than start a new one.
     *
     * True for an empty query: nothing typed yet is not the same as nothing
     * found.
     */
    fun matches(query: String, text: String): Boolean = matchesAny(query, text)

    /**
     * The same, over title, artist and album together.
     *
     * Deliberately one haystack rather than three separate ones: "465 2" is
     * how you ask for Gojira's Backbone, and those two words live in different
     * fields. Requiring each term to be satisfied by the same field would fail
     * exactly the search people actually type.
     */
    fun matchesAny(query: String, vararg fields: String): Boolean {
        val parts = query.split(' ').filter { it.isNotEmpty() }
        if (parts.isEmpty()) return true
        val words = fields.flatMap { words(it) }
        return parts.all { part -> words.any { it.startsWith(part) } }
    }

    /**
     * The digit a single character sits on, folding accents as it goes.
     *
     * Per character rather than per string because [fold] normalises, which
     * changes a string's length -- so an index into folded text does not point
     * at the same place in the original, and highlighting needs indices that
     * do.
     */
    private fun keyOf(c: Char): Char? {
        if (c.isDigit()) return c
        val folded = Normalizer.normalize(c.lowercaseChar().toString(), Normalizer.Form.NFD)
            .replace(DIACRITICS, "")
        return KEYS[folded.firstOrNull() ?: return null]
    }

    /**
     * Where in [text] the words begun by [query]'s terms are.
     *
     * Ranges are into the original string, so they can be used to mark up the
     * text that is actually on screen. Whole words are returned rather than
     * matched prefixes: half a highlighted word looks like a rendering fault,
     * not an answer.
     */
    fun matchRanges(query: String, text: String): List<IntRange> {
        val parts = query.split(' ').filter { it.isNotEmpty() }
        if (parts.isEmpty()) return emptyList()
        val out = ArrayList<IntRange>(4)
        var i = 0
        while (i < text.length) {
            if (keyOf(text[i]) == null) { i++; continue }
            val start = i
            val word = StringBuilder()
            while (i < text.length) {
                val k = keyOf(text[i]) ?: break
                word.append(k)
                i++
            }
            val w = word.toString()
            if (parts.any { w.startsWith(it) }) out.add(start until i)
        }
        return out
    }

    /**
     * A line of text as one keyed string: " w1 w2 w3 ", each word in digits.
     *
     * Searching lyrics means testing the same line against a new query on
     * every keypress, across every line of every song. Converting the text
     * each time is the expensive part, so it is done once and kept; matching
     * then costs an indexOf per term.
     */
    fun keyLine(text: String): String =
        words(text).joinToString(separator = " ", prefix = " ", postfix = " ")

    /**
     * Does every term in [query] begin a word of [keyed]?
     *
     * [keyed] comes from [keyLine]. Because every word is surrounded by
     * spaces, "does a word start with this term" is exactly "does the string
     * contain a space followed by the term".
     */
    fun keyedMatches(query: String, keyed: String): Boolean {
        val parts = query.split(' ').filter { it.isNotEmpty() }
        if (parts.isEmpty()) return false
        return parts.all { keyed.contains(" $it") }
    }

    /**
     * "465 2" -> "ghi mno jkl · abc", for showing what has been typed.
     *
     * The boundary is drawn rather than dropped: without it the hint for two
     * words looks exactly like the hint for one long one.
     */
    fun hint(query: String): String =
        query.mapNotNull { if (it == ' ') "·" else LETTERS[it] }.joinToString(" ")
}

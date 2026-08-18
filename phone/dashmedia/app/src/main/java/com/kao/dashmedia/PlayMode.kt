package com.kao.dashmedia

/**
 * What the middle softkey cycles through.
 *
 * Three states over two independent player properties -- a repeat mode and a
 * shuffle flag -- which is why this is a type rather than two booleans read
 * from different places. Every screen shows the same label for the same pair,
 * and there is one definition of what the next press means.
 *
 * The integers match `Player.REPEAT_MODE_*` deliberately, so that mapping is
 * an equality check rather than a conversion, but they are written out here so
 * this file stays free of Android and testable on a JVM.
 */
enum class PlayMode {

    /** The queue loops in order. */
    RepeatAll,

    /** The current track loops. What is in the queue stops mattering. */
    RepeatOne,

    /** The queue loops, shuffled. */
    Shuffle;

    /** Repeat all, then one, then shuffle, then round again. */
    fun next(): PlayMode = entries[(ordinal + 1) % entries.size]

    val repeatMode: Int
        get() = if (this == RepeatOne) REPEAT_ONE else REPEAT_ALL

    val shuffle: Boolean
        get() = this == Shuffle

    companion object {
        const val REPEAT_OFF = 0
        const val REPEAT_ONE = 1
        const val REPEAT_ALL = 2

        /**
         * Read the mode back out of a player.
         *
         * Repeat-one wins over shuffle: with a single track looping, the order
         * of the rest of the queue is not observable, so reporting "shuffle"
         * would describe something nobody can see. The two can be set together
         * by anything holding the session, so the combination has to mean
         * something rather than be assumed away.
         *
         * Repeat-off maps to repeat-all, because this app never sets it and a
         * queue that silently stops is not one of the three states offered.
         */
        fun of(repeatMode: Int, shuffle: Boolean): PlayMode = when {
            repeatMode == REPEAT_ONE -> RepeatOne
            shuffle -> Shuffle
            else -> RepeatAll
        }

        fun byName(name: String?): PlayMode? = entries.firstOrNull { it.name == name }
    }
}

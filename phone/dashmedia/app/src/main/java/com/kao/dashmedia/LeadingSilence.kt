package com.kao.dashmedia

/**
 * Finding where a track actually starts.
 *
 * ExoPlayer ships silence skipping, but it cuts silence wherever it occurs --
 * including the pause a band left between the last chord and the hidden track,
 * and the breath before a quiet intro. This only trims the run of silence at
 * the very beginning and then gets out of the way for the rest of the file.
 *
 * Pure Kotlin over a PCM buffer so the arithmetic is testable without an
 * audio device, which is the only part of the audio path that has any logic
 * in it at all.
 */
object LeadingSilence {

    /**
     * Anything quieter than this counts as silence. Matches the threshold
     * ExoPlayer's own silence skipping uses for 16-bit PCM -- about -30 dBFS,
     * low enough to ignore dither and encoder noise in a "silent" lead-in,
     * high enough not to eat a genuine fade-in.
     */
    const val SILENCE_THRESHOLD = 1024

    /**
     * Byte offset of the first audible frame in [pcm], or [limit] if the whole
     * buffer is silent.
     *
     * 16-bit little-endian PCM. [frameBytes] is channels * 2, and the result is
     * always frame-aligned: handing back an offset mid-frame would swap the
     * channels for the rest of the track.
     */
    fun firstAudibleByte(
        pcm: ByteArray,
        limit: Int = pcm.size,
        frameBytes: Int = 2,
        threshold: Int = SILENCE_THRESHOLD,
    ): Int {
        if (frameBytes <= 0 || frameBytes % 2 != 0) return 0
        // Whole frames only.
        val end = (limit / frameBytes) * frameBytes
        var i = 0
        while (i < end) {
            // A frame is audible if any of its channels is.
            var j = 0
            while (j < frameBytes) {
                val lo = pcm[i + j].toInt() and 0xFF
                val hi = pcm[i + j + 1].toInt()      // signed: this is the high byte
                val sample = (hi shl 8) or lo
                if (kotlin.math.abs(sample) > threshold) return i
                j += 2
            }
            i += frameBytes
        }
        return end
    }
}

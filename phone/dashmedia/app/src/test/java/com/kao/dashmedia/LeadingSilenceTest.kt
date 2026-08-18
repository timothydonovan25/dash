package com.kao.dashmedia

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * The only part of the audio path with arithmetic in it, so the only part
 * worth testing without an audio device.
 */
class LeadingSilenceTest {

    /** 16-bit little-endian PCM from a list of samples. */
    private fun pcm(vararg samples: Int): ByteArray {
        val b = ByteArray(samples.size * 2)
        samples.forEachIndexed { i, s ->
            b[i * 2] = (s and 0xFF).toByte()
            b[i * 2 + 1] = ((s shr 8) and 0xFF).toByte()
        }
        return b
    }

    @Test
    fun `an audible buffer is not trimmed at all`() {
        val b = pcm(20000, 20000, 20000)
        assertEquals(0, LeadingSilence.firstAudibleByte(b))
    }

    @Test
    fun `leading silence is measured to the first loud sample`() {
        val b = pcm(0, 0, 0, 30000, 0, 0)
        // Three silent mono samples = six bytes.
        assertEquals(6, LeadingSilence.firstAudibleByte(b))
    }

    @Test
    fun `a fully silent buffer reports its whole length`() {
        val b = pcm(0, 0, 0, 0)
        assertEquals(8, LeadingSilence.firstAudibleByte(b))
    }

    @Test
    fun `dither below the threshold still counts as silence`() {
        // A "silent" lead-in off a CD rip is not mathematically zero.
        val b = pcm(3, -5, 200, 900, -1000, 25000)
        assertEquals(10, LeadingSilence.firstAudibleByte(b))
    }

    @Test
    fun `negative samples are as audible as positive ones`() {
        // Getting the sign wrong would make a track starting on a downward
        // wave sound like it starts with silence.
        val b = pcm(0, -30000, 0)
        assertEquals(2, LeadingSilence.firstAudibleByte(b))
    }

    @Test
    fun `the offset is frame aligned in stereo`() {
        // Left silent, right loud, in the second frame. Returning the offset of
        // the loud sample itself would put the channels out by one from then on.
        val b = pcm(0, 0, 0, 30000, 100, 100)
        val at = LeadingSilence.firstAudibleByte(b, frameBytes = 4)
        assertEquals(4, at)
        assertEquals(0, at % 4)
    }

    @Test
    fun `a partial trailing frame is never split`() {
        // Buffers do not arrive on frame boundaries.
        val b = pcm(0, 0, 0) + byteArrayOf(0)
        val at = LeadingSilence.firstAudibleByte(b, frameBytes = 4)
        assertEquals(0, at % 4)
        assertEquals(4, at)
    }

    @Test
    fun `a limit shorter than the buffer is respected`() {
        val b = pcm(0, 0, 30000, 30000)
        // Only the first two samples are in view, and both are silent.
        assertEquals(4, LeadingSilence.firstAudibleByte(b, limit = 4))
    }

    @Test
    fun `an odd frame size is refused rather than misread`() {
        val b = pcm(0, 0, 30000)
        assertEquals(0, LeadingSilence.firstAudibleByte(b, frameBytes = 3))
    }
}

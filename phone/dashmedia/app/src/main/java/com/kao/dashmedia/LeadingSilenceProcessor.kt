package com.kao.dashmedia

import androidx.media3.common.C
import androidx.media3.common.audio.AudioProcessor
import androidx.media3.common.audio.BaseAudioProcessor
import androidx.media3.common.util.UnstableApi
import java.nio.ByteBuffer

/**
 * Drops the silence at the start of a track, and nothing else.
 *
 * ExoPlayer's own `skipSilenceEnabled` removes quiet passages wherever they
 * are, which mangles anything with a deliberate gap in it. This trims only
 * until the first audible frame and is then a pass-through for the remainder
 * of the file, so the shape of the music is left alone.
 *
 * The decision of where the audio starts lives in [LeadingSilence], which is
 * plain Kotlin and tested on the JVM; this class is the plumbing that feeds it
 * buffers and splices the result back into the sink.
 *
 * Note the reset boundary: [onFlush] is what makes trimming start again, and
 * the sink flushes on a seek and on a format change. Two same-format tracks
 * played gaplessly may not flush between them, in which case the second
 * track's lead-in is left alone. That is a limitation, not a design choice.
 */
@UnstableApi
class LeadingSilenceProcessor : BaseAudioProcessor() {

    /** Off means pass everything through, so the toggle takes effect at once. */
    @Volatile
    var enabled: Boolean = true

    private var trimming = true
    private var frameBytes = 2

    override fun onConfigure(inputAudioFormat: AudioProcessor.AudioFormat):
        AudioProcessor.AudioFormat {
        // Only 16-bit PCM is understood. Anything else -- float output, or a
        // passthrough encoding heading for a receiver -- reports NOT_SET, which
        // takes this processor out of the chain rather than failing playback.
        if (inputAudioFormat.encoding != C.ENCODING_PCM_16BIT) {
            return AudioProcessor.AudioFormat.NOT_SET
        }
        frameBytes = inputAudioFormat.bytesPerFrame.coerceAtLeast(2)
        return inputAudioFormat
    }

    override fun queueInput(inputBuffer: ByteBuffer) {
        val count = inputBuffer.remaining()
        if (count == 0) return

        if (!enabled || !trimming) {
            val out = replaceOutputBuffer(count)
            out.put(inputBuffer)
            out.flip()
            return
        }

        val bytes = ByteArray(count)
        inputBuffer.get(bytes)     // consumes the input, as the contract requires

        val at = LeadingSilence.firstAudibleByte(bytes, count, frameBytes)
        if (at >= count) {
            // Still silent. Emit nothing and keep looking in the next buffer.
            replaceOutputBuffer(0).flip()
            return
        }

        trimming = false
        val kept = count - at
        val out = replaceOutputBuffer(kept)
        out.put(bytes, at, kept)
        out.flip()
    }

    override fun onFlush() {
        trimming = true
    }

    override fun onReset() {
        trimming = true
    }
}

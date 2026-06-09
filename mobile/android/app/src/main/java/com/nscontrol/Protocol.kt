package com.nscontrol

object Protocol {
    const val FRAME_SIZE = 212
    const val HID_SIZE = 8
    const val MOTION_SAMPLE_SIZE = 12
    const val MOTION_SAMPLE_COUNT = 3
    const val EXT_PAD_SIZE = 48
    const val PAD_COUNT = 4

    private const val MAGIC = 0x4E535743
    private const val VERSION = 6

    const val RUMBLE_PACKET_SIZE = 8
    const val PRECISION_RUMBLE_PACKET_SIZE = 20
    const val RUMBLE_MAGIC = 0x4E535652
    const val PRECISION_RUMBLE_MAGIC = 0x4E535648

    const val FLAG_RESET = 0x01
    const val FLAG_DISCONNECT = 0x08
    const val FLAG_SINGLE_PAD = 0x04

    private const val PAD_PRESENT = 0x01

    const val BTN_Y       = 1 shl 0
    const val BTN_B       = 1 shl 1
    const val BTN_A       = 1 shl 2
    const val BTN_X       = 1 shl 3
    const val BTN_L       = 1 shl 4
    const val BTN_R       = 1 shl 5
    const val BTN_ZL      = 1 shl 6
    const val BTN_ZR      = 1 shl 7
    const val BTN_MINUS   = 1 shl 8
    const val BTN_PLUS    = 1 shl 9
    const val BTN_LSTICK  = 1 shl 10
    const val BTN_RSTICK  = 1 shl 11
    const val BTN_HOME    = 1 shl 12
    const val BTN_CAPTURE = 1 shl 13

    const val HAT_N = 0
    const val HAT_NE = 1
    const val HAT_E = 2
    const val HAT_SE = 3
    const val HAT_S = 4
    const val HAT_SW = 5
    const val HAT_W = 6
    const val HAT_NW = 7
    const val HAT_NEUTRAL = 8

    const val STANDARD_GRAVITY: Float = 9.80665f

    fun neutralHid(): ByteArray = NativeProtocol.nativeNeutralHid()
    fun neutralMotion(): ByteArray = NativeProtocol.nativeNeutralMotion()

    fun controllerHid(
        buttons: Int,
        dpadUp: Boolean,
        dpadDown: Boolean,
        dpadLeft: Boolean,
        dpadRight: Boolean,
        lx: Float,
        ly: Float,
        rx: Float,
        ry: Float,
        present: Boolean = true
    ): ByteArray = NativeProtocol.nativeControllerHid(buttons, dpadUp, dpadDown, dpadLeft, dpadRight, lx, ly, rx, ry, present)

    fun motionFromAndroid(
        accelX: Float, accelY: Float, accelZ: Float,
        gyroX: Float, gyroY: Float, gyroZ: Float
    ): ByteArray = NativeProtocol.nativeMotionFromAndroid(accelX, accelY, accelZ, gyroX, gyroY, gyroZ)

    fun motionFromValues(
        ax: Short, ay: Short, az: Short,
        gx: Short, gy: Short, gz: Short,
        hasMotion: Boolean
    ): ByteArray = NativeProtocol.nativeMotionFromValues(ax, ay, az, gx, gy, gz, hasMotion)

    fun duplicateMotionSamples(sample: ByteArray): Array<ByteArray>? {
        if (sample.size < MOTION_SAMPLE_SIZE) return null
        val s = sample.copyOfRange(0, MOTION_SAMPLE_SIZE)
        return arrayOf(s.copyOf(), s.copyOf(), s.copyOf())
    }

    fun buildFrame(
        seq: Int,
        flags: Int,
        timestampUs: Long,
        pad0Hid: ByteArray?,
        pad0Motion: ByteArray?
    ): ByteArray = initFrame(flags, seq, timestampUs).also { frame ->
        pad0Hid?.let { setFrameHid(frame, 0, it) }
        pad0Motion?.let { setFrameMotion(frame, 0, it) }
    }

    fun initFrame(flags: Int, seq: Int, timestampUs: Long): ByteArray =
        NativeProtocol.nativeInitFrame(flags, seq, timestampUs)

    fun setFrameHid(frame: ByteArray, padIndex: Int, hid: ByteArray) =
        NativeProtocol.nativeSetFrameHid(frame, padIndex, hid)

    fun setFrameMotion(frame: ByteArray, padIndex: Int, motion: ByteArray) =
        NativeProtocol.nativeSetFrameMotion(frame, padIndex, motion)

    fun setFrameMotionSamples(frame: ByteArray, padIndex: Int, samples: Array<ByteArray>) {
        if (samples.size < MOTION_SAMPLE_COUNT) return
        NativeProtocol.nativeSetFrameMotionSamples(frame, padIndex, samples[0], samples[1], samples[2])
    }

    fun extractPad0HidFromWebFrame(src: ByteArray): ByteArray? =
        NativeProtocol.nativeExtractPadHid(src)

    fun hid(buttons: Int, hat: Int, lx: Int, ly: Int, rx: Int, ry: Int, present: Boolean): ByteArray =
        NativeProtocol.nativeHid(buttons, hat, lx, ly, rx, ry, present)
}

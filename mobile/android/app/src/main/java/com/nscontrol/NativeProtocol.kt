package com.nscontrol

object NativeProtocol {
    init {
        System.loadLibrary("nsprotocol")
    }

    external fun nativeNormalizeShortcuts(buttons: Int): Int
    external fun nativeGyroDeadzone(v: Short): Short
    external fun nativeNeutralHid(): ByteArray
    external fun nativeNeutralMotion(): ByteArray
    external fun nativeInitFrame(flags: Int, seq: Int, timestampUs: Long): ByteArray
    external fun nativeSetFrameHid(frame: ByteArray, padIndex: Int, hid: ByteArray)
    external fun nativeSetFrameMotionSamples(frame: ByteArray, padIndex: Int, m0: ByteArray, m1: ByteArray, m2: ByteArray)
    external fun nativeSetFrameMotion(frame: ByteArray, padIndex: Int, motion: ByteArray)
    external fun nativeHid(buttons: Int, hat: Int, lx: Int, ly: Int, rx: Int, ry: Int, present: Boolean): ByteArray
    external fun nativeControllerHid(
        buttons: Int, dpadUp: Boolean, dpadDown: Boolean,
        dpadLeft: Boolean, dpadRight: Boolean,
        lx: Float, ly: Float, rx: Float, ry: Float,
        present: Boolean
    ): ByteArray
    external fun nativeMotionFromValues(
        ax: Short, ay: Short, az: Short,
        gx: Short, gy: Short, gz: Short,
        hasMotion: Boolean
    ): ByteArray
    external fun nativeSetMotionRemap(axInput: Int, axSign: Int, ayInput: Int, aySign: Int, azInput: Int, azSign: Int)
    external fun nativePhoneMotion(
        accelX: Float, accelY: Float, accelZ: Float,
        gyroX: Float, gyroY: Float, gyroZ: Float
    ): ByteArray
    external fun nativeMotionFromAndroid(
        accelX: Float, accelY: Float, accelZ: Float,
        gyroX: Float, gyroY: Float, gyroZ: Float
    ): ByteArray
    external fun nativeExtractPadHid(frame: ByteArray): ByteArray?
}

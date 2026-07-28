package com.nscontrol

import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
import java.net.InetSocketAddress
import java.security.MessageDigest
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * Authenticated UDP transport to the ns-backend — the same packets the desktop
 * ns-client sends. This replaced the WebSocket path: WS is now tied to the
 * optional embedded webapp (`-w`), while native clients always use UDP, which
 * has no TCP head-of-line blocking and the lowest possible latency.
 *
 * Wire format: payload (auth region) + first 16 bytes of
 * HMAC-SHA256(payload) keyed with SHA-256(shared secret).
 */
class NsUdp(host: String, port: Int) {
    companion object {
        const val DEFAULT_PORT = 7331
        const val HMAC_TAG_SIZE = 16
        const val FRAME_AUTH_SIZE = 212   // input frame: 212 + 16 = 228 on the wire
        const val NAMES_AUTH_SIZE = 208   // ClientNamesPacket: hmac lives at 208..223

        const val SERVER_INFO_MAGIC = 0x4E535349
        const val CLIENT_ASSIGNMENT_MAGIC = 0x4E534341
        const val CLIENT_ASSIGNMENT_SIZE = 16
        const val ASSIGNMENT_FLAG_ACCEPTED = 0x01
        const val ASSIGNMENT_FLAG_SERVER_FULL = 0x02
        const val ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED = 0x10
        const val ROSTER_MAGIC = 0x4E53524F
        const val ROSTER_SIZE = 208

        private const val SECRET = "nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY"
        private val KEY: ByteArray =
            MessageDigest.getInstance("SHA-256").digest(SECRET.toByteArray(Charsets.US_ASCII))

        /**
         * Connectivity probe: 8-byte ServerInfoProbe -> 16-byte ServerInfoReply.
         * Unauthenticated by design (mirrors the desktop client's probe).
         */
        fun probe(host: String, port: Int, timeoutMs: Int = 700, attempts: Int = 3): Boolean {
            return try {
                DatagramSocket().use { s ->
                    val addr = InetSocketAddress(InetAddress.getByName(host), port)
                    s.soTimeout = timeoutMs
                    val probe = ByteArray(8)
                    writeU32LE(probe, 0, SERVER_INFO_MAGIC)
                    probe[4] = 1 // SERVER_INFO_VERSION
                    val reply = DatagramPacket(ByteArray(64), 64)
                    repeat(attempts) {
                        try {
                            s.send(DatagramPacket(probe, probe.size, addr))
                            s.receive(reply)
                            if (reply.length >= 16 && readU32LE(reply.data, 0) == SERVER_INFO_MAGIC) return true
                        } catch (_: java.net.SocketTimeoutException) {}
                    }
                    false
                }
            } catch (_: Throwable) {
                false
            }
        }

        fun readU32LE(b: ByteArray, off: Int): Int =
            (b[off].toInt() and 0xFF) or
                ((b[off + 1].toInt() and 0xFF) shl 8) or
                ((b[off + 2].toInt() and 0xFF) shl 16) or
                ((b[off + 3].toInt() and 0xFF) shl 24)

        private fun writeU32LE(b: ByteArray, off: Int, v: Int) {
            b[off] = (v and 0xFF).toByte()
            b[off + 1] = ((v ushr 8) and 0xFF).toByte()
            b[off + 2] = ((v ushr 16) and 0xFF).toByte()
            b[off + 3] = ((v ushr 24) and 0xFF).toByte()
        }
    }

    private val socket = DatagramSocket()
    private val mac: Mac = Mac.getInstance("HmacSHA256").apply { init(SecretKeySpec(KEY, "HmacSHA256")) }
    @Volatile var closed = false
        private set

    init {
        socket.connect(InetSocketAddress(InetAddress.getByName(host), port))
        // Best-effort low-latency hints; ignored by stacks that do not honor them.
        try { socket.trafficClass = 0x10 } catch (_: Throwable) {} // IPTOS_LOWDELAY
        try { socket.sendBufferSize = 16 * 1024 } catch (_: Throwable) {}
    }

    /**
     * Signs the first [authLen] bytes of [payload] (which must have
     * authLen + 16 bytes of capacity) and sends authLen + 16 bytes.
     * Synchronized: Mac is stateful and both the 4 ms sender thread and the
     * disconnect path use it.
     */
    @Synchronized
    fun sendSigned(payload: ByteArray, authLen: Int) {
        if (closed) throw IllegalStateException("transport closed")
        val out: ByteArray = if (payload.size >= authLen + HMAC_TAG_SIZE) payload
            else payload.copyOf(authLen + HMAC_TAG_SIZE)
        mac.update(out, 0, authLen)
        val tag = mac.doFinal() // 32 bytes; wire carries the first 16
        System.arraycopy(tag, 0, out, authLen, HMAC_TAG_SIZE)
        socket.send(DatagramPacket(out, authLen + HMAC_TAG_SIZE))
    }

    /** Blocking receive for server feedback (rumble/assignment/roster/...). */
    fun receive(buf: ByteArray): Int {
        return try {
            val pkt = DatagramPacket(buf, buf.size)
            socket.receive(pkt)
            pkt.length
        } catch (_: Throwable) {
            -1 // closed or fatal: receiver loop exits
        }
    }

    fun close() {
        closed = true
        try { socket.close() } catch (_: Throwable) {}
    }
}

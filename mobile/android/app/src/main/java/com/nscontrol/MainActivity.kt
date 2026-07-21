package com.nscontrol

import android.content.pm.ActivityInfo
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.hardware.input.InputManager
import android.os.BatteryManager
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.Surface
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.WindowManager
import android.webkit.JavascriptInterface
import android.webkit.WebResourceRequest
import android.webkit.WebSettings
import android.webkit.WebView
import android.webkit.WebViewClient
import android.widget.Button
import android.widget.EditText
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.app.AppCompatDelegate
import org.json.JSONArray
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.abs
import kotlin.math.roundToInt

class MainActivity : AppCompatActivity() {
    companion object {
        private const val TAG = "NSMobile"
        // Switch controller IMU samples are effectively a high-rate stream.
        // Request 200 Hz so short acceleration gestures are not lost between
        // Android's default 50 Hz GAME callbacks.
        private const val MOTION_SENSOR_PERIOD_US = 5_000
    }
    private lateinit var connectView: View
    private lateinit var webView: WebView
    private lateinit var hostInput: EditText
    private lateinit var statusText: TextView
    private lateinit var connectBtn: Button

    private var host = ""
    private var connected = false
    @Volatile private var controlClientActive = false
    @Volatile private var sending = false
    // Authenticated UDP transport (same path as the desktop ns-client).
    @Volatile private var udp: NsUdp? = null
    private val seq = AtomicInteger(0)
    private val senderToken = AtomicInteger(0)
    private val sendLock = Any()

    private lateinit var sensorManager: SensorManager
    private var accelSensor: Sensor? = null
    private var gravitySensor: Sensor? = null
    private var gyroSensor: Sensor? = null
    @Volatile private var phoneSensorsActive = false
    private val phoneSensorLock = Any()
    private val latestPhoneAccel = FloatArray(3)
    private val latestPhoneGravity = FloatArray(3)
    private val latestPhoneGyro = FloatArray(3)
    private var hasLatestPhoneAccel = false
    private var hasLatestPhoneGravity = false
    private var hasLatestPhoneGyro = false
    private val latestMotionSamples = Array(Protocol.MOTION_SAMPLE_COUNT) { ByteArray(Protocol.MOTION_SAMPLE_SIZE) }
    private var latestMotionSampleCount = 0
    private var phoneMotionRevision = 0L
    private var sentPhoneMotionRevision = -1L

    @Volatile private var touchHid: ByteArray? = null
    @Volatile private var touchFrame: ByteArray? = null
    @Volatile private var lastTouchFrameMs: Long = 0
    // 0 forces the 4 ms sender loop to (re)send the client names on its next
    // tick; the loop then refreshes them every 2 s (UDP is lossy, this heals).
    @Volatile private var lastNamesSentMs: Long = 0
    @Volatile private var touchControllerType: Int = 3
    @Volatile private var physicalControllerType: Int = Protocol.CONTROLLER_TYPE_PRO
    @Volatile private var touchExtraButtons: Int = 0
    @Volatile private var lastBridgeFrameParseMs: Long = 0

    private enum class Page { MAIN_MENU, TOUCH_CONTROLS, EDITOR }
    private enum class ClientMode { NONE, TOUCH, PHYSICAL }

    private class PhysicalPad {
        var deviceId: Int = -1
        var name: String = "Empty"
        var present: Boolean = false
        var buttons: Int = 0
        var dpadUp: Boolean = false
        var dpadDown: Boolean = false
        var dpadLeft: Boolean = false
        var dpadRight: Boolean = false
        var lx: Int = 128
        var ly: Int = 128
        var rx: Int = 128
        var ry: Int = 128
        var hasMotion: Boolean = false
        var hasRumble: Boolean = false
        var hasGyro: Boolean = false
        var rumbleLow: Int = 0
        var rumbleHigh: Int = 0
        var rumbleUntilMs: Long = 0L
        var rumbleLastSetMs: Long = 0L
        val motionSamples: Array<ByteArray> = Array(Protocol.MOTION_SAMPLE_COUNT) { Protocol.neutralMotion() }
        var motionSampleCount: Int = 0
        var motionRevision: Long = 0
        var sentMotionRevision: Long = -1

        fun reset() {
            deviceId = -1
            name = "Empty"
            present = false
            buttons = 0
            dpadUp = false
            dpadDown = false
            dpadLeft = false
            dpadRight = false
            lx = 128; ly = 128; rx = 128; ry = 128
            hasMotion = false
            hasRumble = false
            hasGyro = false
            rumbleLow = 0
            rumbleHigh = 0
            rumbleUntilMs = 0L
            rumbleLastSetMs = 0L
            motionSampleCount = 0
            motionRevision = 0
            sentMotionRevision = -1
            for (i in 0 until Protocol.MOTION_SAMPLE_COUNT) motionSamples[i].fill(0)
        }

        fun hid(): ByteArray {
            val hat = when {
                dpadUp && dpadRight -> Protocol.HAT_NE
                dpadUp && dpadLeft -> Protocol.HAT_NW
                dpadDown && dpadRight -> Protocol.HAT_SE
                dpadDown && dpadLeft -> Protocol.HAT_SW
                dpadUp -> Protocol.HAT_N
                dpadRight -> Protocol.HAT_E
                dpadDown -> Protocol.HAT_S
                dpadLeft -> Protocol.HAT_W
                else -> Protocol.HAT_NEUTRAL
            }
            return Protocol.hid(buttons, hat, lx, ly, rx, ry, present)
        }
    }

    private var currentPage = Page.MAIN_MENU
    private val pageStack = mutableListOf<Page>()
    @Volatile private var activeClientMode = ClientMode.NONE

    private lateinit var inputManager: InputManager
    private val physicalLock = Any()
    private val physicalPads = Array(Protocol.PAD_COUNT) { PhysicalPad() }
    private val physicalGravity = Array(Protocol.PAD_COUNT) { FloatArray(3) }
    private val physicalAccel = Array(Protocol.PAD_COUNT) { FloatArray(3) }
    private val physicalGyro = Array(Protocol.PAD_COUNT) { FloatArray(3) }
    private val physicalHasGravity = BooleanArray(Protocol.PAD_COUNT)
    private val physicalHasAccel = BooleanArray(Protocol.PAD_COUNT)
    private val physicalHasGyro = BooleanArray(Protocol.PAD_COUNT)
    private val physicalSensorManagers = arrayOfNulls<SensorManager>(Protocol.PAD_COUNT)
    private val physicalSensorListeners = arrayOfNulls<SensorEventListener>(Protocol.PAD_COUNT)

    private val inputDeviceListener = object : InputManager.InputDeviceListener {
        override fun onInputDeviceAdded(deviceId: Int) { if (activeClientMode == ClientMode.PHYSICAL) runOnUiThread { scanPhysicalControllers() } }
        override fun onInputDeviceRemoved(deviceId: Int) { if (activeClientMode == ClientMode.PHYSICAL) runOnUiThread { scanPhysicalControllers() } }
        override fun onInputDeviceChanged(deviceId: Int) { if (activeClientMode == ClientMode.PHYSICAL) runOnUiThread { scanPhysicalControllers() } }
    }

    private val phoneSensorListener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            synchronized(phoneSensorLock) {
                when (event.sensor.type) {
                    Sensor.TYPE_GRAVITY -> {
                        for (i in 0 until minOf(3, event.values.size)) latestPhoneGravity[i] = event.values[i]
                        hasLatestPhoneGravity = true
                    }
                    Sensor.TYPE_ACCELEROMETER -> {
                        for (i in 0 until minOf(3, event.values.size)) latestPhoneAccel[i] = event.values[i]
                        hasLatestPhoneAccel = true
                    }
                    Sensor.TYPE_GYROSCOPE -> {
                        for (i in 0 until minOf(3, event.values.size)) latestPhoneGyro[i] = event.values[i]
                        hasLatestPhoneGyro = true
                        pushMotionSampleLocked()
                    }
                }
            }
        }
        override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        AppCompatDelegate.setDefaultNightMode(AppCompatDelegate.MODE_NIGHT_FOLLOW_SYSTEM)
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

        connectView = layoutInflater.inflate(R.layout.connect, null)
        webView = WebView(this)
        hostInput = connectView.findViewById(R.id.hostInput)
        statusText = connectView.findViewById(R.id.statusText)
        connectBtn = connectView.findViewById(R.id.connectBtn)
        connectBtn.setOnClickListener { onConnect() }

        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(true) {
            override fun handleOnBackPressed() {
                when {
                    connected && currentPage != Page.MAIN_MENU -> goBack()
                    connected -> disconnect()
                    else -> {
                        isEnabled = false
                        onBackPressedDispatcher.onBackPressed()
                    }
                }
            }
        })

        setContentView(connectView)
        getPreferences(MODE_PRIVATE).getString("host", "")?.let {
            hostInput.setText(it)
            hostInput.setSelection(it.length)
        }

        inputManager = getSystemService(Context.INPUT_SERVICE) as InputManager
        inputManager.registerInputDeviceListener(inputDeviceListener, null)

        sensorManager = getSystemService(Context.SENSOR_SERVICE) as SensorManager
        gravitySensor = sensorManager.getDefaultSensor(Sensor.TYPE_GRAVITY)
        accelSensor = sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
        gyroSensor = sensorManager.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
    }

    private fun parseHostPort(raw: String): Pair<String, Int> {
        // Strip any legacy scheme (old builds accepted ws:// URLs).
        val text = raw.trim().substringAfter("://").substringBefore('/')
        val colon = text.lastIndexOf(':')
        return if (colon > 0 && !text.startsWith("[")) {
            val port = text.substring(colon + 1).toIntOrNull()
            if (port != null && port in 1..65535) {
                Pair(text.substring(0, colon), port)
            } else {
                Pair(text, NsUdp.DEFAULT_PORT)
            }
        } else {
            Pair(text, NsUdp.DEFAULT_PORT)
        }
    }

    private fun onConnect() {
        host = hostInput.text.toString().trim()
        if (host.isEmpty()) return
        getPreferences(MODE_PRIVATE).edit().putString("host", host).apply()
        statusText.text = "Connecting..."
        connectBtn.isEnabled = false
        Thread {
            val (probeHost, probePort) = parseHostPort(host)
            // UDP ServerInfo probe (the backend answers it even with no client
            // slot free); replaces the old TCP probe against the web port.
            val reachable = NsUdp.probe(probeHost, probePort)
            runOnUiThread {
                connectBtn.isEnabled = true
                if (!reachable) {
                    statusText.text = "Server not reachable"
                    return@runOnUiThread
                }
                setupWebView()
                connected = true
                currentPage = Page.MAIN_MENU
                pageStack.clear()
                setContentView(webView)
                loadUrl(pageUrl(Page.MAIN_MENU))
                statusText.text = "Loaded"
            }
        }.start()
    }

    // Authenticated UDP to the Raspberry Pi backend — the same low-latency path
    // as the desktop ns-client. Either physical controllers or Touch Controls
    // owns the only live session. UDP is connectionless: the server accepts on
    // the first signed input frame, so sending starts immediately.
    //
    // Socket creation involves DNS resolution and must stay off the UI thread
    // (unlike OkHttp, plain sockets throw NetworkOnMainThreadException).
    private fun connectUdp(): Boolean {
        val (h, p) = parseHostPort(host)
        if (h.isEmpty()) {
            statusText.text = "Invalid server address"
            return false
        }
        val attemptToken = senderToken.get()
        Thread {
            val transport = try { NsUdp(h, p) } catch (_: Throwable) { null }
            runOnUiThread {
                if (!controlClientActive || senderToken.get() != attemptToken || udp != null) {
                    transport?.close()
                    return@runOnUiThread
                }
                if (transport == null) {
                    statusText.text = "Invalid server address"
                    controlClientActive = false
                    activeClientMode = ClientMode.NONE
                    stopPhysicalControllerSensors()
                    updatePhysicalStatusOnPage("Not connected")
                    return@runOnUiThread
                }
                udp = transport
                lastNamesSentMs = 0 // sender loop pushes names right away, then every 2 s
                startReceiver(transport)
                statusText.text = "Connected"
                if (activeClientMode == ClientMode.PHYSICAL) updatePhysicalStatusOnPage("Connected")
                startSending()
            }
        }.apply { name = "ns-udp-connect" }.start()
        return true
    }

    // Server -> mobile feedback arrives on the same UDP socket: rumble (NSVR,
    // with an NSVH precision fallback whose low/high/duration bytes match) and
    // ClientAssignmentPacket (server full / S2 profile refusals, which the WS
    // path used to deliver through the close reason).
    private fun startReceiver(transport: NsUdp) {
        Thread {
            val buf = ByteArray(1024)
            while (udp === transport && !transport.closed) {
                val n = transport.receive(buf)
                if (n < 0) break
                if (n < 8) continue
                val magic = NsUdp.readU32LE(buf, 0)
                when {
                    (n == Protocol.RUMBLE_PACKET_SIZE && magic == Protocol.RUMBLE_MAGIC) ||
                    (n == Protocol.PRECISION_RUMBLE_PACKET_SIZE && magic == Protocol.PRECISION_RUMBLE_MAGIC) -> {
                        val subpad = buf[4].toInt() and 0xFF
                        val low = buf[5].toInt() and 0xFF
                        val high = buf[6].toInt() and 0xFF
                        val duration10Ms = buf[7].toInt() and 0xFF
                        // Keep rumble/haptics off the UI thread.
                        routeRumble(subpad, low, high, duration10Ms)
                    }
                    n == NsUdp.CLIENT_ASSIGNMENT_SIZE && magic == NsUdp.CLIENT_ASSIGNMENT_MAGIC -> {
                        val flags = buf[5].toInt() and 0xFF
                        val message = when {
                            flags and NsUdp.ASSIGNMENT_FLAG_PROFILE_UNSUPPORTED != 0 ->
                                "Switch 2 mode does not support Joy-Con L + R"
                            flags and NsUdp.ASSIGNMENT_FLAG_SERVER_FULL != 0 -> "Server full"
                            else -> null
                        }
                        if (message != null) runOnUiThread { handleTransportClosed(transport, message) }
                    }
                    else -> Log.d(TAG, "ignored udp feedback magic=0x${magic.toString(16)} size=$n")
                }
            }
        }.apply { name = "ns-udp-recv"; isDaemon = true }.start()
    }

    private fun handleTransportClosed(transport: NsUdp, text: String) {
        if (udp !== transport) return
        statusText.text = text
        senderToken.incrementAndGet()
        sending = false
        controlClientActive = false
        touchHid = null
        touchFrame = null
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        udp = null
        transport.close()
        stopPhoneSensors()
        stopPhysicalControllerSensors()
        stopAllPhysicalRumble()
        activeClientMode = ClientMode.NONE
        updatePhysicalStatusOnPage(text)
        try { Toast.makeText(this, text, Toast.LENGTH_SHORT).show() } catch (_: Throwable) {}
        val escaped = jsEscape(text)
        try { webView.evaluateJavascript("""(function(){
            if (window.__nsTouchDisconnected) window.__nsTouchDisconnected('$escaped');
            if (window.__nsMainDisconnected) window.__nsMainDisconnected('$escaped');
            window._connectionFailed = '$escaped' === 'Connection failed';
            var s=document.getElementById('statusText');
            if(s)s.innerText='$escaped';
            var btn=document.getElementById('btnConnect');
            if(btn){btn.innerText='Connect';btn.classList.remove('connected');btn.style.display='block';}
            var dot=document.getElementById('statusDot');
            if(dot)dot.style.display='none';
        })()""", null) } catch (_: Throwable) {}
    }

    private fun startSending() {
        if (sending) return
        val token = senderToken.incrementAndGet()
        sending = true
        Thread {
            try {
                // Input frames are the latency-critical path: bump the sender
                // above default background priority so 4 ms ticks stay on time.
                try { android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_URGENT_DISPLAY) } catch (_: Throwable) {}
                if (activeClientMode == ClientMode.TOUCH) startPhoneSensors()
                while (sending && controlClientActive && senderToken.get() == token) {
                    sendFrame()
                    val now = SystemClock.uptimeMillis()
                    if (now - lastNamesSentMs >= 2000L) {
                        lastNamesSentMs = now
                        sendNamesFrame()
                    }
                    Thread.sleep(4)
                }
            } catch (_: Throwable) {
                runOnUiThread {
                    if (senderToken.get() != token) return@runOnUiThread
                    statusText.text = "Input sender failed"
                    deactivateControlClient()
                }
            } finally {
                if (senderToken.get() == token) {
                    sending = false
                    stopPhoneSensors()
                    if (activeClientMode != ClientMode.PHYSICAL) stopPhysicalControllerSensors()
                }
            }
        }.start()
    }

    private fun phoneBatteryStatus(): Pair<Int, Boolean>? {
        val intent = registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED)) ?: return null
        val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
        val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
        if (level < 0 || scale <= 0) return null
        val percent = ((level * 100f) / scale).roundToInt().coerceIn(0, 100)
        val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
        val charging = status == BatteryManager.BATTERY_STATUS_CHARGING || status == BatteryManager.BATTERY_STATUS_FULL
        return percent to charging
    }

    private fun sendFrame() {
        sendFrameInternal()
    }

    // FLAG_DISCONNECT frees the server slot immediately (the UDP equivalent of
    // the old WebSocket close); sent a few times since UDP is lossy.
    private fun sendDisconnectFrameTo(transport: NsUdp) {
        try { sendFrameInternal(socketOverride = transport, flagsOverride = Protocol.FLAG_DISCONNECT, forceNeutral = true) } catch (_: Throwable) {}
    }

    private fun sendNamesFrame() {
        val transport = udp ?: return
        val data = ByteArray(224)
        data[0] = 0x4E.toByte()
        data[1] = 0x43.toByte()
        data[2] = 0x53.toByte()
        data[3] = 0x4E.toByte()
        data[4] = 1.toByte()

        val touchActive = controlClientActive && activeClientMode == ClientMode.TOUCH && currentPage == Page.TOUCH_CONTROLS
        if (touchActive) {
            val off = 8
            data[off] = 1.toByte()
            data[off + 1] = (if (phoneSensorsActive) 1 else 0).toByte()
            val name = "Android Controller"
            val nameBytes = name.toByteArray(Charsets.UTF_8).take(47)
            for (k in nameBytes.indices) {
                data[off + 2 + k] = nameBytes[k]
            }
        } else {
            synchronized(physicalLock) {
                for (i in 0 until Protocol.PAD_COUNT) {
                    val pad = physicalPads[i]
                    val off = 8 + i * 50
                    data[off] = (if (pad.present) 1 else 0).toByte()
                    data[off + 1] = (if (pad.hasGyro) 1 else 0).toByte()
                    if (pad.present) {
                        val nameBytes = pad.name.toByteArray(Charsets.UTF_8).take(47)
                        for (k in nameBytes.indices) {
                            data[off + 2 + k] = nameBytes[k]
                        }
                    }
                }
            }
        }
        try { transport.sendSigned(data, NsUdp.NAMES_AUTH_SIZE) } catch (_: Throwable) {}
    }

    private fun sendFrameInternal(socketOverride: NsUdp? = null, flagsOverride: Int? = null, forceNeutral: Boolean = false) {
        synchronized(sendLock) {
            val socket = socketOverride ?: udp ?: return
            val touchActive = !forceNeutral && touchClientActive()
            val flags = flagsOverride ?: if (touchActive) Protocol.FLAG_SINGLE_PAD else 0
            val timestampUs = System.currentTimeMillis() * 1000L
            val frame = Protocol.initFrame(flags, seq.getAndIncrement(), timestampUs)

            when {
                activeClientMode == ClientMode.TOUCH && touchActive -> {
                    val now = SystemClock.uptimeMillis()
                    val hid = if (now - lastTouchFrameMs <= 500L) {
                        touchHid ?: touchFrame?.let { Protocol.extractPad0HidFromWebFrame(it) } ?: Protocol.neutralHid()
                    } else {
                        Protocol.neutralHid()
                    }
                    Protocol.setFrameHid(frame, 0, hid)
                    frame[20 + 7] = (frame[20 + 7].toInt() or touchExtraButtons).toByte()
                    Protocol.setFrameControllerType(frame, 0, touchControllerType)
                    phoneMotionSamples()?.let { batch ->
                        Protocol.setFrameMotionSamples(frame, 0, batch.samples)
                        Protocol.setFrameMotionFresh(frame, 0, batch.fresh)
                    }
                    phoneBatteryStatus()?.let { (percent, charging) -> Protocol.setFrameBatteryPercent(frame, 0, percent, charging) }
                }
                activeClientMode == ClientMode.PHYSICAL && !forceNeutral -> {
                    synchronized(physicalLock) {
                        for (i in 0 until Protocol.PAD_COUNT) {
                            val pad = physicalPads[i]
                            if (!pad.present) continue
                            Protocol.setFrameHid(frame, i, pad.hid())
                            Protocol.setFrameControllerType(frame, i, physicalControllerType)
                            if (pad.hasMotion && pad.motionSampleCount >= Protocol.MOTION_SAMPLE_COUNT) {
                                Protocol.setFrameMotionSamples(frame, i, Array(Protocol.MOTION_SAMPLE_COUNT) { j -> pad.motionSamples[j].copyOf() })
                                Protocol.setFrameMotionFresh(frame, i, pad.motionRevision != pad.sentMotionRevision)
                                pad.sentMotionRevision = pad.motionRevision
                            }
                        }
                    }
                }
            }

            socket.sendSigned(frame, NsUdp.FRAME_AUTH_SIZE)
        }
    }

    private fun pushMotionSampleLocked() {
        if (!hasLatestPhoneGyro || (!hasLatestPhoneAccel && !hasLatestPhoneGravity)) return

        // A Switch IMU report needs total acceleration: gravity plus the user's
        // translational movement. TYPE_GRAVITY is orientation-only and removes
        // gestures such as the upward tennis serve toss. Prefer the hardware
        // accelerometer, which already includes gravity; retain gravity only as
        // a last-resort fallback for unusual devices without accelerometer data.
        val accel = if (hasLatestPhoneAccel) latestPhoneAccel else latestPhoneGravity

        // Pro Controller touch motion follows the forced landscape display.
        // A single Joy-Con L or R is physically held vertically, with the phone's
        // top matching the Joy-Con's top, so its motion must stay in the phone's
        // natural axes instead of being rotated into the landscape UI frame.
        val a = remapPhoneSensorForController(accel)
        val g = remapPhoneSensorForController(latestPhoneGyro)

        val sample = NativeProtocol.nativePhoneMotion(
            a[0], a[1], a[2],
            g[0], g[1], g[2]
        )

        latestMotionSamples[0] = latestMotionSamples[1]
        latestMotionSamples[1] = latestMotionSamples[2]
        latestMotionSamples[2] = sample
        if (latestMotionSampleCount < Protocol.MOTION_SAMPLE_COUNT) latestMotionSampleCount++
        phoneMotionRevision++
    }

    private data class MotionBatch(val samples: Array<ByteArray>, val fresh: Boolean)

    private fun phoneMotionSamples(): MotionBatch? {
        synchronized(phoneSensorLock) {
            if (latestMotionSampleCount < Protocol.MOTION_SAMPLE_COUNT) return null
            val fresh = phoneMotionRevision != sentPhoneMotionRevision
            sentPhoneMotionRevision = phoneMotionRevision
            return MotionBatch(Array(Protocol.MOTION_SAMPLE_COUNT) { i -> latestMotionSamples[i].copyOf() }, fresh)
        }
    }

    private fun clampMotionShort(v: Float): Short = v.roundToInt().coerceIn(-32768, 32767).toShort()
    private fun gyroDeadzoneShort(v: Short): Short = if (kotlin.math.abs(v.toInt()) <= 32) 0 else v

    private fun remapSensorForDisplay(v: FloatArray): FloatArray {
        val rotation = try {
            if (Build.VERSION.SDK_INT >= 30) display?.rotation ?: Surface.ROTATION_0 else legacyDisplayRotation()
        } catch (_: Throwable) { Surface.ROTATION_0 }
        return when (rotation) {
            Surface.ROTATION_90  -> floatArrayOf(-v[1],  v[0], v[2])
            Surface.ROTATION_180 -> floatArrayOf(-v[0], -v[1], v[2])
            Surface.ROTATION_270 -> floatArrayOf( v[1], -v[0], v[2])
            else -> floatArrayOf(v[0], v[1], v[2])
        }
    }

    private fun remapPhoneSensorForController(v: FloatArray): FloatArray =
        when (touchControllerType) {
            Protocol.CONTROLLER_TYPE_JOYCON_L,
            Protocol.CONTROLLER_TYPE_JOYCON_R -> {
                // The app is landscape, but the phone itself is held portrait like a
                // vertical Joy-Con, directly underneath it with both tops aligned.
                // Keep Android's natural device axes: +X points right, +Y points
                // toward the phone/Joy-Con top, and +Z points out of the screen.
                // ns_motion_from_android then maps that physical pose to the common
                // Switch frame (X = forward, Y = left, Z = up).
                floatArrayOf(v[0], v[1], v[2])
            }
            else -> remapSensorForDisplay(v)
        }

    @Suppress("DEPRECATION")
    private fun legacyDisplayRotation(): Int = windowManager.defaultDisplay.rotation

    private fun startPhoneSensors() {
        if (phoneSensorsActive) return
        synchronized(phoneSensorLock) {
            hasLatestPhoneAccel = false
            hasLatestPhoneGravity = false
            hasLatestPhoneGyro = false
            latestPhoneAccel.fill(0.0f)
            latestPhoneGravity.fill(0.0f)
            latestPhoneGyro.fill(0.0f)
            latestMotionSampleCount = 0
            for (i in 0 until Protocol.MOTION_SAMPLE_COUNT) latestMotionSamples[i].fill(0)
        }
        val accelOpened = accelSensor?.let {
            sensorManager.registerListener(phoneSensorListener, it, MOTION_SENSOR_PERIOD_US)
        } ?: false
        var opened = accelOpened
        if (!accelOpened) {
            gravitySensor?.let {
                opened = sensorManager.registerListener(phoneSensorListener, it, MOTION_SENSOR_PERIOD_US) || opened
            }
        }
        gyroSensor?.let {
            opened = sensorManager.registerListener(phoneSensorListener, it, MOTION_SENSOR_PERIOD_US) || opened
        }
        phoneSensorsActive = opened
    }

    private fun stopPhoneSensors() {
        if (!phoneSensorsActive) return
        try { sensorManager.unregisterListener(phoneSensorListener) } catch (_: Throwable) {}
        phoneSensorsActive = false
        synchronized(phoneSensorLock) {
            hasLatestPhoneAccel = false
            hasLatestPhoneGravity = false
            hasLatestPhoneGyro = false
            latestMotionSampleCount = 0
        }
    }

    private fun routeRumble(subpad: Int, low: Int, high: Int, duration10Ms: Int) {
        if (!controlClientActive) return
        when (activeClientMode) {
            ClientMode.PHYSICAL -> physicalRumble(subpad, low, high, duration10Ms)
            ClientMode.TOUCH -> Unit
            ClientMode.NONE -> Unit
        }
    }

    private fun touchClientActive(): Boolean = controlClientActive && activeClientMode == ClientMode.TOUCH && currentPage == Page.TOUCH_CONTROLS

    private fun setupWebView() {
        val baseUserAgent = webView.settings.userAgentString ?: ""
        webView.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            allowFileAccess = true
            allowContentAccess = true
            mixedContentMode = WebSettings.MIXED_CONTENT_ALWAYS_ALLOW
            userAgentString = "$baseUserAgent NS-Mobile/1.0"
        }
        webView.addJavascriptInterface(JSBridge(), "NSBridge")
        webView.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(v: WebView, req: WebResourceRequest): Boolean {
                val url = req.url.toString()
                return when {
                    url.endsWith("/mobile") || url.endsWith("/mobile.html") -> { navTo(Page.TOUCH_CONTROLS); true }
                    url.endsWith("/editor") || url.endsWith("/editor.html") -> { navTo(Page.EDITOR); true }
                    else -> false
                }
            }

            override fun onPageFinished(v: WebView, url: String) {
                if (currentPage == Page.MAIN_MENU) {
                    v.evaluateJavascript("""
                        (function(){
                          var kb = document.getElementById('kbModeContainer');
                          if (kb) kb.style.display = 'none';
                          var bindings = document.getElementById('btnBindings');
                          if (bindings) bindings.style.display = 'none';
                          var macros = document.getElementById('btnMacros');
                          if (macros) macros.style.display = 'none';
                          var oldStart = document.getElementById('btn' + 'HubStart');
                          if (oldStart) oldStart.remove();
                          var oldStop = document.getElementById('btnHubStop');
                          if (oldStop) oldStop.remove();
                          var oldRefresh = document.getElementById('btnHubRefresh');
                          if (oldRefresh) oldRefresh.remove();
                          var connect = document.getElementById('btnConnect');
                          function publishPhysicalControllerType(){
                            var selected = parseInt(localStorage.getItem('nswc_controller_type') || '3');
                            if (![1,2,3].includes(selected)) selected = 3;
                            if (window.NSBridge && NSBridge.onPhysicalControllerType) NSBridge.onPhysicalControllerType(selected);
                          }
                          publishPhysicalControllerType();
                          if (connect) {
                            connect.style.display = 'inline-block';
                            connect.textContent = 'Connect';
                            connect.onclick = function(ev){
                              if (ev) ev.preventDefault();
                              publishPhysicalControllerType();
                              if (window.NSBridge && NSBridge.onPhysicalStart) NSBridge.onPhysicalStart();
                              return false;
                            };
                          }
                          var touch = document.getElementById('btnTouchControls');
                          if (touch) {
                            touch.style.display = 'inline-block';
                            touch.onclick = function(ev){
                              if (ev) ev.preventDefault();
                              if (window.NSBridge && NSBridge.onOpenTouch) NSBridge.onOpenTouch();
                              else window.location.href='mobile.html';
                              return false;
                            };
                          }
                          var editor = document.getElementById('btnEditor');
                          if (editor) {
                            editor.style.display = 'inline-block';
                            editor.onclick = function(ev){
                              if (ev) ev.preventDefault();
                              if (window.NSBridge && NSBridge.onOpenEditor) NSBridge.onOpenEditor();
                              else window.location.href='editor.html';
                              return false;
                            };
                          }
                          if (window.NSBridge && NSBridge.onPhysicalRefresh) NSBridge.onPhysicalRefresh();
                        })();
                    """.trimIndent(), null)
                }
            }
        }
    }

    private fun loadUrl(url: String) { webView.loadUrl(url) }

    private fun pageUrl(page: Page): String = when (page) {
        Page.MAIN_MENU -> "file:///android_asset/ns_mobile/index.html"
        Page.TOUCH_CONTROLS -> "file:///android_asset/ns_mobile/mobile.html"
        Page.EDITOR -> "file:///android_asset/ns_mobile/editor.html"
    }

    private fun navTo(page: Page) {
        if (page == Page.TOUCH_CONTROLS || page == Page.EDITOR) {
            deactivateControlClient()
            stopPhysicalControllerSensors()
        }
        pageStack.add(currentPage)
        enterPage(page)
    }

    private fun enterPage(page: Page) {
        currentPage = page
        if (page == Page.TOUCH_CONTROLS || page == Page.EDITOR) {
            deactivateControlClient()
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
            if (Build.VERSION.SDK_INT >= 30) {
                window.insetsController?.apply {
                    hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                    systemBarsBehavior = WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                }
            } else {
                @Suppress("DEPRECATION")
                window.decorView.systemUiVisibility = (
                    View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    or View.SYSTEM_UI_FLAG_FULLSCREEN
                    or View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                    or View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                    or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                )
            }
        } else {
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
            if (Build.VERSION.SDK_INT >= 30) {
                window.insetsController?.show(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
            } else {
                @Suppress("DEPRECATION")
                window.decorView.systemUiVisibility = View.SYSTEM_UI_FLAG_VISIBLE
            }
        }
        loadUrl(pageUrl(page))
    }

    private fun goBack() {
        if (pageStack.isEmpty()) return
        if (currentPage == Page.TOUCH_CONTROLS || currentPage == Page.EDITOR) {
            deactivateControlClient()
            stopPhysicalControllerSensors()
        }
        enterPage(pageStack.removeAt(pageStack.lastIndex))
    }

    inner class JSBridge {
        @JavascriptInterface
        fun onOpen() {
            runOnUiThread {
                if (currentPage == Page.TOUCH_CONTROLS) activateControlClient()
            }
        }

        @JavascriptInterface
        fun onBinary(json: String) {
            if (currentPage != Page.TOUCH_CONTROLS || !controlClientActive) return
            try {
                val now = SystemClock.uptimeMillis()
                if (now - lastBridgeFrameParseMs < 8L) return
                lastBridgeFrameParseMs = now
                val arr = JSONArray(json)
                if (arr.length() < 20 + Protocol.HID_SIZE) return
                val frame = ByteArray(arr.length()) { i -> arr.getInt(i).toByte() }
                touchFrame = frame
                touchHid = Protocol.extractPad0HidFromWebFrame(frame)
                lastTouchFrameMs = now
            } catch (_: Throwable) {}
        }

        // Keep the 6-arg signature the webapp has always called: the WebView JS
        // bridge matches methods by argument count, so changing the arity breaks
        // touch input whenever the APK and the served webapp are out of sync.
        // The controller type travels through its own optional method instead.
        @JavascriptInterface
        fun onTouchState(buttons: Int, hat: Int, lx: Int, ly: Int, rx: Int, ry: Int) {
            if (currentPage != Page.TOUCH_CONTROLS || !controlClientActive) return
            touchHid = Protocol.hid(
                NativeProtocol.nativeNormalizeShortcuts(buttons),
                hat.coerceIn(0, 8),
                lx.coerceIn(0, 255),
                ly.coerceIn(0, 255),
                rx.coerceIn(0, 255),
                ry.coerceIn(0, 255),
                present = true
            )
            lastTouchFrameMs = SystemClock.uptimeMillis()
        }

        // Arity overload for webapps that still pass the controller type inline.
        @JavascriptInterface
        fun onTouchState(buttons: Int, hat: Int, lx: Int, ly: Int, rx: Int, ry: Int, controllerType: Int) {
            onTouchControllerType(controllerType)
            onTouchState(buttons, hat, lx, ly, rx, ry)
        }

        @JavascriptInterface
        fun onTouchControllerType(controllerType: Int) {
            // 0/unknown means "default": treat as Pro Controller, never Joy-Con.
            touchControllerType = if (controllerType in 1..3) controllerType else 3
        }

        @JavascriptInterface
        fun onPhysicalControllerType(controllerType: Int) {
            if (controlClientActive && activeClientMode == ClientMode.PHYSICAL) return
            physicalControllerType = if (controllerType in 1..3) controllerType else Protocol.CONTROLLER_TYPE_PRO
        }

        @JavascriptInterface
        fun onTouchExtraButtons(extraButtons: Int) {
            touchExtraButtons = extraButtons and (0x10 or 0x20)
        }

        @JavascriptInterface
        fun onClose() { runOnUiThread { deactivateControlClient() } }

        @JavascriptInterface
        fun onPhysicalStart() { runOnUiThread { togglePhysicalControllers() } }

        @JavascriptInterface
        fun onPhysicalStop() { runOnUiThread { deactivateControlClient(); updatePhysicalStatusOnPage("Not connected") } }

        @JavascriptInterface
        fun onPhysicalRefresh() { runOnUiThread { scanPhysicalControllers(); updatePhysicalStatusOnPage() } }

        @JavascriptInterface
        fun onOpenTouch() { runOnUiThread { navTo(Page.TOUCH_CONTROLS) } }

        @JavascriptInterface
        fun onOpenEditor() { runOnUiThread { navTo(Page.EDITOR) } }

        @JavascriptInterface
        fun onBack() { runOnUiThread { goBack() } }
    }

    private fun togglePhysicalControllers() {
        if (controlClientActive && activeClientMode == ClientMode.PHYSICAL) {
            deactivateControlClient()
            updatePhysicalStatusOnPage("Not connected")
        } else {
            activatePhysicalControllers()
        }
    }

    private fun activatePhysicalControllers() {
        if (controlClientActive && activeClientMode == ClientMode.PHYSICAL) {
            scanPhysicalControllers()
            updatePhysicalStatusOnPage("Connected")
            return
        }

        deactivateControlClient()

        currentPage = Page.MAIN_MENU
        activeClientMode = ClientMode.PHYSICAL
        controlClientActive = true

        touchHid = null
        touchFrame = null
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0

        scanPhysicalControllers()
        updatePhysicalStatusOnPage("Connecting...")

        if (!connectUdp()) {
            controlClientActive = false
            activeClientMode = ClientMode.NONE
            stopPhysicalControllerSensors()
            updatePhysicalStatusOnPage("Not connected")
        }
    }

    private fun scanPhysicalControllers() {
        if (!::inputManager.isInitialized) return
        synchronized(physicalLock) {
            val oldByDevice = physicalPads.filter { it.present }.associateBy { it.deviceId }
            stopPhysicalControllerSensorsLocked(clearPads = false)
            for (pad in physicalPads) pad.reset()

            val devices = mutableListOf<InputDevice>()
            for (id in InputDevice.getDeviceIds()) {
                val device = InputDevice.getDevice(id) ?: continue
                if (isControllerDevice(device)) devices.add(device)
                if (devices.size >= Protocol.PAD_COUNT) break
            }

            for ((slot, device) in devices.withIndex()) {
                val prev = oldByDevice[device.id]
                val pad = physicalPads[slot]
                pad.deviceId = device.id
                pad.name = device.name ?: "Controller ${slot + 1}"
                pad.present = true
                pad.hasRumble = deviceHasVibrator(device)
                if (prev != null) {
                    pad.buttons = prev.buttons; pad.dpadUp = prev.dpadUp; pad.dpadDown = prev.dpadDown
                    pad.dpadLeft = prev.dpadLeft; pad.dpadRight = prev.dpadRight
                    pad.lx = prev.lx; pad.ly = prev.ly; pad.rx = prev.rx; pad.ry = prev.ry
                }
                startPhysicalControllerSensorsLocked(slot, device)
            }
        }
        updatePhysicalStatusOnPage()
        lastNamesSentMs = 0 // roster changed: sender loop re-announces names next tick
    }

    private fun isControllerDevice(device: InputDevice): Boolean {
        val sources = device.sources
        val gamepad = (sources and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
        val joystick = (sources and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        return gamepad || joystick
    }

    private fun slotForDeviceIdLocked(deviceId: Int): Int = physicalPads.indexOfFirst { it.present && it.deviceId == deviceId }

    private fun hatFromDpad(up: Boolean, down: Boolean, left: Boolean, right: Boolean): Int = when {
        up && right -> Protocol.HAT_NE
        up && left -> Protocol.HAT_NW
        down && right -> Protocol.HAT_SE
        down && left -> Protocol.HAT_SW
        up -> Protocol.HAT_N
        right -> Protocol.HAT_E
        down -> Protocol.HAT_S
        left -> Protocol.HAT_W
        else -> Protocol.HAT_NEUTRAL
    }

    private fun axisToByte(value: Float): Int {
        val v = value.coerceIn(-1.0f, 1.0f)
        return ((v + 1.0f) * 127.5f).roundToInt().coerceIn(0, 255)
    }

    private fun motionRange(device: InputDevice?, source: Int, axis: Int): InputDevice.MotionRange? =
        device?.getMotionRange(axis, source) ?: device?.getMotionRange(axis)

    private fun axisPresent(device: InputDevice?, source: Int, axis: Int): Boolean =
        motionRange(device, source, axis) != null

    private fun centeredAxis(event: MotionEvent, device: InputDevice?, axis: Int): Float {
        val value = event.getAxisValue(axis)
        val flat = motionRange(device, event.source, axis)?.flat ?: 0.05f
        return if (abs(value) <= flat) 0.0f else value.coerceIn(-1.0f, 1.0f)
    }

    private fun centeredAxisAny(event: MotionEvent, device: InputDevice?, vararg axes: Int): Float {
        for (axis in axes) {
            if (axisPresent(device, event.source, axis)) return centeredAxis(event, device, axis)
        }
        return 0.0f
    }

    private fun buttonBitForKeyCode(code: Int): Int = when (code) {
        KeyEvent.KEYCODE_BUTTON_A -> Protocol.BTN_B
        KeyEvent.KEYCODE_BUTTON_B -> Protocol.BTN_A
        KeyEvent.KEYCODE_BUTTON_X -> Protocol.BTN_Y
        KeyEvent.KEYCODE_BUTTON_Y -> Protocol.BTN_X
        KeyEvent.KEYCODE_BUTTON_L1 -> Protocol.BTN_L
        KeyEvent.KEYCODE_BUTTON_R1 -> Protocol.BTN_R
        KeyEvent.KEYCODE_BUTTON_L2 -> Protocol.BTN_ZL
        KeyEvent.KEYCODE_BUTTON_R2 -> Protocol.BTN_ZR
        KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BACK -> Protocol.BTN_MINUS
        KeyEvent.KEYCODE_BUTTON_START -> Protocol.BTN_PLUS
        KeyEvent.KEYCODE_BUTTON_THUMBL -> Protocol.BTN_LSTICK
        KeyEvent.KEYCODE_BUTTON_THUMBR -> Protocol.BTN_RSTICK
        KeyEvent.KEYCODE_BUTTON_MODE -> Protocol.BTN_HOME
        else -> 0
    }

    private fun handleControllerKey(event: KeyEvent): Boolean {
        if (activeClientMode != ClientMode.PHYSICAL || !controlClientActive) return false
        val actionDown = event.action == KeyEvent.ACTION_DOWN
        if (!actionDown && event.action != KeyEvent.ACTION_UP) return false
        var handled = false
        synchronized(physicalLock) {
            val slot = slotForDeviceIdLocked(event.deviceId)
            if (slot < 0) return false
            val pad = physicalPads[slot]
            when (event.keyCode) {
                KeyEvent.KEYCODE_DPAD_UP -> { pad.dpadUp = actionDown; handled = true }
                KeyEvent.KEYCODE_DPAD_DOWN -> { pad.dpadDown = actionDown; handled = true }
                KeyEvent.KEYCODE_DPAD_LEFT -> { pad.dpadLeft = actionDown; handled = true }
                KeyEvent.KEYCODE_DPAD_RIGHT -> { pad.dpadRight = actionDown; handled = true }
                else -> {
                    val bit = buttonBitForKeyCode(event.keyCode)
                    if (bit != 0) {
                        pad.buttons = if (actionDown) pad.buttons or bit else pad.buttons and bit.inv()
                        handled = true
                    }
                }
            }
        }
        return handled
    }

    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        if (handleControllerKey(event)) return true
        return super.dispatchKeyEvent(event)
    }

    override fun dispatchGenericMotionEvent(event: MotionEvent): Boolean {
        if (handleControllerMotion(event)) return true
        return super.dispatchGenericMotionEvent(event)
    }

    override fun onGenericMotionEvent(event: MotionEvent): Boolean {
        if (handleControllerMotion(event)) return true
        return super.onGenericMotionEvent(event)
    }

    private fun handleControllerMotion(event: MotionEvent): Boolean {
        if (activeClientMode != ClientMode.PHYSICAL || !controlClientActive || event.action != MotionEvent.ACTION_MOVE) return false
        val isJoy = (event.source and InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
        val isGamepad = (event.source and InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
        if (!isJoy && !isGamepad) return false

        synchronized(physicalLock) {
            val slot = slotForDeviceIdLocked(event.deviceId)
            if (slot < 0) return false
            val device = event.device
            val pad = physicalPads[slot]

            // Android controller axis layouts vary a lot. Switch Pro over BT often
            // exposes the right stick as RX/RY, while Xbox-style mappings often use Z/RZ.
            val primaryX = centeredAxisAny(event, device, MotionEvent.AXIS_X)
            val primaryY = centeredAxisAny(event, device, MotionEvent.AXIS_Y)
            val secondaryX = centeredAxisAny(event, device, MotionEvent.AXIS_Z, MotionEvent.AXIS_RX)
            val secondaryY = centeredAxisAny(event, device, MotionEvent.AXIS_RZ, MotionEvent.AXIS_RY)
            val hasPrimary = axisPresent(device, event.source, MotionEvent.AXIS_X) ||
                axisPresent(device, event.source, MotionEvent.AXIS_Y)
            when (physicalControllerType) {
                Protocol.CONTROLLER_TYPE_JOYCON_L -> {
                    pad.lx = axisToByte(if (hasPrimary) primaryX else secondaryX)
                    pad.ly = axisToByte(if (hasPrimary) primaryY else secondaryY)
                    pad.rx = 128
                    pad.ry = 128
                }
                Protocol.CONTROLLER_TYPE_JOYCON_R -> {
                    // A standalone right Joy-Con usually exposes its only stick as X/Y,
                    // even though it is the console's right stick. Some Android drivers
                    // retain a right-stick Z/R* layout, so use that when X/Y are absent.
                    pad.lx = 128
                    pad.ly = 128
                    pad.rx = axisToByte(if (hasPrimary) primaryX else secondaryX)
                    pad.ry = axisToByte(if (hasPrimary) primaryY else secondaryY)
                }
                else -> {
                    pad.lx = axisToByte(primaryX)
                    pad.ly = axisToByte(primaryY)
                    pad.rx = axisToByte(secondaryX)
                    pad.ry = axisToByte(secondaryY)
                }
            }

            val l2 = maxOf(
                centeredAxisAny(event, device, MotionEvent.AXIS_LTRIGGER),
                centeredAxisAny(event, device, MotionEvent.AXIS_BRAKE)
            )
            val r2 = maxOf(
                centeredAxisAny(event, device, MotionEvent.AXIS_RTRIGGER),
                centeredAxisAny(event, device, MotionEvent.AXIS_GAS)
            )
            if (l2 > 0.5f) pad.buttons = pad.buttons or Protocol.BTN_ZL else pad.buttons = pad.buttons and Protocol.BTN_ZL.inv()
            if (r2 > 0.5f) pad.buttons = pad.buttons or Protocol.BTN_ZR else pad.buttons = pad.buttons and Protocol.BTN_ZR.inv()

            val hx = centeredAxisAny(event, device, MotionEvent.AXIS_HAT_X)
            val hy = centeredAxisAny(event, device, MotionEvent.AXIS_HAT_Y)
            pad.dpadLeft = hx < -0.5f; pad.dpadRight = hx > 0.5f
            pad.dpadUp = hy < -0.5f; pad.dpadDown = hy > 0.5f
            return true
        }
    }

    private fun deviceHasVibrator(device: InputDevice): Boolean = try {
        if (Build.VERSION.SDK_INT >= 31) device.vibratorManager.vibratorIds.isNotEmpty() else {
            @Suppress("DEPRECATION")
            device.vibrator.hasVibrator()
        }
    } catch (_: Throwable) { false }

    private fun physicalRumble(subpad: Int, low: Int, high: Int, duration10Ms: Int) {
        if (subpad !in 0 until Protocol.PAD_COUNT) return

        val now = SystemClock.uptimeMillis()
        val neutral = (low == 0 && high == 0) || duration10Ms == 0
        val durationMs = if (neutral) 0L else maxOf(40L, duration10Ms.coerceIn(1, 255) * 10L)
        val strength = maxOf(low, high).coerceIn(1, 255)

        val deviceId = synchronized(physicalLock) {
            val pad = physicalPads[subpad]
            if (!pad.present || pad.deviceId < 0) return

            if (neutral) {
                pad.rumbleLow = 0
                pad.rumbleHigh = 0
                pad.rumbleUntilMs = 0L
                pad.rumbleLastSetMs = now
                pad.deviceId
            } else {
                // Match ns-client-style throttling: avoid restarting the Android
                // controller vibrator every 10-16ms when the same rumble packet repeats.
                if (pad.rumbleLow == low && pad.rumbleHigh == high && now - pad.rumbleLastSetMs < 100L) {
                    pad.rumbleUntilMs = now + durationMs
                    return
                }
                pad.rumbleLow = low
                pad.rumbleHigh = high
                pad.rumbleUntilMs = now + durationMs
                pad.rumbleLastSetMs = now
                pad.deviceId
            }
        }

        val device = InputDevice.getDevice(deviceId) ?: return
        try {
            val vib: Vibrator? = if (Build.VERSION.SDK_INT >= 31) {
                device.vibratorManager.defaultVibrator
            } else {
                @Suppress("DEPRECATION")
                device.vibrator
            }
            if (neutral) {
                vib?.cancel()
                Log.d(TAG, "controller rumble stop slot=${subpad + 1}")
                return
            }
            if (vib != null && vib.hasVibrator()) {
                if (Build.VERSION.SDK_INT >= 26) {
                    val amp = if (vib.hasAmplitudeControl()) strength.coerceAtLeast(32) else VibrationEffect.DEFAULT_AMPLITUDE
                    vib.vibrate(VibrationEffect.createOneShot(durationMs, amp))
                } else {
                    @Suppress("DEPRECATION")
                    vib.vibrate(durationMs)
                }
                Log.d(TAG, "controller rumble slot=${subpad + 1} low=$low high=$high durationMs=$durationMs")
            } else {
                Log.d(TAG, "controller has no vibrator slot=${subpad + 1} ${device.name}")
            }
        } catch (t: Throwable) {
            Log.w(TAG, "controller rumble failed slot=${subpad + 1}", t)
        }
    }

    private fun stopAllPhysicalRumble() {
        for (i in 0 until Protocol.PAD_COUNT) physicalRumble(i, 0, 0, 0)
    }

    private fun startPhysicalControllerSensorsLocked(slot: Int, device: InputDevice) {
        if (Build.VERSION.SDK_INT < 31) return
        try {
            val sm = device.sensorManager
            val gravity = sm.getDefaultSensor(Sensor.TYPE_GRAVITY)
            val accel = sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
            val gyro = sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE) ?: return
            val listener = object : SensorEventListener {
                override fun onSensorChanged(event: SensorEvent) {
                    synchronized(physicalLock) {
                        if (slot !in 0 until Protocol.PAD_COUNT || physicalPads[slot].deviceId != device.id) return
                        when (event.sensor.type) {
                            Sensor.TYPE_GRAVITY -> { for (i in 0 until minOf(3, event.values.size)) physicalGravity[slot][i] = event.values[i]; physicalHasGravity[slot] = true }
                            Sensor.TYPE_ACCELEROMETER -> { for (i in 0 until minOf(3, event.values.size)) physicalAccel[slot][i] = event.values[i]; physicalHasAccel[slot] = true }
                            Sensor.TYPE_GYROSCOPE -> { for (i in 0 until minOf(3, event.values.size)) physicalGyro[slot][i] = event.values[i]; physicalHasGyro[slot] = true; pushPhysicalMotionSampleLocked(slot) }
                        }
                    }
                }
                override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) = Unit
            }
            val accelOpened = accel?.let {
                sm.registerListener(listener, it, MOTION_SENSOR_PERIOD_US)
            } ?: false
            if (!accelOpened) {
                gravity?.let { sm.registerListener(listener, it, MOTION_SENSOR_PERIOD_US) }
            }
            sm.registerListener(listener, gyro, MOTION_SENSOR_PERIOD_US)
            physicalSensorManagers[slot] = sm
            physicalSensorListeners[slot] = listener
            physicalPads[slot].hasGyro = true
        } catch (t: Throwable) {
            Log.d(TAG, "controller gyro unavailable slot=${slot + 1}: ${t.message}")
        }
    }

    private fun pushPhysicalMotionSampleLocked(slot: Int) {
        if (!physicalHasGyro[slot] || (!physicalHasAccel[slot] && !physicalHasGravity[slot])) return

        // Match the phone path: preserve real translational acceleration for
        // gesture-heavy games, with gravity only as a compatibility fallback.
        val a = if (physicalHasAccel[slot]) physicalAccel[slot] else physicalGravity[slot]
        val g = physicalGyro[slot]

        val sample = NativeProtocol.nativePhoneMotion(a[0], a[1], a[2], g[0], g[1], g[2])

        val pad = physicalPads[slot]
        pad.motionSamples[0] = pad.motionSamples[1]
        pad.motionSamples[1] = pad.motionSamples[2]
        pad.motionSamples[2] = sample
        if (pad.motionSampleCount < Protocol.MOTION_SAMPLE_COUNT) pad.motionSampleCount++
        pad.hasMotion = pad.motionSampleCount >= Protocol.MOTION_SAMPLE_COUNT
        pad.motionRevision++
    }

    private fun stopPhysicalControllerSensors() {
        synchronized(physicalLock) { stopPhysicalControllerSensorsLocked(clearPads = true) }
    }

    private fun stopPhysicalControllerSensorsLocked(clearPads: Boolean) {
        for (i in 0 until Protocol.PAD_COUNT) {
            try {
                val sm = physicalSensorManagers[i]
                val listener = physicalSensorListeners[i]
                if (sm != null && listener != null) sm.unregisterListener(listener)
            } catch (_: Throwable) {}
            physicalSensorManagers[i] = null
            physicalSensorListeners[i] = null
            physicalHasGravity[i] = false; physicalHasAccel[i] = false; physicalHasGyro[i] = false
            if (clearPads) physicalPads[i].reset()
        }
    }

    private fun jsEscape(v: String): String = v.replace("\\", "\\\\").replace("'", "\\'").replace("\n", " ")

    private fun updatePhysicalStatusOnPage(prefix: String? = null) {
        if (currentPage != Page.MAIN_MENU) return
        val lines = synchronized(physicalLock) {
            Array(Protocol.PAD_COUNT) { i ->
                val p = physicalPads[i]
                if (!p.present) "P${i + 1}: Empty"
                else "P${i + 1}: ${p.name}${if (p.hasGyro) " + gyro" else ""}"
            }
        }
        val status = prefix ?: when (activeClientMode) {
            ClientMode.PHYSICAL -> "Connected"
            ClientMode.TOUCH -> "Touch Controls running"
            ClientMode.NONE -> "Ready"
        }
        fun jsEscape(v: String): String = v.replace("\\", "\\\\").replace("'", "\\'").replace("\n", " ")
        val connectButtonText = if (activeClientMode == ClientMode.PHYSICAL && controlClientActive) "Disconnect" else "Connect"
        val js = buildString {
            append("(function(){")
            append("var s=document.getElementById('statusText'); if(s)s.textContent='").append(jsEscape(status)).append("';")
            append("var b=document.getElementById('btnConnect'); if(b)b.textContent='").append(jsEscape(connectButtonText)).append("';")
            for (i in 0 until Protocol.PAD_COUNT) {
                append("var p=document.getElementById('p").append(i + 1).append("Text'); if(p)p.textContent='").append(jsEscape(lines[i])).append("';")
            }
            append("})()")
        }
        try { webView.evaluateJavascript(js, null) } catch (_: Throwable) {}
    }

    override fun onDestroy() {
        disconnect()
        try { if (::inputManager.isInitialized) inputManager.unregisterInputDeviceListener(inputDeviceListener) } catch (_: Throwable) {}
        super.onDestroy()
    }

    private fun activateControlClient() {
        if (controlClientActive && activeClientMode == ClientMode.TOUCH) return

        deactivateControlClient()
        stopPhysicalControllerSensors()

        activeClientMode = ClientMode.TOUCH
        touchHid = null
        touchFrame = null
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        controlClientActive = true

        if (!connectUdp()) {
            controlClientActive = false
            activeClientMode = ClientMode.NONE
        }
    }

    private fun deactivateControlClient() {
        if (!controlClientActive && udp == null && !sending) return
        val closing = udp
        senderToken.incrementAndGet()
        sending = false
        controlClientActive = false
        touchHid = null
        touchFrame = null
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        udp = null
        stopPhoneSensors()
        stopPhysicalControllerSensors()
        stopAllPhysicalRumble()
        activeClientMode = ClientMode.NONE

        if (closing != null) {
            Thread {
                try {
                    repeat(3) {
                        sendDisconnectFrameTo(closing)
                        try { Thread.sleep(4) } catch (_: InterruptedException) { return@Thread }
                    }
                } catch (_: Throwable) {}
                closing.close()
            }.start()
        }
    }

    private fun disconnect() {
        deactivateControlClient()
        connected = false
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        try { webView.loadUrl("about:blank") } catch (_: Throwable) {}
        setContentView(connectView)
    }
}

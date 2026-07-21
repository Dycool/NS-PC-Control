import UIKit
import WebKit
import CoreMotion
import CryptoKit
import Network
import GameController
import CoreHaptics

private enum Page {
    case mainMenu
    case touchControls
    case editor
}

private enum ClientMode {
    case none
    case touch
    case physical
}

private enum ProtocolWire {
    static let frameSize = 212
    static let hidSize = 8
    static let motionSampleSize = 12
    static let motionSampleCount = 3
    static let padCount = 4

    static let controllerTypeJoyConL = 1
    static let controllerTypeJoyConR = 2
    static let controllerTypePro = 3

    static let rumblePacketSize = 8
    static let precisionRumblePacketSize = 20
    static let rumbleMagic: UInt32 = 0x4E535652
    static let precisionRumbleMagic: UInt32 = 0x4E535648
    static let serverInfoMagic: UInt32 = 0x4E535349
    static let clientAssignmentMagic: UInt32 = 0x4E534341
    static let clientAssignmentSize = 16
    static let assignmentFlagServerFull = 0x02
    static let assignmentFlagProfileUnsupported = 0x10

    // Authenticated UDP wire format (same as the desktop ns-client): payload
    // (auth region) + first 16 bytes of HMAC-SHA256 keyed with SHA-256(secret).
    static let defaultUdpPort: UInt16 = 7331
    static let hmacTagSize = 16
    static let frameAuthSize = 212   // input frame: 212 + 16 = 228 on the wire
    static let namesAuthSize = 208   // ClientNamesPacket: hmac lives at 208..223
    private static let hmacKey = SymmetricKey(
        data: Data(SHA256.hash(data: Data("nsc-R2xvCy7Eyw2nfbZIOGyKZPnostpaRY".utf8))))

    static func signed(_ payload: [UInt8], authLen: Int) -> Data {
        var data = Data(payload.prefix(authLen))
        let tag = HMAC<SHA256>.authenticationCode(for: data, using: hmacKey)
        data.append(contentsOf: Array(tag).prefix(hmacTagSize))
        return data
    }

    static func signed(_ payload: Data, authLen: Int) -> Data {
        var data = payload.prefix(authLen)
        let tag = HMAC<SHA256>.authenticationCode(for: data, using: hmacKey)
        data.append(contentsOf: Array(tag).prefix(hmacTagSize))
        return data
    }

    static func readU32LE(_ data: Data, _ off: Int) -> UInt32 {
        guard data.count >= off + 4 else { return 0 }
        let b = data
        let i = data.startIndex
        return UInt32(b[i + off]) | (UInt32(b[i + off + 1]) << 8)
            | (UInt32(b[i + off + 2]) << 16) | (UInt32(b[i + off + 3]) << 24)
    }

    static let flagReset = 0x01
    static let flagDisconnect = 0x08
    static let flagSinglePad = 0x04
    static let extStatusBatteryValid = 0x01
    static let extStatusBatteryCharging = 0x02
    static let extStatusMotionFresh = 0x04
    static let extStatusMotionFreshValid = 0x08

    static let btnY       = 1 << 0
    static let btnB       = 1 << 1
    static let btnA       = 1 << 2
    static let btnX       = 1 << 3
    static let btnL       = 1 << 4
    static let btnR       = 1 << 5
    static let btnZL      = 1 << 6
    static let btnZR      = 1 << 7
    static let btnMinus   = 1 << 8
    static let btnPlus    = 1 << 9
    static let btnLStick  = 1 << 10
    static let btnRStick  = 1 << 11
    static let btnHome    = 1 << 12
    static let btnCapture = 1 << 13

    static let hatN = 0
    static let hatNE = 1
    static let hatE = 2
    static let hatSE = 3
    static let hatS = 4
    static let hatSW = 5
    static let hatW = 6
    static let hatNW = 7
    static let hatNeutral = 8

    static func neutralHid() -> [UInt8] {
        var out = [UInt8](repeating: 0, count: hidSize)
        out.withUnsafeMutableBufferPointer { b in
            ns_hid_write_neutral(b.baseAddress!)
        }
        return out
    }

    static func neutralMotion() -> [UInt8] {
        [UInt8](repeating: 0, count: motionSampleSize)
    }

    static func hid(buttons: Int, hat: Int, lx: Int, ly: Int, rx: Int, ry: Int, present: Bool) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: hidSize)
        out.withUnsafeMutableBufferPointer { b in
            ns_hid_write(b.baseAddress!, UInt16(buttons & 0xFFFF), UInt8(clamping: hat),
                         UInt8(clamping: lx), UInt8(clamping: ly), UInt8(clamping: rx), UInt8(clamping: ry),
                         present ? 1 : 0)
        }
        return out
    }

    static func axisToByte(_ value: Float) -> Int {
        Int(ns_axis_to_byte(value))
    }

    static func normalizeShortcuts(_ buttons: Int) -> Int {
        Int(ns_normalize_system_shortcuts(UInt16(buttons & 0xFFFF)))
    }

    static func motionFromApple(accelX: Float, accelY: Float, accelZ: Float,
                                rotationX: Float, rotationY: Float, rotationZ: Float) -> [UInt8] {
        // Apple/CoreMotion acceleration uses the opposite sign from Android/SDL for
        // the rest vector: iPhone face-up is about z=-1G, while Android is +9.8m/s².
        // Convert Apple G units to Android-compatible m/s² before the shared
        // (-Z, -X, +Y) Switch mapping in ns_motion_from_android().
        let g = Float(9.80665)
        var out = [UInt8](repeating: 0, count: motionSampleSize)
        out.withUnsafeMutableBufferPointer { b in
            ns_motion_from_android(b.baseAddress!, -accelX * g, -accelY * g, -accelZ * g,
                                   rotationX, rotationY, rotationZ)
        }
        return out
    }

    private static func clampMotion(_ value: Float) -> Int16 {
        let rounded = value.rounded()
        if rounded > Float(Int16.max) { return Int16.max }
        if rounded < Float(Int16.min) { return Int16.min }
        return Int16(rounded)
    }

    private static func gyroDeadzone(_ value: Int16) -> Int16 {
        return abs(Int(value)) <= 32 ? 0 : value
    }

    static func motionFromControllerApple(accelX: Float, accelY: Float, accelZ: Float,
                                          rotationX: Float, rotationY: Float, rotationZ: Float) -> [UInt8] {
        // GCController.motion uses a different axis basis from the phone/CoreMotion path.
        // Match the known-good PC SDL3 controller path at the final Switch-space packet level:
        //   accel final: (-AppleY, +AppleX, -AppleZ)
        //   gyro final:  (+AppleY, -AppleX, +AppleZ)
        // Do NOT pass this through motionFromApple(), because that phone helper intentionally
        // applies the Android/phone remap and produces a 90-degree rotated accel vector for
        // controllers connected through iOS/iPadOS.
        let accelScale = Float(4096.0)
        let gyroScale = Float(57.29577951308232 * 16.384)

        let ax = clampMotion(-accelY * accelScale)
        let ay = clampMotion( accelX * accelScale)
        let az = clampMotion(-accelZ * accelScale)

        let gx = gyroDeadzone(clampMotion( rotationY * gyroScale))
        let gy = gyroDeadzone(clampMotion(-rotationX * gyroScale))
        let gz = gyroDeadzone(clampMotion( rotationZ * gyroScale))

        var out = [UInt8](repeating: 0, count: motionSampleSize)
        out.withUnsafeMutableBufferPointer { b in
            ns_motion_write_values(b.baseAddress!, ax, ay, az, gx, gy, gz, 1)
        }
        return out
    }

    static func initFrame(flags: Int, seq: UInt32, timestampUs: UInt64) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: frameSize)
        out.withUnsafeMutableBufferPointer { b in
            ns_web_frame_init(b.baseAddress!, UInt8(flags & 0xFF), seq, timestampUs)
        }
        return out
    }

    static func setFrameHid(_ frame: inout [UInt8], pad: Int, hid: [UInt8]) {
        frame.withUnsafeMutableBufferPointer { f in
            hid.withUnsafeBufferPointer { h in
                ns_web_frame_set_hid(f.baseAddress!, Int32(pad), h.baseAddress!)
            }
        }
    }

    static func setFrameMotionSamples(_ frame: inout [UInt8], pad: Int, samples: [[UInt8]]) {
        guard samples.count >= motionSampleCount else { return }
        frame.withUnsafeMutableBufferPointer { f in
            samples[0].withUnsafeBufferPointer { m0 in
                samples[1].withUnsafeBufferPointer { m1 in
                    samples[2].withUnsafeBufferPointer { m2 in
                        ns_web_frame_set_motion_samples(f.baseAddress!, Int32(pad), m0.baseAddress!, m1.baseAddress!, m2.baseAddress!)
                    }
                }
            }
        }
    }

    static func setFrameMotionFresh(_ frame: inout [UInt8], pad: Int, fresh: Bool) {
        guard pad >= 0 && pad < padCount else { return }
        let base = 20 + pad * 48
        guard frame.count >= base + 48 else { return }
        frame[base + 46] |= UInt8(extStatusMotionFreshValid)
        if fresh {
            frame[base + 46] |= UInt8(extStatusMotionFresh)
        } else {
            frame[base + 46] &= ~UInt8(extStatusMotionFresh)
        }
    }

    static func setFrameBatteryPercent(_ frame: inout [UInt8], pad: Int, percent: Int, charging: Bool = false) {
        guard pad >= 0 && pad < padCount && percent >= 0 && percent <= 100 else { return }
        let base = 20 + pad * 48
        guard frame.count >= base + 48 else { return }
        frame[base + 45] = UInt8(percent)
        frame[base + 46] |= UInt8(extStatusBatteryValid)
        if charging { frame[base + 46] |= UInt8(extStatusBatteryCharging) }
    }

    static func setFrameControllerType(_ frame: inout [UInt8], pad: Int, controllerType: Int) {
        guard pad >= 0 && pad < padCount && controllerType >= 1 && controllerType <= 3 else { return }
        let base = 20 + pad * 48
        guard frame.count >= base + 48 else { return }
        frame[base + 47] = UInt8(controllerType)
    }

    static func extractPad0Hid(from frame: [UInt8]) -> [UInt8]? {
        guard frame.count >= 20 + hidSize else { return nil }
        var out = [UInt8](repeating: 0, count: hidSize)
        frame.withUnsafeBufferPointer { f in
            out.withUnsafeMutableBufferPointer { h in
                _ = ns_web_frame_extract_hid(f.baseAddress!, frame.count, 0, h.baseAddress!)
            }
        }
        return out
    }
}

private final class Locked<T> {
    private let lock = NSLock()
    private var value: T

    init(_ value: T) { self.value = value }

    func withLock<R>(_ body: (inout T) -> R) -> R {
        lock.lock()
        defer { lock.unlock() }
        return body(&value)
    }
}

private final class PhysicalPad {
    var controller: GCController?
    var name = "Empty"
    var present = false
    var buttons = 0
    var dpadUp = false
    var dpadDown = false
    var dpadLeft = false
    var dpadRight = false
    var lx = 128
    var ly = 128
    var rx = 128
    var ry = 128
    var hasMotion = false
    var hasGyro = false
    var hasRumble = false
    var rumbleLow = 0
    var rumbleHigh = 0
    var rumbleUntilMs: UInt64 = 0
    var rumbleLastSetMs: UInt64 = 0
    var hapticEngine: CHHapticEngine?
    var hapticPlayer: CHHapticPatternPlayer?
    var motionSamples = Array(repeating: ProtocolWire.neutralMotion(), count: ProtocolWire.motionSampleCount)
    var motionSampleCount = 0
    var motionRevision: UInt64 = 0
    var sentMotionRevision: UInt64 = .max

    func reset() {
        cleanupRumble()
        controller = nil
        name = "Empty"
        present = false
        buttons = 0
        dpadUp = false
        dpadDown = false
        dpadLeft = false
        dpadRight = false
        lx = 128; ly = 128; rx = 128; ry = 128
        hasMotion = false
        hasGyro = false
        hasRumble = false
        rumbleLow = 0
        rumbleHigh = 0
        rumbleUntilMs = 0
        rumbleLastSetMs = 0
        motionSampleCount = 0
        motionRevision = 0
        sentMotionRevision = .max
        motionSamples = Array(repeating: ProtocolWire.neutralMotion(), count: ProtocolWire.motionSampleCount)
    }

    func hid() -> [UInt8] {
        let hat: Int
        if dpadUp && dpadRight { hat = ProtocolWire.hatNE }
        else if dpadUp && dpadLeft { hat = ProtocolWire.hatNW }
        else if dpadDown && dpadRight { hat = ProtocolWire.hatSE }
        else if dpadDown && dpadLeft { hat = ProtocolWire.hatSW }
        else if dpadUp { hat = ProtocolWire.hatN }
        else if dpadRight { hat = ProtocolWire.hatE }
        else if dpadDown { hat = ProtocolWire.hatS }
        else if dpadLeft { hat = ProtocolWire.hatW }
        else { hat = ProtocolWire.hatNeutral }
        return ProtocolWire.hid(buttons: ProtocolWire.normalizeShortcuts(buttons), hat: hat,
                                lx: lx, ly: ly, rx: rx, ry: ry, present: present)
    }

    func stopRumble() {
        try? hapticPlayer?.stop(atTime: CHHapticTimeImmediate)
        hapticPlayer = nil
    }

    func cleanupRumble() {
        stopRumble()
        hapticEngine?.stop(completionHandler: nil)
        hapticEngine = nil
    }
}

final class ViewController: UIViewController, WKScriptMessageHandler, WKNavigationDelegate, UIGestureRecognizerDelegate {
    var orientationMask: UIInterfaceOrientationMask = .allButUpsideDown
    override var supportedInterfaceOrientations: UIInterfaceOrientationMask { orientationMask }
    override var preferredInterfaceOrientationForPresentation: UIInterfaceOrientation {
        currentPage == .touchControls || currentPage == .editor ? .landscapeRight : .portrait
    }

    private let connectView = UIView()
    private let hostField = UITextField()
    private let statusLabel = UILabel()
    private let connectButton = UIButton(type: .system)
    private var webView: WKWebView!

    private var host = ""
    private var connected = false
    private var controlClientActive = false
    private var sending = false
    // Authenticated UDP transport (same path as the desktop ns-client).
    private var udp: NWConnection?
    private var seq: UInt32 = 0
    // Input frames are the latency-critical path: user-interactive QoS keeps
    // the 4 ms sender ticks on time under load.
    private let sendQueue = DispatchQueue(label: "ns.mobile.ios.sender", qos: .userInteractive)
    private let stateQueue = DispatchQueue(label: "ns.mobile.ios.state")
    private let udpQueue = DispatchQueue(label: "ns.mobile.ios.udp", qos: .userInteractive)
    private var senderToken = 0
    // 0 forces the 4 ms sender loop to (re)send the client names on its next
    // tick; the loop then refreshes them every 2 s (UDP is lossy, this heals).
    private var lastNamesSentMs: UInt64 = 0

    private var currentOrientation: UIInterfaceOrientation = .landscapeRight
    private var currentPage: Page = .mainMenu
    private var pageStack: [Page] = []
    private var activeClientMode: ClientMode = .none

    private let motionManager = CMMotionManager()
    private let motionQueue = OperationQueue()
    private var phoneSensorsActive = false
    private let phoneMotion = Locked(PhoneMotionState())

    private struct PhoneMotionState {
        var samples = Array(repeating: ProtocolWire.neutralMotion(), count: ProtocolWire.motionSampleCount)
        var count = 0
        var revision: UInt64 = 0
        var sentRevision: UInt64 = .max
    }

    private var touchHid: [UInt8]?
    private var touchFrame: [UInt8]?
    private var lastTouchFrameMs: UInt64 = 0
    private var touchControllerType = 3
    private var physicalControllerType = ProtocolWire.controllerTypePro
    private var touchExtraButtons = 0
    private var lastBridgeFrameParseMs: UInt64 = 0

    private let physicalPads = Locked((0..<ProtocolWire.padCount).map { _ in PhysicalPad() })
    private var controllerSlots: [ObjectIdentifier: Int] = [:]

    override func viewDidLoad() {
        super.viewDidLoad()
        UIDevice.current.isBatteryMonitoringEnabled = true
        view.backgroundColor = .black
        requestLocalNetworkPermission()
        setupConnectView()
        setupWebView()
        setupControllerObservers()
        view.addSubview(connectView)
        connectView.frame = view.bounds
        connectView.autoresizingMask = [.flexibleWidth, .flexibleHeight]
        hostField.text = UserDefaults.standard.string(forKey: "host") ?? ""
    }

    private func requestLocalNetworkPermission() {
        let connection = NWConnection(
            host: NWEndpoint.Host("255.255.255.255"),
            port: NWEndpoint.Port(integerLiteral: 0),
            using: .udp
        )
        connection.stateUpdateHandler = { state in
            switch state {
            case .ready, .failed, .cancelled, .waiting:
                connection.cancel()
                connection.stateUpdateHandler = nil
            default:
                break
            }
        }
        connection.start(queue: DispatchQueue.global(qos: .background))
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        currentOrientation = view.window?.windowScene?.interfaceOrientation ?? .landscapeRight
        connectView.frame = view.bounds
        webView?.frame = view.bounds
    }

    private func setupConnectView() {
        connectView.backgroundColor = UIColor(red: 0.03, green: 0.03, blue: 0.04, alpha: 1.0)

        let logoView = UIImageView(image: UIImage(named: "Logo"))
        logoView.contentMode = .scaleAspectFit
        logoView.translatesAutoresizingMaskIntoConstraints = false

        let titleLabel = UILabel()
        titleLabel.text = "NS Mobile"
        titleLabel.textColor = UIColor(red: 0.80, green: 0.0, blue: 0.0, alpha: 1.0)
        titleLabel.font = UIFont.systemFont(ofSize: 34, weight: .bold)
        titleLabel.textAlignment = .center
        titleLabel.translatesAutoresizingMaskIntoConstraints = false

        let subtitleLabel = UILabel()
        subtitleLabel.text = "Connect to your Raspberry Pi server"
        subtitleLabel.textColor = UIColor(white: 1.0, alpha: 0.55)
        subtitleLabel.font = UIFont.systemFont(ofSize: 16)
        subtitleLabel.textAlignment = .center
        subtitleLabel.translatesAutoresizingMaskIntoConstraints = false

        hostField.placeholder = "Server IP or domain"
        hostField.autocapitalizationType = .none
        hostField.autocorrectionType = .no
        hostField.keyboardType = .URL
        hostField.returnKeyType = .go
        hostField.textColor = .white
        hostField.tintColor = .white
        hostField.backgroundColor = UIColor(white: 1.0, alpha: 0.10)
        hostField.layer.cornerRadius = 12
        hostField.layer.borderWidth = 1
        hostField.layer.borderColor = UIColor(white: 1.0, alpha: 0.15).cgColor
        hostField.leftView = UIView(frame: CGRect(x: 0, y: 0, width: 16, height: 1))
        hostField.leftViewMode = .always
        hostField.translatesAutoresizingMaskIntoConstraints = false
        hostField.attributedPlaceholder = NSAttributedString(string: "Server IP or domain", attributes: [.foregroundColor: UIColor(white: 1.0, alpha: 0.40)])
        hostField.addTarget(self, action: #selector(hostReturnPressed), for: .editingDidEndOnExit)

        connectButton.setTitle("Connect", for: .normal)
        connectButton.titleLabel?.font = UIFont.systemFont(ofSize: 18, weight: .semibold)
        connectButton.backgroundColor = UIColor(red: 0.80, green: 0.0, blue: 0.0, alpha: 1.0)
        connectButton.tintColor = .white
        connectButton.layer.cornerRadius = 12
        connectButton.translatesAutoresizingMaskIntoConstraints = false
        connectButton.addTarget(self, action: #selector(onConnect), for: .touchUpInside)

        statusLabel.text = "Ready"
        statusLabel.textColor = UIColor(white: 1.0, alpha: 0.45)
        statusLabel.textAlignment = .center
        statusLabel.translatesAutoresizingMaskIntoConstraints = false

        let stack = UIStackView(arrangedSubviews: [logoView, titleLabel, subtitleLabel, hostField, connectButton, statusLabel])
        stack.axis = .vertical
        stack.spacing = 0
        stack.setCustomSpacing(16, after: logoView)
        stack.setCustomSpacing(8, after: titleLabel)
        stack.setCustomSpacing(40, after: subtitleLabel)
        stack.setCustomSpacing(20, after: hostField)
        stack.setCustomSpacing(16, after: connectButton)
        stack.translatesAutoresizingMaskIntoConstraints = false
        connectView.addSubview(stack)

        NSLayoutConstraint.activate([
            logoView.heightAnchor.constraint(equalToConstant: 96),
            logoView.widthAnchor.constraint(equalToConstant: 96),
            stack.centerYAnchor.constraint(equalTo: connectView.centerYAnchor),
            stack.centerXAnchor.constraint(equalTo: connectView.centerXAnchor),
            stack.widthAnchor.constraint(equalTo: connectView.safeAreaLayoutGuide.widthAnchor, constant: -64),
            hostField.heightAnchor.constraint(equalToConstant: 56),
            connectButton.heightAnchor.constraint(equalToConstant: 56)
        ])
        stack.widthAnchor.constraint(lessThanOrEqualToConstant: 400).isActive = true
    }

    private func setupWebView() {
        let content = WKUserContentController()
        let bridgeScript = """
        (function(){
          if (window.NSBridge) return;
          function post(name,args){ window.webkit.messageHandlers.NSBridge.postMessage({name:name,args:args||[]}); }
          window.NSBridge = {
            onOpen:function(){post('onOpen');},
            onBinary:function(json){post('onBinary',[json]);},
            onTouchState:function(buttons,hat,lx,ly,rx,ry){post('onTouchState',[buttons,hat,lx,ly,rx,ry]);},
            onTouchControllerType:function(controllerType){post('onTouchControllerType',[controllerType]);},
            onTouchExtraButtons:function(extraButtons){post('onTouchExtraButtons',[extraButtons]);},
            onClose:function(){post('onClose');},
            onPhysicalControllerType:function(controllerType){post('onPhysicalControllerType',[controllerType]);},
            onPhysicalStart:function(){post('onPhysicalStart');},
            onPhysicalStop:function(){post('onPhysicalStop');},
            onPhysicalRefresh:function(){post('onPhysicalRefresh');},
            onOpenTouch:function(){post('onOpenTouch');},
            onOpenEditor:function(){post('onOpenEditor');},
            onBack:function(){post('onBack');}
          };
        })();
        """
        content.addUserScript(WKUserScript(source: bridgeScript, injectionTime: .atDocumentStart, forMainFrameOnly: false))
        content.add(self, name: "NSBridge")

        let config = WKWebViewConfiguration()
        config.userContentController = content
        config.defaultWebpagePreferences.allowsContentJavaScript = true
        webView = WKWebView(frame: view.bounds, configuration: config)
        webView.navigationDelegate = self
        webView.backgroundColor = .black
        webView.isOpaque = false
        webView.scrollView.bounces = false
        webView.scrollView.isScrollEnabled = false
        webView.autoresizingMask = [.flexibleWidth, .flexibleHeight]

        let edge = UIScreenEdgePanGestureRecognizer(target: self, action: #selector(edgeBack(_:)))
        edge.edges = .left
        edge.cancelsTouchesInView = false
        edge.delegate = self
        webView.addGestureRecognizer(edge)

        let swipeRight = UISwipeGestureRecognizer(target: self, action: #selector(swipeBack(_:)))
        swipeRight.direction = .right
        swipeRight.cancelsTouchesInView = false
        swipeRight.delegate = self
        webView.addGestureRecognizer(swipeRight)
    }

    @objc private func hostReturnPressed() { onConnect() }

    @objc private func onConnect() {
        host = (hostField.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        guard !host.isEmpty else { return }
        UserDefaults.standard.set(host, forKey: "host")
        statusLabel.text = "Connecting..."
        connectButton.isEnabled = false
        probeServer(host) { [weak self] reachable in
            DispatchQueue.main.async {
                guard let self else { return }
                self.connectButton.isEnabled = true
                guard reachable else {
                    self.statusLabel.text = "Server not reachable"
                    return
                }
                self.connected = true
                self.currentPage = .mainMenu
                self.pageStack.removeAll()
                self.statusLabel.text = "Loaded"
                self.connectView.removeFromSuperview()
                self.view.addSubview(self.webView)
                self.load(page: .mainMenu)
            }
        }
    }

    // UDP ServerInfo probe: 8-byte unauthenticated ServerInfoProbe, answered by
    // a 16-byte ServerInfoReply even when every slot is busy. Replaces the old
    // TCP probe against the web port (which no longer exists without -w).
    private func probeServer(_ raw: String, completion: @escaping (Bool) -> Void) {
        guard let (endpointHost, endpointPort) = try? parseHostPort(raw) else { completion(false); return }
        let params = NWParameters.udp
        params.serviceClass = .responsiveData
        let connection = NWConnection(host: endpointHost, port: endpointPort, using: params)
        let probeLock = NSLock()
        var probeDone = false
        func finish(_ ok: Bool) {
            probeLock.lock()
            guard !probeDone else { probeLock.unlock(); return }
            probeDone = true
            probeLock.unlock()
            connection.cancel()
            completion(ok)
        }
        var probe = Data(count: 8)
        probe[0] = 0x49; probe[1] = 0x53; probe[2] = 0x53; probe[3] = 0x4E // 'NSSI' LE
        probe[4] = 1 // SERVER_INFO_VERSION
        func awaitReply() {
            connection.receiveMessage { data, _, _, error in
                if let data, data.count >= 16,
                   ProtocolWire.readU32LE(data, 0) == ProtocolWire.serverInfoMagic {
                    finish(true)
                } else if error != nil {
                    finish(false)
                } else {
                    awaitReply()
                }
            }
        }
        connection.stateUpdateHandler = { state in
            switch state {
            case .ready:
                awaitReply()
                // A couple of retries: single UDP datagrams can be lost.
                for delay in [0.0, 0.6, 1.2] {
                    DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + delay) {
                        probeLock.lock()
                        let done = probeDone
                        probeLock.unlock()
                        if !done { connection.send(content: probe, completion: .contentProcessed { _ in }) }
                    }
                }
            case .failed:
                finish(false)
            default:
                break
            }
        }
        connection.start(queue: DispatchQueue.global(qos: .utility))
        DispatchQueue.global(qos: .utility).asyncAfter(deadline: .now() + 2.0) { finish(false) }
    }

    @objc private func edgeBack(_ recognizer: UIScreenEdgePanGestureRecognizer) {
        guard connected else { return }
        guard recognizer.state == .ended || recognizer.state == .recognized else { return }
        let translation = recognizer.translation(in: webView)
        guard translation.x > 30 else { return }

        if currentPage == .mainMenu {
            disconnect()
        } else {
            goBack()
        }
    }

    @objc private func swipeBack(_ recognizer: UISwipeGestureRecognizer) {
        guard connected else { return }
        guard recognizer.state == .ended || recognizer.state == .recognized else { return }

        if currentPage == .mainMenu {
            disconnect()
        } else {
            goBack()
        }
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldReceive touch: UITouch) -> Bool {
        connected && (currentPage == .mainMenu || currentPage == .touchControls || currentPage == .editor)
    }

    func gestureRecognizer(_ gestureRecognizer: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith otherGestureRecognizer: UIGestureRecognizer) -> Bool {
        true
    }

    private func pageURL(_ page: Page) -> URL? {
        let name: String
        switch page {
        case .mainMenu: name = "index"
        case .touchControls: name = "mobile"
        case .editor: name = "editor"
        }
        return Bundle.main.url(forResource: name, withExtension: "html", subdirectory: "ns_mobile")
    }

    private func load(page: Page) {
        guard let url = pageURL(page), let dir = Bundle.main.url(forResource: "ns_mobile", withExtension: nil) else { return }
        webView.loadFileURL(url, allowingReadAccessTo: dir)
    }

    private func navTo(_ page: Page) {
        if page == .touchControls || page == .editor {
            deactivateControlClient()
            clearPhysicalControllers()
        }
        pageStack.append(currentPage)
        enterPage(page)
    }

    private func enterPage(_ page: Page) {
        currentPage = page
        if page == .touchControls || page == .editor {
            deactivateControlClient()
            lockLandscapeOrientation()
            setFullscreen(true)
        } else {
            unlockInterfaceOrientation()
            setFullscreen(false)
        }
        load(page: page)
    }

    private func goBack() {
        guard !pageStack.isEmpty else { return }
        if currentPage == .touchControls || currentPage == .editor {
            deactivateControlClient()
            clearPhysicalControllers()
        }
        let page = pageStack.removeLast()
        enterPage(page)
    }

    private func lockLandscapeOrientation() {
        // Touch Controls and the editor use one fixed physical landscape pose.
        // Other screens keep the system's normal orientation behavior.
        orientationMask = .landscapeRight
        currentOrientation = .landscapeRight
        if #available(iOS 16.0, *) {
            setNeedsUpdateOfSupportedInterfaceOrientations()
            view.window?.windowScene?.requestGeometryUpdate(.iOS(interfaceOrientations: orientationMask))
        }
        UIDevice.current.setValue(UIInterfaceOrientation.landscapeRight.rawValue, forKey: "orientation")
    }

    private func unlockInterfaceOrientation() {
        orientationMask = .allButUpsideDown
        currentOrientation = view.window?.windowScene?.interfaceOrientation ?? currentOrientation
        if #available(iOS 16.0, *) {
            setNeedsUpdateOfSupportedInterfaceOrientations()
            view.window?.windowScene?.requestGeometryUpdate(.iOS(interfaceOrientations: orientationMask))
        }
    }

    private func setFullscreen(_ fullscreen: Bool) {
        navigationController?.setNavigationBarHidden(fullscreen, animated: false)
        setNeedsUpdateOfHomeIndicatorAutoHidden()
        UIView.performWithoutAnimation { setNeedsStatusBarAppearanceUpdate() }
    }

    override var prefersStatusBarHidden: Bool { currentPage == .touchControls || currentPage == .editor }
    override var prefersHomeIndicatorAutoHidden: Bool { currentPage == .touchControls || currentPage == .editor }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        if currentPage == .mainMenu {
            webView.evaluateJavaScript(mainMenuInjection(), completionHandler: nil)
        }
    }

    private func mainMenuInjection() -> String {
        """
        (function(){
          var kb = document.getElementById('kbModeContainer'); if (kb) kb.style.display = 'none';
          var bindings = document.getElementById('btnBindings'); if (bindings) bindings.style.display = 'none';
          var macros = document.getElementById('btnMacros'); if (macros) macros.style.display = 'none';
          var oldStart = document.getElementById('btn' + 'HubStart'); if (oldStart) oldStart.remove();
          var oldStop = document.getElementById('btn' + 'HubStop'); if (oldStop) oldStop.remove();
          var oldRefresh = document.getElementById('btn' + 'HubRefresh'); if (oldRefresh) oldRefresh.remove();
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
            connect.onclick = function(ev){ if(ev)ev.preventDefault(); publishPhysicalControllerType(); if(window.NSBridge&&NSBridge.onPhysicalStart)NSBridge.onPhysicalStart(); return false; };
          }
          function nsButtonHost(){
            return document.querySelector('.actions') || document.querySelector('main') || document.body;
          }
          var touch = document.getElementById('btnTouchControls');
          if (!touch) {
            touch = document.createElement('button');
            touch.id = 'btnTouchControls';
            touch.textContent = 'Touch Controls';
            nsButtonHost().appendChild(touch);
          }
          touch.style.display = 'inline-block';
          touch.onclick = function(ev){ if(ev)ev.preventDefault(); if(window.NSBridge&&NSBridge.onOpenTouch)NSBridge.onOpenTouch(); else window.location.href='mobile.html'; return false; };

          var editor = document.getElementById('btnEditor');
          if (!editor) {
            editor = document.createElement('button');
            editor.id = 'btnEditor';
            editor.textContent = 'Editor';
            nsButtonHost().appendChild(editor);
          }
          editor.style.display = 'inline-block';
          editor.onclick = function(ev){ if(ev)ev.preventDefault(); if(window.NSBridge&&NSBridge.onOpenEditor)NSBridge.onOpenEditor(); else window.location.href='editor.html'; return false; };
          if (window.NSBridge && NSBridge.onPhysicalRefresh) NSBridge.onPhysicalRefresh();
        })();
        """
    }

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        guard message.name == "NSBridge" else { return }
        guard let dict = message.body as? [String: Any], let name = dict["name"] as? String else { return }
        let args = dict["args"] as? [Any] ?? []

        switch name {
        case "onOpen":
            if currentPage == .touchControls { activateTouchClient() }
        case "onBinary":
            if let json = args.first as? String { onBinary(json: json) }
        case "onTouchState":
            if args.count >= 6 {
                onTouchState(buttons: intArg(args[0]), hat: intArg(args[1]), lx: intArg(args[2]), ly: intArg(args[3]), rx: intArg(args[4]), ry: intArg(args[5]),
                             controllerType: args.count > 6 ? intArg(args[6]) : nil)
            }
        case "onTouchControllerType":
            if let first = args.first {
                touchControllerType = ViewController.normalizedControllerType(intArg(first))
            }
        case "onPhysicalControllerType":
            if !(controlClientActive && activeClientMode == .physical), let first = args.first {
                physicalControllerType = ViewController.normalizedControllerType(intArg(first))
            }
        case "onTouchExtraButtons":
            if let first = args.first { touchExtraButtons = intArg(first) & (0x10 | 0x20) }
        case "onClose":
            deactivateControlClient()
        case "onPhysicalStart":
            togglePhysicalControllers()
        case "onPhysicalStop":
            deactivateControlClient()
            updatePhysicalStatusOnPage(prefix: "Not connected")
        case "onPhysicalRefresh":
            scanPhysicalControllers()
            updatePhysicalStatusOnPage()
        case "onOpenTouch":
            navTo(.touchControls)
        case "onOpenEditor":
            navTo(.editor)
        case "onBack":
            goBack()
        default:
            break
        }
    }

    private func intArg(_ value: Any) -> Int {
        if let v = value as? Int { return v }
        if let v = value as? Double { return Int(v) }
        if let v = value as? String { return Int(v) ?? 0 }
        return 0
    }

    private func onBinary(json: String) {
        guard currentPage == .touchControls && controlClientActive else { return }
        let now = uptimeMs()
        guard now - lastBridgeFrameParseMs >= 8 else { return }
        lastBridgeFrameParseMs = now
        guard let data = json.data(using: .utf8), let arr = try? JSONSerialization.jsonObject(with: data) as? [Any], arr.count >= 20 + ProtocolWire.hidSize else { return }
        let frame = arr.map { UInt8(clamping: intArg($0)) }
        touchFrame = frame
        touchHid = ProtocolWire.extractPad0Hid(from: frame)
        lastTouchFrameMs = now
    }

    // 0/unknown means "default": treat as Pro Controller, never Joy-Con.
    private static func normalizedControllerType(_ value: Int) -> Int {
        return (1...3).contains(value) ? value : 3
    }

    private func onTouchState(buttons: Int, hat: Int, lx: Int, ly: Int, rx: Int, ry: Int, controllerType: Int?) {
        guard currentPage == .touchControls && controlClientActive else { return }
        touchHid = ProtocolWire.hid(buttons: ProtocolWire.normalizeShortcuts(buttons),
                                    hat: min(max(hat, 0), 8),
                                    lx: min(max(lx, 0), 255),
                                    ly: min(max(ly, 0), 255),
                                    rx: min(max(rx, 0), 255),
                                    ry: min(max(ry, 0), 255),
                                    present: true)
        lastTouchFrameMs = uptimeMs()
        // Only trust an inline controller type (legacy webapps); the current
        // webapp sends it once via onTouchControllerType instead.
        if let type = controllerType {
            touchControllerType = ViewController.normalizedControllerType(type)
        }
    }

    private func togglePhysicalControllers() {
        if controlClientActive && activeClientMode == .physical {
            deactivateControlClient()
            updatePhysicalStatusOnPage(prefix: "Not connected")
        } else {
            activatePhysicalControllers()
        }
    }

    private func activatePhysicalControllers() {
        if controlClientActive && activeClientMode == .physical {
            scanPhysicalControllers()
            updatePhysicalStatusOnPage(prefix: "Connected")
            return
        }
        deactivateControlClient()
        currentPage = .mainMenu
        activeClientMode = .physical
        controlClientActive = true
        touchHid = nil
        touchFrame = nil
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        scanPhysicalControllers()
        updatePhysicalStatusOnPage(prefix: "Connecting...")
        if !connectUdp() {
            controlClientActive = false
            activeClientMode = .none
            clearPhysicalControllers()
            updatePhysicalStatusOnPage(prefix: "Not connected")
        }
    }

    private func activateTouchClient() {
        if controlClientActive && activeClientMode == .touch { return }
        deactivateControlClient()
        clearPhysicalControllers()
        activeClientMode = .touch
        touchHid = nil
        touchFrame = nil
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        controlClientActive = true
        if !connectUdp() {
            controlClientActive = false
            activeClientMode = .none
        }
    }

    private func deactivateControlClient() {
        if !controlClientActive && udp == nil && !sending { return }
        let closing = udp
        senderToken += 1
        sending = false
        controlClientActive = false
        touchHid = nil
        touchFrame = nil
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        udp = nil
        stopPhoneSensors()
        stopAllPhysicalRumble()
        if activeClientMode != .physical {
            clearPhysicalControllers()
        }
        activeClientMode = .none

        if let closing {
            sendQueue.async { [weak self] in
                guard let self else { return }
                // FLAG_DISCONNECT frees the server slot immediately (the UDP
                // equivalent of the old WS close); repeated since UDP is lossy.
                for _ in 0..<3 {
                    self.sendDisconnectFrame(to: closing)
                    Thread.sleep(forTimeInterval: 0.004)
                }
                closing.cancel()
            }
        }
    }

    private func disconnect() {
        deactivateControlClient()
        connected = false
        unlockInterfaceOrientation()
        setFullscreen(false)
        webView.loadHTMLString("", baseURL: nil)
        webView.removeFromSuperview()
        view.addSubview(connectView)
    }

    // Host[:port] -> UDP endpoint; strips any legacy ws:// scheme old builds
    // accepted. Default is the backend's controller port (7331), not the web
    // port: native clients no longer touch the WebSocket path.
    private func parseHostPort(_ raw: String) throws -> (NWEndpoint.Host, NWEndpoint.Port) {
        var text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { throw URLError(.badURL) }
        if let range = text.range(of: "://") { text = String(text[range.upperBound...]) }
        if let slash = text.firstIndex(of: "/") { text = String(text[..<slash]) }
        var hostPart = text
        var port = ProtocolWire.defaultUdpPort
        if !text.hasPrefix("["), text.filter({ $0 == ":" }).count == 1,
           let colon = text.lastIndex(of: ":") {
            let candidate = String(text[text.index(after: colon)...])
            if let value = UInt16(candidate), value > 0 {
                hostPart = String(text[..<colon])
                port = value
            }
        }
        guard !hostPart.isEmpty, let nwPort = NWEndpoint.Port(rawValue: port) else { throw URLError(.badURL) }
        return (NWEndpoint.Host(hostPart), nwPort)
    }

    // Authenticated UDP to the Raspberry Pi backend — the same low-latency
    // path as the desktop ns-client. UDP is connectionless: the server accepts
    // on the first signed input frame, so sending starts as soon as the local
    // socket is ready.
    private func connectUdp() -> Bool {
        do {
            let (endpointHost, endpointPort) = try parseHostPort(host)
            let params = NWParameters.udp
            params.serviceClass = .responsiveData // low-latency DSCP marking
            let connection = NWConnection(host: endpointHost, port: endpointPort, using: params)
            udp = connection
            connection.stateUpdateHandler = { [weak self, weak connection] state in
                DispatchQueue.main.async {
                    guard let self, let connection, self.udp === connection else { return }
                    switch state {
                    case .ready:
                        guard self.controlClientActive else { return }
                        self.statusLabel.text = "Connected"
                        if self.activeClientMode == .physical { self.updatePhysicalStatusOnPage(prefix: "Connected") }
                        self.lastNamesSentMs = 0 // sender loop announces names immediately
                        self.startSending()
                    case .failed, .waiting:
                        self.handleTransportClosed(text: "Connection failed")
                    default:
                        break
                    }
                }
            }
            receiveLoop(connection)
            connection.start(queue: udpQueue)
            return true
        } catch {
            statusLabel.text = "Invalid server address"
            return false
        }
    }

    // Server -> mobile feedback arrives on the same UDP socket: rumble (NSVR,
    // with the NSVH precision fallback whose low/high/duration bytes match)
    // and ClientAssignmentPacket refusals, which the WS close reason used to
    // deliver (server full / S2 profile unsupported).
    private func receiveLoop(_ connection: NWConnection) {
        connection.receiveMessage { [weak self, weak connection] data, _, _, error in
            guard let self, let connection else { return }
            guard self.udp === connection else { return }
            if let data, !data.isEmpty { self.handleFeedbackPacket(data) }
            if error == nil {
                self.receiveLoop(connection)
            } else {
                DispatchQueue.main.async { [weak self] in
                    guard let self, self.udp === connection else { return }
                    self.handleTransportClosed(text: "Connection failed")
                }
            }
        }
    }

    private func handleFeedbackPacket(_ data: Data) {
        guard data.count >= 8 else { return }
        let magic = ProtocolWire.readU32LE(data, 0)
        if (data.count == ProtocolWire.rumblePacketSize && magic == ProtocolWire.rumbleMagic)
            || (data.count == ProtocolWire.precisionRumblePacketSize && magic == ProtocolWire.precisionRumbleMagic) {
            handleRumblePacket(data)
            return
        }
        if data.count == ProtocolWire.clientAssignmentSize && magic == ProtocolWire.clientAssignmentMagic {
            let flags = Int(data[data.startIndex + 5])
            let message: String?
            if flags & ProtocolWire.assignmentFlagProfileUnsupported != 0 {
                message = "Switch 2 mode does not support Joy-Con L + R"
            } else if flags & ProtocolWire.assignmentFlagServerFull != 0 {
                message = "Server full"
            } else {
                message = nil
            }
            if let message {
                DispatchQueue.main.async { [weak self] in self?.handleTransportClosed(text: message) }
            }
        }
    }

    private func handleTransportClosed(text: String) {
        statusLabel.text = text
        senderToken += 1
        sending = false
        controlClientActive = false
        touchHid = nil
        touchFrame = nil
        lastTouchFrameMs = 0
        lastBridgeFrameParseMs = 0
        udp?.cancel()
        udp = nil
        stopPhoneSensors()
        stopAllPhysicalRumble()
        clearPhysicalControllers()
        activeClientMode = .none
        updatePhysicalStatusOnPage(prefix: text)
        let ac = UIAlertController(title: nil, message: text, preferredStyle: .alert)
        ac.addAction(UIAlertAction(title: "OK", style: .default))
        present(ac, animated: true)
        let escaped = jsEscape(text)
        let js = """
        (function(){
            if (window.__nsTouchDisconnected) window.__nsTouchDisconnected('\(escaped)');
            if (window.__nsMainDisconnected) window.__nsMainDisconnected('\(escaped)');
            window._connectionFailed = '\(escaped)' === 'Connection failed';
            var s=document.getElementById('statusText');
            if(s)s.innerText='\(escaped)';
            var btn=document.getElementById('btnConnect');
            if(btn){btn.innerText='Connect';btn.classList.remove('connected');btn.style.display='block';}
            var dot=document.getElementById('statusDot');
            if(dot)dot.style.display='none';
        })()
        """
        webView.evaluateJavaScript(js, completionHandler: nil)
    }

    private func startSending() {
        if sending { return }
        senderToken += 1
        let token = senderToken
        sending = true
        sendQueue.async { [weak self] in
            guard let self else { return }
            if self.activeClientMode == .touch { self.startPhoneSensors() }
            while self.sending && self.controlClientActive && self.senderToken == token {
                self.collectPhysicalMotionSamplesIfNeeded()
                self.sendFrame()
                let now = self.uptimeMs()
                if now - self.lastNamesSentMs >= 2000 {
                    self.lastNamesSentMs = now
                    self.sendNamesFrame()
                }
                Thread.sleep(forTimeInterval: 0.004)
            }
            DispatchQueue.main.async { [weak self] in
                guard let self, self.senderToken == token else { return }
                self.sending = false
                self.stopPhoneSensors()
                if self.activeClientMode != .physical { self.clearPhysicalControllers() }
            }
        }
    }

    private func phoneBatteryStatus() -> (percent: Int, charging: Bool)? {
        let level = UIDevice.current.batteryLevel
        guard level >= 0.0 else { return nil }
        let percent = max(0, min(100, Int((level * 100.0).rounded())))
        let state = UIDevice.current.batteryState
        let charging = state == .charging || state == .full
        return (percent, charging)
    }

    private func sendFrame() {
        guard let socket = udp else { return }
        let touchActive = controlClientActive && activeClientMode == .touch && currentPage == .touchControls
        let physicalActive = controlClientActive && activeClientMode == .physical && currentPage == .mainMenu
        let flags = touchActive ? ProtocolWire.flagSinglePad : 0
        var frame = ProtocolWire.initFrame(flags: flags, seq: nextSeq(), timestampUs: UInt64(Date().timeIntervalSince1970 * 1_000_000.0))

        if touchActive {
            let now = uptimeMs()
            let hid: [UInt8]
            if now - lastTouchFrameMs <= 500 {
                hid = touchHid ?? (touchFrame.flatMap { ProtocolWire.extractPad0Hid(from: $0) }) ?? ProtocolWire.neutralHid()
            } else {
                hid = ProtocolWire.neutralHid()
            }
            ProtocolWire.setFrameHid(&frame, pad: 0, hid: hid)
            frame[20 + 7] |= UInt8(touchExtraButtons)
            ProtocolWire.setFrameControllerType(&frame, pad: 0, controllerType: touchControllerType)
            if let batch = phoneMotionSamples() {
                ProtocolWire.setFrameMotionSamples(&frame, pad: 0, samples: batch.samples)
                ProtocolWire.setFrameMotionFresh(&frame, pad: 0, fresh: batch.fresh)
            }
            if let status = phoneBatteryStatus() {
                ProtocolWire.setFrameBatteryPercent(&frame, pad: 0, percent: status.percent, charging: status.charging)
            }
        } else if physicalActive {
            physicalPads.withLock { pads in
                for i in 0..<ProtocolWire.padCount {
                    let pad = pads[i]
                    guard pad.present else { continue }
                    ProtocolWire.setFrameHid(&frame, pad: i, hid: pad.hid())
                    ProtocolWire.setFrameControllerType(&frame, pad: i, controllerType: physicalControllerType)
                    if pad.hasMotion && pad.motionSampleCount >= ProtocolWire.motionSampleCount {
                        ProtocolWire.setFrameMotionSamples(&frame, pad: i, samples: pad.motionSamples)
                        ProtocolWire.setFrameMotionFresh(&frame, pad: i,
                                                         fresh: pad.motionRevision != pad.sentMotionRevision)
                        pad.sentMotionRevision = pad.motionRevision
                    }
                }
            }
        }

        // Hot path (250 Hz): sign and fire-and-forget. Transport failures
        // surface through the NWConnection state handler / receive loop.
        socket.send(content: ProtocolWire.signed(frame, authLen: ProtocolWire.frameAuthSize),
                    completion: .idempotent)
    }

    private func sendDisconnectFrame(to socket: NWConnection) {
        let frame = ProtocolWire.initFrame(flags: ProtocolWire.flagDisconnect, seq: nextSeq(), timestampUs: UInt64(Date().timeIntervalSince1970 * 1_000_000.0))
        socket.send(content: ProtocolWire.signed(frame, authLen: ProtocolWire.frameAuthSize),
                    completion: .contentProcessed { _ in })
    }

    private func sendNamesFrame() {
        guard let socket = udp else { return }
        var data = Data(count: 224)
        data[0] = 0x4E
        data[1] = 0x43
        data[2] = 0x53
        data[3] = 0x4E
        data[4] = 1 // version

        let touchActive = controlClientActive && activeClientMode == .touch && currentPage == .touchControls
        if touchActive {
            let off = 8
            data[off] = 1
            data[off + 1] = phoneSensorsActive ? 1 : 0
            let name = "iOS Controller"
            let nameBytes = Array(name.utf8.prefix(47))
            for (k, byte) in nameBytes.enumerated() {
                data[off + 2 + k] = byte
            }
        } else {
            physicalPads.withLock { pads in
                for i in 0..<4 {
                    let pad = pads[i]
                    let off = 8 + i * 50
                    data[off] = pad.present ? 1 : 0
                    data[off + 1] = pad.hasGyro ? 1 : 0
                    if pad.present {
                        let nameBytes = Array(pad.name.utf8.prefix(47))
                        for (k, byte) in nameBytes.enumerated() {
                            data[off + 2 + k] = byte
                        }
                    }
                }
            }
        }
        socket.send(content: ProtocolWire.signed(data, authLen: ProtocolWire.namesAuthSize),
                    completion: .contentProcessed { _ in })
    }

    private func nextSeq() -> UInt32 {
        let out = seq
        seq &+= 1
        return out
    }

    private func startPhoneSensors() {
        guard !phoneSensorsActive else { return }
        phoneMotion.withLock { state in
            state.count = 0
            state.samples = Array(repeating: ProtocolWire.neutralMotion(), count: ProtocolWire.motionSampleCount)
        }
        guard motionManager.isDeviceMotionAvailable else { return }
        motionManager.deviceMotionUpdateInterval = 1.0 / 200.0
        motionManager.startDeviceMotionUpdates(to: motionQueue) { [weak self] motion, _ in
            guard let self, let motion else { return }
            self.pushPhoneMotionSample(motion)
        }
        phoneSensorsActive = true
    }

    private func stopPhoneSensors() {
        guard phoneSensorsActive else { return }
        motionManager.stopDeviceMotionUpdates()
        phoneSensorsActive = false
        phoneMotion.withLock { state in state.count = 0 }
    }

    private func pushPhoneMotionSample(_ motion: CMDeviceMotion) {
        let gravity = motion.gravity
        let user = motion.userAcceleration
        let a0 = (x: Float(gravity.x + user.x),
                  y: Float(gravity.y + user.y),
                  z: Float(gravity.z + user.z))
        let r0 = motion.rotationRate

        // Pro Controller touch motion follows the locked landscape UI. A single
        // Joy-Con L or R is physically held vertically on top of the phone, with
        // both tops aligned, so Joy-Con motion stays in the phone's natural axes.
        let a = remapPhoneMotionForController(x: a0.x, y: a0.y, z: a0.z)
        let r = remapPhoneMotionForController(x: Float(r0.x), y: Float(r0.y), z: Float(r0.z))

        let sample = ProtocolWire.motionFromApple(accelX: a.0, accelY: a.1, accelZ: a.2,
                                                  rotationX: r.0, rotationY: r.1, rotationZ: r.2)
        phoneMotion.withLock { state in
            state.samples[0] = state.samples[1]
            state.samples[1] = state.samples[2]
            state.samples[2] = sample
            if state.count < ProtocolWire.motionSampleCount { state.count += 1 }
            state.revision &+= 1
        }
    }

    private func phoneMotionSamples() -> (samples: [[UInt8]], fresh: Bool)? {
        phoneMotion.withLock { state in
            guard state.count >= ProtocolWire.motionSampleCount else { return nil }
            let fresh = state.revision != state.sentRevision
            state.sentRevision = state.revision
            return (state.samples, fresh)
        }
    }

    private func remapPhoneMotionForController(x: Float, y: Float, z: Float) -> (Float, Float, Float) {
        switch touchControllerType {
        case ProtocolWire.controllerTypeJoyConL, ProtocolWire.controllerTypeJoyConR:
            // The phone is physically portrait like a vertical Joy-Con, directly
            // underneath it with both tops aligned. Ignore the landscape UI rotation.
            return (x, y, z)
        default:
            return remapForLandscapeTopOnLeft(x: x, y: y, z: z)
        }
    }

    private func remapForLandscapeTopOnLeft(x: Float, y: Float, z: Float) -> (Float, Float, Float) {
        // Same X/Y screen-space remap as Android Surface.ROTATION_270:
        // top/camera/notch on the left -> (y, -x, z).
        return (y, -x, z)
    }

    private func setupControllerObservers() {
        NotificationCenter.default.addObserver(self, selector: #selector(controllerChanged), name: .GCControllerDidConnect, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(controllerChanged), name: .GCControllerDidDisconnect, object: nil)
        GCController.startWirelessControllerDiscovery(completionHandler: nil)
    }

    @objc private func controllerChanged() {
        if activeClientMode == .physical {
            scanPhysicalControllers()
            updatePhysicalStatusOnPage()
            sendNamesFrame()
        }
    }

    private func scanPhysicalControllers() {
        let controllers = Array(GCController.controllers().prefix(ProtocolWire.padCount))

        physicalPads.withLock { pads in
            controllerSlots.removeAll()
            for pad in pads {
                if let motion = pad.controller?.motion, motion.sensorsRequireManualActivation {
                    motion.sensorsActive = false
                }
                pad.reset()
            }

            for (slot, controller) in controllers.enumerated() {
                let pad = pads[slot]
                pad.controller = controller
                pad.name = controller.vendorName ?? "Controller \(slot + 1)"
                pad.present = true
                if let motion = controller.motion {
                    if motion.sensorsRequireManualActivation { motion.sensorsActive = true }
                    pad.hasGyro = (motion.hasRotationRate || motion.hasAttitude)
                } else {
                    pad.hasGyro = false
                }
                pad.hasRumble = !(controller.haptics?.supportedLocalities.isEmpty ?? true)
                controllerSlots[ObjectIdentifier(controller)] = slot
            }
        }

        for controller in controllers {
            configureController(controller)
        }
    }

    private func clearPhysicalControllers() {
        controllerSlots.removeAll()
        physicalPads.withLock { pads in
            for pad in pads {
                if let motion = pad.controller?.motion, motion.sensorsRequireManualActivation {
                    motion.sensorsActive = false
                }
                pad.reset()
            }
        }
    }

    private func configureController(_ controller: GCController) {
        if let motion = controller.motion, motion.sensorsRequireManualActivation {
            motion.sensorsActive = true
        }

        controller.extendedGamepad?.valueChangedHandler = { [weak self, weak controller] gamepad, _ in
            guard let self, let controller else { return }
            self.updateGamepadState(controller: controller, gamepad: gamepad)
        }
        if let gamepad = controller.extendedGamepad {
            updateGamepadState(controller: controller, gamepad: gamepad)
        }
    }

    private func updateGamepadState(controller: GCController, gamepad: GCExtendedGamepad) {
        let id = ObjectIdentifier(controller)
        physicalPads.withLock { pads in
            guard let slot = controllerSlots[id], slot >= 0, slot < pads.count else { return }
            let pad = pads[slot]
            var buttons = 0
            // Physical-position mapping, same as SDL/Android: south->Switch B, east->A, west->Y, north->X.
            if gamepad.buttonA.isPressed { buttons |= ProtocolWire.btnB }
            if gamepad.buttonB.isPressed { buttons |= ProtocolWire.btnA }
            if gamepad.buttonX.isPressed { buttons |= ProtocolWire.btnY }
            if gamepad.buttonY.isPressed { buttons |= ProtocolWire.btnX }
            if gamepad.leftShoulder.isPressed { buttons |= ProtocolWire.btnL }
            if gamepad.rightShoulder.isPressed { buttons |= ProtocolWire.btnR }
            if gamepad.leftTrigger.value > 0.5 { buttons |= ProtocolWire.btnZL }
            if gamepad.rightTrigger.value > 0.5 { buttons |= ProtocolWire.btnZR }
            if gamepad.buttonOptions?.isPressed == true { buttons |= ProtocolWire.btnMinus }
            if gamepad.buttonMenu.isPressed { buttons |= ProtocolWire.btnPlus }
            if gamepad.buttonHome?.isPressed == true { buttons |= ProtocolWire.btnHome }
            if #available(iOS 12.1, *) {
                if gamepad.leftThumbstickButton?.isPressed == true { buttons |= ProtocolWire.btnLStick }
                if gamepad.rightThumbstickButton?.isPressed == true { buttons |= ProtocolWire.btnRStick }
            }
            pad.buttons = buttons
            pad.dpadUp = gamepad.dpad.up.isPressed
            pad.dpadDown = gamepad.dpad.down.isPressed
            pad.dpadLeft = gamepad.dpad.left.isPressed
            pad.dpadRight = gamepad.dpad.right.isPressed
            let leftX = gamepad.leftThumbstick.xAxis.value
            let leftY = gamepad.leftThumbstick.yAxis.value
            let rightX = gamepad.rightThumbstick.xAxis.value
            let rightY = gamepad.rightThumbstick.yAxis.value
            switch physicalControllerType {
            case ProtocolWire.controllerTypeJoyConL:
                let leftMagnitude = abs(leftX) + abs(leftY)
                let rightMagnitude = abs(rightX) + abs(rightY)
                let useRight = leftMagnitude < 0.001 && rightMagnitude >= 0.001
                pad.lx = ProtocolWire.axisToByte(useRight ? rightX : leftX)
                pad.ly = ProtocolWire.axisToByte(-(useRight ? rightY : leftY))
                pad.rx = 128
                pad.ry = 128
            case ProtocolWire.controllerTypeJoyConR:
                // Apple normally assigns a standalone R stick to rightThumbstick. A few
                // GameController mappings expose the sole stick as leftThumbstick, so use
                // whichever one is active while keeping the transmitted left slot neutral.
                let rightMagnitude = abs(rightX) + abs(rightY)
                let leftMagnitude = abs(leftX) + abs(leftY)
                let useLeft = rightMagnitude < 0.001 && leftMagnitude >= 0.001
                pad.lx = 128
                pad.ly = 128
                pad.rx = ProtocolWire.axisToByte(useLeft ? leftX : rightX)
                pad.ry = ProtocolWire.axisToByte(-(useLeft ? leftY : rightY))
            default:
                pad.lx = ProtocolWire.axisToByte(leftX)
                pad.ly = ProtocolWire.axisToByte(-leftY)
                pad.rx = ProtocolWire.axisToByte(rightX)
                pad.ry = ProtocolWire.axisToByte(-rightY)
            }
        }
    }

    private func collectPhysicalMotionSamplesIfNeeded() {
        guard activeClientMode == .physical else { return }
        physicalPads.withLock { pads in
            for pad in pads {
                guard pad.present, let motion = pad.controller?.motion else { continue }

                if motion.sensorsRequireManualActivation && !motion.sensorsActive {
                    motion.sensorsActive = true
                }
                guard motion.hasRotationRate || motion.hasAttitude else { continue }

                let accel: GCAcceleration
                if motion.hasGravityAndUserAcceleration {
                    // Apple says controllers may either expose separated gravity/user accel
                    // or only total acceleration. For Switch IMU packets we want accel too,
                    // so use total accel when possible: gravity + userAcceleration.
                    let gravity = motion.gravity
                    let user = motion.userAcceleration
                    accel = GCAcceleration(x: gravity.x + user.x, y: gravity.y + user.y, z: gravity.z + user.z)
                } else {
                    accel = motion.acceleration
                }

                let r = motion.rotationRate
                let sample = ProtocolWire.motionFromControllerApple(accelX: Float(accel.x),
                                                                            accelY: Float(accel.y),
                                                                            accelZ: Float(accel.z),
                                                                            rotationX: Float(r.x),
                                                                            rotationY: Float(r.y),
                                                                            rotationZ: Float(r.z))
                pad.motionSamples[0] = pad.motionSamples[1]
                pad.motionSamples[1] = pad.motionSamples[2]
                pad.motionSamples[2] = sample
                if pad.motionSampleCount < ProtocolWire.motionSampleCount { pad.motionSampleCount += 1 }
                pad.hasMotion = pad.motionSampleCount >= ProtocolWire.motionSampleCount
                pad.motionRevision &+= 1
            }
        }
    }

    private func updatePhysicalStatusOnPage(prefix: String? = nil) {
        guard currentPage == .mainMenu else { return }
        let lines = physicalPads.withLock { pads in
            (0..<ProtocolWire.padCount).map { i -> String in
                let pad = pads[i]
                if !pad.present { return "P\(i + 1): Empty" }
                return "P\(i + 1): \(pad.name)\(pad.hasGyro ? " + gyro" : "")"
            }
        }
        let status: String
        if let prefix { status = prefix }
        else {
            switch activeClientMode {
            case .physical: status = "Connected"
            case .touch: status = "Touch Controls running"
            case .none: status = "Ready"
            }
        }
        let connectText = (activeClientMode == .physical && controlClientActive) ? "Disconnect" : "Connect"
        var js = "(function(){"
        js += "var s=document.getElementById('statusText'); if(s)s.textContent='\(jsEscape(status))';"
        js += "var b=document.getElementById('btnConnect'); if(b)b.textContent='\(jsEscape(connectText))';"
        for i in 0..<ProtocolWire.padCount {
            js += "var p=document.getElementById('p\(i + 1)Text'); if(p)p.textContent='\(jsEscape(lines[i]))';"
        }
        js += "})()"
        webView.evaluateJavaScript(js, completionHandler: nil)
    }

    private func handleRumblePacket(_ data: Data) {
        let bytes = [UInt8](data)
        guard bytes.count == ProtocolWire.rumblePacketSize || bytes.count == ProtocolWire.precisionRumblePacketSize else { return }
        let magic = readU32LE(bytes, 0)
        guard magic == ProtocolWire.rumbleMagic || magic == ProtocolWire.precisionRumbleMagic else { return }
        let subpad = Int(bytes[4])
        let low = Int(bytes[5])
        let high = Int(bytes[6])
        let duration10Ms = Int(bytes[7])
        routeRumble(subpad: subpad, low: low, high: high, duration10Ms: duration10Ms)
    }

    private func routeRumble(subpad: Int, low: Int, high: Int, duration10Ms: Int) {
        guard controlClientActive else { return }
        switch activeClientMode {
        case .physical:
            physicalRumble(subpad: subpad, low: low, high: high, duration10Ms: duration10Ms)
        case .touch, .none:
            break
        }
    }

    private func physicalRumble(subpad: Int, low: Int, high: Int, duration10Ms: Int) {
        guard subpad >= 0 && subpad < ProtocolWire.padCount else { return }
        let now = uptimeMs()
        let neutral = (low == 0 && high == 0) || duration10Ms == 0
        physicalPads.withLock { pads in
            guard subpad < pads.count, pads[subpad].present else { return }
            if neutral {
                pads[subpad].rumbleLow = 0
                pads[subpad].rumbleHigh = 0
                pads[subpad].rumbleUntilMs = 0
                pads[subpad].rumbleLastSetMs = now
                pads[subpad].stopRumble()
                return
            }
            let duration = UInt64(max(40, min(max(duration10Ms, 1), 255) * 10))
            if pads[subpad].rumbleLow == low && pads[subpad].rumbleHigh == high && now - pads[subpad].rumbleLastSetMs < 100 {
                pads[subpad].rumbleUntilMs = now + duration
                return
            }
            pads[subpad].rumbleLow = low
            pads[subpad].rumbleHigh = high
            pads[subpad].rumbleUntilMs = now + duration
            pads[subpad].rumbleLastSetMs = now
            playControllerHaptic(pad: &pads[subpad], low: low, high: high, durationMs: duration)
        }
    }

    private func playControllerHaptic(pad: inout PhysicalPad, low: Int, high: Int, durationMs: UInt64) {
        guard let haptics = pad.controller?.haptics else { return }
        let supported = haptics.supportedLocalities
        guard !supported.isEmpty else { return }

        do {
            if pad.hapticEngine == nil {
                let preferred: [GCHapticsLocality] = [.all, .default, .leftHandle, .rightHandle]
                let locality = preferred.first { supported.contains($0) } ?? supported.first!
                guard let candidate = haptics.createEngine(withLocality: locality) else { return }
                candidate.resetHandler = { [weak candidate] in
                    try? candidate?.start()
                }
                try candidate.start()
                pad.hapticEngine = candidate
            }
            guard let engine = pad.hapticEngine else { return }

            try? pad.hapticPlayer?.stop(atTime: CHHapticTimeImmediate)
            pad.hapticPlayer = nil

            let strength = Float(max(low, high)) / 255.0
            let intensity = CHHapticEventParameter(parameterID: .hapticIntensity, value: max(0.04, strength))
            let sharpness = CHHapticEventParameter(parameterID: .hapticSharpness, value: min(1.0, max(0.12, Float(high) / 255.0)))
            let event = CHHapticEvent(eventType: .hapticContinuous, parameters: [intensity, sharpness], relativeTime: 0, duration: Double(durationMs) / 1000.0)
            let pattern = try CHHapticPattern(events: [event], parameters: [])
            let player = try engine.makePlayer(with: pattern)
            try player.start(atTime: CHHapticTimeImmediate)
            pad.hapticPlayer = player
        } catch {
            pad.hapticEngine = nil
        }
    }

    private func stopAllPhysicalRumble() {
        physicalPads.withLock { pads in pads.forEach { $0.cleanupRumble() } }
    }

    private func readU32LE(_ bytes: [UInt8], _ off: Int) -> UInt32 {
        guard off + 3 < bytes.count else { return 0 }
        return UInt32(bytes[off]) |
            (UInt32(bytes[off + 1]) << 8) |
            (UInt32(bytes[off + 2]) << 16) |
            (UInt32(bytes[off + 3]) << 24)
    }

    private func uptimeMs() -> UInt64 {
        UInt64(ProcessInfo.processInfo.systemUptime * 1000.0)
    }

    private func jsEscape(_ v: String) -> String {
        v.replacingOccurrences(of: "\\", with: "\\\\")
            .replacingOccurrences(of: "'", with: "\\'")
            .replacingOccurrences(of: "\n", with: " ")
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
        webView.configuration.userContentController.removeScriptMessageHandler(forName: "NSBridge")
    }
}

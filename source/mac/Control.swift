import AppKit
import ApplicationServices
import AVFoundation
import CoreGraphics
import CoreMedia
import CoreVideo
import CryptoKit
import Darwin
import Foundation
import OSLog
import ScreenCaptureKit
import Security

struct Pairing: Decodable {
    let host: String
    let key: String
    var secret: SymmetricKey? {
        let hex = Array(key)
        guard hex.count == 64 else { return nil }
        var data = [UInt8]()
        for index in stride(from: 0, to: 64, by: 2) {
            guard let value = UInt8(String(hex[index...index + 1]), radix: 16) else { return nil }
            data.append(value)
        }
        return SymmetricKey(data: data)
    }
}

struct ScreenFrame {
    let width: Int
    let height: Int
    let bytes: Data
}

enum ViewMode: Int, CaseIterable {
    case text = 6
    case follow = 3
    case full = 1

    var title: String {
        switch self {
        case .text: return "文字模式（跟随光标 · 6 倍）"
        case .follow: return "跟随光标（3 倍）"
        case .full: return "全屏画面"
        }
    }
}

struct CapturedScreen {
    let pixel: CVPixelBuffer
    let bounds: CGRect
    let width: Int
    let height: Int

    func render(cursor: CGPoint?, mode: ViewMode) -> ScreenFrame? {
        guard CVPixelBufferLockBaseAddress(pixel, .readOnly) == kCVReturnSuccess else { return nil }
        defer { CVPixelBufferUnlockBaseAddress(pixel, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(pixel)?.assumingMemoryBound(to: UInt8.self) else { return nil }
        let sourceWidth = CVPixelBufferGetWidth(pixel)
        let sourceHeight = CVPixelBufferGetHeight(pixel)
        let stride = CVPixelBufferGetBytesPerRow(pixel)

        let point = cursor ?? CGPoint(x: bounds.midX, y: bounds.midY)
        let relativeX = min(1, max(0, (point.x - bounds.minX) / bounds.width))
        let relativeY = min(1, max(0, (point.y - bounds.minY) / bounds.height))
        let pointerX = relativeX * CGFloat(sourceWidth)
        let pointerY = relativeY * CGFloat(sourceHeight)
        let zoom = mode.rawValue
        let cropWidth = max(1, sourceWidth / zoom)
        let cropHeight = max(1, sourceHeight / zoom)
        let left = min(sourceWidth - cropWidth, max(0, Int(pointerX) - cropWidth / 2))
        let top = min(sourceHeight - cropHeight, max(0, Int(pointerY) - cropHeight / 2))

        var rgb = Data(count: width * height * 2)
        rgb.withUnsafeMutableBytes { (bytes: UnsafeMutableRawBufferPointer) in
            for y in 0..<height {
                let sourceRow = (top + (2 * y + 1) * cropHeight / (2 * height)) * stride
                for x in 0..<width {
                    let p = sourceRow + (left + (2 * x + 1) * cropWidth / (2 * width)) * 4
                    let out = (y * width + x) * 2
                    bytes[out] = (base[p + 2] & 0xf8) | (base[p + 1] >> 5)
                    bytes[out + 1] = ((base[p + 1] << 3) & 0xe0) | (base[p] >> 3)
                }
            }

            // Draw the current pointer even when the captured desktop is idle.
            if let cursor, bounds.contains(cursor) {
                let x = Int((pointerX - CGFloat(left)) * CGFloat(width) / CGFloat(cropWidth))
                let y = Int((pointerY - CGFloat(top)) * CGFloat(height) / CGFloat(cropHeight))
                for dy in -5...5 {
                    for dx in -5...5 where abs(dx) <= 1 || abs(dy) <= 1 {
                        let px = x + dx, py = y + dy
                        guard (0..<width).contains(px), (0..<height).contains(py) else { continue }
                        let out = (py * width + px) * 2
                        let white: UInt8 = (dx == 0 && abs(dy) < 5) || (dy == 0 && abs(dx) < 5) ? 255 : 0
                        bytes[out] = white
                        bytes[out + 1] = white
                    }
                }
            }
        }
        return ScreenFrame(width: width, height: height, bytes: rgb)
    }
}

final class FrameStore: @unchecked Sendable {
    private let lock = NSLock()
    private let testFrame: ScreenFrame?
    private let cursorEnabled: Bool
    private var capture: CapturedScreen?
    private var mode: ViewMode = .text
    private var lastZoomMode: ViewMode = .text

    init(testFrame: ScreenFrame? = nil, cursorEnabled: Bool = true) {
        self.testFrame = testFrame
        self.cursorEnabled = cursorEnabled
    }

    func setCapture(_ value: CapturedScreen?) {
        lock.lock()
        capture = value
        lock.unlock()
    }

    func setMode(_ value: ViewMode) {
        lock.lock()
        mode = value
        if value != .full { lastZoomMode = value }
        lock.unlock()
    }

    var activeMode: ViewMode {
        lock.lock()
        defer { lock.unlock() }
        return mode
    }

    func toggleFullscreen() -> ViewMode {
        lock.lock()
        defer { lock.unlock() }
        mode = mode == .full ? lastZoomMode : .full
        return mode
    }

    var hasFrame: Bool {
        lock.lock()
        defer { lock.unlock() }
        return testFrame != nil || capture != nil
    }

    func get(cursor: CGPoint? = CGEvent(source: nil)?.location) -> ScreenFrame? {
        if let testFrame { return testFrame }
        lock.lock()
        let latest = capture
        let activeMode = mode
        lock.unlock()
        return latest?.render(cursor: cursorEnabled ? cursor : nil, mode: activeMode)
    }
}

func receive(_ fd: Int32, count: Int) -> Data? {
    var bytes = [UInt8](repeating: 0, count: count)
    var offset = 0
    while offset < count {
        let n = bytes.withUnsafeMutableBytes { recv(fd, $0.baseAddress!.advanced(by: offset), count - offset, 0) }
        if n < 0 && errno == EINTR { continue }
        if n <= 0 { return nil }
        offset += n
    }
    return Data(bytes)
}

func transmit(_ fd: Int32, _ bytes: Data) -> Bool {
    bytes.withUnsafeBytes { buffer in
        var offset = 0
        while offset < buffer.count {
            let n = send(fd, buffer.baseAddress!.advanced(by: offset), buffer.count - offset, 0)
            if n < 0 && errno == EINTR { continue }
            if n <= 0 { return false }
            offset += n
        }
        return true
    }
}

extension Data {
    mutating func put<T: FixedWidthInteger>(_ value: T) {
        var network = value.bigEndian
        Swift.withUnsafeBytes(of: &network) { append(contentsOf: $0) }
    }
}

func openInputPacket(_ packet: Data, key: SymmetricKey, expected: inout UInt32) -> (UInt8, UInt32)? {
    guard packet.count == 25 else { return nil }
    let header = Data(packet.prefix(4))
    let sequence = header.reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
    guard sequence == expected, expected != .max else { return nil }
    var nonce = Data(repeating: 0, count: 8)
    nonce.append(header)
    guard let iv = try? AES.GCM.Nonce(data: nonce),
          let box = try? AES.GCM.SealedBox(nonce: iv, ciphertext: packet[4..<9], tag: packet[9..<25]),
          let body = try? AES.GCM.open(box, using: key, authenticating: header),
          let kind = body.first else { return nil }
    expected += 1
    let value = body.dropFirst().reduce(UInt32(0)) { ($0 << 8) | UInt32($1) }
    return (kind, value)
}

func tcpListener(host: String, port: UInt16) throws -> Int32 {
    let listener = socket(AF_INET, SOCK_STREAM, 0)
    guard listener >= 0 else { throw POSIXError(.EIO) }
    var yes: Int32 = 1
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, socklen_t(MemoryLayout<Int32>.size))
    var addr = sockaddr_in()
    addr.sin_family = sa_family_t(AF_INET)
    addr.sin_port = port.bigEndian
    guard inet_pton(AF_INET, host, &addr.sin_addr) == 1 else {
        close(listener)
        throw POSIXError(.EINVAL)
    }
    let rc = withUnsafePointer(to: &addr) {
        $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(listener, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) }
    }
    guard rc == 0 && listen(listener, 3) == 0 else {
        let failure = POSIXError(POSIXErrorCode(rawValue: errno) ?? .EIO)
        close(listener)
        throw failure
    }
    return listener
}

final class ScreenServer: @unchecked Sendable {
    private let logger = Logger(subsystem: "local.ai-passport.live-control", category: "view")
    private let toggleFullscreenInput: UInt8 = 4
    let host: String
    let secret: SymmetricKey
    let frames: FrameStore
    let test: Bool
    let camera: PassportCamera?
    init(host: String, secret: SymmetricKey, frames: FrameStore, test: Bool, camera: PassportCamera? = nil) {
        self.host = host; self.secret = secret; self.frames = frames; self.test = test
        self.camera = camera
    }

    func run() throws {
        let listener = try tcpListener(host: host, port: camera == nil ? 45902 : 45904)
        defer { close(listener) }
        while true {
            let peer = accept(listener, nil, nil)
            if peer < 0 && errno == EINTR { continue }
            if peer < 0 { throw POSIXError(.EIO) }
            serve(peer)
            close(peer)
        }
    }

    private func serve(_ peer: Int32) {
        var yes: Int32 = 1
        var limit = timeval(tv_sec: 3, tv_usec: 0)
        setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &yes, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &limit, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, &limit, socklen_t(MemoryLayout<timeval>.size))
        let protocolName = camera == nil ? "APSC2" : "APCM2"
        guard let hello = receive(peer, count: 21), hello.prefix(5) == Data(protocolName.utf8) else { return }
        var serverNonce = [UInt8](repeating: 0, count: 16)
        guard SecRandomCopyBytes(kSecRandomDefault, serverNonce.count, &serverNonce) == errSecSuccess,
              transmit(peer, Data(serverNonce)) else { return }
        let key = SymmetricKey(data: Data(HMAC<SHA256>.authenticationCode(for: hello + Data(serverNonce), using: secret)))
        guard let proof = receive(peer, count: 32),
              HMAC<SHA256>.isValidAuthenticationCode(proof, authenticating: Data("client".utf8), using: key) else { return }
        if test { print("Authenticated device", terminator: "\n"); fflush(stdout) }
        let inputKey = SymmetricKey(data: Data(HMAC<SHA256>.authenticationCode(for: Data("input".utf8), using: key)))
        var pending = Data()
        var expectedInput: UInt32 = 1
        var cameraLease: UUID?
        if let camera {
            guard let packet = receive(peer, count: 25),
                  let (kind, value) = openInputPacket(packet, key: inputKey, expected: &expectedInput),
                  kind == 5, value <= 1 else { return }
            cameraLease = camera.beginSession(enabled: value == 1)
        }
        defer { if let cameraLease { camera?.endSession(cameraLease) } }
        var sequence: UInt32 = 1
        var sentFrames = 0
        var lastFrameAt = ProcessInfo.processInfo.systemUptime
        while true {
            let started = ProcessInfo.processInfo.systemUptime
            if camera != nil || frames.hasFrame {
                guard readInputs(peer, inputKey, pending: &pending, expected: &expectedInput, cameraLease: cameraLease) else { return }
            }
            if let camera {
                guard sendMonitorState(camera.state.rawValue, fd: peer, key: key, sequence: &sequence),
                      sendMonitorState(16 + camera.monitorAudioState.rawValue, fd: peer, key: key, sequence: &sequence) else { return }
            }
            if let frame = frames.get() {
                guard sendFrame(frame, fd: peer, key: key, sequence: &sequence) else { return }
                lastFrameAt = started
                sentFrames += 1
                if test && sentFrames == 30 { print("Sent 30 encrypted frames"); fflush(stdout) }
            } else if camera == nil && started - lastFrameAt >= 3 { return }
            Thread.sleep(forTimeInterval: max(0, 0.125 - (ProcessInfo.processInfo.systemUptime - started)))
        }
    }

    private func readInputs(_ peer: Int32, _ key: SymmetricKey, pending: inout Data,
                            expected: inout UInt32, cameraLease: UUID?) -> Bool {
        var bytes = [UInt8](repeating: 0, count: 512)
        let length = bytes.withUnsafeMutableBytes { recv(peer, $0.baseAddress, $0.count, MSG_DONTWAIT) }
        if length < 0 { return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR }
        if length == 0 { return false }
        pending.append(contentsOf: bytes.prefix(length))
        while pending.count >= 25 {
            let packet = Data(pending.prefix(25))
            pending.removeFirst(25)
            guard let (kind, value) = openInputPacket(packet, key: key, expected: &expected) else { return false }
            if let camera, let cameraLease {
                if kind == 5 && value <= 1 { camera.setEnabled(value == 1, lease: cameraLease) }
                else if kind == 6 && value <= 2 { camera.setAudio(value, lease: cameraLease) }
                else { return false }
            } else if kind == toggleFullscreenInput && value == 0 {
                let mode = frames.toggleFullscreen()
                logger.notice("Device view switched to zoom factor \(mode.rawValue)")
            } else if test {
                print("Authenticated input event type \(kind)"); fflush(stdout)
            } else if AXIsProcessTrusted() && frames.hasFrame {
                sendMacInput(kind: kind, value: value)
            }
        }
        return true
    }

    private func sendMonitorState(_ state: UInt16, fd: Int32, key: SymmetricKey,
                                  sequence: inout UInt32) -> Bool {
        guard sequence != .max else { return false }
        var header = Data()
        header.put(UInt16(0)); header.put(state)
        header.put(UInt16(0)); header.put(UInt16(0)); header.put(sequence)
        var nonce = Data(repeating: 0, count: 8)
        nonce.put(sequence)
        guard let iv = try? AES.GCM.Nonce(data: nonce),
              let box = try? AES.GCM.seal(Data(), using: key, nonce: iv, authenticating: header),
              transmit(fd, header + box.tag) else { return false }
        sequence += 1
        return true
    }

    private func sendMacInput(kind: UInt8, value: UInt32) {
        let source = CGEventSource(stateID: .hidSystemState)
        if kind == 1 || kind == 2 {
            let bounds = CGDisplayBounds(CGMainDisplayID())
            let x = bounds.minX + Double(value >> 16) / 65535 * max(0, bounds.width - 1)
            let y = bounds.minY + Double(value & 0xffff) / 65535 * max(0, bounds.height - 1)
            let point = CGPoint(x: x, y: y)
            let types: [CGEventType] = kind == 1 ? [.mouseMoved] : [.leftMouseDown, .leftMouseUp]
            for type in types {
                let event = CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: point, mouseButton: .left)
                if kind == 2 { event?.setIntegerValueField(.mouseEventClickState, value: 1) }
                event?.post(tap: .cghidEventTap)
            }
        } else if kind == 3 {
            let special: [UInt32: CGKeyCode] = [0xff08: 51, 0xff09: 48, 0xff0d: 36]
            guard special[value] != nil || (32...126).contains(value) else { return }
            for down in [true, false] {
                let event = CGEvent(keyboardEventSource: source, virtualKey: special[value] ?? 0, keyDown: down)
                if special[value] == nil {
                    var character = UniChar(value)
                    event?.keyboardSetUnicodeString(stringLength: 1, unicodeString: &character)
                }
                event?.post(tap: .cghidEventTap)
            }
        }
    }

    private func sendFrame(_ frame: ScreenFrame, fd: Int32, key: SymmetricKey, sequence: inout UInt32) -> Bool {
        for y in stride(from: 0, to: frame.height, by: 8) {
            guard sequence != .max else { return false }
            let rows = min(8, frame.height - y)
            var header = Data()
            header.put(UInt16(frame.width)); header.put(UInt16(frame.height))
            header.put(UInt16(y)); header.put(UInt16(rows)); header.put(sequence)
            var nonce = Data(repeating: 0, count: 8)
            nonce.put(sequence)
            let start = y * frame.width * 2
            let end = start + rows * frame.width * 2
            guard let iv = try? AES.GCM.Nonce(data: nonce),
                  let box = try? AES.GCM.seal(frame.bytes[start..<end], using: key, nonce: iv, authenticating: header),
                  transmit(fd, header + box.ciphertext + box.tag) else { return false }
            sequence += 1
        }
        return true
    }
}

final class Capture: NSObject, SCStreamOutput, SCStreamDelegate {
    let frames: FrameStore
    let bounds: CGRect
    let width: Int
    let height: Int
    let systemAudio: VoicePCMConverter

    init(frames: FrameStore, bounds: CGRect, width: Int, height: Int, voice: VoiceTransport) {
        self.frames = frames
        self.bounds = bounds
        self.width = width
        self.height = height
        systemAudio = VoicePCMConverter(transport: voice, source: .system)
    }

    func stream(_ stream: SCStream, didStopWithError error: any Error) { frames.setCapture(nil) }
    func stream(_ stream: SCStream, didOutputSampleBuffer sample: CMSampleBuffer, of type: SCStreamOutputType) {
        if type == .audio {
            systemAudio.feed(sample)
            return
        }
        guard type == .screen, sample.isValid,
              let attachments = (CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]])?.first,
              let raw = attachments[.status] as? Int, let status = SCFrameStatus(rawValue: raw) else { return }
        if status != .complete {
            if status != .idle { frames.setCapture(nil) }
            return
        }
        guard let pixel = CMSampleBufferGetImageBuffer(sample) else { return }
        frames.setCapture(CapturedScreen(pixel: pixel, bounds: bounds, width: width, height: height))
    }
}

@MainActor final class MirrorApp: NSObject, NSApplicationDelegate, NSMenuDelegate {
    let pairing: Pairing
    let secret: SymmetricKey
    let frames = FrameStore()
    var stream: SCStream?
    var capture: Capture?
    var starting = false
    var item: NSStatusItem?
    var state: NSMenuItem?
    var modeItems: [NSMenuItem] = []
    let voice = VoiceTransport()
    lazy var voiceOutput = VoiceOutput(transport: voice)
    var voiceItems: [NSMenuItem] = []
    var captureConfiguration: SCStreamConfiguration?
    var systemOutputReady = false
    var changingVoice = false
    var choseVoiceMode = false
    let camera = PassportCamera()
    let monitorVoice = VoiceTransport()
    lazy var monitorVoiceOutput = VoiceOutput(transport: monitorVoice)
    init(pairing: Pairing, secret: SymmetricKey) { self.pairing = pairing; self.secret = secret }

    func applicationDidFinishLaunching(_ notification: Notification) {
        let bar = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        bar.button?.title = "Passport"
        let menu = NSMenu()
        menu.delegate = self
        let status = NSMenuItem(title: "正在启动", action: nil, keyEquivalent: "")
        menu.addItem(status)
        menu.addItem(.separator())

        let displayMenu = addCategory("画面", to: menu)
        for mode in ViewMode.allCases {
            let option = NSMenuItem(title: mode.title, action: #selector(selectMode(_:)), keyEquivalent: "")
            option.target = self
            option.tag = mode.rawValue
            option.state = mode == .text ? .on : .off
            displayMenu.addItem(option)
            modeItems.append(option)
        }
        let audioMenu = addCategory("语音", to: menu)
        for mode in VoiceMode.allCases {
            let option = NSMenuItem(title: mode.title, action: #selector(selectVoice(_:)), keyEquivalent: "")
            option.target = self
            option.tag = mode.rawValue
            voiceItems.append(option)
            audioMenu.addItem(option)
        }

        let connectionMenu = addCategory("连接与权限", to: menu)
        let retry = NSMenuItem(title: "开始或重试镜像", action: #selector(start), keyEquivalent: "")
        retry.target = self
        connectionMenu.addItem(retry)
        let grant = NSMenuItem(title: "授权鼠标和键盘控制", action: #selector(requestControl), keyEquivalent: "")
        grant.target = self
        connectionMenu.addItem(grant)
        let monitorMenu = addCategory("远程监控", to: menu)
        let closeCameraItem = NSMenuItem(title: "关闭摄像头", action: #selector(stopCamera), keyEquivalent: "")
        closeCameraItem.target = self
        monitorMenu.addItem(closeCameraItem)
        menu.addItem(.separator())
        let quit = NSMenuItem(title: "退出镜像", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        menu.addItem(quit)
        bar.menu = menu
        item = bar; state = status
        camera.onAudioRequest = { [weak self] mode, revision in
            Task { @MainActor in await self?.configureMonitorAudio(mode, revision: revision) }
        }
        let server = ScreenServer(host: pairing.host, secret: secret, frames: frames, test: false)
        DispatchQueue.global(qos: .userInitiated).async {
            do { try server.run() }
            catch { DispatchQueue.main.async { self.show("镜像发送端无法监听", String(describing: error)) } }
        }
        let monitor = ScreenServer(host: pairing.host, secret: secret, frames: camera.frames, test: false, camera: camera)
        DispatchQueue.global(qos: .userInitiated).async {
            do { try monitor.run() }
            catch { DispatchQueue.main.async { self.show("远程监控无法监听", error.localizedDescription) } }
        }
        let voiceServer = VoiceServer(host: pairing.host, secret: secret, transport: voice) { [weak self] pcm in
            DispatchQueue.main.async { [weak self] in self?.voiceOutput.play(pcm) }
        }
        DispatchQueue.global(qos: .userInitiated).async {
            do { try voiceServer.run() }
            catch { DispatchQueue.main.async { self.show("语音发送端无法监听", error.localizedDescription) } }
        }
        let monitorAudioServer = VoiceServer(host: pairing.host, secret: secret, transport: monitorVoice, port: 45905) { [weak self] pcm in
            DispatchQueue.main.async { [weak self] in
                guard let self, self.camera.monitorAudioMode == 2 else { return }
                self.monitorVoiceOutput.play(pcm)
            }
        }
        DispatchQueue.global(qos: .userInitiated).async {
            do { try monitorAudioServer.run() }
            catch { DispatchQueue.main.async { self.show("监控音频无法监听", error.localizedDescription) } }
        }
        do {
            try voiceOutput.start(microphone: false)
            voice.select(.listen)
        } catch {
            Logger(subsystem: "local.ai-passport.live-control", category: "voice")
                .error("Could not start board audio playback: \(error.localizedDescription, privacy: .public)")
        }
        start()
    }

    @objc func stopCamera() { camera.turnOff() }

    func applicationWillTerminate(_ notification: Notification) {
        monitorVoice.select(.off)
        monitorVoiceOutput.stop()
        camera.shutdown()
    }

    private func configureMonitorAudio(_ mode: UInt32, revision: UInt64) async {
        guard camera.audioRequestIsCurrent(mode, revision: revision) else { return }
        if mode == 0 {
            monitorVoice.select(.off)
            monitorVoiceOutput.stop()
            camera.completeAudio(.off, revision: revision)
            return
        }
        let allowed = await AVCaptureDevice.requestAccess(for: .audio)
        guard camera.audioRequestIsCurrent(mode, revision: revision) else { return }
        guard allowed else {
            monitorVoice.select(.off)
            monitorVoiceOutput.stop()
            camera.completeAudio(.permissionRequired, revision: revision)
            return
        }
        do {
            if monitorVoice.currentMode != .microphone {
                try monitorVoiceOutput.start(microphone: true)
                monitorVoice.select(.microphone)
            }
            camera.completeAudio(mode == 1 ? .listening : .talking, revision: revision)
        } catch {
            monitorVoice.select(.off)
            monitorVoiceOutput.stop()
            camera.completeAudio(.failed, revision: revision)
        }
    }

    private func addCategory(_ title: String, to menu: NSMenu) -> NSMenu {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        let submenu = NSMenu(title: title)
        submenu.delegate = self
        item.submenu = submenu
        menu.addItem(item)
        return submenu
    }

    @objc func selectMode(_ sender: NSMenuItem) {
        guard let mode = ViewMode(rawValue: sender.tag) else { return }
        frames.setMode(mode)
    }

    func menuWillOpen(_ menu: NSMenu) {
        let activeMode = frames.activeMode
        for option in modeItems {
            option.state = option.tag == activeMode.rawValue ? .on : .off
        }
        let currentVoice = voice.currentMode
        for option in voiceItems {
            option.state = option.tag == currentVoice.rawValue ? .on : .off
        }
    }

    @objc func selectVoice(_ sender: NSMenuItem) {
        guard !changingVoice, let requested = VoiceMode(rawValue: sender.tag) else { return }
        choseVoiceMode = true
        changeVoice(to: requested)
    }

    private func changeVoice(to requested: VoiceMode) {
        guard requested != voice.currentMode else { return }
        if requested == .system && stream == nil {
            show("请先启动桌面镜像", "系统声音使用同一个屏幕采集会话。")
            return
        }
        changingVoice = true
        Task { @MainActor in
            defer { changingVoice = false }
            if requested == .microphone {
                let allowed = await AVCaptureDevice.requestAccess(for: .audio)
                guard allowed else {
                    show("需要麦克风授权", "请在「系统设置 → 隐私与安全性 → 麦克风」中开启 MaToy。")
                    return
                }
            }
            voice.select(.off)
            voiceOutput.stop()
            do {
                if let stream, let configuration = captureConfiguration {
                    let captureSystem = requested == .system
                    if captureSystem, !systemOutputReady, let capture {
                        try stream.addStreamOutput(capture, type: .audio, sampleHandlerQueue: DispatchQueue(label: "passport.system-audio"))
                        systemOutputReady = true
                    }
                    if configuration.capturesAudio != captureSystem {
                        configuration.capturesAudio = captureSystem
                        try await stream.updateConfiguration(configuration)
                    }
                }
                if requested != .off {
                    try voiceOutput.start(microphone: requested == .microphone)
                    voice.select(requested)
                }
            } catch {
                if let stream, let configuration = captureConfiguration {
                    configuration.capturesAudio = false
                    try? await stream.updateConfiguration(configuration)
                }
                if requested == .system, (try? voiceOutput.start(microphone: false)) != nil {
                    voice.select(.listen)
                }
                show("无法启用语音", error.localizedDescription)
            }
        }
    }

    private func show(_ title: String, _ text: String) {
        state?.title = title
        let alert = NSAlert()
        alert.messageText = title; alert.informativeText = text
        NSApp.activate(ignoringOtherApps: true)
        alert.runModal()
    }

    @objc func requestControl() {
        let prompt = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
        if !AXIsProcessTrustedWithOptions([prompt: true] as CFDictionary) {
            state?.title = "等待辅助功能授权"
        } else {
            state?.title = "鼠标和键盘控制已授权"
        }
    }

    @objc func start() {
        guard !starting, stream == nil else { return }
        guard CGPreflightScreenCaptureAccess() || CGRequestScreenCaptureAccess() else {
            state?.title = "桌面需要屏幕录制授权"
            Logger(subsystem: "local.ai-passport.live-control", category: "view")
                .notice("Desktop capture is waiting for Screen Recording permission")
            return
        }
        starting = true
        Task { @MainActor in
            defer { starting = false }
            do {
                let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
                guard let display = content.displays.first(where: { $0.displayID == CGMainDisplayID() }) ?? content.displays.first else {
                    show("没有可用屏幕", "打开 Mac 屏幕后，再从菜单栏 Passport 菜单重试。")
                    return
                }
                let ratio = min(320.0 / Double(display.width), 276.0 / Double(display.height))
                let width = max(2, Int(Double(display.width) * ratio) & ~1)
                let height = max(2, Int(Double(display.height) * ratio) & ~1)
                let configuration = SCStreamConfiguration()
                configuration.width = width * ViewMode.text.rawValue
                configuration.height = height * ViewMode.text.rawValue
                configuration.pixelFormat = kCVPixelFormatType_32BGRA
                configuration.minimumFrameInterval = CMTime(value: 1, timescale: 8)
                configuration.queueDepth = 3
                configuration.showsCursor = false
                configuration.sampleRate = 16_000
                configuration.channelCount = 1
                configuration.excludesCurrentProcessAudio = true
                let output = Capture(frames: frames, bounds: CGDisplayBounds(display.displayID), width: width, height: height, voice: voice)
                let filter = SCContentFilter(display: display, excludingWindows: [])
                let live = SCStream(filter: filter, configuration: configuration, delegate: output)
                try live.addStreamOutput(output, type: .screen, sampleHandlerQueue: DispatchQueue(label: "passport.screen"))
                try await live.startCapture()
                capture = output; stream = live
                captureConfiguration = configuration
                if !choseVoiceMode && !changingVoice {
                    changeVoice(to: .system)
                }
                state?.title = "正在镜像主屏幕"
                requestControl()
            } catch {
                frames.setCapture(nil)
                state?.title = "桌面采集暂不可用"
                Logger(subsystem: "local.ai-passport.live-control", category: "view")
                    .error("Desktop capture could not start: \(error.localizedDescription, privacy: .public)")
            }
        }
    }
}

@main struct Main {
    @MainActor static func main() throws {
        let root = Bundle.main.bundleURL.deletingLastPathComponent().deletingLastPathComponent()
        let config = root.appendingPathComponent("work/mac-screen-pair.json")
        let pairing = try JSONDecoder().decode(Pairing.self, from: Data(contentsOf: config))
        guard let secret = pairing.secret else { throw POSIXError(.EINVAL) }
        if CommandLine.arguments.contains("--test-pattern") {
            let bytes = Data((0..<(240 * 150)).flatMap { i -> [UInt8] in i % 240 < 120 ? [0xf8, 0] : [0x07, 0xe0] })
            let store = FrameStore(testFrame: ScreenFrame(width: 240, height: 150, bytes: bytes))
            try ScreenServer(host: pairing.host, secret: secret, frames: store, test: true).run()
            return
        }
        let app = NSApplication.shared
        app.setActivationPolicy(.accessory)
        let delegate = MirrorApp(pairing: pairing, secret: secret)
        app.delegate = delegate
        app.run()
        withExtendedLifetime(delegate) {}
    }
}

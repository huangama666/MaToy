import AVFoundation
import CoreMedia
import CryptoKit
import Darwin
import Foundation
import OSLog
import Security

enum VoiceMode: Int, CaseIterable {
    case listen, microphone, system, off

    var title: String {
        switch self {
        case .listen: return "听开发板（按住下键讲话）"
        case .off: return "关闭语音"
        case .microphone: return "语音对讲（Mac 麦克风）"
        case .system: return "传送 Mac 系统声音"
        }
    }
}

final class VoiceTransport: @unchecked Sendable {
    static let sampleRate = 16_000.0
    static let blockSize = 640
    private static let silence = Data(repeating: 0, count: blockSize)
    private let lock = NSLock()
    private var mode: VoiceMode = .off
    private var pending = Data()
    private var mutedUntil: TimeInterval = 0

    var currentMode: VoiceMode { lock.withLock { mode } }

    func select(_ mode: VoiceMode) {
        lock.withLock {
            self.mode = mode
            pending.removeAll(keepingCapacity: true)
            mutedUntil = 0
        }
    }

    func enqueue(_ pcm: Data, from source: VoiceMode) {
        lock.withLock {
            guard mode == source, ProcessInfo.processInfo.systemUptime >= mutedUntil else { return }
            pending.append(pcm)
            let excess = pending.count / Self.blockSize - 5
            if excess > 0 { pending.removeFirst(excess * Self.blockSize) }
        }
    }

    func response(boardTalking: Bool) -> Data? {
        lock.withLock {
            guard mode != .off else { return nil }
            if boardTalking && mode == .microphone {
                mutedUntil = ProcessInfo.processInfo.systemUptime + 0.12
                pending.removeAll(keepingCapacity: true)
            }
            guard !boardTalking, pending.count >= Self.blockSize else { return Self.silence }
            let block = Data(pending.prefix(Self.blockSize))
            pending.removeFirst(Self.blockSize)
            return block
        }
    }

    func resetSession() {
        lock.withLock {
            pending.removeAll(keepingCapacity: true)
            mutedUntil = 0
        }
    }
}

final class VoicePCMConverter: @unchecked Sendable {
    private let target = AVAudioFormat(commonFormat: .pcmFormatInt16,
                                      sampleRate: VoiceTransport.sampleRate, channels: 1, interleaved: false)!
    private let transport: VoiceTransport
    private let source: VoiceMode
    private var converter: AVAudioConverter?

    init(transport: VoiceTransport, source: VoiceMode) {
        self.transport = transport
        self.source = source
    }

    func feed(_ buffer: AVAudioPCMBuffer) {
        guard transport.currentMode == source, buffer.frameLength > 0, buffer.format.sampleRate > 0 else { return }
        if converter?.inputFormat != buffer.format {
            converter = AVAudioConverter(from: buffer.format, to: target)
            converter?.primeMethod = .none
        }
        guard let converter else { return }
        let capacity = AVAudioFrameCount(ceil(Double(buffer.frameLength) * target.sampleRate / buffer.format.sampleRate) + 64)
        guard let converted = AVAudioPCMBuffer(pcmFormat: target, frameCapacity: capacity) else { return }
        var provided = false
        var error: NSError?
        let status = converter.convert(to: converted, error: &error) { _, flag in
            flag.pointee = provided ? .noDataNow : .haveData
            if provided { return nil }
            provided = true
            return buffer
        }
        guard status != .error, converted.frameLength > 0, let samples = converted.int16ChannelData?[0] else { return }
        transport.enqueue(Data(bytes: samples, count: Int(converted.frameLength) * 2), from: source)
    }

    func feed(_ sample: CMSampleBuffer) {
        guard transport.currentMode == source,
              let description = CMSampleBufferGetFormatDescription(sample) else { return }
        let format = AVAudioFormat(cmAudioFormatDescription: description)
        let count = CMSampleBufferGetNumSamples(sample)
        guard count > 0, count <= 32_768,
              let pcm = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: AVAudioFrameCount(count)) else { return }
        pcm.frameLength = AVAudioFrameCount(count)
        if CMSampleBufferCopyPCMDataIntoAudioBufferList(sample, at: 0, frameCount: Int32(count),
                                                      into: pcm.mutableAudioBufferList) == noErr {
            feed(pcm)
        }
    }
}

@MainActor final class VoiceOutput {
    private let transport: VoiceTransport
    private let playback = AVAudioFormat(standardFormatWithSampleRate: VoiceTransport.sampleRate, channels: 1)!
    private var engine: AVAudioEngine?
    private var player: AVAudioPlayerNode?
    private var tapInstalled = false
    private var queued = 0
    private var generation = 0

    init(transport: VoiceTransport) { self.transport = transport }

    func start(microphone: Bool) throws {
        stop()
        let engine = AVAudioEngine()
        let player = AVAudioPlayerNode()
        self.engine = engine
        self.player = player
        engine.attach(player)
        engine.connect(player, to: engine.mainMixerNode, format: playback)
        if microphone {
            let input = engine.inputNode
            let format = input.outputFormat(forBus: 0)
            guard format.sampleRate > 0, format.channelCount > 0 else {
                stop()
                throw POSIXError(.ENODEV)
            }
            let converter = VoicePCMConverter(transport: transport, source: .microphone)
            input.installTap(onBus: 0, bufferSize: 960, format: format) { buffer, _ in
                converter.feed(buffer)
            }
            tapInstalled = true
        }
        do {
            try engine.start()
        } catch {
            stop()
            throw error
        }
    }

    func stop() {
        if tapInstalled { engine?.inputNode.removeTap(onBus: 0) }
        tapInstalled = false
        player?.stop()
        engine?.stop()
        player = nil
        engine = nil
        queued = 0
        generation += 1
    }

    func play(_ pcm: Data) {
        let count = pcm.count / 2
        guard transport.currentMode != .off, let player, queued < 5,
              let buffer = AVAudioPCMBuffer(pcmFormat: playback, frameCapacity: AVAudioFrameCount(count)),
              let samples = buffer.floatChannelData?[0] else { return }
        buffer.frameLength = AVAudioFrameCount(count)
        pcm.withUnsafeBytes { bytes in
            for i in 0..<count {
                let bits = UInt16(bytes[i * 2]) | UInt16(bytes[i * 2 + 1]) << 8
                samples[i] = Float(Int16(bitPattern: bits)) / 32_768
            }
        }
        queued += 1
        let id = generation
        player.scheduleBuffer(buffer, completionCallbackType: .dataPlayedBack) { [weak self] _ in
            DispatchQueue.main.async {
                guard let self, self.generation == id else { return }
                self.queued -= 1
            }
        }
        if !player.isPlaying { player.play() }
    }
}

final class VoiceServer: @unchecked Sendable {
    private let host: String
    private let secret: SymmetricKey
    private let transport: VoiceTransport
    private let onAudio: @Sendable (Data) -> Void
    private let port: UInt16
    private let logger = Logger(subsystem: "local.ai-passport.live-control", category: "voice")

    init(host: String, secret: SymmetricKey, transport: VoiceTransport, port: UInt16 = 45903,
         onAudio: @escaping @Sendable (Data) -> Void) {
        self.host = host
        self.secret = secret
        self.transport = transport
        self.onAudio = onAudio
        self.port = port
    }

    func run() throws {
        let listener = try tcpListener(host: host, port: port)
        defer { close(listener) }
        while true {
            let peer = accept(listener, nil, nil)
            if peer < 0 && errno == EINTR { continue }
            if peer < 0 { throw POSIXError(.EIO) }
            if transport.currentMode != .off { serve(peer) }
            close(peer)
        }
    }

    private func serve(_ peer: Int32) {
        var yes: Int32 = 1
        var limit = timeval(tv_sec: 2, tv_usec: 0)
        setsockopt(peer, SOL_SOCKET, SO_NOSIGPIPE, &yes, socklen_t(MemoryLayout<Int32>.size))
        setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &limit, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, &limit, socklen_t(MemoryLayout<timeval>.size))
        setsockopt(peer, IPPROTO_TCP, TCP_NODELAY, &yes, socklen_t(MemoryLayout<Int32>.size))

        guard let greeting = receive(peer, count: 21), greeting.prefix(5) == Data("APAU1".utf8) else { return }
        var challenge = [UInt8](repeating: 0, count: 16)
        guard SecRandomCopyBytes(kSecRandomDefault, challenge.count, &challenge) == errSecSuccess else { return }
        let session = authenticate(greeting + Data(challenge), with: secret)
        let root = SymmetricKey(data: session)
        guard transmit(peer, Data(challenge) + authenticate(Data("server".utf8), with: root)),
              let proof = receive(peer, count: 32),
              HMAC<SHA256>.isValidAuthenticationCode(proof, authenticating: Data("client".utf8), using: root)
        else { return }

        let fromBoard = SymmetricKey(data: authenticate(Data("board".utf8), with: root))
        let toBoard = SymmetricKey(data: authenticate(Data("mac".utf8), with: root))
        transport.resetSession()
        defer { transport.resetSession() }
        logger.notice("Authenticated audio connected on port \(self.port)")
        let pcmEnd = 5 + VoiceTransport.blockSize
        var sequence: UInt32 = 1
        var talking = false
        var sentSound = false
        var receivedSound = false
        while sequence != .max && transport.currentMode != .off {
            var outbound = Data()
            outbound.put(sequence)
            outbound.append(0)
            let expected = outbound.prefix(4)
            var nonceBytes = Data(repeating: 0, count: 8)
            nonceBytes.append(contentsOf: expected)
            guard let iv = try? AES.GCM.Nonce(data: nonceBytes),
                  let record = receive(peer, count: pcmEnd + 16),
                  record.prefix(4) == expected, record[4] <= 1,
                  let box = try? AES.GCM.SealedBox(nonce: iv, ciphertext: record[5..<pcmEnd], tag: record[pcmEnd...]),
                  let input = try? AES.GCM.open(box, using: fromBoard, authenticating: record.prefix(5)),
                  let response = transport.response(boardTalking: record[4] == 1),
                  let reply = try? AES.GCM.seal(response, using: toBoard, nonce: iv, authenticating: outbound),
                  transmit(peer, outbound + reply.ciphertext + reply.tag)
            else { return }
            if !sentSound && response.contains(where: { $0 != 0 }) {
                logger.notice("Sent Mac audio with signal on port \(self.port)")
                sentSound = true
            }
            if record[4] == 1 {
                if !talking { logger.notice("Receiving board microphone") }
                if !receivedSound && input.contains(where: { $0 != 0 }) {
                    logger.notice("Received board audio with signal on port \(self.port)")
                    receivedSound = true
                }
                onAudio(input)
            }
            talking = record[4] == 1
            sequence += 1
        }
    }

    private func authenticate(_ data: Data, with key: SymmetricKey) -> Data {
        Data(HMAC<SHA256>.authenticationCode(for: data, using: key))
    }
}

import AVFoundation
import CoreGraphics
import CoreMedia
import CoreVideo
import Foundation
import OSLog

enum PassportCameraState: UInt16 {
    case off = 0, starting, on, permissionRequired, unavailable, failed, stopping
}

enum MonitorAudioState: UInt16 {
    case off = 0, starting, listening, talking, permissionRequired, failed, stopping
}

struct CameraRequestState {
    private(set) var lease: UUID?
    private(set) var wanted = false
    var status: PassportCameraState = .off

    mutating func begin(enabled: Bool = true) -> UUID {
        let next = UUID()
        lease = next
        wanted = enabled
        status = enabled ? .starting : .off
        return next
    }

    mutating func request(_ enabled: Bool, lease expected: UUID) -> Bool {
        guard lease == expected else { return false }
        wanted = enabled
        status = enabled ? .starting : .stopping
        return true
    }

    mutating func end(_ expected: UUID) {
        guard lease == expected else { return }
        lease = nil
        wanted = false
        status = .stopping
    }

    func wants(_ expected: UUID) -> Bool { lease == expected && wanted }
}

final class PassportCamera: NSObject, AVCaptureVideoDataOutputSampleBufferDelegate, @unchecked Sendable {
    let frames = FrameStore(cursorEnabled: false)
    private let lock = NSLock()
    private var request = CameraRequestState()
    private let queue = DispatchQueue(label: "passport.camera.capture")
    private var session: AVCaptureSession?
    private var authorizationInFlight = false
    private var audioMode: UInt32 = 0
    private var audioStatus: MonitorAudioState = .off
    private var audioRevision: UInt64 = 0
    var onAudioRequest: (@Sendable (UInt32, UInt64) -> Void)?
    private let logger = Logger(subsystem: "local.ai-passport.live-control", category: "camera")

    override init() {
        super.init()
        frames.setMode(.full)
    }

    var state: PassportCameraState {
        lock.lock()
        defer { lock.unlock() }
        return request.status
    }

    var monitorAudioState: MonitorAudioState {
        lock.lock()
        defer { lock.unlock() }
        return audioStatus
    }

    var monitorAudioMode: UInt32 {
        lock.lock()
        defer { lock.unlock() }
        return audioMode
    }

    func audioRequestIsCurrent(_ mode: UInt32, revision: UInt64) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        return audioMode == mode && audioRevision == revision
    }

    func completeAudio(_ state: MonitorAudioState, revision: UInt64) {
        lock.lock()
        if audioRevision == revision { audioStatus = state }
        lock.unlock()
    }

    func setAudio(_ mode: UInt32, lease: UUID) {
        lock.lock()
        guard request.lease == lease, mode <= 2 else { lock.unlock(); return }
        audioMode = mode
        audioRevision += 1
        let revision = audioRevision
        audioStatus = mode == 0 ? .stopping : .starting
        lock.unlock()
        onAudioRequest?(mode, revision)
    }

    func beginSession(enabled: Bool) -> UUID {
        lock.lock()
        let token = request.begin(enabled: enabled)
        lock.unlock()
        frames.setCapture(nil)
        queue.async { self.reconcile() }
        return token
    }

    func setEnabled(_ enabled: Bool, lease: UUID) {
        lock.lock()
        let accepted = request.request(enabled, lease: lease)
        lock.unlock()
        guard accepted else { return }
        if !enabled { frames.setCapture(nil) }
        queue.async { self.reconcile() }
    }

    func endSession(_ lease: UUID) {
        setAudio(0, lease: lease)
        lock.lock()
        request.end(lease)
        lock.unlock()
        queue.async { self.reconcile() }
    }

    func turnOff() {
        lock.lock()
        if let lease = request.lease { _ = request.request(false, lease: lease) }
        lock.unlock()
        frames.setCapture(nil)
        queue.async { self.reconcile() }
    }

    func shutdown() {
        lock.lock()
        if let lease = request.lease { request.end(lease) }
        lock.unlock()
        queue.sync { self.stopCapture() }
    }

    private func snapshot() -> CameraRequestState {
        lock.lock()
        defer { lock.unlock() }
        return request
    }

    private func setStatus(_ value: PassportCameraState, lease: UUID?) {
        lock.lock()
        if request.lease == lease { request.status = value }
        lock.unlock()
    }

    private func stopCapture() {
        if let session {
            session.stopRunning()
            self.session = nil
            logger.notice("Camera capture stopped")
        }
        frames.setCapture(nil)
    }

    // The serial queue owns the AVCaptureSession. Every queued request observes
    // the latest lease, so a delayed permission response cannot reopen a closed camera.
    private func reconcile() {
        let desired = snapshot()
        guard desired.wanted, let lease = desired.lease else {
            stopCapture()
            setStatus(.off, lease: desired.lease)
            return
        }

        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .notDetermined:
            guard !authorizationInFlight else { return }
            authorizationInFlight = true
            AVCaptureDevice.requestAccess(for: .video) { _ in
                self.queue.async {
                    self.authorizationInFlight = false
                    self.reconcile()
                }
            }
            return
        case .authorized:
            break
        default:
            stopCapture()
            setStatus(.permissionRequired, lease: lease)
            return
        }

        guard snapshot().wants(lease) else { reconcile(); return }
        if let session, session.isRunning {
            setStatus(.on, lease: lease)
            return
        }
        guard let camera = AVCaptureDevice.default(for: .video) else {
            setStatus(.unavailable, lease: lease)
            return
        }
        do {
            let input = try AVCaptureDeviceInput(device: camera)
            let capture = AVCaptureSession()
            capture.beginConfiguration()
            if capture.canSetSessionPreset(.vga640x480) { capture.sessionPreset = .vga640x480 }
            let output = AVCaptureVideoDataOutput()
            output.alwaysDiscardsLateVideoFrames = true
            output.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
            output.setSampleBufferDelegate(self, queue: queue)
            guard capture.canAddInput(input), capture.canAddOutput(output) else {
                capture.commitConfiguration()
                setStatus(.failed, lease: lease)
                return
            }
            capture.addInput(input)
            capture.addOutput(output)
            capture.commitConfiguration()
            guard snapshot().wants(lease) else { reconcile(); return }
            session = capture
            capture.startRunning()
            guard snapshot().wants(lease) else { reconcile(); return }
            setStatus(capture.isRunning ? .on : .failed, lease: lease)
            logger.notice("Camera capture started")
        } catch {
            stopCapture()
            setStatus(.failed, lease: lease)
            logger.error("Camera capture could not start: \(error.localizedDescription, privacy: .public)")
        }
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer,
                       from connection: AVCaptureConnection) {
        let desired = snapshot()
        guard desired.wanted, desired.status == .on,
              let pixel = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        let sourceWidth = CVPixelBufferGetWidth(pixel)
        let sourceHeight = CVPixelBufferGetHeight(pixel)
        let ratio = min(240.0 / Double(sourceWidth), 240.0 / Double(sourceHeight))
        frames.setCapture(CapturedScreen(pixel: pixel, bounds: CGRect(x: 0, y: 0, width: sourceWidth, height: sourceHeight),
                                        width: max(2, Int(Double(sourceWidth) * ratio) & ~1),
                                        height: max(2, Int(Double(sourceHeight) * ratio) & ~1)))
    }
}

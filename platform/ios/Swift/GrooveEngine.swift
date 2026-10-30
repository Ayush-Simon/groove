import Foundation
import Combine

/// SwiftUI-friendly wrapper around GRVEngine (Objective-C++ → C++ core).
/// Everything published here changes on the main thread. Closures capture `self` weakly.
@MainActor
final class GrooveEngine: NSObject, ObservableObject, GRVEngineDelegate {
    @Published private(set) var state: GRVPlaybackState = .stopped
    @Published private(set) var trackId: UInt64 = 0
    @Published private(set) var position: Double = 0
    @Published private(set) var duration: Double = 0
    @Published private(set) var bitPerfect = false
    @Published private(set) var outputSampleRate: UInt32 = 0
    @Published private(set) var lastFailure: String?

    /// Called when a track starts so the queue can preload the following one (gapless).
    var onTrackStarted: ((UInt64) -> Void)?
    var onQueueEnded: (() -> Void)?

    private let engine: GRVEngine
    private var timer: Timer?
    private var tapBuffer = [Float](repeating: 0, count: 4096)

    init?(ringSeconds: Double = 2.0) {
        guard let e = GRVEngine(ringSeconds: ringSeconds) else { return nil }
        engine = e
        super.init()
        engine.delegate = self  // weak on the Obj-C side
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
    }

    deinit {
        timer?.invalidate()
        engine.shutdown()
    }

    func load(_ id: UInt64, url: URL, autoplay: Bool = true, startAt: Double = 0) {
        engine.loadTrack(id, path: url.path, autoplay: autoplay, startAt: startAt)
    }
    func setNext(_ id: UInt64, url: URL?) { engine.setNextTrack(id, path: url?.path) }
    func play() { engine.play() }
    func pause() { engine.pause() }
    func stop() { engine.stop() }
    func seek(to seconds: Double) { engine.seek(to: seconds) }
    func setVolume(_ v: Double) { engine.setVolume(v) }
    func setEQ(enabled: Bool, gains: [Double], preamp: Double = 0, guaranteedHeadroom: Bool = false) {
        precondition(gains.count == 10, "10 bands")
        engine.setEqEnabled(enabled, gains: gains.map { NSNumber(value: $0) }, preamp: preamp, guaranteedHeadroom: guaranteedHeadroom)
    }

    /// Newest output samples for a visualiser, copied straight into a Swift-owned array (no NSData, no ARC per call).
    func readTap() -> ArraySlice<Float> {
        let n = tapBuffer.withUnsafeMutableBufferPointer { buf in
            engine.readTap(into: buf.baseAddress!, capacity: buf.count)
        }
        return tapBuffer[0..<n]
    }
    func setTapEnabled(_ on: Bool) { engine.setTapEnabled(on) }

    private func refresh() {
        let s = engine.snapshot()
        position = s.position
        duration = s.duration
        bitPerfect = s.bitPerfect
    }

    // MARK: GRVEngineDelegate (main queue)
    nonisolated func engineStateDidChange(_ state: GRVPlaybackState, trackId: UInt64) {
        Task { @MainActor [weak self] in guard let self else { return }; self.state = state; self.trackId = trackId }
    }
    nonisolated func engineDidStartTrack(_ trackId: UInt64) {
        Task { @MainActor [weak self] in guard let self else { return }; self.trackId = trackId; self.onTrackStarted?(trackId) }
    }
    nonisolated func engineDidFailTrack(_ trackId: UInt64, code: Int, message: String) {
        Task { @MainActor [weak self] in guard let self else { return }; self.lastFailure = message }
    }
    nonisolated func engineQueueDidEnd() {
        Task { @MainActor [weak self] in guard let self else { return }; self.onQueueEnded?() }
    }
    nonisolated func engineOutputDidChangeSampleRate(_ rate: UInt32, osResamples: Bool, bitPerfect: Bool) {
        Task { @MainActor [weak self] in guard let self else { return }; self.outputSampleRate = rate; self.bitPerfect = bitPerfect }
    }
}

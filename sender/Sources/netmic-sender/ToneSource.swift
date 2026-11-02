import Foundation
import NetMicKit
import NetMicProtocol

/// The sender's audio source; Capture (microphone) or ToneSource (test signal).
protocol AudioSource: AnyObject {
    var onBlock: ((_ samples: [Int16], _ streamId: UInt32, _ samplePos: UInt64) -> Void)? { get set }
    var isRunning: Bool { get }
    func start()
    func stop()
    func takeLevel() -> (rmsDb: Float, peakDb: Float, blocks: UInt64, overflows: UInt64)
}

extension Capture: AudioSource {}

/// Test signal: a -12 dBFS sine paced by the host clock, emitted in 240-frame
/// blocks exactly like captured audio. Lets the network path and the driver be
/// tested without a microphone or someone talking.
final class ToneSource: AudioSource {
    var onBlock: ((_ samples: [Int16], _ streamId: UInt32, _ samplePos: UInt64) -> Void)?
    var log: (String) -> Void = { print($0) }

    private let frequency: Double
    private let lock = NSLock()
    private var thread: Thread?
    private var stopFlag: StopFlag?
    private var blocks: UInt64 = 0

    private final class StopFlag {
        private let lock = NSLock()
        private var value = false
        var isSet: Bool { lock.withLock { value } }
        func set() { lock.withLock { value = true } }
    }

    init(frequency: Double) { self.frequency = frequency }

    var isRunning: Bool { lock.withLock { thread != nil } }

    func start() {
        lock.lock()
        defer { lock.unlock() }
        guard thread == nil else { return }
        let flag = StopFlag()
        let streamId = UInt32.random(in: 1...UInt32.max)
        let freq = frequency
        let t = Thread { [weak self] in
            let fpp = Int(NetMic.framesPerPacket)
            let rate = Double(NetMic.sampleRate)
            let start = hostSeconds()
            var pos: UInt64 = 0
            while !flag.isSet {
                // Emit every block whose end time has passed, then sleep ~1 ms.
                while Double(pos + UInt64(fpp)) / rate <= hostSeconds() - start {
                    var block = [Int16](repeating: 0, count: fpp)
                    for i in 0..<fpp {
                        let phase = 2 * Double.pi * freq * Double(pos + UInt64(i)) / rate
                        block[i] = PCM.toInt16(Float(0.25 * sin(phase)))
                    }
                    self?.onBlock?(block, streamId, pos)
                    self?.lock.withLock { self?.blocks += 1 }
                    pos += UInt64(fpp)
                }
                usleep(1000)
            }
        }
        t.name = "netmic.tone"
        t.qualityOfService = .userInteractive
        thread = t
        stopFlag = flag
        t.start()
        log(String(format: "tone: started %.0f Hz test signal, stream %08x", frequency, streamId))
    }

    func stop() {
        lock.withLock {
            guard thread != nil else { return }
            stopFlag?.set()
            thread = nil
            stopFlag = nil
        }
        log("tone: stopped")
    }

    func takeLevel() -> (rmsDb: Float, peakDb: Float, blocks: UInt64, overflows: UInt64) {
        let (n, running) = lock.withLock { (blocks, thread != nil) }
        return running ? (dbfs(0.25 / sqrt(2)), dbfs(0.25), n, 0) : (-120, -120, n, 0)
    }
}

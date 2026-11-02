import AVFoundation
import CNetMicSupport
import Foundation
import NetMicProtocol

/// Microphone capture → 48 kHz mono Int16 blocks of 240 frames.
///
/// Pipeline: AVAudioEngine inputNode → AVAudioSinkNode (real-time thread; C code
/// reduces to mono into a lock-free ring) → packetizer thread (rate conversion,
/// gain, Int16, 240-frame blocks) → `onBlock`.
///
/// installTap is not used: its buffers are 100–400 ms, far over the latency budget.
/// The sink node receives buffers at the device IO size (~10 ms).
final class Capture {
    struct Options {
        var deviceQuery: String
        var channel: Int32  // -1 = mix all channels
        var gainDb: Float
    }

    /// Called on the packetizer thread for every 240-frame block.
    var onBlock: ((_ samples: [Int16], _ streamId: UInt32, _ samplePos: UInt64) -> Void)?
    var log: (String) -> Void = { print($0) }

    private let options: Options
    private let queue = DispatchQueue(label: "netmic.capture")

    // Owned by `queue`.
    private var wantRunning = false
    private var engine: AVAudioEngine?
    private var worker: Worker?
    private var configObserver: NSObjectProtocol?
    private var retryScheduled = false

    private let statsLock = NSLock()
    private var levelSumSquares: Double = 0
    private var levelCount: Int = 0
    private var levelPeak: Float = 0
    private var blocks: UInt64 = 0
    private var running = false

    init(options: Options) { self.options = options }

    func start() { queue.async { self.wantRunning = true; self.ensureState() } }
    func stop() { queue.async { self.wantRunning = false; self.ensureState() } }

    var isRunning: Bool { statsLock.withLock { running } }

    /// RMS and peak level (dBFS) since the last call, plus total blocks produced.
    func takeLevel() -> (rmsDb: Float, peakDb: Float, blocks: UInt64, overflows: UInt64) {
        let overflows = queue.sync { worker?.overflows ?? 0 }
        return statsLock.withLock {
            let rms = levelCount > 0 ? sqrt(levelSumSquares / Double(levelCount)) : 0
            defer {
                levelSumSquares = 0
                levelCount = 0
                levelPeak = 0
            }
            return (dbfs(Float(rms)), dbfs(levelPeak), blocks, overflows)
        }
    }

    // MARK: - engine lifecycle (on `queue`)

    private func ensureState() {
        if wantRunning && engine == nil {
            do {
                try startEngine()
            } catch {
                log("capture: \(error) — retrying in 1 s")
                scheduleRetry()
            }
        } else if !wantRunning && engine != nil {
            stopEngine()
            log("capture: stopped (microphone released)")
        }
    }

    private func scheduleRetry() {
        guard !retryScheduled else { return }
        retryScheduled = true
        queue.asyncAfter(deadline: .now() + 1) {
            self.retryScheduled = false
            self.ensureState()
        }
    }

    private struct CaptureError: Error, CustomStringConvertible { let description: String }

    private func startEngine() throws {
        let device: InputDevice
        switch findInputDevice(options.deviceQuery) {
        case .found(let d): device = d
        case .notFound: throw CaptureError(description: "input device \"\(options.deviceQuery)\" not found")
        case .ambiguous(let list):
            throw CaptureError(description: "device query matches several devices: \(list.map(\.name).joined(separator: ", "))")
        }
        if options.channel >= device.inputChannels {
            throw CaptureError(description: "channel \(options.channel) out of range (device has \(device.inputChannels))")
        }

        let engine = AVAudioEngine()
        let input = engine.inputNode
        guard let unit = input.audioUnit else { throw CaptureError(description: "inputNode has no audio unit") }
        // Select the device on the input unit only; the system default input is left alone.
        var devID = device.id
        let st = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                                      &devID, UInt32(MemoryLayout<AudioDeviceID>.size))
        guard st == noErr else { throw CaptureError(description: "selecting device failed (OSStatus \(st))") }

        let hw = input.inputFormat(forBus: 0)
        guard hw.sampleRate > 0, hw.channelCount > 0,
              let format = AVAudioFormat(standardFormatWithSampleRate: hw.sampleRate, channels: hw.channelCount)
        else { throw CaptureError(description: "device reports no usable input format (\(hw))") }

        guard let ring = nm_ring_create(UInt32(hw.sampleRate)) else { throw CaptureError(description: "ring alloc") }
        let channel = options.channel
        let sink = AVAudioSinkNode { _, frames, abl in
            nm_ring_push_abl(ring, abl, frames, channel)
            return noErr
        }
        engine.attach(sink)
        engine.connect(input, to: sink, format: format)

        let worker = Worker(ring: ring, inputRate: hw.sampleRate, gain: powf(10, options.gainDb / 20))
        worker.onBlock = { [weak self] samples, streamId, pos in
            self?.account(samples)
            self?.onBlock?(samples, streamId, pos)
        }

        engine.prepare()
        do {
            try engine.start()
        } catch {
            worker.finish()  // never started: frees the ring
            throw CaptureError(description: "engine start failed: \(error)")
        }
        worker.start()

        configObserver = NotificationCenter.default.addObserver(
            forName: .AVAudioEngineConfigurationChange, object: engine, queue: nil
        ) { [weak self] _ in
            guard let self else { return }
            self.queue.async {
                guard self.engine === engine else { return }
                self.log("capture: audio configuration changed — restarting with a new stream")
                self.stopEngine()
                self.ensureState()
            }
        }
        self.engine = engine
        self.worker = worker
        statsLock.withLock { running = true }
        log(String(format: "capture: started \"%@\" (%.0f Hz, %d ch, using %@), stream %08x", device.name,
                   hw.sampleRate, hw.channelCount, channel < 0 ? "mix" : "channel \(channel)", worker.streamId))
    }

    private func stopEngine() {
        if let obs = configObserver { NotificationCenter.default.removeObserver(obs) }
        configObserver = nil
        engine?.stop()  // after this returns the sink callback no longer runs
        engine = nil
        worker?.finish()
        worker = nil
        statsLock.withLock { running = false }
    }

    private func account(_ samples: [Int16]) {
        var sum: Double = 0
        var peak: Float = 0
        for s in samples {
            let x = Float(s) / 32768
            sum += Double(x * x)
            peak = max(peak, abs(x))
        }
        statsLock.withLock {
            levelSumSquares += sum
            levelCount += samples.count
            levelPeak = max(levelPeak, peak)
            blocks += 1
        }
    }
}

func dbfs(_ x: Float) -> Float { x > 0 ? 20 * log10f(x) : -120 }

/// Packetizer thread: drains the ring, converts to 48 kHz, applies gain and emits 240-frame Int16 blocks.
private final class Worker {
    let streamId = UInt32.random(in: 1...UInt32.max)
    var onBlock: (([Int16], UInt32, UInt64) -> Void)?

    private let ring: OpaquePointer
    private let inputRate: Double
    private let gain: Float
    private let stopLock = NSLock()
    private var stopRequested = false
    private let done = DispatchSemaphore(value: 0)
    private var thread: Thread?

    init(ring: OpaquePointer, inputRate: Double, gain: Float) {
        self.ring = ring
        self.inputRate = inputRate
        self.gain = gain
    }

    var overflows: UInt64 { nm_ring_overflows(ring) }

    func start() {
        let t = Thread { [self] in run() }
        t.name = "netmic.packetizer"
        t.qualityOfService = .userInteractive
        thread = t
        t.start()
    }

    /// Stops the thread (if started), waits for it and frees the ring.
    func finish() {
        if thread != nil {
            stopLock.withLock { stopRequested = true }
            done.wait()
        }
        nm_ring_destroy(ring)
    }

    private var shouldStop: Bool { stopLock.withLock { stopRequested } }

    private func run() {
        defer { done.signal() }
        let chunk = 4096
        var scratch = [Float](repeating: 0, count: chunk)
        var pending: [Float] = []
        pending.reserveCapacity(chunk * 2)
        var samplePos: UInt64 = 0
        let converter = inputRate == Double(NetMic.sampleRate) ? nil : RateConverter(from: inputRate)

        while !shouldStop {
            let n = Int(scratch.withUnsafeMutableBufferPointer { nm_ring_read(ring, $0.baseAddress!, UInt32(chunk)) })
            if n == 0 {
                usleep(2000)
                continue
            }
            if let converter {
                pending.append(contentsOf: converter.convert(scratch, count: n))
            } else {
                pending.append(contentsOf: scratch[0..<n])
            }
            let fpp = Int(NetMic.framesPerPacket)
            var offset = 0
            while pending.count - offset >= fpp {
                var block = [Int16](repeating: 0, count: fpp)
                for i in 0..<fpp { block[i] = PCM.toInt16(pending[offset + i] * gain) }
                onBlock?(block, streamId, samplePos)
                samplePos += UInt64(fpp)
                offset += fpp
            }
            pending.removeFirst(offset)
        }
    }
}

/// Streaming mono Float32 sample-rate converter to 48 kHz.
private final class RateConverter {
    private let converter: AVAudioConverter
    private let inFormat: AVAudioFormat
    private let outFormat: AVAudioFormat

    init?(from rate: Double) {
        guard let i = AVAudioFormat(standardFormatWithSampleRate: rate, channels: 1),
              let o = AVAudioFormat(standardFormatWithSampleRate: Double(NetMic.sampleRate), channels: 1),
              let c = AVAudioConverter(from: i, to: o)
        else { return nil }
        c.sampleRateConverterQuality = AVAudioQuality.high.rawValue
        inFormat = i
        outFormat = o
        converter = c
    }

    func convert(_ samples: [Float], count: Int) -> [Float] {
        guard let inBuf = AVAudioPCMBuffer(pcmFormat: inFormat, frameCapacity: AVAudioFrameCount(count)) else { return [] }
        inBuf.frameLength = AVAudioFrameCount(count)
        samples.withUnsafeBufferPointer { inBuf.floatChannelData![0].update(from: $0.baseAddress!, count: count) }
        let capacity = AVAudioFrameCount(Double(count) * outFormat.sampleRate / inFormat.sampleRate) + 64
        guard let outBuf = AVAudioPCMBuffer(pcmFormat: outFormat, frameCapacity: capacity) else { return [] }
        var supplied = false
        var error: NSError?
        converter.convert(to: outBuf, error: &error) { _, status in
            if supplied {
                status.pointee = .noDataNow
                return nil
            }
            supplied = true
            status.pointee = .haveData
            return inBuf
        }
        return Array(UnsafeBufferPointer(start: outBuf.floatChannelData![0], count: Int(outBuf.frameLength)))
    }
}

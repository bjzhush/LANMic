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
/// The sink node receives buffers at the device IO size (240 frames = 5 ms by default, see --io-frames).
public final class Capture: @unchecked Sendable {  // state on `queue` or behind `statsLock`
    public struct Options {
        public var deviceQuery: String
        public var channel: Int32  // -1 = mix all channels
        public var gainDb: Float
        public var ioFrames: UInt32  // requested device IO buffer size; 0 = device default

        public init(deviceQuery: String, channel: Int32 = 0, gainDb: Float = 0, ioFrames: UInt32 = 240) {
            self.deviceQuery = deviceQuery
            self.channel = channel
            self.gainDb = gainDb
            self.ioFrames = ioFrames
        }
    }

    /// Called on the packetizer thread for every 240-frame block.
    public var onBlock: ((_ samples: [Int16], _ streamId: UInt32, _ samplePos: UInt64) -> Void)?
    public var log: (String) -> Void = { print($0) }

    private let options: Options
    private let queue = DispatchQueue(label: "netmic.capture")

    // Owned by `queue`.
    private var wantRunning = false
    private var engine: AVAudioEngine?
    private var worker: Worker?
    private var configObserver: NSObjectProtocol?
    private var retryScheduled = false
    private var lastStartError: String?  // logged once until it changes or capture starts
    private var configChangePending = false
    private var startedDevice: AudioDeviceID = 0
    private var startedFormat: (rate: Double, channels: AVAudioChannelCount) = (0, 0)
    private var lastStart: DispatchTime = .now()
    private var watchdog: DispatchSourceTimer?
    private var watchdogBlocks: UInt64 = 0
    private var watchdogStalledSince: DispatchTime?

    private let statsLock = NSLock()
    private var levelSumSquares: Double = 0
    private var levelCount: Int = 0
    private var levelPeak: Float = 0
    private var blocks: UInt64 = 0
    private var running = false

    public init(options: Options) {
        self.options = options
        // Watchdog: the engine can stop delivering audio without a configuration
        // change notification (sleep/wake, device unplugged). If capture should be
        // running but no block arrived for 2 s, rebuild it; while the device is
        // missing, startEngine fails and the 1 s retry takes over.
        let timer = DispatchSource.makeTimerSource(queue: queue)
        timer.schedule(deadline: .now() + 1, repeating: 1)
        timer.setEventHandler { [weak self] in self?.checkWatchdog() }
        timer.resume()
        watchdog = timer
    }

    public func start() { queue.async { self.wantRunning = true; self.ensureState() } }
    public func stop() { queue.async { self.wantRunning = false; self.ensureState() } }

    /// Stops capture and the watchdog and waits for both; the object is unusable afterwards.
    public func shutdown() {
        queue.sync {
            wantRunning = false
            ensureState()
            watchdog?.cancel()
            watchdog = nil
        }
    }

    public var isRunning: Bool { statsLock.withLock { running } }

    /// RMS and peak level (dBFS) since the last call, plus total blocks produced.
    public func takeLevel() -> (rmsDb: Float, peakDb: Float, blocks: UInt64, overflows: UInt64) {
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
                lastStartError = nil
            } catch {
                let message = "\(error)"
                if message != lastStartError { log("capture: \(message) — retrying every 1 s") }
                lastStartError = message
                scheduleRetry()
            }
        } else if !wantRunning && engine != nil {
            stopEngine()
            log("capture: stopped (microphone released)")
        } else if !wantRunning {
            lastStartError = nil
        }
    }

    private func checkWatchdog() {
        guard wantRunning, engine != nil else {
            watchdogStalledSince = nil
            return
        }
        let blocks = statsLock.withLock { self.blocks }
        if blocks != watchdogBlocks {
            watchdogBlocks = blocks
            watchdogStalledSince = nil
            return
        }
        let since = watchdogStalledSince ?? .now()
        watchdogStalledSince = since
        if Double(DispatchTime.now().uptimeNanoseconds - since.uptimeNanoseconds) / 1e9 >= 2 {
            log("capture: no audio for 2 s (engine running: \(engine?.isRunning == true ? "yes" : "no")) — restarting")
            watchdogStalledSince = nil
            stopEngine()
            ensureState()
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
        let ioFrames = setBufferFrameSize(device.id, options.ioFrames)

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
                self.configurationChanged()
            }
        }
        self.engine = engine
        self.worker = worker
        startedDevice = device.id
        startedFormat = (hw.sampleRate, hw.channelCount)
        lastStart = .now()
        statsLock.withLock { running = true }
        log(String(format: "capture: started \"%@\" (%.0f Hz, %d ch, using %@, IO %u frames), stream %08x",
                   device.name, hw.sampleRate, hw.channelCount, channel < 0 ? "mix" : "channel \(channel)",
                   ioFrames, worker.streamId))
    }

    /// AVAudioEngineConfigurationChange also fires for changes that need no action
    /// (selecting the device on the input unit posts one shortly after start), so
    /// notifications are coalesced for 300 ms and the engine is only rebuilt when it
    /// stopped or the device / format changed. Rebuilds are at least 1 s apart.
    private func configurationChanged() {
        guard !configChangePending else { return }
        configChangePending = true
        queue.asyncAfter(deadline: .now() + 0.3) {
            self.configChangePending = false
            guard let engine = self.engine, let unit = engine.inputNode.audioUnit else { return }
            var dev = AudioDeviceID(0)
            var size = UInt32(MemoryLayout<AudioDeviceID>.size)
            AudioUnitGetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, &size)
            let hw = engine.inputNode.inputFormat(forBus: 0)
            let state = String(format: "running %@, device %u→%u, %.0f Hz %d ch → %.0f Hz %d ch",
                               engine.isRunning ? "yes" : "no", self.startedDevice, dev,
                               self.startedFormat.rate, self.startedFormat.channels, hw.sampleRate, hw.channelCount)
            if engine.isRunning && dev == self.startedDevice && hw.sampleRate == self.startedFormat.rate
                && hw.channelCount == self.startedFormat.channels {
                self.log("capture: audio configuration change ignored (\(state))")
                return
            }
            self.log("capture: audio configuration changed (\(state)) — restarting with a new stream")
            self.stopEngine()
            let wait = 1.0 - Double(DispatchTime.now().uptimeNanoseconds - self.lastStart.uptimeNanoseconds) / 1e9
            if wait > 0 {
                self.queue.asyncAfter(deadline: .now() + wait) { self.ensureState() }
            } else {
                self.ensureState()
            }
        }
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

public func dbfs(_ x: Float) -> Float { x > 0 ? 20 * log10f(x) : -120 }

/// Asks for `frames` as this process's IO buffer size on `device` (the HAL keeps
/// it per process), clamped to the device's range. Returns the size in effect.
private func setBufferFrameSize(_ device: AudioDeviceID, _ frames: UInt32) -> UInt32 {
    var addr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyBufferFrameSize,
                                          mScope: kAudioObjectPropertyScopeGlobal,
                                          mElement: kAudioObjectPropertyElementMain)
    if frames > 0 {
        var rangeAddr = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyBufferFrameSizeRange,
                                                   mScope: kAudioObjectPropertyScopeGlobal,
                                                   mElement: kAudioObjectPropertyElementMain)
        var range = AudioValueRange()
        var size = UInt32(MemoryLayout<AudioValueRange>.size)
        var want = frames
        if AudioObjectGetPropertyData(device, &rangeAddr, 0, nil, &size, &range) == noErr {
            want = UInt32(min(max(Double(frames), range.mMinimum), range.mMaximum))
        }
        AudioObjectSetPropertyData(device, &addr, 0, nil, UInt32(MemoryLayout<UInt32>.size), &want)
    }
    var actual: UInt32 = 0
    var size = UInt32(MemoryLayout<UInt32>.size)
    AudioObjectGetPropertyData(device, &addr, 0, nil, &size, &actual)
    return actual
}

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
                usleep(1000)
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

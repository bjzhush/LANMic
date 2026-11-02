import Foundation
import NetMicKit
import NetMicProtocol

/// UDP control + audio fan-out. The network loop runs on the thread that calls
/// run() until stop(); sendAudio() is called from the capture packetizer thread.
public final class Server: @unchecked Sendable {  // state behind `lock`
    public struct Options {
        public var port: UInt16
        public var name: String
        public var token: Token
        public var alwaysOn: Bool
        public var stopGrace: Double
        public var allowAnySource: Bool
        public var verbose: Bool

        public init(port: UInt16 = NetMic.defaultPort, name: String = "M4 Mic", token: Token = .zero,
                    alwaysOn: Bool = false, stopGrace: Double = 5, allowAnySource: Bool = false, verbose: Bool = false) {
            self.port = port
            self.name = name
            self.token = token
            self.alwaysOn = alwaysOn
            self.stopGrace = stopGrace
            self.allowAnySource = allowAnySource
            self.verbose = verbose
        }
    }

    public struct Status {
        public var subscribers: [String]
        public var capturing: Bool
        public var packetsSent: UInt64
    }

    private let options: Options
    private let capture: AudioSource
    private let socket: UDPSocket

    private let lock = NSLock()
    private var subscribers: [SocketAddress: Double] = [:]  // → last SUBSCRIBE time
    private var audioSeq: UInt32 = 0  // packetizer thread only
    private var packetsSent: UInt64 = 0
    private var alwaysOnValue: Bool
    private var stopRequested = false

    public init(options: Options, capture: AudioSource) throws {
        self.options = options
        self.capture = capture
        alwaysOnValue = options.alwaysOn
        socket = try UDPSocket(port: options.port)
        capture.onBlock = { [unowned self] samples, streamId, pos in sendAudio(samples, streamId: streamId, samplePos: pos) }
    }

    /// Capture regardless of subscribers (can be changed while running).
    public var alwaysOn: Bool {
        get { lock.withLock { alwaysOnValue } }
        set { lock.withLock { alwaysOnValue = newValue } }
    }

    public func status() -> Status {
        let (subs, sent) = lock.withLock { (subscribers.keys.map { "\($0)" }.sorted(), packetsSent) }
        return Status(subscribers: subs, capturing: capture.isRunning, packetsSent: sent)
    }

    /// Makes run() return (within ~100 ms), releasing the microphone and the port.
    public func stop() { lock.withLock { stopRequested = true } }

    public func run() {
        log("listening on UDP \(options.port) as \"\(options.name)\"\(alwaysOn ? ", always-on" : "")")
        defer {
            capture.shutdown()
            capture.onBlock = nil
            lock.withLock { subscribers.removeAll() }
        }
        var wantCapture = false
        var emptySince: Double? = hostSeconds()
        var lastStats = hostSeconds()
        var lastPacketsSent: UInt64 = 0

        while !lock.withLock({ stopRequested }) {
            if let (bytes, from) = socket.receive(timeoutMs: 100) { handle(bytes, from: from) }
            let now = hostSeconds()

            let (count, alwaysOn): (Int, Bool) = lock.withLock {
                for (addr, seen) in subscribers where now - seen > Double(NetMic.subscriberTimeoutMs) / 1000 {
                    subscribers[addr] = nil
                    log("subscriber \(addr) timed out")
                }
                return (subscribers.count, alwaysOnValue)
            }

            let desired: Bool
            if alwaysOn || count > 0 {
                emptySince = nil
                desired = true
            } else {
                if emptySince == nil { emptySince = now }
                desired = wantCapture && now - emptySince! < options.stopGrace
            }
            if desired != wantCapture {
                wantCapture = desired
                if desired { capture.start() } else { capture.stop() }
            }

            if options.verbose && now - lastStats >= 5 {
                let sent = lock.withLock { packetsSent }
                let level = capture.takeLevel()
                log(String(format: "subscribers %d | capture %@ | sent %llu (+%llu) | level rms %.1f dBFS, peak %.1f dBFS | overflows %llu",
                           count, capture.isRunning ? "on" : "off", sent, sent - lastPacketsSent,
                           level.rmsDb, level.peakDb, level.overflows))
                lastPacketsSent = sent
                lastStats = now
            }
        }
    }

    private func handle(_ bytes: [UInt8], from: SocketAddress) {
        guard options.allowAnySource || isPrivateIPv4(from.ip) else {
            if options.verbose { log("ignored packet from non-private address \(from)") }
            return
        }
        guard let (header, payload) = try? Packet.decode(bytes) else {
            if options.verbose { log("ignored malformed packet from \(from)") }
            return
        }
        switch header.type {
        case .discover, .subscribe, .unsubscribe:
            guard options.token == .zero || Array(payload) == options.token.bytes else {
                if options.verbose { log("ignored \(header.type) with wrong token from \(from)") }
                return
            }
        default:
            return
        }

        switch header.type {
        case .discover:
            socket.send(Packet.announce(Announce(name: options.name)), to: from)
            if options.verbose { log("announced to \(from)") }
        case .subscribe:
            let isNew = lock.withLock {
                let new = subscribers[from] == nil
                subscribers[from] = hostSeconds()
                return new
            }
            if isNew { log("subscriber \(from) added") }
        case .unsubscribe:
            let removed = lock.withLock { subscribers.removeValue(forKey: from) != nil }
            if removed { log("subscriber \(from) unsubscribed") }
        default:
            break
        }
    }

    private func sendAudio(_ samples: [Int16], streamId: UInt32, samplePos: UInt64) {
        let targets = lock.withLock { Array(subscribers.keys) }
        guard !targets.isEmpty else { return }
        let packet = Packet.audio(seq: audioSeq, streamId: streamId, samplePos: samplePos, samples: samples)
        audioSeq &+= 1
        for t in targets { socket.send(packet, to: t) }
        lock.withLock { packetsSent += 1 }
    }
}

/// Where log lines go (default: stdout). Set before starting a Server or Capture.
public var logSink: (String) -> Void = { print($0) }

private let timestampFormatter: DateFormatter = {
    let f = DateFormatter()
    f.dateFormat = "HH:mm:ss.SSS"
    return f
}()

public func log(_ message: String) {
    logSink("[\(timestampFormatter.string(from: Date()))] \(message)")
}

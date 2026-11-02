import Foundation
import NetMicKit
import NetMicProtocol

/// UDP control + audio fan-out. The network loop runs on the calling thread;
/// sendAudio() is called from the capture packetizer thread.
final class Server {
    struct Options {
        var port: UInt16
        var name: String
        var token: Token
        var alwaysOn: Bool
        var stopGrace: Double
        var allowAnySource: Bool
        var verbose: Bool
    }

    private let options: Options
    private let capture: AudioSource
    private let socket: UDPSocket

    private let lock = NSLock()
    private var subscribers: [SocketAddress: Double] = [:]  // → last SUBSCRIBE time
    private var audioSeq: UInt32 = 0  // packetizer thread only
    private var packetsSent: UInt64 = 0

    init(options: Options, capture: AudioSource) throws {
        self.options = options
        self.capture = capture
        socket = try UDPSocket(port: options.port)
        capture.onBlock = { [unowned self] samples, streamId, pos in sendAudio(samples, streamId: streamId, samplePos: pos) }
    }

    func run() -> Never {
        log("listening on UDP \(options.port) as \"\(options.name)\"\(options.alwaysOn ? ", always-on" : "")")
        var wantCapture = false
        var emptySince: Double? = hostSeconds()
        var lastStats = hostSeconds()
        var lastPacketsSent: UInt64 = 0

        while true {
            if let (bytes, from) = socket.receive(timeoutMs: 100) { handle(bytes, from: from) }
            let now = hostSeconds()

            let count: Int = lock.withLock {
                for (addr, seen) in subscribers where now - seen > Double(NetMic.subscriberTimeoutMs) / 1000 {
                    subscribers[addr] = nil
                    log("subscriber \(addr) timed out")
                }
                return subscribers.count
            }

            let desired: Bool
            if options.alwaysOn || count > 0 {
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

private let timestampFormatter: DateFormatter = {
    let f = DateFormatter()
    f.dateFormat = "HH:mm:ss.SSS"
    return f
}()

func log(_ message: String) {
    print("[\(timestampFormatter.string(from: Date()))] \(message)")
}

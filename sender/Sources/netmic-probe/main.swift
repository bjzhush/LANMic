// Receiver-side test tool (runs on M2): discovery, recording to WAV and link statistics.
import Foundation
import NetMicKit
import NetMicProtocol

let usage = """
    usage:
      netmic-probe --discover                              [--sender <ip>] [--port 47810] [--token <hex>]
      netmic-probe --record <seconds> --out <file.wav>     [--sender <ip>] [--port 47810] [--token <hex>]
      netmic-probe --stats <seconds>                       [--sender <ip>] [--port 47810] [--token <hex>]

    Without --sender the sender is found by UDP broadcast. Ctrl-C ends a
    recording / stats run early (the file is still written).
    """

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data("netmic-probe: \(message)\n".utf8))
    exit(2)
}

func need<T>(_ value: T?, _ message: @autoclosure () -> String) -> T {
    guard let value else { fail(message()) }
    return value
}

setvbuf(stdout, nil, _IOLBF, 0)

enum Mode {
    case discover
    case record(seconds: Double, out: String)
    case stats(seconds: Double)
}

var args = Array(CommandLine.arguments.dropFirst())
func takeValue(_ flag: String) -> String {
    guard !args.isEmpty else { fail("\(flag) needs a value") }
    return args.removeFirst()
}

var mode: Mode?
var recordSeconds: Double?
var outPath: String?
var senderIP: String?
var port = NetMic.defaultPort
var token = Token.zero

while !args.isEmpty {
    let a = args.removeFirst()
    switch a {
    case "--discover": mode = .discover
    case "--record": recordSeconds = need(Double(takeValue(a)).flatMap { $0 > 0 ? $0 : nil }, "bad --record")
    case "--out": outPath = takeValue(a)
    case "--stats": mode = .stats(seconds: need(Double(takeValue(a)).flatMap { $0 > 0 ? $0 : nil }, "bad --stats"))
    case "--sender": senderIP = takeValue(a)
    case "--port": port = need(UInt16(takeValue(a)), "bad --port")
    case "--token": token = need(Token(hex: takeValue(a)), "--token must be 32 hex digits")
    case "-h", "--help":
        print(usage)
        exit(0)
    default: fail("unknown argument \(a)\n\(usage)")
    }
}
if let s = recordSeconds {
    guard let outPath else { fail("--record needs --out <file.wav>") }
    mode = .record(seconds: s, out: outPath)
}
guard let mode else { fail(usage) }

nonisolated(unsafe) var interrupted: sig_atomic_t = 0
signal(SIGINT) { _ in interrupted = 1 }

let socket: UDPSocket
do {
    socket = try UDPSocket(port: 0, broadcast: true)
} catch {
    fail("\(error)")
}

var discoverTargets: [SocketAddress] {
    if let senderIP {
        guard let a = SocketAddress(senderIP, port: port) else { fail("bad --sender address \(senderIP)") }
        return [a]
    }
    return [SocketAddress.limitedBroadcast(port: port)] + interfaceBroadcastAddresses().map { SocketAddress(ip: $0, port: port) }
}

/// Broadcasts DISCOVER once a second. Returns every sender that answered
/// (stops at the first one when `firstOnly`).
func discover(timeout: Double, firstOnly: Bool) -> [(SocketAddress, Announce)] {
    var found: [(SocketAddress, Announce)] = []
    let start = hostSeconds()
    var lastSend = -1.0
    var seq: UInt32 = 0
    while hostSeconds() - start < timeout && interrupted == 0 {
        if hostSeconds() - lastSend >= 1 {
            for t in discoverTargets { socket.send(Packet.discover(token: token, seq: seq), to: t) }
            seq &+= 1
            lastSend = hostSeconds()
        }
        guard let (bytes, from) = socket.receive(timeoutMs: 100),
              let (h, payload) = try? Packet.decode(bytes), h.type == .announce,
              let a = try? Announce.decode(payload)
        else { continue }
        if found.contains(where: { $0.0 == from }) { continue }
        found.append((from, a))
        if firstOnly { break }
    }
    return found
}

func describe(_ a: Announce) -> String {
    "\"\(a.name)\" — \(a.sampleRate) Hz, \(a.channels) ch, format \(a.format == NetMic.formatInt16 ? "Int16" : "#\(a.format)"), \(a.framesPerPacket) frames/packet"
}

func statsLine(_ s: ReceiverStats, elapsed: Double) -> String {
    let drift = s.driftPpm.map { String(format: "%+.1f ppm", $0) } ?? "n/a (needs 10 s)"
    return String(format: "t=%5.1fs  packets %llu  lost %llu (%.3f%%)  reordered %llu  dup %llu  jitter %.2f ms  drift %@%@",
                  elapsed, s.packets, s.lost, s.lossRate * 100, s.reordered, s.duplicates, s.jitter * 1000, drift,
                  s.streamChanges > 0 ? "  stream changes \(s.streamChanges)" : "")
}

/// Mono 16-bit PCM WAV.
func writeWAV(_ samples: [Int16], sampleRate: UInt32, to path: String) throws {
    var d: [UInt8] = []
    func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
    func u16(_ v: UInt16) { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
    let dataBytes = UInt32(samples.count * 2)
    d.append(contentsOf: Array("RIFF".utf8)); u32(36 + dataBytes)
    d.append(contentsOf: Array("WAVE".utf8))
    d.append(contentsOf: Array("fmt ".utf8)); u32(16); u16(1); u16(1); u32(sampleRate); u32(sampleRate * 2); u16(2); u16(16)
    d.append(contentsOf: Array("data".utf8)); u32(dataBytes)
    d.reserveCapacity(d.count + samples.count * 2)
    for s in samples { u16(UInt16(bitPattern: s)) }
    try Data(d).write(to: URL(fileURLWithPath: path))
}

/// Subscribes to `sender` for `seconds` (counted from the first audio packet).
func session(sender: SocketAddress, seconds: Double, recordTo path: String?) {
    let capacity = Int(seconds * Double(NetMic.sampleRate))
    var recording = path != nil ? [Int16](repeating: 0, count: capacity) : []
    var recorded = 0         // highest written index + 1
    var streamBase: Int?     // recording index of samplePos 0 for the current stream
    var currentStream: UInt32?
    var stats = ReceiverStats()

    var subSeq: UInt32 = 0
    var lastSubscribe = -1.0
    let start = hostSeconds()
    var firstAudio: Double?
    var lastReport = start

    print("subscribing to \(sender)…")
    while interrupted == 0 {
        let now = hostSeconds()
        if let f = firstAudio, now - f >= seconds { break }
        if firstAudio == nil && now - start > 10 { fail("no audio from \(sender) within 10 s") }
        if now - lastSubscribe >= Double(NetMic.subscribeIntervalMs) / 1000 {
            socket.send(Packet.subscribe(token: token, seq: subSeq), to: sender)
            subSeq &+= 1
            lastSubscribe = now
        }
        if now - lastReport >= 5, let f = firstAudio {
            print(statsLine(stats, elapsed: now - f))
            lastReport = now
        }

        guard let (bytes, from) = socket.receive(timeoutMs: 50), from.ip == sender.ip,
              let (h, payload) = try? Packet.decode(bytes), h.type == .audio
        else { continue }
        let arrival = hostSeconds()
        if firstAudio == nil {
            firstAudio = arrival
            print(String(format: "first audio after %.0f ms", (arrival - start) * 1000))
        }
        stats.record(seq: h.seq, streamId: h.streamId, samplePos: h.samplePos, arrival: arrival)

        guard path != nil else { continue }
        if currentStream != h.streamId {
            // New stream: continue the file after what we already have.
            currentStream = h.streamId
            streamBase = recorded - Int(clamping: h.samplePos)
        }
        let samples = PCM.decode(payload)
        let at = streamBase! + Int(clamping: h.samplePos)
        guard at >= 0 else { continue }
        for (i, s) in samples.enumerated() where at + i < capacity { recording[at + i] = s }
        recorded = max(recorded, min(capacity, at + samples.count))
    }

    for _ in 0..<2 { socket.send(Packet.unsubscribe(token: token), to: sender) }
    let elapsed = firstAudio.map { hostSeconds() - $0 } ?? 0
    print("— final —")
    print(statsLine(stats, elapsed: elapsed))

    if let path {
        do {
            try writeWAV(Array(recording.prefix(recorded)), sampleRate: NetMic.sampleRate, to: path)
            print(String(format: "wrote %@ (%.2f s)", path, Double(recorded) / Double(NetMic.sampleRate)))
        } catch {
            fail("writing \(path): \(error)")
        }
    }
}

func findSender() -> SocketAddress {
    guard let (addr, a) = discover(timeout: 10, firstOnly: true).first else {
        fail("no sender answered within 10 s" + (senderIP == nil ? " (try --sender <ip>)" : ""))
    }
    print("sender \(addr.ipString): \(describe(a))")
    return addr
}

switch mode {
case .discover:
    let found = discover(timeout: 3, firstOnly: false)
    if found.isEmpty { fail("no sender answered within 3 s") }
    for (addr, a) in found { print("\(addr.ipString): \(describe(a))") }
case .record(let seconds, let out):
    session(sender: findSender(), seconds: seconds, recordTo: out)
case .stats(let seconds):
    session(sender: findSender(), seconds: seconds, recordTo: nil)
}

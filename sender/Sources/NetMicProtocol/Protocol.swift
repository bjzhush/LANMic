// NetMic wire protocol v1. Must stay byte-compatible with shared/netmic_protocol.h;
// both are tested against shared/test_vectors.txt.

public enum NetMic {
    public static let magic: UInt32 = 0x4349_4D4E  // "NMIC" on the wire
    public static let version: UInt8 = 1
    public static let defaultPort: UInt16 = 47810

    public static let headerBytes = 24
    public static let tokenBytes = 16
    public static let maxPacketBytes = 1500
    public static let maxPayloadBytes = maxPacketBytes - headerBytes

    public static let sampleRate: UInt32 = 48000
    public static let framesPerPacket: UInt16 = 240
    public static let formatInt16: UInt8 = 1

    public static let announceFixedBytes = 9
    public static let maxNameBytes = 255

    public static let subscribeIntervalMs = 500
    public static let subscriberTimeoutMs = 3000
    public static let discoverIntervalMs = 1000
    public static let senderTimeoutMs = 5000
}

public enum PacketType: UInt8, Sendable {
    case discover = 1
    case announce = 2
    case subscribe = 3
    case unsubscribe = 4
    case audio = 5
}

/// Mirrors netmic_status in the C header (raw values match).
public enum ProtocolError: Int, Error, Sendable {
    case short = 1
    case magic = 2
    case version = 3
    case type = 4
    case length = 5
}

public struct PacketHeader: Equatable, Sendable {
    public var type: PacketType
    public var seq: UInt32
    public var streamId: UInt32
    public var samplePos: UInt64

    public init(type: PacketType, seq: UInt32 = 0, streamId: UInt32 = 0, samplePos: UInt64 = 0) {
        self.type = type
        self.seq = seq
        self.streamId = streamId
        self.samplePos = samplePos
    }
}

/// A 16-byte pre-shared token. All zeros means "no authentication".
public struct Token: Equatable, Hashable, Sendable {
    public var bytes: [UInt8]

    public static let zero = Token(bytes: [UInt8](repeating: 0, count: NetMic.tokenBytes))

    public init(bytes: [UInt8]) {
        precondition(bytes.count == NetMic.tokenBytes)
        self.bytes = bytes
    }

    /// Parses 32 hex digits; an empty string yields `.zero`.
    public init?(hex: String) {
        if hex.isEmpty {
            self = .zero
            return
        }
        guard let b = parseHex(hex), b.count == NetMic.tokenBytes else { return nil }
        self.bytes = b
    }
}

public struct Announce: Equatable, Sendable {
    public var sampleRate: UInt32
    public var channels: UInt8
    public var format: UInt8
    public var framesPerPacket: UInt16
    /// UTF-8, truncated to 255 bytes on encode.
    public var nameBytes: [UInt8]

    public var name: String { String(decoding: nameBytes, as: UTF8.self) }

    public init(sampleRate: UInt32 = NetMic.sampleRate, channels: UInt8 = 1, format: UInt8 = NetMic.formatInt16,
                framesPerPacket: UInt16 = NetMic.framesPerPacket, name: String) {
        self.init(sampleRate: sampleRate, channels: channels, format: format, framesPerPacket: framesPerPacket,
                  nameBytes: Array(name.utf8))
    }

    public init(sampleRate: UInt32, channels: UInt8, format: UInt8, framesPerPacket: UInt16, nameBytes: [UInt8]) {
        self.sampleRate = sampleRate
        self.channels = channels
        self.format = format
        self.framesPerPacket = framesPerPacket
        self.nameBytes = Array(nameBytes.prefix(NetMic.maxNameBytes))
    }

    public func encode() -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(NetMic.announceFixedBytes + nameBytes.count)
        out.appendLE(sampleRate)
        out.append(channels)
        out.append(format)
        out.appendLE(framesPerPacket)
        out.append(UInt8(nameBytes.count))
        out.append(contentsOf: nameBytes)
        return out
    }

    public static func decode<C: Collection>(_ payload: C) throws -> Announce where C.Element == UInt8 {
        let p = Array(payload)
        guard p.count >= NetMic.announceFixedBytes, p.count == NetMic.announceFixedBytes + Int(p[8]) else {
            throw ProtocolError.length
        }
        return Announce(sampleRate: p.readLE(UInt32.self, at: 0), channels: p[4], format: p[5],
                        framesPerPacket: p.readLE(UInt16.self, at: 6), nameBytes: Array(p[9...]))
    }
}

public enum Packet {
    /// Encodes header + payload into a single datagram.
    public static func encode(_ header: PacketHeader, payload: [UInt8]) -> [UInt8] {
        precondition(payload.count <= NetMic.maxPayloadBytes)
        var out: [UInt8] = []
        out.reserveCapacity(NetMic.headerBytes + payload.count)
        out.appendLE(NetMic.magic)
        out.append(NetMic.version)
        out.append(header.type.rawValue)
        out.appendLE(UInt16(payload.count))
        out.appendLE(header.seq)
        out.appendLE(header.streamId)
        out.appendLE(header.samplePos)
        out.append(contentsOf: payload)
        return out
    }

    /// Parses and validates a whole datagram (same rules and error precedence as
    /// netmic_packet_decode in the C header).
    public static func decode(_ bytes: [UInt8]) throws -> (header: PacketHeader, payload: ArraySlice<UInt8>) {
        guard bytes.count >= NetMic.headerBytes else { throw ProtocolError.short }
        guard bytes.readLE(UInt32.self, at: 0) == NetMic.magic else { throw ProtocolError.magic }
        guard bytes[4] == NetMic.version else { throw ProtocolError.version }
        let payloadBytes = Int(bytes.readLE(UInt16.self, at: 6))
        let available = bytes.count - NetMic.headerBytes
        guard available >= payloadBytes else { throw ProtocolError.short }
        guard available == payloadBytes else { throw ProtocolError.length }
        guard let type = PacketType(rawValue: bytes[5]) else { throw ProtocolError.type }
        let payload = bytes[NetMic.headerBytes...]
        try checkPayload(type: type, payload)
        let header = PacketHeader(type: type, seq: bytes.readLE(UInt32.self, at: 8),
                                  streamId: bytes.readLE(UInt32.self, at: 12),
                                  samplePos: bytes.readLE(UInt64.self, at: 16))
        return (header, payload)
    }

    static func checkPayload(type: PacketType, _ p: ArraySlice<UInt8>) throws {
        switch type {
        case .discover, .subscribe, .unsubscribe:
            guard p.count == NetMic.tokenBytes else { throw ProtocolError.length }
        case .announce:
            guard p.count >= NetMic.announceFixedBytes,
                  p.count == NetMic.announceFixedBytes + Int(p[p.startIndex + 8])
            else { throw ProtocolError.length }
        case .audio:
            guard p.count > 0, p.count % 2 == 0 else { throw ProtocolError.length }
        }
    }

    // MARK: convenience builders

    public static func discover(token: Token, seq: UInt32 = 0) -> [UInt8] {
        encode(PacketHeader(type: .discover, seq: seq), payload: token.bytes)
    }

    public static func subscribe(token: Token, seq: UInt32 = 0) -> [UInt8] {
        encode(PacketHeader(type: .subscribe, seq: seq), payload: token.bytes)
    }

    public static func unsubscribe(token: Token, seq: UInt32 = 0) -> [UInt8] {
        encode(PacketHeader(type: .unsubscribe, seq: seq), payload: token.bytes)
    }

    public static func announce(_ a: Announce, seq: UInt32 = 0) -> [UInt8] {
        encode(PacketHeader(type: .announce, seq: seq), payload: a.encode())
    }

    public static func audio(seq: UInt32, streamId: UInt32, samplePos: UInt64, samples: [Int16]) -> [UInt8] {
        encode(PacketHeader(type: .audio, seq: seq, streamId: streamId, samplePos: samplePos),
               payload: PCM.encode(samples))
    }
}

public enum PCM {
    public static func encode(_ samples: [Int16]) -> [UInt8] {
        var out: [UInt8] = []
        out.reserveCapacity(samples.count * 2)
        for s in samples { out.appendLE(UInt16(bitPattern: s)) }
        return out
    }

    public static func decode<C: Collection>(_ payload: C) -> [Int16] where C.Element == UInt8 {
        let p = Array(payload)
        return stride(from: 0, to: p.count - 1, by: 2).map { Int16(bitPattern: p.readLE(UInt16.self, at: $0)) }
    }

    /// Float [-1, 1] → Int16 with saturation (gain already applied by the caller).
    @inline(__always)
    public static func toInt16(_ x: Float) -> Int16 {
        let v = (x * 32768).rounded()
        if v >= 32767 { return .max }
        if v <= -32768 { return .min }
        return Int16(v)
    }
}

/// True for the address ranges the sender accepts requests from by default:
/// 10/8, 172.16/12, 192.168/16, 169.254/16, 127/8. `addr` is in host byte order.
public func isPrivateIPv4(_ addr: UInt32) -> Bool {
    let a = addr >> 24, b = (addr >> 16) & 0xFF
    return a == 10 || a == 127 || (a == 172 && (16...31).contains(b)) || (a == 192 && b == 168)
        || (a == 169 && b == 254)
}

public func parseHex(_ s: String) -> [UInt8]? {
    let chars = Array(s.utf8)
    guard chars.count % 2 == 0 else { return nil }
    func nibble(_ c: UInt8) -> UInt8? {
        switch c {
        case 48...57: return c - 48
        case 97...102: return c - 87
        case 65...70: return c - 55
        default: return nil
        }
    }
    var out: [UInt8] = []
    out.reserveCapacity(chars.count / 2)
    var i = 0
    while i < chars.count {
        guard let hi = nibble(chars[i]), let lo = nibble(chars[i + 1]) else { return nil }
        out.append(hi << 4 | lo)
        i += 2
    }
    return out
}

extension Array where Element == UInt8 {
    mutating func appendLE<T: FixedWidthInteger>(_ v: T) {
        for i in 0..<MemoryLayout<T>.size { append(UInt8(truncatingIfNeeded: v >> (8 * i))) }
    }

    func readLE<T: FixedWidthInteger>(_: T.Type, at offset: Int) -> T {
        var v: T = 0
        for i in (0..<MemoryLayout<T>.size).reversed() { v = v << 8 | T(self[offset + i]) }
        return v
    }
}

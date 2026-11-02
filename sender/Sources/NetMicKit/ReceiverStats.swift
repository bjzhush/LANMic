/// Receive-side statistics for AUDIO packets: loss / reordering / duplicates by
/// seq, RFC 3550 interarrival jitter, and sender-vs-receiver clock drift from a
/// least-squares fit of arrival time against samplePos.
public struct ReceiverStats {
    public let sampleRate: Double

    public private(set) var packets: UInt64 = 0     // unique packets of the current stream
    public private(set) var duplicates: UInt64 = 0
    public private(set) var reordered: UInt64 = 0
    public private(set) var streamChanges: UInt64 = 0
    public private(set) var streamId: UInt32?
    /// RFC 3550 interarrival jitter, seconds.
    public private(set) var jitter: Double = 0

    private var firstSeq: UInt32 = 0
    private var maxSeq: UInt32 = 0
    private var seen = [UInt32](repeating: .max, count: 4096)
    private var lastTransit: Double?

    // Drift regression (Welford-style co-moments, x = sender seconds, y = arrival seconds).
    private var n: Double = 0
    private var x0: Double = 0, y0: Double = 0
    private var meanX: Double = 0, meanY: Double = 0, cxx: Double = 0, cxy: Double = 0
    private var lastX: Double = 0

    public init(sampleRate: Double = 48000) { self.sampleRate = sampleRate }

    /// Packets the sender emitted (by seq) that never arrived.
    public var lost: UInt64 {
        guard packets > 0 else { return 0 }
        let expected = UInt64(maxSeq &- firstSeq) + 1
        return expected > packets ? expected - packets : 0
    }

    public var lossRate: Double {
        let total = packets + lost
        return total == 0 ? 0 : Double(lost) / Double(total)
    }

    /// Seconds of sender audio covered by the drift regression.
    public var spanSeconds: Double { lastX }

    /// Sender clock rate relative to the receiver, in ppm (positive = sender
    /// produces samples faster). Nil until at least 10 s of audio was seen.
    public var driftPpm: Double? {
        guard n > 100, cxx > 0, lastX >= 10 else { return nil }
        let slope = cxy / cxx  // receiver seconds per sender second
        return (1 / slope - 1) * 1e6
    }

    public mutating func record(seq: UInt32, streamId: UInt32, samplePos: UInt64, arrival: Double) {
        if self.streamId != streamId {
            if self.streamId != nil { streamChanges += 1 }
            resetStream(streamId)
        }

        let slot = Int(seq % UInt32(seen.count))
        if packets > 0 && seen[slot] == seq {
            duplicates += 1
            return
        }
        seen[slot] = seq

        if packets == 0 {
            firstSeq = seq
            maxSeq = seq
        } else if Int32(bitPattern: seq &- maxSeq) > 0 {
            maxSeq = seq
        } else {
            reordered += 1
        }
        packets += 1

        let sendTime = Double(samplePos) / sampleRate
        let transit = arrival - sendTime
        if let last = lastTransit { jitter += (abs(transit - last) - jitter) / 16 }
        lastTransit = transit

        if n == 0 {
            x0 = sendTime
            y0 = arrival
        }
        let x = sendTime - x0, y = arrival - y0
        n += 1
        let dx = x - meanX
        meanX += dx / n
        meanY += (y - meanY) / n
        cxx += dx * (x - meanX)
        cxy += dx * (y - meanY)
        lastX = max(lastX, x)
    }

    private mutating func resetStream(_ id: UInt32) {
        streamId = id
        packets = 0
        duplicates = 0
        reordered = 0
        jitter = 0
        lastTransit = nil
        seen = [UInt32](repeating: .max, count: seen.count)
        n = 0
        meanX = 0
        meanY = 0
        cxx = 0
        cxy = 0
        lastX = 0
    }
}

import Darwin

/// IPv4 address + port, both in host byte order.
public struct SocketAddress: Hashable, CustomStringConvertible, Sendable {
    public var ip: UInt32
    public var port: UInt16

    public init(ip: UInt32, port: UInt16) {
        self.ip = ip
        self.port = port
    }

    /// Parses a dotted-quad IPv4 address.
    public init?(_ string: String, port: UInt16) {
        var addr = in_addr()
        guard inet_pton(AF_INET, string, &addr) == 1 else { return nil }
        self.init(ip: UInt32(bigEndian: addr.s_addr), port: port)
    }

    public static func limitedBroadcast(port: UInt16) -> SocketAddress { SocketAddress(ip: 0xFFFF_FFFF, port: port) }

    init(_ sa: sockaddr_in) {
        self.init(ip: UInt32(bigEndian: sa.sin_addr.s_addr), port: UInt16(bigEndian: sa.sin_port))
    }

    var sockaddr: sockaddr_in {
        var sa = sockaddr_in()
        sa.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
        sa.sin_family = sa_family_t(AF_INET)
        sa.sin_port = port.bigEndian
        sa.sin_addr.s_addr = ip.bigEndian
        return sa
    }

    public var ipString: String { "\(ip >> 24).\((ip >> 16) & 0xFF).\((ip >> 8) & 0xFF).\(ip & 0xFF)" }
    public var description: String { "\(ipString):\(port)" }
}

public struct SocketError: Error, CustomStringConvertible {
    public let description: String
    init(_ what: String) { description = "\(what): \(String(cString: strerror(errno)))" }
}

/// Blocking IPv4 UDP socket. send() may be called from any thread; receive()
/// from one thread at a time.
public final class UDPSocket {
    public let fd: Int32

    /// `port` 0 binds an ephemeral port.
    public init(port: UInt16 = 0, broadcast: Bool = false) throws {
        fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)
        guard fd >= 0 else { throw SocketError("socket") }
        var one: Int32 = 1
        if broadcast {
            guard setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, socklen_t(MemoryLayout<Int32>.size)) == 0 else {
                close(fd)
                throw SocketError("setsockopt(SO_BROADCAST)")
            }
        }
        // Don't die on a send to a vanished peer.
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
        var sa = SocketAddress(ip: 0, port: port).sockaddr  // INADDR_ANY
        let rc = withUnsafePointer(to: &sa) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
            }
        }
        guard rc == 0 else {
            let err = SocketError("bind(port \(port))")
            close(fd)
            throw err
        }
    }

    deinit { close(fd) }

    public var localPort: UInt16 {
        var sa = sockaddr_in()
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        _ = withUnsafeMutablePointer(to: &sa) {
            $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getsockname(fd, $0, &len) }
        }
        return UInt16(bigEndian: sa.sin_port)
    }

    @discardableResult
    public func send(_ bytes: [UInt8], to dest: SocketAddress) -> Bool {
        var sa = dest.sockaddr
        let sent = bytes.withUnsafeBytes { buf in
            withUnsafePointer(to: &sa) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                    sendto(fd, buf.baseAddress, buf.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size))
                }
            }
        }
        return sent == bytes.count
    }

    /// Waits up to `timeoutMs` for one datagram. Returns nil on timeout or error.
    public func receive(timeoutMs: Int32) -> (bytes: [UInt8], from: SocketAddress)? {
        var pfd = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
        guard poll(&pfd, 1, timeoutMs) > 0 else { return nil }
        var buf = [UInt8](repeating: 0, count: 2048)
        var sa = sockaddr_in()
        var len = socklen_t(MemoryLayout<sockaddr_in>.size)
        let n = buf.withUnsafeMutableBytes { b in
            withUnsafeMutablePointer(to: &sa) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(fd, b.baseAddress, b.count, 0, $0, &len) }
            }
        }
        guard n >= 0 else { return nil }
        buf.removeSubrange(n...)
        return (buf, SocketAddress(sa))
    }
}

/// Directed broadcast addresses of all up, non-loopback IPv4 interfaces
/// (255.255.255.255 alone only leaves through the primary interface).
public func interfaceBroadcastAddresses() -> [UInt32] {
    var list: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&list) == 0, let first = list else { return [] }
    defer { freeifaddrs(list) }
    var out: [UInt32] = []
    for ifa in sequence(first: first, next: { $0.pointee.ifa_next }) {
        let flags = Int32(ifa.pointee.ifa_flags)
        guard flags & IFF_UP != 0, flags & IFF_BROADCAST != 0, flags & IFF_LOOPBACK == 0,
              let addr = ifa.pointee.ifa_addr, addr.pointee.sa_family == sa_family_t(AF_INET),
              let bcast = ifa.pointee.ifa_dstaddr  // ifa_broadaddr
        else { continue }
        let ip = bcast.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { UInt32(bigEndian: $0.pointee.sin_addr.s_addr) }
        if ip != 0, !out.contains(ip) { out.append(ip) }
    }
    return out
}

/// Monotonic seconds on the mach_absolute_time clock (the clock Core Audio uses).
public func hostSeconds() -> Double {
    Double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) * 1e-9
}

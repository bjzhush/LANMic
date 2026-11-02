import Foundation
import NetMicKit
import NetMicProtocol
import SenderCore
import Testing

/// The lifecycle the menu-bar app relies on: run on a thread, subscribe, stream,
/// toggle always-on, stop() → run() returns, source stopped, port free again.
@Suite struct ServerLifecycleTests {
    @Test func runStopAndRestart() throws {
        logSink = { _ in }
        let port = UInt16.random(in: 47900...48900)
        for _ in 0..<2 {  // the second round proves stop() released the port
            let tone = ToneSource(frequency: 440)
            let server = try Server(options: .init(port: port, name: "Test", stopGrace: 0.2), capture: tone)
            let done = DispatchSemaphore(value: 0)
            Thread { server.run(); done.signal() }.start()

            let client = try UDPSocket()
            let dest = try #require(SocketAddress("127.0.0.1", port: port))
            #expect(client.send(Packet.subscribe(token: .zero), to: dest))
            var audio = 0
            let deadline = hostSeconds() + 1
            while hostSeconds() < deadline && audio < 10 {
                if let (bytes, _) = client.receive(timeoutMs: 100),
                   let (h, _) = try? Packet.decode(bytes), h.type == .audio { audio += 1 }
            }
            #expect(audio >= 10)
            #expect(server.status().subscribers.count == 1)
            #expect(server.status().capturing)

            // Unsubscribed + always-on: keeps capturing past the stop grace.
            server.alwaysOn = true
            #expect(client.send(Packet.unsubscribe(token: .zero), to: dest))
            Thread.sleep(forTimeInterval: 0.5)
            #expect(server.status().subscribers.isEmpty)
            #expect(tone.isRunning)
            server.alwaysOn = false
            Thread.sleep(forTimeInterval: 0.5)
            #expect(!tone.isRunning)

            server.alwaysOn = true
            Thread.sleep(forTimeInterval: 0.3)
            #expect(tone.isRunning)
            server.stop()
            #expect(done.wait(timeout: .now() + 1) == .success)
            #expect(!tone.isRunning)
        }
    }
}

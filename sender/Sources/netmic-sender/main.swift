import AVFoundation
import Foundation
import NetMicProtocol
import SenderCore

let usage = """
    usage:
      netmic-sender --list-devices
      netmic-sender --device <name substring or UID> | --tone <Hz>
                    [--name "M4 Mic"] [--port 47810]
                    [--channel 0|<n>|mix] [--gain-db 0] [--io-frames 240] [--token <32 hex digits>]
                    [--always-on] [--stop-grace 5] [--allow-any-source] [-v]

    Captures only while at least one receiver is subscribed (unless --always-on);
    stops --stop-grace seconds after the last subscriber leaves.
    --tone sends a -12 dBFS sine instead of the microphone (for testing).
    --io-frames asks the input device for this IO buffer size (frames at the
    device rate; 0 = leave the device default). Smaller buffers make packets
    leave evenly every 5 ms instead of in bursts, so the receiver needs less buffering.
    """

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data("netmic-sender: \(message)\n".utf8))
    exit(2)
}

func need<T>(_ value: T?, _ message: @autoclosure () -> String) -> T {
    guard let value else { fail(message()) }
    return value
}

setvbuf(stdout, nil, _IOLBF, 0)

var args = Array(CommandLine.arguments.dropFirst())
func takeValue(_ flag: String) -> String {
    guard !args.isEmpty else { fail("\(flag) needs a value") }
    return args.removeFirst()
}

var listDevices = false
var deviceQuery: String?
var toneHz: Double?
var name = "M4 Mic"
var port = NetMic.defaultPort
var channel: Int32 = 0
var gainDb: Float = 0
var ioFrames: UInt32 = 240
var token = Token.zero
var alwaysOn = false
var stopGrace = 5.0
var allowAnySource = false
var verbose = false

while !args.isEmpty {
    let a = args.removeFirst()
    switch a {
    case "--list-devices": listDevices = true
    case "--device": deviceQuery = takeValue(a)
    case "--tone": toneHz = need(Double(takeValue(a)).flatMap { $0 > 0 && $0 < 24000 ? $0 : nil }, "bad --tone")
    case "--name": name = takeValue(a)
    case "--port": port = need(UInt16(takeValue(a)), "bad --port")
    case "--channel":
        let v = takeValue(a)
        channel = v == "mix" ? -1 : need(Int32(v).flatMap { $0 >= 0 ? $0 : nil }, "bad --channel \(v)")
    case "--gain-db": gainDb = need(Float(takeValue(a)), "bad --gain-db")
    case "--io-frames": ioFrames = need(UInt32(takeValue(a)), "bad --io-frames")
    case "--token": token = need(Token(hex: takeValue(a)), "--token must be 32 hex digits")
    case "--always-on": alwaysOn = true
    case "--stop-grace": stopGrace = need(Double(takeValue(a)).flatMap { $0 >= 0 ? $0 : nil }, "bad --stop-grace")
    case "--allow-any-source": allowAnySource = true
    case "-v", "--verbose": verbose = true
    case "-h", "--help":
        print(usage)
        exit(0)
    default: fail("unknown argument \(a)\n\(usage)")
    }
}

if listDevices {
    let devices = listInputDevices()
    if devices.isEmpty { print("no input devices found") }
    for d in devices {
        print("\(d.name)\n    UID: \(d.uid)\n    \(Int(d.sampleRate)) Hz, \(d.inputChannels) input channel(s)")
    }
    exit(0)
}

let source: AudioSource
if let toneHz {
    let tone = ToneSource(frequency: toneHz)
    tone.log = { log($0) }
    source = tone
} else {
    guard let deviceQuery else { fail("--device or --tone is required\n\(usage)") }

    switch findInputDevice(deviceQuery) {
    case .found(let d):
        if channel >= d.inputChannels { fail("--channel \(channel) out of range: \"\(d.name)\" has \(d.inputChannels) channel(s)") }
        log("device: \"\(d.name)\" (\(d.uid))")
    case .notFound: fail("no input device matches \"\(deviceQuery)\" (see --list-devices)")
    case .ambiguous(let list): fail("\"\(deviceQuery)\" matches several devices: \(list.map(\.name).joined(separator: ", ")); use the UID")
    }

    // Ask for microphone access up front so the prompt doesn't appear mid-session.
    // From a terminal, the permission belongs to the terminal app.
    switch AVCaptureDevice.authorizationStatus(for: .audio) {
    case .authorized: break
    case .notDetermined:
        let sem = DispatchSemaphore(value: 0)
        var granted = false
        AVCaptureDevice.requestAccess(for: .audio) { granted = $0; sem.signal() }
        sem.wait()
        if !granted { fail("microphone access denied") }
    default:
        fail("microphone access denied — enable your terminal app in System Settings → Privacy & Security → Microphone")
    }

    let capture = Capture(options: .init(deviceQuery: deviceQuery, channel: channel, gainDb: gainDb,
                                          ioFrames: ioFrames))
    capture.log = { log($0) }
    source = capture
}

signal(SIGINT) { _ in exit(0) }  // the system releases the microphone on exit
signal(SIGTERM) { _ in exit(0) }

do {
    let server = try Server(options: .init(port: port, name: name, token: token, alwaysOn: alwaysOn,
                                           stopGrace: stopGrace, allowAnySource: allowAnySource, verbose: verbose),
                            capture: source)
    server.run()
    exit(0)
} catch {
    fail("\(error)")
}

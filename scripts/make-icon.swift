// Draws the NetMic Sender app icon and writes an .icns.
//   swift scripts/make-icon.swift sender/App/AppIcon.icns [preview.png]
// The .icns is checked in; rerun this only after changing the drawing.
import AppKit

func color(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat(hex >> 16 & 255) / 255, green: CGFloat(hex >> 8 & 255) / 255,
            blue: CGFloat(hex & 255) / 255, alpha: alpha)
}

/// All coordinates are on a 1024 x 1024 canvas, origin at the bottom left.
func draw(_ c: CGContext, pixels: Int) {
    let srgb = CGColorSpace(name: CGColorSpace.sRGB)!
    let k = CGFloat(pixels) / 1024
    c.scaleBy(x: k, y: k)

    // Body: 824 pt rounded square with a soft shadow.
    let body = CGPath(roundedRect: CGRect(x: 100, y: 100, width: 824, height: 824),
                      cornerWidth: 186, cornerHeight: 186, transform: nil)
    c.saveGState()
    // Shadow parameters are in device pixels, not scaled by the CTM.
    c.setShadow(offset: CGSize(width: 0, height: -10 * k), blur: 24 * k, color: color(0x000000, 0.35))
    c.addPath(body)
    c.setFillColor(color(0x12306B))
    c.fillPath()
    c.restoreGState()
    c.saveGState()
    c.addPath(body)
    c.clip()
    let bg = CGGradient(colorsSpace: srgb, colors: [color(0x17B8B5), color(0x1769C2), color(0x1B2A78)] as CFArray,
                        locations: [0, 0.5, 1])!
    c.drawLinearGradient(bg, start: CGPoint(x: 250, y: 924), end: CGPoint(x: 774, y: 100),
                         options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
    c.restoreGState()

    c.setLineCap(.round)
    let mx: CGFloat = 392

    // Sound waves leaving to the right.
    let center = CGPoint(x: mx, y: 605)
    let waves: [(CGFloat, CGFloat)] = [(290, 1), (364, 0.7), (438, 0.4)]
    for (radius, alpha) in waves {
        c.setStrokeColor(color(0x8FF5E6, alpha))
        c.setLineWidth(40)
        c.addArc(center: center, radius: radius, startAngle: -.pi / 6.3, endAngle: .pi / 6.3, clockwise: false)
        c.strokePath()
    }

    // Microphone: capsule, cradle, stem, base.
    c.setFillColor(color(0xFFFFFF))
    c.addPath(CGPath(roundedRect: CGRect(x: mx - 100, y: 455, width: 200, height: 345),
                     cornerWidth: 100, cornerHeight: 100, transform: nil))
    c.fillPath()
    c.setStrokeColor(color(0xFFFFFF))
    c.setLineWidth(46)
    c.move(to: CGPoint(x: mx - 172, y: 560))
    c.addArc(center: CGPoint(x: mx, y: 545), radius: 172, startAngle: .pi, endAngle: 0, clockwise: false)
    c.addLine(to: CGPoint(x: mx + 172, y: 560))
    c.strokePath()
    c.move(to: CGPoint(x: mx, y: 373))
    c.addLine(to: CGPoint(x: mx, y: 255))
    c.move(to: CGPoint(x: mx - 105, y: 255))
    c.addLine(to: CGPoint(x: mx + 105, y: 255))
    c.strokePath()
}

func png(_ pixels: Int) -> Data {
    let c = CGContext(data: nil, width: pixels, height: pixels, bitsPerComponent: 8, bytesPerRow: 0,
                      space: CGColorSpace(name: CGColorSpace.sRGB)!,
                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    draw(c, pixels: pixels)
    return NSBitmapImageRep(cgImage: c.makeImage()!).representation(using: .png, properties: [:])!
}

let args = CommandLine.arguments
guard args.count >= 2 else { fatalError("usage: make-icon.swift out.icns [preview.png]") }
let set = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString).iconset")
try FileManager.default.createDirectory(at: set, withIntermediateDirectories: true)
// No 16/32 px versions: macOS 27 puts a grey plate behind them but scales the larger ones cleanly.
for size in [128, 256, 512] {
    try png(size).write(to: set.appendingPathComponent("icon_\(size)x\(size).png"))
    try png(size * 2).write(to: set.appendingPathComponent("icon_\(size)x\(size)@2x.png"))
}
let tool = Process()
tool.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
tool.arguments = ["-c", "icns", set.path, "-o", args[1]]
try tool.run()
tool.waitUntilExit()
try? FileManager.default.removeItem(at: set)
guard tool.terminationStatus == 0 else { fatalError("iconutil failed") }
if args.count > 2 { try png(1024).write(to: URL(fileURLWithPath: args[2])) }

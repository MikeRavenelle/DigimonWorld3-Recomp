// Draws the project icon (a digital egg that dissolves into pixels, on a deep blue tile) and writes square PNGs.
// Usage: swift make_icons.swift <out dir> [size ...]   (default sizes: 16 24 32 48 64 128 256 512 1024)
// Size 128 also writes icon-128.rgba (raw RGBA).
// Run by packaging/icons/make_icons.sh; the PNGs, .icns and .ico it produces are committed, so builds need no Swift.
import AppKit

func gradient(_ a: (CGFloat, CGFloat, CGFloat), _ b: (CGFloat, CGFloat, CGFloat)) -> NSGradient {
    NSGradient(starting: NSColor(red: a.0, green: a.1, blue: a.2, alpha: 1), ending: NSColor(red: b.0, green: b.1, blue: b.2, alpha: 1))!
}

func render(_ px: Int) -> (png: Data, rgba: Data) {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: px, pixelsHigh: px, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let s = CGFloat(px)

    // Tile.
    let tile = NSRect(x: s * 0.04, y: s * 0.04, width: s * 0.92, height: s * 0.92)
    let tilePath = NSBezierPath(roundedRect: tile, xRadius: s * 0.205, yRadius: s * 0.205)
    gradient((0.10, 0.20, 0.55), (0.02, 0.05, 0.22)).draw(in: tilePath, angle: -90)

    NSGraphicsContext.saveGraphicsState()
    tilePath.addClip()

    // Faint pixel grid behind the egg.
    let cell = s / 16
    NSColor(white: 1, alpha: 0.045).setFill()
    for gx in 0..<16 { for gy in 0..<16 where (gx + gy) % 2 == 0 {
        NSRect(x: CGFloat(gx) * cell, y: CGFloat(gy) * cell, width: cell, height: cell).fill()
    } }

    // Egg: wide at the bottom, narrower at the top.
    let cx = s * 0.5, cy = s * 0.5
    let w = s * 0.50, h = s * 0.64
    let egg = NSBezierPath()
    let top = NSPoint(x: cx, y: cy + h / 2), bottom = NSPoint(x: cx, y: cy - h / 2)
    egg.move(to: top)
    egg.curve(to: NSPoint(x: cx + w / 2, y: cy - h * 0.12), controlPoint1: NSPoint(x: cx + w * 0.34, y: cy + h / 2),
              controlPoint2: NSPoint(x: cx + w / 2, y: cy + h * 0.16))
    egg.curve(to: bottom, controlPoint1: NSPoint(x: cx + w / 2, y: cy - h * 0.36), controlPoint2: NSPoint(x: cx + w * 0.30, y: cy - h / 2))
    egg.curve(to: NSPoint(x: cx - w / 2, y: cy - h * 0.12), controlPoint1: NSPoint(x: cx - w * 0.30, y: cy - h / 2),
              controlPoint2: NSPoint(x: cx - w / 2, y: cy - h * 0.36))
    egg.curve(to: top, controlPoint1: NSPoint(x: cx - w / 2, y: cy + h * 0.16), controlPoint2: NSPoint(x: cx - w * 0.34, y: cy + h / 2))
    egg.close()

    // Glow.
    NSGraphicsContext.saveGraphicsState()
    let glow = NSShadow()
    glow.shadowColor = NSColor(red: 0.25, green: 0.85, blue: 1, alpha: 0.75)
    glow.shadowBlurRadius = s * 0.07
    glow.shadowOffset = .zero
    glow.set()
    NSColor(red: 0.3, green: 0.85, blue: 1, alpha: 1).setFill()
    egg.fill()
    NSGraphicsContext.restoreGraphicsState()

    // Egg body.
    gradient((1.0, 0.86, 0.35), (0.98, 0.42, 0.12)).draw(in: egg, angle: -90)

    // Dissolve: the lower part of the egg becomes blocks that fall away (the "recompiled" idea).
    NSGraphicsContext.saveGraphicsState()
    egg.addClip()
    let blocks = 22
    let b = w / CGFloat(blocks) * 1.0
    let startY = cy - h * 0.12
    let bgA = NSColor(red: 0.07, green: 0.14, blue: 0.42, alpha: 1)
    for row in 0..<14 {
        let y = startY - CGFloat(row) * b
        let cut = Double(row) / 14.0           // probability that a cell is cut away grows downwards
        for col in 0..<blocks {
            let x = cx - w / 2 + CGFloat(col) * b
            let hash = abs(sin(Double(col) * 12.9898 + Double(row) * 78.233) * 43758.5453).truncatingRemainder(dividingBy: 1)
            if hash < cut * 0.9 { bgA.setFill(); NSRect(x: x, y: y, width: b + 0.5, height: b + 0.5).fill() }
        }
    }
    NSGraphicsContext.restoreGraphicsState()

    // Loose blocks drifting off the bottom of the egg.
    let drift: [(CGFloat, CGFloat, CGFloat)] = [(-0.17, -0.40, 0.040), (0.03, -0.44, 0.032), (0.17, -0.37, 0.036), (-0.06, -0.455, 0.026), (0.26, -0.45, 0.024)]
    for (dx, dy, sz) in drift {
        NSColor(red: 1.0, green: 0.62, blue: 0.2, alpha: 1).setFill()
        NSRect(x: cx + dx * s, y: cy + dy * s, width: sz * s, height: sz * s).fill()
    }

    // The "3", white with a soft shadow, on the upper egg.
    let digit = NSAttributedString(string: "3", attributes: [
        .font: NSFont.systemFont(ofSize: s * 0.34, weight: .black), .foregroundColor: NSColor.white,
        .shadow: { let sh = NSShadow(); sh.shadowColor = NSColor(white: 0, alpha: 0.35); sh.shadowBlurRadius = s * 0.015
                   sh.shadowOffset = NSSize(width: 0, height: -s * 0.01); return sh }(),
    ])
    let ds = digit.size()
    digit.draw(at: NSPoint(x: cx - ds.width / 2, y: cy + h * 0.10 - ds.height / 2))

    NSGraphicsContext.restoreGraphicsState()
    NSGraphicsContext.restoreGraphicsState()
    let raw = Data(bytes: rep.bitmapData!, count: px * px * 4)  // straight (non-premultiplied) RGBA, rows top to bottom
    return (rep.representation(using: .png, properties: [:])!, raw)
}

let args = CommandLine.arguments
let out = args[1]
let sizes = args.count > 2 ? args[2...].compactMap { Int($0) } : [16, 24, 32, 48, 64, 128, 256, 512, 1024]
try FileManager.default.createDirectory(atPath: out, withIntermediateDirectories: true)
for px in sizes {
    let image = render(px)
    try image.png.write(to: URL(fileURLWithPath: "\(out)/icon-\(px).png"))
    if px == 128 { try image.rgba.write(to: URL(fileURLWithPath: "\(out)/icon-128.rgba")) }  // the window icon embedded in the executable
}

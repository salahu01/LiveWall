#!/usr/bin/env swift
//
// Draws Resources/AppIcon.icns.
//
//   swift tools/make-icon.swift
//
// Generated rather than hand-drawn so the icon is reproducible and lives in the
// repo as code. The motif is the app's own procedural gradient — the same deep
// violet-to-teal field the shader draws — behind a rounded display shape.

import AppKit
import CoreGraphics
import Foundation

let root = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
let iconset = root.appendingPathComponent("build/AppIcon.iconset")
let output = root.appendingPathComponent("Resources/AppIcon.icns")

try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)

func draw(size: Int) -> Data {
    let dimension = CGFloat(size)
    let scale = dimension / 1024

    let space = CGColorSpaceCreateDeviceRGB()
    guard let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8,
                              bytesPerRow: 0, space: space,
                              bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else {
        fatalError("could not create a bitmap context")
    }

    ctx.setAllowsAntialiasing(true)
    ctx.interpolationQuality = .high

    // macOS app icons sit inset inside their canvas with a squircle-ish corner.
    let inset = 100 * scale
    let rect = CGRect(x: inset, y: inset, width: dimension - inset * 2, height: dimension - inset * 2)
    let radius = rect.width * 0.2237

    let body = CGPath(roundedRect: rect, cornerWidth: radius, cornerHeight: radius, transform: nil)
    ctx.addPath(body)
    ctx.clip()

    // Base gradient: the deep violet and teal from wallpaper.metal.
    let colors = [
        CGColor(red: 0.09, green: 0.07, blue: 0.20, alpha: 1),
        CGColor(red: 0.20, green: 0.12, blue: 0.38, alpha: 1),
        CGColor(red: 0.04, green: 0.24, blue: 0.30, alpha: 1)
    ] as CFArray
    if let gradient = CGGradient(colorsSpace: space, colors: colors, locations: [0, 0.55, 1]) {
        ctx.drawLinearGradient(gradient,
                               start: CGPoint(x: rect.minX, y: rect.maxY),
                               end: CGPoint(x: rect.maxX, y: rect.minY),
                               options: [])
    }

    // Two drifting bands, echoing the shader's crossed sinusoids. Drawn as
    // filled waves so they stay legible when the icon is 16 points wide.
    for (index, alpha) in [(0, 0.30), (1, 0.16)] {
        let phase = CGFloat(index) * .pi * 0.8
        let amplitude = rect.height * (0.075 - CGFloat(index) * 0.02)
        let midline = rect.midY + rect.height * (CGFloat(index) * 0.20 - 0.06)

        let wave = CGMutablePath()
        wave.move(to: CGPoint(x: rect.minX, y: rect.minY))
        var x = rect.minX
        while x <= rect.maxX {
            let t = (x - rect.minX) / rect.width
            let y = midline + sin(t * .pi * 2.4 + phase) * amplitude
            wave.addLine(to: CGPoint(x: x, y: y))
            x += max(1, scale * 4)
        }
        wave.addLine(to: CGPoint(x: rect.maxX, y: rect.minY))
        wave.closeSubpath()

        ctx.setFillColor(CGColor(red: 0.72, green: 0.55, blue: 1.0, alpha: CGFloat(alpha)))
        ctx.addPath(wave)
        ctx.fillPath()
    }

    // A soft highlight along the top edge so the shape reads as glass rather
    // than a flat tile.
    if let sheen = CGGradient(colorsSpace: space, colors: [
        CGColor(red: 1, green: 1, blue: 1, alpha: 0.16),
        CGColor(red: 1, green: 1, blue: 1, alpha: 0)
    ] as CFArray, locations: [0, 1]) {
        ctx.drawLinearGradient(sheen,
                               start: CGPoint(x: rect.midX, y: rect.maxY),
                               end: CGPoint(x: rect.midX, y: rect.midY),
                               options: [])
    }

    ctx.resetClip()
    ctx.addPath(body)
    ctx.setStrokeColor(CGColor(red: 1, green: 1, blue: 1, alpha: 0.10))
    ctx.setLineWidth(max(1, 3 * scale))
    ctx.strokePath()

    guard let image = ctx.makeImage() else { fatalError("could not render at \(size)") }
    let rep = NSBitmapImageRep(cgImage: image)
    guard let data = rep.representation(using: .png, properties: [:]) else {
        fatalError("could not encode PNG at \(size)")
    }
    return data
}

// The set macOS expects; anything missing shows as a generic icon at that size.
for base in [16, 32, 128, 256, 512] {
    try draw(size: base).write(to: iconset.appendingPathComponent("icon_\(base)x\(base).png"))
    try draw(size: base * 2).write(to: iconset.appendingPathComponent("icon_\(base)x\(base)@2x.png"))
}

let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", iconset.path, "-o", output.path]
try iconutil.run()
iconutil.waitUntilExit()
guard iconutil.terminationStatus == 0 else { exit(iconutil.terminationStatus) }

try? FileManager.default.removeItem(at: iconset)
let bytes = (try? FileManager.default.attributesOfItem(atPath: output.path))?[.size] as? Int ?? 0
print("wrote Resources/AppIcon.icns (\(bytes) bytes)")

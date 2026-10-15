// Copyright (C) 2026 AV Vibes. Part of AV Dexed, a modified version of Dexed.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Draws an SVG logo on a transparent PNG, for the installer background.
//   swift render_logo.swift logo.svg out.png canvasW canvasH logoW logoX logoY scale
// Sizes are in points; scale 2 makes the Retina image. logoX/logoY place the
// logo's bottom-left corner (origin at the bottom left, like the installer).

import AppKit

let args = CommandLine.arguments
let numbers = args.dropFirst(3).compactMap(Double.init)
guard args.count == 9, numbers.count == 6, let logo = NSImage(contentsOfFile: args[1]) else {
    FileHandle.standardError.write("usage: render_logo.swift logo.svg out.png canvasW canvasH logoW logoX logoY scale\n".data(using: .utf8)!)
    exit(1)
}
let (width, height, logoWidth, logoX, logoY, scale) =
    (numbers[0], numbers[1], numbers[2], numbers[3], numbers[4], numbers[5])

let context = CGContext(data: nil, width: Int(width * scale), height: Int(height * scale),
                        bitsPerComponent: 8, bytesPerRow: 0,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
context.scaleBy(x: scale, y: scale)
context.interpolationQuality = .high
NSGraphicsContext.current = NSGraphicsContext(cgContext: context, flipped: false)
let logoHeight = logoWidth * logo.size.height / logo.size.width
logo.draw(in: NSRect(x: logoX, y: logoY, width: logoWidth, height: logoHeight))

let image = NSBitmapImageRep(cgImage: context.makeImage()!)
image.size = NSSize(width: width, height: height)   // 144 dpi at scale 2
guard let png = image.representation(using: .png, properties: [:]),
      (try? png.write(to: URL(fileURLWithPath: args[2]))) != nil else {
    FileHandle.standardError.write("could not write \(args[2])\n".data(using: .utf8)!)
    exit(1)
}

import Foundation
import AVFoundation
import CoreGraphics
import ImageIO
import UniformTypeIdentifiers

let args = CommandLine.arguments
let src = args[1], prefix = args[2]
let times = args[3...].compactMap { Double($0) }
let asset = AVURLAsset(url: URL(fileURLWithPath: src))
let gen = AVAssetImageGenerator(asset: asset)
gen.appliesPreferredTrackTransform = true
gen.requestedTimeToleranceBefore = .zero
gen.requestedTimeToleranceAfter = .zero
let dur = CMTimeGetSeconds(asset.duration)
print(String(format: "duration %.2fs", dur))
for t in times {
    let tt = CMTimeMakeWithSeconds(min(t, dur - 0.05), preferredTimescale: 600)
    do {
        let img = try gen.copyCGImage(at: tt, actualTime: nil)
        let out = String(format: "%@_%05.2f.png", prefix, t)
        let url = URL(fileURLWithPath: out)
        if let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil) {
            CGImageDestinationAddImage(dest, img, nil)
            CGImageDestinationFinalize(dest)
            print("wrote \(out)")
        }
    } catch { print("fail at \(t): \(error)") }
}

import AppKit
import Foundation

// Headless conversion mode, mainly so the import pipeline can be exercised and
// verified without driving the UI:
//
//   LiveWall --convert <source> <destination.mov> [ultra|balanced|fidelity]
//
if CommandLine.arguments.contains("--convert") {
    runHeadlessConversion()
} else if alreadyRunning() {
    // Two copies would mean two desktop windows per screen and two decoders,
    // with the second stacked invisibly over the first. LaunchServices
    // coalesces `open` on a bundle, but nothing stops a login-item launch
    // racing a manual one, or the executable being run directly.
    FileHandle.standardError.write("LiveWall is already running.\n".data(using: .utf8)!)
    exit(0)
} else {
    // Accessory policy: no dock icon, no menu bar ownership, no main window.
    // Everything the app does hangs off the status item.
    let app = NSApplication.shared
    app.setActivationPolicy(.accessory)

    let delegate = AppDelegate()
    app.delegate = delegate
    app.run()
}

/// Another process with our bundle identifier is already up.
func alreadyRunning() -> Bool {
    guard let id = Bundle.main.bundleIdentifier else { return false }
    let mine = ProcessInfo.processInfo.processIdentifier
    return NSRunningApplication.runningApplications(withBundleIdentifier: id)
        .contains { $0.processIdentifier != mine }
}

func runHeadlessConversion() -> Never {
    let args = CommandLine.arguments
    guard let flagIndex = args.firstIndex(of: "--convert"), args.count > flagIndex + 2 else {
        FileHandle.standardError.write(
            "usage: LiveWall --convert <source> <destination.mov> [ultra|balanced|fidelity]\n"
                .data(using: .utf8)!)
        exit(2)
    }

    let source = URL(fileURLWithPath: args[flagIndex + 1])
    let destination = URL(fileURLWithPath: args[flagIndex + 2])

    let preset: Transcoder.Preset
    switch args.count > flagIndex + 3 ? args[flagIndex + 3] : "balanced" {
    case "ultra": preset = .ultraLight
    case "native", "fidelity": preset = .native
    default: preset = .balanced
    }

    // Headless runs still size against the real panel when there is one, so
    // `--convert` produces the same file the menu would.
    let display = MainActor.assumeIsolated { Transcoder.DisplayTarget.main() }

    let semaphore = DispatchSemaphore(value: 0)
    var exitCode: Int32 = 0
    var lastPercent = -1

    Task {
        do {
            let result = try await Transcoder.convert(
                source: source, destination: destination, preset: preset, display: display,
                progress: { fraction in
                    let percent = Int(fraction * 100)
                    if percent != lastPercent, percent % 10 == 0 {
                        lastPercent = percent
                        FileHandle.standardError.write("\(percent)%\n".data(using: .utf8)!)
                    }
                })
            print("\(Int(result.size.width))x\(Int(result.size.height)) @ \(result.fps) fps, "
                  + "\(result.bitDepth)-bit, "
                  + "\(ByteCountFormatter.string(fromByteCount: result.byteCount, countStyle: .file))")
        } catch {
            FileHandle.standardError.write("error: \(error.localizedDescription)\n".data(using: .utf8)!)
            exitCode = 1
        }
        semaphore.signal()
    }

    semaphore.wait()
    exit(exitCode)
}

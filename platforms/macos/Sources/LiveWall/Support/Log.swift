import Foundation

/// Minimal stderr logger. No os.log — avoids the logging subsystem's buffers.
enum Log {
    static var verbose = ProcessInfo.processInfo.environment["LIVEWALL_VERBOSE"] != nil

    static func info(_ message: @autoclosure () -> String) {
        guard verbose else { return }
        FileHandle.standardError.write("[livewall] \(message())\n".data(using: .utf8)!)
    }

    static func error(_ message: @autoclosure () -> String) {
        FileHandle.standardError.write("[livewall] ERROR \(message())\n".data(using: .utf8)!)
    }
}

import Foundation
import AVFoundation

/// On-disk store of converted wallpapers.
///
/// Originals are never copied or modified — only the normalised output lives
/// here, so the library size stays proportional to what actually plays.
struct WallpaperItem: Codable, Equatable {
    let id: UUID
    var title: String
    var filename: String
    var width: Int
    var height: Int
    var fps: Int
    var byteCount: Int64
    var addedAt: Date
    /// Optional so that an index written before 10-bit support still decodes —
    /// a non-optional addition would fail the whole array and silently empty the
    /// library. Absent means 8-bit, which is what those files are.
    var bitDepth: Int?

    var pixelBitDepth: Int { bitDepth ?? 8 }

    var resolutionLabel: String { "\(width)×\(height) · \(fps) fps · \(pixelBitDepth)-bit" }

    var sizeLabel: String {
        ByteCountFormatter.string(fromByteCount: byteCount, countStyle: .file)
    }
}

final class Library {

    private(set) var items: [WallpaperItem] = []

    let directory: URL
    private let indexURL: URL
    private let defaults = UserDefaults.standard
    private static let selectionKey = "selectedWallpaperID"
    private static let presetKey = "importPresetName"
    private static let pauseOnBatteryKey = "pauseOnBattery"
    private static let fitModeKey = "fitMode"

    init() {
        let support = FileManager.default.urls(for: .applicationSupportDirectory,
                                               in: .userDomainMask)[0]
        directory = support.appendingPathComponent("LiveWall/library", isDirectory: true)
        indexURL = support.appendingPathComponent("LiveWall/index.json")
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        load()
    }

    // MARK: - Persistence

    private func load() {
        guard let data = try? Data(contentsOf: indexURL), !data.isEmpty else { return }

        let outcome = Self.decodeIndex(data)

        // A file that parses as JSON but yields nothing usable is more likely a
        // bug or a bad migration than an empty library, and `save()` would
        // overwrite it moments later. Keep a copy before that happens.
        if outcome.items.isEmpty && outcome.dropped > 0 {
            let backup = indexURL.deletingPathExtension().appendingPathExtension("corrupt.json")
            try? FileManager.default.removeItem(at: backup)
            try? FileManager.default.copyItem(at: indexURL, to: backup)
            Log.error("index unreadable — kept a copy at \(backup.lastPathComponent)")
        } else if outcome.dropped > 0 {
            Log.error("skipped \(outcome.dropped) unreadable entries in the library index")
        }

        items = outcome.items
        // Drop entries whose file vanished (manual delete, migration…).
        items.removeAll { !FileManager.default.fileExists(atPath: url(for: $0).path) }
    }

    /// Decodes the index one entry at a time.
    ///
    /// Decoding straight into `[WallpaperItem]` is all-or-nothing: a single
    /// malformed entry — one bad id, one field written by a future version —
    /// throws for the whole array and the library silently reads as empty, which
    /// then looks to the user like every wallpaper they imported has vanished.
    /// One bad row should cost one row.
    static func decodeIndex(_ data: Data) -> (items: [WallpaperItem], dropped: Int) {
        /// Succeeds even when the entry inside it doesn't, so one failure can't
        /// abort the array.
        struct Lenient: Decodable {
            let item: WallpaperItem?
            init(from decoder: Decoder) throws {
                item = try? WallpaperItem(from: decoder)
            }
        }

        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601

        guard let rows = try? decoder.decode([Lenient].self, from: data) else {
            return ([], 1)
        }
        let items = rows.compactMap(\.item)
        return (items, rows.count - items.count)
    }

    private func save() {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = .prettyPrinted
        guard let data = try? encoder.encode(items) else { return }
        try? data.write(to: indexURL, options: .atomic)
    }

    // MARK: - Items

    func url(for item: WallpaperItem) -> URL {
        directory.appendingPathComponent(item.filename)
    }

    func destinationURL(id: UUID) -> URL {
        directory.appendingPathComponent("\(id.uuidString).mov")
    }

    func add(_ item: WallpaperItem) {
        items.removeAll { $0.id == item.id }
        items.append(item)
        items.sort { $0.addedAt > $1.addedAt }
        save()
    }

    func remove(_ item: WallpaperItem) {
        try? FileManager.default.removeItem(at: url(for: item))
        items.removeAll { $0.id == item.id }
        if selectedID == item.id { selectedID = nil }
        save()
    }

    func item(withID id: UUID) -> WallpaperItem? {
        items.first { $0.id == id }
    }

    var totalBytes: Int64 {
        items.reduce(0) { $0 + $1.byteCount }
    }

    // MARK: - Settings

    var selectedID: UUID? {
        get {
            guard let raw = defaults.string(forKey: Self.selectionKey) else { return nil }
            return UUID(uuidString: raw)
        }
        set {
            if let newValue {
                defaults.set(newValue.uuidString, forKey: Self.selectionKey)
            } else {
                defaults.removeObject(forKey: Self.selectionKey)
            }
        }
    }

    var preset: Transcoder.Preset {
        get {
            let name = defaults.string(forKey: Self.presetKey) ?? Transcoder.Preset.balanced.name
            // "Fidelity" was the old top preset. Anyone who chose it wanted the
            // best available, which is now "Native".
            if name == "Fidelity" { return .native }
            return Transcoder.Preset.all.first { $0.name == name } ?? .balanced
        }
        set { defaults.set(newValue.name, forKey: Self.presetKey) }
    }

    var pauseOnBattery: Bool {
        get { defaults.bool(forKey: Self.pauseOnBatteryKey) }
        set { defaults.set(newValue, forKey: Self.pauseOnBatteryKey) }
    }

    var fitMode: FitMode {
        get {
            guard let raw = defaults.string(forKey: Self.fitModeKey) else { return .default }
            return FitMode(rawValue: raw) ?? .default
        }
        set { defaults.set(newValue.rawValue, forKey: Self.fitModeKey) }
    }
}

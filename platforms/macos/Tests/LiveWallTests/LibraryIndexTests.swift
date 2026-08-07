import Foundation
import Testing
@testable import LiveWall

/// The index is the only record that a wallpaper was ever imported. Decoding it
/// used to be all-or-nothing, so a single bad row read as "your library is
/// empty" — and the next save wrote that emptiness to disk.
struct LibraryIndexTests {

    private func json(_ entries: [String]) -> Data {
        Data("[\(entries.joined(separator: ","))]".utf8)
    }

    private let valid = """
        {"id":"C144E512-4B3E-4FE6-B6CB-071F4C7B8237","title":"vegeta","filename":"a.mov",
         "width":1920,"height":1080,"fps":24,"byteCount":123,"addedAt":"2026-08-06T12:49:04Z"}
        """

    private let alsoValid = """
        {"id":"6062DC86-29E7-4567-91DE-299207E82D2E","title":"itachi","filename":"b.mov",
         "width":3492,"height":1964,"fps":24,"byteCount":456,"addedAt":"2026-08-06T12:22:29Z",
         "bitDepth":10}
        """

    /// The exact shape that broke it: `id` must be a UUID, and a bench harness
    /// wrote a plain string.
    private let badID = """
        {"id":"BENCH-r4","title":"bench","filename":"c.mov",
         "width":1280,"height":720,"fps":24,"byteCount":789,"addedAt":"2026-08-06T12:00:00Z"}
        """

    @Test func readsAWellFormedIndex() {
        let outcome = Library.decodeIndex(json([valid, alsoValid]))
        #expect(outcome.items.count == 2)
        #expect(outcome.dropped == 0)
    }

    @Test func oneBadEntryCostsOnlyThatEntry() {
        let outcome = Library.decodeIndex(json([valid, badID, alsoValid]))
        #expect(outcome.items.count == 2)
        #expect(outcome.dropped == 1)
        #expect(outcome.items.map(\.title) == ["vegeta", "itachi"])
    }

    /// Entries written before 10-bit support have no `bitDepth` at all. Adding
    /// it as a non-optional field would have thrown for every one of them.
    @Test func entriesWithoutBitDepthStillDecodeAsEightBit() {
        let outcome = Library.decodeIndex(json([valid]))
        #expect(outcome.items.first?.bitDepth == nil)
        #expect(outcome.items.first?.pixelBitDepth == 8)
    }

    @Test func bitDepthIsReadWhenPresent() {
        let outcome = Library.decodeIndex(json([alsoValid]))
        #expect(outcome.items.first?.pixelBitDepth == 10)
    }

    /// Reported as dropped rather than as an empty library, which is what makes
    /// `load()` preserve the file instead of overwriting it.
    @Test func unparseableFileIsReportedAsDropped() {
        let outcome = Library.decodeIndex(Data("not json at all".utf8))
        #expect(outcome.items.isEmpty)
        #expect(outcome.dropped > 0)
    }

    /// A genuinely empty library is not a failure and must not trigger the
    /// corrupt-file backup path.
    @Test func emptyArrayIsNotTreatedAsCorruption() {
        let outcome = Library.decodeIndex(Data("[]".utf8))
        #expect(outcome.items.isEmpty)
        #expect(outcome.dropped == 0)
    }

    @Test func everyEntryBeingBadStillReportsThemIndividually() {
        let outcome = Library.decodeIndex(json([badID, badID]))
        #expect(outcome.items.isEmpty)
        #expect(outcome.dropped == 2)
    }
}

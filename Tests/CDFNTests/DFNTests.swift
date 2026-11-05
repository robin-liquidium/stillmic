import CDFN
import Foundation
import XCTest

final class DFNTests: XCTestCase {
    private let root = URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
    private let hop = Int(DFN_HOP)

    private func engine() throws -> OpaquePointer {
        let path = root.appendingPathComponent("Resources/dfn3.weights").path
        return try XCTUnwrap(dfn_create(path))
    }

    private func samples(_ name: String) throws -> [Int16] {
        let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent("Fixtures/\(name)")
        let pcm = try Data(contentsOf: url).dropFirst(44)  // canonical WAV header
        return pcm.withUnsafeBytes { Array($0.bindMemory(to: Int16.self)) }
    }

    private func render(_ d: OpaquePointer, _ input: [Int16]) -> [Int16] {
        var output = [Int16]()
        var x = [Float](repeating: 0, count: hop)
        var y = [Float](repeating: 0, count: hop)
        for start in stride(from: 0, to: input.count - hop + 1, by: hop) {
            for i in 0..<hop { x[i] = Float(input[start + i]) / 32768 }
            dfn_process(d, x, &y)
            output.append(contentsOf: y.map { Int16(max(-1, min(1, $0)) * 32767) })
        }
        return output
    }

    func testRejectsWrongWeightsFile() {
        XCTAssertNil(dfn_create(#filePath))
        XCTAssertNil(dfn_create("/nonexistent"))
    }

    /// Aggressive uses libDF's default thresholds, so it must match the reference implementation.
    /// `reference.wav` is `bench/dftract` (deep_filter 0.5.6) run on `input.wav`.
    func testAggressiveMatchesReference() throws {
        let d = try engine()
        defer { dfn_destroy(d) }
        dfn_set_mode(d, DFN_AGGRESSIVE)
        let output = render(d, try samples("input.wav"))
        let reference = try samples("reference.wav")
        XCTAssertEqual(output.count, reference.count)
        let worst = zip(output, reference).map { abs(Int($0) - Int($1)) }.max() ?? .max
        XCTAssertLessThanOrEqual(worst, 2)
        XCTAssertGreaterThan(reference.map { abs(Int($0)) }.max() ?? 0, 500, "fixture must contain speech-like signal")
    }

    func testStandardKeepsMoreThanAggressiveAndIsFinite() throws {
        let d = try engine()
        defer { dfn_destroy(d) }
        let input = try samples("input.wav")
        let standard = render(d, input)
        dfn_reset(d)
        dfn_set_mode(d, DFN_AGGRESSIVE)
        let aggressive = render(d, input)
        let energy = { (x: [Int16]) in x.reduce(0.0) { $0 + Double($1) * Double($1) } }
        XCTAssertGreaterThanOrEqual(energy(standard), energy(aggressive))
    }

    func testSilenceStaysSilent() throws {
        let d = try engine()
        defer { dfn_destroy(d) }
        XCTAssertTrue(render(d, [Int16](repeating: 0, count: hop * 50)).allSatisfy { $0 == 0 })
    }
}

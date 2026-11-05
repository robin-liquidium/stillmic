// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "Stillmic",
    platforms: [.macOS("15.0")],
    products: [.executable(name: "Stillmic", targets: ["Stillmic"])],
    dependencies: [
        .package(url: "https://github.com/sparkle-project/Sparkle", exact: "2.10.0"),
    ],
    targets: [
        .target(
            name: "CDFN",
            path: "Sources/CDFN",
            cSettings: [
                .define("ACCELERATE_NEW_LAPACK"),
                // Baseline for all Apple Silicon, so releases run on every M-series Mac.
                .unsafeFlags(["-O3", "-mcpu=apple-m1", "-ffp-contract=fast"], .when(configuration: .release)),
            ],
            linkerSettings: [
                .linkedFramework("Accelerate"),
                .linkedFramework("AudioToolbox"),
                .linkedFramework("CoreAudio"),
            ]
        ),
        .executableTarget(
            name: "Stillmic",
            dependencies: ["CDFN", .product(name: "Sparkle", package: "Sparkle")],
            path: "Sources/Stillmic",
            linkerSettings: [
                .linkedFramework("ServiceManagement"),
                .unsafeFlags(["-Xlinker", "-rpath", "-Xlinker", "@executable_path/../Frameworks"]),
            ]
        ),
        .testTarget(
            name: "CDFNTests",
            dependencies: ["CDFN"],
            path: "Tests/CDFNTests",
            exclude: ["Fixtures"]
        ),
    ]
)

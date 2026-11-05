// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "NoNoiseMac",
    platforms: [.macOS("15.0")],
    products: [.executable(name: "NoNoiseMac", targets: ["NoNoiseMac"])],
    targets: [
        .target(
            name: "CDFN",
            path: "Sources/CDFN",
            cSettings: [
                .define("ACCELERATE_NEW_LAPACK"),
                .unsafeFlags(["-O3", "-mcpu=apple-m4", "-ffp-contract=fast"], .when(configuration: .release)),
            ],
            linkerSettings: [
                .linkedFramework("Accelerate"),
                .linkedFramework("AudioToolbox"),
                .linkedFramework("CoreAudio"),
            ]
        ),
        .executableTarget(
            name: "NoNoiseMac",
            dependencies: ["CDFN"],
            path: "Sources/NoNoiseMac",
            linkerSettings: [.linkedFramework("ServiceManagement")]
        ),
    ]
)

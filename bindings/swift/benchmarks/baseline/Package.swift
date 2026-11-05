// swift-tools-version: 5.9
import PackageDescription

// Side-by-side benchmark: pbwire-swift (path dependency on the binding) vs
// apple/swift-protobuf with generated code for the same matrix3.proto. The
// generated model comes from swift-protobuf's own build plugin, so there is
// no checked-in generated code and no protoc plugin to install. Needs
// network once (to fetch swift-protobuf); `swift run -c release BenchBoth`.
let package = Package(
    name: "baseline",
    dependencies: [
        .package(path: "../.."),
        .package(url: "https://github.com/apple/swift-protobuf.git", from: "1.28.0"),
    ],
    targets: [
        .target(
            name: "Model",
            dependencies: [
                .product(name: "SwiftProtobuf", package: "swift-protobuf"),
            ],
            path: "Sources/Model",
            plugins: [
                .plugin(name: "SwiftProtobufPlugin", package: "swift-protobuf"),
            ]),
        .executableTarget(
            name: "BenchBoth",
            dependencies: [
                .product(name: "Pbwire", package: "swift"),
                "Model",
                .product(name: "SwiftProtobuf", package: "swift-protobuf"),
            ],
            path: "Sources/BenchBoth",
            linkerSettings: [.unsafeFlags(["../../../../build/libpbwire.a"])]),
    ]
)

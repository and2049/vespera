import Foundation
// swift-tools-version: 6.0
import PackageDescription

let packageRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let infoPlist = packageRoot.appendingPathComponent("Info.plist").path
let sdkInclude = packageRoot.appendingPathComponent("../../include").standardized.path

let package = Package(
  name: "VesperaUI",
  platforms: [.macOS(.v14)],
  products: [.executable(name: "vespera", targets: ["VesperaUI"])],
  targets: [
    .systemLibrary(name: "ModuleSDK", path: "ModuleSDK"),
    .target(
      name: "NativeGallery",
      path: "NativeGallery",
      publicHeadersPath: "include",
      cxxSettings: [.unsafeFlags(["-I", sdkInclude, "-fobjc-arc"])],
      linkerSettings: [.linkedFramework("AppKit")]
    ),
    .target(
      name: "LayoutRenderer",
      path: "LayoutRenderer",
      publicHeadersPath: "include",
      cxxSettings: [.unsafeFlags(["-std=c++20", "-I", sdkInclude, "-fobjc-arc"])],
      linkerSettings: [.linkedFramework("AppKit")]
    ),
    .executableTarget(
      name: "VesperaUI",
      dependencies: ["ModuleSDK", "NativeGallery", "LayoutRenderer"],
      path: ".",
      exclude: [
        "Tests", "ModuleSDK", "NativeGallery", "LayoutRenderer", "Info.plist", "build-app.sh",
        "run-dev.sh", "AppIcon.icon",
      ],
      sources: ["Sources"],
      resources: [.copy("Resources/Icons")],
      swiftSettings: [.unsafeFlags(["-Xcc", "-I\(sdkInclude)"])],
      linkerSettings: [
        .unsafeFlags([
          "-Xlinker", "-sectcreate", "-Xlinker", "__TEXT", "-Xlinker", "__info_plist", "-Xlinker",
          infoPlist,
        ])
      ]
    ),
    .testTarget(
      name: "VesperaUITests", dependencies: ["VesperaUI", "LayoutRenderer"], path: "Tests",
      exclude: ["Fixtures"], swiftSettings: [.unsafeFlags(["-Xcc", "-I\(sdkInclude)"])]),
  ]
)

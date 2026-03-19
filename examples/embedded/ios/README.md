# iOS embedding

`make ios` from the repo root produces `xs-ios.a` (arm64 device + x86_64
simulator). For Apple Silicon simulator support run `make ios-sim-arm64`
and combine the slices via `xcodebuild -create-xcframework`.

To use in an Xcode project:

1. Drag `xs-ios.a` into the project (or add it under the target's "Frameworks,
   Libraries, and Embedded Content").
2. Add `src/xs_embed.h` to the bridging header, or wrap it as in
   `XSBridge.swift`.
3. Build settings: enable "Disable Implicit Function Signatures" only if
   you're not using `@_silgen_name`.

The runtime ships without the JIT (Apple's W^X policy), so the VM is
the fastest available backend on iOS.

# Mobile and embedded

XS targets four non-desktop architectures with cross-compile
Makefile targets:

| target       | toolchain               | output             |
|--------------|-------------------------|--------------------|
| iOS          | Xcode + xcrun           | `xs-ios.a`         |
| Android      | NDK r25+                | `xs-android.so`    |
| ESP32        | esp-idf clang/RISC-V    | partition image    |
| Raspberry Pi | aarch64-linux-gnu-gcc   | `xs-arm64`         |

## iOS

```sh
make ios
```

Produces `xs-ios.a` for both arm64 (devices) and x86_64 (simulator).
Drop into a Swift project as a static library; bridge through C with
`@_silgen_name` or via `xs_embed.h`.

```swift
import Foundation

@_silgen_name("xs_eval_cstr")
func xs_eval_cstr(_ src: UnsafePointer<CChar>) -> UnsafePointer<CChar>?

let result = String(cString: xs_eval_cstr("1 + 1")!)
```

The XS runtime's GC and threading work fine inside an iOS app; the
JIT does *not* (Apple disallows W^X transitions without
entitlements). On iOS the bytecode VM is the fastest available
backend.

## Android

```sh
ANDROID_NDK=$ANDROID_HOME/ndk/25.2.9519653 make android
```

Produces `xs-android.so` for arm64-v8a + armeabi-v7a + x86_64. Load
through JNI:

```kotlin
class XS {
    init { System.loadLibrary("xs") }
    external fun eval(src: String): String
}

val xs = XS()
println(xs.eval("[1, 2, 3].fold(0, |a, b| a + b)"))
```

Android allows the JIT (it's not Apple's policy).

## ESP32 / RISC-V microcontrollers

```sh
make esp32
```

Produces a partition image suitable for `idf.py flash`. The build
strips the JIT, FFI, transpilers, and HTTP server; the resulting
runtime is around 250 KB plus your bytecode.

```c
// in your esp-idf project's main.c
#include "xs_embed.h"
extern const uint8_t app_bytecode_start[] asm("_binary_app_xsc_start");
extern const uint8_t app_bytecode_end[]   asm("_binary_app_xsc_end");

void app_main(void) {
    XSContext *xs = xs_new();
    xs_run_bytecode(xs, app_bytecode_start,
                    app_bytecode_end - app_bytecode_start);
}
```

What works on ESP32: arithmetic, control flow, closures, channels,
small allocations. What doesn't: heavy regex, the full crypto suite
(AES is fine, RSA isn't built), networking past raw TCP.

## Raspberry Pi (aarch64 Linux)

```sh
make release CC=aarch64-linux-gnu-gcc TARGET=xs-arm64
```

Native arm64 build, full feature set. The JIT works on arm64; the
codegen is at `src/jit/ra_codegen_arm64.c`.

## Picking a target

| use case                          | recommendation                          |
|-----------------------------------|-----------------------------------------|
| Mobile app embedding              | iOS / Android static library            |
| Robot / drone control plane       | aarch64 Linux full build                |
| Battery-powered IoT sensor        | ESP32 with bytecode-only build          |
| Smart home hub                    | aarch64 Linux full build                |
| Apple Watch                       | Swift wrapping `xs-ios.a` (no JIT)      |
| Wear OS app                       | Android `.so`                           |

## What's not yet there

- **Bare-metal** (no OS): the runtime needs `malloc`, `read`,
  `write`, `clock_gettime`. Porting to a no-stdlib environment is
  RFC territory.
- **WebUSB / WebBLE in browsers**: use the JS SDK and proxy.

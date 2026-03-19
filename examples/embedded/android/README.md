# Android embedding

`ANDROID_NDK=$ANDROID_HOME/ndk/25.2.9519653 make android` from the repo root
builds `build/android/<abi>/libxs.so` for arm64-v8a, armeabi-v7a, and x86_64.

Drop the per-ABI `.so` files into `app/src/main/jniLibs/<abi>/`, then call
through JNI as in `jni_xs.c` and `XS.kt`.

Override defaults via env / make vars:

```sh
ANDROID_NDK=/path/to/ndk ANDROID_API=24 ANDROID_ABIS="arm64-v8a x86_64" make android
```

Android allows the JIT (no W^X restriction), so the JIT tier is enabled
on this build.

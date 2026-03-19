# ESP32 embedding

This is a worked esp-idf project that boots an XS bytecode program from
flash. To build:

```sh
# 1. Build XS as an esp-idf component (xtensa-esp32-elf-gcc on PATH).
. $IDF_PATH/export.sh
make -C ../../.. esp32
make -C ../../.. esp32-component
cp ../../../build/esp32/libxs.a ../../../examples/embedded/esp32/components/xs/

# 2. Compile your XS program to bytecode on the host.
echo 'println("hello esp32")' > app.xs
xs build app.xs -o main/app.xsc

# 3. Build and flash with idf.py.
idf.py set-target esp32
idf.py build flash monitor
```

The bytecode is linked into the firmware via `EMBED_FILES`, so the
runtime never touches the filesystem - boot time is essentially the
cost of allocating the VM and decoding the bytecode header.

What works: arithmetic, control flow, closures, channels, small
allocations, the `math`, `time`, `string`, `path`, `json`, `re`,
`fmt`, `log`, `tracing`, `collections` modules.

What's stripped: JIT, FFI, transpilers (JS/C/WASM), HTTP server, BearSSL
TLS, full crypto. Use IDF's mbedtls or HTTP client for those.

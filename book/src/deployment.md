# Single-binary deployment

`xs build app.xs -o app.xsc` produces a self-contained bytecode
bundle that any `xs` runtime can execute. For deployments that don't
already have `xs` available, ship the source plus an `xs` binary,
or use the C transpiler for an actual native build.

## The xsc bundle

```sh
xs build src/lib.xs -o myservice.xsc
xs run myservice.xsc
```

`.xsc` files are versioned bytecode; an older `xs` will refuse a
newer `.xsc`. They're typically 10-20× smaller than the equivalent
`.xs` source.

## Single static native binary

```sh
make release                     # produces ./xs (~2 MB, no shared deps)
xs build src/main.xs -o app.xsc

# package both
mkdir dist
cp xs app.xsc dist/
cp xs.toml dist/
```

Or fold the bytecode into the binary:

```sh
xs --embed-program app.xsc xs -o app
./app                            # standalone, single binary
```

The `--embed-program` flag appends the bytecode to the runtime and
emits a launcher that finds and runs it. The result behaves like a
go binary: 2-3 MB, runs anywhere with the same architecture and OS.

## Cross-compile

```sh
make release CC=aarch64-linux-gnu-gcc TARGET=xs-arm64
make release CC=x86_64-w64-mingw32-gcc TARGET=xs.exe
```

For mobile and embedded see [mobile / embedded](./mobile.md).

## Docker

```dockerfile
FROM gcr.io/distroless/static
COPY app /
COPY app.xsc /
ENTRYPOINT ["/app", "/app.xsc"]
```

The runtime has no shared library dependencies; distroless static is
sufficient.

## SystemD service

```ini
[Unit]
Description=My XS Service

[Service]
Type=simple
ExecStart=/usr/local/bin/xs /opt/myservice/app.xsc
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

Resource limits (memory, instruction count, wall time) are
configurable per-service via XS_LIMITS_* env vars; combine with
systemd's `MemoryMax=` for double protection.

## Verifying a release

```sh
curl -O https://xslang.org/dl/xs-linux-x86_64
curl -O https://xslang.org/dl/xs-linux-x86_64.minisig
minisign -V -P "$(curl -s https://xslang.org/pubkey)" -m xs-linux-x86_64
```

Releases are signed with [minisign](https://jedisct1.github.io/minisign/).
The public key is checked into the repo as `MINISIGN.pub` and served
at `https://xslang.org/pubkey`.

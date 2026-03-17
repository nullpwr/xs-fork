# First-party XS packages

Bundle of small, focused packages owned by the XS team. Each lives in
its own subdirectory with an `xs.toml`, `src/lib.xs`, `tests/`, and a
`README.md`.

| package          | what it does                                              |
|------------------|-----------------------------------------------------------|
| `xs-cli-args`    | declarative argv parser with subcommands                  |
| `xs-test`        | tiny test runner: `describe`/`it`, async, snapshots       |
| `xs-assert`      | rich assertion helpers, deep equal, throws-with-message   |
| `xs-uuid`        | UUID v4 and v7 generation, parsing, validation            |
| `xs-semver`      | parse / compare / range-satisfies for semver versions     |
| `xs-jwt`         | HS256 / HS384 / HS512 sign + verify                       |
| `xs-dotenv`      | read `.env` files, expand `${VAR}` references             |
| `xs-yaml`        | small subset YAML parser (mappings, sequences, scalars)   |
| `xs-markdown`    | CommonMark-ish markdown to HTML                           |
| `xs-diff`        | line / word diff, unified-diff output                     |
| `xs-async-queue` | bounded async queue with backpressure                     |
| `xs-retry`       | exponential backoff with jitter, predicate-based retry    |
| `xs-http-client` | higher-level wrapper over `net.http` (auto-retry, json)   |
| `xs-sql`         | parameterised query builder for the in-memory db module   |
| `xs-otlp`        | export tracing spans to an OTLP HTTP collector            |

## Install

Once `xs install <name>` knows about the registry these are published
to, every package is `xs install xs-cli-args` away. Until then, copy
the directory into your project's `xs_lib/` or symlink it.

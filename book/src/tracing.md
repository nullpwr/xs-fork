# Tracing and observability

The `tracing` stdlib module gives you span-based structured logging
with pluggable sinks. The companion `xs-otlp` package exports to an
OTLP/HTTP collector (Jaeger, Tempo, etc.).

## Quickstart

```xs
import tracing

tracing.add_sink(tracing.console_sink)
tracing.set_level("debug")

tracing.info("server starting", #{port: 8080})

tracing.with_span("request", || {
    handle_request()
    tracing.debug("processed")
})
```

```text
[info ] server starting
[debug] processed
[span] request 4.281ms
```

## Levels

`trace` < `debug` < `info` < `warn` < `error`. `set_level("info")`
filters everything below.

## Records

Every emitted record is a Map with at minimum:

```xs
#{
    kind:  "event" | "span",
    level: "trace" | "debug" | "info" | "warn" | "error",
    ts:    1735689600123456789,           -- epoch nanoseconds
    msg:   "...",                          -- on events only
    name:  "...",                          -- on spans only
    span_id: 42,
    parent_id: 0,
    duration_ns: 4281000,                 -- on spans only
    -- plus any attrs you passed
}
```

Sinks receive these maps and decide what to do.

## Spans nest automatically

```xs
tracing.with_span("outer", || {
    tracing.with_span("inner", || {
        tracing.info("deep")
    })
})
```

`info` carries `parent_id = inner.span_id`; `inner` carries
`parent_id = outer.span_id`. The console sink prints them flat; OTLP
preserves the parent-child relationship for trace UIs.

## Custom sinks

```xs
tracing.add_sink(|rec| {
    if rec.get("level") == "error" {
        send_to_pagerduty(rec)
    }
})

tracing.add_sink(|rec| {
    fs.append("/var/log/app.jsonl", json.stringify(rec) + "\n")
})
```

Sinks compose: `add_sink` doesn't replace the previous; both run.
`tracing.remove_sinks()` clears the registry.

## OTLP export

```xs
import otlp                                  -- packages/xs-otlp
otlp.install("http://localhost:4318/v1/traces", #{
    service: "my-app",
    batch_size: 100,
})

-- ... your program ...
otlp.flush()                                  -- before exit
```

The bridge subscribes as a tracing sink, batches up to `batch_size`
spans, and POSTs them to the collector.

## What the runtime traces for free

Nothing, on purpose. The runtime emits no spans without your
explicit instrumentation. Auto-tracing through `before_eval` hooks
is possible (see [Plugins](./plugins.md)) but isn't on by default —
the cost would be unbounded.

## Conventions

- `tracing.with_span("verb_noun", || ...)` — `"fetch_user"`,
  `"render_template"`, `"db_query"`.
- Pass attrs as a Map: `tracing.info("login", #{user_id: id, ip: ip})`.
- Don't put large bodies in attrs; spans are meant to be compact.
- Keep level filters in mind — `tracing.debug` calls inside hot
  loops still pay the call cost even when the level is filtered.

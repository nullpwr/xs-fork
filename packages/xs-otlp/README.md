# xs-otlp

Bridge for the stdlib `tracing` module to an OTLP-compatible
collector (Jaeger, Tempo, etc.). Subscribes as a tracing sink, batches
spans, and POSTs them to the collector's HTTP endpoint.

```xs
import otlp
import tracing
otlp.install("http://localhost:4318/v1/traces", #{service: "my-app", batch_size: 100})

tracing.with_span("request", || handle_request())
otlp.flush()        // before exit, drain any remaining
```

-- xs-otlp: register a tracing sink that batches and exports OTLP/HTTP
-- spans to a collector.

import tracing
import json
import net

var _buf = []
var _flush_threshold = 50
var _endpoint = ""
var _service = "xs-app"

fn _to_otlp_span(rec) {
    return #{
        name: rec.get("name") ?? "span",
        traceId: str(rec.get("trace_id") ?? rec.get("span_id") ?? 0),
        spanId: str(rec.get("span_id") ?? 0),
        parentSpanId: str(rec.get("parent_id") ?? 0),
        startTimeUnixNano: rec.get("ts") ?? 0,
        endTimeUnixNano: (rec.get("ts") ?? 0) + (rec.get("duration_ns") ?? 0),
        attributes: [],   -- pull additional fields if present
    }
}

fn _flush() {
    if _buf.is_empty() or _endpoint == "" { return }
    let payload = #{
        resourceSpans: [#{
            resource: #{attributes: [#{key: "service.name", value: #{stringValue: _service}}]},
            scopeSpans: [#{spans: _buf}],
        }],
    }
    net.http("POST", _endpoint, #{"Content-Type": "application/json"},
             json.stringify(payload))
    _buf = []
}

fn _sink(rec) {
    if rec.get("kind") != "span" { return }
    _buf.push(_to_otlp_span(rec))
    if _buf.len() >= _flush_threshold { _flush() }
}

fn install(endpoint, opts) {
    _endpoint = endpoint
    let o = opts ?? #{}
    _service = o.get("service") ?? _service
    _flush_threshold = o.get("batch_size") ?? _flush_threshold
    tracing.add_sink(_sink)
}

fn flush() { _flush() }

# xs-retry

Exponential backoff with optional jitter.

```xs
import retry
import net

let resp = retry.run(
    || net.http_get("https://api.example.com"),
    #{
        tries: 5,
        base_ms: 100,
        max_ms: 5000,
        jitter: true,
        should_retry: |e| str(e).contains("timeout"),
        on_retry: |attempt, delay, e| println("retry {attempt} in {delay}ms: {e}"),
    }
)
```

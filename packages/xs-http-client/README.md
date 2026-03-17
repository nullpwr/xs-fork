# xs-http-client

```xs
import http_client
let api = http_client.make(#{
    base_url: "https://api.example.com",
    headers: #{Authorization: "Bearer ..."},
    retries: 3,
    retry_on_status: [502, 503, 504],
})

let user = api.get_json("/users/42")
api.post_json("/events", #{event: "click", user_id: 42})
```

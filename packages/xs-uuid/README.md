# xs-uuid

```xs
import uuid

let id = uuid.v4()           // "f47ac10b-58cc-4372-a567-0e02b2c3d479"
let oid = uuid.v7()          // time-ordered, sorts by creation time
uuid.is_valid(id)            // true
uuid.version(oid)            // 7
```

# Node.js - threaded Runner lifetime

A threaded `Runner` holds a reference on Node's event loop only between `start()`
and `stop()`. This script ends on its own, with no forced exit. Background in
[Threaded Runner lifetime in Node](../explanation/node-threaded-runner-lifetime.md).

```
node docs/examples/node_threaded_runner_lifetime.js
```

```javascript
--8<-- "examples/node_threaded_runner_lifetime.js"
```

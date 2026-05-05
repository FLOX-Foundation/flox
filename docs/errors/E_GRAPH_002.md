---
code: E_GRAPH_002
title: Circular dependency in indicator graph
severity: error
since: 0.5.7
---

# E_GRAPH_002 — Circular dependency in indicator graph

The indicator pipeline detected that a node transitively depends on
itself. The error message names the node that closed the cycle.

## How to fix

Trace the `deps=[...]` chain back to the cycle and break it. The
indicator graph is a DAG — values only flow forward.

=== "Python"
    ```python
    # ✗ cycle: a → b → c → a
    g.add_node("a", deps=["c"], fn=...)
    g.add_node("b", deps=["a"], fn=...)
    g.add_node("c", deps=["b"], fn=...)

    # ✓ fix: introduce a stateful "previous a" feature instead of
    # making `c` literally depend on `a`.
    g.add_node("a_prev", deps=[], fn=...)   # delayed copy
    g.add_node("a",      deps=["c"], fn=...)
    g.add_node("b",      deps=["a"], fn=...)
    g.add_node("c",      deps=["a_prev", "b"], fn=...)
    ```

## Common causes

- A feature was refactored to depend on its consumer (e.g. an EMA-of-EMA
  setup that accidentally references the wrong input).
- Two parallel chains were merged but the merge introduced a back-edge.

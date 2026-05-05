---
code: E_GRAPH_001
title: Unknown indicator graph node
severity: error
since: 0.5.7
---

# E_GRAPH_001 — Unknown indicator graph node

The indicator pipeline asked for a node by name, but no node with that
name has been registered. The error message includes the missing
node name.

## How to fix

Register every node before requiring it:

=== "Python"
    ```python
    from flox import IndicatorGraph

    g = IndicatorGraph()
    g.add_node("close", deps=[], fn=...)
    g.add_node("ema_fast", deps=["close"], fn=lambda x: ema(x, 12))
    g.add_node("ema_slow", deps=["close"], fn=lambda x: ema(x, 26))
    g.add_node("macd", deps=["ema_fast", "ema_slow"], fn=...)
    # macd → ema_fast → close — all required nodes exist
    ```

## Common causes

- Typo in a `deps=[...]` list — case-sensitive.
- Forgetting to register a feature column that the strategy uses.
- Renaming a node and missing one of its dependents.

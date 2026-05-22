# Replace a leg of an active OrderGroup

`OrderGroup` is a passive state machine. After the strategy fires
`replaceOrder(oldId, newOrder)` through its executor and the
executor surfaces the replace event, the group needs three things:

1. The new exchange order id stitched onto the same leg slot so the
   policy state machine (OneSided cancel-on-fill, AllOrNothing
   revert-on-failure) keeps tracking the right leg.
2. A way to route a later replace / fill event back from the
   exchange id to the leg, since the id has changed.
3. A no-op on a late-replace race when the leg has already filled
   between the replace request and the ack landing.

## API

=== "Python"

    ```python
    g.record_replace_accepted(leg_idx, new_order_id=201)
    # OR — replace ack arrived late, leg already filled
    g.record_replace_rejected(leg_idx)

    # Route the next executor event back to the leg:
    leg_idx = g.find_leg_by_order_id(order_id)  # None if not found
    ```

=== "Node.js"

    ```javascript
    g.recordReplaceAccepted(legIdx, 201);
    g.recordReplaceRejected(legIdx);
    const legIdx = g.findLegByOrderId(orderId);  // null if not found
    ```

=== "QuickJS"

    ```javascript
    __flox_order_group_record_replace_accepted(handle, legIdx, 201n);
    __flox_order_group_record_replace_rejected(handle, legIdx);
    const idx = __flox_order_group_find_leg_by_order_id(handle, orderId);
    ```

=== "Codon"

    ```python
    g.record_replace_accepted(leg_idx, 201)
    g.record_replace_rejected(leg_idx)
    leg_idx = g.find_leg_by_order_id(order_id)  # -1 if not found
    ```

=== "C++"

    ```cpp
    g.recordReplaceAccepted(legIdx, 201);
    g.recordReplaceRejected(legIdx);
    auto leg = g.findLegByOrderId(orderId);  // std::optional<size_t>
    ```

## Late-replace race

When the configured replace ack latency knob is non-zero
(see how-to: replace-in-flight), the executor surfaces the replace
async. If the leg fills inside the latency window, the simulator
emits `REPLACE_REJECTED` and the original order's fill stands.

The OrderGroup mirrors this: calling `recordReplaceAccepted` on a
leg that is already `Filled` or `Cancelled` is a no-op — the leg's
order id is *not* overwritten, otherwise downstream bookkeeping
would point at a phantom id.

The recommended pattern in the strategy:

```python
def on_replace_event(self, ev):
    leg = self.group.find_leg_by_order_id(ev.original_order_id)
    if leg is None:
        return  # not a grouped order

    if ev.kind == "accepted":
        self.group.record_replace_accepted(leg, ev.new_order_id)
    else:
        self.group.record_replace_rejected(leg)
```

## Policy interaction

A replace ack does **not** change the leg's LegState. A leg in
`Submitted` stays `Submitted`, a `PartiallyFilled` leg stays
`PartiallyFilled` with its accumulated fill quantity intact. The
OneSided "cancel the other leg on first fill" and AllOrNothing
"revert all filled legs on any failure" policies continue to fire
against the leg slot, using whatever exchange id the leg currently
holds.

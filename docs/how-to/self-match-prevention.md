# Self-trade prevention

When the same account has crossing orders on both sides of a price,
real venues prevent the match — both to avoid fee-wash and to comply
with regulatory wash-trade rules. flox simulator now applies one of
four STP modes at order submission.

The simulator currently treats every order on one `SimulatedExecutor`
as belonging to one logical account. Cross-account STP (institutional
master + sub-accounts) is filed as a follow-up.

## Modes

| Mode             | Behaviour                                                         |
|------------------|-------------------------------------------------------------------|
| `none` (default) | Self-match allowed (legacy behaviour).                              |
| `cancel_newest`  | Reject the incoming order with reason `stp_cancel_newest`.          |
| `cancel_oldest`  | Cancel the resting order; the incoming order proceeds normally.      |
| `cancel_both`    | Cancel the resting order and reject the incoming one.                |
| `decrement`      | Cancel the smaller side fully; reduce the larger side by the smaller's qty. |

`decrement` is the most permissive — neither leg blocks the other
completely, both books shrink toward zero. The other three modes
either reject or cancel.

## Apply from a strategy

=== "Python"

    ```python
    --8<-- "examples/python_stp_modes.py"
    ```

=== "Node.js"

    ```javascript
    exec.setSTPMode('cancel_newest');
    ```

=== "Codon"

    ```python
    exec.set_stp_mode("cancel_newest")
    ```

=== "QuickJS"

    ```javascript
    __flox_simulated_executor_set_stp_mode(exec, 1);  // 1 = CancelNewest
    ```

=== "C++"

    ```cpp
    sim.setSTPMode(flox::STPMode::CancelNewest);
    ```

## Multi-account STP

The simulator keys STP on `Order::accountId` (default 0). Two orders
share an STP scope when:

1. their `accountId` values match, OR
2. both accounts map into the same explicit STP group (configured
   via `setSTPGroupMembership(account_id, group_id)`).

Master / sub-account topologies use the group mechanism: every
sub-account opts into the same group id, and crossings between any
two sub-accounts in that group count as self-trade.

=== "Python"

    ```python
    exec.set_stp_group_membership(42, 100)
    exec.set_stp_group_membership(43, 100)
    # Submit with account_id keyword:
    exec.submit_order(id=1, side='buy', price=50000, quantity=1,
                       symbol=1, account_id=42)
    exec.submit_order(id=2, side='sell', price=50000, quantity=1,
                       symbol=1, account_id=43)
    # → STP fires (same group 100)
    ```

=== "Node.js"

    ```javascript
    exec.setSTPGroupMembership(42, 100);
    exec.setSTPGroupMembership(43, 100);
    ```

## Notes

- The crossing check uses limit price only. A market order against
  the same account's resting limit triggers STP if the resting price
  is on the opposite side; without a price the simulator skips the
  check (market orders never match against the queue tracker in the
  current simulator anyway, so this corner is theoretical).
- `decrement` mode mutates the resting order's quantity in place
  when the incoming order is smaller. The smaller side's
  REJECTED event carries reason `stp_decrement_newest`; the larger
  side stays in the book at the reduced quantity.
- STP runs after rate-limit and reduce-only checks, before the
  POST_ONLY / FOK / IOC checks. An incoming order that would be
  rate-limited never reaches STP.
- Cancelled resting orders go through the same path as a user
  cancel — they emit `CANCELED` and disappear from the queue
  tracker / market-position tracker.

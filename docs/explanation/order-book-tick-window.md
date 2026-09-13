# The order book's tick window

`NLevelOrderBook` is a flat array indexed by tick. That is what makes a level
lookup a single load instead of a tree walk, and it is also the source of the
one constraint you have to design around: the array is finite, so at any moment
the book can only represent a bounded range of prices. This page describes where
that range sits, when it moves, and what happens to a price outside it.

## The window

A book of `MaxLevels` levels with tick size `t` covers `MaxLevels` consecutive
ticks starting at a base index. Multiply out and the price range is
`MaxLevels * t` wide:

| Levels | Tick | Window width | Room either side of the market |
| ------ | ---- | ------------ | ------------------------------ |
| 512    | 0.01 | $5.12        | $2.56                          |
| 512    | 0.1  | $51.20       | $25.60                         |
| 8192   | 0.01 | $81.92       | $40.96                         |
| 512    | 0.001 (0..1 prediction market) | 0.512 | 0.256 |

The right-hand column is the one that matters. The book centres its window on
the market, so the usable headroom in either direction is half the level count,
not all of it. `SymbolContext` gives each symbol a `NLevelOrderBook<512>`, which
is where the first two rows come from.

## When the window moves

A `SNAPSHOT` re-anchors unconditionally: the book measures the tick range of the
levels in the snapshot, centres the window on it, and refills from scratch.

A `DELTA` carries only what changed, so the book has to decide for itself. It
re-anchors when the delta adds a level outside the current window and everything
already in the book still fits alongside it. Both conditions, not either. The
new window is centred on the union of the two ranges, and the existing levels
are carried across rather than dropped.

When the two do not fit together, the out-of-window level is discarded and the
book is left alone. This is deliberate. A venue that emits one stale quote, a
placeholder price, or a fat finger thousands of ticks from the market should not
be able to evict live depth; losing one bad level is cheaper than losing the
book.

A removal never re-anchors. A delta that zeroes a price outside the window is
naming a level the book cannot be holding, so there is nothing to do.

A delta that arrives before any snapshot anchors the book on its own levels.

## Why this matters on a live feed

Bybit and Bitget send a snapshot on connect and after a sequence gap, and
nothing in between. Hyperliquid sends a snapshot with every update, so it never
depends on any of this. On the first kind of feed, a book that only re-anchored
on snapshots would follow the market until it reached the edge of its window and
then stop: additions past the edge would be dropped while removals kept being
applied, so one side would empty out, then the other, and neither would come
back until the connection dropped and the feed re-sent a snapshot. On a
512-level book at a 0.1 tick, that is about $25 of BTC movement to lose one side
and about $30 to lose both.

## Sizing the book

Pick `MaxLevels` so that half of it, in ticks, comfortably exceeds the distance
the market moves between snapshots on your feed. Re-anchoring costs a shift of
both ladders, so a larger window is not free, but it happens once per window's
worth of drift rather than once per update. The default of 8192 is generous for
most instruments; `SymbolContext`'s 512 is tuned for many symbols at once.

## Tick size

The tick size must be positive. The constructor throws `std::invalid_argument`
otherwise, and the bindings surface that the way each language expects:

| Surface | Bad tick size |
| ------- | ------------- |
| C++     | throws `std::invalid_argument` |
| Python  | raises `ValueError` |
| Node    | throws a `RangeError` |
| QuickJS | throws a `RangeError` |
| Codon   | raises `ValueError` |
| C ABI   | `flox_book_create` returns `NULL` |

A tick size arrives from instrument configuration and from user code, so a bad
one is bad input, not a broken invariant, and it is reported rather than
asserted.

Price is a fixed-point type with a scale of 1e8, so the smallest representable
tick is 1e-8 and the sub-cent pairs really do quote there. Tick sizes of 1e-8
and 2e-8 work exactly like any other.

## Mid price

`mid()` is `tickSize * (bidTick + askTick) / 2`, with a single division at the
end. It agrees exactly with `SymbolContext::mid()`, which averages the two raw
prices. `Price` is an integer underneath, so an odd sum of tick indices
truncates by at most half a raw unit, which is 5e-9 at the standard price scale.

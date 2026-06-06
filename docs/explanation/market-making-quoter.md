# Market-making quoter

A market maker posts bids and asks on both sides of a fair price and earns the
spread when both fill. Doing that well means more than placing one bid and one
ask. The quotes step out across several levels, they lean against the
position the maker is already carrying, and they hold still through small
price moves instead of churning the book. `Quoter` packages that into a
reusable primitive so a strategy does not rebuild it each time.

## The ladder

Given a fair price, the quoter produces a ladder: a set of bid levels and a
set of ask levels, each a configured size. The first level sits a target
half-spread away from the price, and each further level steps out by a fixed
amount. A wider ladder captures more flow at worse prices; a tighter one fills
more often near the top.

## Inventory skew

A maker that keeps buying ends up long and exposed. To lean against that, the
quoter shifts a reservation price away from the fair price in proportion to
the current inventory. A long position pulls the whole ladder down, so the
asks become more attractive and sells fill first; a short position pulls it
up. With no inventory or no skew configured, the reservation price is the fair
price and the ladder is symmetric.

## Holding quotes steady

Re-quoting on every tick burns through order operations and queue priority for
no gain. `shouldRequote` answers whether a resting quote has drifted far
enough from its target to be worth replacing, measured in price ticks. A
strategy uses it to hold quotes through small moves and only cancel-replace
when the price has moved past a tolerance.

## What the strategy still owns

The quoter computes the desired quotes. It does not place them. Reconciling the
ladder with live orders, deciding what to cancel, what to modify, and what to
leave, stays with the strategy, which is where the order ids and the venue
rules live. The toolkit is the math a maker would otherwise write by hand, not
a replacement for the strategy's own order management.

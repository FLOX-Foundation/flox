# Market data

The engine emits `OutboundEvent`s about individual orders. A market data feed is
a different thing: an ordered, sequenced stream that subscribers can apply to
rebuild the book. `MarketDataPublisher` is the bridge.

```cpp
MarketDataPublisher<> md([](const MdMessage& m) { multicast(m); },
                         /*tickSize*/ Price::fromDouble(0.01), /*symbol*/ 1);

MatchingEngine<MatchingBook> venue(cfg, [&](const OutboundEvent& e)
                                   {
                                     md.onEvent(e);   // fan the venue stream into MD
                                     ledgerOrMetrics(e);
                                   });

md.book();   // flox::NLevelOrderBook kept in step with the venue book
```

## Messages

| `MdType` | Meaning | `qty` field |
|---|---|---|
| `AddOrder` | an order joined the book | shown quantity |
| `Trade` | a print (`id` is the taker) | executed quantity |
| `Executed` | a resting order was hit | displayed remaining |
| `Cancel` | an order left the book | — |
| `Replace` | an order was repriced/resized | displayed remaining |
| `Triggered` | a stop fired | — |

Every message carries a monotonically increasing `seq`, so a consumer can detect
a gap and request a resend.

## Depth is the aggregate, kept in step

The publisher maintains a `flox::NLevelOrderBook` — the same aggregate book type
strategies consume elsewhere — so market-data depth and the venue's internal
order-level book stay in agreement. That agreement is not assumed: the test
suite drives 300k commands and periodically compares the publisher's full depth
against the engine's book.

## Icebergs never leak

An iceberg shows a peak and hides the rest. The public feed must only ever see
the peak, or the book can be probed for hidden size by watching what the feed
reports.

That is why `OrderExecuted` carries **two** quantities:

- `leavesQty` — the whole remaining (peak + hidden), for the **owner's**
  execution report. You should see your own order in full.
- `displayLeaves` — the displayed peak, for the **public feed**.

The publisher drives depth from `displayLeaves`. Using the whole remaining would
both leak the reserve and, on a partial peak fill, corrupt the level aggregate.

## ITCH codec and multicast

`itch_codec.h` encodes `MdMessage` into a fixed 46-byte big-endian frame, and
`UdpMdPublisher` (`udp_multicast.h`) publishes it over IP multicast (with `UdpMdSubscriber` on the receiving end) — the shape a real feed takes.

```cpp
std::vector<uint8_t> buf;
ItchCodec::encode(msg, buf);                       // appends the frame

MdMessage out;
const bool ok = ItchCodec::decode(buf.data(), buf.size(), out);   // false if short
```

The decoder bounds-checks before reading, and is exercised by the parser fuzz
against hostile input alongside the FIX/OUCH/REST parsers.

## Tape

`tape_recorder.h` writes the outbound stream through flox's
`BinaryLogWriter`, so a venue session records into the same binary format the
rest of flox reads — the tape a venue produces can be replayed by ordinary flox
tooling.

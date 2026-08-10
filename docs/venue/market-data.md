# Market data

The engine emits `OutboundEvent`s about individual orders. A market data feed
is an ordered, sequenced stream that subscribers can apply to rebuild the
book. `MarketDataPublisher` is the bridge between the two.

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

Every message carries a monotonically increasing `seq`, so a consumer can
detect a gap and request a resend.

## Depth is the aggregate, kept in step

The publisher maintains a `flox::NLevelOrderBook`, the same aggregate book
type strategies consume elsewhere in FLOX, so market-data depth and the
venue's internal order-level book stay in agreement. A test drives 300k
commands and periodically compares the publisher's full depth against the
engine's book.

## Icebergs and the public feed

An iceberg shows a peak and hides the rest. The public feed must only ever see
the peak, or the book can be probed for hidden size by watching what the feed
reports.

That is why `OrderExecuted` carries two quantities:

- `leavesQty`: the whole remaining (peak + hidden), for the owner's execution
  report. You should see your own order in full.
- `displayLeaves`: the displayed peak, for the public feed.

The publisher drives depth from `displayLeaves`. Using the whole remaining
would leak the reserve and, on a partial peak fill, corrupt the level
aggregate.

## SBE codec and multicast

`sbe_md_codec.h` encodes `MdMessage` in SBE (Simple Binary Encoding): a
canonical SBE message header (`blockLength`, `templateId`, `schemaId`,
`version`) followed by a per-type little-endian root block. One template per
`MdType`, each carrying only the fields that type needs. The schema in
`venue/schema/md-sbe.xml` is the source of truth; the codec is hand-written
against it (no Java codegen in the build). This is the venue's own SBE schema,
not Nasdaq ITCH.

`UdpMdPublisher` (`udp_multicast.h`) publishes frames over IP multicast, with
`UdpMdSubscriber` on the receiving end.

```cpp
std::vector<uint8_t> buf;
SbeMdCodec::encode(msg, buf);                        // header + template block

MdMessage out;
const bool ok = SbeMdCodec::decode(buf.data(), buf.size(), out);  // false if short/foreign
```

The decoder validates the SBE header (rejects a foreign `schemaId`), uses
`blockLength` to skip trailing fields a newer schema version appended (forward
compatible), and bounds-checks before reading. The parser fuzz drives it with
hostile input alongside the FIX / SBE order-entry / REST parsers.

## Tape

`tape_recorder.h` writes the outbound stream through `BinaryLogWriter`, so a
venue session records into the same binary format the rest of FLOX reads. The
tape a venue produces can be replayed by ordinary FLOX tooling.

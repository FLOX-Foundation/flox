# L3OrderBook
`L3OrderBook` is an exception free, fixed capacity, level 3 order book, designed for HFT systems and simulation. 
It tracks individual resting orders (order ID, price, quantity, side), maintains time-price FIFO ordering at each price level, 
and provides efficient access to order-level and price-level inquiries. 

The implementation uses preallocated flat hash maps with tombstoning, intrusive doubly linked lists for FIFO ordering, and supports explicit lifecycle boundaries via snapshots. 
```
template <std::size_t MaxOrders = 8192>
class L3OrderBook
{
 public:
  L3OrderBook(); 

  OrderStatus addOrder(OrderId id, Price price, Quantity quantity, Side side) noexcept; 
 
  OrderStatus removeOrder(OrderId id) noexcept;

  OrderStatus modifyOrder(OrderId id, Quantity newQty) noexcept;

  std::optional<Price> bestBid() noexcept;

  std::optional<Price> bestAsk() noexcept;

  Quantity bidAtPrice(Price price) const noexcept;

  Quantity askAtPrice(Price price) const noexcept;

  void buildFromSnapshot(const L3Snapshot& snap) noexcept;

  L3Snapshot exportSnapshot() const noexcept;
};
```
## Purpose
Maintain full-depth, order-level (L3) view of the market: 
- Track which orders exits 
- At which prices
- On which sides 
- With what quantity
- And in FIFO time order per price level 

## Design Invariants
1) Fixed capacity (`MaxOrders`)
2) Zero dynamic allocation after construction
3) Exception-free
4) Deterministic memory layout
5) Time–price FIFO within each price level
6) Explicit lifecycle boundaries via snapshots

## OrderStatus Contract 
All mutating operations return an `OrderStatus` describing the outcome: 
```
enum class OrderStatus : uint8_t
{
  Ok,         // Operation succeeded
  NoCapacity, // Book or internal index is full
  NotFound,   // OrderId not present
  Extant      // Duplicate OrderId on add
};
```
Semantic Paths 
| Operation     | Status Meaning                                |
| ------------- | --------------------------------------------- |
| `addOrder`    | `Extant` if OrderId already exists            |
| `addOrder`    | `NoCapacity` if book or index space exhausted |
| `addOrder`    | `Ok` on successful order insertion            |
| `removeOrder` | `NotFound` if OrderId does not exist          |
| `removeOrder` | `Ok` on successful order removal              |
| `modifyOrder` | `NotFound` if OrderId does not exist          |
| `modifyOrder` | `Ok` on successful order quantity update      |

No mutating operation leaves the book in a partially mutated state. All operations either succeed or inflict no state change. 

## Responsibilities 
| Aspect         | Description                                                           |
| -------------- | --------------------------------------------------------------------- |
| Input          | Explicit order-level events (`add`, `remove`, `modify`)               |
| Order Tracking | Individual orders indexed by `OrderId`                                |
| Price Levels   | Aggregated quantity per price for compatibility with L2-style queries |
| FIFO Ordering  | Intrusive doubly linked lists per price level                         |
| Queries        | `bestBid`, `bestAsk`, `bidAtPrice`, `askAtPrice`                      |
| Lifecycle      | Snapshot export and rebuild for session boundaries                    |



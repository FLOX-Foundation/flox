# Tools

Utility classes for order books, position tracking, profiles, data I/O, and statistics.

```codon
from flox.tools import (
    OrderBook, L3Book, CompositeBook,
    PositionTracker, PositionGroupTracker, OrderTracker,
    VolumeProfile, MarketProfile, FootprintBar,
    DataWriter, DataReader, DataRecorder, Partitioner,
    correlation, profit_factor, win_rate,
    permutation_test, bootstrap_ci,
    validate_segment, merge_segments, merge_dir,
    split_segment, export_segment, validate_segment_full, validate_dataset,
    recompress_segment, extract_symbols, extract_time_range,
)
```

---

## Order books

### OrderBook

L2 order book.

```codon
book = OrderBook(tick_size=0.01)
book.apply_snapshot(bid_prices, bid_qtys, ask_prices, ask_qtys)
bid = book.best_bid()
```

| Method | Returns | Description |
|--------|---------|-------------|
| `apply_snapshot(bp, bq, ap, aq)` | `None` | Full snapshot |
| `apply_delta(bp, bq, ap, aq)` | `None` | Incremental update |
| `best_bid()` | `Optional[float]` | Best bid price |
| `best_ask()` | `Optional[float]` | Best ask price |
| `mid()` | `Optional[float]` | Mid price |
| `spread()` | `Optional[float]` | Bid-ask spread |
| `is_crossed()` | `bool` | True if book is crossed |
| `clear()` | `None` | Clear all levels |

### L3Book

Order-level book with individual order tracking.

```codon
book = L3Book()
book.add_order(order_id, price, qty, "buy")
```

| Method | Returns | Description |
|--------|---------|-------------|
| `add_order(order_id, price, qty, side)` | `int` | 0 on success |
| `remove_order(order_id)` | `int` | 0 on success |
| `modify_order(order_id, new_qty)` | `int` | 0 on success |
| `best_bid()` | `Optional[float]` | Best bid price |
| `best_ask()` | `Optional[float]` | Best ask price |
| `bid_at_price(price)` | `float` | Total bid quantity at price |
| `ask_at_price(price)` | `float` | Total ask quantity at price |

### CompositeBook

Aggregates books across multiple exchanges per symbol.

```codon
cb = CompositeBook()
result = cb.best_bid(symbol_id)  # Optional[Tuple[float, float]]
```

| Method | Returns | Description |
|--------|---------|-------------|
| `best_bid(symbol)` | `Optional[Tuple[float, float]]` | `(price, qty)` or `None` |
| `best_ask(symbol)` | `Optional[Tuple[float, float]]` | `(price, qty)` or `None` |
| `has_arbitrage(symbol)` | `bool` | True if arbitrage opportunity exists |
| `mark_stale(exchange, symbol)` | `None` | Mark exchange data as stale |
| `check_staleness(now_ns, threshold_ns)` | `None` | Evict stale data |

---

## Position tracking

### PositionTracker

FIFO/average-cost position tracking.

```codon
tracker = PositionTracker()          # FIFO (default)
tracker = PositionTracker(1)         # average cost

tracker.on_fill(symbol, "buy", price, qty)
pos = tracker.position(symbol)
```

| Method | Returns | Description |
|--------|---------|-------------|
| `on_fill(symbol, side, price, qty)` | `None` | Record a fill |
| `position(symbol)` | `float` | Current net position |
| `avg_entry_price(symbol)` | `float` | Average entry price |
| `realized_pnl(symbol)` | `float` | Realized PnL for symbol |
| `total_realized_pnl()` | `float` | Total realized PnL |

### PositionGroupTracker

Tracks individual named positions (open/partial-close/close).

```codon
tracker = PositionGroupTracker()
pos_id = tracker.open_position(order_id, symbol, "buy", price, qty)
tracker.close_position(pos_id, exit_price)
```

| Method | Returns | Description |
|--------|---------|-------------|
| `open_position(order_id, symbol, side, price, qty)` | `int` (position ID) | Open a position |
| `close_position(position_id, exit_price)` | `None` | Close fully |
| `partial_close(position_id, qty, exit_price)` | `None` | Partial close |
| `net_position(symbol)` | `float` | Net position for symbol |
| `realized_pnl(symbol)` | `float` | Realized PnL for symbol |
| `total_realized_pnl()` | `float` | Total realized PnL |
| `open_position_count(symbol)` | `int` | Number of open positions |
| `prune()` | `None` | Free closed position memory |

### OrderTracker

Tracks submitted/filled/canceled orders.

```codon
tracker = OrderTracker()
tracker.on_submitted(order_id, symbol, "buy", price, qty)
tracker.on_filled(order_id, fill_qty)
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `on_submitted(order_id, symbol, side, price, qty)` | `bool` | Register a submitted order |
| `on_filled(order_id, fill_qty)` | `bool` | Record a fill |
| `on_canceled(order_id)` | `bool` | Record a cancellation |
| `is_active(order_id)` | `bool` | True if order is still active |
| `active_count` | `int` | Number of active orders |
| `total_count` | `int` | Total orders seen |
| `prune()` | `None` | Free completed order memory |

---

## Profiles

### VolumeProfile

```codon
vp = VolumeProfile(tick_size=0.01)
vp.add_trade(price, qty, is_buy)
poc = vp.poc()
```

| Method | Returns | Description |
|--------|---------|-------------|
| `add_trade(price, qty, is_buy)` | `None` | Feed a trade |
| `poc()` | `float` | Point of control |
| `value_area_high()` | `float` | Value area high |
| `value_area_low()` | `float` | Value area low |
| `total_volume()` | `float` | Total volume |
| `clear()` | `None` | Reset |

### MarketProfile

```codon
mp = MarketProfile(tick_size=0.01, period_minutes=30, session_start_ns=0)
mp.add_trade(timestamp_ns, price, qty, is_buy)
```

| Method | Returns | Description |
|--------|---------|-------------|
| `add_trade(ts_ns, price, qty, is_buy)` | `None` | Feed a trade |
| `poc()` | `float` | Point of control |
| `value_area_high()` | `float` | Value area high |
| `value_area_low()` | `float` | Value area low |
| `initial_balance_high()` | `float` | Initial balance high |
| `initial_balance_low()` | `float` | Initial balance low |
| `is_poor_high()` | `bool` | Poor high |
| `is_poor_low()` | `bool` | Poor low |
| `clear()` | `None` | Reset |

### FootprintBar

Buy/sell delta per price level.

```codon
fp = FootprintBar(tick_size=0.01)
fp.add_trade(price, qty, is_buy)
delta = fp.total_delta()
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `add_trade(price, qty, is_buy)` | `None` | Feed a trade |
| `total_delta()` | `float` | Net buy minus sell volume |
| `total_volume()` | `float` | Total volume |
| `num_levels` | `int` | Number of price levels |
| `clear()` | `None` | Reset |

---

## Statistics

```codon
from flox.tools import correlation, profit_factor, win_rate, permutation_test, bootstrap_ci

r = correlation(x, y)
pf = profit_factor(pnl_list)
wr = win_rate(pnl_list)
p = permutation_test(group1, group2, num_permutations=10000)
lo, med, hi = bootstrap_ci(data, confidence=0.95, num_samples=10000)
```

| Function | Returns | Description |
|----------|---------|-------------|
| `correlation(x, y)` | `float` | Pearson correlation |
| `profit_factor(pnl)` | `float` | Gross profit / gross loss |
| `win_rate(pnl)` | `float` | Fraction of positive values |
| `permutation_test(g1, g2, n=10000)` | `float` | Two-sample permutation p-value |
| `bootstrap_ci(data, confidence=0.95, n=10000)` | `Tuple[float, float, float]` | `(lower, median, upper)` |

---

## Data I/O

### DataWriter

```codon
writer = DataWriter("./data", max_segment_mb=256, exchange_id=0)
writer.write_trade(exchange_ts_ns, recv_ts_ns, price, qty, trade_id, symbol_id, side)
writer.flush()
writer.close()
```

| Method | Returns | Description |
|--------|---------|-------------|
| `write_trade(exchange_ts_ns, recv_ts_ns, price, qty, trade_id, symbol_id, side)` | `bool` | Write a trade record |
| `flush()` | `None` | Flush to disk |
| `close()` | `None` | Close and finalize |
| `stats()` | `WriterStats` | Write statistics |

`WriterStats` fields: `bytes_written`, `events_written`, `segments_created`, `trades_written`.

### DataReader

```codon
reader = DataReader("./data")
reader = DataReader.create_filtered("./data", from_ns=0, to_ns=0, symbols=[])
trades = reader.read_trades()
```

| Method / Property | Returns | Description |
|-------------------|---------|-------------|
| `count()` | `int` | Total event count |
| `summary()` | `DatasetSummary` | Dataset summary |
| `reader_stats()` | `ReaderStats` | Read statistics |
| `read_trades(max_trades=0)` | `List[TradeRecord]` | Read trades (all if `max_trades=0`) |

`DatasetSummary` fields: `first_event_ns`, `last_event_ns`, `total_events`, `segment_count`, `total_bytes`, `duration_seconds`.

`TradeRecord` fields: `exchange_ts_ns`, `recv_ts_ns`, `price_raw`, `qty_raw`, `trade_id`, `symbol_id`, `side`. Methods: `price()`, `qty()`, `is_buy()`.

### DataRecorder

```codon
recorder = DataRecorder("./data", exchange_name="binance", max_segment_mb=256)
recorder.add_symbol(symbol_id, "BTCUSDT", base="BTC", quote="USDT")
recorder.start()
# feed live data...
recorder.stop()
```

| Method / Property | Description |
|-------------------|-------------|
| `add_symbol(symbol_id, name, base="", quote="", price_precision=8, qty_precision=8)` | Register a symbol |
| `start()` | Start recording |
| `stop()` | Stop recording |
| `flush()` | Flush buffers to disk |
| `is_recording` | `bool` property |

---

## Partitioner

Splits a dataset into partitions for parallel backtesting.

```codon
p = Partitioner("./data")
partitions = p.by_time(num_partitions=4, warmup_ns=0)
for part in partitions:
    # part.from_ns, part.to_ns, part.warmup_from_ns, ...
    pass
p.close()
```

`Partition` fields: `partition_id`, `from_ns`, `to_ns`, `warmup_from_ns`, `estimated_events`, `estimated_bytes`.

| Method | Description |
|--------|-------------|
| `by_time(num_partitions, warmup_ns=0)` | Split into N equal time slices |
| `by_duration(duration_ns, warmup_ns=0)` | Split by fixed duration |
| `by_calendar(unit=CALENDAR_MONTHLY, warmup_ns=0)` | Split by calendar unit |
| `by_symbol(num_partitions)` | Split by symbol group |
| `per_symbol()` | One partition per symbol |
| `by_event_count(num_partitions)` | Split by event count |
| `close()` | Free resources |

Calendar unit constants: `Partitioner.CALENDAR_DAILY = 0`, `CALENDAR_WEEKLY = 1`, `CALENDAR_MONTHLY = 2`.

---

## Segment operations

```codon
from flox.tools import validate_segment, merge_segments, merge_dir, split_segment, export_segment

validate_segment("path/to/seg")            # bool
merge_segments("input_dir", "output.bin")  # bool
```

| Function | Returns | Description |
|----------|---------|-------------|
| `validate_segment(path)` | `bool` | Quick validation |
| `validate_segment_full(path, verify_crc=True, verify_timestamps=True)` | `SegmentValidation` | Full validation |
| `validate_dataset(data_dir)` | `DatasetValidation` | Validate all segments in directory |
| `merge_segments(input_dir, output_path)` | `bool` | Merge directory into one file |
| `merge_dir(input_dir, output_dir)` | `MergeResult` | Merge with result metadata |
| `split_segment(input_path, output_dir, mode=0, time_interval_ns=0, events_per_file=0)` | `SplitResult` | Split segment |
| `export_segment(input_path, output_path, format=0, from_ns=0, to_ns=0, symbols=[])` | `ExportResult` | Export to CSV or other format |
| `recompress_segment(input_path, output_path, compression=1)` | `bool` | Recompress |
| `extract_symbols(input_path, output_path, symbols)` | `int` | Extract specific symbols |
| `extract_time_range(input_path, output_path, from_ns, to_ns)` | `int` | Extract time slice |

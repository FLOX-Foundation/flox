"""Attach an OrderJourneyTracer to a backtest run and read its analytics."""
import flox_py as flox

registry = flox.SymbolRegistry()
sym = registry.add_symbol("backtest", "BTCUSDT", tick_size=0.01)

runner = flox.BacktestRunner(registry, fee_rate=0.0,
                              initial_capital=100_000.0)
runner.executor().set_queue_model("tob", 1)

tracer = flox.OrderJourneyTracer(
    max_orders=10_000,
    max_records_per_order=64,
    sample_rate=1.0,
)
runner.add_execution_listener(tracer)

# ... run the backtest ...

# Inspect a single order.
trace = tracer.journey(order_id=42)
for row in trace:
    print(row["seq"], row["status"], row["ts_ns"],
          row["queue_ahead"], row["is_maker"])

# Aggregate analytics.
print("orders:", tracer.order_count())
print("median ack latency:", tracer.median_ack_latency_ns(), "ns")
print("median time to first fill:",
      tracer.median_time_to_first_fill_ns(), "ns")
print("maker fill ratio:", tracer.maker_fill_ratio())
print("cancel race loss rate:", tracer.cancel_race_loss_rate())

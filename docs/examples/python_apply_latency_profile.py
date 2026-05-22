"""Apply a named latency profile to a SimulatedExecutor."""
import flox_py as flox

exec = flox.SimulatedExecutor()
exec.set_queue_model("tob", 1)
exec.apply_latency_profile("binance_um_futures")

# Equivalent fine-grained calls:
# exec.set_submit_ack_latency(5_000_000, 3_000_000)
# exec.set_cancel_ack_latency(8_000_000, 3_000_000)
# exec.set_replace_ack_latency(12_000_000, 4_000_000)

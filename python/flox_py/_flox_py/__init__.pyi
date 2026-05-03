"""
Flox -- Python bindings
"""
from __future__ import annotations
import collections.abc
import numpy
import numpy.typing
import typing
from . import targets
__all__: list[str] = ['ATR', 'AutoCorrelation', 'BacktestResult', 'BacktestRunner', 'BarData', 'Bollinger', 'CCI', 'CompositeBookMatrix', 'Correlation', 'DEMA', 'DataReader', 'DataRecorder', 'DataWriter', 'EMA', 'Engine', 'FootprintBar', 'IndicatorGraph', 'KAMA', 'Kurtosis', 'L3Book', 'MACD', 'MarketProfile', 'OrderBook', 'OrderTracker', 'PRICE_SCALE', 'ParkinsonVol', 'Partitioner', 'PositionGroupTracker', 'PositionTracker', 'QUANTITY_SCALE', 'QUEUE_FULL', 'QUEUE_NONE', 'QUEUE_TOB', 'RMA', 'RSI', 'RogersSatchellVol', 'RollingZScore', 'Runner', 'SLIPPAGE_FIXED_BPS', 'SLIPPAGE_FIXED_TICKS', 'SLIPPAGE_NONE', 'SLIPPAGE_VOLUME_IMPACT', 'SMA', 'ShannonEntropy', 'Signal', 'SignalBuilder', 'SimulatedExecutor', 'Skewness', 'Slope', 'Stats', 'Stochastic', 'Strategy', 'StreamingIndicatorGraph', 'Symbol', 'SymbolContext', 'SymbolRegistry', 'TEMA', 'TradeData', 'VOLUME_SCALE', 'VolumeProfile', 'adf', 'adx', 'aggregate_heikin_ashi_bars', 'aggregate_range_bars', 'aggregate_renko_bars', 'aggregate_tick_bars', 'aggregate_time_bars', 'aggregate_volume_bars', 'atr', 'autocorrelation', 'bar_returns', 'bollinger', 'bootstrap_ci', 'cci', 'chop', 'correlation', 'cvd', 'dema', 'ema', 'export_data', 'extract_symbols', 'extract_time_range', 'inspect', 'kama', 'kurtosis', 'list_indicators', 'macd', 'merge', 'merge_dir', 'obv', 'parkinson_vol', 'permutation_test', 'prices_to_double', 'profit_factor', 'quantities_to_double', 'recompress', 'rma', 'rogers_satchell_vol', 'rolling_zscore', 'rsi', 'shannon_entropy', 'skewness', 'slope', 'sma', 'split', 'stochastic', 'targets', 'tema', 'trade_pnl', 'validate', 'validate_dataset', 'volumes_to_double', 'vwap', 'win_rate']
class ATR:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg2: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class AutoCorrelation:
    def __init__(self, window: typing.SupportsInt | typing.SupportsIndex, lag: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class BacktestResult:
    def __init__(self, initial_capital: typing.SupportsFloat | typing.SupportsIndex = 100000.0, fee_rate: typing.SupportsFloat | typing.SupportsIndex = 0.0001, use_percentage_fee: bool = True, fixed_fee_per_trade: typing.SupportsFloat | typing.SupportsIndex = 0.0, risk_free_rate: typing.SupportsFloat | typing.SupportsIndex = 0.0, annualization_factor: typing.SupportsFloat | typing.SupportsIndex = 252.0) -> None:
        ...
    def equity_curve(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Return equity curve as numpy structured array
        """
    def ingest_executor(self, executor: SimulatedExecutor) -> None:
        """
        Drain executor fills into this result in FIFO order
        """
    def record_fill(self, order_id: typing.SupportsInt | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, timestamp_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def stats(self) -> dict:
        """
        Compute stats as a dict (includes new metrics: streaks, durations, TWR)
        """
    def trades(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Return trade records as numpy structured array
        """
    def write_equity_curve_csv(self, path: str) -> bool:
        """
        Write equity curve to CSV (timestamp_ns,equity,drawdown_pct header)
        """
class BacktestRunner:
    def __init__(self, registry: SymbolRegistry, fee_rate: typing.SupportsFloat | typing.SupportsIndex = 0.0004, initial_capital: typing.SupportsFloat | typing.SupportsIndex = 100000.0) -> None:
        ...
    def run_bars(self, start_time_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], end_time_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], open: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], volume: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], symbol: str = '', bar_type: typing.SupportsInt | typing.SupportsIndex = 0, bar_type_param: typing.SupportsInt | typing.SupportsIndex = 0) -> typing.Any:
        ...
    def run_csv(self, path: str, symbol: str = '') -> typing.Any:
        ...
    def run_ohlcv(self, ts: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], symbol: str = '') -> typing.Any:
        ...
    def set_strategy(self, strategy: Strategy) -> None:
        ...
class BarData:
    symbol_name: str
    def __init__(self) -> None:
        ...
    @property
    def bar_type(self) -> int:
        ...
    @bar_type.setter
    def bar_type(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def bar_type_param(self) -> int:
        ...
    @bar_type_param.setter
    def bar_type_param(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def buy_volume(self) -> float:
        ...
    @buy_volume.setter
    def buy_volume(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def close(self) -> float:
        ...
    @close.setter
    def close(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def close_reason(self) -> int:
        ...
    @close_reason.setter
    def close_reason(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def end_time_ns(self) -> int:
        ...
    @end_time_ns.setter
    def end_time_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def high(self) -> float:
        ...
    @high.setter
    def high(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def low(self) -> float:
        ...
    @low.setter
    def low(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def open(self) -> float:
        ...
    @open.setter
    def open(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def start_time_ns(self) -> int:
        ...
    @start_time_ns.setter
    def start_time_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def symbol(self) -> int:
        ...
    @symbol.setter
    def symbol(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def volume(self) -> float:
        ...
    @volume.setter
    def volume(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
class Bollinger:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex = 20, multiplier: typing.SupportsFloat | typing.SupportsIndex = 2.0) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> dict:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def lower(self) -> float | None:
        ...
    @property
    def middle(self) -> float | None:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def upper(self) -> float | None:
        ...
    @property
    def value(self) -> float | None:
        ...
class CCI:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex = 20) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg2: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class CompositeBookMatrix:
    """
    Cross-exchange composite order book. Tracks best bid/ask across up to 4 exchanges per symbol.
    """
    def __init__(self, staleness_threshold_ms: typing.SupportsInt | typing.SupportsIndex = 5000) -> None:
        """
        Create a CompositeBookMatrix with staleness threshold in milliseconds
        """
    def ask_for_exchange(self, symbol: typing.SupportsInt | typing.SupportsIndex, exchange: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Best ask on a specific exchange or None
        """
    def best_ask(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Best ask across all exchanges (dict with price, quantity, exchange) or None
        """
    def best_bid(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Best bid across all exchanges (dict with price, quantity, exchange) or None
        """
    def bid_for_exchange(self, symbol: typing.SupportsInt | typing.SupportsIndex, exchange: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Best bid on a specific exchange or None
        """
    def check_staleness(self, now_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Check all entries against staleness threshold using current timestamp
        """
    def has_arbitrage_opportunity(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> bool:
        """
        Check if best bid on one exchange > best ask on another
        """
    def mark_exchange_stale(self, exchange: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Mark all symbols on an exchange as stale
        """
    def mark_stale(self, exchange: typing.SupportsInt | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Mark a specific exchange+symbol as stale
        """
    def spread(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Composite spread (best ask - best bid) or None
        """
    def update_book(self, exchange: typing.SupportsInt | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex, bid_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], bid_quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ask_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ask_quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], recv_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        """
        Feed a book update from an exchange
        """
class Correlation:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, x: typing.SupportsFloat | typing.SupportsIndex, y: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class DEMA:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class DataReader:
    def __init__(self, data_dir: str, from_ns: typing.Any = None, to_ns: typing.Any = None, symbols: typing.Any = None) -> None:
        """
        Create a DataReader for a binary log data directory
        """
    def count(self) -> int:
        """
        Return the total number of events
        """
    def read_bbo(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Read best bid/ask from every book update as a numpy structured array (PyBBO dtype)
        """
    def read_bbo_from(self, start_ts_ns: typing.SupportsInt | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Read BBO starting from a given timestamp (nanoseconds)
        """
    def read_book_updates(self) -> tuple:
        """
        Read all book updates. Returns (headers, levels) tuple of numpy structured arrays. headers dtype: PyBookUpdateHeader; levels dtype: PyLevel. For event i: bids=levels[h['level_offset']:h['level_offset']+h['bid_count']], asks=levels[h['level_offset']+h['bid_count']:h['level_offset']+h['bid_count']+h['ask_count']]
        """
    def read_book_updates_from(self, start_ts_ns: typing.SupportsInt | typing.SupportsIndex) -> tuple:
        """
        Read book updates starting from a given timestamp (nanoseconds). Same return shape as read_book_updates().
        """
    def read_trades(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Read all trades as a numpy structured array (PyTrade dtype)
        """
    def read_trades_from(self, start_ts_ns: typing.SupportsInt | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Read trades starting from a given timestamp (nanoseconds)
        """
    def segment_files(self) -> list:
        """
        Return list of segment file paths
        """
    def segments(self) -> list:
        """
        Return list of segment info dicts
        """
    def stats(self) -> dict:
        """
        Return reader statistics as a dict
        """
    def summary(self) -> dict:
        """
        Return a dict summarizing the dataset
        """
    def symbols(self) -> set:
        """
        Return the set of available symbol IDs
        """
    def time_range(self) -> typing.Any:
        """
        Return (start_ns, end_ns) tuple or None
        """
class DataRecorder:
    def __init__(self, output_dir: str, exchange_name: str, max_segment_mb: typing.SupportsInt | typing.SupportsIndex = 256) -> None:
        """
        Create a DataRecorder for market data recording
        """
    def add_symbol(self, symbol_id: typing.SupportsInt | typing.SupportsIndex, name: str, base: str = '', quote: str = '', price_precision: typing.SupportsInt | typing.SupportsIndex = 8, qty_precision: typing.SupportsInt | typing.SupportsIndex = 8) -> None:
        """
        Register a symbol for recording metadata
        """
    def flush(self) -> None:
        """
        Flush buffered data to disk
        """
    def is_recording(self) -> bool:
        """
        Return True if currently recording
        """
    def start(self) -> None:
        """
        Start recording
        """
    def stats(self) -> dict:
        """
        Return recorder statistics as a dict
        """
    def stop(self) -> None:
        """
        Stop recording and finalize output
        """
class DataWriter:
    def __init__(self, output_dir: str, max_segment_mb: typing.SupportsInt | typing.SupportsIndex = 256, exchange_id: typing.SupportsInt | typing.SupportsIndex = 0, compression: str = 'none') -> None:
        """
        Create a DataWriter for binary log output
        """
    def close(self) -> None:
        """
        Close the writer and finalize all segments
        """
    def current_segment_path(self) -> str:
        """
        Return the path of the current segment being written
        """
    def flush(self) -> None:
        """
        Flush buffered data to disk
        """
    def stats(self) -> dict:
        """
        Return writer statistics as a dict
        """
    def write_trade(self, exchange_ts_ns: typing.SupportsInt | typing.SupportsIndex, recv_ts_ns: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, trade_id: typing.SupportsInt | typing.SupportsIndex, symbol_id: typing.SupportsInt | typing.SupportsIndex, side: typing.SupportsInt | typing.SupportsIndex) -> bool:
        """
        Write a single trade record
        """
    def write_trades(self, exchange_ts_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], recv_ts_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], trade_ids: typing.Annotated[numpy.typing.ArrayLike, numpy.uint64], symbol_ids: typing.Annotated[numpy.typing.ArrayLike, numpy.uint32], sides: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8]) -> int:
        """
        Write trades from numpy arrays (vectorized). Returns number of trades written.
        """
class EMA:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Engine:
    def __init__(self, initial_capital: typing.SupportsFloat | typing.SupportsIndex = 100000.0, fee_rate: typing.SupportsFloat | typing.SupportsIndex = 0.0001) -> None:
        ...
    def bar_count(self, symbol: str = '') -> int:
        ...
    def close(self, symbol: str = '') -> numpy.typing.NDArray[numpy.float64]:
        ...
    def high(self, symbol: str = '') -> numpy.typing.NDArray[numpy.float64]:
        ...
    def load_csv(self, path: str, symbol: str = '') -> None:
        ...
    def load_df(self, df: typing.Any, symbol: str = '') -> None:
        ...
    def load_ohlcv(self, data: dict, symbol: str = '') -> None:
        ...
    def low(self, symbol: str = '') -> numpy.typing.NDArray[numpy.float64]:
        ...
    def open(self, symbol: str = '') -> numpy.typing.NDArray[numpy.float64]:
        ...
    def resample(self, symbol: str, target: str, interval: str) -> None:
        ...
    def run(self, signals: SignalBuilder, default_symbol: typing.SupportsInt | typing.SupportsIndex = 0) -> Stats:
        ...
    def ts(self, symbol: str = '') -> numpy.typing.NDArray[numpy.float64]:
        ...
    def volume(self, symbol: str = '') -> numpy.typing.NDArray[numpy.float64]:
        ...
    @property
    def symbols(self) -> list:
        ...
class FootprintBar:
    """
    Footprint bar for order flow analysis. Tracks bid/ask volume at each price level.
    """
    def __init__(self, tick_size: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def add_trade(self, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool) -> None:
        ...
    def add_trades(self, prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8]) -> None:
        ...
    def clear(self) -> None:
        ...
    def highest_buying_pressure(self) -> float:
        ...
    def highest_selling_pressure(self) -> float:
        ...
    def levels(self) -> list:
        ...
    def num_levels(self) -> int:
        ...
    def strongest_imbalance(self, threshold: typing.SupportsFloat | typing.SupportsIndex = 0.7) -> typing.Any:
        ...
    def total_delta(self) -> float:
        ...
    def total_volume(self) -> float:
        ...
class IndicatorGraph:
    def __init__(self) -> None:
        ...
    def add_node(self, name: str, deps: collections.abc.Sequence[str], fn: typing.Any) -> None:
        ...
    def bar_count(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def close(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def current(self, symbol: typing.SupportsInt | typing.SupportsIndex, name: str) -> float:
        ...
    def get(self, symbol: typing.SupportsInt | typing.SupportsIndex, name: str) -> typing.Any:
        ...
    def high(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def indicator(self, name: str, indicator: typing.Any, source: str = 'close') -> None:
        """
        Add a node that runs `indicator.compute(graph.<source>(sym))`. Sugar over add_node.
        """
    def invalidate(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def invalidate_all(self) -> None:
        ...
    def low(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def require(self, symbol: typing.SupportsInt | typing.SupportsIndex, name: str) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def reset_all(self) -> None:
        ...
    def set_bars(self, symbol: typing.SupportsInt | typing.SupportsIndex, close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64] | None = None, low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64] | None = None, volume: typing.Annotated[numpy.typing.ArrayLike, numpy.float64] | None = None) -> None:
        ...
    def step(self, symbol: typing.SupportsInt | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex, high: typing.SupportsFloat | typing.SupportsIndex | None = None, low: typing.SupportsFloat | typing.SupportsIndex | None = None, volume: typing.SupportsFloat | typing.SupportsIndex | None = None) -> None:
        ...
    def volume(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
        ...
class KAMA:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex, fast: typing.SupportsInt | typing.SupportsIndex = 2, slow: typing.SupportsInt | typing.SupportsIndex = 30) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Kurtosis:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class L3Book:
    def __init__(self) -> None:
        ...
    def add_order(self, order_id: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, side: str) -> str:
        ...
    def ask_at_price(self, price: typing.SupportsFloat | typing.SupportsIndex) -> float:
        ...
    def best_ask(self) -> typing.Any:
        ...
    def best_bid(self) -> typing.Any:
        ...
    def bid_at_price(self, price: typing.SupportsFloat | typing.SupportsIndex) -> float:
        ...
    def build_from_snapshot(self, orders: list) -> None:
        ...
    def export_snapshot(self) -> list:
        ...
    def modify_order(self, order_id: typing.SupportsInt | typing.SupportsIndex, new_quantity: typing.SupportsFloat | typing.SupportsIndex) -> str:
        ...
    def remove_order(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> str:
        ...
class MACD:
    def __init__(self, fast: typing.SupportsInt | typing.SupportsIndex = 12, slow: typing.SupportsInt | typing.SupportsIndex = 26, signal: typing.SupportsInt | typing.SupportsIndex = 9) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> dict:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def histogram(self) -> float | None:
        ...
    @property
    def line(self) -> float | None:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def signal(self) -> float | None:
        ...
    @property
    def value(self) -> float | None:
        ...
class MarketProfile:
    """
    Market Profile (TPO) aggregator. Tracks price activity across time periods.
    """
    def __init__(self, tick_size: typing.SupportsFloat | typing.SupportsIndex, period_minutes: typing.SupportsInt | typing.SupportsIndex, session_start_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def add_trade(self, timestamp_ns: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool) -> None:
        ...
    def add_trades(self, timestamps_ns: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8]) -> None:
        ...
    def clear(self) -> None:
        ...
    def current_period(self) -> int:
        ...
    def initial_balance_high(self) -> float:
        ...
    def initial_balance_low(self) -> float:
        ...
    def is_poor_high(self) -> bool:
        ...
    def is_poor_low(self) -> bool:
        ...
    def levels(self) -> list:
        ...
    def num_levels(self) -> int:
        ...
    def poc(self) -> float:
        ...
    def single_prints(self) -> list:
        ...
    def value_area_high(self) -> float:
        ...
    def value_area_low(self) -> float:
        ...
class OrderBook:
    def __init__(self, tick_size: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def apply_delta(self, bid_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], bid_quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ask_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ask_quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> None:
        """
        Apply an incremental book update
        """
    def apply_snapshot(self, bid_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], bid_quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ask_prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], ask_quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> None:
        """
        Apply a full book snapshot
        """
    def ask_at_price(self, price: typing.SupportsFloat | typing.SupportsIndex) -> float:
        ...
    def best_ask(self) -> typing.Any:
        """
        Best ask price or None
        """
    def best_bid(self) -> typing.Any:
        """
        Best bid price or None
        """
    def bid_at_price(self, price: typing.SupportsFloat | typing.SupportsIndex) -> float:
        ...
    def clear(self) -> None:
        ...
    def consume_asks(self, quantity: typing.SupportsFloat | typing.SupportsIndex) -> tuple:
        """
        Simulate market buy: returns (filled_qty, total_cost)
        """
    def consume_bids(self, quantity: typing.SupportsFloat | typing.SupportsIndex) -> tuple:
        """
        Simulate market sell: returns (filled_qty, total_cost)
        """
    def get_asks(self, max_levels: typing.SupportsInt | typing.SupportsIndex = 20) -> numpy.typing.NDArray[numpy.float64]:
        """
        Get ask levels as Nx2 array [price, qty]
        """
    def get_bids(self, max_levels: typing.SupportsInt | typing.SupportsIndex = 20) -> numpy.typing.NDArray[numpy.float64]:
        """
        Get bid levels as Nx2 array [price, qty]
        """
    def is_crossed(self) -> bool:
        ...
    def mid(self) -> typing.Any:
        """
        Mid price or None
        """
    def spread(self) -> typing.Any:
        """
        Bid-ask spread or None
        """
class OrderTracker:
    def __init__(self) -> None:
        ...
    def active_count(self) -> int:
        """
        Number of active orders
        """
    def get(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Get order state dict or None
        """
    def is_active(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> bool:
        """
        Check if order is active
        """
    def on_canceled(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> bool:
        """
        Record order cancellation
        """
    def on_filled(self, order_id: typing.SupportsInt | typing.SupportsIndex, fill_quantity: typing.SupportsFloat | typing.SupportsIndex) -> bool:
        """
        Record order fill
        """
    def on_rejected(self, order_id: typing.SupportsInt | typing.SupportsIndex, reason: str) -> bool:
        """
        Record order rejection
        """
    def on_submitted(self, order_id: typing.SupportsInt | typing.SupportsIndex, exchange_order_id: str, client_order_id: str = '') -> bool:
        """
        Record order submission
        """
    def prune_terminal(self) -> None:
        """
        Remove terminal orders from memory
        """
    def total_count(self) -> int:
        """
        Total tracked orders
        """
class ParkinsonVol:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Partitioner:
    def __init__(self, data_dir: str) -> None:
        ...
    def partition_by_calendar(self, unit: str, warmup_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> list:
        ...
    def partition_by_duration(self, duration_ns: typing.SupportsInt | typing.SupportsIndex, warmup_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> list:
        ...
    def partition_by_event_count(self, num_partitions: typing.SupportsInt | typing.SupportsIndex) -> list:
        ...
    def partition_by_symbol(self, num_partitions: typing.SupportsInt | typing.SupportsIndex) -> list:
        ...
    def partition_by_time(self, num_partitions: typing.SupportsInt | typing.SupportsIndex, warmup_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> list:
        ...
    def partition_per_symbol(self) -> list:
        ...
class PositionGroupTracker:
    def __init__(self) -> None:
        ...
    def assign_to_group(self, position_id: typing.SupportsInt | typing.SupportsIndex, group_id: typing.SupportsInt | typing.SupportsIndex) -> bool:
        """
        Assign a position to a group
        """
    def close_position(self, position_id: typing.SupportsInt | typing.SupportsIndex, exit_price: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Close a position at exit price
        """
    def create_group(self, parent_id: typing.SupportsInt | typing.SupportsIndex = 0) -> int:
        """
        Create a position group, returns group id
        """
    def get_position(self, position_id: typing.SupportsInt | typing.SupportsIndex) -> typing.Any:
        """
        Get position dict or None
        """
    def group_net_position(self, group_id: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Net position for a group
        """
    def group_realized_pnl(self, group_id: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Realized PnL for a group
        """
    def group_unrealized_pnl(self, group_id: typing.SupportsInt | typing.SupportsIndex, current_price: typing.SupportsFloat | typing.SupportsIndex) -> float:
        """
        Unrealized PnL for a group at current price
        """
    def net_position(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Net position for symbol
        """
    def open_position(self, order_id: typing.SupportsInt | typing.SupportsIndex, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        """
        Open a new individual position, returns position id
        """
    def open_position_count(self, symbol: typing.Any = None) -> int:
        """
        Count of open positions (optionally filtered by symbol)
        """
    def open_positions(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> list:
        """
        List of open position dicts for symbol
        """
    def partial_close(self, position_id: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, exit_price: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Partially close a position
        """
    def prune_closed(self) -> None:
        """
        Remove closed positions from memory
        """
    def realized_pnl(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Realized PnL for symbol
        """
    def total_realized_pnl(self) -> float:
        """
        Total realized PnL across all symbols
        """
class PositionTracker:
    def __init__(self, cost_basis: str = 'fifo') -> None:
        ...
    def avg_entry_price(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Average entry price for symbol
        """
    def on_fill(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Record a fill
        """
    def position(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Signed position for symbol
        """
    def realized_pnl(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> float:
        """
        Realized PnL for symbol
        """
    def total_realized_pnl(self) -> float:
        """
        Total realized PnL across all symbols
        """
class RMA:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class RSI:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class RogersSatchellVol:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg2: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg3: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, open: typing.SupportsFloat | typing.SupportsIndex, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class RollingZScore:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Runner:
    def __init__(self, registry: SymbolRegistry, on_signal: typing.Any, threaded: bool = False) -> None:
        ...
    def add_strategy(self, strategy: Strategy) -> None:
        ...
    def on_bar(self, symbol: typing.Any, open: typing.SupportsFloat | typing.SupportsIndex, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex, volume: typing.SupportsFloat | typing.SupportsIndex = 0.0, buy_volume: typing.SupportsFloat | typing.SupportsIndex = 0.0, start_time_ns: typing.SupportsInt | typing.SupportsIndex = 0, end_time_ns: typing.SupportsInt | typing.SupportsIndex = 0, bar_type: typing.SupportsInt | typing.SupportsIndex = 0, bar_type_param: typing.SupportsInt | typing.SupportsIndex = 0, close_reason: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def on_book_snapshot(self, symbol: typing.Any, bid_prices: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], bid_qtys: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], ask_prices: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], ask_qtys: collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex], ts_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def on_trade(self, symbol: typing.Any, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool, ts_ns: typing.SupportsInt | typing.SupportsIndex = 0) -> None:
        ...
    def start(self) -> None:
        ...
    def stop(self) -> None:
        ...
class SMA:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class ShannonEntropy:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex, bins: typing.SupportsInt | typing.SupportsIndex = 10) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Signal:
    order_type: str
    side: str
    def __init__(self) -> None:
        ...
    @property
    def new_price(self) -> float:
        ...
    @new_price.setter
    def new_price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def new_quantity(self) -> float:
        ...
    @new_quantity.setter
    def new_quantity(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def order_id(self) -> int:
        ...
    @order_id.setter
    def order_id(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def price(self) -> float:
        ...
    @price.setter
    def price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def quantity(self) -> float:
        ...
    @quantity.setter
    def quantity(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def symbol(self) -> int:
        ...
    @symbol.setter
    def symbol(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def trailing_bps(self) -> int:
        ...
    @trailing_bps.setter
    def trailing_bps(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def trailing_offset(self) -> float:
        ...
    @trailing_offset.setter
    def trailing_offset(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def trigger_price(self) -> float:
        ...
    @trigger_price.setter
    def trigger_price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
class SignalBuilder:
    def __init__(self) -> None:
        ...
    def __len__(self) -> int:
        ...
    def buy(self, ts: typing.SupportsInt | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str = '') -> None:
        ...
    def clear(self) -> None:
        ...
    def limit_buy(self, ts: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str = '') -> None:
        ...
    def limit_sell(self, ts: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str = '') -> None:
        ...
    def sell(self, ts: typing.SupportsInt | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str = '') -> None:
        ...
class SimulatedExecutor:
    def __init__(self) -> None:
        ...
    def advance_clock(self, timestamp_ns: typing.SupportsInt | typing.SupportsIndex) -> None:
        """
        Advance simulation clock to timestamp
        """
    def cancel_all(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def cancel_order(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def fills(self) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
        """
        Get all fills as numpy structured array
        """
    def fills_list(self) -> list:
        """
        Get all fills as list of dicts
        """
    def on_bar(self, symbol: typing.SupportsInt | typing.SupportsIndex, close_price: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Feed a bar close price for order matching
        """
    def on_best_levels(self, symbol: typing.SupportsInt | typing.SupportsIndex, bid_price: typing.SupportsFloat | typing.SupportsIndex, bid_qty: typing.SupportsFloat | typing.SupportsIndex, ask_price: typing.SupportsFloat | typing.SupportsIndex, ask_qty: typing.SupportsFloat | typing.SupportsIndex) -> None:
        """
        Feed a top-of-book snapshot (both best bid and best ask in one call)
        """
    def on_book_snapshot(self, symbol: typing.SupportsInt | typing.SupportsIndex, bid_levels: collections.abc.Sequence[tuple[typing.SupportsFloat | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]], ask_levels: collections.abc.Sequence[tuple[typing.SupportsFloat | typing.SupportsIndex, typing.SupportsFloat | typing.SupportsIndex]]) -> None:
        """
        Feed a full L2 snapshot. bid_levels and ask_levels are lists of (price, qty) tuples
        """
    def on_trade(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool) -> None:
        """
        Feed a trade for order matching
        """
    def on_trade_qty(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool) -> None:
        """
        Feed a trade with quantity (enables queue-fill simulation)
        """
    def set_default_slippage(self, model: str, ticks: typing.SupportsInt | typing.SupportsIndex = 0, tick_size: typing.SupportsFloat | typing.SupportsIndex = 0.0, bps: typing.SupportsFloat | typing.SupportsIndex = 0.0, impact_coeff: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> None:
        """
        Configure default slippage. model: none|fixed_ticks|fixed_bps|volume_impact. tick_size is in price units (0.0 falls back to one raw price unit).
        """
    def set_queue_model(self, model: str, depth: typing.SupportsInt | typing.SupportsIndex = 1) -> None:
        """
        Configure queue simulation. model: none|tob|full
        """
    def set_symbol_slippage(self, symbol: typing.SupportsInt | typing.SupportsIndex, model: str, ticks: typing.SupportsInt | typing.SupportsIndex = 0, tick_size: typing.SupportsFloat | typing.SupportsIndex = 0.0, bps: typing.SupportsFloat | typing.SupportsIndex = 0.0, impact_coeff: typing.SupportsFloat | typing.SupportsIndex = 0.0) -> None:
        """
        Configure slippage for a specific symbol
        """
    def submit_order(self, id: typing.SupportsInt | typing.SupportsIndex, side: str, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, type: str = 'market', symbol: typing.SupportsInt | typing.SupportsIndex = 1) -> None:
        """
        Submit an order to the simulated exchange
        """
    @property
    def fill_count(self) -> int:
        ...
class Skewness:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Slope:
    def __init__(self, length: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Stats:
    def __getitem__(self, arg0: str) -> typing.Any:
        ...
    def __repr__(self) -> str:
        ...
    def to_dict(self) -> dict:
        ...
    @property
    def avg_loss(self) -> float:
        ...
    @property
    def avg_win(self) -> float:
        ...
    @property
    def calmar(self) -> float:
        ...
    @property
    def final_capital(self) -> float:
        ...
    @property
    def gross_loss(self) -> float:
        ...
    @property
    def gross_profit(self) -> float:
        ...
    @property
    def initial_capital(self) -> float:
        ...
    @property
    def losing_trades(self) -> int:
        ...
    @property
    def max_drawdown(self) -> float:
        ...
    @property
    def max_drawdown_pct(self) -> float:
        ...
    @property
    def net_pnl(self) -> float:
        ...
    @property
    def profit_factor(self) -> float:
        ...
    @property
    def return_pct(self) -> float:
        ...
    @property
    def sharpe(self) -> float:
        ...
    @property
    def sortino(self) -> float:
        ...
    @property
    def total_fees(self) -> float:
        ...
    @property
    def total_pnl(self) -> float:
        ...
    @property
    def total_trades(self) -> int:
        ...
    @property
    def win_rate(self) -> float:
        ...
    @property
    def winning_trades(self) -> int:
        ...
class Stochastic:
    def __init__(self, k_period: typing.SupportsInt | typing.SupportsIndex = 14, d_period: typing.SupportsInt | typing.SupportsIndex = 3) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], arg2: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> dict:
        ...
    def reset(self) -> None:
        ...
    def update(self, high: typing.SupportsFloat | typing.SupportsIndex, low: typing.SupportsFloat | typing.SupportsIndex, close: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def d(self) -> float | None:
        ...
    @property
    def k(self) -> float | None:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class Strategy:
    def __init__(self, symbols: list) -> None:
        ...
    def best_ask(self, symbol: str | None = None) -> float:
        ...
    def best_bid(self, symbol: str | None = None) -> float:
        ...
    def cancel_all_orders(self, symbol: str | None = None) -> None:
        ...
    def cancel_order(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def close_position(self, symbol: str | None = None) -> int:
        ...
    def ctx(self, symbol: typing.SupportsInt | typing.SupportsIndex | None = None) -> SymbolContext:
        ...
    def emit_cancel(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def emit_cancel_all(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def emit_close_position(self, symbol: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def emit_limit_buy(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_limit_buy_tif(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, tif: str = 'gtc') -> int:
        ...
    def emit_limit_sell(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_limit_sell_tif(self, symbol: typing.SupportsInt | typing.SupportsIndex, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, tif: str = 'gtc') -> int:
        ...
    def emit_market_buy(self, symbol: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_market_sell(self, symbol: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_modify(self, order_id: typing.SupportsInt | typing.SupportsIndex, new_price: typing.SupportsFloat | typing.SupportsIndex, new_quantity: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def emit_stop_limit(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, limit_price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_stop_market(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_take_profit_limit(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, limit_price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_take_profit_market(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_trailing_stop(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, offset: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def emit_trailing_stop_percent(self, symbol: typing.SupportsInt | typing.SupportsIndex, side: str, callback_bps: typing.SupportsInt | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex) -> int:
        ...
    def get_order_status(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def last_price(self, symbol: str | None = None) -> float:
        ...
    def limit_buy(self, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None, tif: str = 'gtc') -> int:
        ...
    def limit_sell(self, price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None, tif: str = 'gtc') -> int:
        ...
    def market_buy(self, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def market_sell(self, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def mid_price(self, symbol: str | None = None) -> float:
        ...
    def modify_order(self, order_id: typing.SupportsInt | typing.SupportsIndex, new_price: typing.SupportsFloat | typing.SupportsIndex, new_qty: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def on_bar(self, ctx: SymbolContext, bar: BarData) -> None:
        ...
    def on_book_update(self, ctx: SymbolContext) -> None:
        ...
    def on_start(self) -> None:
        ...
    def on_stop(self) -> None:
        ...
    def on_trade(self, ctx: SymbolContext, trade: TradeData) -> None:
        ...
    def order_status(self, order_id: typing.SupportsInt | typing.SupportsIndex) -> int:
        ...
    def pos(self, symbol: str | None = None) -> float:
        ...
    def position(self, symbol: typing.SupportsInt | typing.SupportsIndex | None = None) -> float:
        ...
    def stop_limit(self, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, limit_price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def stop_market(self, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def take_profit_limit(self, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, limit_price: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def take_profit_market(self, side: str, trigger: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def trailing_stop(self, side: str, offset: typing.SupportsFloat | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    def trailing_stop_percent(self, side: str, callback_bps: typing.SupportsInt | typing.SupportsIndex, qty: typing.SupportsFloat | typing.SupportsIndex, symbol: str | None = None) -> int:
        ...
    @property
    def primary_symbol_name(self) -> str:
        ...
    @property
    def symbol_names(self) -> list[str]:
        ...
    @property
    def symbols(self) -> list[int]:
        ...
class Symbol:
    def __eq__(self, arg0: object) -> bool:
        ...
    def __hash__(self) -> int:
        ...
    def __index__(self) -> int:
        ...
    def __int__(self) -> int:
        ...
    def __repr__(self) -> str:
        ...
    def __str__(self) -> str:
        ...
    @property
    def exchange(self) -> str:
        ...
    @property
    def id(self) -> int:
        ...
    @property
    def name(self) -> str:
        ...
    @property
    def tick_size(self) -> float:
        ...
class SymbolContext:
    symbol: str
    def __init__(self) -> None:
        ...
    def book_spread(self) -> float:
        ...
    def is_flat(self) -> bool:
        ...
    def is_long(self) -> bool:
        ...
    def is_short(self) -> bool:
        ...
    @property
    def best_ask(self) -> float:
        ...
    @best_ask.setter
    def best_ask(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def best_bid(self) -> float:
        ...
    @best_bid.setter
    def best_bid(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def last_trade_price(self) -> float:
        ...
    @last_trade_price.setter
    def last_trade_price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def mid_price(self) -> float:
        ...
    @mid_price.setter
    def mid_price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def position(self) -> float:
        ...
    @position.setter
    def position(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def symbol_id(self) -> int:
        ...
    @symbol_id.setter
    def symbol_id(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def unrealized_pnl(self) -> float:
        ...
    @unrealized_pnl.setter
    def unrealized_pnl(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
class SymbolRegistry:
    def __init__(self) -> None:
        ...
    def add_symbol(self, exchange: str, symbol: str, tick_size: typing.SupportsFloat | typing.SupportsIndex = 0.01) -> Symbol:
        ...
    def symbol_count(self) -> int:
        ...
class TEMA:
    def __init__(self, period: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    def compute(self, arg0: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
        ...
    def reset(self) -> None:
        ...
    def update(self, value: typing.SupportsFloat | typing.SupportsIndex) -> float | None:
        ...
    @property
    def count(self) -> int:
        ...
    @property
    def ready(self) -> bool:
        ...
    @property
    def value(self) -> float | None:
        ...
class TradeData:
    is_buy: bool
    side: str
    symbol_name: str
    def __init__(self) -> None:
        ...
    @property
    def price(self) -> float:
        ...
    @price.setter
    def price(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def quantity(self) -> float:
        ...
    @quantity.setter
    def quantity(self, arg0: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    @property
    def symbol(self) -> int:
        ...
    @symbol.setter
    def symbol(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
    @property
    def timestamp_ns(self) -> int:
        ...
    @timestamp_ns.setter
    def timestamp_ns(self, arg0: typing.SupportsInt | typing.SupportsIndex) -> None:
        ...
class VolumeProfile:
    """
    Volume Profile aggregator. Tracks volume distribution across price levels.
    """
    def __init__(self, tick_size: typing.SupportsFloat | typing.SupportsIndex) -> None:
        ...
    def add_trade(self, price: typing.SupportsFloat | typing.SupportsIndex, quantity: typing.SupportsFloat | typing.SupportsIndex, is_buy: bool) -> None:
        ...
    def add_trades(self, prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8]) -> None:
        ...
    def clear(self) -> None:
        ...
    def levels(self) -> list:
        ...
    def num_levels(self) -> int:
        ...
    def poc(self) -> float:
        ...
    def total_delta(self) -> float:
        ...
    def total_volume(self) -> float:
        ...
    def value_area_high(self) -> float:
        ...
    def value_area_low(self) -> float:
        ...
    def volume_at(self, price: typing.SupportsFloat | typing.SupportsIndex) -> float:
        ...
def adf(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], max_lag: typing.SupportsInt | typing.SupportsIndex = 4, regression: str = 'c') -> dict:
    ...
def adx(high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex = 14) -> dict:
    ...
def aggregate_heikin_ashi_bars(timestamps: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8], interval_seconds: typing.SupportsFloat | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
    """
    Aggregate trades into Heikin-Ashi bars
    """
def aggregate_range_bars(timestamps: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8], range_size: typing.SupportsFloat | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
    """
    Aggregate trades into range bars
    """
def aggregate_renko_bars(timestamps: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8], brick_size: typing.SupportsFloat | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
    """
    Aggregate trades into renko bars
    """
def aggregate_tick_bars(timestamps: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8], tick_count: typing.SupportsInt | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
    """
    Aggregate trades into tick bars
    """
def aggregate_time_bars(timestamps: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8], interval_seconds: typing.SupportsFloat | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
    """
    Aggregate trades into time bars
    """
def aggregate_volume_bars(timestamps: typing.Annotated[numpy.typing.ArrayLike, numpy.int64], prices: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], quantities: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], is_buy: typing.Annotated[numpy.typing.ArrayLike, numpy.uint8], volume_threshold: typing.SupportsFloat | typing.SupportsIndex) -> numpy.ndarray[typing.Any, numpy.dtype[numpy.void]]:
    """
    Aggregate trades into volume bars
    """
def atr(high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def autocorrelation(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], window: typing.SupportsInt | typing.SupportsIndex, lag: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def bar_returns(signal_long: typing.Annotated[numpy.typing.ArrayLike, numpy.int8], signal_short: typing.Annotated[numpy.typing.ArrayLike, numpy.int8], log_returns: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
    ...
def bollinger(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex = 20, stddev: typing.SupportsFloat | typing.SupportsIndex = 2.0) -> dict:
    ...
def bootstrap_ci(data: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], confidence: typing.SupportsFloat | typing.SupportsIndex = 0.95, num_samples: typing.SupportsInt | typing.SupportsIndex = 10000) -> tuple[float, float, float]:
    """
    Bootstrap confidence interval, returns (lower, median, upper)
    """
def cci(high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex = 20) -> numpy.typing.NDArray[numpy.float64]:
    ...
def chop(high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex = 14) -> numpy.typing.NDArray[numpy.float64]:
    ...
@typing.overload
def correlation(x: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], y: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
@typing.overload
def correlation(x: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], y: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> float:
    """
    Pearson correlation coefficient
    """
def cvd(open: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], volume: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
    ...
def dema(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def ema(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def export_data(input_path: str, output_path: str, format: str = 'csv', from_ns: typing.Any = None, to_ns: typing.Any = None, symbols: typing.Any = None) -> dict:
    ...
def extract_symbols(input_path: str, output_path: str, symbols: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex]) -> int:
    ...
def extract_time_range(input_path: str, output_path: str, from_ns: typing.SupportsInt | typing.SupportsIndex, to_ns: typing.SupportsInt | typing.SupportsIndex) -> int:
    ...
def inspect(data_dir: str) -> dict:
    """
    Inspect a data directory and return summary statistics
    """
def kama(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex = 10) -> numpy.typing.NDArray[numpy.float64]:
    ...
def kurtosis(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def list_indicators() -> list:
    """
    Return the list of indicator names registered in this build (both batch compute() and streaming update()/value on each).
    """
def macd(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], fast: typing.SupportsInt | typing.SupportsIndex = 12, slow: typing.SupportsInt | typing.SupportsIndex = 26, signal: typing.SupportsInt | typing.SupportsIndex = 9) -> dict:
    ...
def merge(input_paths: collections.abc.Sequence[str], output_dir: str, output_name: str = 'merged', compression: str = 'none', sort: bool = True) -> dict:
    ...
def merge_dir(input_dir: str, output_dir: str) -> dict:
    ...
def obv(close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], volume: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
    ...
def parkinson_vol(high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def permutation_test(group1: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], group2: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], num_permutations: typing.SupportsInt | typing.SupportsIndex = 10000) -> float:
    """
    Two-sample permutation test, returns p-value
    """
def prices_to_double(raw: typing.Annotated[numpy.typing.ArrayLike, numpy.int64]) -> numpy.typing.NDArray[numpy.float64]:
    """
    Convert raw int64 price array to float64 array (divides by PRICE_SCALE).
    """
def profit_factor(returns: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> float:
    ...
def quantities_to_double(raw: typing.Annotated[numpy.typing.ArrayLike, numpy.int64]) -> numpy.typing.NDArray[numpy.float64]:
    """
    Convert raw int64 quantity array to float64 array (divides by QUANTITY_SCALE).
    """
def recompress(input_path: str, output_path: str, compression: str = 'lz4') -> bool:
    ...
def rma(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def rogers_satchell_vol(open: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def rolling_zscore(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def rsi(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def shannon_entropy(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex, bins: typing.SupportsInt | typing.SupportsIndex = 10) -> numpy.typing.NDArray[numpy.float64]:
    ...
def skewness(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def slope(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], length: typing.SupportsInt | typing.SupportsIndex = 1) -> numpy.typing.NDArray[numpy.float64]:
    ...
def sma(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def split(input_path: str, output_dir: str, mode: str = 'time', time_interval_ns: typing.SupportsInt | typing.SupportsIndex = 3600000000000, events_per_file: typing.SupportsInt | typing.SupportsIndex = 1000000) -> dict:
    ...
def stochastic(high: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], low: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], k_period: typing.SupportsInt | typing.SupportsIndex = 14, d_period: typing.SupportsInt | typing.SupportsIndex = 3) -> dict:
    ...
def tema(input: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], period: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def trade_pnl(signal_long: typing.Annotated[numpy.typing.ArrayLike, numpy.int8], signal_short: typing.Annotated[numpy.typing.ArrayLike, numpy.int8], log_returns: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> numpy.typing.NDArray[numpy.float64]:
    ...
def validate(segment_path: str, verify_crc: bool = True, verify_timestamps: bool = True) -> dict:
    ...
def validate_dataset(data_dir: str) -> dict:
    ...
def volumes_to_double(raw: typing.Annotated[numpy.typing.ArrayLike, numpy.int64]) -> numpy.typing.NDArray[numpy.float64]:
    """
    Convert raw int64 volume array to float64 array (divides by VOLUME_SCALE).
    """
def vwap(close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], volume: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], window: typing.SupportsInt | typing.SupportsIndex = 96) -> numpy.typing.NDArray[numpy.float64]:
    ...
def win_rate(trade_pnls: typing.Annotated[numpy.typing.ArrayLike, numpy.float64]) -> float:
    ...
PRICE_SCALE: int = 100000000
QUANTITY_SCALE: int = 100000000
QUEUE_FULL: int = 2
QUEUE_NONE: int = 0
QUEUE_TOB: int = 1
SLIPPAGE_FIXED_BPS: int = 2
SLIPPAGE_FIXED_TICKS: int = 1
SLIPPAGE_NONE: int = 0
SLIPPAGE_VOLUME_IMPACT: int = 3
VOLUME_SCALE: int = 100000000
StreamingIndicatorGraph = IndicatorGraph

// TypeScript definitions for @flox-foundation/flox.
//
// Hand-written from the N-API binding in node/src/. Kept in sync with the
// C++ surface by:
//   - scripts/check_dts_exports.py   (source-level: every `exports.Set("X", ...)`
//                                     in node/src/*.h has a matching declaration)
//   - node/test/test_types.ts        (signature-level: tsc --noEmit)
// Both run in CI (linux-gcc job).

// ── Errors ────────────────────────────────────────────────────────────

/**
 * Structured FLOX error. Thrown by Engine / BacktestRunner / etc. when
 * a stable error code is appropriate. Compatible with `instanceof Error`.
 *
 * The `code` follows the convention `E_<DOMAIN>_<NNN>` (e.g. `E_IO_001`,
 * `E_RUN_002`). The `helpUrl` points to the canonical fix-recipe page on
 * the FLOX docs site.
 *
 * Example:
 *
 * ```js
 * try {
 *   engine.loadCsv("missing.csv", "BTC");
 * } catch (e) {
 *   if (e.code === "E_IO_001") console.log(`see ${e.helpUrl}`);
 *   throw e;
 * }
 * ```
 */
export interface FloxError extends Error {
  readonly code: string;
  readonly helpUrl: string;
}

// ── Common types ──────────────────────────────────────────────────────

/** Side of an order or trade. */
export type Side = "buy" | "sell";

/** Order type accepted by the executors. */
export type OrderType =
  | "market"
  | "limit"
  | "stop_market"
  | "stop_limit"
  | "trailing_stop";

/** Slippage model name accepted by `SimulatedExecutor.setDefaultSlippage`. */
export type SlippageModel =
  | "none"
  | "fixed_ticks"
  | "fixed_bps"
  | "volume_impact";

/** Limit-order queue model name accepted by `SimulatedExecutor.setQueueModel`. */
export type QueueModel = "none" | "tob" | "full";

/** Position cost-basis mode accepted by `PositionTracker`. */
export type PositionMode = 0 | 1;

/** OHLCV bar emitted by the C++ aggregators / fed via `Runner.onBar`. */
export interface BarData {
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
  buyVolume: number;
  startTimeNs: number;
  endTimeNs: number;
  /** 0=Time, 1=Tick, 2=Volume, 3=Range, 4=Renko, 5=HeikinAshi. */
  barType: number;
  /** Interval / threshold for the active bar policy. */
  barTypeParam: number;
  /** 0=Threshold, 1=Gap, 2=Forced, 3=Warmup. */
  closeReason: number;
}

/** Per-symbol live context passed to strategy callbacks. */
export interface SymbolContext {
  position: number;
  symbolId: number;
  lastTradePrice: number;
  bestBid: number;
  bestAsk: number;
  midPrice: number;
}

/** Trade tick passed to `onTrade`. */
export interface TradeData {
  price: number;
  qty: number;
  isBuy: boolean;
  side: Side;
  timestampNs: bigint;
}

/** Order-event lifecycle status, mirrored from `FloxOrderEventStatus`. */
export type OrderEventStatus =
  | "NEW"
  | "ACCEPTED"
  | "PENDING_NEW"
  | "PARTIALLY_FILLED"
  | "FILLED"
  | "PENDING_CANCEL"
  | "CANCELED"
  | "EXPIRED"
  | "REJECTED"
  | "REPLACED"
  | "PENDING_TRIGGER"
  | "TRIGGERED"
  | "TRAILING_UPDATED"
  | "UNKNOWN";

/** Order-event payload delivered to `onFill` / `onOrderUpdate`. */
export interface OrderEventData {
  orderId: number;
  symbolId: number;
  side: Side;
  orderType:
    | "LIMIT"
    | "MARKET"
    | "STOP_MARKET"
    | "STOP_LIMIT"
    | "TP_MARKET"
    | "TP_LIMIT"
    | "ICEBERG"
    | "UNKNOWN";
  status: OrderEventStatus;
  /** Cumulative or last-fill quantity depending on the executor. */
  fillQty: number;
  /** Last-fill price; 0 for non-fill events. */
  fillPrice: number;
  exchangeTsNs: number;
  /** Set only when status === "REJECTED". */
  rejectReason: string | null;
}

/** Order-emission helper passed as the third arg to strategy callbacks. */
export interface EmitMethods {
  marketBuy(qty: number): void;
  marketSell(qty: number): void;
  limitBuy(price: number, qty: number): void;
  limitSell(price: number, qty: number): void;
  cancel(orderId: number): void;
  closePosition(): void;
}

/** Backtest signal emitted by `Runner` / collected by `SimulatedExecutor`. */
export interface Signal {
  side: Side;
  quantity: number;
  /** 0 for market orders. */
  price: number;
  orderType: OrderType;
  orderId: number;
}

/** User-supplied strategy object. All callbacks are optional. */
export interface Strategy {
  /** Symbols this strategy subscribes to. */
  symbols?: ReadonlyArray<Symbol | number>;
  onStart?(): void;
  onStop?(): void;
  onTrade?(ctx: SymbolContext, trade: TradeData, emit: EmitMethods): void;
  onBookUpdate?(ctx: SymbolContext, emit: EmitMethods): void;
  onBar?(ctx: SymbolContext, bar: BarData, emit: EmitMethods): void;
  /** Fires on every fill the strategy's own orders produce
   *  (status PARTIALLY_FILLED or FILLED). */
  onFill?(ctx: SymbolContext, ev: OrderEventData, emit: EmitMethods): void;
  /** Fires on every order-lifecycle status change: NEW / ACCEPTED /
   *  CANCELED / REJECTED / REPLACED / TRIGGERED / TRAILING_UPDATED.
   *  Includes fills too — pick `onFill` if you only care about those. */
  onOrderUpdate?(ctx: SymbolContext, ev: OrderEventData, emit: EmitMethods): void;
}

// ── Extension hooks ──────────────────────────────────────────────────
//
// All hooks are plain JS objects. Pass to `runner.set<Hook>(obj)` /
// `backtestRunner.set<Hook>(obj)`. Pass `null` to detach.

/** Order snapshot delivered to ExecutionListener / Executor callbacks. */
export interface Order {
  id: number;
  clientOrderId: number;
  symbol: number;
  strategyId: number;
  orderTag: number;
  side: Side;
  orderType: OrderType | "tp_market" | "tp_limit" | "iceberg";
  timeInForce: "gtc" | "ioc" | "fok" | "gtd" | "post_only" | "unknown";
  reduceOnly: boolean;
  postOnly: boolean;
  closePosition: boolean;
  price: number;
  quantity: number;
  filledQuantity: number;
  triggerPrice: number;
  trailingOffset: number;
  createdAtNs: number;
  exchangeTsNs: number;
}

/** Returned by `Executor.capabilities()` — what the venue supports. */
export interface ExchangeCapabilities {
  stopMarket?: boolean;
  stopLimit?: boolean;
  takeProfitMarket?: boolean;
  takeProfitLimit?: boolean;
  trailingStop?: boolean;
  iceberg?: boolean;
  oco?: boolean;
  gtc?: boolean;
  ioc?: boolean;
  fok?: boolean;
  gtd?: boolean;
  postOnly?: boolean;
  reduceOnly?: boolean;
  closePosition?: boolean;
}

/** Post-emission observer of every signal. Fires from a Node background
 *  thread for live engine; from the JS thread for sync runner. */
export interface PnLTracker {
  onSignal?(signal: Signal): void;
}

/** Persist every emitted signal (DB, audit log, etc.). */
export interface StorageSink {
  store?(signal: Signal): void;
}

/** Pre-trade gate: return false to drop. Sync only — engine reads the
 *  return value inline, so this can't be used with `threaded: true`. */
export interface RiskManager {
  allow?(signal: Signal): boolean;
}

/** Pre-trade gate; halts trading when `check` returns false. Sync only. */
export interface KillSwitch {
  check?(signal: Signal): boolean;
}

/** Pre-trade gate; rejects signals that fail validation. Sync only. */
export interface OrderValidator {
  validate?(signal: Signal): boolean;
}

/** Receives every market-data event fed into the engine, for custom
 *  recording (CSV, parquet, custom binary). */
export interface MarketDataRecorderHook {
  onTrade?(trade: TradeData): void;
  /** `bids` / `asks` are arrays of `[price, quantity]` pairs. */
  onBookUpdate?(
    symbol: number,
    isSnapshot: boolean,
    bids: ReadonlyArray<readonly [number, number]>,
    asks: ReadonlyArray<readonly [number, number]>,
    timestampNs: number
  ): void;
  onStart?(): void;
  onStop?(): void;
}

/** A binding-supplied event source for `BacktestRunner.runReplaySource`.
 *  `next()` is called repeatedly; return `null` when the stream ends. */
export interface ReplayEvent {
  type: "trade" | "book_snapshot" | "book_delta";
  timestampNs: number;
  // Trade payload (when type === "trade")
  tradeSymbol?: number;
  tradeIsBuy?: boolean;
  tradePrice?: number;
  tradeQuantity?: number;
  // Book payload (when type === "book_snapshot" | "book_delta")
  bookSymbol?: number;
  bids?: ReadonlyArray<readonly [number, number]>;
  asks?: ReadonlyArray<readonly [number, number]>;
}

export interface ReplaySource {
  onStart?(): void;
  onStop?(): void;
  seekTo?(timestampNs: number): boolean;
  next?(): ReplayEvent | null | undefined;
}

/** Replaces the built-in SimulatedExecutor with a binding-supplied one
 *  (real broker, paper-trading bridge, custom simulator). Sync only —
 *  `capabilities()` is queried inline by the engine. */
export interface Executor {
  submit?(order: Order): void;
  cancel?(orderId: number): void;
  cancelAll?(symbol: number): void;
  replace?(oldOrderId: number, newOrder: Order): void;
  submitOco?(order1: Order, order2: Order): void;
  capabilities?(): ExchangeCapabilities;
  onStart?(): void;
  onStop?(): void;
}

/** Observes order lifecycle from SimulatedExecutor / live broker fills. */
export interface ExecutionListener {
  onSubmitted?(order: Order): void;
  onAccepted?(order: Order): void;
  onPartiallyFilled?(order: Order, fillQuantity: number): void;
  onFilled?(order: Order): void;
  onPendingCancel?(order: Order): void;
  onCanceled?(order: Order): void;
  onExpired?(order: Order): void;
  onRejected?(order: Order, reason: string): void;
  onReplaced?(oldOrder: Order, newOrder: Order): void;
  onPendingTrigger?(order: Order): void;
  onTriggered?(order: Order): void;
  onTrailingStopUpdated?(order: Order, newTriggerPrice: number): void;
}

/** Install a Node callable as the global FLOX log sink. Pass null to
 *  detach. Receives `(level: number, msg: string)`; level: 0=info,
 *  1=warn, 2=error. */
export function setLogCallback(
  callback: ((level: number, msg: string) => void) | null
): void;

/** Stats returned by `BacktestRunner.runCsv` / `runOhlcv` and `BacktestResult.stats()`. */
export interface BacktestStats {
  totalTrades: number;
  winningTrades: number;
  losingTrades: number;
  initialCapital: number;
  finalCapital: number;
  netPnl: number;
  totalPnl: number;
  totalFees: number;
  grossProfit: number;
  grossLoss: number;
  maxDrawdown: number;
  maxDrawdownPct: number;
  winRate: number;
  profitFactor: number;
  avgWin: number;
  avgLoss: number;
  sharpe: number;
  sortino: number;
  calmar: number;
  returnPct: number;
}

// ── Symbol & registry ─────────────────────────────────────────────────

/** Result of `SymbolRegistry.addSymbol`. Coerces to its numeric `id`. */
export interface Symbol {
  readonly id: number;
  readonly name: string;
  readonly exchange: string;
  readonly tickSize: number;
  toString(): string;
  valueOf(): number;
}

export class SymbolRegistry {
  constructor();
  addSymbol(exchange: string, name: string, tickSize: number): Symbol;
  symbolCount(): number;
}

// ── Runner ────────────────────────────────────────────────────────────

export class Runner {
  /** `threaded=true` runs callbacks on a background C++ Disruptor thread. */
  constructor(
    registry: SymbolRegistry,
    onSignal: (sig: Signal) => void,
    threaded?: boolean,
  );
  addStrategy(strategy: Strategy): void;
  /** Atomically swap the strategy at `index` for a new one. The old
   *  strategy's `onStop` fires before the swap; the new strategy's
   *  `onStart` fires after. Bus subscriptions, in-flight orders, and
   *  WebSocket / gRPC connections are untouched. Must be invoked on
   *  the V8 thread. */
  replaceStrategy(index: number, strategy: Strategy): void;
  start(): void;
  stop(): void;
  onTrade(
    symbol: Symbol | number,
    price: number,
    qty: number,
    isBuy: boolean,
    timestampNs: number | bigint,
  ): void;
  onBookSnapshot(
    symbol: Symbol | number,
    bidPrices: Float64Array,
    bidQtys: Float64Array,
    askPrices: Float64Array,
    askQtys: Float64Array,
    timestampNs: number | bigint,
  ): void;
  onBar(symbol: Symbol | number, bar: Partial<BarData> & Pick<BarData, "open" | "high" | "low" | "close">): void;

  // ── Hook setters ──
  setPnlTracker(tracker: PnLTracker | null): void;
  setStorageSink(sink: StorageSink | null): void;
  /** Sync only — RiskManager.allow is read inline. Throws if `threaded`. */
  setRiskManager(rm: RiskManager | null): void;
  /** Sync only — KillSwitch.check is read inline. */
  setKillSwitch(ks: KillSwitch | null): void;
  /** Sync only — OrderValidator.validate is read inline. */
  setOrderValidator(ov: OrderValidator | null): void;
  setMarketDataRecorder(recorder: MarketDataRecorderHook | null): void;
  /** Sync only — Executor.capabilities() is read inline. */
  setExecutor(executor: Executor | null): void;

  /** Auto-capture every signal into the given `.floxrun` recorder.
   *  Pass `null` to detach. Sync mode only; throws otherwise.
   *  Order / fill auto-capture is a follow-up — wire those through
   *  the executor's listener bus today. */
  attachTraceRecorder(recorder: TraceRecorder | null): void;
  /** Stamp every recorded signal with this `feed_ts_ns`. The runner
   *  copies it into each `SignalView.feed_ts_ns` slot until the next
   *  call. Useful for pinning replay determinism. */
  setTraceFeedTsNs(feedTsNs: number): void;

  /** Mirror an order event into the attached recorder. No-op when
   *  no recorder is attached. Wire from your executor wrapper after
   *  the corresponding `on_submitted` / `on_canceled` / etc. fires.
   *  `eventKind`: 1=Submit, 2=Cancel, 3=Modify, 4=Ack, 5=Reject,
   *               6=Expire. */
  traceOrderEvent(opts: {
    orderId: number;
    parentSignalId?: number;
    symbolId: number;
    eventKind: number;
    side: 0 | 1;
    orderType?: number;
    price?: number;
    qty?: number;
    flags?: number;
  }): void;

  /** Mirror a fill into the attached recorder. `liquidity`:
   *  0=Unknown, 1=Maker, 2=Taker. */
  traceFill(opts: {
    orderId: number;
    fillId?: number;
    price: number;
    qty: number;
    fee?: number;
    symbolId: number;
    side: 0 | 1;
    liquidity?: number;
  }): void;
}

// ── Backtest ──────────────────────────────────────────────────────────

export class Engine {
  constructor(registry: SymbolRegistry);
  loadCsv(path: string, symbol: string): void;
  loadOhlcv(
    timestamps: Float64Array,
    opens: Float64Array,
    highs: Float64Array,
    lows: Float64Array,
    closes: Float64Array,
    volumes: Float64Array,
    symbol: string,
  ): void;
  resample(intervalSeconds: number): void;
  /** Run a callback strategy or replay a `SignalBuilder`. */
  run(strategyOrSignals: Strategy | SignalBuilder): BacktestStats;
  barCount(): number;
  ts(): Float64Array;
  open(): Float64Array;
  high(): Float64Array;
  low(): Float64Array;
  close(): Float64Array;
  volume(): Float64Array;
  readonly symbols: string[];
}

export class SignalBuilder {
  constructor();
  buy(index: number): void;
  sell(index: number): void;
  limitBuy(index: number, price: number): void;
  limitSell(index: number, price: number): void;
  clear(): void;
  readonly length: number;
}

/** Equity curve from the most recent run, returned by
 *  `BacktestRunner.equityCurve()`. Arrays are aligned: `equity[i]` and
 *  `drawdownPct[i]` correspond to `timestampNs[i]`. */
export interface EquityCurve {
  timestampNs: BigInt64Array;
  equity: Float64Array;
  drawdownPct: Float64Array;
}

/** Closed trades from the most recent run, returned by
 *  `BacktestRunner.trades()`. Arrays are aligned by row index.
 *  `side`: 0 = long (buy entry, sell exit), 1 = short. */
export interface BacktestTrades {
  symbol: Uint32Array;
  side: Uint8Array;
  entryPrice: Float64Array;
  exitPrice: Float64Array;
  quantity: Float64Array;
  pnl: Float64Array;
  fee: Float64Array;
  entryTimeNs: BigInt64Array;
  exitTimeNs: BigInt64Array;
}

/** Walk-forward fold metadata + train/test stats. */
export interface WalkForwardFold {
  foldIndex: number;
  trainStartBar: number;
  trainEndBar: number;
  testStartBar: number;
  testEndBar: number;
  trainStartNs: bigint;
  trainEndNs: bigint;
  testStartNs: bigint;
  testEndNs: bigint;
  trainStats: BacktestStats;
  testStats: BacktestStats;
}

/** Walk-forward configuration. `mode = 'anchored'`: train window
 *  expands from bar 0. `mode = 'sliding'`: fixed-size train window
 *  slides forward. */
export interface WalkForwardConfig {
  mode: 'anchored' | 'sliding';
  trainSize?: number;
  testSize: number;
  step?: number;
  minTrainSize?: number;
}

export class WalkForwardRunner {
  constructor(registry: SymbolRegistry, feeRate: number,
              initialCapital: number, config: WalkForwardConfig);
  /** Factory called twice per fold (train, then test) to build a
   *  fresh strategy object. */
  setStrategyFactory(factory: (foldIndex: number) => Strategy): void;
  runCsv(path: string, symbol: string): WalkForwardFold[];
}

/** Result row from `GridSearch.run()`. */
export interface GridSearchResult {
  index: number;
  params: number[];
  stats: BacktestStats;
}

/** Type-erased grid search over numeric parameter axes. The last axis
 *  varies fastest. */
export class GridSearch {
  constructor();
  addAxis(values: number[]): void;
  setFactory(factory: (params: number[]) => BacktestStats): void;
  total(): number;
  paramsForIndex(index: number): number[];
  run(): GridSearchResult[];
}

export class BacktestRunner {
  constructor(registry: SymbolRegistry, feeRate: number, initialCapital: number);
  setStrategy(strategy: Strategy): void;
  runCsv(path: string, symbol: string): BacktestStats;
  runOhlcv(timestamps: Float64Array, closes: Float64Array, symbol: string): BacktestStats;
  /** Replace the built-in SimulatedExecutor with a binding-supplied one. */
  setExecutor(executor: Executor | null): void;
  /** Attach a listener for order lifecycle events. Multiple listeners
   *  may be attached; each fires for every order event. */
  addExecutionListener(listener: ExecutionListener): void;
  /** Equity curve from the most recent run. Throws FloxError(E_RUN_002)
   *  if no run has completed. */
  equityCurve(): EquityCurve;
  /** Closed trades from the most recent run. Throws FloxError(E_RUN_002)
   *  if no run has completed. */
  trades(): BacktestTrades;
}

export class SimulatedExecutor {
  constructor();
  submitOrder(
    id: number,
    side: Side,
    price: number,
    qty: number,
    type: OrderType,
    symbol: number,
  ): void;
  cancelOrder(orderId: number): void;
  cancelAll(symbol: number): void;
  onBar(symbol: number, closePrice: number): void;
  onTrade(symbol: number, price: number, isBuy: boolean): void;
  advanceClock(timestampNs: number | bigint): void;
  setDefaultSlippage(
    model: SlippageModel | number,
    ticks: number,
    tickSize: number,
    bps: number,
    impactCoeff: number,
  ): void;
  setQueueModel(model: QueueModel | number, depth: number): void;
  readonly fillCount: number;
}

export class BacktestResult {
  constructor(initialCapital: number, feeRate: number);
  recordFill(
    orderId: number,
    symbol: number,
    side: Side,
    price: number,
    qty: number,
    timestampNs: number | bigint,
  ): void;
  ingestExecutor(executor: SimulatedExecutor): void;
  stats(): BacktestStats;
}

// ── Streaming indicators ──────────────────────────────────────────────

interface StreamingSingleInput {
  /** Returns the current value (or null until warmed up). */
  update(value: number): number | null;
  /** Current value (NaN until warmed up). */
  readonly value: number;
  /** True once `update` has been called enough times to produce a value. */
  readonly ready: boolean;
  /** Reset state but keep configuration. */
  reset(): void;
  /** Batch convenience: apply `update` over an array, returning the values. */
  compute(input: Float64Array): Float64Array;
}

export class SMA implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class EMA implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class RMA implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class RSI implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class DEMA implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class TEMA implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class KAMA implements StreamingSingleInput {
  constructor(period: number, fast?: number, slow?: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class Slope implements StreamingSingleInput {
  constructor(length: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class Skewness implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class Kurtosis implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class RollingZScore implements StreamingSingleInput {
  constructor(period: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class ShannonEntropy {
  constructor(period: number, bins: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}
export class AutoCorrelation {
  constructor(window: number, lag: number);
  update(value: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): Float64Array;
}

/** Streaming OHLC indicator: `update(high, low, close)`. */
export class ATR {
  constructor(period: number);
  update(high: number, low: number, close: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(high: Float64Array, low: Float64Array, close: Float64Array): Float64Array;
}
export class CCI {
  constructor(period: number);
  update(high: number, low: number, close: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(high: Float64Array, low: Float64Array, close: Float64Array): Float64Array;
}
export class Stochastic {
  constructor(kPeriod: number, dPeriod?: number);
  update(high: number, low: number, close: number): { k: number | null; d: number | null };
  readonly k: number;
  readonly d: number;
  readonly ready: boolean;
  reset(): void;
  compute(
    high: Float64Array,
    low: Float64Array,
    close: Float64Array,
  ): { k: Float64Array; d: Float64Array };
}
export class ParkinsonVol {
  constructor(period: number);
  update(high: number, low: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(high: Float64Array, low: Float64Array): Float64Array;
}
export class RogersSatchellVol {
  constructor(period: number);
  update(open: number, high: number, low: number, close: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(
    open: Float64Array,
    high: Float64Array,
    low: Float64Array,
    close: Float64Array,
  ): Float64Array;
}
export class Correlation {
  constructor(period: number);
  update(x: number, y: number): number | null;
  readonly value: number;
  readonly ready: boolean;
  reset(): void;
  compute(x: Float64Array, y: Float64Array): Float64Array;
}

/** Multi-output: MACD line/signal/histogram. */
export class MACD {
  constructor(fast?: number, slow?: number, signal?: number);
  update(value: number): { line: number | null; signal: number | null; histogram: number | null };
  readonly line: number;
  readonly signal: number;
  readonly histogram: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): { line: Float64Array; signal: Float64Array; histogram: Float64Array };
}
/** Multi-output: Bollinger upper/middle/lower bands. */
export class Bollinger {
  constructor(period: number, stdDev?: number);
  update(value: number): { upper: number | null; middle: number | null; lower: number | null };
  readonly upper: number;
  readonly middle: number;
  readonly lower: number;
  readonly ready: boolean;
  reset(): void;
  compute(input: Float64Array): { upper: Float64Array; middle: Float64Array; lower: Float64Array };
}

// ── Batch indicator functions ─────────────────────────────────────────

export function sma(input: Float64Array, period: number): Float64Array;
export function ema(input: Float64Array, period: number): Float64Array;
export function rma(input: Float64Array, period: number): Float64Array;
export function rsi(input: Float64Array, period: number): Float64Array;
export function dema(input: Float64Array, period: number): Float64Array;
export function tema(input: Float64Array, period: number): Float64Array;
export function kama(input: Float64Array, period: number, fast?: number, slow?: number): Float64Array;
export function slope(input: Float64Array, length: number): Float64Array;
export function skewness(input: Float64Array, period: number): Float64Array;
export function kurtosis(input: Float64Array, period: number): Float64Array;
export function rolling_zscore(input: Float64Array, period: number): Float64Array;
export function shannon_entropy(input: Float64Array, period: number, bins: number): Float64Array;
export function autocorrelation(input: Float64Array, window: number, lag: number): Float64Array;
/** Rolling Pearson correlation over a moving `period` window. */
export function rollingCorrelation(x: Float64Array, y: Float64Array, period: number): Float64Array;
export function adf(input: Float64Array, lag: number): number;
export function chop(high: Float64Array, low: Float64Array, close: Float64Array, period: number): Float64Array;
export function atr(high: Float64Array, low: Float64Array, close: Float64Array, period: number): Float64Array;
export function cci(high: Float64Array, low: Float64Array, close: Float64Array, period: number): Float64Array;
export function parkinson_vol(high: Float64Array, low: Float64Array, period: number): Float64Array;
export function rogers_satchell_vol(
  open: Float64Array,
  high: Float64Array,
  low: Float64Array,
  close: Float64Array,
  period: number,
): Float64Array;
export function adx(
  high: Float64Array,
  low: Float64Array,
  close: Float64Array,
  period: number,
): { adx: Float64Array; plusDi: Float64Array; minusDi: Float64Array };
export function bollinger(
  input: Float64Array,
  period: number,
  stdDev: number,
): { upper: Float64Array; middle: Float64Array; lower: Float64Array };
export function macd(
  input: Float64Array,
  fast: number,
  slow: number,
  signal: number,
): { line: Float64Array; signal: Float64Array; histogram: Float64Array };
export function stochastic(
  high: Float64Array,
  low: Float64Array,
  close: Float64Array,
  kPeriod: number,
  dPeriod: number,
): { k: Float64Array; d: Float64Array };
export function obv(close: Float64Array, volume: Float64Array): Float64Array;
export function vwap(close: Float64Array, volume: Float64Array, window: number): Float64Array;
export function cvd(
  open: Float64Array,
  high: Float64Array,
  low: Float64Array,
  close: Float64Array,
  volume: Float64Array,
): Float64Array;
/** Returns the list of indicator names (used by the catalog generator). */
export function list_indicators(): string[];

// ── Bar aggregation ───────────────────────────────────────────────────

/** A single aggregated bar emitted by `aggregate*` helpers. */
export interface AggregatedBar {
  startTimeNs: number;
  endTimeNs: number;
  open: number;
  high: number;
  low: number;
  close: number;
  volume: number;
  buyVolume: number;
  tradeCount: number;
}

export function aggregateTimeBars(
  timestamps: Float64Array,
  prices: Float64Array,
  quantities: Float64Array,
  isBuy: Uint8Array,
  intervalSeconds: number,
): AggregatedBar[];
export function aggregateTickBars(
  timestamps: Float64Array,
  prices: Float64Array,
  quantities: Float64Array,
  isBuy: Uint8Array,
  tickCount: number,
): AggregatedBar[];
export function aggregateVolumeBars(
  timestamps: Float64Array,
  prices: Float64Array,
  quantities: Float64Array,
  isBuy: Uint8Array,
  threshold: number,
): AggregatedBar[];
export function aggregateRangeBars(
  timestamps: Float64Array,
  prices: Float64Array,
  quantities: Float64Array,
  isBuy: Uint8Array,
  rangeSize: number,
): AggregatedBar[];
export function aggregateRenkoBars(
  timestamps: Float64Array,
  prices: Float64Array,
  quantities: Float64Array,
  isBuy: Uint8Array,
  brickSize: number,
): AggregatedBar[];
export function aggregateHeikinAshiBars(
  timestamps: Float64Array,
  prices: Float64Array,
  quantities: Float64Array,
  isBuy: Uint8Array,
  intervalSeconds: number,
): AggregatedBar[];

// ── Order books ───────────────────────────────────────────────────────

export class OrderBook {
  constructor(tickSize: number);
  applySnapshot(
    bidPrices: Float64Array,
    bidQtys: Float64Array,
    askPrices: Float64Array,
    askQtys: Float64Array,
  ): void;
  applyDelta(
    bidPrices: Float64Array,
    bidQtys: Float64Array,
    askPrices: Float64Array,
    askQtys: Float64Array,
  ): void;
  bestBid(): number | null;
  bestAsk(): number | null;
  mid(): number | null;
  spread(): number | null;
  getBids(n: number): Array<[number, number]>;
  getAsks(n: number): Array<[number, number]>;
  isCrossed(): boolean;
  clear(): void;
}

export class L3Book {
  constructor();
  /** Returns 0 on success. */
  addOrder(orderId: number, price: number, qty: number, side: Side): number;
  removeOrder(orderId: number): number;
  modifyOrder(orderId: number, newQty: number): number;
  bestBid(): number | null;
  bestAsk(): number | null;
  bidAtPrice(price: number): number;
  askAtPrice(price: number): number;
}

export class CompositeBookMatrix {
  constructor();
  bestBid(symbol: number): { price: number; qty: number } | null;
  bestAsk(symbol: number): { price: number; qty: number } | null;
  hasArbitrage(symbol: number): boolean;
  markStale(exchange: number, symbol: number): void;
  checkStaleness(nowNs: number | bigint, thresholdNs: number | bigint): void;
}

// ── Position / order tracking ─────────────────────────────────────────

export class PositionTracker {
  constructor(mode?: PositionMode);
  onFill(symbol: number, side: Side, price: number, qty: number): void;
  position(symbol: number): number;
  avgEntryPrice(symbol: number): number;
  realizedPnl(symbol: number): number;
  totalRealizedPnl(): number;
}

export class PositionGroupTracker {
  constructor();
  /** Returns position ID. */
  openPosition(orderId: number, symbol: number, side: Side, price: number, qty: number): number;
  closePosition(positionId: number, exitPrice: number): void;
  partialClose(positionId: number, qty: number, exitPrice: number): void;
  netPosition(symbol: number): number;
  realizedPnl(symbol: number): number;
  totalRealizedPnl(): number;
  openCount(symbol: number): number;
  prune(): void;
}

export class OrderTracker {
  constructor();
  onSubmitted(orderId: number, symbol: number, side: Side, price: number, qty: number): boolean;
  onFilled(orderId: number, fillQty: number): boolean;
  onCanceled(orderId: number): boolean;
  isActive(orderId: number): boolean;
  readonly activeCount: number;
  readonly totalCount: number;
  prune(): void;
}

// ── Profiles ──────────────────────────────────────────────────────────

export class VolumeProfile {
  constructor(tickSize: number);
  addTrade(price: number, qty: number, isBuy: boolean): void;
  poc(): number;
  valueAreaHigh(): number;
  valueAreaLow(): number;
  totalVolume(): number;
  clear(): void;
}

export class MarketProfile {
  constructor(tickSize: number, periodMinutes: number, sessionStartNs: number | bigint);
  addTrade(timestampNs: number | bigint, price: number, qty: number, isBuy: boolean): void;
  poc(): number;
  valueAreaHigh(): number;
  valueAreaLow(): number;
  initialBalanceHigh(): number;
  initialBalanceLow(): number;
  isPoorHigh(): boolean;
  isPoorLow(): boolean;
  clear(): void;
}

export class FootprintBar {
  constructor(tickSize: number);
  addTrade(price: number, qty: number, isBuy: boolean): void;
  totalDelta(): number;
  totalVolume(): number;
  readonly numLevels: number;
  clear(): void;
}

// ── Statistics ────────────────────────────────────────────────────────

/**
 * Pearson correlation coefficient over the entire pair of arrays.
 * For a rolling correlation see {@link rollingCorrelation}.
 */
export function correlation(x: Float64Array, y: Float64Array): number;
export function profitFactor(pnl: Float64Array): number;
export function winRate(pnl: Float64Array): number;
export function bootstrapCI(
  data: Float64Array,
  confidence?: number,
  samples?: number,
): { lower: number; median: number; upper: number };
export function permutationTest(
  group1: Float64Array,
  group2: Float64Array,
  samples?: number,
): number;
/**
 * White's reality check (Stationary Bootstrap).
 *
 * Tests whether the best-performing strategy among `numStrategies`
 * candidates is significantly better than zero, after correcting for
 * the multiple-comparison bias from picking the best.
 *
 * `returns` is a flat row-major matrix of EXCESS returns (each
 * strategy's series concatenated): length must be at least
 * `numStrategies * numPeriods`. The caller is responsible for
 * subtracting any benchmark.
 *
 * `avgBlockSize` controls the stationary-bootstrap mean block length;
 * `0` (default) uses `sqrt(numPeriods)`.
 */
export function whitesRealityCheck(
  returns: Float64Array,
  numStrategies: number,
  numPeriods: number,
  numBootstrap?: number,
  avgBlockSize?: number,
): { p_value: number; best_stat: number; best_index: number };
export function barReturns(
  longSignals: Int8Array,
  shortSignals: Int8Array,
  logReturns: Float64Array,
): Float64Array;
export function tradePnl(
  longSignals: Int8Array,
  shortSignals: Int8Array,
  logReturns: Float64Array,
): Float64Array;

// ── Data I/O ──────────────────────────────────────────────────────────

export interface TradeRecord {
  exchangeTsNs: number;
  recvTsNs: number;
  price: number;
  qty: number;
  tradeId: number;
  symbolId: number;
  side: Side;
}

export interface BboRecord {
  exchangeTsNs: number;
  recvTsNs: number;
  seq: number;
  symbolId: number;
  /** 2 = snapshot, 3 = delta. */
  eventType: number;
  bidPrice: number;
  bidQty: number;
  askPrice: number;
  askQty: number;
}

export interface BookLevel {
  price: number;
  qty: number;
}

export interface BookUpdateRecord {
  exchangeTsNs: number;
  recvTsNs: number;
  seq: number;
  symbolId: number;
  eventType: number;
  bids: BookLevel[];
  asks: BookLevel[];
}

export interface DataWriterStats {
  bytesWritten: number;
  eventsWritten: number;
  segmentsCreated: number;
  tradesWritten: number;
}

export interface DataReaderSummary {
  firstEventNs: number;
  lastEventNs: number;
  totalEvents: number;
  segmentCount: number;
  totalBytes: number;
  durationSeconds: number;
}

export interface DataReaderStats {
  filesRead: number;
  eventsRead: number;
  tradesRead: number;
  bookUpdatesRead: number;
  bytesRead: number;
  crcErrors: number;
}

export class DataWriter {
  constructor(outputDir: string, maxSegmentMb?: number, exchangeId?: number);
  writeTrade(
    exchangeTsNs: number | bigint,
    recvTsNs: number | bigint,
    price: number,
    qty: number,
    tradeId: number,
    symbolId: number,
    side: Side,
  ): boolean;
  flush(): void;
  close(): void;
  stats(): DataWriterStats;
}

export class DataReader {
  /** `fromNs` / `toNs` accept either number or bigint; pass bigint for true ns precision. */
  constructor(dataDir: string, fromNs?: number | bigint, toNs?: number | bigint);
  readonly count: number;
  summary(): DataReaderSummary;
  stats(): DataReaderStats;
  readTrades(max?: number): TradeRecord[];
  readTradesFrom(startTsNs: number | bigint, max?: number): TradeRecord[];
  readBBO(max?: number): BboRecord[];
  readBBOFrom(startTsNs: number | bigint, max?: number): BboRecord[];
  readBookUpdates(): BookUpdateRecord[];
  readBookUpdatesFrom(startTsNs: number | bigint): BookUpdateRecord[];
}

export class DataRecorder {
  constructor(outputDir: string, exchangeName?: string, maxSegmentMb?: number);
  addSymbol(
    symbolId: number,
    name: string,
    base?: string,
    quote?: string,
    pricePrecision?: number,
    qtyPrecision?: number,
  ): void;
  start(): void;
  stop(): void;
  flush(): void;
  readonly isRecording: boolean;
}

export interface Partition {
  partitionId: number;
  fromNs: number;
  toNs: number;
  warmupFromNs: number;
  estimatedEvents: number;
  estimatedBytes: number;
}

export class Partitioner {
  constructor(dataDir: string);
  byTime(numPartitions: number, warmupNs: number | bigint): Partition[];
  byDuration(durationNs: number | bigint, warmupNs: number | bigint): Partition[];
  /** `unit`: 0=day, 1=week, 2=month. */
  byCalendar(unit: 0 | 1 | 2, warmupNs: number | bigint): Partition[];
  bySymbol(numPartitions: number): Partition[];
  perSymbol(): Partition[];
  byEventCount(numPartitions: number): Partition[];
}

// ── Segment-level data operations ─────────────────────────────────────

/** Result of `validate(path)` (full single-segment validation). */
export interface SegmentValidationResult {
  valid: boolean;
  headerValid: boolean;
  reportedEventCount: number;
  actualEventCount: number;
  hasIndex: boolean;
  indexValid: boolean;
  tradesFound: number;
  bookUpdatesFound: number;
  crcErrors: number;
  timestampAnomalies: number;
}

/** Result of `validateDataset(dir)`. */
export interface DatasetValidationResult {
  valid: boolean;
  totalSegments: number;
  validSegments: number;
  corruptedSegments: number;
  totalEvents: number;
  totalBytes: number;
  firstTimestamp: number;
  lastTimestamp: number;
}

/** Result of `mergeDir(dir, outputPath)`. */
export interface MergeDirResult {
  success: boolean;
  segmentsMerged: number;
  eventsWritten: number;
  bytesWritten: number;
}

/** Result of `split(inputPath, outputDir, ...)`. */
export interface SplitResult {
  success: boolean;
  segmentsCreated: number;
  eventsWritten: number;
}

/** Result of `exportData(inputPath, outputPath, ...)`. */
export interface ExportResult {
  success: boolean;
  eventsExported: number;
  bytesWritten: number;
}

/** Mode for `split`. `time` = by duration ns, `event_count` = by event count, `size` = by bytes, `symbol` = by symbol id. */
export type SplitMode = "time" | "event_count" | "size" | "symbol";

/** Output format for `exportData`. `binary` (raw segments), `json`, `jsonlines`. */
export type ExportFormat = "binary" | "json" | "jsonlines";

/** Compression codec for `recompress`. `lz4` is the default. */
export type RecompressCodec = "lz4" | "none";

/** Boolean header-only validity check. */
export function validateSegment(path: string): boolean;
/** Full validation: walks every record, checks CRC, indexes, ordering. */
export function validate(path: string): SegmentValidationResult;
/** Recursive dataset validation across all segments under `dir`. */
export function validateDataset(dir: string): DatasetValidationResult;
/** Concatenate one or more input segments into a single output. */
export function mergeSegments(inputPath: string, outputPath: string): boolean;
/** Merge every segment under a directory into one output. */
export function mergeDir(dir: string, outputPath: string): MergeDirResult;
/**
 * Split an input segment by `mode`. `splitArg` interpretation:
 *   - `mode="time"`           → nanoseconds per slice (default 3,600,000,000,000 = 1h)
 *   - `mode="event_count"`    → events per slice
 *   - `mode="size"`           → bytes per slice
 *   - `mode="symbol"`         → ignored (one slice per distinct symbol id)
 *
 * `bufferSize` controls the writer buffer in events (default 1,000,000).
 */
export function split(
  inputPath: string,
  outputDir: string,
  mode?: SplitMode,
  splitArg?: number | bigint,
  bufferSize?: number | bigint,
): SplitResult;
/**
 * Export a segment to JSON / JSON-Lines / re-encoded binary, optionally
 * filtered by `[fromNs, toNs]` (pass 0 for "all").
 */
export function exportData(
  inputPath: string,
  outputPath: string,
  format?: ExportFormat,
  fromNs?: number | bigint,
  toNs?: number | bigint,
): ExportResult;
/** Re-write a segment with a different compression codec. */
export function recompress(inputPath: string, outputPath: string, codec?: RecompressCodec): boolean;
/**
 * Filter a segment to a subset of symbol ids; `symbols` is a `Uint32Array`
 * of numeric ids (NOT names). Returns the number of events written.
 */
export function extractSymbols(inputPath: string, outputPath: string, symbols: Uint32Array): number;
/** Filter a segment to events within `[fromNs, toNs]`. Returns events written. */
export function extractTimeRange(
  inputPath: string,
  outputPath: string,
  fromNs: number | bigint,
  toNs: number | bigint,
): number;
/** Quick summary of a segment (same shape as `DataReader.summary()`). */
export function inspect(path: string): DataReaderSummary;

// ── Indicator graphs ──────────────────────────────────────────────────

/**
 * Node compute callback: receives the graph instance and the current symbol
 * id, returns this bar's value. Use `graph.get(node, sym)` / `graph.close(sym)`
 * etc. inside the callback.
 */
export type IndicatorGraphCompute = (
  graph: IndicatorGraph | StreamingIndicatorGraph,
  symbolId: number,
) => number | Float64Array | number[];

export class IndicatorGraph {
  constructor();
  setBars(
    symbol: number,
    close: Float64Array,
    high: Float64Array,
    low: Float64Array,
    volume: Float64Array,
  ): void;
  /** Returns the node id. `dependencies` are previously-added node ids. */
  addNode(name: string, dependencies: number[], compute: IndicatorGraphCompute): number;
  require(node: number): void;
  get(node: number, symbol: number): Float64Array;
  close(symbol: number): Float64Array;
  high(symbol: number): Float64Array;
  low(symbol: number): Float64Array;
  volume(symbol: number): Float64Array;
  invalidate(node: number): void;
  invalidateAll(): void;
}

export class StreamingIndicatorGraph {
  constructor();
  addNode(name: string, dependencies: number[], compute: IndicatorGraphCompute): number;
  step(symbol: number, close: number, high?: number, low?: number, volume?: number): void;
  current(symbol: number, name: string): number;
  barCount(symbol: number): number;
  reset(symbol: number): void;
  resetAll(): void;
  close(symbol: number): Float64Array;
  high(symbol: number): Float64Array;
  low(symbol: number): Float64Array;
  volume(symbol: number): Float64Array;
}

// ── Forward-looking labels (research-only) ────────────────────────────

export namespace targets {
  /** Future return over `horizon` bars. */
  function future_return(input: Float64Array, horizon: number): Float64Array;
  /** Realized close-to-close volatility over `horizon` bars. */
  function future_ctc_volatility(input: Float64Array, horizon: number): Float64Array;
  /** Linear-fit slope over `horizon` bars. */
  function future_linear_slope(input: Float64Array, horizon: number): Float64Array;
}

// ── Constants ─────────────────────────────────────────────────────────

export const SLIPPAGE_NONE: number;
export const SLIPPAGE_FIXED_TICKS: number;
export const SLIPPAGE_FIXED_BPS: number;
export const SLIPPAGE_VOLUME_IMPACT: number;

export const QUEUE_NONE: number;
export const QUEUE_TOB: number;
export const QUEUE_FULL: number;

export const POSITION_FIFO: 0;
export const POSITION_AVG_COST: 1;

// ── Heatmap renderer ──────────────────────────────────────────────────

export interface HeatmapOptions {
  rowLabels?: ReadonlyArray<string>;
  colLabels?: ReadonlyArray<string>;
  title?: string;
  xAxisName?: string;
  yAxisName?: string;
  metricName?: string;
}

export const report: {
  /** Render a 2D matrix as a self-contained HTML page with an
   *  inline-SVG heatmap. `z` is row-major: z[row][col]. Output is
   *  byte-identical to the Python / Codon binding for the same input
   *  (renderer lives in C++). */
  heatmapHtml(z: ReadonlyArray<ReadonlyArray<number>>,
              opts?: HeatmapOptions): string;
};

// ── Latency models (Phase 1 sampling primitive) ───────────────────────

/** One draw from a latency model. All values are non-negative ns. */
export interface LatencySample {
  feedNs: number;
  orderNs: number;
  fillNs: number;
}

/** Common surface implemented by every concrete latency model. */
export interface LatencyModel {
  feedDelay(): number;
  orderDelay(): number;
  fillDelay(): number;
  sample(): LatencySample;
  /** Re-seed the underlying RNG. No-op for deterministic models. */
  reset(seed?: number): void;
}

export interface ConstantLatencyOptions {
  feedNs?: number;
  orderNs?: number;
  fillNs?: number;
}
export interface GaussianLatencyOptions {
  feedMeanNs?: number;
  feedStddevNs?: number;
  orderMeanNs?: number;
  orderStddevNs?: number;
  fillMeanNs?: number;
  fillStddevNs?: number;
  seed?: number;
}
export interface ExponentialLatencyOptions {
  feedMeanNs?: number;
  orderMeanNs?: number;
  fillMeanNs?: number;
  seed?: number;
}
export interface EmpiricalLatencyOptions {
  feedSamples?: ReadonlyArray<number>;
  orderSamples?: ReadonlyArray<number>;
  fillSamples?: ReadonlyArray<number>;
  seed?: number;
}

/** Returns the same nanoseconds every call. */
export class ConstantLatency implements LatencyModel {
  constructor(opts?: ConstantLatencyOptions);
  feedDelay(): number;
  orderDelay(): number;
  fillDelay(): number;
  sample(): LatencySample;
  reset(seed?: number): void;
}

/** Independent normal samples per component, clamped to non-negative. */
export class GaussianLatency implements LatencyModel {
  constructor(opts?: GaussianLatencyOptions);
  feedDelay(): number;
  orderDelay(): number;
  fillDelay(): number;
  sample(): LatencySample;
  reset(seed?: number): void;
}

/** Exponential per component, parameterised by mean. */
export class ExponentialLatency implements LatencyModel {
  constructor(opts?: ExponentialLatencyOptions);
  feedDelay(): number;
  orderDelay(): number;
  fillDelay(): number;
  sample(): LatencySample;
  reset(seed?: number): void;
}

/** Resample with replacement from observed values. */
export class EmpiricalLatency implements LatencyModel {
  constructor(opts?: EmpiricalLatencyOptions);
  feedDelay(): number;
  orderDelay(): number;
  fillDelay(): number;
  sample(): LatencySample;
  reset(seed?: number): void;
}

// ── Tape diff ─────────────────────────────────────────────────────────

/** One trade-record snapshot used for both sides of a diff. */
export interface TapeDiffTrade {
  exchangeTsNs: number;
  symbolId: number;
  priceRaw: number;
  qtyRaw: number;
  side: number;
}

/** One pair of records that did not match. */
export interface TapeDiffMismatch {
  index: number;
  left: TapeDiffTrade;
  right: TapeDiffTrade;
}

export interface TapeDiffOptions {
  /** Maximum mismatches to record. 0 means no cap. Default 16. */
  maxMismatches?: number;
  /** Allowed symmetric tolerance on exchangeTsNs. Default 0. */
  fieldToleranceNs?: number;
}

export interface TapeDiffResult {
  leftPath: string;
  rightPath: string;
  leftCount: number;
  rightCount: number;
  /** Index of the first mismatch, or null when tapes are equal. */
  firstDivergenceIndex: number | null;
  mismatches: ReadonlyArray<TapeDiffMismatch>;
  equal: boolean;
}

/** Compare two .floxlog directories trade-by-trade. The walk runs in
 *  the C++ engine; result fields match the Python `flox_py.tape.diff_tapes`
 *  output. */
export function tapeDiff(leftPath: string, rightPath: string,
                         opts?: TapeDiffOptions): TapeDiffResult;

// ── Portfolio risk aggregator ─────────────────────────────────────────

/** Cross-strategy risk limits. Pass any subset; missing fields stay
 *  uncapped. */
export interface PortfolioRiskRules {
  maxDrawdownPct?: number;
  maxDailyLoss?: number;
  maxGrossExposure?: number;
  maxConcentrationPct?: number;
}

export interface PortfolioRiskOptions {
  rules?: PortfolioRiskRules;
  initialEquity?: number;
}

/** One row's writable view; pass any subset to `update`. */
export interface StrategyAccountFields {
  realizedPnl?: number;
  unrealizedPnl?: number;
  fees?: number;
  grossExposure?: number;
  netExposure?: number;
  tradeCount?: number;
}

export interface PortfolioBreach {
  rule: string;
  value: number;
  limit: number;
  detail: string;
}

export interface PortfolioSnapshotJs {
  totalDailyPnl: number;
  totalGrossExposure: number;
  currentEquity: number;
  drawdownPct: number;
  killSwitchActive: boolean;
  breaches: ReadonlyArray<PortfolioBreach>;
  accountCount: number;
}

export class PortfolioRiskAggregator {
  constructor(opts?: PortfolioRiskOptions);
  update(name: string, fields: StrategyAccountFields): void;
  remove(name: string): void;
  resetKillSwitch(): void;
  /** Returns `null` when allowed, or a breach describing the rule
   *  hit. Pre-trade gate; does not mutate state. */
  checkOrder(strategy: string, notional: number, side: string): PortfolioBreach | null;
  snapshot(): PortfolioSnapshotJs;
  totalDailyPnl(): number;
  totalGrossExposure(): number;
  currentEquity(): number;
  drawdownPct(): number;
  killSwitchActive(): boolean;
}

// ── Multi-leg order group ────────────────────────────────────────────

export type OrderGroupPolicyName = 'BestEffort' | 'AllOrNothing' | 'OneSided';
export type OrderGroupStateName =
  | 'Pending' | 'Submitted' | 'PartiallyFilled' | 'Filled'
  | 'Cancelled' | 'Reverting' | 'Failed';
export type LegStateName =
  | 'Pending' | 'Submitted' | 'PartiallyFilled' | 'Filled' | 'Cancelled' | 'Failed';

export const OrderGroupPolicy: Readonly<Record<OrderGroupPolicyName, OrderGroupPolicyName>>;
export const OrderGroupState: Readonly<Record<OrderGroupStateName, OrderGroupStateName>>;

export interface OrderGroupOptions {
  parentSignalId?: number;
  policy?: OrderGroupPolicyName;
}

export interface OrderGroupCancelAction {
  kind: 'cancel';
  legIndex: number;
  orderId: number;
}

export interface OrderGroupRevertAction {
  kind: 'revert';
  legIndex: number;
  symbol: number;
  side: 0 | 1;
  qty: number;
}

export type OrderGroupAction = OrderGroupCancelAction | OrderGroupRevertAction;

export class OrderGroup {
  constructor(opts?: OrderGroupOptions);
  addMarketLeg(symbol: number, side: 0 | 1, qty: number): number;
  addLimitLeg(symbol: number, side: 0 | 1, price: number, qty: number): number;
  legCount(): number;
  legState(legIndex: number): LegStateName;
  legFilled(legIndex: number): number;
  legOrderId(legIndex: number): number;
  recordSubmit(legIndex: number, orderId: number): void;
  recordFill(legIndex: number, cumulativeQty: number): void;
  recordCancel(legIndex: number): void;
  recordFailure(legIndex: number): void;
  state(): OrderGroupStateName;
  recommendedActions(): OrderGroupAction[];
}

// ── Bar-close dispatch recorder (cross-binding parity test fixture) ──

export class BarDispatchRecorder {
  constructor();
  /** Register a time-bar timeframe. Returns its slot index (or 8 if full). */
  addTimeIntervalSeconds(seconds: number): number;
  onTrade(symbol: number, price: number, qty: number, tsNs: number): void;
  /** Drain the bus so all bars at the final tied close fire. */
  finalize(): void;
  count(): number;
  /** BarType enum value at index. */
  typeAt(index: number): number;
  /** barTypeParam (nanoseconds for time bars) at index. */
  paramAt(index: number): number;
}

// ── Multi-feed clock ─────────────────────────────────────────────────

export type FeedClockPolicyName = 'WaitForAll' | 'FireOnAny' | 'LeaderFollower';

export const FeedClockPolicy: Readonly<Record<FeedClockPolicyName, FeedClockPolicyName>>;

export interface MultiFeedClockOptions {
  symbols: number[];
  policy?: FeedClockPolicyName;
  timeoutMs?: number;
  leaderSymbol?: number;
  stalenessBudgetMs?: number;
}

export interface FeedClockSnapshot {
  fired: boolean;
  triggeredBy: number;
  /** Symbol id → last-seen exchange-ts in nanoseconds (0 if never). */
  lastTsNs: Record<number, number>;
  /** Symbol id → staleness in nanoseconds at the moment of this tick. */
  stalenessNs: Record<number, number>;
}

export class MultiFeedClock {
  constructor(opts: MultiFeedClockOptions);
  tick(tsNs: number, symbol: number): FeedClockSnapshot;
  reset(): void;
  symbolCount(): number;
}

// ── Execution algorithms (TWAP / VWAP / Iceberg / POV) ────────────────

export interface ExecChildOrder {
  orderId: number;
  timestampNs: number;
  qty: number;
  price: number;
  type: "market" | "limit";
}

interface ExecAlgoCommon {
  targetQty: number;
  side: Side;
  symbol?: number;
  type?: OrderType;
  limitPrice?: number;
}

export interface TWAPOptions extends ExecAlgoCommon {
  durationNs: number;
  sliceCount: number;
  startTimeNs: number;
}

export interface VWAPOptions extends ExecAlgoCommon {
  /** Array of [timestampNs, volume] rows ordered by time. */
  volumeCurve: ReadonlyArray<[number, number]>;
}

export interface IcebergOptions extends ExecAlgoCommon {
  visibleQty: number;
}

export interface POVOptions extends ExecAlgoCommon {
  participationRate: number;
  minSliceQty?: number;
}

interface ExecAlgo {
  /** Drive the state machine forward and return any newly emitted
   *  child orders. The engine clears its pending buffer before
   *  returning so the next `step` only yields fresh entries. */
  step(nowNs: number): ReadonlyArray<ExecChildOrder>;
  /** User reports a fill on a previously emitted child order. */
  reportFill(qty: number): void;
  /** POV-only. Reports observed market volume. No-op for other algos. */
  observeVolume(qty: number): void;
  submittedQty(): number;
  filledQty(): number;
  remainingQty(): number;
  isDone(): boolean;
}

export class TWAPExecutor implements ExecAlgo {
  constructor(opts: TWAPOptions);
  step(nowNs: number): ReadonlyArray<ExecChildOrder>;
  reportFill(qty: number): void;
  observeVolume(qty: number): void;
  submittedQty(): number;
  filledQty(): number;
  remainingQty(): number;
  isDone(): boolean;
}

export class VWAPExecutor implements ExecAlgo {
  constructor(opts: VWAPOptions);
  step(nowNs: number): ReadonlyArray<ExecChildOrder>;
  reportFill(qty: number): void;
  observeVolume(qty: number): void;
  submittedQty(): number;
  filledQty(): number;
  remainingQty(): number;
  isDone(): boolean;
}

export class IcebergExecutor implements ExecAlgo {
  constructor(opts: IcebergOptions);
  step(nowNs: number): ReadonlyArray<ExecChildOrder>;
  reportFill(qty: number): void;
  observeVolume(qty: number): void;
  submittedQty(): number;
  filledQty(): number;
  remainingQty(): number;
  isDone(): boolean;
}

export class POVExecutor implements ExecAlgo {
  constructor(opts: POVOptions);
  step(nowNs: number): ReadonlyArray<ExecChildOrder>;
  reportFill(qty: number): void;
  observeVolume(qty: number): void;
  submittedQty(): number;
  filledQty(): number;
  remainingQty(): number;
  isDone(): boolean;
}

// ── Delta book compression ────────────────────────────────────────────

export interface DeltaBookLevel {
  priceRaw: number;
  qtyRaw: number;
}

export interface DeltaBookEncodeResult {
  /** false when the encoder emits a full anchor snapshot, true for
   *  a delta against the previous state. */
  isDelta: boolean;
  bids: ReadonlyArray<DeltaBookLevel>;
  asks: ReadonlyArray<DeltaBookLevel>;
}

export interface DeltaBookSnapshot {
  bids: ReadonlyArray<DeltaBookLevel>;
  asks: ReadonlyArray<DeltaBookLevel>;
}

export interface DeltaBookEncoderOptions {
  /** Cadence of full-snapshot anchors. 0 means snapshot-only.
   *  Default 100. */
  anchorEvery?: number;
}

/** Encode a stream of L2 snapshots as anchor snapshots plus deltas. */
export class DeltaBookEncoder {
  constructor(opts?: DeltaBookEncoderOptions);
  encode(symbolId: number, bids: ReadonlyArray<DeltaBookLevel>,
         asks: ReadonlyArray<DeltaBookLevel>): DeltaBookEncodeResult;
  reset(symbolId: number): void;
  resetAll(): void;
}

/** Reverse of DeltaBookEncoder. Reconstructs full snapshots from a
 *  stream of (type, bids, asks) events. type=0 snapshot, type=1 delta. */
export class DeltaBookReplayer {
  constructor();
  apply(type: number, symbolId: number,
        bids: ReadonlyArray<DeltaBookLevel>,
        asks: ReadonlyArray<DeltaBookLevel>): DeltaBookSnapshot;
  reset(symbolId: number): void;
  resetAll(): void;
}

// ---------------------------------------------------------------------------
// .floxrun strategy-trace recorder + reader.
// ---------------------------------------------------------------------------

export interface TraceRecorderOptions {
  path: string;
  strategyId?: string;
  strategyHash?: string;
  runStartedNs?: number;
}

export interface TraceTapeRefInput {
  path: string;
  contentHash?: string;
  firstEventNs?: number;
  lastEventNs?: number;
}

export interface TraceSignalInput {
  runTsNs: number;
  feedTsNs?: number;
  signalId?: number;
  flags?: number;
  strengthRaw?: number;
  name?: string;
  symbolIds?: ReadonlyArray<number>;
  payload?: Buffer | string;
}

export interface TraceOrderEventInput {
  runTsNs: number;
  feedTsNs?: number;
  orderId?: number;
  parentSignalId?: number;
  priceRaw?: number;
  qtyRaw?: number;
  symbolId?: number;
  eventKind?: number;
  side?: number;
  orderType?: number;
  flags?: number;
  reason?: string;
}

export interface TraceFillInput {
  runTsNs: number;
  feedTsNs?: number;
  orderId?: number;
  fillId?: number;
  priceRaw?: number;
  qtyRaw?: number;
  feeRaw?: number;
  symbolId?: number;
  side?: number;
  liquidity?: number;
}

export interface TraceSignalRecord {
  runTsNs: number;
  feedTsNs: number;
  signalId: number;
  flags: number;
  strengthRaw: number;
  name: string;
  symbolIds: number[];
  payload: Buffer;
}

export interface TraceOrderEventRecord {
  runTsNs: number;
  feedTsNs: number;
  orderId: number;
  parentSignalId: number;
  priceRaw: number;
  qtyRaw: number;
  symbolId: number;
  eventKind: number;
  side: number;
  orderType: number;
  flags: number;
  reason: string;
}

export interface TraceFillRecord {
  runTsNs: number;
  feedTsNs: number;
  orderId: number;
  fillId: number;
  priceRaw: number;
  qtyRaw: number;
  feeRaw: number;
  symbolId: number;
  side: number;
  liquidity: number;
}

/** Writes signals, order events, and fills into a `.floxrun` directory. */
export class TraceRecorder {
  constructor(opts: TraceRecorderOptions);
  addTapeRef(opts: TraceTapeRefInput): void;
  setRunEndedNs(ns: number): void;
  writeSignal(opts: TraceSignalInput): void;
  writeOrderEvent(opts: TraceOrderEventInput): void;
  writeFill(opts: TraceFillInput): void;
  close(): void;
}

/** Reads back a `.floxrun` directory written by TraceRecorder. */
export class TraceReader {
  constructor(path: string);
  strategyId(): string;
  strategyHash(): string;
  runStartedNs(): number;
  runEndedNs(): number;
  tapeRefs(): Array<{ path: string }>;
  readAllSignals(): TraceSignalRecord[];
  readAllOrderEvents(): TraceOrderEventRecord[];
  readAllFills(): TraceFillRecord[];
  close(): void;
}

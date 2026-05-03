// node/test/test_types.ts — TypeScript smoke test for index.d.ts.
//
// Touches every exported name so `tsc --noEmit` flags any signature
// drift between the C++ binding and the .d.ts. Companion to
// `scripts/check_dts_exports.py` which catches name-level drift.
//
// Not run at runtime — type-check only.

import * as flox from "..";
import {
  // Classes
  ATR,
  AutoCorrelation,
  BacktestResult,
  BacktestRunner,
  Bollinger,
  CCI,
  CompositeBookMatrix,
  Correlation,
  DEMA,
  DataReader,
  DataRecorder,
  DataWriter,
  EMA,
  Engine,
  FootprintBar,
  IndicatorGraph,
  KAMA,
  Kurtosis,
  L3Book,
  MACD,
  MarketProfile,
  OrderBook,
  OrderTracker,
  ParkinsonVol,
  Partitioner,
  PositionGroupTracker,
  PositionTracker,
  RMA,
  RSI,
  RogersSatchellVol,
  RollingZScore,
  Runner,
  SMA,
  ShannonEntropy,
  SignalBuilder,
  SimulatedExecutor,
  Skewness,
  Slope,
  Stochastic,
  StreamingIndicatorGraph,
  SymbolRegistry,
  TEMA,
  VolumeProfile,
  // Constants
  POSITION_AVG_COST,
  POSITION_FIFO,
  QUEUE_FULL,
  QUEUE_NONE,
  QUEUE_TOB,
  SLIPPAGE_FIXED_BPS,
  SLIPPAGE_FIXED_TICKS,
  SLIPPAGE_NONE,
  SLIPPAGE_VOLUME_IMPACT,
  // Submodule
  targets,
  // Types
  AggregatedBar,
  BarData,
  BboRecord,
  BookUpdateRecord,
  EmitMethods,
  Partition,
  Signal,
  Strategy,
  SymbolContext,
  TradeData,
  TradeRecord,
} from "..";

const arr = new Float64Array([1, 2, 3, 4, 5]);
const ohlcArr = { open: arr, high: arr, low: arr, close: arr, vol: arr };
const ib = new Uint8Array([1, 0, 1, 0, 1]);

// ── Strategy / Runner ──────────────────────────────────────────────────
const registry = new SymbolRegistry();
const sym = registry.addSymbol("binance", "BTCUSDT", 0.01);
const _symId: number = sym.id;
const _symName: string = sym.name;
const _symCount: number = registry.symbolCount();

const strategy: Strategy = {
  symbols: [sym, 1],
  onStart() {},
  onStop() {},
  onTrade(ctx: SymbolContext, trade: TradeData, emit: EmitMethods) {
    void ctx.position;
    void trade.price;
    void trade.timestampNs;
    emit.marketBuy(0.1);
    emit.marketSell(0.1);
    emit.limitBuy(100, 0.1);
    emit.limitSell(100, 0.1);
    emit.cancel(1);
    emit.closePosition();
  },
  onBar(_ctx, bar: BarData, _emit) {
    void bar.open;
    void bar.startTimeNs;
    void bar.barType;
  },
  onBookUpdate(_ctx, _emit) {},
};

const runner = new Runner(registry, (sig: Signal) => {
  void sig.side;
  void sig.orderType;
  void sig.orderId;
});
runner.addStrategy(strategy);
runner.start();
runner.onTrade(sym, 100, 1, true, 0n);
runner.onTrade(1, 100, 1, false, 0);
runner.onBookSnapshot(sym, arr, arr, arr, arr, 0n);
runner.onBar(sym, { open: 1, high: 2, low: 0.5, close: 1.5 });
runner.stop();

const threadedRunner = new Runner(registry, (_s) => {}, true);
threadedRunner.start();
threadedRunner.stop();

// ── Engine + SignalBuilder ─────────────────────────────────────────────
const engine = new Engine(registry);
engine.loadOhlcv(arr, arr, arr, arr, arr, arr, "BTCUSDT");
engine.resample(60);
const _bc: number = engine.barCount();
const _ts: Float64Array = engine.ts();
const _opens: Float64Array = engine.open();
const _highs: Float64Array = engine.high();
const _lows: Float64Array = engine.low();
const _closes: Float64Array = engine.close();
const _vols: Float64Array = engine.volume();
const _syms: string[] = engine.symbols;

const signals = new SignalBuilder();
signals.buy(0);
signals.sell(1);
signals.limitBuy(2, 100);
signals.limitSell(3, 100);
const _sigLen: number = signals.length;
signals.clear();

const stats = engine.run(signals);
const _net: number = stats.netPnl;
const _sharpe: number = stats.sharpe;

// ── Backtest classes ───────────────────────────────────────────────────
const bt = new BacktestRunner(registry, 0.0001, 100000);
bt.setStrategy(strategy);
const _btStats = bt.runOhlcv(arr, arr, "BTCUSDT");
void _btStats.totalTrades;

const exec = new SimulatedExecutor();
exec.submitOrder(1, "buy", 100, 0.1, "limit", 1);
exec.cancelOrder(1);
exec.cancelAll(1);
exec.onBar(1, 100);
exec.onTrade(1, 100, true);
exec.advanceClock(1n);
exec.setDefaultSlippage("fixed_bps", 0, 0, 2.0, 0);
exec.setDefaultSlippage(SLIPPAGE_FIXED_BPS, 0, 0, 2.0, 0);
exec.setQueueModel("tob", 1);
exec.setQueueModel(QUEUE_TOB, 1);
const _fc: number = exec.fillCount;

const result = new BacktestResult(100000, 0.0001);
result.recordFill(1, 1, "buy", 100, 0.1, 0n);
result.ingestExecutor(exec);
const _resStats = result.stats();
void _resStats.profitFactor;

// ── Streaming indicators ───────────────────────────────────────────────
function exerciseSingle<T extends { update: (v: number) => number | null; value: number; ready: boolean; reset: () => void; compute: (a: Float64Array) => Float64Array }>(ind: T) {
  const v = ind.update(1);
  void (v === null ? null : v + 1);
  void ind.value;
  void ind.ready;
  ind.reset();
  void ind.compute(arr);
}

exerciseSingle(new SMA(3));
exerciseSingle(new EMA(3));
exerciseSingle(new RMA(3));
exerciseSingle(new RSI(14));
exerciseSingle(new DEMA(3));
exerciseSingle(new TEMA(3));
exerciseSingle(new KAMA(10, 2, 30));
exerciseSingle(new Slope(5));
exerciseSingle(new Skewness(20));
exerciseSingle(new Kurtosis(20));
exerciseSingle(new RollingZScore(20));

const se = new ShannonEntropy(20, 8);
void se.update(1);
void se.compute(arr);
const ac = new AutoCorrelation(20, 1);
void ac.update(1);

const atrInd = new ATR(14);
void atrInd.update(1, 0.5, 0.8);
void atrInd.compute(arr, arr, arr);

const cciInd = new CCI(14);
void cciInd.update(1, 0.5, 0.8);

const stoch = new Stochastic(14, 3);
const stochOut = stoch.update(1, 0.5, 0.8);
void (stochOut.k === null ? 0 : stochOut.k);
const stochBatch = stoch.compute(arr, arr, arr);
void stochBatch.k;
void stochBatch.d;

const pv = new ParkinsonVol(14);
void pv.update(1, 0.5);
const rsv = new RogersSatchellVol(14);
void rsv.update(1, 1.2, 0.8, 1);
const corrI = new Correlation(20);
void corrI.update(1, 2);

const macdInd = new MACD(12, 26, 9);
const macdOut = macdInd.update(1);
void (macdOut.line === null ? 0 : macdOut.line);
const macdBatch = macdInd.compute(arr);
void macdBatch.line;
void macdBatch.signal;
void macdBatch.histogram;

const bb = new Bollinger(20, 2);
const bbOut = bb.update(1);
void (bbOut.upper === null ? 0 : bbOut.upper);
const bbBatch = bb.compute(arr);
void bbBatch.upper;
void bbBatch.middle;
void bbBatch.lower;

// ── Batch indicator functions ──────────────────────────────────────────
void flox.sma(arr, 3);
void flox.ema(arr, 3);
void flox.rma(arr, 3);
void flox.rsi(arr, 14);
void flox.dema(arr, 3);
void flox.tema(arr, 3);
void flox.kama(arr, 10, 2, 30);
void flox.slope(arr, 5);
void flox.skewness(arr, 20);
void flox.kurtosis(arr, 20);
void flox.rolling_zscore(arr, 20);
void flox.shannon_entropy(arr, 20, 8);
void flox.autocorrelation(arr, 20, 1);
const adfV: number = flox.adf(arr, 1);
void adfV;
void flox.chop(arr, arr, arr, 14);
void flox.atr(arr, arr, arr, 14);
void flox.cci(arr, arr, arr, 14);
void flox.parkinson_vol(arr, arr, 14);
void flox.rogers_satchell_vol(arr, arr, arr, arr, 14);
const adxOut = flox.adx(arr, arr, arr, 14);
void adxOut.plusDi;
void adxOut.minusDi;
const bbBatch2 = flox.bollinger(arr, 20, 2);
void bbBatch2.middle;
const macdBatch2 = flox.macd(arr, 12, 26, 9);
void macdBatch2.histogram;
const stochBatch2 = flox.stochastic(arr, arr, arr, 14, 3);
void stochBatch2.k;
void flox.obv(arr, arr);
void flox.vwap(arr, arr, 20);
void flox.cvd(arr, arr, arr, arr, arr);
const indNames: string[] = flox.list_indicators();
void indNames;

// ── Aggregators ───────────────────────────────────────────────────────
const bars: AggregatedBar[] = flox.aggregateTimeBars(arr, arr, arr, ib, 60);
void bars[0]?.open;
void flox.aggregateTickBars(arr, arr, arr, ib, 100);
void flox.aggregateVolumeBars(arr, arr, arr, ib, 1000);
void flox.aggregateRangeBars(arr, arr, arr, ib, 1.0);
void flox.aggregateRenkoBars(arr, arr, arr, ib, 1.0);
void flox.aggregateHeikinAshiBars(arr, arr, arr, ib, 60);

// ── Books ─────────────────────────────────────────────────────────────
const ob = new OrderBook(0.01);
ob.applySnapshot(arr, arr, arr, arr);
ob.applyDelta(arr, arr, arr, arr);
const _bid: number | null = ob.bestBid();
const _ask: number | null = ob.bestAsk();
void ob.mid();
void ob.spread();
void ob.getBids(5);
void ob.getAsks(5);
void ob.isCrossed();
ob.clear();

const l3 = new L3Book();
void l3.addOrder(1, 100, 1, "buy");
void l3.removeOrder(1);
void l3.modifyOrder(1, 2);
void l3.bidAtPrice(100);
void l3.askAtPrice(100);

const cbm = new CompositeBookMatrix();
const bidQ = cbm.bestBid(1);
if (bidQ) {
  void bidQ.price;
  void bidQ.qty;
}
void cbm.bestAsk(1);
void cbm.hasArbitrage(1);
cbm.markStale(1, 1);
cbm.checkStaleness(0n, 1n);

// ── Position tracking ─────────────────────────────────────────────────
const pt = new PositionTracker(POSITION_FIFO);
pt.onFill(1, "buy", 100, 0.1);
void pt.position(1);
void pt.avgEntryPrice(1);
void pt.realizedPnl(1);
void pt.totalRealizedPnl();
const ptAvg = new PositionTracker(POSITION_AVG_COST);
void ptAvg.position(1);

const pgt = new PositionGroupTracker();
const posId = pgt.openPosition(1, 1, "buy", 100, 0.1);
pgt.closePosition(posId, 105);
pgt.partialClose(posId, 0.05, 105);
void pgt.netPosition(1);
void pgt.openCount(1);
pgt.prune();

const ot = new OrderTracker();
void ot.onSubmitted(1, 1, "buy", 100, 0.1);
void ot.onFilled(1, 0.1);
void ot.onCanceled(1);
void ot.isActive(1);
const _ac: number = ot.activeCount;
const _tc: number = ot.totalCount;
ot.prune();

// ── Profiles ──────────────────────────────────────────────────────────
const vp = new VolumeProfile(0.01);
vp.addTrade(100, 0.1, true);
void vp.poc();
void vp.valueAreaHigh();
void vp.valueAreaLow();
void vp.totalVolume();
vp.clear();

const mp = new MarketProfile(0.01, 30, 0n);
mp.addTrade(0n, 100, 0.1, true);
void mp.poc();
void mp.initialBalanceHigh();
void mp.initialBalanceLow();
void mp.isPoorHigh();
void mp.isPoorLow();
mp.clear();

const fb = new FootprintBar(0.01);
fb.addTrade(100, 0.1, true);
void fb.totalDelta();
void fb.totalVolume();
const _nl: number = fb.numLevels;
fb.clear();

// ── Stats ─────────────────────────────────────────────────────────────
const _corr: number = flox.correlation(arr, arr);
void _corr;
void flox.profitFactor(arr);
void flox.winRate(arr);
const ci = flox.bootstrapCI(arr, 0.95, 1000);
void ci.lower;
void ci.median;
void ci.upper;
const sigInt = new Int8Array([1, 0, -1, 0, 1]);
void flox.permutationTest(arr, arr, 1000);
void flox.barReturns(sigInt, sigInt, arr);
void flox.tradePnl(sigInt, sigInt, arr);

// ── Data I/O ─────────────────────────────────────────────────────────
const dw = new DataWriter("/tmp/flox-test", 64, 1);
void dw.writeTrade(0n, 0n, 100, 0.1, 1, 1, "buy");
dw.flush();
const dwStats = dw.stats();
void dwStats.bytesWritten;
void dwStats.eventsWritten;
dw.close();

const dr = new DataReader("/tmp/flox-test", 0n, 1n);
const _drCount: number = dr.count;
const summary = dr.summary();
void summary.totalEvents;
void summary.durationSeconds;
const drStats = dr.stats();
void drStats.tradesRead;
const trades: TradeRecord[] = dr.readTrades(100);
void trades[0]?.price;
void dr.readTradesFrom(0n, 100);
const bbo: BboRecord[] = dr.readBBO();
void bbo[0]?.bidPrice;
void dr.readBBOFrom(0n);
const bookU: BookUpdateRecord[] = dr.readBookUpdates();
void bookU[0]?.bids;
void dr.readBookUpdatesFrom(0n);

const recorder = new DataRecorder("/tmp/flox-test", "binance", 64);
recorder.addSymbol(1, "BTCUSDT", "BTC", "USDT", 2, 8);
recorder.start();
recorder.flush();
const _recOn: boolean = recorder.isRecording;
recorder.stop();

const part = new Partitioner("/tmp/flox-test");
const parts: Partition[] = part.byTime(4, 0n);
void parts[0]?.fromNs;
void part.byDuration(1_000_000n, 0n);
void part.byCalendar(0, 0n);
void part.bySymbol(2);
void part.perSymbol();
void part.byEventCount(4);

// ── Segment ops ──────────────────────────────────────────────────────
const _vsOk: boolean = flox.validateSegment("/tmp/x");
void _vsOk;
const valRes = flox.validate("/tmp/x");
void valRes.headerValid;
void valRes.tradesFound;
const dsRes = flox.validateDataset("/tmp/x");
void dsRes.totalSegments;
void dsRes.firstTimestamp;
const _msOk: boolean = flox.mergeSegments("/tmp/a", "/tmp/b");
void _msOk;
const mdRes = flox.mergeDir("/tmp/a", "/tmp/b");
void mdRes.segmentsMerged;
const splitRes = flox.split("/tmp/a", "/tmp/b", "time", 3_600_000_000_000n, 1_000_000);
void splitRes.segmentsCreated;
const exRes = flox.exportData("/tmp/a", "/tmp/b", "json", 0n, 0n);
void exRes.eventsExported;
const _rcOk: boolean = flox.recompress("/tmp/a", "/tmp/b", "lz4");
void _rcOk;
const _esCount: number = flox.extractSymbols(
  "/tmp/a",
  "/tmp/b",
  new Uint32Array([1, 2, 3]),
);
void _esCount;
const _etCount: number = flox.extractTimeRange("/tmp/a", "/tmp/b", 0n, 1n);
void _etCount;
const insp = flox.inspect("/tmp/x");
void insp.totalEvents;
void insp.durationSeconds;
void flox.rollingCorrelation(arr, arr, 20);

// ── Indicator graphs ──────────────────────────────────────────────────
const ig = new IndicatorGraph();
ig.setBars(1, arr, arr, arr, arr);
const nodeId = ig.addNode("emaClose", [], (g, sym) => {
  const closes = g.close(sym);
  return closes;
});
ig.require(nodeId);
void ig.get(nodeId, 1);
void ig.close(1);
void ig.high(1);
void ig.low(1);
void ig.volume(1);
ig.invalidate(nodeId);
ig.invalidateAll();

const sg = new StreamingIndicatorGraph();
const sNodeId = sg.addNode("triple", [], (_g, _s) => 0);
void sNodeId;
sg.step(1, 100);
sg.step(1, 100, 100, 100, 1);
void sg.current(1, "triple");
void sg.barCount(1);
sg.reset(1);
sg.resetAll();
void sg.close(1);

// ── targets submodule ────────────────────────────────────────────────
const fr: Float64Array = targets.future_return(arr, 5);
void fr;
void targets.future_ctc_volatility(arr, 5);
void targets.future_linear_slope(arr, 5);

// ── Constants exist ──────────────────────────────────────────────────
void SLIPPAGE_NONE;
void SLIPPAGE_FIXED_BPS;
void SLIPPAGE_FIXED_TICKS;
void SLIPPAGE_VOLUME_IMPACT;
void QUEUE_NONE;
void QUEUE_TOB;
void QUEUE_FULL;
void POSITION_FIFO;
void POSITION_AVG_COST;

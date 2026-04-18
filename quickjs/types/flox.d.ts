interface BookSnapshot {
    readonly bidPrice: number;
    readonly askPrice: number;
    readonly midPrice: number;
    readonly spread: number;
}

interface SymbolContext {
    readonly symbolId: number;
    readonly symbol: string;
    readonly position: number;
    readonly avgEntryPrice: number;
    readonly lastTradePrice: number;
    readonly lastUpdateNs: number;
    readonly book: BookSnapshot;
}

interface TradeData {
    readonly symbolId: number;
    readonly symbol: string;
    readonly price: number;
    readonly qty: number;
    readonly side: "buy" | "sell";
    readonly isBuy: boolean;
    readonly timestampNs: number;
}

interface BookData {
    readonly symbolId: number;
    readonly symbol: string;
    readonly timestampNs: number;
    readonly snapshot: BookSnapshot;
}

interface MarketOrderOpts {
    symbol?: string;
    qty: number;
}

interface LimitOrderOpts {
    symbol?: string;
    price: number;
    qty: number;
    tif?: "GTC" | "IOC" | "FOK" | "POST_ONLY";
}

interface StopMarketOpts {
    symbol?: string;
    side: "buy" | "sell";
    trigger: number;
    qty: number;
}

interface StopLimitOpts {
    symbol?: string;
    side: "buy" | "sell";
    trigger: number;
    price: number;
    qty: number;
}

interface TakeProfitMarketOpts {
    symbol?: string;
    side: "buy" | "sell";
    trigger: number;
    qty: number;
}

interface TakeProfitLimitOpts {
    symbol?: string;
    side: "buy" | "sell";
    trigger: number;
    price: number;
    qty: number;
}

interface TrailingStopOpts {
    symbol?: string;
    side: "buy" | "sell";
    offset: number;
    qty: number;
}

interface TrailingStopPercentOpts {
    symbol?: string;
    side: "buy" | "sell";
    callbackBps: number;
    qty: number;
}

interface ModifyOpts {
    price?: number;
    qty?: number;
}

declare const enum OrderStatus {
    NEW = 0,
    SUBMITTED = 1,
    ACCEPTED = 2,
    PARTIALLY_FILLED = 3,
    FILLED = 4,
    PENDING_CANCEL = 5,
    CANCELED = 6,
    EXPIRED = 7,
    REJECTED = 8,
    REPLACED = 9,
    PENDING_TRIGGER = 10,
    TRIGGERED = 11,
    TRAILING_UPDATED = 12,
}

interface StrategyConfig {
    exchange?: string;
    symbols: string[];
}

declare class Strategy {
    constructor(config: StrategyConfig);

    onTrade(ctx: SymbolContext, trade: TradeData): void;
    onBookUpdate(ctx: SymbolContext, book: BookData): void;
    onStart(): void;
    onStop(): void;

    marketBuy(opts: MarketOrderOpts): number;
    marketSell(opts: MarketOrderOpts): number;
    limitBuy(opts: LimitOrderOpts): number;
    limitSell(opts: LimitOrderOpts): number;
    stopMarket(opts: StopMarketOpts): number;
    stopLimit(opts: StopLimitOpts): number;
    takeProfitMarket(opts: TakeProfitMarketOpts): number;
    takeProfitLimit(opts: TakeProfitLimitOpts): number;
    trailingStop(opts: TrailingStopOpts): number;
    trailingStopPercent(opts: TrailingStopPercentOpts): number;
    cancel(orderId: number): void;
    cancelAll(symbol?: string): void;
    modify(orderId: number, opts: ModifyOpts): void;
    closePosition(symbol?: string): number;

    position(symbol?: string): number;
    lastPrice(symbol?: string): number;
    bestBid(symbol?: string): number;
    bestAsk(symbol?: string): number;
    midPrice(symbol?: string): number;
    orderStatus(orderId: number): OrderStatus | -1;

    readonly primarySymbol: string;
    readonly hasPosition: boolean;
    readonly symbols: string[];
}

// ============================================================
// Indicators — all 18, unified per-tick + batch API
// ============================================================

// --- Moving Averages ---

declare class SMA {
    constructor(period: number);
    update(value: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(data: number[], period: number): number[];
}

declare class EMA {
    constructor(period: number);
    update(value: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(data: number[], period: number): number[];
}

declare class RMA {
    constructor(period: number);
    update(value: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(data: number[], period: number): number[];
}

declare class DEMA {
    constructor(period: number);
    update(value: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(data: number[], period: number): number[];
}

declare class TEMA {
    constructor(period: number);
    update(value: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(data: number[], period: number): number[];
}

declare class KAMA {
    constructor(period: number, fast?: number, slow?: number);
    update(value: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(data: number[], period: number, fast?: number, slow?: number): number[];
}

// --- Oscillators ---

declare class RSI {
    constructor(period: number);
    update(value: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(data: number[], period: number): number[];
}

declare class Stochastic {
    constructor(kPeriod?: number, dPeriod?: number);
    update(high: number, low: number, close: number): number;
    readonly k: number;
    readonly d: number;
    readonly value: number;
    readonly ready: boolean;
    static compute(high: number[], low: number[], close: number[], kPeriod?: number, dPeriod?: number): { k: number[]; d: number[] };
}

declare class CCI {
    constructor(period?: number);
    update(high: number, low: number, close: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(high: number[], low: number[], close: number[], period?: number): number[];
}

declare class CHOP {
    constructor(period: number);
    update(high: number, low: number, close: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(high: number[], low: number[], close: number[], period: number): number[];
}

// --- Trend ---

declare class ATR {
    constructor(period: number);
    update(high: number, low: number, close: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(high: number[], low: number[], close: number[], period: number): number[];
}

declare class ADX {
    constructor(period: number);
    update(high: number, low: number, close: number): number;
    readonly adx: number;
    readonly plusDi: number;
    readonly minusDi: number;
    readonly value: number;
    readonly ready: boolean;
    static compute(high: number[], low: number[], close: number[], period: number): { adx: number[]; plusDi: number[]; minusDi: number[] };
}

declare class Slope {
    constructor(length: number);
    update(value: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(data: number[], length: number): number[];
}

declare class MACD {
    constructor(fastPeriod?: number, slowPeriod?: number, signalPeriod?: number);
    update(value: number): number;
    readonly line: number;
    readonly signal: number;
    readonly histogram: number;
    readonly value: number;
    readonly ready: boolean;
    static compute(data: number[], fast?: number, slow?: number, signal?: number): {
        line: number[];
        signal: number[];
        histogram: number[];
    };
}

declare class Bollinger {
    constructor(period?: number, multiplier?: number);
    update(value: number): number;
    readonly upper: number;
    readonly middle: number;
    readonly lower: number;
    readonly value: number;
    readonly ready: boolean;
    static compute(data: number[], period?: number, multiplier?: number): {
        upper: number[];
        middle: number[];
        lower: number[];
    };
}

// --- Volume ---

declare class OBV {
    constructor();
    update(close: number, volume: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(close: number[], volume: number[]): number[];
}

declare class VWAP {
    constructor(window: number);
    update(close: number, volume: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(close: number[], volume: number[], window: number): number[];
}

declare class CVD {
    constructor();
    update(open: number, high: number, low: number, close: number, volume: number): number;
    readonly value: number;
    readonly ready: boolean;
    static compute(open: number[], high: number[], low: number[], close: number[], volume: number[]): number[];
}

declare const flox: {
    register(strategy: Strategy): void;
    correlation(x: number[], y: number[]): number;
    profitFactor(pnl: number[]): number;
    winRate(pnl: number[]): number;
    bootstrapCI(data: number[], confidence?: number, samples?: number): { lower: number; median: number; upper: number };
    permutationTest(group1: number[], group2: number[], numPermutations?: number): number;
    validateSegment(path: string): boolean;
    mergeSegments(inputDir: string, outputPath: string): boolean;
};

// ============================================================
// Infrastructure classes
// ============================================================

declare class OrderBook {
    constructor(tickSize?: number);
    destroy(): void;
    applySnapshot(bidPrices: number[], bidQtys: number[], askPrices: number[], askQtys: number[]): void;
    applyDelta(bidPrices: number[], bidQtys: number[], askPrices: number[], askQtys: number[]): void;
    bestBid(): number | null;
    bestAsk(): number | null;
    mid(): number | null;
    spread(): number | null;
    getBids(maxLevels?: number): number[][];
    getAsks(maxLevels?: number): number[][];
    isCrossed(): boolean;
    clear(): void;
}

declare class L3Book {
    constructor();
    destroy(): void;
    addOrder(orderId: number, price: number, qty: number, side: "buy" | "sell"): number;
    removeOrder(orderId: number): number;
    modifyOrder(orderId: number, newQty: number): number;
    bestBid(): number | null;
    bestAsk(): number | null;
}

declare type SlippageModelName = "none" | "fixed_ticks" | "fixed_bps" | "volume_impact";
declare type QueueModelName = "none" | "tob" | "full";

declare class SimulatedExecutor {
    constructor();
    destroy(): void;
    submitOrder(id: number, side: "buy" | "sell", price: number, qty: number, type?: number, symbol?: number): void;
    onBar(symbol: number, closePrice: number): void;
    onTrade(symbol: number, price: number, isBuy: boolean): void;
    onTradeQty(symbol: number, price: number, quantity: number, isBuy: boolean): void;
    onBestLevels(symbol: number, bidPrice: number, bidQty: number, askPrice: number, askQty: number): void;
    advanceClock(timestampNs: number): void;
    setDefaultSlippage(model: SlippageModelName, ticks?: number, tickSize?: number, bps?: number, impactCoeff?: number): void;
    setSymbolSlippage(symbol: number, model: SlippageModelName, ticks?: number, tickSize?: number, bps?: number, impactCoeff?: number): void;
    setQueueModel(model: QueueModelName, depth?: number): void;
    readonly fillCount: number;
}

declare interface BacktestStats {
    totalTrades: number;
    winningTrades: number;
    losingTrades: number;
    maxConsecutiveWins: number;
    maxConsecutiveLosses: number;
    initialCapital: number;
    finalCapital: number;
    totalPnl: number;
    totalFees: number;
    netPnl: number;
    grossProfit: number;
    grossLoss: number;
    maxDrawdown: number;
    maxDrawdownPct: number;
    winRate: number;
    profitFactor: number;
    avgWin: number;
    avgLoss: number;
    avgWinLossRatio: number;
    avgTradeDurationNs: number;
    medianTradeDurationNs: number;
    maxTradeDurationNs: number;
    sharpeRatio: number;
    sortinoRatio: number;
    calmarRatio: number;
    timeWeightedReturn: number;
    returnPct: number;
    startTimeNs: number;
    endTimeNs: number;
}

declare interface EquityPoint {
    timestampNs: number;
    equity: number;
    drawdownPct: number;
}

declare class BacktestResult {
    constructor(initialCapital?: number, feeRate?: number, usePercentageFee?: boolean,
                fixedFeePerTrade?: number, riskFreeRate?: number, annualizationFactor?: number);
    destroy(): void;
    recordFill(orderId: number, symbol: number, side: "buy" | "sell", price: number,
               quantity: number, timestampNs: number): void;
    ingestExecutor(executor: SimulatedExecutor): void;
    stats(): BacktestStats;
    equityCurve(): EquityPoint[];
    writeEquityCurveCsv(path: string): boolean;
}

declare class PositionTracker {
    constructor(costBasis?: number);
    destroy(): void;
    onFill(symbol: number, side: "buy" | "sell", price: number, qty: number): void;
    position(symbol: number): number;
    avgEntryPrice(symbol: number): number;
    realizedPnl(symbol: number): number;
    totalRealizedPnl(): number;
}

declare class PositionGroupTracker {
    constructor();
    destroy(): void;
    openPosition(orderId: number, symbol: number, side: "buy" | "sell", price: number, qty: number): number;
    closePosition(positionId: number, exitPrice: number): void;
    netPosition(symbol: number): number;
    realizedPnl(symbol: number): number;
    totalRealizedPnl(): number;
    openPositionCount(symbol: number): number;
    prune(): void;
}

declare class OrderTracker {
    constructor();
    destroy(): void;
    onSubmitted(orderId: number, symbol: number, side: "buy" | "sell", price: number, qty: number): boolean;
    onFilled(orderId: number, fillQty: number): boolean;
    onCanceled(orderId: number): boolean;
    isActive(orderId: number): boolean;
    readonly activeCount: number;
    prune(): void;
}

declare class VolumeProfile {
    constructor(tickSize?: number);
    destroy(): void;
    addTrade(price: number, qty: number, isBuy: boolean): void;
    poc(): number;
    valueAreaHigh(): number;
    valueAreaLow(): number;
    totalVolume(): number;
    clear(): void;
}

declare class FootprintBar {
    constructor(tickSize?: number);
    destroy(): void;
    addTrade(price: number, qty: number, isBuy: boolean): void;
    totalDelta(): number;
    totalVolume(): number;
    clear(): void;
}

declare class MarketProfile {
    constructor(tickSize?: number, periodMinutes?: number, sessionStartNs?: number);
    destroy(): void;
    addTrade(timestampNs: number, price: number, qty: number, isBuy: boolean): void;
    poc(): number;
    valueAreaHigh(): number;
    valueAreaLow(): number;
    initialBalanceHigh(): number;
    initialBalanceLow(): number;
    isPoorHigh(): boolean;
    isPoorLow(): boolean;
    clear(): void;
}

declare class CompositeBook {
    constructor();
    destroy(): void;
    bestBid(symbol: number): { price: number; quantity: number } | null;
    bestAsk(symbol: number): { price: number; quantity: number } | null;
    hasArbitrage(symbol: number): boolean;
}

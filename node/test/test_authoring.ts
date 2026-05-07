// Compile-and-run test for the TypeScript authoring layer.
//
// Builds a small SMA-crossover strategy as a decorated TS class, runs it
// through Runner against an in-memory tape, and asserts that the run
// produces signals. The actual JS that runs at test time is the tsc
// output under `node/test/dist/`.

declare const console: { log(...args: unknown[]): void; error(...args: unknown[]): void };
declare const process: { exit(code: number): never };

import * as flox from "..";
import { compile, onStart, onStop, onTrade, strategy, StrategyBase } from "../authoring/dist";

let passed = 0;
let failed = 0;

function check(condition: unknown, msg: string): void {
  if (condition) {
    console.log(`  ok  ${msg}`);
    passed += 1;
  } else {
    console.error(`  FAIL  ${msg}`);
    failed += 1;
  }
}

const registry = new flox.SymbolRegistry();
const symbolId: number = Number(registry.addSymbol("test", "BTCUSDT", 0.01));

@strategy({ symbols: [symbolId] })
class SmaCross extends StrategyBase {
  private readonly fast = new flox.SMA(3);
  private readonly slow = new flox.SMA(8);
  public starts = 0;
  public stops = 0;
  public seenTrades = 0;

  @onStart
  start(): void {
    this.starts += 1;
  }

  @onStop
  stop(): void {
    this.stops += 1;
  }

  @onTrade
  onEveryTrade(ctx: flox.SymbolContext, t: flox.TradeData, emit: flox.EmitMethods): void {
    this.seenTrades += 1;
    const f = this.fast.update(t.price);
    const s = this.slow.update(t.price);
    if (f === null || s === null) return;
    if (f > s && ctx.position === 0) emit.marketBuy(0.01);
    else if (f < s && ctx.position > 0) emit.marketSell(0.01);
  }
}

const instance = new SmaCross();
const strat = compile(instance);

check(Array.isArray(strat.symbols) && strat.symbols!.length === 1,
  "compile() preserves @strategy symbols");
check(typeof strat.onTrade === "function", "compile() wires @onTrade");
check(typeof strat.onStart === "function", "compile() wires @onStart");
check(typeof strat.onStop === "function", "compile() wires @onStop");
check(strat.onBar === undefined, "compile() omits handlers that were not decorated");

const signals: flox.Signal[] = [];
const runner = new flox.Runner(registry, (sig) => signals.push(sig), false);
runner.addStrategy(strat);
runner.start();

for (let i = 0; i < 40; i += 1) {
  const price = 100 + (i < 20 ? i * 0.5 : (40 - i) * 0.5);
  const ts = (i + 1) * 1_000_000;
  runner.onTrade(symbolId, price, 1.0, i % 2 === 0, ts);
}
runner.stop();

check(instance.starts === 1, `onStart fired exactly once (got ${instance.starts})`);
check(instance.stops === 1, `onStop fired exactly once (got ${instance.stops})`);
check(instance.seenTrades === 40, `onTrade fired for every tick (got ${instance.seenTrades})`);
check(signals.length > 0, `strategy emitted at least one signal (got ${signals.length})`);

if (failed > 0) {
  console.error(`\n${failed} check(s) failed`);
  process.exit(1);
}
console.log(`\n${passed} check(s) passed`);

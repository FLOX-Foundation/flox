// flox/dex -- the symbol-aware comfort layer over the exact AMM curves, for Node.
//
// Mirrors python/flox_py/dex: Token / Amount / Pool venue classes / Quote / Router /
// arb / Tape, the same object model and the same results to the wei. A thin layer over
// the native ammCurve* / poolTape* handles -- wei math stays in the addon, this carries
// the decimals and the token order so a mis-routed swap raises instead of returning a
// quiet zero.
//
//   const { dex } = require('@flox-foundation/flox');
//   const weth = new dex.Token('WETH', 18), usdc = new dex.Token('USDC', 6);
//   const pool = new dex.UniswapV2(weth, usdc,
//                  { reserves: ['1000 WETH', '2000000 USDC'], fee: '0.30%' });
//   pool.quote('10 WETH').out.toString();   // '19743.160687 USDC'
'use strict';

const path = require('path');

// Resolve the native addon the same way index.js does, so this module is
// self-contained and usable as `require('@flox-foundation/flox/lib/dex')`.
function loadNative() {
  const key = `${process.platform}-${process.arch}`;
  const prebuilt = path.join(__dirname, '..', 'prebuilds', key, 'flox_node.node');
  const local = path.join(__dirname, '..', 'build', 'Release', 'flox_node.node');
  try {
    return require(prebuilt);
  } catch (_) {
    return require(local);
  }
}

const N = loadNative();

// ── units ─────────────────────────────────────────────────────────────

function pow10(n) { return 10n ** BigInt(n); }

class Token {
  constructor(symbol, decimals) {
    this.symbol = symbol;
    this.decimals = decimals;
  }

  // Human units -> wei. A bigint is whole units; a string/number may carry a fraction.
  toWei(human) {
    if (typeof human === 'bigint') return human * pow10(this.decimals);
    let s = String(human).replace(/_/g, '').trim();
    const neg = s.startsWith('-');
    if (neg) s = s.slice(1);
    let [i, f = ''] = s.split('.');
    f = (f + '0'.repeat(this.decimals)).slice(0, this.decimals);  // pad / truncate to scale
    const wei = BigInt((i || '0') + f);
    return neg ? -wei : wei;
  }

  // wei -> exact human string (no trailing zeros).
  toHuman(wei) {
    const neg = wei < 0n;
    let w = neg ? -wei : wei;
    if (this.decimals === 0) return (neg ? '-' : '') + w.toString();
    const s = w.toString().padStart(this.decimals + 1, '0');
    const intPart = s.slice(0, s.length - this.decimals);
    const frac = s.slice(s.length - this.decimals).replace(/0+$/, '');
    return (neg ? '-' : '') + intPart + (frac ? '.' + frac : '');
  }

  amount(human) { return new Amount(this, this.toWei(human)); }
}

class Amount {
  constructor(token, wei) {
    this.token = token;
    this.wei = BigInt(wei);
  }

  get human() { return Number(this.token.toHuman(this.wei)); }     // for arithmetic / display
  get humanString() { return this.token.toHuman(this.wei); }       // exact
  toString() { return `${this.humanString} ${this.token.symbol}`; }

  static parse(spec, tokens) {
    if (spec instanceof Amount) return spec;
    const s = String(spec).trim();
    const sp = s.lastIndexOf(' ');
    if (sp < 0) throw new Error(`amount needs a symbol: ${spec!=null?spec:''} (e.g. "10 WETH")`);
    const num = s.slice(0, sp);
    const sym = s.slice(sp + 1).trim();
    const token = tokens[sym];
    if (!token) throw new Error(`unknown token '${sym}'`);
    return new Amount(token, token.toWei(num));
  }
}

class Quote {
  constructor(amountIn, out, fee, priceImpact, avgPrice) {
    this.amountIn = amountIn;
    this.out = out;
    this.fee = fee;
    this.priceImpact = priceImpact;
    this.avgPrice = avgPrice;
  }
  get exactWei() { return this.out.wei; }
  toString() {
    return `Quote(out=${this.out}, impact=${(this.priceImpact * 100).toFixed(3)}%, `
         + `avgPx=${this.avgPrice.toFixed(4)}, fee=${this.fee})`;
  }
}

// ── pool ──────────────────────────────────────────────────────────────

const Q96 = 2 ** 96;

class Pool {
  constructor(token0, token1, curve) {
    this.token0 = token0;
    this.token1 = token1;
    this._c = curve;
    this._tokens = { [token0.symbol]: token0, [token1.symbol]: token1 };
  }

  get venue() { return 'amm'; }

  _index(token) {
    if (token === this.token0 || token.symbol === this.token0.symbol) return 0;
    if (token === this.token1 || token.symbol === this.token1.symbol) return 1;
    throw new Error(`${token.symbol} is not in this pool `
                  + `(${this.token0.symbol}/${this.token1.symbol})`);
  }

  _other(token) {
    return (token.symbol === this.token0.symbol) ? this.token1 : this.token0;
  }

  _resolve(amount, into) {
    const amt = Amount.parse(amount, this._tokens);
    let outTok;
    if (into == null) {
      outTok = this._other(amt.token);
    } else if (into instanceof Token) {
      outTok = into;
    } else {
      outTok = this._tokens[into];
      if (!outTok) throw new Error(`unknown out-token '${into}'`);
    }
    return { amt, outTok };
  }

  _tokenAt(i) { return i === 0 ? this.token0 : this.token1; }

  balances() { return [N.ammCurveBalance(this._c, 0), N.ammCurveBalance(this._c, 1)]; }

  _mid(i, j) {
    const sp = N.ammCurveSqrtPrice(this._c);   // bigint | null
    if (sp !== null && sp !== undefined) {
      const raw = (Number(sp) / Q96) ** 2;      // token1_wei per token0_wei
      const price0in1 = raw * 10 ** this.token0.decimals / 10 ** this.token1.decimals;
      if (i === 0 && j === 1) return price0in1;
      return price0in1 ? 1 / price0in1 : 0;
    }
    const b = this.balances();
    if (b[i] === 0n) return 0;
    const hi = Number(this._tokenAt(i).toHuman(b[i]));
    const hj = Number(this._tokenAt(j).toHuman(b[j]));
    return hi > 0 ? hj / hi : 0;
  }

  _feeWei(_amountInWei) { return 0n; }

  quote(amount, into) {
    const { amt, outTok } = this._resolve(amount, into);
    const i = this._index(amt.token), j = this._index(outTok);
    const outWei = N.ammCurveAmountOut(this._c, i, j, amt.wei);
    const b = this.balances();
    if (amt.wei > 0n && outWei === 0n && b[0] > 0n && b[1] > 0n) {
      throw new Error(`swap of ${amt} -> ${outTok.symbol} returned 0 -- check token order / `
                    + `decimals (amount is ${amt.token.symbol}, ${amt.token.decimals} decimals)`);
    }
    const out = new Amount(outTok, outWei);
    const avg = amt.human > 0 ? out.human / amt.human : 0;
    const mid = this._mid(i, j);
    const impact = mid > 0 ? 1 - avg / mid : 0;
    const fee = new Amount(amt.token, this._feeWei(amt.wei));
    return new Quote(amt, out, fee, impact, avg);
  }

  swap(amount, into) {
    const { amt, outTok } = this._resolve(amount, into);
    const i = this._index(amt.token), j = this._index(outTok);
    const outWei = N.ammCurveApplySwap(this._c, i, j, amt.wei);
    const out = new Amount(outTok, outWei);
    const avg = amt.human > 0 ? out.human / amt.human : 0;
    return new Quote(amt, out, new Amount(amt.token, this._feeWei(amt.wei)), 0, avg);
  }

  get reserves() {
    const b = this.balances();
    return [new Amount(this.token0, b[0]), new Amount(this.token1, b[1])];
  }

  get spotPrice() { return this._mid(0, 1); }

  price(of, in_) { return this._mid(this._index(of), this._index(in_)); }

  depth(sizes) {
    return sizes.map((s) => {
      const q = this.quote(s);
      return {
        in: q.amountIn.toString(), out: q.out.toString(),
        avgPrice: q.avgPrice, impactPct: q.priceImpact * 100,
      };
    });
  }

  clone() {
    const p = Object.create(Object.getPrototypeOf(this));
    Object.assign(p, this);
    p._c = N.ammCurveClone(this._c);
    return p;
  }

  toString() {
    const [r0, r1] = this.reserves;
    return `<${this.venue} ${this.token0.symbol}/${this.token1.symbol}  `
         + `px=${this.spotPrice.toFixed(2)}  reserves=(${r0}, ${r1})>`;
  }
}

// ── venues ────────────────────────────────────────────────────────────

function pct(spec) {
  if (typeof spec === 'string') return Number(spec.trim().replace(/%$/, '')) / 100;
  return Number(spec);
}

class UniswapV2 extends Pool {
  constructor(token0, token1, { reserves, fee = '0.30%', feeDen = 1000 } = {}) {
    const toks = { [token0.symbol]: token0, [token1.symbol]: token1 };
    const r0 = Amount.parse(reserves[0], toks), r1 = Amount.parse(reserves[1], toks);
    const f = pct(fee);
    const feeNum = Math.round((1 - f) * feeDen);
    super(token0, token1, N.ammCurveConstantProduct(r0.wei, r1.wei, feeNum, feeDen));
    this._feeNum = feeNum; this._feeDen = feeDen;
  }
  get venue() { return 'UniswapV2'; }
  _feeWei(amountInWei) {
    return amountInWei - amountInWei * BigInt(this._feeNum) / BigInt(this._feeDen);
  }
}

class RaydiumCp extends Pool {
  constructor(token0, token1,
              { reserves, tradeFee = '0.25%', creatorFee = '0%', creatorFeeOnInput = true } = {}) {
    const toks = { [token0.symbol]: token0, [token1.symbol]: token1 };
    const r0 = Amount.parse(reserves[0], toks), r1 = Amount.parse(reserves[1], toks);
    const trade = Math.round(pct(tradeFee) * 1e6);
    const creator = Math.round(pct(creatorFee) * 1e6);
    super(token0, token1, N.ammCurveRaydiumCp(r0.wei, r1.wei, trade, creator, creatorFeeOnInput));
    this._trade = trade;
  }
  get venue() { return 'RaydiumCp'; }
  _feeWei(amountInWei) {
    // ceil-div of the trade fee over a 1e6 denominator.
    const num = amountInWei * BigInt(this._trade);
    return (num + 999999n) / 1000000n;
  }
}

class UniswapV3 extends Pool {
  constructor(token0, token1, { sqrtPriceX96, liquidity, fee = '0.05%', ticks = [] } = {}) {
    const feePips = Math.round(pct(fee) * 1e6);
    super(token0, token1, N.ammCurveUniswapV3(BigInt(sqrtPriceX96), BigInt(liquidity), feePips,
                                              ticks.map(([s, n]) => [BigInt(s), BigInt(n)])));
    this._feePips = feePips;
  }
  get venue() { return 'UniswapV3'; }
  get sqrtPrice() { return N.ammCurveSqrtPrice(this._c); }
  get liquidity() { return N.ammCurveLiquidity(this._c); }
  _feeWei(amountInWei) { return amountInWei * BigInt(this._feePips) / 1000000n; }

  static fromPrice(token0, token1, { price, liquidity, fee = '0.05%', ticks = [] }) {
    // price = token1_human per token0_human; raw = token1_wei / token0_wei.
    const raw = Number(price) * 10 ** token1.decimals / 10 ** token0.decimals;
    const sqrtX96 = BigInt(Math.round(Math.sqrt(raw) * Q96));
    return new UniswapV3(token0, token1, { sqrtPriceX96: sqrtX96, liquidity, fee, ticks });
  }
}

// ── tape / backtest ───────────────────────────────────────────────────

class Tape {
  constructor(pool) {
    this.pool = pool;
    this._swaps = [];       // {ts, i, weiIn, j}
    this._checkpoints = []; // {ts, r0, r1}
  }

  swap(ts, amount, into) {
    const { amt, outTok } = this.pool._resolve(amount, into);
    this._swaps.push({ ts: Number(ts), i: this.pool._index(amt.token),
                       weiIn: amt.wei, j: this.pool._index(outTok) });
    return this;
  }

  swapWei(ts, amountInWei, baseForQuote) {
    const [i, j] = baseForQuote ? [0, 1] : [1, 0];
    this._swaps.push({ ts: Number(ts), i, weiIn: BigInt(amountInWei), j });
    return this;
  }

  checkpoint(ts, reserve0, reserve1) {
    this._checkpoints.push({
      ts: Number(ts),
      r0: Tape._toWei(reserve0, this.pool.token0),
      r1: Tape._toWei(reserve1, this.pool.token1),
    });
    return this;
  }

  fromSwaps(swaps) {
    for (const s of swaps) this.swap(s[0], s[1], s[2]);
    return this;
  }

  static _toWei(value, token) {
    if (value instanceof Amount) return value.wei;
    if (typeof value === 'string' && /[a-zA-Z]/.test(value)) {
      return Amount.parse(value, { [token.symbol]: token }).wei;
    }
    return token.toWei(value);
  }

  // Decoded Uniswap v2 Swap logs: data words (amount0In, amount1In, amount0Out, amount1Out).
  static fromEvmLogs(pool, swapLogs) {
    const tape = new Tape(pool);
    for (const lg of swapLogs) {
      const data = lg.data.startsWith('0x') ? lg.data.slice(2) : lg.data;
      const words = [];
      for (let k = 0; k < data.length; k += 64) words.push(BigInt('0x' + data.slice(k, k + 64)));
      const a0in = words[0], a1in = words[1];
      const bn = lg.blockNumber != null ? lg.blockNumber : (lg.ts != null ? lg.ts : 0);
      const ts = typeof bn === 'string' ? parseInt(bn, 16) : Number(bn);
      if (a0in > 0n) tape.swapWei(ts, a0in, true);
      else if (a1in > 0n) tape.swapWei(ts, a1in, false);
    }
    return tape;
  }

  replay() {
    const work = this.pool.clone();
    const t0 = this.pool.token0, t1 = this.pool.token1;
    const b0 = work.balances();
    const start0 = Number(t0.toHuman(b0[0])), start1 = Number(t1.toHuman(b0[1]));

    const events = [
      ...this._swaps.map((s) => ({ kind: 'swap', ...s })),
      ...this._checkpoints.map((c) => ({ kind: 'checkpoint', ...c })),
    ].sort((a, b) => a.ts - b.ts);

    let driftCount = 0;
    const rows = [];
    for (const ev of events) {
      if (ev.kind === 'swap') {
        N.ammCurveApplySwap(work._c, ev.i, ev.j, ev.weiIn);
        const bal = work.balances();
        const price = work.spotPrice;
        const r0h = Number(t0.toHuman(bal[0])), r1h = Number(t1.toHuman(bal[1]));
        const lpValue = r0h * price + r1h;
        const hodl = start0 * price + start1;
        const il = hodl > 0 ? lpValue / hodl - 1 : 0;
        rows.push({ ts: ev.ts, price, reserve0: bal[0], reserve1: bal[1],
                    lpValue, il, drift: false, trade: true });
      } else {
        const bal = work.balances();
        const mismatch = bal[0] !== ev.r0 || bal[1] !== ev.r1;
        if (mismatch) driftCount += 1;
        rows.push({ ts: ev.ts, price: work.spotPrice, reserve0: bal[0], reserve1: bal[1],
                    lpValue: null, il: null, drift: mismatch, trade: false });
      }
    }
    rows.driftCount = driftCount;
    rows.finalReserve0 = work.balances()[0];
    rows.finalReserve1 = work.balances()[1];
    return rows;
  }
}

class LpPosition {
  constructor(pool, value) {
    this.pool = pool;
    this._t0 = pool.token0; this._t1 = pool.token1;
    const [r0, r1] = pool.reserves;
    this._entry0 = r0.human; this._entry1 = r1.human;
    this._entryPrice = pool.spotPrice;
    const poolValue = this._entry0 * this._entryPrice + this._entry1;
    this.share = value == null ? 1 : this._valueInToken1(value) / poolValue;
  }

  _valueInToken1(value) {
    let amt = null;
    if (value instanceof Amount) amt = value;
    else if (typeof value === 'string' && /[a-zA-Z]/.test(value)) {
      amt = Amount.parse(value, { [this._t0.symbol]: this._t0, [this._t1.symbol]: this._t1 });
    } else return Number(value);
    if (amt.token.symbol !== this._t1.symbol) {
      throw new Error(`position value must be in ${this._t1.symbol} (the value currency), `
                    + `got ${amt.token.symbol}`);
    }
    return amt.human;
  }

  value(at) {
    const p = at || this.pool;
    const [r0, r1] = p.reserves;
    return (r0.human * p.spotPrice + r1.human) * this.share;
  }

  hodlValue(at) {
    const p = at || this.pool;
    return (this._entry0 * p.spotPrice + this._entry1) * this.share;
  }

  impermanentLoss(at) {
    const hodl = this.hodlValue(at);
    return hodl > 0 ? this.value(at) / hodl - 1 : 0;
  }
}

// ── router / arb ──────────────────────────────────────────────────────

class Router {
  constructor(pools) {
    this.pools = Array.from(pools);
    if (!this.pools.length) throw new Error('Router needs at least one pool');
  }

  quotes(amount, into) {
    const out = [];
    for (const p of this.pools) {
      try { out.push([p, p.quote(amount, into)]); } catch (_) { /* skip */ }
    }
    return out;
  }

  best(amount, into) {
    const qs = this.quotes(amount, into);
    if (!qs.length) throw new Error('no pool in the router could price this swap');
    return qs.reduce((a, b) => (b[1].out.wei > a[1].out.wei ? b : a));
  }

  table(amount, into) {
    return this.quotes(amount, into)
      .sort((a, b) => (b[1].out.wei > a[1].out.wei ? 1 : -1))
      .map(([p, q]) => ({ venue: p.venue, out: q.out.toString(),
                          outHuman: q.out.human, impactPct: q.priceImpact * 100 }));
  }
}

class Arb {
  constructor(size, profit, route) {
    this.size = size; this.profit = profit; this.route = route;
  }
  get profitable() { return this.profit.wei > 0n; }
  toString() {
    if (!this.profitable) return 'Arb(no edge)';
    return `Arb(size=${this.size}, profit=${this.profit}, route=${this.route[0]}->${this.route[1]})`;
  }
}

function arb(poolA, poolB) {
  const t0 = poolA.token0, t1 = poolA.token1;
  const pairB = new Set([poolB.token0.symbol, poolB.token1.symbol]);
  if (pairB.size !== 2 || !pairB.has(t0.symbol) || !pairB.has(t1.symbol)) {
    throw new Error(`pools trade different pairs: ${t0.symbol}/${t1.symbol} vs `
                  + `${poolB.token0.symbol}/${poolB.token1.symbol}`);
  }
  const pa = poolA.price(t0, t1), pb = poolB.price(t0, t1);
  if (pa === pb) return new Arb(new Amount(t1, 0n), new Amount(t1, 0n), null);
  const [buy, sell] = pa < pb ? [poolA, poolB] : [poolB, poolA];

  const bi1 = buy._index(t1), bi0 = buy._index(t0);
  const si0 = sell._index(t0), si1 = sell._index(t1);

  const profitWei = (x) => {
    if (x <= 0n) return 0n;
    const t0out = N.ammCurveAmountOut(buy._c, bi1, bi0, x);
    if (t0out === 0n) return 0n;
    return N.ammCurveAmountOut(sell._c, si0, si1, t0out) - x;
  };

  let hi = buy.balances()[bi1];
  if (hi <= 0n) return new Arb(new Amount(t1, 0n), new Amount(t1, 0n), null);
  let lo = 0n;
  while (hi - lo > 2n) {
    const m1 = lo + (hi - lo) / 3n;
    const m2 = hi - (hi - lo) / 3n;
    if (profitWei(m1) < profitWei(m2)) lo = m1; else hi = m2;
  }
  let bestX = lo, bestP = profitWei(lo);
  for (let x = lo; x <= hi; x += 1n) {
    const p = profitWei(x);
    if (p > bestP) { bestP = p; bestX = x; }
  }
  if (bestP <= 0n) return new Arb(new Amount(t1, 0n), new Amount(t1, 0n), null);
  return new Arb(new Amount(t1, bestX), new Amount(t1, bestP), [buy.venue, sell.venue]);
}

module.exports = {
  Token, Amount, Quote, Pool, UniswapV2, RaydiumCp, UniswapV3,
  Tape, LpPosition, Router, Arb, arb,
};

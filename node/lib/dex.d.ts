// Type declarations for flox/dex -- the symbol-aware comfort layer over the exact
// AMM curves. Mirrors python/flox_py/dex. Shipped next to lib/dex.js so
// `import { Token } from '@flox-foundation/flox/lib/dex'` (or the `.dex` property of
// the main module) is fully typed. These types live here rather than in index.d.ts
// because the d.ts-export parity gate only allows names backed by a NAPI export.

export type AmountLike = string | number | bigint | Amount;

export class Token {
  symbol: string;
  decimals: number;
  constructor(symbol: string, decimals: number);
  /** Human units to wei. A bigint is whole units; a string/number may carry a fraction. */
  toWei(human: string | number | bigint): bigint;
  /** wei to an exact human string (no trailing zeros). */
  toHuman(wei: bigint): string;
  amount(human: string | number | bigint): Amount;
}

export class Amount {
  token: Token;
  wei: bigint;
  constructor(token: Token, wei: bigint | number);
  /** Human magnitude as a number, for display and arithmetic. */
  readonly human: number;
  /** Exact human string. */
  readonly humanString: string;
  toString(): string;
  static parse(spec: AmountLike, tokens: Record<string, Token>): Amount;
}

export class Quote {
  amountIn: Amount;
  out: Amount;
  fee: Amount;
  priceImpact: number;
  avgPrice: number;
  /** The raw curve output in wei, lossless. */
  readonly exactWei: bigint;
  toString(): string;
}

export class Pool {
  token0: Token;
  token1: Token;
  readonly venue: string;
  balances(): [bigint, bigint];
  quote(amount: AmountLike, into?: Token | string): Quote;
  /** Like quote(), but moves the pool. */
  swap(amount: AmountLike, into?: Token | string): Quote;
  readonly reserves: [Amount, Amount];
  /** Marginal price of token0 in token1 (e.g. WETH priced in USDC). */
  readonly spotPrice: number;
  price(of: Token, in_: Token): number;
  depth(sizes: AmountLike[]): Array<{
    in: string; out: string; avgPrice: number; impactPct: number;
  }>;
  clone(): this;
  toString(): string;
}

export interface UniswapV2Opts { reserves: [AmountLike, AmountLike]; fee?: string | number; feeDen?: number; }
export class UniswapV2 extends Pool {
  constructor(token0: Token, token1: Token, opts: UniswapV2Opts);
}

export interface RaydiumCpOpts {
  reserves: [AmountLike, AmountLike];
  tradeFee?: string | number;
  creatorFee?: string | number;
  creatorFeeOnInput?: boolean;
}
export class RaydiumCp extends Pool {
  constructor(token0: Token, token1: Token, opts: RaydiumCpOpts);
}

export interface UniswapV3Opts {
  sqrtPriceX96: bigint | number | string;
  liquidity: bigint | number | string;
  fee?: string | number;
  ticks?: Array<[bigint | number, bigint | number]>;
}
export interface UniswapV3FromPriceOpts {
  price: number | string;
  liquidity: bigint | number | string;
  fee?: string | number;
  ticks?: Array<[bigint | number, bigint | number]>;
}
export class UniswapV3 extends Pool {
  constructor(token0: Token, token1: Token, opts: UniswapV3Opts);
  readonly sqrtPrice: bigint | null;
  readonly liquidity: bigint | null;
  static fromPrice(token0: Token, token1: Token, opts: UniswapV3FromPriceOpts): UniswapV3;
}

export interface ReplayRow {
  ts: number;
  price: number;
  reserve0: bigint;
  reserve1: bigint;
  lpValue: number | null;
  il: number | null;
  drift: boolean;
  trade: boolean;
}
export interface ReplayResult extends Array<ReplayRow> {
  driftCount: number;
  finalReserve0: bigint;
  finalReserve1: bigint;
}

export class Tape {
  constructor(pool: Pool);
  swap(ts: number, amount: AmountLike, into?: Token | string): this;
  swapWei(ts: number, amountInWei: bigint | number, baseForQuote: boolean): this;
  checkpoint(ts: number, reserve0: AmountLike, reserve1: AmountLike): this;
  fromSwaps(swaps: Array<[number, AmountLike] | [number, AmountLike, Token | string]>): this;
  static fromEvmLogs(pool: Pool, swapLogs: Array<{ data: string; blockNumber?: string | number; ts?: number }>): Tape;
  replay(): ReplayResult;
}

export class LpPosition {
  constructor(pool: Pool, value?: AmountLike);
  share: number;
  value(at?: Pool): number;
  hodlValue(at?: Pool): number;
  impermanentLoss(at?: Pool): number;
}

export class Router {
  constructor(pools: Pool[]);
  quotes(amount: AmountLike, into?: Token | string): Array<[Pool, Quote]>;
  best(amount: AmountLike, into?: Token | string): [Pool, Quote];
  table(amount: AmountLike, into?: Token | string): Array<{
    venue: string; out: string; outHuman: number; impactPct: number;
  }>;
}

export class Arb {
  size: Amount;
  profit: Amount;
  route: [string, string] | null;
  readonly profitable: boolean;
  toString(): string;
}

export function arb(poolA: Pool, poolB: Pool): Arb;

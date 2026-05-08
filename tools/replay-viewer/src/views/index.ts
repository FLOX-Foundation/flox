// View renderers. Each takes a slice of state and a container element.
// Cheap re-render on every cursor tick is acceptable for the MVP scope
// (50K events, M-class macbook target). Optimize later if needed.

import type { ViewerState } from '../state.ts';
import { tapeSlice, signalsSlice, ordersSlice, fillsSlice } from '../state.ts';
import type { Trade } from '../parsers/floxlog.ts';

const fmtTs = (ns: bigint) => {
  const ms = Number(ns / 1_000_000n);
  const d = new Date(ms);
  return d.toISOString().slice(11, 23);
};
const fmtNum = (n: number, dp = 2) => n.toFixed(dp);

// Cache the most recent render fingerprint per view so we skip the
// expensive part (innerHTML / canvas redraw) when the visible slice
// has not changed since the previous frame. The price chart cursor
// line is the one element that does need to redraw every frame, so
// renderPriceChart bypasses this cache.
let lastTradesSig = '';
let lastSignalsSig = '';
let lastOrdersSig = '';
let lastOrderbookSig = '';
let lastEquitySig = '';

export function renderTrades(state: ViewerState) {
  const el = document.getElementById('view-trades')!;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="trades"]')!;
  const trades = tapeSlice(state).filter((e) => e.type === 'trade').slice(-100);
  const sig = `${state.tape.length}|${trades.length}|${trades[trades.length - 1]?.ts_ns ?? ''}`;
  if (sig === lastTradesSig) return;
  lastTradesSig = sig;
  if (trades.length === 0 && state.tape.length === 0) {
    empty.hidden = false;
    empty.textContent = 'No `.floxlog` loaded. Drop one to see trades.';
    el.innerHTML = '';
    return;
  }
  empty.hidden = true;
  el.innerHTML = trades
    .reverse()
    .map((t) => {
      if (t.type !== 'trade') return '';
      return `<div class="row ${t.side === 'buy' ? 'bid' : 'ask'}">
        <span class="ts">${fmtTs(t.ts_ns)}</span>
        <span>${t.side}</span>
        <span>${fmtNum(t.price)}</span>
        <span>${fmtNum(t.qty, 4)}</span>
      </div>`;
    })
    .join('');
}

export function renderOrderbook(state: ViewerState) {
  const canvas = document.getElementById('view-orderbook') as HTMLCanvasElement;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="orderbook"]')!;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = w * dpr;
  canvas.height = h * dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  // Reconstruct the latest book by replaying snapshots and deltas up to cursor.
  // For MVP simplicity, find the most recent snapshot before cursor and apply
  // deltas after it.
  const tape = tapeSlice(state);
  const bookEvents = tape.filter((e) => e.type === 'book_snapshot' || e.type === 'book_delta');
  const sig = `${state.tape.length}|${bookEvents.length}|${bookEvents[bookEvents.length - 1]?.ts_ns ?? ''}|${w}x${h}`;
  if (sig === lastOrderbookSig) return;
  lastOrderbookSig = sig;
  if (bookEvents.length === 0) {
    empty.hidden = false;
    empty.textContent = state.tape.length === 0
      ? 'No `.floxlog` loaded.'
      : 'Tape has no book events at this cursor.';
    return;
  }
  empty.hidden = true;
  let lastSnapIdx = -1;
  for (let i = bookEvents.length - 1; i >= 0; i--) {
    if (bookEvents[i].type === 'book_snapshot') { lastSnapIdx = i; break; }
  }
  if (lastSnapIdx === -1) {
    ctx.fillStyle = '#9ca3af';
    ctx.font = '12px sans-serif';
    ctx.fillText('waiting for first snapshot…', 16, 30);
    return;
  }
  const bids = new Map<number, number>();
  const asks = new Map<number, number>();
  for (let i = lastSnapIdx; i < bookEvents.length; i++) {
    const ev = bookEvents[i];
    if (ev.type !== 'book_snapshot' && ev.type !== 'book_delta') continue;
    if (ev.type === 'book_snapshot') {
      bids.clear(); asks.clear();
    }
    for (const lvl of ev.bids) {
      if (lvl.qty === 0) bids.delete(lvl.price);
      else bids.set(lvl.price, lvl.qty);
    }
    for (const lvl of ev.asks) {
      if (lvl.qty === 0) asks.delete(lvl.price);
      else asks.set(lvl.price, lvl.qty);
    }
  }
  // Render top-N levels each side.
  const N = 15;
  const bidList = [...bids.entries()].sort((a, b) => b[0] - a[0]).slice(0, N);
  const askList = [...asks.entries()].sort((a, b) => a[0] - b[0]).slice(0, N);
  const maxQty = Math.max(
    1,
    ...bidList.map(([, q]) => q),
    ...askList.map(([, q]) => q),
  );
  const rowH = Math.min(18, (h - 8) / (N * 2));
  ctx.font = '11px ui-monospace, monospace';
  ctx.textBaseline = 'middle';
  // asks (top half, ascending price descending visually so best ask sits at the spread)
  askList.slice().reverse().forEach(([price, qty], idx) => {
    const y = 8 + idx * rowH;
    const barW = (qty / maxQty) * (w * 0.45);
    ctx.fillStyle = 'rgba(214, 70, 75, 0.18)';
    ctx.fillRect(w / 2, y, barW, rowH - 2);
    ctx.fillStyle = '#d6464b';
    ctx.fillText(`${price.toFixed(2)}  ${qty.toFixed(4)}`, w / 2 + 4, y + rowH / 2);
  });
  bidList.forEach(([price, qty], idx) => {
    const y = 8 + (askList.length + idx) * rowH;
    const barW = (qty / maxQty) * (w * 0.45);
    ctx.fillStyle = 'rgba(33, 161, 101, 0.18)';
    ctx.fillRect(w / 2 - barW, y, barW, rowH - 2);
    ctx.fillStyle = '#21a165';
    ctx.fillText(`${price.toFixed(2)}  ${qty.toFixed(4)}`, w / 2 - 100, y + rowH / 2);
  });
}

export function renderSignals(state: ViewerState) {
  const el = document.getElementById('view-signals')!;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="signals"]')!;
  if (!state.run) {
    empty.hidden = false;
    empty.textContent = 'No `.floxrun` loaded. Drop one to see strategy decisions.';
    el.innerHTML = '';
    return;
  }
  const sigsAll = signalsSlice(state);
  const sig = `${state.run.signals.length}|${sigsAll.length}|${sigsAll[sigsAll.length - 1]?.run_ts_ns ?? ''}`;
  if (sig === lastSignalsSig) return;
  lastSignalsSig = sig;
  const sigs = sigsAll.slice(-50).reverse();
  empty.hidden = true;
  el.innerHTML = sigs.map((s) => `
    <div class="signal-card">
      <div><span class="ts">${fmtTs(s.run_ts_ns)}</span> <span class="name">${escapeHtml(s.name)}</span> id=${s.signal_id}</div>
      <div>symbols=[${s.symbol_ids.join(', ')}] flags=${s.flags} strength=${s.strength.toFixed(4)}</div>
      ${s.payload ? `<div style="color: var(--muted)">${escapeHtml(s.payload)}</div>` : ''}
    </div>
  `).join('');
}

const ORDER_KIND_NAMES = ['?', 'submit', 'cancel', 'modify', 'ack', 'reject', 'expire'];

export function renderOrders(state: ViewerState) {
  const el = document.getElementById('view-orders')!;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="orders"]')!;
  if (!state.run) {
    empty.hidden = false;
    el.innerHTML = '';
    return;
  }
  const ordersAll = ordersSlice(state);
  const fillsAll = fillsSlice(state);
  const sig = `${state.run.orders.length}|${ordersAll.length}|${fillsAll.length}|${ordersAll[ordersAll.length - 1]?.run_ts_ns ?? ''}|${fillsAll[fillsAll.length - 1]?.run_ts_ns ?? ''}`;
  if (sig === lastOrdersSig) return;
  lastOrdersSig = sig;
  const orders = ordersAll.slice(-50).reverse();
  const fills = fillsAll;
  const fillsByOrder = new Map<string, number>();
  for (const f of fills) {
    const k = f.order_id.toString();
    fillsByOrder.set(k, (fillsByOrder.get(k) ?? 0) + f.qty);
  }
  empty.hidden = true;
  el.innerHTML = orders.map((o) => {
    const filled = fillsByOrder.get(o.order_id.toString()) ?? 0;
    const kind = ORDER_KIND_NAMES[o.event_kind] ?? `?${o.event_kind}`;
    return `
      <div class="order-card">
        <div><span class="ts">${fmtTs(o.run_ts_ns)}</span>
             <span class="id">order=${o.order_id}</span>
             ${kind} ${o.side === 0 ? 'buy' : 'sell'} ${o.qty.toFixed(4)} @ ${o.price.toFixed(2)}</div>
        ${filled > 0 ? `<div>filled: ${filled.toFixed(4)}</div>` : ''}
        ${o.reason ? `<div style="color: var(--muted)">${escapeHtml(o.reason)}</div>` : ''}
      </div>
    `;
  }).join('');
}

export function renderEquity(state: ViewerState) {
  const canvas = document.getElementById('view-equity') as HTMLCanvasElement;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="equity"]')!;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = w * dpr;
  canvas.height = h * dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  if (!state.run) {
    empty.hidden = false;
    return;
  }
  const fills = fillsSlice(state);
  const sig = `${state.run.fills.length}|${fills.length}|${fills[fills.length - 1]?.run_ts_ns ?? ''}|${w}x${h}`;
  if (sig === lastEquitySig) return;
  lastEquitySig = sig;
  if (fills.length === 0) {
    empty.hidden = false;
    empty.textContent = 'No fills yet at this cursor.';
    return;
  }
  empty.hidden = true;
  // Naive PnL: position * mark, mark = last fill price per symbol.
  // Realized = signed fill * price. Good enough for MVP.
  type SymState = { pos: number; cash: number };
  const states = new Map<number, SymState>();
  const points: Array<{ ts: bigint; equity: number }> = [];
  for (const f of fills) {
    let st = states.get(f.symbol_id);
    if (!st) { st = { pos: 0, cash: 0 }; states.set(f.symbol_id, st); }
    const signedQty = f.side === 0 ? f.qty : -f.qty;
    st.pos += signedQty;
    st.cash -= signedQty * f.price + f.fee;
    let total = 0;
    for (const s of states.values()) total += s.cash + s.pos * f.price; // mark with last seen price
    points.push({ ts: f.run_ts_ns, equity: total });
  }
  const minEq = Math.min(...points.map((p) => p.equity), 0);
  const maxEq = Math.max(...points.map((p) => p.equity), 0);
  const span = Math.max(1e-9, maxEq - minEq);
  const t0 = points[0].ts;
  const tN = points[points.length - 1].ts;
  const tSpan = Math.max(1, Number(tN - t0));
  ctx.strokeStyle = '#138caf';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  points.forEach((p, i) => {
    const x = ((Number(p.ts - t0) / tSpan) * (w - 16)) + 8;
    const y = h - 8 - ((p.equity - minEq) / span) * (h - 16);
    if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.stroke();
  ctx.fillStyle = '#9ca3af';
  ctx.font = '10px ui-monospace, monospace';
  ctx.fillText(`min ${minEq.toFixed(2)}`, 8, h - 4);
  ctx.fillText(`max ${maxEq.toFixed(2)}`, 8, 12);
}

// Cached chart geometry from the most recent render. Mouse handlers
// read this to convert pointer position back into a tape timestamp.
interface ChartContext {
  trades: Trade[];
  t0: bigint;
  tN: bigint;
  padL: number;
  padT: number;
  plotW: number;
  plotH: number;
  minP: number;
  maxP: number;
}
let priceChartCtx: ChartContext | null = null;
let priceChartWired = false;

// Offscreen canvas holding everything that does not move when the
// cursor moves: grid, line, trade dots, signal bands, fill triangles.
// We blit it onto the visible canvas every frame and only paint the
// cursor on top in real time.
let priceChartStaticCanvas: HTMLCanvasElement | null = null;
let lastPriceStaticSig = '';

function wirePriceChart(canvas: HTMLCanvasElement, tooltip: HTMLElement) {
  if (priceChartWired) return;
  priceChartWired = true;

  const xToTs = (x: number): bigint | null => {
    const c = priceChartCtx;
    if (!c) return null;
    const tSpan = Number(c.tN - c.t0);
    const frac = (x - c.padL) / c.plotW;
    if (!Number.isFinite(frac) || frac < 0 || frac > 1) return null;
    return c.t0 + BigInt(Math.round(frac * tSpan));
  };

  const nearestTrade = (ts: bigint): Trade | null => {
    const c = priceChartCtx;
    if (!c || c.trades.length === 0) return null;
    let lo = 0, hi = c.trades.length - 1;
    while (lo < hi) {
      const mid = (lo + hi) >>> 1;
      if (c.trades[mid].ts_ns < ts) lo = mid + 1; else hi = mid;
    }
    const candidates = [c.trades[lo - 1], c.trades[lo]].filter(Boolean) as Trade[];
    let best = candidates[0];
    for (const t of candidates) {
      if (t.ts_ns > ts ? t.ts_ns - ts : ts - t.ts_ns) {
        const da = t.ts_ns > ts ? t.ts_ns - ts : ts - t.ts_ns;
        const db = best.ts_ns > ts ? best.ts_ns - ts : ts - best.ts_ns;
        if (da < db) best = t;
      }
    }
    return best;
  };

  const fmtTs = (ns: bigint) =>
    new Date(Number(ns / 1_000_000n)).toISOString().slice(11, 23);

  canvas.addEventListener('mousemove', (ev) => {
    const c = priceChartCtx;
    if (!c) return;
    const rect = canvas.getBoundingClientRect();
    const x = ev.clientX - rect.left;
    const y = ev.clientY - rect.top;
    if (x < c.padL || x > c.padL + c.plotW || y < c.padT || y > c.padT + c.plotH) {
      tooltip.hidden = true;
      return;
    }
    const ts = xToTs(x);
    if (ts === null) {
      tooltip.hidden = true;
      return;
    }
    const t = nearestTrade(ts);
    if (!t) {
      tooltip.hidden = true;
      return;
    }
    const sideClass = t.side === 'buy' ? 'side-buy' : 'side-sell';
    tooltip.innerHTML =
      `<span class="ts">${fmtTs(t.ts_ns)}</span>` +
      `<span class="${sideClass}">${t.side}</span> ` +
      `${t.price.toFixed(2)} × ${t.qty.toFixed(4)}`;
    tooltip.hidden = false;
    // Position above-right of the cursor; clamp to canvas.
    const tipX = Math.min(x + 12, canvas.clientWidth - tooltip.offsetWidth - 4);
    const tipY = Math.max(y - tooltip.offsetHeight - 8, 4);
    tooltip.style.left = `${tipX}px`;
    tooltip.style.top = `${tipY}px`;
  });
  canvas.addEventListener('mouseleave', () => { tooltip.hidden = true; });
  canvas.addEventListener('click', (ev) => {
    const rect = canvas.getBoundingClientRect();
    const ts = xToTs(ev.clientX - rect.left);
    if (ts !== null) {
      canvas.dispatchEvent(new CustomEvent('viewer-seek', {
        detail: { ts }, bubbles: true,
      }));
    }
  });
}

export function renderPriceChart(state: ViewerState) {
  const canvas = document.getElementById('view-price') as HTMLCanvasElement;
  const tooltip = document.getElementById('price-tooltip') as HTMLElement;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="price"]')!;
  const ctx = canvas.getContext('2d');
  if (!ctx) return;
  wirePriceChart(canvas, tooltip);
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth, h = canvas.clientHeight;
  canvas.width = w * dpr;
  canvas.height = h * dpr;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);

  const trades: Trade[] = state.tape.filter((e): e is Trade => e.type === 'trade');
  if (trades.length === 0) {
    empty.hidden = false;
    empty.textContent = state.tape.length === 0
      ? 'No `.floxlog` loaded. Drop one to see the price chart.'
      : 'Tape has no trades.';
    priceChartCtx = null;
    return;
  }
  empty.hidden = true;

  const t0 = state.ts_min;
  const tN = state.ts_max;
  let minP = Infinity, maxP = -Infinity;
  for (const t of trades) {
    if (t.price < minP) minP = t.price;
    if (t.price > maxP) maxP = t.price;
  }
  const padP = Math.max(1e-9, (maxP - minP) * 0.08);
  minP -= padP; maxP += padP;

  const padL = 56, padR = 12, padT = 10, padB = 22;
  const plotW = w - padL - padR;
  const plotH = h - padT - padB;
  priceChartCtx = { trades, t0, tN, padL, padT, plotW, plotH, minP, maxP };

  const sigCount = state.run ? state.run.signals.length : 0;
  const fillCount = state.run ? state.run.fills.length : 0;
  const sigKey = `${trades.length}|${sigCount}|${fillCount}|${w}x${h}|${minP}|${maxP}`;
  if (sigKey !== lastPriceStaticSig) {
    lastPriceStaticSig = sigKey;
    if (!priceChartStaticCanvas) priceChartStaticCanvas = document.createElement('canvas');
    priceChartStaticCanvas.width = w * dpr;
    priceChartStaticCanvas.height = h * dpr;
    const sctx = priceChartStaticCanvas.getContext('2d')!;
    sctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    sctx.clearRect(0, 0, w, h);
    paintPriceStatic(sctx, w, h, trades, state, t0, tN, padL, padT, plotW, plotH, minP, maxP);
  }

  if (priceChartStaticCanvas) ctx.drawImage(priceChartStaticCanvas, 0, 0, w, h);
  if (state.cursor_ns >= t0 && state.cursor_ns <= tN) {
    const tSpan = Math.max(1, Number(tN - t0));
    const cx = padL + (Number(state.cursor_ns - t0) / tSpan) * plotW;
    ctx.strokeStyle = 'rgba(26, 35, 50, 0.4)';
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(cx, padT);
    ctx.lineTo(cx, h - padB);
    ctx.stroke();
  }
}

function paintPriceStatic(target: CanvasRenderingContext2D, w: number, h: number,
                           trades: Trade[], state: ViewerState,
                           t0: bigint, tN: bigint,
                           padL: number, padT: number, plotW: number, plotH: number,
                           minP: number, maxP: number) {
  const tSpan = Math.max(1, Number(tN - t0));
  const pSpan = Math.max(1e-9, maxP - minP);
  const xOf = (ts: bigint) => padL + (Number(ts - t0) / tSpan) * plotW;
  const yOf = (price: number) => padT + (1 - (price - minP) / pSpan) * plotH;
  target.strokeStyle = '#eef0f4';
  target.lineWidth = 1;
  target.fillStyle = '#9ca3af';
  target.font = '10px ui-monospace, monospace';
  target.textBaseline = 'middle';
  for (let i = 0; i <= 4; i++) {
    const y = padT + (i / 4) * plotH;
    target.beginPath();
    target.moveTo(padL, y);
    target.lineTo(w - 12, y);
    target.stroke();
    const p = maxP - (i / 4) * pSpan;
    target.fillText(p.toFixed(2), 6, y);
  }
  target.strokeStyle = '#138caf';
  target.lineWidth = 1.2;
  target.beginPath();
  trades.forEach((t, i) => {
    const x = xOf(t.ts_ns), y = yOf(t.price);
    if (i === 0) target.moveTo(x, y); else target.lineTo(x, y);
  });
  target.stroke();
  for (const t of trades) {
    target.fillStyle = t.side === 'buy' ? 'rgba(33, 161, 101, 0.55)' : 'rgba(214, 70, 75, 0.55)';
    target.beginPath();
    target.arc(xOf(t.ts_ns), yOf(t.price), 1.6, 0, Math.PI * 2);
    target.fill();
  }
  const sigs = state.run ? state.run.signals : [];
  target.font = '10px ui-monospace, monospace';
  for (const s of sigs) {
    if (s.run_ts_ns < t0 || s.run_ts_ns > tN) continue;
    const x = xOf(s.run_ts_ns);
    const isExit = (s.flags & 0x02) !== 0;
    target.strokeStyle = isExit ? 'rgba(214, 70, 75, 0.5)' : 'rgba(33, 161, 101, 0.5)';
    target.lineWidth = 1;
    target.setLineDash([3, 3]);
    target.beginPath();
    target.moveTo(x, padT);
    target.lineTo(x, h - 22);
    target.stroke();
    target.setLineDash([]);
    target.fillStyle = isExit ? '#d6464b' : '#21a165';
    target.fillText(s.name, x + 3, padT + 8);
  }
  const fills = state.run ? state.run.fills : [];
  for (const f of fills) {
    if (f.run_ts_ns < t0 || f.run_ts_ns > tN) continue;
    const x = xOf(f.run_ts_ns);
    const y = yOf(f.price);
    target.fillStyle = f.side === 0 ? '#21a165' : '#d6464b';
    target.beginPath();
    if (f.side === 0) {
      target.moveTo(x, y - 6);
      target.lineTo(x - 4, y);
      target.lineTo(x + 4, y);
    } else {
      target.moveTo(x, y + 6);
      target.lineTo(x - 4, y);
      target.lineTo(x + 4, y);
    }
    target.closePath();
    target.fill();
  }
}

function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
}

// View renderers. Each takes a slice of state and a container element.
// Cheap re-render on every cursor tick is acceptable for the MVP scope
// (50K events, M-class macbook target). Optimize later if needed.

import type { ViewerState } from '../state.ts';
import { tapeSlice, signalsSlice, ordersSlice, fillsSlice } from '../state.ts';

const fmtTs = (ns: bigint) => {
  const ms = Number(ns / 1_000_000n);
  const d = new Date(ms);
  return d.toISOString().slice(11, 23);
};
const fmtNum = (n: number, dp = 2) => n.toFixed(dp);

export function renderTrades(state: ViewerState) {
  const el = document.getElementById('view-trades')!;
  const empty = document.querySelector<HTMLElement>('.emptystate[data-for="trades"]')!;
  const trades = tapeSlice(state).filter((e) => e.type === 'trade').slice(-100);
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
  const sigs = signalsSlice(state).slice(-50).reverse();
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
  const orders = ordersSlice(state).slice(-50).reverse();
  const fills = fillsSlice(state);
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

function escapeHtml(s: string): string {
  return s.replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]!));
}

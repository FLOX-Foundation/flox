// Timeline state for the replay viewer.
//
// Holds parsed .floxlog and .floxrun events. Maintains a cursor in
// nanoseconds; each view subscribes and renders the slice up to the
// cursor on tick. URL bookmarks store cursor + speed so a shared link
// resumes at the same position.

import type { FloxlogEvent } from './parsers/floxlog.ts';
import type { FloxrunData, Signal, OrderEvent, Fill } from './parsers/floxrun.ts';

export interface ViewerState {
  tape: FloxlogEvent[];
  run: FloxrunData | null;
  // Range covered by the loaded data, in nanoseconds.
  ts_min: bigint;
  ts_max: bigint;
  // Current playback cursor.
  cursor_ns: bigint;
  // Playback speed multiplier. 1 = wall-clock real-time.
  speed: number;
  playing: boolean;
}

export type Listener = (state: ViewerState) => void;

export class Store {
  private state: ViewerState = {
    tape: [],
    run: null,
    ts_min: 0n,
    ts_max: 0n,
    cursor_ns: 0n,
    speed: 1,
    playing: false,
  };
  private listeners: Listener[] = [];
  private rafHandle: number | null = null;
  private lastTick: number | null = null;

  get(): ViewerState { return this.state; }

  subscribe(fn: Listener): () => void {
    this.listeners.push(fn);
    return () => { this.listeners = this.listeners.filter((l) => l !== fn); };
  }

  private notify() {
    for (const l of this.listeners) l(this.state);
  }

  setTape(events: FloxlogEvent[]) {
    this.state.tape = events;
    this.recomputeRange();
    this.notify();
  }

  setRun(run: FloxrunData) {
    this.state.run = run;
    this.recomputeRange();
    this.notify();
  }

  private recomputeRange() {
    let lo: bigint | null = null;
    let hi: bigint | null = null;
    for (const ev of this.state.tape) {
      if (lo === null || ev.ts_ns < lo) lo = ev.ts_ns;
      if (hi === null || ev.ts_ns > hi) hi = ev.ts_ns;
    }
    if (this.state.run) {
      const allRun: Array<{ run_ts_ns: bigint }> = [
        ...this.state.run.signals,
        ...this.state.run.orders,
        ...this.state.run.fills,
      ];
      for (const r of allRun) {
        if (lo === null || r.run_ts_ns < lo) lo = r.run_ts_ns;
        if (hi === null || r.run_ts_ns > hi) hi = r.run_ts_ns;
      }
    }
    this.state.ts_min = lo ?? 0n;
    this.state.ts_max = hi ?? 0n;
    if (this.state.cursor_ns < this.state.ts_min) this.state.cursor_ns = this.state.ts_min;
    if (this.state.cursor_ns > this.state.ts_max) this.state.cursor_ns = this.state.ts_max;
  }

  setCursor(ns: bigint) {
    if (ns < this.state.ts_min) ns = this.state.ts_min;
    if (ns > this.state.ts_max) ns = this.state.ts_max;
    this.state.cursor_ns = ns;
    this.notify();
  }

  setSpeed(s: number) {
    this.state.speed = s;
    this.notify();
  }

  play() {
    if (this.state.playing) return;
    this.state.playing = true;
    this.lastTick = performance.now();
    const tick = (now: number) => {
      if (!this.state.playing) return;
      const dt_ms = now - (this.lastTick ?? now);
      this.lastTick = now;
      const dt_ns = BigInt(Math.floor(dt_ms * 1e6 * this.state.speed));
      let next = this.state.cursor_ns + dt_ns;
      if (next >= this.state.ts_max) {
        next = this.state.ts_max;
        this.state.playing = false;
      }
      this.state.cursor_ns = next;
      this.notify();
      if (this.state.playing) this.rafHandle = requestAnimationFrame(tick);
    };
    this.rafHandle = requestAnimationFrame(tick);
    this.notify();
  }

  pause() {
    this.state.playing = false;
    if (this.rafHandle !== null) cancelAnimationFrame(this.rafHandle);
    this.rafHandle = null;
    this.notify();
  }
}

// Slices.

export function tapeSlice(s: ViewerState): FloxlogEvent[] {
  return s.tape.filter((e) => e.ts_ns <= s.cursor_ns);
}

export function signalsSlice(s: ViewerState): Signal[] {
  if (!s.run) return [];
  return s.run.signals.filter((r) => r.run_ts_ns <= s.cursor_ns);
}

export function ordersSlice(s: ViewerState): OrderEvent[] {
  if (!s.run) return [];
  return s.run.orders.filter((r) => r.run_ts_ns <= s.cursor_ns);
}

export function fillsSlice(s: ViewerState): Fill[] {
  if (!s.run) return [];
  return s.run.fills.filter((r) => r.run_ts_ns <= s.cursor_ns);
}

// URL bookmark.

export function readBookmark(): { cursor_ns?: bigint; speed?: number } {
  const params = new URLSearchParams(window.location.hash.slice(1));
  const out: { cursor_ns?: bigint; speed?: number } = {};
  const c = params.get('t');
  if (c) {
    try { out.cursor_ns = BigInt(c); } catch { /* ignore */ }
  }
  const sp = params.get('s');
  if (sp) {
    const n = parseFloat(sp);
    if (Number.isFinite(n)) out.speed = n;
  }
  return out;
}

export function writeBookmark(state: ViewerState) {
  const params = new URLSearchParams();
  params.set('t', state.cursor_ns.toString());
  params.set('s', state.speed.toString());
  const next = '#' + params.toString();
  if (window.location.hash !== next) {
    history.replaceState(null, '', next);
  }
}

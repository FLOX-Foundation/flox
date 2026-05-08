// Parser for .floxrun strategy-trace files.
//
// Layout: RunSegmentHeader(64) + stream of (FrameHeader(12) + record).
// Frame types: 10 = Signal, 11 = OrderEvent, 12 = Fill.
// See docs/spec/floxrun.md.

const MAGIC_RUN = 0x4e555246; // "FRUN"
const PRICE_SCALE = 1e8;
const QTY_SCALE = 1e8;

export interface Signal {
  kind: 'signal';
  run_ts_ns: bigint;
  feed_ts_ns: bigint;
  signal_id: number;
  flags: number;
  strength: number;
  name: string;
  symbol_ids: number[];
  payload: string;
}

export interface OrderEvent {
  kind: 'order';
  run_ts_ns: bigint;
  feed_ts_ns: bigint;
  order_id: bigint;
  parent_signal_id: bigint;
  price: number;
  qty: number;
  symbol_id: number;
  event_kind: number; // 1=submit, 2=cancel, 3=modify, 4=ack, 5=reject, 6=expire
  side: number;
  order_type: number;
  flags: number;
  reason: string;
}

export interface Fill {
  kind: 'fill';
  run_ts_ns: bigint;
  feed_ts_ns: bigint;
  order_id: bigint;
  fill_id: bigint;
  price: number;
  qty: number;
  fee: number;
  symbol_id: number;
  side: number;
  liquidity: number;
}

export type FloxrunRecord = Signal | OrderEvent | Fill;

function readI64(v: DataView, o: number) { return v.getBigInt64(o, true); }
function readU64(v: DataView, o: number) { return v.getBigUint64(o, true); }
function readU32(v: DataView, o: number) { return v.getUint32(o, true); }
function readU16(v: DataView, o: number) { return v.getUint16(o, true); }

function parseRunSegment(buf: ArrayBuffer, kind: number): FloxrunRecord[] {
  const view = new DataView(buf);
  if (buf.byteLength < 64) throw new Error('run segment shorter than header');
  if (readU32(view, 0) !== MAGIC_RUN) throw new Error('bad floxrun magic');
  const out: FloxrunRecord[] = [];
  let cursor = 64;
  const decoder = new TextDecoder('utf-8');
  while (cursor + 12 <= buf.byteLength) {
    const size = readU32(view, cursor);
    const type = view.getUint8(cursor + 8);
    cursor += 12;
    if (cursor + size > buf.byteLength) break;
    const off = cursor;
    cursor += size;
    if (type === 10 && kind === 1 && size >= 48) {
      const name_len = readU16(view, off + 20);
      const sym_count = readU16(view, off + 22);
      const payload_len = readU32(view, off + 24);
      let p = off + 48;
      const name = name_len > 0
        ? decoder.decode(new Uint8Array(buf, p, name_len))
        : '';
      p += name_len;
      const symbol_ids: number[] = [];
      for (let i = 0; i < sym_count; i++, p += 4) {
        symbol_ids.push(readU32(view, p));
      }
      const payload = payload_len > 0
        ? decoder.decode(new Uint8Array(buf, p, payload_len))
        : '';
      out.push({
        kind: 'signal',
        run_ts_ns: readI64(view, off + 0),
        feed_ts_ns: readI64(view, off + 8),
        signal_id: readU32(view, off + 16),
        flags: readU32(view, off + 28),
        strength: Number(readI64(view, off + 32)) / PRICE_SCALE,
        name,
        symbol_ids,
        payload,
      });
    } else if (type === 11 && kind === 2 && size >= 64) {
      const reason_len = readU32(view, off + 56);
      const reason = reason_len > 0
        ? decoder.decode(new Uint8Array(buf, off + 64, reason_len))
        : '';
      out.push({
        kind: 'order',
        run_ts_ns: readI64(view, off + 0),
        feed_ts_ns: readI64(view, off + 8),
        order_id: readU64(view, off + 16),
        parent_signal_id: readU64(view, off + 24),
        price: Number(readI64(view, off + 32)) / PRICE_SCALE,
        qty: Number(readI64(view, off + 40)) / QTY_SCALE,
        symbol_id: readU32(view, off + 48),
        event_kind: view.getUint8(off + 52),
        side: view.getUint8(off + 53),
        order_type: view.getUint8(off + 54),
        flags: readU32(view, off + 60),
        reason,
      });
    } else if (type === 12 && kind === 3 && size >= 64) {
      out.push({
        kind: 'fill',
        run_ts_ns: readI64(view, off + 0),
        feed_ts_ns: readI64(view, off + 8),
        order_id: readU64(view, off + 16),
        fill_id: readU64(view, off + 24),
        price: Number(readI64(view, off + 32)) / PRICE_SCALE,
        qty: Number(readI64(view, off + 40)) / QTY_SCALE,
        fee: Number(readI64(view, off + 48)) / PRICE_SCALE,
        symbol_id: readU32(view, off + 56),
        side: view.getUint8(off + 60),
        liquidity: view.getUint8(off + 61),
      });
    }
  }
  return out;
}

export interface FloxrunManifest {
  schema_version: number;
  format_version: number;
  strategy_id: string;
  strategy_hash: string;
  run_started_ns: number;
  run_ended_ns: number;
  tape_refs: Array<{ path: string; content_hash?: string }>;
  segments: Array<{ name: string; record_kind: 'signals' | 'orders' | 'fills'; event_count: number }>;
}

export interface FloxrunData {
  manifest: FloxrunManifest;
  signals: Signal[];
  orders: OrderEvent[];
  fills: Fill[];
}

const KIND_BY_NAME: Record<string, number> = { signals: 1, orders: 2, fills: 3 };

export async function parseFloxrunDirectory(files: File[]): Promise<FloxrunData> {
  const manifestFile = files.find((f) => f.name === 'manifest.json');
  if (!manifestFile) {
    throw new Error('no manifest.json in dropped files');
  }
  const manifest = JSON.parse(await manifestFile.text()) as FloxrunManifest;
  const data: FloxrunData = { manifest, signals: [], orders: [], fills: [] };
  for (const segMeta of manifest.segments) {
    const file = files.find((f) => f.name === segMeta.name);
    if (!file) continue;
    const buf = await file.arrayBuffer();
    const recs = parseRunSegment(buf, KIND_BY_NAME[segMeta.record_kind] ?? 0);
    for (const r of recs) {
      if (r.kind === 'signal') data.signals.push(r);
      else if (r.kind === 'order') data.orders.push(r);
      else if (r.kind === 'fill') data.fills.push(r);
    }
  }
  data.signals.sort((a, b) => Number(a.run_ts_ns - b.run_ts_ns));
  data.orders.sort((a, b) => Number(a.run_ts_ns - b.run_ts_ns));
  data.fills.sort((a, b) => Number(a.run_ts_ns - b.run_ts_ns));
  return data;
}

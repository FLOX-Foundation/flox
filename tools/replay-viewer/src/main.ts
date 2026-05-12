// Entry point: wires drop-zone -> parsers -> store -> views.

import { parseFloxlogDirectory } from './parsers/floxlog.ts';
import { parseFloxrunDirectory } from './parsers/floxrun.ts';
import { Store, readBookmark, writeBookmark } from './state.ts';
import {
  renderTrades,
  renderOrderbook,
  renderSignals,
  renderOrders,
  renderEquity,
  renderPriceChart,
  renderSymbolLegend,
} from './views/index.ts';

const store = new Store();
const statusEl = document.getElementById('status')!;
const dropzone = document.getElementById('dropzone') as HTMLElement;
const filepicker = document.getElementById('filepicker') as HTMLInputElement;
const controls = document.getElementById('controls') as HTMLElement;
const viewsEl = document.getElementById('views') as HTMLElement;
const playpause = document.getElementById('playpause') as HTMLButtonElement;
const speedSel = document.getElementById('speed') as HTMLSelectElement;
const scrubber = document.getElementById('scrubber') as HTMLInputElement;
const cursorEl = document.getElementById('cursor') as HTMLElement;

function showStatus(msg: string) { statusEl.textContent = msg; }

async function loadFiles(files: File[]) {
  // Group files by their containing directory (webkitRelativePath prefix).
  const groups = new Map<string, File[]>();
  for (const f of files) {
    const path = (f as File & { webkitRelativePath?: string }).webkitRelativePath ?? f.name;
    const dir = path.includes('/') ? path.slice(0, path.lastIndexOf('/')) : '';
    const arr = groups.get(dir) ?? [];
    arr.push(f);
    groups.set(dir, arr);
  }
  const messages: string[] = [];
  for (const [dirName, group] of groups) {
    const m = group.find((f) => f.name === 'manifest.json');
    if (m) {
      const json = JSON.parse(await m.text());
      if (Array.isArray(json.segments) && json.segments.some((s: { record_kind?: string }) => s.record_kind)) {
        const run = await parseFloxrunDirectory(group);
        store.setRun(run);
        messages.push(
          `${dirName || '.floxrun'}: ${run.signals.length} signals, ${run.orders.length} orders, ${run.fills.length} fills`,
        );
        continue;
      }
      if (Array.isArray(json.segments)) {
        const tape = await parseFloxlogDirectory(group);
        store.setTape(tape.events);
        store.setSymbols(tape.symbols);
        messages.push(`${dirName || '.floxlog'}: ${tape.events.length} events`);
        continue;
      }
    }
    // No manifest. Treat the group as a legacy bare-segment .floxlog
    // directory if any candidate file carries the FLOX magic.
    try {
      const tape = await parseFloxlogDirectory(group);
      if (tape.events.length > 0) {
        store.setTape(tape.events);
        store.setSymbols(tape.symbols);
        messages.push(`${dirName || 'tape'}: ${tape.events.length} events`);
      }
    } catch (err) {
      messages.push(`${dirName}: parse error: ${(err as Error).message}`);
    }
  }
  if (messages.length === 0) {
    showStatus('No flox artifacts recognized. Drop a .floxlog or .floxrun directory.');
    return;
  }
  showStatus('loaded · ' + messages.join(' · '));
  // Apply URL bookmark cursor if present.
  const bookmark = readBookmark();
  const s = store.get();
  if (bookmark.cursor_ns !== undefined) store.setCursor(bookmark.cursor_ns);
  else store.setCursor(s.ts_min);
  if (bookmark.speed) {
    store.setSpeed(bookmark.speed);
    speedSel.value = String(bookmark.speed);
  }
  controls.hidden = false;
  viewsEl.hidden = false;
  dropzone.hidden = true;
}

dropzone.addEventListener('dragover', (e) => {
  e.preventDefault();
  dropzone.classList.add('dragover');
});
dropzone.addEventListener('dragleave', () => dropzone.classList.remove('dragover'));
dropzone.addEventListener('drop', async (e) => {
  e.preventDefault();
  dropzone.classList.remove('dragover');
  const items = e.dataTransfer?.items;
  const files: File[] = [];
  if (items) {
    for (const item of Array.from(items)) {
      const entry = (item as DataTransferItem & { webkitGetAsEntry?: () => FileSystemEntry | null }).webkitGetAsEntry?.();
      if (entry) await collectEntry(entry, '', files);
      else if (item.kind === 'file') {
        const f = item.getAsFile();
        if (f) files.push(f);
      }
    }
  } else if (e.dataTransfer?.files) {
    for (const f of Array.from(e.dataTransfer.files)) files.push(f);
  }
  if (files.length > 0) await loadFiles(files);
});

async function collectEntry(entry: FileSystemEntry, prefix: string, out: File[]): Promise<void> {
  if (entry.isFile) {
    const file = await new Promise<File>((res, rej) => (entry as FileSystemFileEntry).file(res, rej));
    Object.defineProperty(file, 'webkitRelativePath', { value: prefix + file.name, configurable: true });
    out.push(file);
  } else if (entry.isDirectory) {
    const reader = (entry as FileSystemDirectoryEntry).createReader();
    const entries: FileSystemEntry[] = await new Promise((res, rej) => reader.readEntries(res, rej));
    for (const child of entries) {
      await collectEntry(child, prefix + entry.name + '/', out);
    }
  }
}

filepicker.addEventListener('change', async () => {
  if (filepicker.files) await loadFiles(Array.from(filepicker.files));
});

playpause.addEventListener('click', () => {
  const s = store.get();
  if (s.playing) store.pause(); else store.play();
});

speedSel.addEventListener('change', () => {
  store.setSpeed(parseFloat(speedSel.value));
});

scrubber.addEventListener('input', () => {
  const s = store.get();
  const span = s.ts_max - s.ts_min;
  const frac = parseInt(scrubber.value, 10) / 1000;
  store.setCursor(s.ts_min + BigInt(Math.floor(Number(span) * frac)));
});

let renderScheduled = false;
let lastBookmarkAt = 0;
store.subscribe((s) => {
  playpause.textContent = s.playing ? 'Pause' : 'Play';
  const span = s.ts_max - s.ts_min;
  const cursor_off = s.cursor_ns - s.ts_min;
  if (span > 0n) {
    const frac = Number(cursor_off) / Number(span);
    scrubber.value = String(Math.round(frac * 1000));
  }
  cursorEl.textContent = s.cursor_ns.toString();
  // Throttle URL hash writes. history.replaceState at 60fps is one of
  // the biggest playback-time costs; 4 Hz is plenty for a shareable
  // bookmark and the user gets the final value on pause.
  const now = performance.now();
  if (!s.playing || now - lastBookmarkAt > 250) {
    writeBookmark(s);
    lastBookmarkAt = now;
  }
  if (!renderScheduled) {
    renderScheduled = true;
    requestAnimationFrame(() => {
      renderScheduled = false;
      const cur = store.get();
      renderPriceChart(cur);
      renderSymbolLegend(cur);
      renderTrades(cur);
      renderOrderbook(cur);
      renderSignals(cur);
      renderOrders(cur);
      renderEquity(cur);
      updateOverflowMarkers();
    });
  }
});

function updateOverflowMarkers() {
  for (const view of document.querySelectorAll<HTMLElement>('.view')) {
    const inner = view.querySelector<HTMLElement>(':scope > div:not(.emptystate):not(.chart-tooltip)');
    if (!inner) {
      view.classList.remove('has-overflow');
      continue;
    }
    view.classList.toggle('has-overflow', inner.scrollHeight > inner.clientHeight + 4);
  }
}

// Autoload from a server-staged directory (used by `flox tape view`).
async function tryAutoload() {
  const params = new URLSearchParams(window.location.search);
  const base = params.get('autoload');
  if (!base) return;
  const collected: File[] = [];
  for (const sub of ['tape', 'run']) {
    try {
      const idx = await fetch(`/${base}/${sub}/`);
      if (!idx.ok) continue;
      const html = await idx.text();
      const names: string[] = [];
      for (const m of html.matchAll(/href="([^"?]+?)(?:\?[^"]*)?"/g)) {
        const name = decodeURIComponent(m[1]).replace(/^\.?\//, '').replace(/\/$/, '');
        if (!name || name === '..' || name.startsWith('?')) continue;
        if (name === 'manifest.json' || /\.(bin|floxlog)$/i.test(name)) {
          names.push(name);
        }
      }
      for (const name of names) {
        const r = await fetch(`/${base}/${sub}/${name}`);
        if (!r.ok) continue;
        const buf = await r.arrayBuffer();
        const f = new File([buf], name, { type: 'application/octet-stream' });
        const dir = sub === 'tape' ? 'tape.floxlog' : 'run.floxrun';
        Object.defineProperty(f, 'webkitRelativePath', { value: `${dir}/${name}`, configurable: true });
        collected.push(f);
      }
    } catch { /* skip */ }
  }
  if (collected.length > 0) await loadFiles(collected);
}
tryAutoload();

document.addEventListener('viewer-seek', (e) => {
  const ts = (e as CustomEvent<{ ts: bigint }>).detail?.ts;
  if (typeof ts === 'bigint') store.setCursor(ts);
});

window.addEventListener('hashchange', () => {
  const bookmark = readBookmark();
  if (bookmark.cursor_ns !== undefined) store.setCursor(bookmark.cursor_ns);
  if (bookmark.speed !== undefined) {
    store.setSpeed(bookmark.speed);
    speedSel.value = String(bookmark.speed);
  }
});

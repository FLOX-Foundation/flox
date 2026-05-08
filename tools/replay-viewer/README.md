# flox replay viewer

Single-page UI for inspecting `.floxlog` and `.floxrun` artifacts.

## Build

```bash
npm install
npm run build       # → dist/
```

## Dev

```bash
npm run dev         # vite at http://localhost:5173
```

Drop a `.floxlog` directory, `.floxrun` directory, or both onto the drop zone.

## CLI launch

The recommended way to open the viewer for an existing artifact is the
flox CLI subcommand, which builds the viewer (or uses an installed
copy), serves it, and opens a browser tab pointing at it:

```bash
flox tape view path/to/btc.floxlog
flox tape view path/to/btc.floxlog path/to/btc.floxrun
```

See `docs/how-to/replay-viewer.md` for the full guide.

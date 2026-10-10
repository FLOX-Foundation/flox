# lrvx replay viewer

Single-page UI for inspecting `.lrvx` and `.lrvxrun` artifacts.

## Build

```bash
npm install
npm run build       # → dist/
```

## Dev

```bash
npm run dev         # vite at http://localhost:5173
```

Drop a `.lrvx` directory, `.lrvxrun` directory, or both onto the drop zone.

## CLI launch

The recommended way to open the viewer for an existing artifact is the
lrvx CLI subcommand, which builds the viewer (or uses an installed
copy), serves it, and opens a browser tab pointing at it:

```bash
lrvx tape view path/to/btc.lrvx
lrvx tape view path/to/btc.lrvx path/to/btc.lrvxrun
```

See `docs/how-to/replay-viewer.md` for the full guide.

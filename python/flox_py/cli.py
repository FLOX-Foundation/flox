"""``flox`` CLI — scaffolds new FLOX projects from templates.

Installed as a console_script via [project.scripts] in pyproject.toml,
so `flox new my-strategy` works after `pip install flox-py`.

Usage::

    flox new <project-name>                     # default template (research)
    flox new <project-name> --template=research
    flox new <project-name> --template=live
    flox new <project-name> --template=indicator-library
    flox new <project-name> --here              # scaffold into the current dir
    flox templates                              # list available templates
    flox report <stats.json>                    # render an HTML backtest report
    flox report <stats.json> -o report.html --equity equity.json --trades trades.json

Each template is a directory under ``flox_py/templates/<name>/`` shipped
with the wheel. ``flox new`` copies the directory verbatim, then
substitutes ``__PROJECT_NAME__``, ``__PROJECT_SLUG__``,
``__PROJECT_PREFIX__`` (upper-cased slug — used as a generic env-var
prefix, e.g. ``MYBOT``), and ``__PROJECT_ENV__``
(``<PREFIX>_DATA`` — used by the research template for the CSV path
env var).

Substitution applies to file *contents* AND to file/directory
*names* — the indicator-library template ships its package directory
as ``__PROJECT_SLUG__/`` so it lands at e.g. ``my_indicators/`` for
``flox new my-indicators``.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from importlib import resources
from pathlib import Path
from typing import Any, Iterable, Optional


# ── Template helpers ───────────────────────────────────────────────────


def _slug(name: str) -> str:
    """Lower-snake-case slug from a project name."""
    s = re.sub(r"[^A-Za-z0-9_-]+", "_", name).strip("_")
    return s.lower().replace("-", "_") or "project"


def _templates_root():
    """Return Traversable for the bundled templates directory, or None."""
    try:
        return resources.files("flox_py").joinpath("templates")
    except ModuleNotFoundError:
        return None


def _list_templates() -> list[str]:
    """List bundled template names by walking flox_py/templates/."""
    root = _templates_root()
    if root is None or not root.is_dir():
        return []
    out: list[str] = []
    for entry in root.iterdir():
        if entry.is_dir():
            out.append(entry.name)
    out.sort()
    return out


def _copy_template(template: str, dest: Path, project_name: str) -> int:
    """Copy bundled template to ``dest`` with placeholder substitution.

    Returns 0 on success, non-zero on error (with a printed message).
    """
    root = _templates_root()
    src_root = root.joinpath(template) if root is not None else None
    if src_root is None or not src_root.is_dir():
        avail = ", ".join(_list_templates()) or "(none)"
        print(f"flox new: unknown template '{template}'. "
              f"Available: {avail}",
              file=sys.stderr)
        return 1

    if dest.exists() and any(dest.iterdir()):
        print(f"flox new: destination '{dest}' is not empty. "
              f"Pick a fresh path or remove existing files.",
              file=sys.stderr)
        return 1

    dest.mkdir(parents=True, exist_ok=True)
    slug = _slug(project_name)
    upper = slug.upper()

    def _subst(s: str) -> str:
        return (s.replace("__PROJECT_NAME__", project_name)
                 .replace("__PROJECT_SLUG__", slug)
                 .replace("__PROJECT_PREFIX__", upper)
                 .replace("__PROJECT_ENV__", upper + "_DATA"))

    def _walk(node, target: Path) -> None:
        for child in node.iterdir():
            # Substitute placeholders in file/directory names too — the
            # indicator-library template ships its package directory as
            # `__PROJECT_SLUG__/` which becomes e.g. `my_indicators/`.
            child_target = target / _subst(child.name)
            if child.is_dir():
                child_target.mkdir(exist_ok=True)
                _walk(child, child_target)
            else:
                # Read the template file as text and substitute. Binary
                # files (data, images) shouldn't appear in templates;
                # fall back to a raw copy if decoding fails.
                try:
                    text = child.read_text()
                except UnicodeDecodeError:
                    child_target.write_bytes(child.read_bytes())
                    continue
                child_target.write_text(_subst(text))

    _walk(src_root, dest)
    return 0


# ── Command handlers ───────────────────────────────────────────────────


def cmd_new(args: argparse.Namespace) -> int:
    project_name: str = args.name
    template: str = args.template

    if args.here:
        dest = Path.cwd()
    else:
        dest = Path.cwd() / project_name

    rc = _copy_template(template, dest, project_name)
    if rc != 0:
        return rc

    print(f"created '{project_name}' from template '{template}' at {dest}")
    print()
    print("next steps:")
    print(f"  cd {dest.relative_to(Path.cwd()) if not args.here else '.'}")
    if template == "indicator-library":
        print('  pip install -e ".[dev]"')
        print(f"  pytest")
        print(f"  python examples/use_in_strategy.py")
    else:
        print(f"  pip install -r requirements.txt")
        print(f"  python main.py")
    return 0


def cmd_templates(_args: argparse.Namespace) -> int:
    names = _list_templates()
    if not names:
        print("no templates found (is flox-py installed?)", file=sys.stderr)
        return 1
    for n in names:
        print(n)
    return 0


def cmd_report(args: argparse.Namespace) -> int:
    import json
    from . import report as report_mod

    try:
        with open(args.stats) as f:
            stats = json.load(f)
    except OSError as e:
        print(f"flox report: cannot read stats file '{args.stats}': {e}",
              file=sys.stderr)
        return 1

    equity = None
    if args.equity:
        try:
            with open(args.equity) as f:
                equity = json.load(f)
        except OSError as e:
            print(f"flox report: cannot read equity file: {e}", file=sys.stderr)
            return 1

    trades = None
    if args.trades:
        try:
            with open(args.trades) as f:
                trades = json.load(f)
        except OSError as e:
            print(f"flox report: cannot read trades file: {e}", file=sys.stderr)
            return 1

    out = Path(args.output)
    report_mod.write_html(
        out,
        stats=stats,
        equity_curve=equity,
        trades=trades,
        title=args.title or "FLOX backtest report",
        subtitle=args.subtitle or "",
    )
    print(f"wrote {out}")
    return 0


# ── tape command handlers ─────────────────────────────────────────────


_DURATION_UNITS = {"s": 1.0, "m": 60.0, "h": 3600.0, "d": 86400.0}


def _parse_duration(s: str) -> float:
    """`'60s'` → 60.0, `'5m'` → 300.0, `'1h'` → 3600.0. Bare digits = seconds."""
    if not s:
        raise ValueError("empty duration")
    if s[-1].lower() in _DURATION_UNITS:
        return float(s[:-1]) * _DURATION_UNITS[s[-1].lower()]
    return float(s)


def _normalize_ccxt_symbol(sym: str) -> str:
    """Allow either 'BTCUSDT' or 'BTC/USDT' on the CLI; pass through to ccxt
    in its preferred slash form."""
    if "/" in sym:
        return sym
    # Heuristic split for the common quote currencies. Anything else
    # the user can pass with a slash explicitly.
    for quote in ("USDT", "USDC", "BUSD", "USD", "BTC", "ETH"):
        if sym.endswith(quote) and len(sym) > len(quote):
            return f"{sym[: -len(quote)]}/{quote}"
    return sym


def cmd_tape_record(args: argparse.Namespace) -> int:
    import asyncio
    import signal

    from . import tape as tape_mod

    duration = _parse_duration(args.duration) if args.duration else None
    ccxt_sym = _normalize_ccxt_symbol(args.symbol)
    out = Path(args.output).expanduser().resolve()

    print(f"flox tape record: {args.exchange} {ccxt_sym} → {out}")
    if duration is not None:
        print(f"  duration: {args.duration} ({duration:.0f}s)")
    else:
        print("  duration: until SIGINT")

    try:
        from .ccxt import CcxtBroker
    except Exception as exc:  # pragma: no cover — ccxt is an optional dep
        print(f"flox tape record: ccxt-side helpers unavailable: {exc}",
              file=sys.stderr)
        return 1

    # Stamp the venue into metadata.json so MergedTapeReader can key
    # this tape against other captures of the same instrument from a
    # different exchange. Without this, multi-tape merge of CLI output
    # loses every symbol mapping ((exchange="" , name="") collides).
    recorder = tape_mod.make_recorder_hook(
        out, max_segment_mb=args.max_segment_mb,
        exchange_name=args.exchange,
        instrument_type=("perpetual" if ":" in ccxt_sym else "spot"),
    )

    async def _run() -> int:
        broker = CcxtBroker(
            exchange_id=args.exchange,
            sandbox=args.testnet,
        )
        async with broker:
            await broker.add_symbol(ccxt_sym)
            # Persist the (flox-side) symbol mapping into metadata.json
            # so consumers of the tape know what id 1, 2, ... mean.
            # `_ccxt_to_sym` is the broker's local registry: ccxt name
            # (e.g. "BTC/USDT:USDT") → flox SymbolId.
            for ccxt_name, flox_sid in broker._ccxt_to_sym.items():
                # Strip the perp suffix for the metadata `name` field so
                # MergedTapeReader's (exchange, name) key matches across
                # exchanges that label perps differently.
                flat_name = ccxt_name.replace("/", "").split(":")[0]
                recorder.add_symbol(int(flox_sid), flat_name, "", "", 2, 8)
            broker.set_market_data_recorder(recorder)
            run_task = asyncio.create_task(
                broker.run(streams=("trades", "book"),
                           book_depth=args.book_depth, reconcile=False)
            )

            stop_event = asyncio.Event()
            loop = asyncio.get_running_loop()

            def _stop(*_args: Any) -> None:
                stop_event.set()

            for sig in (signal.SIGINT, signal.SIGTERM):
                try:
                    loop.add_signal_handler(sig, _stop)
                except (NotImplementedError, RuntimeError):
                    pass  # Windows / non-main thread

            try:
                if duration is not None:
                    await asyncio.wait_for(stop_event.wait(), timeout=duration)
                else:
                    await stop_event.wait()
            except asyncio.TimeoutError:
                pass
            finally:
                run_task.cancel()
                try:
                    await run_task
                except (asyncio.CancelledError, Exception):
                    pass
                recorder.close()

        s = recorder.stats()
        print(f"  trades written      : {s.get('trades_written', 0)}")
        print(f"  book updates written: {s.get('book_updates_written', 0)}")
        if s.get("errors"):
            print(f"  ERROR               : {s['errors']} write failure(s)",
                  file=sys.stderr)
            return 1
        return 0

    return asyncio.run(_run())


def cmd_bundle_pack(args: argparse.Namespace) -> int:
    from . import bundle as bundle_mod

    cfg = {
        "slippage_model": args.slippage_model,
        "slippage_params": {"bps": float(args.slippage_bps)} if args.slippage_model == "fixed_bps" else {},
    }
    out = bundle_mod.pack_bundle(
        strategy=Path(args.strategy),
        tape=Path(args.tape),
        output=Path(args.output),
        config=cfg,
        symbol_name=args.symbol_name,
        exchange=args.exchange,
        tick_size=float(args.tick_size),
    )
    print(f"flox bundle pack: wrote {out}")
    return 0


def cmd_bundle_replay(args: argparse.Namespace) -> int:
    from . import bundle as bundle_mod

    res = bundle_mod.replay_bundle(Path(args.path))
    print(f"flox bundle replay: {args.path}")
    print(f"  flox_version:           {res.manifest.get('flox_version')}")
    print(f"  bundle_format_version:  {res.manifest.get('bundle_format_version')}")
    print(f"  trade_count actual:     {res.actual['trade_count']}")
    print(f"  fill_count actual:      {res.actual['fill_count']}")
    print(f"  total_filled actual:    {res.actual['total_filled_quantity']}")
    print(f"  trade_count expected:   {res.expected['trade_count']}")
    print(f"  fill_count expected:    {res.expected['fill_count']}")
    return 0


def cmd_bundle_validate(args: argparse.Namespace) -> int:
    from . import bundle as bundle_mod

    res = bundle_mod.validate_bundle(Path(args.path))
    if res.matches:
        print(f"flox bundle validate: OK ({args.path}) — actual matches expected")
        return 0
    print(f"flox bundle validate: DIVERGENCE ({args.path})", file=sys.stderr)
    for d in res.diff:
        print(f"  {d}", file=sys.stderr)
    return 1


def cmd_tape_diff(args: argparse.Namespace) -> int:
    from . import tape as tape_mod

    left = Path(args.left).expanduser()
    right = Path(args.right).expanduser()
    if not left.is_dir() or not right.is_dir():
        print(f"flox tape diff: both paths must be .floxlog directories",
              file=sys.stderr)
        return 2
    diff = tape_mod.diff_tapes(
        left, right,
        max_mismatches=int(args.max_mismatches),
        field_tolerance_ns=int(args.ts_tolerance_ns),
    )
    if args.json:
        import json as _json
        print(_json.dumps(diff.to_dict(), indent=2, sort_keys=True))
    else:
        if diff.equal:
            print(f"flox tape diff: EQUAL "
                  f"({diff.left_count} trade(s) on each side)")
        else:
            print(f"flox tape diff: DIVERGENCE")
            print(f"  left:  {diff.left_path}  trades={diff.left_count}")
            print(f"  right: {diff.right_path}  trades={diff.right_count}")
            print(f"  first_divergence_index: {diff.first_divergence_index}")
            for m in diff.mismatches[:10]:
                print(f"  [{m['index']}] left={m['left']} right={m['right']}")
            if len(diff.mismatches) > 10:
                print(f"  ... {len(diff.mismatches) - 10} more recorded mismatches")
    return 0 if diff.equal else 1


def cmd_lint_lookahead(args: argparse.Namespace) -> int:
    from . import lookahead as lookahead_mod

    p = Path(args.path).expanduser()
    if not p.is_file():
        print(f"flox lint lookahead: file not found: {p}", file=sys.stderr)
        return 2
    report = lookahead_mod.analyze_path(p)
    if args.json:
        import json as _json
        print(_json.dumps(report.to_dict(), indent=2, sort_keys=True))
    else:
        if report.ok:
            print(f"flox lint lookahead: OK ({p}); no patterns flagged")
        else:
            print(f"flox lint lookahead: {len(report.findings)} finding(s) in {p}")
            for f in report.findings:
                print(f"  {f.line}:{f.col} [{f.rule}] {f.message}")
                if f.snippet:
                    print(f"      {f.snippet}")
    return 0 if report.ok else 1


def cmd_tape_inspect(args: argparse.Namespace) -> int:
    from . import tape as tape_mod

    p = Path(args.path).expanduser().resolve()
    if not p.exists():
        print(f"flox tape inspect: path not found: {p}", file=sys.stderr)
        return 1
    stats = tape_mod.inspect_tape(p)
    print(f"flox tape inspect: {stats.path}")
    print(f"  trades       : {stats.trade_count}")
    if stats.trade_count > 0:
        span_ns = stats.last_ts_ns - stats.first_ts_ns
        print(f"  first ts (ns): {stats.first_ts_ns}")
        print(f"  last  ts (ns): {stats.last_ts_ns}")
        print(f"  span         : {span_ns / 1e9:.3f}s")
        print(f"  symbols      : {stats.symbol_ids}")
    return 0


def cmd_tape_replay(args: argparse.Namespace) -> int:
    from . import tape as tape_mod

    p = Path(args.path).expanduser().resolve()
    if not p.exists():
        print(f"flox tape replay: path not found: {p}", file=sys.stderr)
        return 1

    counter = [0]
    limit = args.limit

    def _on_trade(ts_ns: int, sym_id: int, price: float,
                  qty: float, side: int) -> None:
        counter[0] += 1
        if limit is not None and counter[0] >= limit:
            raise StopIteration  # stops the loop cleanly via inner try

    try:
        n = tape_mod.replay_tape(p, on_trade=_on_trade)
    except StopIteration:
        n = counter[0]
    print(f"flox tape replay: dispatched {counter[0]} trade(s) "
          f"from {p} (tape contains {n} total)")
    return 0


def _materialize_merged_tape(paths: list[Path], out_dir: Path) -> None:
    """Write a synthetic `.floxlog` directory at `out_dir` containing
    the merged event stream from `paths`. Symbols are rekeyed to global
    IDs; metadata.json carries the merged symbol table so the viewer can
    label them.

    MVP for `flox tape view path1 path2 ...`. The streaming HTTP `/merge`
    endpoint that the spec preferred lives behind SPA-side multi-tape
    support — until that lands this materialization gives the viewer a
    file it already knows how to render.
    """
    import json
    import flox_py
    import numpy as np

    out_dir.mkdir(parents=True, exist_ok=True)
    reader = flox_py.MergedTapeReader([str(p) for p in paths])

    writer = flox_py.DataWriter(str(out_dir), max_segment_mb=256,
                                exchange_id=0, compression="none")

    # Trades: convert raw int64 → double for the existing
    # DataWriter.write_trade signature. Acceptable precision loss for
    # the viewer use case (prices have plenty of headroom under 1e15).
    for row in reader.read_trades():
        writer.write_trade(
            exchange_ts_ns=int(row["exchange_ts_ns"]),
            recv_ts_ns=int(row["recv_ts_ns"]),
            price=float(row["price_raw"]) / 1e8,
            qty=float(row["qty_raw"]) / 1e8,
            trade_id=int(row["trade_id"]),
            symbol_id=int(row["symbol_id"]),
            side=int(row["side"]),
        )

    # Books: write_book takes raw int64 directly, lossless round-trip.
    headers, levels = reader.read_books()
    if headers.size:
        for h in headers:
            offset = int(h["level_offset"])
            n_bid = int(h["bid_count"])
            n_ask = int(h["ask_count"])
            bids = levels[offset:offset + n_bid]
            asks = levels[offset + n_bid:offset + n_bid + n_ask]
            writer.write_book(
                exchange_ts_ns=int(h["exchange_ts_ns"]),
                recv_ts_ns=int(h["recv_ts_ns"]),
                seq=int(h["seq"]),
                symbol_id=int(h["symbol_id"]),
                is_snapshot=int(h["event_type"]) == 2,
                bids=bids,
                asks=asks,
            )
    writer.close()

    # Synthesize a metadata.json that maps the global symbol IDs back to
    # readable (exchange, name) tuples. The viewer reads this for chart
    # legends / tooltips.
    sym_table = reader.symbol_table()
    metadata = {
        "exchange": "merged",
        "exchange_type": "synthetic",
        "instrument_type": "merged",
        "symbols": [
            {
                "symbol_id": int(s["global_id"]),
                "name": f"{s['exchange']}/{s['name']}",
                "base_asset": "",
                "quote_asset": "",
                "price_precision": int(s["price_precision"]),
                "qty_precision": int(s["qty_precision"]),
            }
            for s in sym_table
        ],
        "has_trades": True,
        "has_book_snapshots": bool(headers.size),
        "has_book_deltas": False,
        "price_scale": 100000000,
        "qty_scale": 100000000,
        "description": (
            "Merged view of: " + ", ".join(str(p) for p in paths)
        ),
    }
    (out_dir / "metadata.json").write_text(json.dumps(metadata, indent=2))


def cmd_tape_view(args: argparse.Namespace) -> int:
    """Serve the static replay viewer locally with the requested tape /
    run directory pre-staged, then open a browser."""
    import http.server
    import socketserver
    import threading
    import webbrowser
    import shutil as _shutil

    pkg_dir = Path(__file__).resolve().parent
    # Look up the bundled viewer SPA. It is shipped under
    # `flox_py/data/replay-viewer/dist/` for installed wheels and falls
    # back to the in-repo source build during development.
    candidates = [
        pkg_dir / "data" / "replay-viewer",
        pkg_dir.parent.parent / "tools" / "replay-viewer" / "dist",
    ]
    viewer_root = next((c for c in candidates if c.exists()), None)
    if viewer_root is None:
        print("flox tape view: viewer build not found. Run "
              "`cd tools/replay-viewer && npm install && npm run build` "
              "first, or install a published wheel.", file=sys.stderr)
        return 1

    tape_paths: list[Path] = []
    run_path: Optional[Path] = None
    for raw in args.paths:
        p = Path(raw).expanduser().resolve()
        if not p.exists():
            print(f"flox tape view: path not found: {p}", file=sys.stderr)
            return 1
        suffix = p.suffix.lower()
        if suffix == ".floxlog" or any(p.glob("*.floxlog")):
            tape_paths.append(p)
        elif suffix == ".floxrun":
            run_path = p
        else:
            print(f"flox tape view: cannot tell artifact kind from {p}",
                  file=sys.stderr)
            return 1

    # Stage a copy of the artifacts inside a temp serve root so the
    # viewer can fetch them without same-origin gymnastics.
    import tempfile
    work = Path(tempfile.mkdtemp(prefix="flox-view-"))
    serve_root = work / "site"
    _shutil.copytree(viewer_root, serve_root)
    fixtures = serve_root / "fixture-cli"
    fixtures.mkdir()

    if len(tape_paths) == 1:
        _shutil.copytree(tape_paths[0], fixtures / "tape")
    elif len(tape_paths) >= 2:
        # Multi-tape view — materialize a merged synthetic tape into
        # fixtures/tape so the existing single-tape SPA renders the
        # union. The merged tape carries rekeyed (global) symbol IDs.
        # SPA-side multi-venue legend / per-venue book chart is a
        # separate follow-up; for now the viewer sees one synthetic
        # stream with N distinct symbol IDs and colours them
        # accordingly.
        _materialize_merged_tape(tape_paths, fixtures / "tape")
    if run_path is not None:
        _shutil.copytree(run_path, fixtures / "run")

    port = args.port

    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *_a, **_kw):  # quiet
            return

    Handler.directory = str(serve_root)  # type: ignore[attr-defined]

    httpd = socketserver.TCPServer(("127.0.0.1", port),
                                    lambda *a, **kw: Handler(*a,
                                                              directory=str(serve_root),
                                                              **kw))
    url = f"http://127.0.0.1:{port}/"
    if tape_paths or run_path is not None:
        url += "?autoload=fixture-cli"

    def _serve() -> None:
        httpd.serve_forever()

    t = threading.Thread(target=_serve, daemon=True)
    t.start()
    print(f"flox tape view: serving {serve_root} at {url}")
    if not args.no_open:
        webbrowser.open(url)
    print("ctrl+c to stop")
    try:
        t.join()
    except KeyboardInterrupt:
        httpd.shutdown()
    return 0


# ── Argparse setup ─────────────────────────────────────────────────────


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="flox",
        description="FLOX project scaffolding & utilities.",
    )
    sub = p.add_subparsers(dest="command", required=True)

    p_new = sub.add_parser("new", help="Scaffold a new FLOX project.")
    p_new.add_argument("name", help="project name (used as directory & slug)")
    p_new.add_argument(
        "--template", default="research",
        help="template to use (default: research). "
             "Run `flox templates` to list.",
    )
    p_new.add_argument(
        "--here", action="store_true",
        help="scaffold into the current directory instead of "
             "creating a new one",
    )
    p_new.set_defaults(handler=cmd_new)

    p_t = sub.add_parser("templates", help="List available templates.")
    p_t.set_defaults(handler=cmd_templates)

    p_r = sub.add_parser("report", help="Render a backtest stats JSON to HTML.")
    p_r.add_argument("stats", help="path to a JSON file with backtest stats")
    p_r.add_argument(
        "-o", "--output", default="report.html",
        help="output HTML path (default: report.html)",
    )
    p_r.add_argument(
        "--equity",
        help="optional path to a JSON dump of the equity curve "
             "(keys: timestamp_ns, equity, drawdown_pct)",
    )
    p_r.add_argument(
        "--trades",
        help="optional path to a JSON dump of trades "
             "(keys: symbol, side, entry_price, exit_price, quantity, pnl, "
             "fee, entry_time_ns, exit_time_ns)",
    )
    p_r.add_argument("--title", default=None, help="report title")
    p_r.add_argument("--subtitle", default=None, help="subtitle")
    p_r.set_defaults(handler=cmd_report)

    # ── engine ─────────────────────────────────────────────────────
    from . import engine_cli
    engine_cli.add_engine_subparser(sub)

    # ── tape ───────────────────────────────────────────────────────
    p_tape = sub.add_parser(
        "tape",
        help="Record / replay / inspect deterministic .floxlog tapes.",
    )
    tape_sub = p_tape.add_subparsers(dest="tape_command", required=True)

    p_rec = tape_sub.add_parser(
        "record",
        help="Stream a CCXT exchange feed into a .floxlog directory.",
    )
    p_rec.add_argument("exchange",
                       help="CCXT exchange id (e.g. 'bybit', 'bitget').")
    p_rec.add_argument("symbol",
                       help="Symbol in CCXT spelling (e.g. 'BTC/USDT' or 'BTCUSDT').")
    p_rec.add_argument(
        "--duration", default=None,
        help="Stop after this much wall time (e.g. '60s', '5m', '1h'). "
             "Default: run until SIGINT.",
    )
    p_rec.add_argument(
        "--output", "-o", required=True,
        help="Output directory for the .floxlog segments + metadata.",
    )
    p_rec.add_argument(
        "--max-segment-mb", type=int, default=256,
        help="Rotate to a new segment after this many MB (default: 256).",
    )
    p_rec.add_argument(
        "--testnet", action="store_true",
        help="Use the exchange's testnet/sandbox endpoint.",
    )
    p_rec.add_argument(
        "--book-depth", type=int, default=50,
        help=("Order-book depth subscribed via watchOrderBook. Exchanges have "
              "different valid ranges: bitget perp accepts 20, bybit spot "
              "wants 1/50/200/1000. Default 50 is the safe cross-exchange "
              "value."),
    )
    p_rec.set_defaults(handler=cmd_tape_record)

    p_rep = tape_sub.add_parser(
        "replay",
        help="Replay a recorded tape — by default print summary stats.",
    )
    p_rep.add_argument("path", help="Path to the .floxlog directory.")
    p_rep.add_argument(
        "--limit", type=int, default=None,
        help="Stop after dispatching this many trades (debugging aid).",
    )
    p_rep.set_defaults(handler=cmd_tape_replay)

    p_ins = tape_sub.add_parser(
        "inspect",
        help="Print summary stats (event count, time range, symbols) for a tape.",
    )
    p_ins.add_argument("path", help="Path to the .floxlog directory.")
    p_ins.set_defaults(handler=cmd_tape_inspect)

    p_view = tape_sub.add_parser(
        "view",
        help="Open the replay viewer in a browser, pre-loaded with the "
             "given .floxlog and / or .floxrun directory.",
    )
    p_view.add_argument("paths", nargs="+",
                        help="One or more .floxlog directories and/or one "
                             ".floxrun. With multiple .floxlog dirs the "
                             "viewer renders a merged synthetic tape "
                             "(symbols rekeyed into a global id space "
                             "keyed by (exchange, name) — see "
                             "docs/how-to/multi-tape.md).")
    p_view.add_argument("--port", type=int, default=8765,
                        help="Local port to serve the viewer on (default 8765).")
    p_view.add_argument("--no-open", action="store_true",
                        help="Do not auto-open a browser tab; just print the URL.")
    p_view.set_defaults(handler=cmd_tape_view)

    p_diff = tape_sub.add_parser(
        "diff",
        help="Compare two .floxlog directories trade-by-trade. "
             "Exits 0 when equal, 1 when tapes diverge.",
    )
    p_diff.add_argument("left", help="Path to the first .floxlog directory.")
    p_diff.add_argument("right", help="Path to the second .floxlog directory.")
    p_diff.add_argument(
        "--max-mismatches", type=int, default=16,
        help="Stop recording per-trade mismatches after this many. Default 16.",
    )
    p_diff.add_argument(
        "--ts-tolerance-ns", type=int, default=0,
        help="Allow this much jitter on exchange_ts_ns when comparing. Default 0.",
    )
    p_diff.add_argument(
        "--json", action="store_true",
        help="Emit the diff as JSON instead of human-readable lines.",
    )
    p_diff.set_defaults(handler=cmd_tape_diff)

    # ── bundle ─────────────────────────────────────────────────────
    p_bundle = sub.add_parser(
        "bundle",
        help="Pack / replay / validate a reproducibility bundle "
             "(strategy + tape + expected output).",
    )
    bundle_sub = p_bundle.add_subparsers(dest="bundle_command", required=True)

    p_pack = bundle_sub.add_parser(
        "pack",
        help="Build a bundle from a strategy file + tape directory.",
    )
    p_pack.add_argument(
        "--strategy", required=True,
        help="Path to a .py file containing one flox.Strategy subclass.",
    )
    p_pack.add_argument(
        "--tape", required=True,
        help="Path to a .floxlog directory the strategy will replay against.",
    )
    p_pack.add_argument(
        "--output", "-o", required=True,
        help="Bundle output path (.tar).",
    )
    p_pack.add_argument(
        "--symbol-name", default="BTCUSDT",
        help="Symbol name to register (default: BTCUSDT).",
    )
    p_pack.add_argument(
        "--exchange", default="bundle",
        help="Exchange tag for the symbol (default: bundle).",
    )
    p_pack.add_argument(
        "--tick-size", type=float, default=0.01,
        help="Tick size for the symbol (default: 0.01).",
    )
    p_pack.add_argument(
        "--slippage-model", default="none",
        choices=("none", "fixed_ticks", "fixed_bps", "volume_impact"),
        help="Slippage model for the simulator (default: none).",
    )
    p_pack.add_argument(
        "--slippage-bps", type=float, default=0.0,
        help="Slippage in basis points (used when slippage-model=fixed_bps).",
    )
    p_pack.set_defaults(handler=cmd_bundle_pack)

    p_replay = bundle_sub.add_parser(
        "replay",
        help="Extract a bundle, replay the strategy, print actual vs expected.",
    )
    p_replay.add_argument("path", help="Path to the bundle .tar file.")
    p_replay.set_defaults(handler=cmd_bundle_replay)

    p_validate = bundle_sub.add_parser(
        "validate",
        help="Replay a bundle and assert the output is byte-equal to expected. "
             "Exit 0 on match, 1 on divergence (with diff printed).",
    )
    p_validate.add_argument("path", help="Path to the bundle .tar file.")
    p_validate.set_defaults(handler=cmd_bundle_validate)

    # ── lint (lookahead) ────────────────────────────────────────────
    p_lint = sub.add_parser(
        "lint",
        help="Static-analysis lints over strategy code.",
    )
    lint_sub = p_lint.add_subparsers(dest="lint_command", required=True)

    p_lookahead = lint_sub.add_parser(
        "lookahead",
        help="Detect common lookahead-bias patterns "
             "(.shift(-N), forward index arithmetic, open-upper "
             "slices in per-bar callbacks, future-named attributes).",
    )
    p_lookahead.add_argument("path", help="Path to the strategy .py file.")
    p_lookahead.add_argument(
        "--json", action="store_true",
        help="Emit findings as JSON instead of human-readable lines.",
    )
    p_lookahead.set_defaults(handler=cmd_lint_lookahead)

    return p


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)
    return args.handler(args)


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())

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

    recorder = tape_mod.make_recorder_hook(
        out, max_segment_mb=args.max_segment_mb,
    )

    async def _run() -> int:
        broker = CcxtBroker(
            exchange_id=args.exchange,
            sandbox=args.testnet,
        )
        async with broker:
            await broker.add_symbol(ccxt_sym)
            broker.set_market_data_recorder(recorder)
            run_task = asyncio.create_task(
                broker.run(streams=("trades",), reconcile=False)
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

        s = recorder.stats
        print(f"  trades written : {s.trades_written}")
        if s.book_updates_skipped:
            print(f"  book updates   : {s.book_updates_skipped} skipped "
                  f"(book write API not yet exposed; trades only in v1)")
        if s.error:
            print(f"  ERROR          : {s.error}", file=sys.stderr)
            return 1
        return 0

    return asyncio.run(_run())


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

    return p


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)
    return args.handler(args)


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())

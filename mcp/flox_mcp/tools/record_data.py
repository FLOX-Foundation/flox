"""record_data — MCP wrapper around the canonical recording paths.

This is not a parallel recorder. It shells out to the existing
canonical paths:

- `mode="historical"` → `scripts/backfill_to_tape.py` (ccxt
  `fetch_ohlcv` / `fetch_trades` writing into a `.floxlog`).
- `mode="live"` → `flox tape record` CLI from `flox-py`.

Decision was: keep ccxt out of the MCP server itself; only pull it
in when the user actually invokes a recording. The `backfill_to_tape.py`
script imports ccxt only when run.
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Optional


_REPO_ROOT = Path(__file__).resolve().parents[3]
_BACKFILL_SCRIPT = _REPO_ROOT / "scripts" / "backfill_to_tape.py"

SUPPORTED_MODES = ("historical", "live")
SUPPORTED_TYPES = ("trades", "klines")


def record_data(
    mode: str,
    exchange: str,
    symbol: str,
    out_path: str,
    *,
    data_type: str = "klines",
    from_dt: Optional[str] = None,
    to_dt: Optional[str] = None,
    duration: Optional[str] = None,
    max_records: int = 1_000_000,
) -> str:
    if mode not in SUPPORTED_MODES:
        return (
            f"record_data: unsupported mode {mode!r}. "
            f"Supported: {', '.join(SUPPORTED_MODES)}."
        )
    if data_type not in SUPPORTED_TYPES:
        return (
            f"record_data: unsupported data_type {data_type!r}. "
            f"Supported: {', '.join(SUPPORTED_TYPES)}."
        )
    if not exchange or not symbol or not out_path:
        return "record_data: `exchange`, `symbol`, and `out_path` are required."

    if mode == "historical":
        if not from_dt or not to_dt:
            return ("record_data: historical mode requires both `from_dt` "
                    "and `to_dt` (ISO date or unix-ms).")
        return _run_historical(exchange, symbol, data_type, from_dt, to_dt,
                               out_path, max_records)
    return _run_live(exchange, symbol, out_path, duration)


def _run_historical(exchange: str, symbol: str, data_type: str,
                    from_dt: str, to_dt: str, out_path: str,
                    max_records: int) -> str:
    if not _BACKFILL_SCRIPT.is_file():
        return (
            f"record_data: backfill script missing at {_BACKFILL_SCRIPT}. "
            f"This is a packaging bug — flox-mcp ships with the script."
        )
    cmd = [
        sys.executable, str(_BACKFILL_SCRIPT),
        "--exchange", exchange,
        "--symbol", symbol,
        "--type", data_type,
        "--from", from_dt,
        "--to", to_dt,
        "--out", out_path,
        "--max-records", str(int(max_records)),
    ]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    except subprocess.TimeoutExpired:
        return ("record_data: backfill exceeded the 15-minute wall-clock "
                "limit. Narrow the time range or split into multiple calls.")

    out = (proc.stdout or "").strip()
    err = (proc.stderr or "").strip()
    payload: dict = {}
    if out:
        try:
            payload = json.loads(out.splitlines()[-1])
        except Exception:
            payload = {"raw_stdout": out}

    lines = [
        f"# record_data: historical / {exchange} / {symbol} / {data_type}",
        "",
    ]
    if proc.returncode != 0:
        lines += [
            f"`backfill_to_tape.py` exited {proc.returncode}.",
            "",
            "## Result",
            "```json",
            json.dumps(payload, indent=2)[:2000],
            "```",
        ]
        if err:
            lines += ["", "## stderr", "```", err[-1000:], "```"]
        return "\n".join(lines)

    lines += [
        "## Result",
        "```json",
        json.dumps(payload, indent=2),
        "```",
        "",
        "## Next steps",
        "",
        f'- `docs_search("backtest")` — drive a strategy off `{out_path}`',
        f'- `docs_search("record tape")` — extend the tape with more data',
    ]
    return "\n".join(lines)


def _run_live(exchange: str, symbol: str, out_path: str,
              duration: Optional[str]) -> str:
    flox_cli = shutil.which("flox")
    if flox_cli is None:
        return ("record_data: `flox` CLI is not on PATH. Install with "
                "`pip install flox-py`.")

    cmd = [flox_cli, "tape", "record", exchange, symbol, "--output", out_path]
    if duration:
        cmd += ["--duration", duration]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=86_400)
    except subprocess.TimeoutExpired:
        return ("record_data: live recording hit the 24-hour wall-clock "
                "ceiling on the worker. Use --duration explicitly for "
                "longer runs and supervise with the CLI directly.")

    out = (proc.stdout or "").strip()
    err = (proc.stderr or "").strip()
    lines = [
        f"# record_data: live / {exchange} / {symbol}",
        "",
    ]
    if proc.returncode != 0:
        lines += [
            f"`flox tape record` exited {proc.returncode}.",
            "",
            "## stderr",
            "```",
            err[-1500:] or "(empty)",
            "```",
        ]
        return "\n".join(lines)

    lines += [
        "## CLI output",
        "```",
        out[-1500:] or "(empty)",
        "```",
        "",
        "## Next steps",
        "",
        f'- `docs_search("backtest")` — drive a strategy off `{out_path}`',
    ]
    return "\n".join(lines)

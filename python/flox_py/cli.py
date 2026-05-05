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
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from importlib import resources
from pathlib import Path
from typing import Iterable, Optional


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

    def _walk(node, target: Path) -> None:
        for child in node.iterdir():
            child_target = target / child.name
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
                upper = slug.upper()
                text = text.replace("__PROJECT_NAME__", project_name)
                text = text.replace("__PROJECT_SLUG__", slug)
                text = text.replace("__PROJECT_PREFIX__", upper)
                text = text.replace("__PROJECT_ENV__", upper + "_DATA")
                child_target.write_text(text)

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

    return p


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(list(argv) if argv is not None else None)
    return args.handler(args)


if __name__ == "__main__":  # pragma: no cover
    sys.exit(main())

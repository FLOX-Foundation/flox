"""Command-line entry: `python -m flox_codegen.cli <subcommand>`.

Subcommands:

    emit-capi      Generate the C header from the spec.
    check          Diff codegen output vs the live flox_capi.h.
    extract        Print IR as JSON (debugging).
"""
from __future__ import annotations

import argparse
import dataclasses
import json
import sys
from pathlib import Path

from . import check_signatures, emit_capi, extractor


def _cmd_emit_capi(args: argparse.Namespace) -> int:
    module = extractor.parse_spec(Path(args.spec))
    # Anchor clang-format on the repo's .clang-format by passing a path
    # adjacent to it as the assume-filename hint. clang-format walks up from
    # there to find the style config.
    style_anchor = Path(args.spec).resolve()
    text = emit_capi.emit(
        module, format=not args.no_format, style_file=style_anchor
    )
    if args.out == "-":
        sys.stdout.write(text)
    else:
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(text)
        print(f"wrote {out} ({len(module.functions)} functions, "
              f"{len(module.handles)} handles, {len(module.structs)} structs)")
    return 0


def _cmd_check(args: argparse.Namespace) -> int:
    include_dirs = [Path(d) for d in (args.include_dir or [])]
    mismatches, missing, extra = check_signatures.check(
        expected_header=Path(args.expected),
        actual_header=Path(args.actual),
        include_dirs=include_dirs,
        require_full_coverage=args.require_full_coverage,
    )

    rc = 0
    if mismatches:
        rc = 1
        print("SIGNATURE MISMATCHES:", file=sys.stderr)
        for m in mismatches:
            print(f"  {m.name}: {m.reason} — {m.detail}", file=sys.stderr)

    if missing:
        if args.require_full_coverage:
            rc = 1
            print("MISSING FROM CODEGEN (full coverage required):", file=sys.stderr)
        else:
            print("missing from codegen (informational; coverage gap):",
                  file=sys.stderr)
        for n in missing:
            print(f"  {n}", file=sys.stderr)

    if extra:
        print("extra in codegen (informational):", file=sys.stderr)
        for n in extra:
            print(f"  {n}", file=sys.stderr)

    if rc == 0:
        # On success, summarize for human-friendly CI logs.
        total = len(mismatches) + len(missing) + len(extra)
        if total == 0:
            print("OK: signatures match exactly.")
        else:
            print(f"OK: {len(mismatches)} mismatches, "
                  f"{len(missing)} missing (informational), "
                  f"{len(extra)} extra.")
    return rc


def _cmd_extract(args: argparse.Namespace) -> int:
    module = extractor.parse_spec(Path(args.spec))

    def to_jsonable(obj):
        if dataclasses.is_dataclass(obj):
            return {k: to_jsonable(v) for k, v in dataclasses.asdict(obj).items()}
        if isinstance(obj, (list, tuple)):
            return [to_jsonable(x) for x in obj]
        if isinstance(obj, dict):
            return {k: to_jsonable(v) for k, v in obj.items()}
        return obj

    json.dump(to_jsonable(module), sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(prog="flox_codegen")
    sub = p.add_subparsers(dest="cmd", required=True)

    pe = sub.add_parser("emit-capi", help="Generate flox_capi.h from spec.")
    pe.add_argument("--spec", required=True,
                    help="Path to flox_capi_spec.hpp")
    pe.add_argument("--out", required=True,
                    help="Output path (use '-' for stdout)")
    pe.add_argument("--no-format", action="store_true",
                    help="Skip the clang-format post-pass (debugging only).")
    pe.set_defaults(func=_cmd_emit_capi)

    pc = sub.add_parser("check", help="Diff signatures across two C headers.")
    pc.add_argument("--expected", required=True,
                    help="Reference header (e.g. live flox_capi.h)")
    pc.add_argument("--actual", required=True,
                    help="Generated header (e.g. tools/codegen/golden/...)")
    pc.add_argument("-I", "--include-dir", action="append",
                    help="Pass-through -I dir (repeatable)")
    pc.add_argument("--require-full-coverage", action="store_true",
                    help=("Fail when expected has functions absent from actual. "
                          "Off by default during prototype; turned on in T014."))
    pc.set_defaults(func=_cmd_check)

    px = sub.add_parser("extract", help="Print IR as JSON (debug).")
    px.add_argument("--spec", required=True)
    px.set_defaults(func=_cmd_extract)

    return p


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

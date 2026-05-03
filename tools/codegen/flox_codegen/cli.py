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

from . import abi_snapshot, check_signatures, emit_capi, emit_codon, emit_llms, extractor


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


def _emit_to_path(text: str, out: str, kind: str, count: int) -> None:
    if out == "-":
        sys.stdout.write(text)
    else:
        out_path = Path(out)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(text)
        print(f"wrote {out_path} ({count} functions, {kind})")


def _cmd_emit_codon(args: argparse.Namespace) -> int:
    module = extractor.parse_spec(Path(args.spec))
    text = emit_codon.emit(module)
    _emit_to_path(text, args.out, "codon FFI", len(module.functions))
    return 0


def _cmd_emit_llms(args: argparse.Namespace) -> int:
    module = extractor.parse_spec(Path(args.spec))
    text = emit_llms.emit(module)
    _emit_to_path(text, args.out, "llms reference", len(module.functions))
    return 0


def _cmd_abi_snapshot(args: argparse.Namespace) -> int:
    """Read a C header, write its ABI snapshot to a file (or stdout)."""
    sigs = abi_snapshot.from_header(Path(args.header))
    text = abi_snapshot.render(sigs)
    if args.out == "-":
        sys.stdout.write(text)
    else:
        out_path = Path(args.out)
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(text)
        print(f"wrote {out_path} ({len(sigs)} functions)")
    return 0


def _cmd_abi_diff(args: argparse.Namespace) -> int:
    """Compare a committed snapshot file vs a freshly-derived one.

    Exits 0 when the diff is non-breaking (added functions only) or has no
    changes; exits 1 when functions were removed or had their signature
    changed without the `--allow-breaking` flag.
    """
    old = abi_snapshot.parse(Path(args.snapshot).read_text())
    new = abi_snapshot.from_header(Path(args.header))

    d = abi_snapshot.diff(old, new)

    if d.added:
        print(f"added: {len(d.added)} functions")
        for n in d.added:
            print(f"  + {n}")
    if d.changed:
        print(f"changed: {len(d.changed)} functions (BREAKING)")
        for n in d.changed:
            o = old[n]
            x = new[n]
            print(f"  ~ {n}")
            print(f"      old: {o.return_type} {n}({', '.join(o.param_types)})")
            print(f"      new: {x.return_type} {n}({', '.join(x.param_types)})")
    if d.removed:
        print(f"removed: {len(d.removed)} functions (BREAKING)")
        for n in d.removed:
            o = old[n]
            print(f"  - {n}: was {o.return_type} {n}({', '.join(o.param_types)})")

    if abi_snapshot.is_breaking(d):
        if args.allow_breaking:
            print("breaking changes acknowledged via --allow-breaking; allowing.")
            return 0
        print(
            "::error::ABI break detected. If intentional, include "
            "'BREAKING:' in the commit message and re-run with "
            "--allow-breaking, then update .api/c-api.snapshot via "
            "`tools/codegen/scripts/regenerate.sh`.",
            file=sys.stderr,
        )
        return 1

    if not d.added and not d.changed and not d.removed:
        print("ABI snapshot in sync.")
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

    pco = sub.add_parser("emit-codon",
                         help="Generate Codon `from C import` declarations.")
    pco.add_argument("--spec", required=True)
    pco.add_argument("--out", required=True,
                     help="Output path (use '-' for stdout)")
    pco.set_defaults(func=_cmd_emit_codon)

    pll = sub.add_parser("emit-llms",
                         help="Generate Markdown C-API reference for AI agents.")
    pll.add_argument("--spec", required=True)
    pll.add_argument("--out", required=True,
                     help="Output path (use '-' for stdout)")
    pll.set_defaults(func=_cmd_emit_llms)

    pas = sub.add_parser("abi-snapshot",
                         help="Write an ABI snapshot from a C header.")
    pas.add_argument("--header", required=True,
                     help="Source C header (e.g. include/flox/capi/flox_capi.h)")
    pas.add_argument("--out", required=True,
                     help="Output snapshot path (use '-' for stdout)")
    pas.set_defaults(func=_cmd_abi_snapshot)

    pad = sub.add_parser("abi-diff",
                         help="Diff a committed snapshot vs a fresh one.")
    pad.add_argument("--snapshot", required=True,
                     help="Committed snapshot path")
    pad.add_argument("--header", required=True,
                     help="C header to derive the fresh snapshot from")
    pad.add_argument("--allow-breaking", action="store_true",
                     help=("Allow breaking changes (used by CI when the commit "
                           "message contains BREAKING:)."))
    pad.set_defaults(func=_cmd_abi_diff)

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

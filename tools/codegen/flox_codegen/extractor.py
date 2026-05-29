"""libclang-based extractor — annotated C++ headers → IR.

The extractor parses a single TU (translation unit), walks the AST, and pulls
out every declaration tagged with FLOX_EXPORT. The annotation string carried
by clang::annotate is parsed for key=value arguments.

The extractor is deliberately strict: unknown annotation keys are kept as-is
in `Function.annotations` (emitters decide whether to honor them), but
malformed annotations raise.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

from . import ir


# ── libclang library resolution ──────────────────────────────────────


def _resolve_libclang_path() -> Optional[str]:
    """Find a libclang library to load.

    Order:
    1. FLOX_LIBCLANG_PATH env var (escape hatch).
    2. Bundled libclang from the `libclang` PyPI package, if installed.
    3. Homebrew LLVM (macOS).
    4. Fall back to system search via clang.cindex defaults.
    """
    env = os.environ.get("FLOX_LIBCLANG_PATH")
    if env and Path(env).exists():
        return env

    # The `libclang` PyPI package installs under the top-level `clang.native`
    # namespace and ships a libclang.dylib / .so / .dll next to its __init__.py.
    try:
        import clang  # type: ignore
    except ImportError:
        clang = None  # type: ignore
    if clang is not None:
        clang_dir = Path(clang.__file__).parent / "native"
        for candidate in (
            clang_dir / "libclang.dylib",
            clang_dir / "libclang.so",
            clang_dir / "libclang.dll",
        ):
            if candidate.exists():
                return str(candidate)

    for candidate in (
        "/opt/homebrew/opt/llvm/lib/libclang.dylib",
        "/usr/local/opt/llvm/lib/libclang.dylib",
        "/usr/lib/x86_64-linux-gnu/libclang-14.so.1",
        "/usr/lib/x86_64-linux-gnu/libclang-15.so.1",
        "/usr/lib/x86_64-linux-gnu/libclang.so",
        "/Library/Developer/CommandLineTools/usr/lib/libclang.dylib",
    ):
        if Path(candidate).exists():
            return candidate

    return None


_LIBCLANG_CONFIGURED = False


def _ensure_libclang_loaded() -> None:
    """Configure clang.cindex to point at a real libclang library.

    Called once at extractor start. Idempotent — second call is a no-op,
    because clang.cindex.Config.set_library_file refuses to run after the
    library has been loaded.
    """
    global _LIBCLANG_CONFIGURED
    if _LIBCLANG_CONFIGURED:
        return
    import clang.cindex  # imported lazily so `import flox_codegen.ir` is cheap

    path = _resolve_libclang_path()
    if path is not None:
        try:
            clang.cindex.Config.set_library_file(path)
        except Exception:
            # Already loaded earlier — that's fine, we don't need to retry.
            pass
    _LIBCLANG_CONFIGURED = True


# ── System include path discovery ────────────────────────────────────


def _run_clang(clang_bin: str, args: List[str], *, stdin: str = "",
               timeout: float = 30.0, retries: int = 1):
    """Run a clang subcommand, retrying once on timeout (CI runners under load
    occasionally blow a tight deadline). Returns the CompletedProcess or None."""
    for attempt in range(retries + 1):
        try:
            return subprocess.run(
                [clang_bin, *args], input=stdin, capture_output=True,
                text=True, timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            if attempt >= retries:
                return None
        except OSError:
            return None
    return None


def _resource_dir_include(clang_bin: str) -> Optional[str]:
    """The clang builtin-headers directory (stddef.h, stdint.h, ...). The PyPI
    libclang ships no resource-dir, so this is the single most important include
    to recover. `clang -print-resource-dir` is a fast, dedicated query, far less
    flake-prone than scraping `-E -v` output."""
    proc = _run_clang(clang_bin, ["-print-resource-dir"], timeout=15.0)
    if proc is None or proc.returncode != 0:
        return None
    rd = proc.stdout.strip()
    if not rd:
        return None
    inc = str(Path(rd) / "include")
    return inc if Path(inc).is_dir() else None


def _discover_system_includes() -> List[str]:
    """Return the system include directories the host clang would use.

    The libclang shipped with the PyPI `libclang` package does not ship a
    resource-dir, so headers like <stddef.h> aren't found out of the box. We
    recover the search path two ways and merge them: a dedicated
    `-print-resource-dir` query for the builtin headers (the critical piece),
    and parsing `clang -E -v` for the full system search list. Both subcommands
    retry on timeout. Cached per-process via `_DISCOVERED_INCLUDES`.

    Returns -I directories suitable for prepending to clang command-line args.
    """
    global _DISCOVERED_INCLUDES
    cached = globals().get("_DISCOVERED_INCLUDES")
    if cached is not None:
        return cached  # type: ignore[return-value]

    clang_bin = shutil.which("clang") or shutil.which("clang++")
    paths: List[str] = []
    if clang_bin:
        # 1) Builtin-headers (resource-dir/include) — the must-have.
        rd_inc = _resource_dir_include(clang_bin)
        if rd_inc:
            paths.append(rd_inc)

        # 2) Full system search list via -E -v (retries on timeout).
        proc = _run_clang(clang_bin, ["-x", "c++", "-E", "-v", "-"], stdin="")
        if proc is not None:
            collecting = False
            for raw in proc.stderr.splitlines():
                line = raw.rstrip()
                if "search starts" in line:
                    collecting = True
                    continue
                if "End of search list" in line:
                    collecting = False
                    continue
                if not collecting:
                    continue
                # Skip framework dirs — flagged by trailing "(framework directory)"
                stripped = line.strip()
                if stripped.endswith("(framework directory)"):
                    continue
                if stripped.startswith("/") and Path(stripped).is_dir():
                    paths.append(stripped)

    # On macOS, also try the SDK explicitly via xcrun in case `clang -E` failed.
    if sys.platform == "darwin":
        sdk = _run_clang("xcrun", ["--show-sdk-path"], timeout=15.0) \
            if shutil.which("xcrun") else None
        if sdk is not None and sdk.returncode == 0 and sdk.stdout.strip():
            paths.append(sdk.stdout.strip() + "/usr/include")

    # Dedup preserving order.
    seen: set = set()
    deduped: List[str] = []
    for p in paths:
        if p not in seen:
            seen.add(p)
            deduped.append(p)

    _DISCOVERED_INCLUDES = deduped
    return deduped


# ── Annotation parsing ───────────────────────────────────────────────

_ANNOT_RE = re.compile(r"^flox::export\s*\((.*)\)\s*$", re.DOTALL)
# A single key=value argument. Value is either a quoted string or a bare ident.
# We accept = with arbitrary whitespace and optional trailing commas.
_KV_RE = re.compile(
    r"""
    \s*
    (?P<key>[A-Za-z_][A-Za-z0-9_]*)        # key
    \s*=\s*
    (?:
        " (?P<sval>[^"]*) "                # quoted
      | (?P<bval>[A-Za-z0-9_./:-]+)        # bare
    )
    \s*,?\s*
    """,
    re.VERBOSE,
)
# A bare flag with no value, e.g. `pointer_out_wrapper` or `internal_only`.
_FLAG_RE = re.compile(r"\s*(?P<flag>[A-Za-z_][A-Za-z0-9_]*)\s*,?\s*")


def parse_annotation(raw: str) -> dict:
    """Parse a raw clang::annotate string ("flox::export(k=v, ...)") into a dict.

    Bare flags (e.g. `pointer_out_wrapper`) are stored as the flag name keyed to
    the empty string, distinguishing them from absence.

    Raises ValueError if the body is malformed.
    """
    m = _ANNOT_RE.match(raw)
    if not m:
        raise ValueError(f"not a flox::export annotation: {raw!r}")
    body = m.group(1).strip()
    if not body:
        return {}
    out: dict = {}
    pos = 0
    while pos < len(body):
        # Try key=value first.
        kv = _KV_RE.match(body, pos)
        if kv:
            key = kv.group("key")
            val = kv.group("sval")
            if val is None:
                val = kv.group("bval")
            out[key] = val
            pos = kv.end()
            continue
        # Fall through to bare flag.
        flag = _FLAG_RE.match(body, pos)
        if flag:
            out[flag.group("flag")] = ""
            pos = flag.end()
            continue
        raise ValueError(
            f"could not parse annotation body at offset {pos}: {body[pos:pos + 40]!r}"
        )
    return out


# ── AST walk ─────────────────────────────────────────────────────────


def _flox_export_annotations(cursor) -> List[str]:
    """All clang::annotate strings on cursor whose body is a flox::export(...).

    Multiple annotations on the same declaration are merged via parse_annotation
    union; this returns the raw strings so the caller can keep source-location
    fidelity for errors.
    """
    import clang.cindex

    out: List[str] = []
    for child in cursor.get_children():
        if child.kind == clang.cindex.CursorKind.ANNOTATE_ATTR:
            text = child.spelling or child.displayname
            if _ANNOT_RE.match(text):
                out.append(text)
    return out


def _format_loc(cursor) -> Optional[str]:
    loc = cursor.location
    if loc.file is None:
        return None
    return f"{loc.file.name}:{loc.line}"


def _params_from_function_cursor(cursor) -> Tuple[ir.Param, ...]:
    import clang.cindex

    params = []
    for arg in cursor.get_arguments():
        # arg.spelling can be empty for unnamed params; libclang gives us a slot.
        params.append(ir.Param(name=arg.spelling or "", type=arg.type.spelling))
    return tuple(params)


def _walk_module(tu_cursor, *, spec_path: Path) -> ir.Module:
    import clang.cindex

    mod = ir.Module()
    seen_handles: set = set()
    seen_structs: set = set()
    seen_fnptrs: set = set()
    seen_enums: set = set()
    seen_functions: set = set()

    K = clang.cindex.CursorKind
    spec_str = str(spec_path)

    def _from_spec(cursor) -> bool:
        loc = cursor.location
        return loc.file is not None and loc.file.name == spec_str

    def visit(cursor):
        # Recurse through namespaces and extern "C" linkage specs even when
        # they're not from the spec file (the spec's own extern "C" block has
        # children we want).
        if cursor.kind in (K.NAMESPACE, K.UNEXPOSED_DECL, K.LINKAGE_SPEC):
            for child in cursor.get_children():
                visit(child)
            return

        # Skip declarations from system headers, transitively-included engine
        # headers, etc. Only emit what the spec itself declares.
        if not _from_spec(cursor):
            return

        # typedef void* FloxXHandle;
        if cursor.kind == K.TYPEDEF_DECL:
            underlying = cursor.underlying_typedef_type.spelling
            name = cursor.spelling
            if underlying == "void *" and name not in seen_handles:
                seen_handles.add(name)
                mod.handles.append(ir.HandleTypedef(name=name))
            elif underlying in seen_handles and name not in seen_handles:
                # `typedef FloxFooHandle FloxBarHandle;` — alias to an earlier
                # opaque handle; carry the original name through so the emitter
                # writes a `typedef <orig> <name>` line.
                seen_handles.add(name)
                mod.handles.append(ir.HandleTypedef(name=name, alias_of=underlying))
            elif underlying.startswith("void (*)") or "(*)(" in underlying:
                if name not in seen_fnptrs:
                    seen_fnptrs.add(name)
                    # Re-extract via the pointee-function decl if libclang
                    # exposes it, else fall back to type-spelling parsing.
                    pointee = cursor.underlying_typedef_type.get_pointee()
                    rt = pointee.get_result().spelling if pointee else "void"
                    arg_types = (
                        list(pointee.argument_types()) if pointee else []
                    )
                    params = tuple(
                        ir.Param(name="", type=t.spelling) for t in arg_types
                    )
                    mod.function_pointers.append(
                        ir.FunctionPointerTypedef(
                            name=name, return_type=rt, params=params
                        )
                    )
            elif underlying.startswith("struct ") or cursor.type.get_canonical().kind == clang.cindex.TypeKind.RECORD:
                # `typedef struct {...} FooT;` — handled via the inline RECORD
                # decl below; nothing to do here.
                pass
            return

        # typedef enum { ... } FloxFooKind — both anonymous body + typedef name
        # come through as one ENUM_DECL whose spelling is the typedef name.
        if cursor.kind == K.ENUM_DECL:
            name = cursor.spelling
            if not name or name in seen_enums:
                return
            seen_enums.add(name)
            values: List[ir.EnumValue] = []
            for child in cursor.get_children():
                if child.kind == K.ENUM_CONSTANT_DECL:
                    # libclang exposes enum_value as int (signed) or unsigned;
                    # we treat the signed reading as canonical for C enums.
                    try:
                        ev: Optional[int] = int(child.enum_value)
                    except Exception:
                        ev = None
                    values.append(ir.EnumValue(name=child.spelling, value=ev))
            mod.enums.append(ir.Enum(name=name, values=tuple(values)))
            return

        # Anonymous struct -> typedef.
        if cursor.kind == K.STRUCT_DECL:
            # We accept structs with a name OR structs declared via
            # `typedef struct { ... } Foo;` — the latter shows up as a STRUCT_DECL
            # whose spelling is the typedef name (in libclang ≥ 11).
            name = cursor.spelling
            if not name:
                return
            if name in seen_structs:
                return
            seen_structs.add(name)
            fields: List[ir.StructField] = []
            for child in cursor.get_children():
                if child.kind == K.FIELD_DECL:
                    fields.append(
                        ir.StructField(
                            name=child.spelling, type=child.type.spelling
                        )
                    )
            mod.structs.append(ir.Struct(name=name, fields=tuple(fields)))
            return

        # Free function — only retained when annotated.
        if cursor.kind == K.FUNCTION_DECL:
            annots = _flox_export_annotations(cursor)
            if not annots:
                return
            merged: dict = {}
            for raw in annots:
                merged.update(parse_annotation(raw))
            if "internal_only" in merged:
                return
            name = cursor.spelling
            if name in seen_functions:
                return
            seen_functions.add(name)
            mod.functions.append(
                ir.Function(
                    name=name,
                    return_type=cursor.result_type.spelling,
                    params=_params_from_function_cursor(cursor),
                    annotations=merged,
                    source_location=_format_loc(cursor),
                )
            )
            return

    for child in tu_cursor.get_children():
        visit(child)
    return mod


def _walk_module_for_spec(tu_cursor, spec_path: Path) -> ir.Module:
    """Public wrapper preserving the historical signature for tests."""
    return _walk_module(tu_cursor, spec_path=spec_path)


# ── Public entry point ───────────────────────────────────────────────


def parse_spec(
    spec_path: Path,
    *,
    include_dirs: Iterable[Path] = (),
    extra_args: Iterable[str] = (),
) -> ir.Module:
    """Parse the given spec header and return an IR Module.

    Args:
        spec_path: Path to flox_capi_spec.hpp (or test fixture).
        include_dirs: Additional -I directories. The flox `include/` root is
            inferred from the spec path's `include/flox/capi/` ancestor and
            added automatically.
        extra_args: Pass-through clang command-line args.
    """
    _ensure_libclang_loaded()
    import clang.cindex

    spec_path = Path(spec_path).resolve()

    # Auto-add the engine include root if the spec lives under it.
    inferred_include_root: Optional[Path] = None
    for ancestor in spec_path.parents:
        if (ancestor / "flox" / "capi").is_dir() and ancestor.name == "include":
            inferred_include_root = ancestor
            break
        if (ancestor / "include" / "flox" / "capi").is_dir():
            inferred_include_root = ancestor / "include"
            break

    args: List[str] = ["-x", "c++", "-std=c++23"]
    if inferred_include_root is not None:
        args += ["-I", str(inferred_include_root)]
    for d in include_dirs:
        args += ["-I", str(d)]
    for d in _discover_system_includes():
        args += ["-I", d]
    args += list(extra_args)

    index = clang.cindex.Index.create()
    tu = index.parse(str(spec_path), args=args, options=0)

    diags = [d for d in tu.diagnostics if d.severity >= clang.cindex.Diagnostic.Error]
    if diags:
        msg = "\n".join(f"  {d.location}: {d.spelling}" for d in diags)
        raise RuntimeError(
            f"libclang errors parsing {spec_path}:\n{msg}"
        )

    return _walk_module(tu.cursor, spec_path=spec_path)

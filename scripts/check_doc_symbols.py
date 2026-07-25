#!/usr/bin/env python3
"""scripts/check_doc_symbols.py

Verify that every API symbol a hand-written doc names actually exists.

The generated-doc gates (`gen_indicator_docs.py`, `gen_api_index.py`,
`check_dts_exports.py`, ...) only prove that *generated* pages match the
source. Hand-written prose is unchecked, and that is where whole pages
documenting functions that never existed come from. This gate closes that
hole by extracting API-looking references from Markdown and resolving each
against the real binding surface:

* **Python** — `flox.<Name>`, `flox_py.<Name>`, `from flox_py import X`,
  bare `Name(...)` calls that nothing in the snippet binds, and attribute
  access on a variable whose class is known from `x = flox.<Class>(...)`.
  Surface: `python/flox_py/_flox_py/__init__.pyi` (`__all__` + class/def
  names), `python/flox_py/__init__.pyi`, and the pure-Python submodules
  under `python/flox_py/`.
* **Node** — `flox.<name>(`, `new flox.<Name>(` and destructured
  `const { A, B } = require('@flox-foundation/flox')` inside
  ```javascript / ```js / ```ts blocks. Surface: exported names in
  `node/index.d.ts`.
* **Codon** — `from flox.<mod> import X` inside Codon blocks. Surface:
  `class X` / `def x` in `codon/flox/<mod>.codon`.

Blocks are classified by fence language, by the enclosing `=== "..."` tab
label, and by page path. QuickJS blocks (embedded runtime, globals only, no
`node/index.d.ts`) are not checked here.

Prose (inline code outside fences) is checked leniently: a `flox.X`
reference in prose only fails when `X` exists in *no* binding surface.

False positives are expected on prose that reads like an API call. Add the
symbol to `scripts/doc_symbols_allow.txt`, one per line, each with a `#`
comment saying why. Entries may be bare (`Foo`) or page-scoped
(`docs/how-to/x.md:Foo`).

Usage:
    python3 scripts/check_doc_symbols.py             # report + exit 1
    python3 scripts/check_doc_symbols.py --quiet     # only failures
    python3 scripts/check_doc_symbols.py --list      # allowlist-ready dump
"""
from __future__ import annotations

import argparse
import ast
import builtins
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple


REPO_ROOT = Path(__file__).resolve().parents[1]
DOCS_DIR = REPO_ROOT / "docs"
PY_PKG_DIR = REPO_ROOT / "python" / "flox_py"
NODE_DTS = REPO_ROOT / "node" / "index.d.ts"
CODON_PKG_DIR = REPO_ROOT / "codon" / "flox"
ALLOWLIST_PATH = REPO_ROOT / "scripts" / "doc_symbols_allow.txt"

# Generated aggregates and the generated Python API index: their contents
# come from the source of truth already, and _api_index.md re-lists every
# symbol, which would drown the report.
EXCLUDED_DOCS = {
    DOCS_DIR / "llms.txt",
    DOCS_DIR / "llms-full.txt",
    DOCS_DIR / "reference" / "python" / "_api_index.md",
}

PY_LANGS = {"python", "py", "python3"}
JS_LANGS = {"js", "javascript", "node", "ts", "typescript", "jsx", "tsx"}
CODON_LANGS = {"codon"}

# The names docs use for the Python package.
PY_ALIASES = {"flox", "flox_py"}

# Fences can be longer than three characters so a block can contain a fence
# (the doc-gates page shows a ```python block inside a ````markdown one).
_FENCE_RE = re.compile(r"^\s*(?P<mark>`{3,}|~{3,})(?P<lang>[A-Za-z0-9_+-]*)")
_TAB_RE = re.compile(r'^\s*(?:===|\?{3})[+!]?\s+"(?P<label>[^"]*)"')
_INCLUDE_RE = re.compile(r'^\s*--8<--\s+".+?"\s*$')
_INLINE_CODE_RE = re.compile(r"`([^`\n]+)`")
_FILENAME_RE = re.compile(
    r"^[\w.*/-]+\.(?:c|h|cc|cpp|hpp|py|pyi|pyx|ts|js|mjs|cjs|json|toml|yaml"
    r"|yml|md|txt|so|dylib|dll|codon|sh|csv|zip|floxlog|floxrun|def|in|lock"
    r"|cmake|cpython-\d+-\w+\.so)$"
)

_ATTR_CHAIN_RE = re.compile(
    r"(?<![\w.])(?P<root>[A-Za-z_]\w*)\.(?P<rest>[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)"
)
_PY_FROM_IMPORT_RE = re.compile(
    r"^\s*from\s+(?P<mod>flox(?:_py)?(?:\.[\w.]+)?)\s+import\s+(?P<names>.+)$"
)
_JS_DESTRUCTURE_RE = re.compile(
    r"(?:const|let|var)\s*\{(?P<names>[^}]*)\}\s*=\s*require\(\s*"
    r"['\"]@flox-foundation/flox['\"]\s*\)(?!\s*\.)"
)
# `require('@flox-foundation/flox').composite` — the sub-namespace itself
# has to be an export.
_JS_NAMESPACE_RE = re.compile(
    r"require\(\s*['\"]@flox-foundation/flox['\"]\s*\)\.(?P<name>\w+)"
)
_JS_NEW_RE = re.compile(r"new\s+flox\.(?P<name>[A-Za-z_]\w*)")
_JS_CALL_RE = re.compile(r"(?<![\w.])flox\.(?P<name>[A-Za-z_]\w*)")

_BUILTINS = set(dir(builtins))
# Names a doc snippet may use without the snippet binding them: stdlib and
# scientific-stack conventions, plus typing.
_AMBIENT_NAMES = {
    "List", "Dict", "Set", "Tuple", "Optional", "Any", "Union", "Callable",
    "Iterator", "Iterable", "Sequence", "Mapping", "Deque", "DefaultDict",
    "Path", "Decimal", "Enum", "IntEnum", "Thread", "Lock", "Queue",
    "TypeVar", "Protocol", "Final", "ClassVar", "Literal", "NamedTuple",
    "TypedDict", "TextIO", "BinaryIO", "StringIO", "BytesIO", "Fraction",
    "Counter", "OrderedDict", "ThreadPoolExecutor", "ProcessPoolExecutor",
    "Future", "ABC", "AbstractSet", "Self",
}


# --------------------------------------------------------------------------
# Surface loading
# --------------------------------------------------------------------------

@dataclass
class Surface:
    py_modules: Dict[str, Set[str]] = field(default_factory=dict)
    py_class_members: Dict[str, Set[str]] = field(default_factory=dict)
    node_exports: Set[str] = field(default_factory=set)
    codon_modules: Dict[str, Set[str]] = field(default_factory=dict)

    @property
    def py_root(self) -> Set[str]:
        return self.py_modules.get("", set())

    def any_surface(self) -> Set[str]:
        out: Set[str] = set()
        for names in self.py_modules.values():
            out |= names
        out |= self.node_exports
        for names in self.codon_modules.values():
            out |= names
        out |= set(self.codon_modules)
        return out


def _module_level_names(tree: ast.Module) -> Tuple[Set[str], Dict[str, Set[str]]]:
    """Top-level public names of a parsed module, plus its class members."""
    names: Set[str] = set()
    class_members: Dict[str, Set[str]] = {}
    for node in tree.body:
        if isinstance(node, ast.ClassDef):
            names.add(node.name)
            members: Set[str] = set()
            for sub in node.body:
                if isinstance(sub, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    members.add(sub.name)
                elif isinstance(sub, ast.AnnAssign) and isinstance(sub.target, ast.Name):
                    members.add(sub.target.id)
                elif isinstance(sub, ast.Assign):
                    for target in sub.targets:
                        if isinstance(target, ast.Name):
                            members.add(target.id)
            class_members.setdefault(node.name, set()).update(members)
        elif isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            names.add(node.name)
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                if isinstance(target, ast.Name):
                    names.add(target.id)
                    if target.id == "__all__":
                        names |= _string_list(node.value)
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            names.add(node.target.id)
            if node.target.id == "__all__" and node.value is not None:
                names |= _string_list(node.value)
        elif isinstance(node, ast.ImportFrom):
            for alias in node.names:
                if alias.name != "*":
                    names.add(alias.asname or alias.name)
        elif isinstance(node, ast.Import):
            for alias in node.names:
                names.add(alias.asname or alias.name.split(".")[0])
    return names, class_members


def _string_list(node: ast.expr) -> Set[str]:
    out: Set[str] = set()
    if isinstance(node, (ast.List, ast.Tuple, ast.Set)):
        for element in node.elts:
            if isinstance(element, ast.Constant) and isinstance(element.value, str):
                out.add(element.value)
    return out


def _load_python_surface(surface: Surface) -> None:
    root_names: Set[str] = set()

    for pyi in (PY_PKG_DIR / "_flox_py" / "__init__.pyi",
                PY_PKG_DIR / "__init__.pyi"):
        if not pyi.exists():
            continue
        tree = ast.parse(pyi.read_text(encoding="utf-8"), filename=str(pyi))
        names, members = _module_level_names(tree)
        root_names |= names
        for cls, attrs in members.items():
            surface.py_class_members.setdefault(cls, set()).update(attrs)

    def load_module(path: Path, dotted: str) -> None:
        try:
            tree = ast.parse(path.read_text(encoding="utf-8"), filename=str(path))
        except SyntaxError:
            return
        names, members = _module_level_names(tree)
        surface.py_modules.setdefault(dotted, set()).update(names)
        for cls, attrs in members.items():
            surface.py_class_members.setdefault(cls, set()).update(attrs)

    for path in sorted(PY_PKG_DIR.rglob("*.py")):
        rel = path.relative_to(PY_PKG_DIR)
        parts = list(rel.parts)
        if any(p.startswith("__pycache__") for p in parts):
            continue
        if parts[-1] == "__init__.py":
            dotted = ".".join(parts[:-1])
        else:
            dotted = ".".join(parts)[: -len(".py")]
        if dotted.startswith("_flox_py"):
            continue
        load_module(path, dotted)
        # Register the module itself as a member of its parent.
        if dotted:
            head, _, tail = dotted.rpartition(".")
            surface.py_modules.setdefault(head, set()).add(tail)

    # The compiled extension is reachable both as `flox_py.<name>` and as
    # `flox_py._flox_py.<name>`.
    root_names.add("_flox_py")
    surface.py_modules.setdefault("", set()).update(root_names)
    surface.py_modules.setdefault("_flox_py", set()).update(root_names)


_TS_EXPORT_RE = re.compile(
    r"^\s*export\s+(?:declare\s+)?"
    r"(?:abstract\s+)?"
    r"(?:class|interface|type|function|const|let|var|enum|namespace)\s+"
    r"(?P<name>[A-Za-z_]\w*)"
)


def _load_node_surface(surface: Surface) -> None:
    if not NODE_DTS.exists():
        return
    depth = 0
    for line in NODE_DTS.read_text(encoding="utf-8").splitlines():
        match = _TS_EXPORT_RE.match(line)
        if match and depth == 0:
            surface.node_exports.add(match.group("name"))
        elif match and depth > 0:
            # `export ...` inside `declare namespace flox { ... }` is still
            # reachable as `flox.<name>`.
            surface.node_exports.add(match.group("name"))
        depth += line.count("{") - line.count("}")
        depth = max(depth, 0)


_CODON_DEF_RE = re.compile(r"^(?:class|def)\s+(?P<name>[A-Za-z_]\w*)")
_CODON_ASSIGN_RE = re.compile(r"^(?P<name>[A-Z][A-Z0-9_]*)\s*[:=]")


def _load_codon_surface(surface: Surface) -> None:
    if not CODON_PKG_DIR.is_dir():
        return
    for path in sorted(CODON_PKG_DIR.glob("*.codon")):
        mod = path.stem
        names: Set[str] = set()
        for line in path.read_text(encoding="utf-8").splitlines():
            match = _CODON_DEF_RE.match(line)
            if match:
                names.add(match.group("name"))
                continue
            match = _CODON_ASSIGN_RE.match(line)
            if match:
                names.add(match.group("name"))
            if line.startswith("from ") or line.startswith("import "):
                names |= _codon_imported_names(line)
        surface.codon_modules[mod] = names


def _codon_imported_names(line: str) -> Set[str]:
    """Names a `.codon` module re-exports.

    Handles Codon's C-FFI form too: `from C import flox_registry_create()
    -> cobj` binds `flox_registry_create`.
    """
    match = re.match(r"^from\s+[\w.]+\s+import\s+(?P<names>.+)$", line)
    if not match:
        return set()
    names = match.group("names").strip()
    signature = re.match(r"(?P<name>[A-Za-z_]\w*)\s*\(", names)
    if signature:
        # `from C import fn(arg, arg) -> ret`: the commas are parameters.
        return {signature.group("name")}
    out: Set[str] = set()
    for chunk in names.split(","):
        name = chunk.strip().strip("()").split(" as ")[-1].strip()
        ident = re.match(r"[A-Za-z_]\w*", name)
        if ident and ident.group(0) != "*":
            out.add(ident.group(0))
    return out


def load_surface() -> Surface:
    surface = Surface()
    _load_python_surface(surface)
    _load_node_surface(surface)
    _load_codon_surface(surface)
    return surface


# --------------------------------------------------------------------------
# Markdown segmentation
# --------------------------------------------------------------------------

@dataclass
class Block:
    kind: str            # "python" | "node" | "codon" | "prose" | "skip"
    lang: str
    start_line: int
    lines: List[str]

    @property
    def text(self) -> str:
        return "\n".join(self.lines)


def _classify(lang: str, tab: str, page: Path, body: str,
              surface: Surface) -> str:
    label = tab.lower()
    rel = page.relative_to(REPO_ROOT).as_posix()

    quickjs_page = rel.startswith("docs/reference/quickjs/") or \
        rel == "docs/bindings/javascript.md"
    if "quickjs" in label or (quickjs_page and "node" not in label):
        return "skip"

    if lang in CODON_LANGS or "codon" in label:
        return "codon"
    if lang in PY_LANGS:
        # Codon strategy snippets are tagged ```python. If the block imports
        # from a Codon module path, treat it as Codon.
        for match in re.finditer(r"^\s*from\s+flox\.(?P<mod>\w+)\s+import",
                                 body, re.MULTILINE):
            if match.group("mod") in surface.codon_modules:
                return "codon"
        return "python"
    if lang in JS_LANGS:
        return "node"
    return "skip"


def _segment(page: Path, surface: Surface) -> Tuple[List[Block], List[Tuple[int, str]]]:
    """Split a page into typed code blocks plus prose lines."""
    blocks: List[Block] = []
    prose: List[Tuple[int, str]] = []
    lines = page.read_text(encoding="utf-8").splitlines()

    fence_lang: Optional[str] = None
    fence_mark = ""
    fence_start = 0
    body: List[str] = []
    tab = ""

    for lineno, line in enumerate(lines, 1):
        fence = _FENCE_RE.match(line)
        if fence_lang is None:
            if fence:
                fence_lang = fence.group("lang").lower()
                fence_mark = fence.group("mark")
                fence_start = lineno
                body = []
                continue
            tab_match = _TAB_RE.match(line)
            if tab_match:
                tab = tab_match.group("label")
            elif line.startswith("#"):
                tab = ""  # a heading always ends the tab group
            prose.append((lineno, line))
            continue

        if (fence and not fence.group("lang")
                and fence.group("mark")[0] == fence_mark[0]
                and len(fence.group("mark")) >= len(fence_mark)):
            text = "\n".join(body)
            if not any(_INCLUDE_RE.match(b) for b in body):
                kind = _classify(fence_lang, tab, page, text, surface)
                if kind != "skip":
                    blocks.append(Block(kind, fence_lang, fence_start + 1, body))
            fence_lang = None
            continue
        body.append(line)

    return blocks, prose


# --------------------------------------------------------------------------
# Reference extraction
# --------------------------------------------------------------------------

@dataclass
class Violation:
    page: Path
    line: int
    symbol: str
    message: str


def _resolve_python_chain(surface: Surface, parts: List[str]) -> Optional[str]:
    """Resolve `flox.<parts>`; return the first component that does not exist."""
    module = ""
    for index, part in enumerate(parts):
        members = surface.py_modules.get(module, set())
        if part not in members:
            if index == 0:
                return part
            return ".".join(parts[: index + 1])
        candidate = f"{module}.{part}" if module else part
        if candidate in surface.py_modules:
            module = candidate
            continue
        # Reached a class / function: stop walking (member checks are done
        # separately against the .pyi class bodies).
        return None
    return None


def _check_python_block(block: Block, page: Path, surface: Surface,
                        page_bound: Set[str]) -> List[Violation]:
    out: List[Violation] = []
    text = block.text

    aliases = set(PY_ALIASES)
    for match in re.finditer(r"^\s*import\s+flox_py\s+as\s+(\w+)", text, re.MULTILINE):
        aliases.add(match.group(1))

    # 1. `flox.<chain>` attribute paths.
    for lineno, line in enumerate(block.lines, block.start_line):
        for match in _ATTR_CHAIN_RE.finditer(line):
            if match.group("root") not in aliases:
                continue
            parts = match.group("rest").split(".")
            bad = _resolve_python_chain(surface, parts)
            if bad:
                out.append(Violation(
                    page, lineno, f"flox.{bad}",
                    f"`{match.group('root')}.{bad}` is not in the Python "
                    f"surface (python/flox_py/**/__init__.pyi)"))

    # 2. `from flox_py[.sub] import X`
    for lineno, line in enumerate(block.lines, block.start_line):
        match = _PY_FROM_IMPORT_RE.match(line)
        if not match:
            continue
        mod = match.group("mod")
        sub = mod.split(".", 1)[1] if "." in mod else ""
        if sub and sub not in surface.py_modules:
            out.append(Violation(
                page, lineno, f"flox.{sub}",
                f"`{mod}` is not a module under python/flox_py/"))
            continue
        members = surface.py_modules.get(sub, set())
        for chunk in match.group("names").split("#")[0].split(","):
            name = chunk.strip().strip("()").split(" as ")[0].strip()
            if not name or name == "*":
                continue
            if name not in members:
                out.append(Violation(
                    page, lineno, name,
                    f"`from {mod} import {name}` — {mod} exports no {name}"))

    # 3 + 4 need a parse tree.
    try:
        tree = ast.parse(text)
    except SyntaxError:
        return out

    bound = _bound_names(tree) | aliases | page_bound
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
            name = node.func.id
            if not name[:1].isupper():
                continue  # snake_case free functions are too prose-like
            if name in bound or name in _BUILTINS or name in _AMBIENT_NAMES:
                continue
            if name in surface.py_root:
                continue
            out.append(Violation(
                page, block.start_line + node.lineno - 1, name,
                f"`{name}(...)` is called but nothing binds it and the "
                f"Python surface has no `{name}`"))

    for var, cls in _instance_types(tree, aliases, surface).items():
        members = surface.py_class_members.get(cls, set())
        if not members:
            continue
        for node in ast.walk(tree):
            if (isinstance(node, ast.Attribute)
                    and isinstance(node.value, ast.Name)
                    and node.value.id == var
                    and not node.attr.startswith("_")
                    and node.attr not in members):
                out.append(Violation(
                    page, block.start_line + node.lineno - 1,
                    f"{cls}.{node.attr}",
                    f"`{var}.{node.attr}` — {cls} has no member "
                    f"`{node.attr}` in the .pyi"))
    return out


def _bound_names(tree: ast.AST) -> Set[str]:
    """Every name the snippet itself binds."""
    bound: Set[str] = set()

    def add_target(node: ast.AST) -> None:
        if isinstance(node, ast.Name):
            bound.add(node.id)
        elif isinstance(node, (ast.Tuple, ast.List)):
            for element in node.elts:
                add_target(element)
        elif isinstance(node, ast.Starred):
            add_target(node.value)

    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)):
            bound.add(node.name)
        elif isinstance(node, (ast.Import, ast.ImportFrom)):
            for alias in node.names:
                bound.add(alias.asname or alias.name.split(".")[0])
        elif isinstance(node, ast.Assign):
            for target in node.targets:
                add_target(target)
        elif isinstance(node, (ast.AnnAssign, ast.AugAssign)):
            add_target(node.target)
        elif isinstance(node, (ast.For, ast.AsyncFor)):
            add_target(node.target)
        elif isinstance(node, (ast.With, ast.AsyncWith)):
            for item in node.items:
                if item.optional_vars is not None:
                    add_target(item.optional_vars)
        elif isinstance(node, ast.ExceptHandler) and node.name:
            bound.add(node.name)
        elif isinstance(node, (ast.comprehension,)):
            add_target(node.target)
        elif isinstance(node, ast.arg):
            bound.add(node.arg)
        elif isinstance(node, ast.Global) or isinstance(node, ast.Nonlocal):
            bound.update(node.names)
    return bound


def _instance_types(tree: ast.AST, aliases: Set[str],
                    surface: Surface) -> Dict[str, str]:
    """`x = flox.RSI(14)` -> {"x": "RSI"}; drop vars assigned twice."""
    types: Dict[str, str] = {}
    rebound: Set[str] = set()
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign) or len(node.targets) != 1:
            continue
        target = node.targets[0]
        if not isinstance(target, ast.Name):
            continue
        value = node.value
        cls = None
        if (isinstance(value, ast.Call) and isinstance(value.func, ast.Attribute)
                and isinstance(value.func.value, ast.Name)
                and value.func.value.id in aliases):
            cls = value.func.attr
        if cls and cls in surface.py_class_members:
            if target.id in types and types[target.id] != cls:
                rebound.add(target.id)
            types[target.id] = cls
        elif target.id in types:
            rebound.add(target.id)
    for name in rebound:
        types.pop(name, None)
    return types


def _page_bound_names(blocks: List[Block]) -> Set[str]:
    """Names defined anywhere in the page's Python/Codon blocks.

    Doc pages routinely define `class SMACross(...)` in one block and use it
    two blocks later; without this the second block looks like it calls an
    API that does not exist.
    """
    bound: Set[str] = set()
    for block in blocks:
        if block.kind not in ("python", "codon"):
            continue
        try:
            tree = ast.parse(block.text)
        except SyntaxError:
            for match in re.finditer(r"^\s*(?:class|def)\s+([A-Za-z_]\w*)",
                                     block.text, re.MULTILINE):
                bound.add(match.group(1))
            for match in re.finditer(r"^\s*(?:from\s+[\w.]+\s+)?import\s+(.+)$",
                                     block.text, re.MULTILINE):
                for chunk in match.group(1).split(","):
                    name = chunk.strip().split(" as ")[-1].strip().strip("()")
                    ident = re.match(r"[A-Za-z_]\w*", name)
                    if ident:
                        bound.add(ident.group(0))
            continue
        for node in tree.body:
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef,
                                 ast.ClassDef)):
                bound.add(node.name)
        bound |= _bound_names(tree)
    return bound


def _check_node_block(block: Block, page: Path, surface: Surface) -> List[Violation]:
    out: List[Violation] = []
    for lineno, line in enumerate(block.lines, block.start_line):
        for match in _JS_NAMESPACE_RE.finditer(line):
            name = match.group("name")
            if name not in surface.node_exports:
                out.append(Violation(
                    page, lineno, name,
                    f"`require('@flox-foundation/flox').{name}` — "
                    f"node/index.d.ts declares no `{name}`"))
        for match in _JS_DESTRUCTURE_RE.finditer(line):
            for chunk in match.group("names").split(","):
                name = chunk.split(":")[0].strip()
                if name and name not in surface.node_exports:
                    out.append(Violation(
                        page, lineno, name,
                        f"`{name}` is destructured from "
                        f"'@flox-foundation/flox' but node/index.d.ts does "
                        f"not export it"))
        seen: Set[str] = set()
        for match in list(_JS_NEW_RE.finditer(line)) + list(_JS_CALL_RE.finditer(line)):
            name = match.group("name")
            if name in seen:
                continue  # `new flox.X` matches both patterns
            seen.add(name)
            if name not in surface.node_exports:
                out.append(Violation(
                    page, lineno, name,
                    f"`flox.{name}` is not exported by node/index.d.ts"))
    return out


_CODON_IMPORT_RE = re.compile(
    r"^\s*from\s+flox\.(?P<mod>\w+)\s+import\s+(?P<names>.+)$"
)


def _check_codon_block(block: Block, page: Path, surface: Surface) -> List[Violation]:
    out: List[Violation] = []
    for lineno, line in enumerate(block.lines, block.start_line):
        match = _CODON_IMPORT_RE.match(line)
        if not match:
            continue
        mod = match.group("mod")
        if mod not in surface.codon_modules:
            out.append(Violation(
                page, lineno, f"flox.{mod}",
                f"`flox.{mod}` — no codon/flox/{mod}.codon"))
            continue
        members = surface.codon_modules[mod]
        for chunk in match.group("names").split("#")[0].split(","):
            name = chunk.strip().strip("()").split(" as ")[0].strip()
            if not name or name == "*":
                continue
            if name not in members:
                out.append(Violation(
                    page, lineno, name,
                    f"`from flox.{mod} import {name}` — "
                    f"codon/flox/{mod}.codon defines no {name}"))
    return out


def _check_prose(prose: List[Tuple[int, str]], page: Path,
                 surface: Surface) -> List[Violation]:
    """Lenient: only flag `flox.X` in inline code that exists nowhere."""
    out: List[Violation] = []
    rel = page.relative_to(REPO_ROOT).as_posix()
    if rel.startswith("docs/reference/quickjs/") or rel == "docs/bindings/javascript.md":
        # `flox` there is the QuickJS global namespace, whose surface lives
        # in quickjs/flox/*.js and is not modelled by this gate.
        return out
    known = surface.any_surface()
    for lineno, line in prose:
        for code in _INLINE_CODE_RE.findall(line):
            if _FILENAME_RE.match(code.strip()):
                continue  # `flox.toml`, `flox.d.ts`, `flox.cpp`: not APIs
            for match in _ATTR_CHAIN_RE.finditer(code):
                if match.group("root") not in PY_ALIASES:
                    continue
                head = match.group("rest").split(".")[0]
                if head in known or head in surface.py_modules:
                    continue
                out.append(Violation(
                    page, lineno, f"flox.{head}",
                    f"`{match.group('root')}.{head}` appears in no binding "
                    f"surface (Python / Node / Codon)"))
    return out


# --------------------------------------------------------------------------
# Allowlist
# --------------------------------------------------------------------------

def _load_allowlist() -> Set[str]:
    if not ALLOWLIST_PATH.exists():
        return set()
    out: Set[str] = set()
    for raw in ALLOWLIST_PATH.read_text(encoding="utf-8").splitlines():
        line = raw.split("#", 1)[0].strip()
        if line:
            out.add(line)
    return out


def _allowed(violation: Violation, allowlist: Set[str]) -> bool:
    rel = violation.page.relative_to(REPO_ROOT).as_posix()
    bare = violation.symbol.split(".")[-1]
    return any(key in allowlist for key in (
        violation.symbol,
        bare,
        f"{rel}:{violation.symbol}",
        f"{rel}:{bare}",
    ))


# --------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Check that API symbols named in hand-written docs "
                    "exist in the Python / Node / Codon surface."
    )
    parser.add_argument("--quiet", action="store_true",
                        help="only print failures")
    parser.add_argument("--list", action="store_true",
                        help="print unique unknown symbols in "
                             "allowlist-file format and exit 0")
    args = parser.parse_args()

    surface = load_surface()
    if not surface.py_root:
        print("::error::empty Python surface — is "
              "python/flox_py/_flox_py/__init__.pyi missing?", file=sys.stderr)
        return 1
    if not surface.node_exports:
        print("::error::empty Node surface — is node/index.d.ts missing?",
              file=sys.stderr)
        return 1

    allowlist = _load_allowlist()
    pages = sorted(p for p in DOCS_DIR.rglob("*.md") if p not in EXCLUDED_DOCS)

    violations: List[Violation] = []
    allowed_hits = 0
    for page in pages:
        blocks, prose = _segment(page, surface)
        page_bound = _page_bound_names(blocks)
        found: List[Violation] = []
        for block in blocks:
            if block.kind == "python":
                found += _check_python_block(block, page, surface, page_bound)
            elif block.kind == "node":
                found += _check_node_block(block, page, surface)
            elif block.kind == "codon":
                found += _check_codon_block(block, page, surface)
        found += _check_prose(prose, page, surface)
        for violation in found:
            if _allowed(violation, allowlist):
                allowed_hits += 1
            else:
                violations.append(violation)

    if args.list:
        seen: Dict[str, List[str]] = {}
        for violation in violations:
            rel = violation.page.relative_to(REPO_ROOT).as_posix()
            seen.setdefault(violation.symbol, []).append(f"{rel}:{violation.line}")
        for symbol in sorted(seen):
            print(f"{symbol}  # {', '.join(seen[symbol][:3])}")
        return 0

    rc = 0
    if violations:
        rc = 1
        print(f"::error::{len(violations)} doc symbol(s) do not exist in any "
              f"binding surface:", file=sys.stderr)
        for violation in sorted(violations, key=lambda v: (str(v.page), v.line)):
            rel = violation.page.relative_to(REPO_ROOT).as_posix()
            print(f"::error file={rel},line={violation.line}::"
                  f"{violation.message}", file=sys.stderr)
        print("Fix the doc, or — if the reference is legitimate prose that "
              "only looks like an API call — add the symbol to "
              "scripts/doc_symbols_allow.txt with a reason.", file=sys.stderr)

    if not args.quiet:
        print(f"Resolved doc symbols across {len(pages)} pages against "
              f"{len(surface.py_root)} Python names, "
              f"{len(surface.node_exports)} Node exports, "
              f"{len(surface.codon_modules)} Codon modules "
              f"({allowed_hits} allowlisted hit(s)).")
        if rc == 0:
            print("OK: every API symbol named in the docs exists.")
    return rc


if __name__ == "__main__":
    sys.exit(main())

"""Shared setup for the binding-gate tests.

The gates under test (`scripts/check_binding_parity.py`,
`scripts/check_binding_smoke.py`) read the whole repository. Running them
against the real tree proves only that the tree is currently in whatever
state it is in; it cannot show that the gate would have *caught* a
regression. So every behavioural test here builds a miniature repository
with the same directory layout, points the gate at it with `--root`, checks
that the gate is green, then removes exactly one thing and checks that the
gate is red and names what went missing.

`--root` is the seam these tests need: the gate must resolve every file it
reads under that directory instead of under its own location. The
`flox_codegen` import stays bound to the script's own path -- the fixture
tree carries no codegen package.
"""

from __future__ import annotations

import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPTS = REPO / "scripts"
PARITY = SCRIPTS / "check_binding_parity.py"
SMOKE = SCRIPTS / "check_binding_smoke.py"


@dataclass
class Run:
    returncode: int
    stdout: str
    stderr: str

    @property
    def output(self) -> str:
        return self.stdout + self.stderr


def run_script(script: Path, args: list[str], env: dict[str, str] | None = None) -> Run:
    r = subprocess.run([sys.executable, str(script), *args],
                       capture_output=True, text=True, env=env, cwd=REPO)
    return Run(r.returncode, r.stdout, r.stderr)


def run_parity(args: list[str]) -> Run:
    return run_script(PARITY, args)


def run_smoke(args: list[str], env: dict[str, str] | None = None) -> Run:
    return run_script(SMOKE, args, env)


# ── The miniature repository ──────────────────────────────────────────

IDL = """\
#pragma once
extern "C"
{
  FLOX_EXPORT(group = "widget")
  FloxWidgetHandle flox_widget_create(void);
  FLOX_EXPORT(group = "widget")
  void flox_widget_destroy(FloxWidgetHandle widget);
  FLOX_EXPORT(group = "widget")
  int32_t flox_widget_size(FloxWidgetHandle widget);
}
"""

PARITY_YAML = """\
groups:
  widget:
    pybind11: { status: required, classes: [Widget] }
    napi: { status: required, classes: [Widget] }
    codon: { status: required }
    quickjs:
      status: required
      classes: [Widget]
      functions:
        flox_widget_create: __flox_widget_create
        flox_widget_destroy: __flox_widget_destroy
        flox_widget_size: __flox_widget_size
"""

# Generated from the same IDL the gate reads, so every group is present here
# by construction. A Codon check that reads this file can never fail.
CODON_GOLDEN = """\
# Generated from flox_capi_spec.hpp. Do not edit.

# ── widget ──
from C import flox_widget_create() -> cobj
from C import flox_widget_destroy(cobj)
from C import flox_widget_size(cobj) -> i32
"""

# The shipped Codon module. Unlike the golden, this is hand-written and can
# fall behind the C ABI -- which is the whole point of checking it instead.
CODON_MODULE = """\
# flox/widget.codon -- the shipped Codon wrapper.

from C import flox_widget_create() -> cobj
from C import flox_widget_destroy(cobj)
from C import flox_widget_size(cobj) -> i32


class Widget:
    _handle: cobj

    def __init__(self):
        self._handle = flox_widget_create()

    def size(self) -> int:
        return int(flox_widget_size(self._handle))
"""

PYI = """\
class Widget:
    def __init__(self) -> None: ...
    def size(self) -> int: ...
"""

DTS = """\
export class Widget {
  constructor();
  size(): number;
}
"""

PYBIND_SOURCE = """\
#include <flox/capi/flox_capi.h>

void registerWidget(py::module_& m)
{
  py::class_<Widget>(m, "Widget")
      .def(py::init([]() { return flox_widget_create(); }))
      .def("size", [](Widget& w) { return flox_widget_size(w.handle); });
  m.add_object("_widget_cleanup", py::capsule(flox_widget_destroy));
}
"""

NAPI_SOURCE = """\
#include <flox/capi/flox_capi.h>

Napi::Value WidgetNew(const Napi::CallbackInfo& info)
{
  return Wrap(flox_widget_create());
}
Napi::Value WidgetSize(const Napi::CallbackInfo& info)
{
  return Napi::Number::New(info.Env(), flox_widget_size(Handle(info)));
}
void WidgetFinalize(void* h) { flox_widget_destroy(h); }
"""

QUICKJS_BINDINGS = """\
#include "quickjs.h"

static void registerWidget(JSContext* ctx)
{
  addGlobalFunc(ctx, "__flox_widget_create", jsWidgetCreate, 0);
  addGlobalFunc(ctx, "__flox_widget_destroy", jsWidgetDestroy, 1);
  addGlobalFunc(ctx, "__flox_widget_size", jsWidgetSize, 1);
}
"""

# scan_quickjs reads classes out of the raw-string JS prelude, indented by
# exactly four spaces -- the shape the real js_strategy.cpp has.
QUICKJS_PRELUDE = R'''
static const char* kPrelude = R"JS(
  (function() {
    class Widget {
      constructor() { this.h = __flox_widget_create(); }
      size() { return __flox_widget_size(this.h); }
    }
    globalThis.Widget = Widget;
  })();
)JS";
'''


class ParityTree:
    """A miniature repository the parity gate can be pointed at."""

    FILES = {
        "include/flox/capi/flox_capi_spec.hpp": IDL,
        "tools/codegen/binding_parity.yaml": PARITY_YAML,
        "tools/codegen/golden/flox_capi.codon": CODON_GOLDEN,
        "codon/flox/widget.codon": CODON_MODULE,
        "python/flox_py/_flox_py/__init__.pyi": PYI,
        "python/widget_bindings.h": PYBIND_SOURCE,
        "node/index.d.ts": DTS,
        "node/src/widget.h": NAPI_SOURCE,
        "src/quickjs/js_bindings.cpp": QUICKJS_BINDINGS,
        "src/quickjs/js_strategy.cpp": QUICKJS_PRELUDE,
    }

    def __init__(self, root: Path):
        self.root = root
        for rel, text in self.FILES.items():
            self.write(rel, text)

    def write(self, rel: str, text: str) -> None:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)

    def read(self, rel: str) -> str:
        return (self.root / rel).read_text()

    def drop_line(self, rel: str, needle: str) -> None:
        """Remove every line containing `needle` -- the mutation these tests use."""
        kept = [ln for ln in self.read(rel).splitlines(True) if needle not in ln]
        assert len(kept) < len(self.read(rel).splitlines(True)), \
            f"{needle} not found in {rel}; the fixture drifted"
        (self.root / rel).write_text("".join(kept))

    def run(self, *args: str) -> Run:
        return run_parity(["--root", str(self.root), *args])


@pytest.fixture
def parity_tree(tmp_path: Path) -> ParityTree:
    return ParityTree(tmp_path / "tree")

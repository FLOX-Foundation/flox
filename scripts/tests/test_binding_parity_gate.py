"""Acceptance tests for scripts/check_binding_parity.py.

What the gate is supposed to promise, per docs/contributors/parity-gate.md:
"I added a function to the C ABI but forgot the pybind11/NAPI wrapper" is a
CI failure. It did not keep that promise. It matched class names in the
`.pyi` / `.d.ts` -- a name that satisfies a whole group no matter how many
functions the group gained -- and it checked Codon against a file generated
from the same IDL it had just read, which cannot disagree with it. QuickJS
was only checked for the groups that happened to declare it.

These tests pin the behaviour, not the current contents of the tree: each
builds a miniature repository, asserts the gate is green on it, removes one
function reference, and asserts the gate turns red and says which function
and which binding.
"""

from __future__ import annotations

from conftest import run_parity


def test_a_function_dropped_from_the_pybind11_source_turns_the_gate_red(parity_tree):
    """The class name stays in the .pyi; only the wrapper is gone."""
    baseline = parity_tree.run()
    assert baseline.returncode == 0, baseline.output

    parity_tree.drop_line("python/widget_bindings.h", "flox_widget_size")
    assert "class Widget" in parity_tree.read("python/flox_py/_flox_py/__init__.pyi")

    r = parity_tree.run()
    assert r.returncode == 1, r.output
    assert "flox_widget_size" in r.output
    assert "pybind11" in r.output


def test_a_function_dropped_from_the_napi_source_turns_the_gate_red(parity_tree):
    baseline = parity_tree.run()
    assert baseline.returncode == 0, baseline.output

    parity_tree.drop_line("node/src/widget.h", "flox_widget_size")
    assert "export class Widget" in parity_tree.read("node/index.d.ts")

    r = parity_tree.run()
    assert r.returncode == 1, r.output
    assert "flox_widget_size" in r.output
    assert "napi" in r.output


def test_the_codon_half_reads_the_shipped_module_and_not_the_golden(parity_tree):
    """Dropping the import from codon/flox/*.codon must fail even though the
    IDL-generated golden still lists it -- the golden cannot disagree with the
    IDL, so a check against it proves nothing about what Codon can call."""
    baseline = parity_tree.run()
    assert baseline.returncode == 0, baseline.output

    parity_tree.drop_line("codon/flox/widget.codon", "flox_widget_size")
    assert "flox_widget_size" in parity_tree.read("tools/codegen/golden/flox_capi.codon")

    r = parity_tree.run()
    assert r.returncode == 1, r.output
    assert "flox_widget_size" in r.output
    assert "codon" in r.output


def test_require_quickjs_is_on_by_default(parity_tree):
    """A group with no `quickjs` entry is a failure unless it is turned off."""
    parity_tree.write("tools/codegen/binding_parity.yaml", """\
groups:
  widget:
    pybind11: { status: required, classes: [Widget] }
    napi: { status: required, classes: [Widget] }
    codon: { status: required }
""")

    r = parity_tree.run()
    assert r.returncode == 1, r.output
    assert "quickjs" in r.output

    off = parity_tree.run("--no-require-quickjs")
    assert off.returncode == 0, off.output


def test_a_new_c_function_with_no_wrapper_is_red_in_all_four_bindings(parity_tree):
    """The mutation the gate exists for: the C ABI grows a function and every
    binding stays as it was."""
    baseline = parity_tree.run()
    assert baseline.returncode == 0, baseline.output

    idl = parity_tree.read("include/flox/capi/flox_capi_spec.hpp")
    idl = idl.replace("}\n", '  FLOX_EXPORT(group = "widget")\n'
                             "  void flox_widget_reset(FloxWidgetHandle widget);\n}\n")
    parity_tree.write("include/flox/capi/flox_capi_spec.hpp", idl)

    r = parity_tree.run()
    assert r.returncode == 1, r.output
    assert r.output.count("flox_widget_reset") >= 4, r.output
    for binding in ("pybind11", "napi", "codon", "quickjs"):
        assert binding in r.output, f"{binding} stayed green: {r.output}"


def test_the_real_repository_passes_its_own_gate():
    """Run with no arguments, the way CI does. `--require-quickjs` being the
    default means no group may be left undeclared."""
    r = run_parity([])
    assert "were NOT checked" not in r.output, r.output
    assert "undeclared" not in r.output, r.output
    assert r.returncode == 0, r.output

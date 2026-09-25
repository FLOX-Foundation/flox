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


# ── Second pass: holes a mutation run found ───────────────────────────
#
# Each test below corresponds to a mutation that survived the first suite.
# The pattern is the same -- green fixture, one targeted change, red gate --
# but the change is chosen so that a *weaker* check (a prefix match instead
# of exact membership, a mapping nobody verifies, a comment counted as code)
# would let it through.


def test_a_call_site_that_merely_starts_with_the_name_does_not_satisfy_it(parity_tree):
    """`flox_widget_create_ex` is a different function. Matching it as
    "reachable" for `flox_widget_create` is how a prefix match reads a
    neighbouring wrapper as the one that is missing."""
    baseline = parity_tree.run()
    assert baseline.returncode == 0, baseline.output

    for rel in ("python/widget_bindings.h", "node/src/widget.h",
                "codon/flox/widget.codon"):
        parity_tree.write(rel, parity_tree.read(rel).replace(
            "flox_widget_create", "flox_widget_create_ex"))

    r = parity_tree.run()
    assert r.returncode == 1, r.output
    assert r.output.count("flox_widget_create'") >= 3, r.output
    for binding in ("pybind11", "napi", "codon"):
        assert binding in r.output, f"{binding} accepted the prefix: {r.output}"


def test_a_quickjs_global_that_is_not_registered_is_red(parity_tree):
    """The map is a claim about `addGlobalFunc`, not a fact. A name nobody
    registered is a strategy calling into nothing."""
    baseline = parity_tree.run()
    assert baseline.returncode == 0, baseline.output

    parity_tree.write("tools/codegen/binding_parity.yaml",
                      parity_tree.read("tools/codegen/binding_parity.yaml").replace(
                          "flox_widget_size: __flox_widget_size",
                          "flox_widget_size: __flox_widget_size_typo"))

    r = parity_tree.run()
    assert r.returncode == 1, r.output
    assert "quickjs" in r.output
    assert "__flox_widget_size_typo" in r.output, r.output


def test_a_quickjs_global_that_is_only_a_prefix_of_a_registration_is_red(parity_tree):
    """`__flox_widget_size` is not registered; `__flox_widget_size_ex` is.
    A prefix match would call that satisfied."""
    parity_tree.write("src/quickjs/js_bindings.cpp",
                      parity_tree.read("src/quickjs/js_bindings.cpp").replace(
                          '"__flox_widget_size"', '"__flox_widget_size_ex"'))

    r = parity_tree.run()
    assert r.returncode == 1, r.output
    assert "quickjs" in r.output
    assert "__flox_widget_size" in r.output, r.output


def test_a_function_named_only_in_a_comment_is_not_reachable(parity_tree):
    """A comment is documentation, not a wrapper. Both comment forms, because
    the scanner strips them separately."""
    baseline = parity_tree.run()
    assert baseline.returncode == 0, baseline.output

    parity_tree.write("python/widget_bindings.h", """\
#include <flox/capi/flox_capi.h>

// flox_widget_size is reached through the C++ class, not the C ABI.
void registerWidget(py::module_& m)
{
  py::class_<Widget>(m, "Widget")
      .def(py::init([]() { return flox_widget_create(); }));
  m.add_object("_widget_cleanup", py::capsule(flox_widget_destroy));
}
""")
    parity_tree.write("node/src/widget.h", """\
#include <flox/capi/flox_capi.h>

/* The size accessor used to call flox_widget_size here; it now goes
   through the cached handle instead. */
Napi::Value WidgetNew(const Napi::CallbackInfo& info)
{
  return Wrap(flox_widget_create());
}
void WidgetFinalize(void* h) { flox_widget_destroy(h); }
""")

    r = parity_tree.run()
    assert r.returncode == 1, r.output
    assert r.output.count("flox_widget_size") >= 2, r.output
    for binding in ("pybind11", "napi"):
        assert binding in r.output, f"{binding} read a comment as a call site: {r.output}"


def test_a_required_quickjs_group_with_no_mapped_functions_is_red(parity_tree):
    """A `required` entry that maps nothing has verified nothing. Empty map
    and absent key are the same claim and must fail the same way."""
    header = """\
groups:
  widget:
    pybind11: { status: required, classes: [Widget] }
    napi: { status: required, classes: [Widget] }
    codon: { status: required }
    quickjs:
      status: required
      classes: [Widget]
"""
    for tail in ("", "      functions: {}\n"):
        parity_tree.write("tools/codegen/binding_parity.yaml", header + tail)
        r = parity_tree.run()
        assert r.returncode == 1, f"functions:{tail!r} passed: {r.output}"
        assert "quickjs" in r.output
        assert r.output.count("flox_widget_") >= 3, r.output


def test_an_idl_group_absent_from_the_manifest_is_red(parity_tree):
    """A group that grows in the IDL and is declared nowhere is the case the
    manifest exists to make impossible -- it must not pass by being skipped."""
    baseline = parity_tree.run()
    assert baseline.returncode == 0, baseline.output

    idl = parity_tree.read("include/flox/capi/flox_capi_spec.hpp")
    idl = idl.replace("}\n", '  FLOX_EXPORT(group = "gadget")\n'
                             "  void flox_gadget_reset(FloxGadgetHandle gadget);\n}\n")
    parity_tree.write("include/flox/capi/flox_capi_spec.hpp", idl)

    r = parity_tree.run()
    assert r.returncode == 1, r.output
    assert "gadget" in r.output
    assert "binding_parity.yaml" in r.output, r.output

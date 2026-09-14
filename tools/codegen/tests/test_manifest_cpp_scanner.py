"""Unit tests for the lightweight C++ header scanner that feeds
`binding_manifest.json` -- independent of libclang."""
from __future__ import annotations

from flox_codegen.manifest import scan_cpp


_HEADER_WITH_PRIVATE_NESTED_HELPERS = """\
class Widget
{
 public:
  void compute();

 private:
  struct Node
  {
    int x;
  };

  struct Helper
  {
    void release();
  };

  void resetAll();
};
"""


def test_outer_class_methods_stay_attributed_after_a_private_nested_type():
    _, methods = scan_cpp({"widget.h": _HEADER_WITH_PRIVATE_NESTED_HELPERS})
    assert ("Widget", "compute") in methods
    # resetAll() is declared textually after two private nested helper
    # structs (Node, Helper). Before the fix, the scanner tracked a single
    # "current owner" reassigned by every class/struct match regardless of
    # nesting, so a method declared after a nested type was attributed to
    # that nested type instead of the class it is actually a member of.
    assert ("Widget", "resetAll") in methods
    assert ("Node", "resetAll") not in methods
    assert ("Helper", "resetAll") not in methods


def test_private_nested_types_are_not_listed_as_top_level_symbols():
    types, _ = scan_cpp({"widget.h": _HEADER_WITH_PRIVATE_NESTED_HELPERS})
    names = {name for name, _ in types}
    assert names == {"Widget"}


def test_two_top_level_types_in_one_header_stay_scoped_independently():
    text = """\
class First
{
 public:
  void a();
};

class Second
{
 public:
  void b();
};
"""
    types, methods = scan_cpp({"two.h": text})
    assert sorted(name for name, _ in types) == ["First", "Second"]
    assert ("First", "a") in methods
    assert ("Second", "b") in methods
    assert ("First", "b") not in methods
    assert ("Second", "a") not in methods

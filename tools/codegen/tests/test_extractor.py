"""End-to-end tests: tiny annotated TUs → IR.

These tests exercise the libclang glue, the spec-file source filter, and the
annotation parser against handcrafted minimal headers. They do NOT depend on
the engine include tree.
"""
from __future__ import annotations

import textwrap
from pathlib import Path

from lrvx_codegen import extractor


# Resolve the repo's lrvx_export.h once — the fixtures all #include it.
_REPO_ROOT = Path(__file__).resolve().parents[3]
_INCLUDE_DIR = _REPO_ROOT / "include"


def _write_spec(tmp_path: Path, body: str) -> Path:
    p = tmp_path / "spec.hpp"
    p.write_text(
        textwrap.dedent(
            """\
            #pragma once
            #include <stddef.h>
            #include <stdint.h>
            #include "lrvx/capi/lrvx_export.h"
            #ifdef __cplusplus
            extern "C" {
            #endif
            """
        )
        + textwrap.dedent(body)
        + textwrap.dedent(
            """
            #ifdef __cplusplus
            }
            #endif
            """
        )
    )
    return p


def test_extracts_simple_function(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        LRVX_EXPORT(group="x")
        void lrvx_demo(int a, double b);
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    assert len(mod.functions) == 1
    fn = mod.functions[0]
    assert fn.name == "lrvx_demo"
    assert fn.return_type == "void"
    assert fn.annotations == {"group": "x"}
    assert [(p.name, p.type) for p in fn.params] == [("a", "int"), ("b", "double")]


def test_internal_only_suppresses_function(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        LRVX_EXPORT(internal_only)
        void lrvx_hidden(void);

        LRVX_EXPORT(group="x")
        void lrvx_visible(void);
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    names = [fn.name for fn in mod.functions]
    assert names == ["lrvx_visible"]


def test_drops_unannotated_functions(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        // Plain — no LRVX_EXPORT, must not appear.
        void unrelated_helper(int);

        LRVX_EXPORT(group="x")
        int lrvx_keep(int a);
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    assert [fn.name for fn in mod.functions] == ["lrvx_keep"]


def test_groups_partition(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        LRVX_EXPORT(group="a") void f1(void);
        LRVX_EXPORT(group="b") void f2(void);
        LRVX_EXPORT(group="a") void f3(void);
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    grouped = mod.functions_by_group()
    assert sorted(grouped) == ["a", "b"]
    # Source order is preserved within a group.
    assert [fn.name for fn in grouped["a"]] == ["f1", "f3"]


def test_extracts_marked_macro_constant(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        // lrvx::export_macro(group="thing")
        #define LRVX_THING_A 0
        // lrvx::export_macro(group="thing")
        #define LRVX_THING_B 1
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    assert [(m.name, m.value, m.group) for m in mod.macros] == [
        ("LRVX_THING_A", "0", "thing"),
        ("LRVX_THING_B", "1", "thing"),
    ]


def test_unmarked_macro_is_dropped(tmp_path):
    # A #define with no lrvx::export_macro(...) marker directly above it is
    # invisible to codegen, exactly like a function with no LRVX_EXPORT(...).
    # This is the case a dropped marker regresses to -- it must never come
    # back as a silent pass.
    spec = _write_spec(
        tmp_path,
        '''
        #define LRVX_INTERNAL_DETAIL 7

        // lrvx::export_macro(group="thing")
        #define LRVX_THING_A 0
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    assert [m.name for m in mod.macros] == ["LRVX_THING_A"]


def test_macro_groups_partition(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        // lrvx::export_macro(group="a")
        #define LRVX_A_ONE 0
        // lrvx::export_macro(group="b")
        #define LRVX_B_ONE 0
        // lrvx::export_macro(group="a")
        #define LRVX_A_TWO 1
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    grouped = mod.macros_by_group()
    assert sorted(grouped) == ["a", "b"]
    assert [m.name for m in grouped["a"]] == ["LRVX_A_ONE", "LRVX_A_TWO"]


def test_macro_marker_with_no_following_define_raises(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        // lrvx::export_macro(group="thing")
        typedef struct { int32_t x; } LrvxNotAMacro;
        ''',
    )
    try:
        extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
        assert False, "expected ValueError for a dangling macro marker"
    except ValueError as e:
        assert "not followed by a #define" in str(e)


def test_macro_marker_missing_group_key_raises(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        // lrvx::export_macro()
        #define LRVX_THING_A 0
        ''',
    )
    try:
        extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
        assert False, "expected ValueError for a marker missing group="
    except ValueError as e:
        assert "missing group=" in str(e)


def test_picks_up_handles_structs_and_fnptrs(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        typedef void* LrvxThingHandle;

        typedef struct {
            int32_t x;
            uint8_t flags[4];
        } LrvxRecord;

        typedef void (*LrvxOnEvent)(void* user_data, int32_t code);

        typedef struct {
            LrvxOnEvent on_event;
            void* user_data;
        } LrvxBundle;

        LRVX_EXPORT(group="g")
        LrvxThingHandle lrvx_thing_create(void);
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    assert [h.name for h in mod.handles] == ["LrvxThingHandle"]
    assert [s.name for s in mod.structs] == ["LrvxRecord", "LrvxBundle"]
    assert [fp.name for fp in mod.function_pointers] == ["LrvxOnEvent"]
    record = mod.structs[0]
    assert record.fields[1].type == "uint8_t[4]"

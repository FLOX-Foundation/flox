"""End-to-end tests: tiny annotated TUs → IR.

These tests exercise the libclang glue, the spec-file source filter, and the
annotation parser against handcrafted minimal headers. They do NOT depend on
the engine include tree.
"""
from __future__ import annotations

import textwrap
from pathlib import Path

from flox_codegen import extractor


# Resolve the repo's flox_export.h once — the fixtures all #include it.
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
            #include "flox/capi/flox_export.h"
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
        FLOX_EXPORT(group="x")
        void flox_demo(int a, double b);
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    assert len(mod.functions) == 1
    fn = mod.functions[0]
    assert fn.name == "flox_demo"
    assert fn.return_type == "void"
    assert fn.annotations == {"group": "x"}
    assert [(p.name, p.type) for p in fn.params] == [("a", "int"), ("b", "double")]


def test_internal_only_suppresses_function(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        FLOX_EXPORT(internal_only)
        void flox_hidden(void);

        FLOX_EXPORT(group="x")
        void flox_visible(void);
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    names = [fn.name for fn in mod.functions]
    assert names == ["flox_visible"]


def test_drops_unannotated_functions(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        // Plain — no FLOX_EXPORT, must not appear.
        void unrelated_helper(int);

        FLOX_EXPORT(group="x")
        int flox_keep(int a);
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    assert [fn.name for fn in mod.functions] == ["flox_keep"]


def test_groups_partition(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        FLOX_EXPORT(group="a") void f1(void);
        FLOX_EXPORT(group="b") void f2(void);
        FLOX_EXPORT(group="a") void f3(void);
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    grouped = mod.functions_by_group()
    assert sorted(grouped) == ["a", "b"]
    # Source order is preserved within a group.
    assert [fn.name for fn in grouped["a"]] == ["f1", "f3"]


def test_picks_up_handles_structs_and_fnptrs(tmp_path):
    spec = _write_spec(
        tmp_path,
        '''
        typedef void* FloxThingHandle;

        typedef struct {
            int32_t x;
            uint8_t flags[4];
        } FloxRecord;

        typedef void (*FloxOnEvent)(void* user_data, int32_t code);

        typedef struct {
            FloxOnEvent on_event;
            void* user_data;
        } FloxBundle;

        FLOX_EXPORT(group="g")
        FloxThingHandle flox_thing_create(void);
        ''',
    )
    mod = extractor.parse_spec(spec, include_dirs=[_INCLUDE_DIR])
    assert [h.name for h in mod.handles] == ["FloxThingHandle"]
    assert [s.name for s in mod.structs] == ["FloxRecord", "FloxBundle"]
    assert [fp.name for fp in mod.function_pointers] == ["FloxOnEvent"]
    record = mod.structs[0]
    assert record.fields[1].type == "uint8_t[4]"

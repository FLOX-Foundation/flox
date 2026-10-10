"""Unit tests for the annotation grammar — independent of libclang."""
from __future__ import annotations

import pytest

from lrvx_codegen.extractor import parse_annotation


def test_empty_body():
    assert parse_annotation("lrvx::export()") == {}


def test_single_kv():
    assert parse_annotation('lrvx::export(c_name="lrvx_x")') == {"c_name": "lrvx_x"}


def test_multiple_kv():
    got = parse_annotation('lrvx::export(c_name="lrvx_x", group="indicator")')
    assert got == {"c_name": "lrvx_x", "group": "indicator"}


def test_bare_flag():
    got = parse_annotation('lrvx::export(pointer_out_wrapper)')
    assert got == {"pointer_out_wrapper": ""}


def test_mixed_kv_and_flag():
    got = parse_annotation('lrvx::export(c_name="lrvx_x", pointer_out_wrapper)')
    assert got == {"c_name": "lrvx_x", "pointer_out_wrapper": ""}


def test_bare_value_unquoted():
    # Bare values are accepted for ergonomics; grammar restricts charset.
    got = parse_annotation("lrvx::export(group=indicator)")
    assert got == {"group": "indicator"}


def test_whitespace_tolerant():
    # `#__VA_ARGS__` may stringify with surrounding whitespace; parser is loose.
    got = parse_annotation('lrvx::export(  c_name = "lrvx_x" ,  group = "stat" )')
    assert got == {"c_name": "lrvx_x", "group": "stat"}


def test_rejects_non_lrvx_export():
    with pytest.raises(ValueError):
        parse_annotation("clang::annotate(other)")


def test_rejects_garbage_body():
    with pytest.raises(ValueError):
        parse_annotation("lrvx::export(=missing_key)")

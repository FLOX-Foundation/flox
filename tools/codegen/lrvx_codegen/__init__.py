"""lrvx_codegen — IDL-driven code generation for the lrvx C API.

Pipeline (per .notes/api-idl-rfc.md):

    annotated C++ headers  --libclang-->  IR  --emitters-->  lrvx_capi.h, .pyi, .d.ts, ...

This package implements the IR and the extractor; per-binding emitters live
alongside (emit_capi.py for now, more to follow).
"""
__version__ = "0.1.0"

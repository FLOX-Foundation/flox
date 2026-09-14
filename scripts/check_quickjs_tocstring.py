#!/usr/bin/env python3
"""Guard the QuickJS string boundary.

JS_ToCString returns nullptr for any value that cannot be converted to a
string (a Symbol, a throwing toString, an out-of-memory runtime). A call
site that skips the test hands that nullptr to a C function or a
std::string constructor, and the process dies on strlen(nullptr).

Two rules, both checked here:

1. No file under src/quickjs/ calls JS_ToCString directly. The owning
   holder in js_cstring.h is the single entry point, so a new call site
   inherits the free-on-every-path behaviour instead of re-deriving it.

2. Every holder is tested before its pointer is used: an early-out
   (FLOX_JS_CSTRING_OR_THROW, or an explicit `if (!x)`), an `if (x)`
   that wraps the uses, or a `x ? x : fallback` at every use.

Run with -v to list every site and how it is guarded.
"""
import re
import sys
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "quickjs"
HOLDER_HEADER = "js_cstring.h"

RAW_CALL = re.compile(r"\bJS_ToCString\s*\(")
RAW_FREE = re.compile(r"\bJS_FreeCString\s*\(")
DECL = re.compile(r"^\s*(?:flox::)?JsCString (\w+)\s*[({]")
MACRO = re.compile(r"^\s*FLOX_JS_CSTRING_OR(?:_THROW)?\(\s*(\w+)\s*,")
OPTIONAL = re.compile(r"^\s*(?:flox::)?JsCString (\w+)\s*=\s*(?:flox::)?jsOptionalCString\(")
SCOPE = 60


def is_comment(line):
    s = line.strip()
    return s.startswith("//") or s.startswith("*") or s.startswith("/*")


def strip_literals(line):
    """Drop string literals and trailing comments so an identifier spelled
    inside an error message is not mistaken for a use."""
    line = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)
    line = re.sub(r"'(?:[^'\\]|\\.)*'", "''", line)
    return line.split("//")[0]


def classify(lines, idx, var):
    word = re.compile(r"\b%s\b" % re.escape(var))
    IF = re.compile(r"^(?:\}\s*)?(?:else\s+)?if\s*\((.*)\)\s*\{?\s*$")

    def is_null_test(stripped):
        m = IF.match(stripped)
        if not m:
            return False
        for tok in re.split(r"\|\||&&", m.group(1)):
            tok = tok.strip()
            while tok.startswith("(") and tok.endswith(")"):
                tok = tok[1:-1].strip()
            if tok.startswith("!"):
                tok = tok[1:].strip()
            if tok in (var, "%s.ok()" % var, "%s.present()" % var):
                return True
            if re.fullmatch(r"%s\s*[!=]=\s*(?:nullptr|NULL)" % re.escape(var), tok):
                return True
        return False

    raw_uses = []
    for j, raw_line in enumerate(lines[idx + 1: idx + 1 + SCOPE]):
        if raw_line.startswith("}"):
            break
        if is_comment(raw_line):
            continue
        line = strip_literals(raw_line)
        if not word.search(line):
            continue
        stripped = line.strip()
        if is_null_test(stripped):
            if not raw_uses:
                return True, "tested at line %d" % (idx + j + 2)
            continue
        # `x.str()`, `x.or_else(...)` and `x.ok()` are null-safe by construction
        safe = re.sub(
            r"\b%s\.(?:str|or_else|ok|present)\s*\(" % re.escape(var), "SAFE(", line
        )
        # `x ? x : fallback` is a guarded use
        safe = re.sub(r"%s\s*\?\s*%s\s*:" % (re.escape(var), re.escape(var)), "", safe)
        if not word.search(safe):
            continue
        raw_uses.append(idx + j + 2)

    if not raw_uses:
        return True, "no unguarded use"
    return False, "used at line(s) %s" % ",".join(str(u) for u in raw_uses[:3])


def main():
    verbose = "-v" in sys.argv
    failures = []
    sites = {"guarded": [], "unguarded": []}
    raw = []

    for f in sorted(SRC.glob("*.cpp")) + sorted(SRC.glob("*.h")):
        lines = f.read_text().splitlines()
        rel = str(f.relative_to(ROOT))
        for i, line in enumerate(lines):
            if is_comment(line):
                continue
            if f.name != HOLDER_HEADER and (RAW_CALL.search(line) or RAW_FREE.search(line)):
                raw.append((rel, i + 1, line.strip()))
            m = DECL.match(line) or MACRO.match(line)
            if not m:
                continue
            var = m.group(1)
            if MACRO.match(line) or OPTIONAL.match(line):
                # the macro early-outs by definition; the optional factory
                # returns an absent holder whose caller has to ask present()
                ok, why = classify(lines, i, var)
                if MACRO.match(line):
                    ok, why = True, "early-out macro"
            else:
                ok, why = classify(lines, i, var)
            sites["guarded" if ok else "unguarded"].append((rel, i + 1, var, why))

    total = len(sites["guarded"]) + len(sites["unguarded"])
    print("JsCString sites            : %d" % total)
    print("  guarded                  : %d" % len(sites["guarded"]))
    print("  unguarded                : %d" % len(sites["unguarded"]))
    print("raw JS_ToCString/FreeCString outside %s: %d" % (HOLDER_HEADER, len(raw)))

    if verbose:
        for rel, ln, var, why in sites["guarded"]:
            print("  ok   %s:%d %s (%s)" % (rel, ln, var, why))

    for rel, ln, txt in raw:
        failures.append("%s:%d calls the raw QuickJS string API: %s" % (rel, ln, txt))
    for rel, ln, var, why in sites["unguarded"]:
        failures.append("%s:%d '%s' is used without a null test (%s)" % (rel, ln, var, why))

    if failures:
        print("\nFAIL")
        for f in failures:
            print("  " + f)
        print(
            "\nConvert the site to FLOX_JS_CSTRING_OR_THROW, or test .ok() "
            "before the pointer reaches a consumer. See src/quickjs/js_cstring.h."
        )
        return 1
    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""
scripts/gen_indicator_docs.py

Reads include/flox/indicator/registry.def and rewrites the auto-generated
sections in the per-binding indicator docs. Adding an indicator to the
registry → it appears in the docs without manual edits to N markdown
files.

Auto-gen sections are bounded by:
    <!-- INDICATOR-LIST-START -->
    ...generated content...
    <!-- INDICATOR-LIST-END -->

Anything outside those markers is preserved verbatim. Files without the
markers are skipped.

CI gate: this script should be run before push; if `git diff docs/` is
non-empty after running, docs are stale.

Usage:
    python3 scripts/gen_indicator_docs.py
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
REGISTRY = REPO / "include" / "flox" / "indicator" / "registry.def"

START = "<!-- INDICATOR-LIST-START -->"
END = "<!-- INDICATOR-LIST-END -->"


def parse_registry() -> list[dict]:
    pattern = re.compile(
        r"^FLOX_INDICATOR\(\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*\((.*?)\)\s*\)\s*$"
    )
    indicators = []
    for raw in REGISTRY.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("//"):
            continue
        m = pattern.match(line)
        if not m:
            continue
        cls, snake, kind, args = m.groups()
        indicators.append(
            {
                "class": cls,
                "snake": snake,
                "kind": kind,
                "args": args.strip(),
            }
        )
    return indicators


def py_signature(ind: dict) -> str:
    args = ind["args"]
    args = args.replace("size_t ", "").replace("double ", "")
    return f"flox.{ind['class']}({args})"


def py_block(indicators: list[dict]) -> str:
    lines = [
        "Every indicator below is **one Python class** with both a batch",
        "`compute()` method and streaming `update()`/`value`/`ready`/`reset()`.",
        "Same instance, two ways to use it:",
        "",
        "```python",
        "import flox_py as flox",
        "ema = flox.EMA(10)",
        "out = ema.compute(prices)             # batch",
        "for v in stream:",
        "    ema.update(v)",
        "    if ema.ready: print(ema.value)    # streaming on the same instance",
        "```",
        "",
        "| Indicator | Constructor | Kind |",
        "|---|---|---|",
    ]
    for ind in indicators:
        lines.append(f"| `{ind['class']}` | `{py_signature(ind)}` | {ind['kind']} |")
    lines.append("")
    lines.append("Discovery: `flox.list_indicators()` returns the full list at runtime.")
    return "\n".join(lines)


def js_signature(ind: dict, ctor_kw: str = "new ") -> str:
    args = ind["args"]
    args = args.replace("size_t ", "").replace("double ", "")
    return f"{ctor_kw}flox.{ind['class']}({args})"


def js_block(indicators: list[dict], runtime: str) -> str:
    lines = [
        f"Every indicator below is **one {runtime} class** with both a batch",
        "`compute()` method and streaming `update()` / `value` / `ready` / `reset()`.",
        "Same instance, two ways to use it:",
        "",
        "```js",
        "const flox = require('flox-node');",
        "const ema = new flox.EMA(10);",
        "const out = ema.compute(prices);            // batch",
        "for (const v of stream) {",
        "  ema.update(v);",
        "  if (ema.ready) console.log(ema.value);    // streaming on the same instance",
        "}",
        "```",
        "",
        "| Indicator | Constructor | Kind |",
        "|---|---|---|",
    ]
    for ind in indicators:
        lines.append(
            f"| `{ind['class']}` | `{js_signature(ind)}` | {ind['kind']} |"
        )
    lines.append("")
    return "\n".join(lines)


def cpp_block(indicators: list[dict]) -> str:
    lines = [
        "Every indicator below is **one C++ class** with both a batch",
        "`compute()` method and streaming `update()` / `value()` / `ready()` /",
        "`reset()`. Same instance, two ways to use it:",
        "",
        "```cpp",
        '#include "flox/indicator/ema.h"',
        "flox::indicator::EMA ema(10);",
        "auto out = ema.compute(prices);            // batch",
        "for (auto v : stream) {",
        "    ema.update(v);",
        "    if (ema.ready()) {",
        "        std::cout << ema.value();          // streaming on same instance",
        "    }",
        "}",
        "```",
        "",
        "| Indicator | Header | Constructor | Kind |",
        "|---|---|---|---|",
    ]
    for ind in indicators:
        header = f"flox/indicator/{ind['snake']}.h"
        if ind["snake"] == "tema":
            header = "flox/indicator/dema.h"
        lines.append(
            f"| `{ind['class']}` | `<{header}>` | `{ind['class']}({ind['args']})` | {ind['kind']} |"
        )
    lines.append("")
    return "\n".join(lines)


def codon_block(indicators: list[dict]) -> str:
    lines = [
        "Every indicator below is **one Codon class** with both a batch",
        "`compute()` method and streaming `update()` / `value` / `ready` / `reset()`.",
        "",
        "| Indicator | Constructor | Kind |",
        "|---|---|---|",
    ]
    for ind in indicators:
        lines.append(
            f"| `{ind['class']}` | `flox.{ind['class']}({ind['args']})` | {ind['kind']} |"
        )
    lines.append("")
    return "\n".join(lines)


def replace_block(path: Path, content: str) -> bool:
    if not path.exists():
        return False
    text = path.read_text()
    if START not in text or END not in text:
        return False
    # Match the entire block including blank lines around the marker, then
    # rewrite with a stable canonical form: blank line after START, blank
    # line before END.
    pattern = re.compile(
        rf"{re.escape(START)}.*?{re.escape(END)}", re.DOTALL
    )
    canonical = f"{START}\n\n{content}\n\n{END}"
    new = pattern.sub(canonical, text, count=1)
    if new != text:
        path.write_text(new)
        return True
    return False


def main() -> int:
    indicators = parse_registry()
    if not indicators:
        print("error: no indicators found in registry.def", file=sys.stderr)
        return 1

    targets = [
        (REPO / "docs" / "reference" / "python" / "indicators.md", py_block(indicators)),
        (REPO / "docs" / "reference" / "node" / "indicators.md", js_block(indicators, "Node.js")),
        (REPO / "docs" / "reference" / "quickjs" / "indicators.md", js_block(indicators, "QuickJS")),
        (REPO / "docs" / "reference" / "codon" / "indicators.md", codon_block(indicators)),
        # No docs/reference/cpp/ today; the C++ surface lives under
        # docs/reference/api/ — keep generating once added.
    ]

    changed_any = False
    for path, block in targets:
        if not path.exists():
            print(f"  skip   {path.relative_to(REPO)} (does not exist)")
            continue
        text = path.read_text()
        if START not in text:
            # Inject markers near the top so the user can re-run and the
            # auto-gen section will populate.
            inject = f"\n## Indicator catalog\n\n{START}\n\n{block}\n\n{END}\n"
            path.write_text(text.rstrip() + "\n" + inject)
            changed_any = True
            print(f"  inject {path.relative_to(REPO)}")
        else:
            if replace_block(path, block):
                changed_any = True
                print(f"  update {path.relative_to(REPO)}")
            else:
                print(f"  ok     {path.relative_to(REPO)}")

    if changed_any:
        print(f"\nUpdated {sum(1 for _ in targets)} files for {len(indicators)} indicators.")
    else:
        print(f"\nAll {len(targets)} doc files are already in sync with the {len(indicators)} registry entries.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

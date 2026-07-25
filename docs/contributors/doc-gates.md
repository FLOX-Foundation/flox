# Documentation gates

Generated pages (the indicator catalog, the Python API index, `llms.txt`)
have been gated against their source of truth for a long time. Hand-written
prose was not, and a full-tree audit found what that costs: pages
documenting functions that never existed, snippets that raise on the first
line, wrong struct sizes, orphan pages nobody could navigate to, and links
to files that were renamed years ago. Every generated-doc gate was green the
whole time — they simply do not read prose.

The five gates below close that hole. All of them are plain `python3`
scripts: no compiler, no network, no site build, a few seconds end to end.

## Run them locally

```bash
for gate in symbols examples nav links conventions; do
  python3 "scripts/check_doc_$gate.py" || break
done
```

Each script also takes `--help` and `--quiet`, prints `::error::` lines that
GitHub Actions turns into inline PR annotations, and exits non-zero on the
first real problem.

## The gates

### `check_doc_symbols.py` — the symbol must exist

Extracts every API-looking reference from `docs/**/*.md` and resolves it
against the real binding surface:

| Language | Reference forms | Surface of truth |
|---|---|---|
| Python | package attribute access, `from` imports, bare `Name(...)` calls nothing binds, attribute access on a variable whose class the snippet reveals | `python/flox_py/_flox_py/__init__.pyi`, `python/flox_py/__init__.pyi`, `python/flox_py/*.py` |
| Node | package attribute access, `new` expressions, destructured package imports, in `js` / `javascript` / `ts` blocks | `node/index.d.ts` |
| Codon | `from flox.<module> import X` in Codon blocks | `codon/flox/<module>.codon` |

Blocks are classified by fence language, by the enclosing `=== "..."` tab
label, and by page path — a Codon snippet tagged `python` inside a Codon tab
is still checked as Codon. QuickJS pages are skipped: there the namespace is
the embedded runtime's globals, which this gate does not model.

Prose is checked leniently — an inline-code reference only fails when the
name exists in *no* binding surface.

This gate is the one that catches an entire invented API: a page-length
walkthrough of a function that was never bound.

### `check_doc_examples.py` — the example must run

Executes every `docs/examples/*.py` and fails on a non-zero exit, then
syntax-checks every `docs/examples/*.js` with `node --check` (skipped with a
notice when `node` is absent).

Examples that cannot run in CI are listed in `SKIP_PY` at the top of the
script, each with its reason. The only entry today is the live ccxt example,
which needs network and credentials.

The compiled extension is not present in the docs-only CI job, so there the
gate degrades to a syntax check and says so. The real execution coverage
comes from the `linux-gcc` job, which runs the same script with
`--require-runtime` after building the bindings.

This is what makes the `--8<--` include pattern load-bearing: a page that
includes a real file instead of pasting a snippet inherits a red build the
moment that file rots. A pasted snippet inherits nothing.

### `check_doc_nav.py` — the page must be reachable

Every `docs/**/*.md` must appear in the `mkdocs.yml` nav, and every nav entry
must point at a file that exists. The theme enables `navigation.prune`, so a
page missing from nav is built but unreachable: no sidebar entry, no
breadcrumb, no next/previous link. Only site search finds it. The audit found
28 pages in that state.

`mkdocs.yml` carries a `!!python/name:` tag for the mermaid fence, so the
gate parses it with a tag-tolerant loader (and falls back to a regex scan of
the nav block when PyYAML is missing).

### `check_doc_links.py` — the link must resolve

Validates relative `.md` links, repo-relative links out of `docs/`, and
`#anchor` fragments. Anchor ids are reproduced the way python-markdown's
`toc` extension builds them, including `{#custom-id}` overrides and the
`_N` suffix on duplicate headings.

C++ lambda captures (`[&](const Order& o)`) are indistinguishable from
Markdown links by shape, so a target only counts as a link when it looks
like a path: it contains `.md`, contains `/`, or starts with `#`.

### `check_doc_conventions.py` — the conventions the docs keep breaking

| Rule | Fails on | Why |
|---|---|---|
| `PY_IMPORT` | a bare import of `flox` as a Python module | the installed distribution is `flox_py`; the bare name is an ImportError |
| `NODE_PKG` | importing the npm package as `flox` or `flox-node` | the published package is `@flox-foundation/flox` |
| `QUICKJS_REQ` | a module-loader call on a `docs/reference/quickjs/**` page | the embedded runtime injects classes as globals; prose stating that absence is allowed |
| `CMAKE_FLAG` | `FLOX_ENABLE_*` inside a runnable shell block | renamed to `FLOX_BUILD_*`; the alias warns and will be removed. The alias table in `build/feature-flags.md` is prose, so it is not flagged |
| `EMOJI` | any emoji | the project forbids them. `✓` and `✗` are table markers, not emoji |

Exemptions live in `_EXEMPTIONS` in the script, keyed by `(page, rule)`,
each with a reason. A stale exemption — one whose violation is gone — fails
the gate too, so the list cannot rot.

## Allowlists

Two gates carry an allowlist because both have unavoidable false positives:

* `scripts/doc_symbols_allow.txt` — one entry per line, either a bare symbol
  (`Foo`) or page-scoped (`docs/how-to/x.md:Foo`). **Prefer page-scoped**, so
  the same name used wrongly on a new page still fails.
* `_EXEMPTIONS` in `scripts/check_doc_conventions.py` — `(page, rule)` pairs.

Every entry needs a comment saying why. There are exactly three acceptable
justifications:

1. **Placeholder.** The name is one the reader supplies (`MyStrategy()`), not
   one the framework ships.
2. **Unmodelled surface.** The symbol is real but lives somewhere the gate
   does not read (the QuickJS globals).
3. **A real defect in a file the current change cannot touch.** Mark it
   `TODO:` and name the defect and its owner. These are debt, not policy —
   delete the entry with the fix.

An allowlist entry with no reason, or with a reason that boils down to "the
gate is annoying", is a request to re-introduce the exact class of defect the
audit found. Reviewers should treat one as a code change, not a config
tweak.

## The rule for new APIs

**If you add a public API, the prose that documents it must be an executable
example, or it will not be checked.**

A snippet pasted into Markdown is checked for *symbol existence* only. That
catches a name that never existed; it cannot catch a wrong argument order, a
renamed keyword, a changed return shape, or a struct whose size the page
states in bytes. The only mechanism that catches those is a file under
`docs/examples/` that CI runs, included in the page:

````markdown
```python
--8<-- "examples/my_feature.py"
```
````

The snippet ratchet in `scripts/check_doc_snippets.py` enforces the
direction of travel: CI pins a floor on the number of `--8<--` includes and
that floor only ever goes up. Migrating a snippet to a runnable file is
always a net win; pasting a new inline block for a language the ratchet
lints requires an allowlist entry in `docs/.snippet-allowlist.txt`.

## Where they run

All five run in the `verify-docs-current` job in `.github/workflows/ci.yml`,
each as its own named step so a failure names itself in the PR checks. That
job gates the build matrix, so a docs defect fails fast instead of after ten
minutes of compilation. `check_doc_examples.py` runs a second time in
`linux-gcc` with `--require-runtime`, where the bindings exist and the
examples actually execute.

# Build profiles: the full tree and `venue-lite`

`FLOX_BUILD_VENUE` answers one question -- is the venue in this build. This
page answers the other one: is anything *else*.

A downstream consumer that runs the execution venue compiles against 37 of
this tree's headers. Everything else here -- the backtest module, the
exchange connectors and their DEX curves, the Python, Node, C-API, Codon and
QuickJS bindings, the demo, the benchmarks -- is weight that consumer carries
and never calls, and each one drags a dependency: OpenSSL, CURL, zlib,
ixwebsocket, LZ4, pybind11. `FLOX_VENUE_LITE=ON` is the configuration that
does not have them.

## The profile

```bash
cmake --preset venue-lite
cmake --build build-venue-lite -j"$(nproc)"
ctest --preset venue-lite
```

The preset lives in `CMakePresets.json` and sets `FLOX_VENUE_LITE=ON`,
`FLOX_BUILD_TESTS=ON`, `CMAKE_BUILD_TYPE=Release`. There is a second preset,
`venue-lite-engine-only`, which is the same thing with
`FLOX_VENUE_PERIMETER=OFF`: the matching engine, its journal and its
checkpoints, on a platform the perimeter does not reach yet (see
[Building with and without the venue](build.md) for what that split is).

The option can also be set by hand -- `-DFLOX_VENUE_LITE=ON` -- and forces
everything below rather than defaulting it, so a cache left over from a full
configure cannot leave half the tree switched on.

## What is in it

| | |
|---|---|
| `flox-venue` | the matching engine, clearing, the ledger, the journal, checkpoints, the sequenced shard |
| the perimeter | FIX and SBE codecs, sessions, the TCP gateway, the control plane, metrics (`FLOX_VENUE_PERIMETER=ON`, the default off Windows) |
| the FIX initiator | `flox/connector/fix/` -- the client side of the same protocol |
| the core library | `src/clearing`, `src/engine`, `src/execution`, `src/log`, and the header-only utilities the venue links |
| the venue suite | `venue/tests`, golden replay included |

## What is not

| | why |
|---|---|
| the backtest module | `FLOX_ENABLE_BACKTEST=OFF`. The venue needed it until `FeeSchedule`, `Account` and `LeveragedPosition` moved from `flox/backtest/` to `flox/clearing/`; it does not any more. |
| exchange connectors, DEX curves | `FLOX_BUILD_CONNECTORS=OFF`, and with them OpenSSL, CURL, zlib and ixwebsocket |
| Python, Node, C API, Codon, QuickJS | nothing in the venue's surface crosses a binding |
| the demo and the benchmarks | not a deliverable of this profile |
| `src/replay`, `src/report`, `src/risk`, `src/run` | the venue links none of them, and `src/replay` is the only thing in the core that needs LZ4 -- so `FLOX_ENABLE_LZ4=OFF` here |
| the core test suite (`tests/`) | it drives the backtest module, the replay pipeline and the indicator graph. `FLOX_BUILD_TESTS=ON` in this profile builds the venue's suite only. |

These are subdirectories that are never added and `find_package` calls that
never run, not targets excluded at build time: a lite configure asks the
system for nothing the venue does not use. The CI job installs no
`libssl-dev`, no `libcurl`, no `zlib`, no `lz4`, and if that list ever has to
grow, the profile has stopped being lite and the diff says so.

Three venue tests are not built here, each derived from what it includes
rather than from a list kept by hand: `test_venue_derivatives` (drives the
backtest liquidation engine through `flox-venue/liquidation_monitor.h`),
`test_venue_tape` (writes through `flox::replay::BinaryLogWriter`), and
`test_venue_fix_tls_channel` (needs `FLOX_FIX_TLS`, off by default in every
profile).

## The surface contract

`venue/lite_surface.txt` is the list of headers the consumer includes -- the
contract, written down in one place. Two things read it:

* `venue/tests/support/lite_surface.cpp` includes exactly that list plus
  `engine_surface.h`, and is compiled in this profile only. In a full
  configure the backtest module and the connectors are present, so the TU
  would pass whether or not the profile still held; here it fails instead.
* `scripts/lite_closure.py` walks the transitive closure of the list and
  refuses a closure that reaches `flox/backtest/`, `flox/aggregator/`,
  `flox/connector/` other than the FIX initiator, `flox-connectors/`, or any
  directory outside the ones the profile allows. It also checks the TU's
  includes against the list, so the contract cannot become two lists with one
  of them stale.

```bash
python3 scripts/lite_closure.py          # the closure, and the direction gate
python3 scripts/lite_closure.py --paths  # the same closure as file paths
```

The lite configure runs `--paths` and installs exactly what comes back, so
the installed header surface *is* the closure rather than a second list. The
`venue_lite_surface` target depends on the full check, so an edit to
`venue/lite_surface.txt` that the translation unit does not match reddens
that target on your own build, not three jobs later in CI.

## Moving a consumer onto it

1. Drop `-DFLOX_ENABLE_BACKTEST=ON`. It was required before the clearing
   primitives moved into the core and is not any more.
2. Configure with `cmake --preset venue-lite`, or add `-DFLOX_VENUE_LITE=ON`
   to the existing command.
3. Install as usual. `cmake --install` now places the closure and the two
   library targets and nothing else, so a consumer build that still compiles
   is a consumer build that was inside the contract all along.
4. If a header you need is missing, add it to `venue/lite_surface.txt` in the
   same change -- that is what the file is for. `scripts/lite_closure.py`
   will say if it drags a direction the profile does not allow.

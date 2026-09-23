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

The root `CMakeLists.txt` reads the same closure to decide which
`src/{clearing,engine,execution,log}/*.cpp` the lite profile compiles: a
`src/<x>/<y>.cpp` is kept only when `include/flox/<x>/<y>.h` -- its own
header -- is on the list. Most of what is in those four directories is not:
`engine.cpp`, `symbol_registry.cpp`, `algos.cpp`, `order_journey_tracer.cpp`,
`order_tracker.cpp` and `atomic_logger.cpp` exist for the full engine, the
venue links none of them, and the filter drops all six. One entry in
`venue/lite_surface.txt` is there for this reason rather than because the
consumer includes it: `flox/log/console_logger.h`, needed by
`src/log/log_stream.cpp` (the `LogStream` destructor's fallback logger), not
by anything a venue consumer writes. It is marked "required by lite
library" in the file so the two reasons an entry can be on the list stay
distinguishable.

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

## The standalone tree

The venue core used to be its own repository, merged into this one so
changes across its boundary could land atomically. Splitting it back out
loses that; instead, T013 step 3 makes the venue module a *derived*,
read-only mirror -- CI cuts a fixed list of paths out of this repository on
every merge to main and publishes them, with history, to a separate
repository the consumer pins as a submodule. Before that repository exists,
the question worth answering is simpler: does a tree made of *only* those
paths configure, build and pass the venue-lite profile's tests on its own?

`scripts/mirror_paths.py` is the list -- one source, printed one path per
line, that `scripts/lite_standalone_check.sh` (below) and the eventual
mirror-publish step both read, so they cannot drift into disagreement about
what "the mirror" contains.

It is not just `scripts/lite_closure.py --paths`. That closure starts from
`venue/lite_surface.txt` -- the consumer's 37 headers plus the one entry
kept there because the lite library needs it to link -- and what they
transitively include; deliberately narrow, because it is what
`cmake --install` ships. The venue module's own
source reaches further: `venue/tests/test_venue_admin_surface.cpp` includes
`flox-venue/control_server.h`, the perimeter's admin surface, which no
downstream consumer's 38-entry contract ever mentions because no consumer
instantiates it. A first rehearsal of this cut (`git filter-repo` into a
local copy) found this by building: with only the consumer's closure
present, that test failed to compile on a missing header. So
`mirror_paths.py` walks a second, wider closure -- the union of the consumer
contract and everything `venue/src`, `venue/tests` (minus the three files
the lite profile itself never builds, same rule as above) and
`venue/benchmarks` actually `#include` -- through the same BFS and the same
direction gate as `lite_closure.py`, imported rather than reimplemented.
`venue/include/` is then copied whole regardless of that closure: two of its
headers (`tape_recorder.h`, `liquidation_monitor.h`) reach
`flox/replay/`/`flox/backtest/` and are never included by anything the lite
profile compiles, so leaving them out of the mirror would ship a smaller
module than the one flox actually has, while leaving their forbidden
`#include`s out of the *closure* is what keeps the direction gate meaningful.

```bash
python3 scripts/mirror_paths.py            # the path list, one per line
python3 scripts/mirror_paths.py --summary  # + closure sizes and dropped
                                            # tests, on stderr
```

`scripts/lite_standalone_check.sh` copies that list into a scratch directory
with `rsync --files-from` -- which opens nothing not on the list, so a file
missing from it is a build failure here rather than a silent gap discovered
downstream -- and runs the profile there:

```bash
scripts/lite_standalone_check.sh              # scratch dir: mktemp -d, removed on exit
scripts/lite_standalone_check.sh /some/dir     # scratch dir: /some/dir, removed on exit
KEEP=1 scripts/lite_standalone_check.sh /some/dir  # left in place afterwards
```

It fails the same way a broken profile would -- a non-zero `cmake --build`
or `ctest` -- and additionally on a plain `CMake Warning` from the configure
step, which is what an `add_subdirectory` or `file(GLOB)` running outside
`FLOX_VENUE_LITE`'s forced-off options would look like: the standalone tree
has no `python/`, `node/`, `connectors/`, `tests/`, `demo/`, `tools/`, or
`benchmarks/` outside `venue/`, so anything that reaches for one is a
misconfigured guard, not a missing directory to warn about and move past.
CI's `venue-lite-standalone` job (`.github/workflows/ci.yml`) runs this on
every push and additionally asserts the scratch tree has no forbidden
directory and no file beyond what `mirror_paths.py` listed.

`venue-lite` (the profile, in this checkout) and `venue-lite-standalone`
(the mirror, in a scratch tree built from nothing else) are independent
jobs proving different things and are expected to report the same test
count -- currently 93 -- since both configure and build the identical
profile from what is, for the venue module's own purposes, the identical
set of sources.

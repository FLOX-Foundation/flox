# Building with and without the venue

The venue is an optional module. If you only write strategies, backtests, or
connectors, you never pay for it.

## The flag

```bash
# with the venue (default)
cmake -S . -B build
cmake --build build

# without it — nothing venue-related is configured, compiled, or tested
cmake -S . -B build -DFLOX_BUILD_VENUE=OFF
cmake --build build
```

`FLOX_BUILD_VENUE` follows the repository's `FLOX_BUILD_*` convention (the flags
that gate optional build outputs; `FLOX_ENABLE_*` gates capabilities of the core
library). Turning it off removes:

- the `flox-venue` library target and its headers from the build,
- every venue test from `ctest`,
- `multi_agent_venue_demo` from the demo targets.

CI builds the `OFF` configuration on every run and checks that the full test
suite still passes with no venue code compiled.

## Platform support

The module has two platform requirements, not one, and they are separable.

The **engine** needs a native 128-bit integer -- `venue::Amount`, the type
the ledger keeps money in -- and nothing else from the system. GCC, Clang and
clang-cl all have one. `cl` does not, and Microsoft has announced no plan to
add one, so `FLOX_BUILD_VENUE` turns itself off there and says why.

The **perimeter** used to need POSIX sockets. It does not any more: every
socket call goes through `flox/net/socket.h`, and nothing under `venue/`
includes a POSIX network header. `FLOX_VENUE_PERIMETER` still controls it and
still defaults off on Windows, for what is actually left there -- simdjson and
OpenSSL under clang-cl, and one lifecycle test that interposes `close(2)`
through `dlsym`:

```bash
# the engine without the network perimeter
cmake -S . -B build -DFLOX_BUILD_VENUE=ON -DFLOX_VENUE_PERIMETER=OFF
```

With the perimeter off the gateways, sessions, distribution and the control
and metrics servers are not built, their tests are excluded, and simdjson is
not fetched. The exclusion is derived from what each test includes rather
than from a list kept by hand -- a list was tried first and was wrong on the
first build.

What that costs is worth stating plainly: eighteen test binaries do not run,
and they cover the session layer, delivery, recovery over the wire and the
parser fuzz. The engine's own suite, including crash recovery, the
differential fuzz and the conservation fuzz, runs in full. CI builds this
configuration on every run and checks both halves of that claim: that the
engine's tests are present and the perimeter's are not.

### What actually runs on Windows

Measured on the `windows-clang-cl` job rather than reasoned about:

| | builds | tests |
|---|---|---|
| Linux / macOS, perimeter on | everything | 230 |
| Windows (clang-cl), engine only | engine, journal, recovery, codecs | 207 |
| Windows (`cl`) | nothing of this module | -- |

The 207 include the conservation and differential fuzzes and the
process-death recovery drill, which runs there through a helper binary rather
than a fork -- Windows has neither fork nor a way to continue this process in
a child, so the scenario is a second executable that writes the journal and
abandons itself. The parser fuzz is perimeter and does not run there.

`cl` is a separate question with a known price rather than an unknown one:
it has no 128-bit integer and Microsoft has announced no plan for one, so
supporting it means money arithmetic on a portable wide type. That was
measured: the software 256-bit integer costs about 3 microseconds per
settlement against effectively free for the hardware type, 94x, so it is not
the instrument. The portable path that IS used -- `mulDivI64`, a 64x64->128
through 32-bit halves -- is compiled on every platform and checked against the
hardware one, so if `cl` support is ever wanted, the arithmetic underneath it
already works and is tested.

One durability note. A checkpoint's rename is made durable on POSIX by
syncing the directory afterwards; Windows has no directory handle and orders
the rename itself, so `flox::fileio::syncDirectory` does the work on one and
nothing on the other. See `flox/util/file_io.h`.

## Dependencies

The venue links `${FLOX}` core and `simdjson` (pulled via FetchContent at
configure time, same pin as `connectors/`; the REST perimeter codec parses
with it). OpenSSL is needed by the TLS gateway alone; it is detected inside
`venue/CMakeLists.txt`, and without it the module still builds, skipping only
the TLS gateway test.

## Consuming it

```cmake
target_link_libraries(my_target PRIVATE flox::venue)
```

That brings the include prefix with it:

```cpp
#include "flox-venue/matching_engine.h"   // module
#include "flox-venue/matching_book.h"      // module (reference oracle book)
```

## Layout

```
venue/
  CMakeLists.txt          target flox::venue, gated by FLOX_BUILD_VENUE
  include/flox-venue/     public headers, namespace flox::venue
  src/                    the few non-header translation units
  tests/                  the verification corpus (registered into FLOX ctest)
  scripts/                sanitizer gate
  benchmarks/             book microbenchmark
```

Order-level book primitives (`flox/book/{resting_order,ladder_book}.h`) and
the shared utilities the venue needed (`flox/util/{crypto,wire,transport,
websocket,system_clock}.h`) live in core, because they are useful on their own
and carry no venue-specific policy. The map-based `MatchingBook` lives in the
module (`flox-venue/matching_book.h`): it is the correctness oracle the venue
matcher is fuzz-verified against, not a general-purpose book.

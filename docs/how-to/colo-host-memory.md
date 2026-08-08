# Prepare a colo host: memory profile

On a dedicated trading host, set `memoryProfile: "colo"` and give the
process memlock privilege. Without this, the memory flox carefully
pre-allocates and touches at startup can still be paged out under pressure,
and the page comes back as a major fault in the middle of a trading
session.

To enable it you need two things:

1. `memoryProfile = "colo"` in the engine config. The engine then calls
   `mlockall(MCL_CURRENT | MCL_FUTURE)` at start.
2. Memlock privilege on the host (see below). Without it the engine still
   starts, logs a warning, and records the failure in the startup report.

The default profile does nothing and is right for shared VPS and cloud
hosts, where a hard mlockall would fail on most accounts.

## Host prerequisites

Either grant the capability to the binary:

```bash
sudo setcap cap_ipc_lock=+ep ./your-flox-binary
```

or raise the memlock limit for the trading user in
`/etc/security/limits.conf`:

```text
trader  soft  memlock  unlimited
trader  hard  memlock  unlimited
```

Then check from the shell that will launch the process:

```bash
ulimit -l
```

You want `unlimited`, or at least a value comfortably above the process
RSS.

## Reading the startup report

The engine logs one line about memory state at start:

```text
memory profile=colo mlock=applied memlock-limit=unlimited huge-arena=n/a
```

- `mlock=` is `not-requested`, `applied`, or `FAILED(<errno text>)`.
  `FAILED(Cannot allocate memory)` means the memlock limit is below the
  process footprint. `FAILED(Operation not permitted)` means the capability
  is missing.
- `memlock-limit=` is the `RLIMIT_MEMLOCK` soft limit the process sees.
- `huge-arena=` shows the backing mode of the large-arena layer, when the
  build uses one (next section).

## Huge pages for large hot structures

Large rings and pools on ordinary 4K pages exhaust TLB coverage: roughly
1500 dTLB entries times 4K is about 6 MB, and a single big ring walks past
that. On 2 MB pages the same entries cover gigabytes.

The large-arena layer tries three backings in order and reports which one
it got:

1. explicit huge pages (`MAP_HUGETLB`) from the pool reserved at boot.
   This either succeeds or fails immediately, so behaviour is predictable;
2. transparent-huge-page advice (`madvise(MADV_HUGEPAGE)`), best effort;
3. plain 4K pages. Always works, and the only mode on macOS.

Reserve the explicit pool on the host (example: 512 pages = 1 GB):

```bash
echo 512 | sudo tee /proc/sys/vm/nr_hugepages
cat /proc/meminfo | grep HugePages
```

The `huge-arena=` field of the startup report shows the weakest backing in
use plus total arena bytes. To measure the effect, compare
`perf stat -e dTLB-load-misses` before and after: on a working set of a few
hundred megabytes the miss count usually drops by an order of magnitude.

## Scope

flox adapts to a prepared host and reports what it found. Host tuning
itself, such as isolating cores, moving IRQs, NIC coalescing, and the huge
page reservation above, stays outside the engine.

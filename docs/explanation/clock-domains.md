# Clock domains

flox handles time from three clocks that must never be confused, and one
interval type that belongs to none of them. The types live in
`flox/util/base/time.h`; this page is the contract behind them.

## Why types instead of discipline

The nanosecond fields used to be plain integers, and an integer accepts any
clock. That is not a hypothetical failure: a market-data connector once filled
a wall-clock field from the steady clock and produced a day of book records 56
years adrift of their own trades, and the same mistake was later found in two
more connectors. The difference between clock domains is decades, and it
surfaces as quietly corrupted data, not as a crash.

A wrapped integer with no implicit conversions turns every one of those
mistakes into a compile error. The wrappers are 8 bytes, trivially copyable,
and byte-identical to the integers they replace, so wire formats, journals and
tapes are unaffected.

## The domains

| Type | Clock | Written by | Comparable across processes? |
|---|---|---|---|
| `UnixNanos` | wall (epoch) | venue payloads, tape `exchange_ts`, backtest simulated clock | yes |
| `MonoNanos` | steady | receive stamps, publish stamps, latency envelopes | no — only differences mean anything |
| `SeqNanos` | venue sequencer | a matching shard's journaled clock | replay-stable, but never against the wall |
| `DurationNs` | none | same-domain subtraction | it is an interval; intervals have no epoch |

`SeqNanos` deserves the extra sentence. A venue shard stamps every command
with a deterministic, journaled timestamp. At capture that value is derived
from the wall clock, and that is the one legitimate crossing between the two
domains. From then on it is not wall time: a replayed journal re-derives the
same `SeqNanos` values at a completely different wall moment. Comparing a
sequencer stamp against `UnixNanos` is therefore meaningful exactly once and
wrong forever after, which is precisely the kind of rule a type can hold and a
comment cannot.

## The rules

1. A field is typed at its declaration. The compiler, not grep, enumerates
   everyone who must then say what they meant.
2. Conversions happen only at named boundaries: wire, journal, C ABI and
   bindings use `raw()` / `fromRaw()`; clock reads use the named helpers
   (`nowUnixNanos()`, `nowMonoNanos()`, `msToUnixNs()`).
3. A cross-domain comparison is a design question, not a cast site. Every
   `raw()` at a domain crossing states why the crossing is legitimate.
4. Layout is part of the contract: `static_assert` pins size, alignment and
   trivial copyability, and a test memcpy's a wrapper to prove the bytes are
   the integer. Journals and tapes written before the types read back
   unchanged.
5. Bindings keep integer surfaces. Python, Node, Codon and QuickJS see plain
   nanosecond integers; the domain typing is a C++ compile-time guarantee, not
   an API for other languages.

The negative half of the contract is enforced by
`tests/test_time_domains.cpp`: cross-domain assignment, cross-domain
subtraction and implicit integer conversion in any direction are pinned shut
with `static_assert` on requires-expressions. Reopening any of them fails the
build, not a test run.

## What found bugs

Introducing the types was itself an audit. The compiler-enumerated sweep
caught three live instances of a steady-clock reading stored as an exchange
timestamp, in shipped connectors, before any of them was diffed against a
real epoch in production.

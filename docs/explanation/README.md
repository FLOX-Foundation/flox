# Explanation

Understand concepts and design decisions behind FLOX. These pages are language-agnostic; code samples use tabs where they help.

## Overview

| Topic | What You'll Learn |
|-------|-------------------|
| [Architecture](architecture.md) | How components fit together |
| [Bar Types](bar-types.md) | Time, tick, volume, range, Renko, Heikin-Ashi — when to use which |
| [Disruptor Pattern](disruptor.md) | Why we use ring buffers (system-level, C++ internals) |
| [Memory Model](memory-model.md) | Zero-allocation event delivery (system-level, C++ internals) |
| [Integration Flow](integration-flow.md) | End-to-end data flow through the system |
| [Indicators](indicators.md) | What each indicator measures and when to use it |
| [Per-symbol scale](per-symbol-scale.md) | How fixed-point price/quantity scale is chosen per symbol for DEX-range tokens |
| [Replay-equivalence gate](replay-equivalence-gate.md) | The CI check that defends deterministic backtest replay |

## When to read these

- **Before** diving deep into customization
- **When** you want to understand design trade-offs
- **If** you're debugging performance issues

## A note on language

Some explanation pages — Disruptor Pattern, Memory Model — describe internals of the C++ engine itself. Code in those pages is C++ because that *is* the implementation. If you only use the Python, Node.js, or Codon bindings you can read them as background; the bindings expose the relevant behaviour through their own APIs without you needing to touch C++.

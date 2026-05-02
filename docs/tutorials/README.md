# Tutorials

Step-by-step lessons to get you productive with FLOX.

## Pick your language

Each language has its own quickstart. After that the tutorials are language-agnostic and use code tabs so you can pick what you read.

| Quickstart | Audience |
|------------|----------|
| [Python quickstart](python-quickstart.md) | Quants and researchers — backtest in pandas-friendly Python |
| [Node.js quickstart](node-quickstart.md) | JavaScript / TypeScript stacks — same API as Python |
| [Codon quickstart](codon-strategy.md) | Python syntax compiled to native code |
| [C++ quickstart](quickstart.md) | Direct access to the engine — lowest latency |

## Core path

After your language quickstart, work through these. They cover the same concepts; each page has tabs for Python / Node.js / C++ / Codon (where applicable).

| Tutorial | What You'll Learn |
|----------|-------------------|
| [First Strategy](first-strategy.md) | Write a simple trading strategy |
| [Multi-Timeframe Strategy](multi-timeframe-strategy.md) | Build strategies using multiple bar timeframes |
| [Recording Data](recording-data.md) | Capture live market data to disk |
| [Backtesting](backtesting.md) | Replay recorded data through your strategy |
| [Run Demo](demo.md) | End-to-end demo runner |

## Recommended order

1. **Quickstart** for your language — verify your environment works
2. **First Strategy** — understand the core programming model
3. **Multi-Timeframe Strategy** — work with multiple bar timeframes
4. **Recording Data** — set up market data capture
5. **Backtesting** — test strategies against historical data

After completing these tutorials, move on to [How-To Guides](../how-to/README.md) for specific tasks or [Explanation](../explanation/README.md) for deeper understanding.

## Requirements per language

=== "Python"
    - Python 3.10+
    - `pip install flox-py` or build from source

=== "Node.js"
    - Node.js 18+
    - `npm install @flox-foundation/flox` or build from source

=== "Codon"
    - Codon compiler installed
    - Flox built with `-DFLOX_ENABLE_CAPI=ON`

=== "C++"
    - C++20 compiler (GCC 13+ or Clang 16+)
    - CMake 3.22+
    - Linux (recommended) or macOS

See the [Bindings](../bindings/README.md) page for full per-language build details.

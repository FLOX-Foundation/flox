# Tutorial: Multi-Timeframe Strategy

This tutorial walks you through building a multi-timeframe momentum strategy from scratch. You'll learn how to:

- Set up bar aggregation for multiple timeframes
- Store bar history in a BarMatrix
- Access bars from different timeframes in your strategy
- Generate trading signals based on MTF analysis

## Prerequisites

- Basic C++ knowledge
- Flox Engine installed and building
- Completed [First Strategy](first-strategy.md) tutorial

## What We're Building

A momentum strategy that:
1. Uses **H1** (hourly) bars to determine trend direction
2. Uses **M5** (5-minute) bars to identify pullbacks
3. Uses **M1** (1-minute) bars for entry timing

**Entry logic**: Buy when H1 is bullish, M5 shows a pullback, and M1 prints a reversal bar.

## Step 1: Project Setup

Create a new file `my_mtf_strategy.cpp`:

```cpp
#include "flox/aggregator/bar_aggregator.h"
#include "flox/aggregator/bar_matrix.h"
#include "flox/aggregator/bus/bar_bus.h"
#include "flox/aggregator/multi_timeframe_aggregator.h"
#include "flox/aggregator/timeframe.h"
#include "flox/book/events/trade_event.h"
#include "flox/common.h"
#include "flox/engine/abstract_market_data_subscriber.h"

#include <iostream>

using namespace flox;
```

## Step 2: Define the Strategy Class

```cpp
class MTFMomentumStrategy : public IMarketDataSubscriber
{
 public:
  explicit MTFMomentumStrategy(SymbolId symbol, BarMatrix<256, 4, 64>* matrix)
      : _symbol(symbol), _matrix(matrix)
  {
  }

  SubscriberId id() const override
  {
    return reinterpret_cast<SubscriberId>(this);
  }

  void onBar(const BarEvent& ev) override
  {
    // We'll implement this next
  }

 private:
  SymbolId _symbol;
  BarMatrix<256, 4, 64>* _matrix;
};
```

The strategy:
- Takes a symbol ID and pointer to a BarMatrix
- Implements `IMarketDataSubscriber` to receive bar events
- Will analyze bars in `onBar()`

## Step 3: Implement the Trading Logic

Add the signal generation in `onBar()`:

```cpp
void onBar(const BarEvent& ev) override
{
  // Only process bars for our symbol
  if (ev.symbol != _symbol)
  {
    return;
  }

  // Only act on M1 bars (fastest timeframe for entries)
  if (ev.barType != BarType::Time || ev.barTypeParam != 60)
  {
    return;
  }

  // Get bars from different timeframes
  const Bar* h1 = _matrix->bar(_symbol, timeframe::H1, 0);      // Latest H1
  const Bar* h1_prev = _matrix->bar(_symbol, timeframe::H1, 1); // Previous H1
  const Bar* m5 = _matrix->bar(_symbol, timeframe::M5, 0);      // Latest M5
  const Bar* m1 = _matrix->bar(_symbol, timeframe::M1, 0);      // Latest M1

  // Need all bars to generate signals
  if (!h1 || !h1_prev || !m5 || !m1)
  {
    return;  // Not enough data yet
  }

  // 1. Check H1 trend
  bool h1Bullish = h1->close > h1_prev->close;

  // 2. Check M5 pullback (bearish bar in uptrend)
  bool m5Pullback = m5->close < m5->open;

  // 3. Check M1 reversal (bullish bar after pullback)
  bool m1Reversal = m1->close > m1->open;

  // Generate signal
  if (h1Bullish && m5Pullback && m1Reversal)
  {
    std::cout << "[SIGNAL] BUY @ " << m1->close.toDouble()
              << " | H1 bullish, M5 pullback, M1 reversal" << std::endl;
  }
}
```

**Key points:**

- We filter for M1 bars to trigger entry logic on the fastest timeframe
- `bar(symbol, timeframe, index)` gives us historical bars (0 = latest, 1 = previous)
- We check conditions across all three timeframes

## Step 4: Set Up the Infrastructure

Now create `main()` to wire everything together:

```cpp
int main()
{
  constexpr SymbolId SYMBOL = 1;

  // 1. Create the bar event bus
  BarBus bus;

  // 2. Create multi-timeframe aggregator
  MultiTimeframeAggregator<4> aggregator(&bus);
  aggregator.addTimeInterval(std::chrono::seconds(60));    // M1
  aggregator.addTimeInterval(std::chrono::seconds(300));   // M5
  aggregator.addTimeInterval(std::chrono::seconds(3600));  // H1

  // 3. Create bar matrix for history storage
  BarMatrix<256, 4, 64> matrix;
  std::array<TimeframeId, 3> timeframes = {
    timeframe::M1,
    timeframe::M5,
    timeframe::H1
  };
  matrix.configure(timeframes);

  // 4. Create strategy
  MTFMomentumStrategy strategy(SYMBOL, &matrix);

  // 5. Subscribe to bar events
  bus.subscribe(&matrix);    // Matrix stores bars
  bus.subscribe(&strategy);  // Strategy receives bars

  // 6. Start components
  bus.start();
  aggregator.start();

  // 7. Feed trades (in real system, this comes from connector)
  // aggregator.onTrade(tradeEvent);

  // 8. Cleanup
  aggregator.stop();
  bus.stop();

  return 0;
}
```

## Step 5: Understanding the Data Flow

```
TradeEvent
    │
    ▼
MultiTimeframeAggregator
    │
    ├──► M1 aggregator ──► BarEvent (M1)
    ├──► M5 aggregator ──► BarEvent (M5)
    └──► H1 aggregator ──► BarEvent (H1)
                               │
                               ▼
                            BarBus
                               │
              ┌────────────────┴────────────────┐
              ▼                                 ▼
          BarMatrix                      MTFMomentumStrategy
    (stores bar history)                  (generates signals)
```

1. **Trades** come from your connector
2. **MultiTimeframeAggregator** builds bars for all timeframes simultaneously
3. **BarBus** distributes BarEvents to subscribers
4. **BarMatrix** stores history for lookback
5. **Strategy** accesses BarMatrix for multi-timeframe analysis

## Step 6: Adding More Signal Logic

Let's enhance the strategy with sell signals and tracking:

```cpp
class MTFMomentumStrategy : public IMarketDataSubscriber
{
 public:
  // ... constructor ...

  void onBar(const BarEvent& ev) override
  {
    if (ev.symbol != _symbol) return;
    if (ev.barType != BarType::Time || ev.barTypeParam != 60) return;

    const Bar* h1 = _matrix->bar(_symbol, timeframe::H1, 0);
    const Bar* h1_prev = _matrix->bar(_symbol, timeframe::H1, 1);
    const Bar* m5 = _matrix->bar(_symbol, timeframe::M5, 0);
    const Bar* m1 = _matrix->bar(_symbol, timeframe::M1, 0);

    if (!h1 || !h1_prev || !m5 || !m1) return;

    // Trend detection
    bool h1Bullish = h1->close > h1_prev->close;
    bool h1Bearish = h1->close < h1_prev->close;

    // Pullback detection
    bool m5Pullback = m5->close < m5->open;   // Bearish M5 in uptrend
    bool m5Rally = m5->close > m5->open;      // Bullish M5 in downtrend

    // Entry bar
    bool m1BullishReversal = m1->close > m1->open;
    bool m1BearishReversal = m1->close < m1->open;

    // BUY signal
    if (h1Bullish && m5Pullback && m1BullishReversal)
    {
      std::cout << "[BUY] @ " << m1->close.toDouble() << std::endl;
      ++_buySignals;
    }
    // SELL signal
    else if (h1Bearish && m5Rally && m1BearishReversal)
    {
      std::cout << "[SELL] @ " << m1->close.toDouble() << std::endl;
      ++_sellSignals;
    }
  }

  void printStats() const
  {
    std::cout << "Buy signals:  " << _buySignals << std::endl;
    std::cout << "Sell signals: " << _sellSignals << std::endl;
  }

 private:
  SymbolId _symbol;
  BarMatrix<256, 4, 64>* _matrix;
  int _buySignals = 0;
  int _sellSignals = 0;
};
```

## Step 7: Adding Delta Confirmation

Use buy/sell volume for confirmation:

```cpp
// In onBar(), after getting bars:

// Check delta (buy pressure - sell pressure)
Volume buyVol = m1->buyVolume;
Volume sellVol = Volume::fromRaw(m1->volume.raw() - m1->buyVolume.raw());
bool positiveDelta = buyVol.raw() > sellVol.raw();

// Enhanced BUY signal with delta confirmation
if (h1Bullish && m5Pullback && m1BullishReversal && positiveDelta)
{
  std::cout << "[BUY] @ " << m1->close.toDouble()
            << " | Delta: +" << (buyVol.raw() - sellVol.raw()) << std::endl;
}
```

## Complete Example

See [multi_timeframe_demo.cpp](https://github.com/FLOX-Foundation/flox/blob/main/demo/src/multi_timeframe_demo.cpp) for a complete working example.

## Next Steps

- Add position management and risk controls
- Implement stop-loss and take-profit logic
- Add [Volume Profile](../components/aggregator/volume_profile.md) for key level detection
- Use [Footprint](../components/aggregator/footprint_chart.md) for order flow confirmation

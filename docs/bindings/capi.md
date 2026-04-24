# C API

`flox_capi.h` is a C interface to the Flox engine. All existing bindings (Python, Node.js, Codon, embedded JS) use it. If you're adding support for a new language or embedding Flox in a C project, start here.

## Build

```bash
cmake -B build \
  -DFLOX_ENABLE_CAPI=ON \
  -DCMAKE_BUILD_TYPE=Release

cmake --build build
```

Produces `build/src/capi/libflox_capi.so`.

## Header

```c
#include "flox/capi/flox_capi.h"
```

## Minimal example

```c
#include "flox/capi/flox_capi.h"
#include <stdio.h>

static void on_trade(FloxStrategyHandle strat, FloxSymbolContext* ctx,
                     FloxTradeData* trade, void* user_data) {
    double price = flox_price_to_double(trade->price_raw);
    printf("trade: %.2f\n", price);
}

static void on_signal(const FloxSignal* sig, void* user_data) {
    printf("signal: %s qty=%.4f\n",
           sig->side == 0 ? "buy" : "sell",
           sig->quantity);
}

int main(void) {
    FloxRegistryHandle reg = flox_registry_create();
    uint32_t btc = flox_registry_add_symbol(reg, "binance", "BTCUSDT", 0.01);

    FloxStrategyCallbacks cbs = {0};
    cbs.on_trade = on_trade;

    FloxStrategyHandle strat = flox_strategy_create(0, &btc, 1, &cbs, NULL);

    FloxRunnerHandle runner = flox_runner_create(reg, on_signal, NULL);
    flox_runner_add_strategy(runner, strat);
    flox_runner_start(runner);

    // inject a tick
    flox_runner_on_trade(runner, btc, 67000.0, 0.01, 1, 0);

    flox_runner_stop(runner);
    flox_runner_destroy(runner);
    flox_strategy_destroy(strat);
    flox_registry_destroy(reg);
    return 0;
}
```

Compile:

```bash
gcc -o example example.c \
  -I/path/to/flox/include \
  -L/path/to/build/src/capi \
  -lflox_capi \
  -Wl,-rpath,/path/to/build/src/capi
```

## Full API reference

See [C API Reference](../reference/api/capi/flox_capi.md) for all functions, structs, and callback signatures.

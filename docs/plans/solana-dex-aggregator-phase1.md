# Solana DEX Aggregator — Phase 1: Quote Engine MVP

## Цель

Обкатка Flox компонентов на реальных данных Solana. Построение read-only Quote Engine с подключением к Yellowstone gRPC.

**Не входит в scope:** построение и отправка транзакций, UI, production deployment.

---

## Архитектура

```
Yellowstone gRPC (своя нода)
       │
       │ AccountUpdate stream
       ▼
┌──────────────────────────────────────────────────────────┐
│                    GeyserClient                          │
│            (gRPC consumer, backpressure)                 │
└─────────────────────────┬────────────────────────────────┘
                          │
                          ▼
┌──────────────────────────────────────────────────────────┐
│                   AccountRouter                          │
│                                                          │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────┐   │
│  │ PumpDecoder │  │RaydiumV4Dec │  │ TokenAccDecoder │   │
│  └─────────────┘  └─────────────┘  └─────────────────┘   │
└─────────────────────────┬────────────────────────────────┘
                          │
                          │ PoolUpdateEvent
                          ▼
┌──────────────────────────────────────────────────────────┐
│                 Flox EventBus                            │
└─────────────────────────┬────────────────────────────────┘
                          │
                          ▼
┌──────────────────────────────────────────────────────────┐
│                    PoolMatrix                            │
│           (adapted CompositeBookMatrix)                  │
│                                                          │
│  - Lock-free atomic reads                                │
│  - Per-pool state: reserves, fees, slot                  │
│  - AMM math: getAmountOut()                              │
└─────────────────────────┬────────────────────────────────┘
                          │
                          ▼
┌──────────────────────────────────────────────────────────┐
│                    QuoteService                          │
│                                                          │
│  GET /quote?inputMint=X&outputMint=Y&amount=Z            │
│                                                          │
│  Response: { amountOut, pool, priceImpact, latencyNs }   │
└──────────────────────────────────────────────────────────┘
```

---

## Задачи

### 1. GeyserClient

**Описание:** gRPC клиент для Yellowstone, подписка на account updates.

**Файлы:**
- `include/flox/solana/geyser_client.h`
- `src/solana/geyser_client.cpp`

**Зависимости:**
- gRPC / grpc++
- Yellowstone proto files (https://github.com/rpcpool/yellowstone-grpc)

**Интерфейс:**
```cpp
namespace flox::solana {

struct AccountUpdate {
    Pubkey pubkey;
    Pubkey owner;          // program id
    uint64_t lamports;
    std::span<const uint8_t> data;
    uint64_t slot;
    bool isStartup;        // initial snapshot vs live update
};

using AccountUpdateCallback = std::function<void(const AccountUpdate&)>;

struct GeyserConfig {
    std::string endpoint;  // "http://localhost:10000"
    std::vector<Pubkey> programFilters;  // subscribe by owner
    size_t bufferSize = 10000;           // backpressure buffer
};

class GeyserClient {
public:
    explicit GeyserClient(const GeyserConfig& config);

    void subscribe(AccountUpdateCallback callback);
    void start();  // blocking, runs event loop
    void stop();

    // Stats
    uint64_t updatesReceived() const;
    uint64_t updatesDropped() const;  // backpressure
    uint64_t currentSlot() const;
};

}  // namespace flox::solana
```

**Acceptance Criteria:**
- [ ] Подключение к Yellowstone endpoint
- [ ] Подписка с фильтром по program owner
- [ ] Callback вызывается для каждого AccountUpdate
- [ ] Backpressure: если consumer не успевает, дропаем старые (с метрикой)
- [ ] Graceful shutdown
- [ ] Unit test с mock gRPC server

**Фильтры для подписки:**
```
Program IDs:
- Pump.fun: 6EF8rrecthR5Dkzon8Nwu78hRvfCKubJ14M5uBEwF6P
- Raydium AMM V4: 675kPX9MHTjS2zt1qfr1NYHuzeLXfQM9H24wFSUt1Mp8
- Token Program: TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA
```

---

### 2. Account Decoders

**Описание:** Парсеры account data для каждого протокола.

**Файлы:**
- `include/flox/solana/decoders/pool_decoder.h` (interface)
- `include/flox/solana/decoders/pump_decoder.h`
- `include/flox/solana/decoders/raydium_v4_decoder.h`
- `include/flox/solana/decoders/token_account_decoder.h`

#### 2.1 Base Interface

```cpp
namespace flox::solana {

enum class PoolModel : uint8_t {
    CONSTANT_PRODUCT,   // x * y = k
    BONDING_CURVE,      // pump.fun style
    CLMM,               // concentrated (future)
};

struct PoolState {
    Pubkey address;
    Pubkey tokenA;       // mint
    Pubkey tokenB;       // mint (SOL = So11111111111111111111111111111111111111112)

    uint64_t reserveA;
    uint64_t reserveB;

    uint32_t feeBps;     // fee in basis points (100 = 1%)
    PoolModel model;

    uint64_t lastSlot;
    bool valid;
};

class IPoolDecoder {
public:
    virtual ~IPoolDecoder() = default;
    virtual Pubkey programId() const = 0;
    virtual std::optional<PoolState> decode(const AccountUpdate& update) = 0;
};

}  // namespace flox::solana
```

#### 2.2 Pump.fun Decoder

```cpp
// Account layout
#pragma pack(push, 1)
struct PumpBondingCurve {
    uint64_t discriminator;          // 8 bytes, Anchor
    uint64_t virtualTokenReserves;   // 8
    uint64_t virtualSolReserves;     // 8
    uint64_t realTokenReserves;      // 8
    uint64_t realSolReserves;        // 8
    uint64_t tokenTotalSupply;       // 8
    uint8_t complete;                // 1 (bool)
    // total: 49 bytes minimum
};
#pragma pack(pop)

class PumpDecoder : public IPoolDecoder {
public:
    static constexpr std::array<uint8_t, 32> PROGRAM_ID = {/* 6EF8... */};

    Pubkey programId() const override { return Pubkey(PROGRAM_ID); }

    std::optional<PoolState> decode(const AccountUpdate& update) override {
        if (update.data.size() < sizeof(PumpBondingCurve)) {
            return std::nullopt;
        }

        auto* bc = reinterpret_cast<const PumpBondingCurve*>(update.data.data());

        // Skip completed curves (migrated to Raydium)
        if (bc->complete) {
            return std::nullopt;
        }

        return PoolState{
            .address = update.pubkey,
            .tokenA = deriveMint(update.pubkey),  // PDA derivation needed
            .tokenB = NATIVE_SOL,
            .reserveA = bc->virtualTokenReserves,
            .reserveB = bc->virtualSolReserves,
            .feeBps = 100,  // 1%
            .model = PoolModel::BONDING_CURVE,
            .lastSlot = update.slot,
            .valid = true,
        };
    }
};
```

**Note:** Для Pump.fun mint derivation:
```
mint = PDA(["mint", bonding_curve], PUMP_PROGRAM)
```

#### 2.3 Raydium V4 Decoder

```cpp
#pragma pack(push, 1)
struct RaydiumAmmV4 {
    uint64_t status;              // 0
    uint64_t nonce;               // 8
    uint64_t orderNum;            // 16
    uint64_t depth;               // 24
    uint64_t coinDecimals;        // 32
    uint64_t pcDecimals;          // 40
    uint64_t state;               // 48
    uint64_t resetFlag;           // 56
    uint64_t minSize;             // 64
    uint64_t volMaxCutRatio;      // 72
    uint64_t amountWaveRatio;     // 80
    uint64_t coinLotSize;         // 88
    uint64_t pcLotSize;           // 96
    uint64_t minPriceMultiplier;  // 104
    uint64_t maxPriceMultiplier;  // 112
    uint64_t sysDecimalValue;     // 120
    uint64_t tradeFeeNumerator;   // 128
    uint64_t tradeFeeDenominator; // 136
    uint64_t pnlNumerator;        // 144
    uint64_t pnlDenominator;      // 152
    uint64_t swapFeeNumerator;    // 160
    uint64_t swapFeeDenominator;  // 168
    // ... skip intermediate fields ...
    // Offsets for key fields:
    // poolCoinTokenAccount: offset 336 (32 bytes)
    // poolPcTokenAccount: offset 368 (32 bytes)
    // coinMintAddress: offset 400 (32 bytes)
    // pcMintAddress: offset 432 (32 bytes)
};
#pragma pack(pop)

class RaydiumV4Decoder : public IPoolDecoder {
public:
    // ВАЖНО: Raydium V4 хранит reserves в отдельных Token Accounts,
    // не в AMM account. Этот decoder возвращает pool metadata,
    // reserves обновляются через TokenAccountDecoder.

    std::optional<PoolState> decode(const AccountUpdate& update) override {
        if (update.data.size() < 752) {
            return std::nullopt;
        }

        auto* amm = reinterpret_cast<const RaydiumAmmV4*>(update.data.data());

        // Status check (1 = active)
        if (amm->status != 1) {
            return std::nullopt;
        }

        uint32_t feeBps = (amm->tradeFeeNumerator * 10000) / amm->tradeFeeDenominator;

        return PoolState{
            .address = update.pubkey,
            .tokenA = extractPubkey(update.data, 400),  // coinMint
            .tokenB = extractPubkey(update.data, 432),  // pcMint
            .reserveA = 0,  // заполняется из vault accounts
            .reserveB = 0,
            .feeBps = feeBps,
            .model = PoolModel::CONSTANT_PRODUCT,
            .lastSlot = update.slot,
            .valid = true,
        };
    }

    // Возвращает vault pubkeys для подписки
    std::pair<Pubkey, Pubkey> getVaults(const AccountUpdate& update) {
        return {
            extractPubkey(update.data, 336),  // poolCoinTokenAccount
            extractPubkey(update.data, 368),  // poolPcTokenAccount
        };
    }
};
```

#### 2.4 Token Account Decoder (для Raydium vaults)

```cpp
#pragma pack(push, 1)
struct TokenAccount {
    Pubkey mint;          // 0
    Pubkey owner;         // 32
    uint64_t amount;      // 64
    // ... остальные поля не нужны
};
#pragma pack(pop)

class TokenAccountDecoder {
public:
    struct Balance {
        Pubkey account;
        Pubkey mint;
        uint64_t amount;
        uint64_t slot;
    };

    std::optional<Balance> decode(const AccountUpdate& update) {
        if (update.owner != TOKEN_PROGRAM) {
            return std::nullopt;
        }
        if (update.data.size() < 72) {
            return std::nullopt;
        }

        auto* ta = reinterpret_cast<const TokenAccount*>(update.data.data());

        return Balance{
            .account = update.pubkey,
            .mint = ta->mint,
            .amount = ta->amount,
            .slot = update.slot,
        };
    }
};
```

**Acceptance Criteria:**
- [ ] PumpDecoder корректно парсит bonding curve accounts
- [ ] RaydiumV4Decoder парсит AMM metadata
- [ ] TokenAccountDecoder парсит vault balances
- [ ] Unit tests с реальными account snapshots (fixtures)
- [ ] Benchmark: decode throughput >1M/sec

---

### 3. PoolMatrix

**Описание:** Thread-safe хранилище pool state. Адаптация `CompositeBookMatrix` под AMM.

**Файлы:**
- `include/flox/solana/pool_matrix.h`
- `src/solana/pool_matrix.cpp`

**Интерфейс:**
```cpp
namespace flox::solana {

template <size_t MaxPools = 8192>
class PoolMatrix {
public:
    using PoolId = uint32_t;
    static constexpr PoolId InvalidPool = std::numeric_limits<PoolId>::max();

    // Pool registration
    PoolId registerPool(const Pubkey& address);
    PoolId findPool(const Pubkey& address) const;

    // Writer thread (Geyser consumer)
    void updatePool(PoolId id, const PoolState& state);
    void updateReserves(PoolId id, uint64_t reserveA, uint64_t reserveB, uint64_t slot);

    // Reader thread (Quote service) - lock-free
    PoolState getPool(PoolId id) const;

    // AMM math
    uint64_t getAmountOut(PoolId id, uint64_t amountIn, bool aToB) const;

    // Lookup by token pair
    std::vector<PoolId> findPools(const Pubkey& tokenA, const Pubkey& tokenB) const;

    // Stats
    size_t poolCount() const;
    uint64_t lastUpdateSlot() const;

private:
    struct alignas(64) AtomicPoolState {
        std::atomic<uint64_t> reserveA{0};
        std::atomic<uint64_t> reserveB{0};
        std::atomic<uint64_t> lastSlot{0};
        std::atomic<uint32_t> feeBps{0};
        std::atomic<uint8_t> model{0};
        std::atomic<bool> valid{false};

        // Non-atomic (set once at registration)
        Pubkey address;
        Pubkey tokenA;
        Pubkey tokenB;
    };

    std::array<AtomicPoolState, MaxPools> _pools;
    // ... index structures
};

}  // namespace flox::solana
```

**AMM Math:**
```cpp
uint64_t getAmountOut(PoolId id, uint64_t amountIn, bool aToB) const {
    auto& p = _pools[id];

    uint64_t reserveIn = aToB
        ? p.reserveA.load(std::memory_order_acquire)
        : p.reserveB.load(std::memory_order_acquire);
    uint64_t reserveOut = aToB
        ? p.reserveB.load(std::memory_order_acquire)
        : p.reserveA.load(std::memory_order_acquire);

    if (reserveIn == 0 || reserveOut == 0) {
        return 0;
    }

    uint32_t feeBps = p.feeBps.load(std::memory_order_acquire);

    // Constant product with fee
    // amountOut = (amountIn * (10000 - feeBps) * reserveOut) /
    //             (reserveIn * 10000 + amountIn * (10000 - feeBps))

    uint64_t amountInWithFee = amountIn * (10000 - feeBps);
    uint64_t numerator = amountInWithFee * reserveOut;
    uint64_t denominator = reserveIn * 10000 + amountInWithFee;

    return numerator / denominator;
}
```

**Acceptance Criteria:**
- [ ] Lock-free reads (verify with ThreadSanitizer)
- [ ] Correct AMM math (unit tests vs reference implementation)
- [ ] Pool lookup by address O(1)
- [ ] Pool lookup by token pair
- [ ] Benchmark: getAmountOut <100ns
- [ ] Benchmark: update throughput >1M/sec

---

### 4. Integration: AccountRouter

**Описание:** Связывает GeyserClient → Decoders → PoolMatrix.

**Файлы:**
- `include/flox/solana/account_router.h`
- `src/solana/account_router.cpp`

```cpp
namespace flox::solana {

class AccountRouter {
public:
    AccountRouter(PoolMatrix<>& poolMatrix);

    void registerDecoder(std::unique_ptr<IPoolDecoder> decoder);

    // Callback for GeyserClient
    void onAccountUpdate(const AccountUpdate& update);

    // Stats
    uint64_t updatesProcessed() const;
    uint64_t poolsDiscovered() const;

private:
    PoolMatrix<>& _poolMatrix;
    std::vector<std::unique_ptr<IPoolDecoder>> _decoders;
    std::unordered_map<Pubkey, IPoolDecoder*> _decoderByProgram;

    // Raydium vault tracking
    std::unordered_map<Pubkey, PoolMatrix<>::PoolId> _vaultToPool;

    TokenAccountDecoder _tokenDecoder;
};

}  // namespace flox::solana
```

**Логика:**
```cpp
void AccountRouter::onAccountUpdate(const AccountUpdate& update) {
    // 1. Check if it's a known vault (Token Account)
    if (update.owner == TOKEN_PROGRAM) {
        auto it = _vaultToPool.find(update.pubkey);
        if (it != _vaultToPool.end()) {
            auto balance = _tokenDecoder.decode(update);
            if (balance) {
                // Determine if it's reserveA or reserveB based on mint
                _poolMatrix.updateReserveByVault(it->second, update.pubkey, balance->amount, update.slot);
            }
        }
        return;
    }

    // 2. Try protocol decoders
    auto it = _decoderByProgram.find(update.owner);
    if (it == _decoderByProgram.end()) {
        return;
    }

    auto poolState = it->second->decode(update);
    if (!poolState) {
        return;
    }

    // 3. Register or update pool
    auto poolId = _poolMatrix.findPool(update.pubkey);
    if (poolId == PoolMatrix<>::InvalidPool) {
        poolId = _poolMatrix.registerPool(update.pubkey);

        // For Raydium: register vault subscriptions
        if (auto* raydium = dynamic_cast<RaydiumV4Decoder*>(it->second)) {
            auto [vaultA, vaultB] = raydium->getVaults(update);
            _vaultToPool[vaultA] = poolId;
            _vaultToPool[vaultB] = poolId;
            // TODO: динамически добавить в Geyser subscription
        }
    }

    _poolMatrix.updatePool(poolId, *poolState);
}
```

**Acceptance Criteria:**
- [ ] Routes updates to correct decoder
- [ ] Registers new pools on first sight
- [ ] Updates reserves from vault accounts
- [ ] Handles unknown accounts gracefully
- [ ] Integration test: mock Geyser → AccountRouter → PoolMatrix → query

---

### 5. QuoteService

**Описание:** HTTP API для получения quotes.

**Файлы:**
- `include/flox/solana/quote_service.h`
- `src/solana/quote_service.cpp`

**Зависимости:**
- cpp-httplib или аналог (header-only preferred)

**Endpoints:**

```
GET /quote
    ?inputMint=<base58>
    &outputMint=<base58>
    &amount=<uint64>

Response:
{
    "inputMint": "So11111111111111111111111111111111111111112",
    "outputMint": "EPjFWdd5AufqSSqeM2qN1xzybapC8G4wEGGkZwyTDt1v",
    "inputAmount": 1000000000,
    "outputAmount": 234567890,
    "priceImpactBps": 12,
    "pool": "PoolAddress...",
    "poolModel": "CONSTANT_PRODUCT",
    "quoteLatencyNs": 85,
    "dataSlot": 312847561
}

GET /health

Response:
{
    "status": "ok",
    "poolCount": 1234,
    "lastSlot": 312847561,
    "updatesPerSec": 45000
}

GET /pools
    ?tokenA=<base58>
    &tokenB=<base58>

Response:
{
    "pools": [
        {
            "address": "...",
            "tokenA": "...",
            "tokenB": "...",
            "reserveA": 123456789,
            "reserveB": 987654321,
            "feeBps": 30,
            "model": "CONSTANT_PRODUCT"
        }
    ]
}
```

**Acceptance Criteria:**
- [ ] /quote возвращает корректный amountOut
- [ ] /quote latency <1ms end-to-end
- [ ] /health показывает актуальные метрики
- [ ] /pools возвращает все pools для пары
- [ ] Error handling (invalid mint, no pool)

---

### 6. Benchmark Suite

**Описание:** Измерение performance всех компонентов.

**Файлы:**
- `benchmarks/solana/geyser_benchmark.cpp`
- `benchmarks/solana/decoder_benchmark.cpp`
- `benchmarks/solana/pool_matrix_benchmark.cpp`
- `benchmarks/solana/quote_benchmark.cpp`

**Метрики:**

| Компонент | Метрика | Target |
|-----------|---------|--------|
| GeyserClient | Updates/sec throughput | >50K/s |
| Decoders | Decode latency | <1μs |
| Decoders | Throughput | >1M/s |
| PoolMatrix.getAmountOut() | Latency | <100ns |
| PoolMatrix.updateReserves() | Latency | <100ns |
| PoolMatrix | Read/write contention | No degradation |
| QuoteService /quote | E2E latency | <1ms |

**Benchmark против Jupiter API:**
```cpp
// Наш quote
auto start = now();
auto myQuote = poolMatrix.getAmountOut(poolId, amountIn, true);
auto myLatency = now() - start;

// Jupiter API (HTTP)
auto jupStart = now();
auto jupResponse = http.get("https://quote-api.jup.ag/v6/quote?...");
auto jupLatency = now() - jupStart;

// Ожидаем: myLatency < 1μs, jupLatency ~ 50-200ms
```

**Acceptance Criteria:**
- [ ] Все benchmarks проходят target
- [ ] Benchmark результаты в CI/README
- [ ] Сравнение с Jupiter API (latency, accuracy)

---

## Структура файлов

```
flox/
├── include/flox/solana/
│   ├── types.h              # Pubkey, common types
│   ├── geyser_client.h
│   ├── account_router.h
│   ├── pool_matrix.h
│   ├── quote_service.h
│   └── decoders/
│       ├── pool_decoder.h   # interface
│       ├── pump_decoder.h
│       ├── raydium_v4_decoder.h
│       └── token_account_decoder.h
├── src/solana/
│   ├── geyser_client.cpp
│   ├── account_router.cpp
│   ├── pool_matrix.cpp
│   ├── quote_service.cpp
│   └── decoders/
│       ├── pump_decoder.cpp
│       └── raydium_v4_decoder.cpp
├── tests/solana/
│   ├── test_pump_decoder.cpp
│   ├── test_raydium_decoder.cpp
│   ├── test_pool_matrix.cpp
│   ├── test_account_router.cpp
│   └── fixtures/            # real account snapshots
│       ├── pump_bonding_curve.bin
│       ├── raydium_amm.bin
│       └── token_account.bin
├── benchmarks/solana/
│   ├── decoder_benchmark.cpp
│   ├── pool_matrix_benchmark.cpp
│   └── quote_benchmark.cpp
└── demo/solana/
    └── quote_engine_demo.cpp
```

---

## Зависимости

```cmake
# CMakeLists.txt additions

find_package(gRPC REQUIRED)
find_package(Protobuf REQUIRED)

# Yellowstone proto
add_custom_command(
    OUTPUT ${PROTO_SRCS} ${PROTO_HDRS}
    COMMAND protoc --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
                   --grpc_out=${CMAKE_CURRENT_BINARY_DIR}
                   -I ${YELLOWSTONE_PROTO_DIR}
                   geyser.proto
)

# HTTP server (header-only)
FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.15.0
)
```

---

## Test Fixtures

Нужны реальные account snapshots для тестов. Получить через:

```bash
# Pump.fun bonding curve
solana account <BONDING_CURVE_ADDRESS> --output json > fixtures/pump_bonding_curve.json

# Raydium AMM
solana account <RAYDIUM_AMM_ADDRESS> --output json > fixtures/raydium_amm.json

# Token account
solana account <TOKEN_ACCOUNT_ADDRESS> --output json > fixtures/token_account.json
```

Или через RPC:
```typescript
const accountInfo = await connection.getAccountInfo(new PublicKey("..."));
fs.writeFileSync("fixture.bin", accountInfo.data);
```

---

## Definition of Done

Phase 1 считается завершённой когда:

1. [ ] GeyserClient подключается к Yellowstone и получает updates
2. [ ] Pump.fun pools корректно декодируются
3. [ ] Raydium V4 pools корректно декодируются (включая vault reserves)
4. [ ] PoolMatrix хранит >1000 pools без деградации
5. [ ] /quote API возвращает корректные quotes
6. [ ] Quote latency <1ms (без учёта HTTP overhead)
7. [ ] Все unit tests проходят
8. [ ] Все benchmarks достигают targets
9. [ ] Сравнение с Jupiter API задокументировано
10. [ ] README с инструкцией запуска

---

## Timeline

| Неделя | Задачи |
|--------|--------|
| 1 | GeyserClient + базовые типы + Pump decoder |
| 2 | Raydium decoder + TokenAccount decoder + PoolMatrix |
| 3 | AccountRouter + QuoteService + интеграция |
| 4 | Benchmarks + тесты + документация |

---

## Вопросы для уточнения

1. Yellowstone endpoint и credentials?
2. Есть ли существующий gRPC setup в проекте?
3. Предпочтения по HTTP серверу (httplib / beast / другое)?
4. Нужен ли Docker для demo?
5. CI pipeline требования?

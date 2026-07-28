# Venue module — design notes

> Historical build plan, kept for the rationale behind the design. Paths were
> rewritten when this became the flox `venue/` module; behaviour is documented
> in `docs/venue/`.

CLOB matching engine (ядро биржи/брокерского venue) поверх FLOX v0.6.8.
C++23. Для операторов бирж и брокеров (crypto spot + derivatives).

> Статус: design doc. Код ещё не писан. Базовый flox склонирован в этот же каталог
> (`include/`, `src/`, `benchmarks/`, ...) как fork-point и источник примитивов.

---

## 0. Резюме решения (TL;DR)

- **FLOX — это buy-side фреймворк** (стратегии подключаются к биржам через коннекторы).
  Matching engine — это **sell-side** (само ядро venue). Поэтому: берём из flox
  фундамент (fixed-point типы, order-модель, Disruptor-шину, L3-книгу, журнал floxlog,
  lifecycle), а **сам матчинг строим новый** — в flox его нет (L3-книга только *учитывает*
  ордера, но не *пересекает* их и не генерит сделки).
- **Архитектура матчинг-ядра — single-writer sequenced core** (LMAX Disruptor):
  один поток на symbol-shard владеет книгой, все мутации сериализованы через ring buffer,
  детерминированно. Это одновременно даёт и производительность, и надёжность
  (детерминированный replay журнала = recovery + аудит + state-machine replication для HA).
- **Интерфейс — многоуровневый, не «REST или нет», а всё сразу через нормализующие шлюзы:**
  - hot path — **бинарный OUCH-style order entry (SBE)** поверх TCP (+ опция kernel-bypass в colo);
  - **FIX 4.4 / 5.0 (FIXT)** gateway — обязателен для институциональной адопции и drop-copy;
  - **REST + WebSocket (JSON)** — онбординг, retail-брокеры, дашборды (высокая адопция, не hot path);
  - market data — **ITCH-style бинарный incremental feed** (UDP multicast в colo / TCP)
    + WebSocket-фан-аут для retail;
  - gRPC — внутренний control-plane (админ, листинг инструментов, риск-лимиты).
  Все шлюзы терминируют в **один внутренний бинарный InboundCommand**, который кормит sequencer.
  Так закрываются оба требования: производительность (бинарный hot path) и адопция (FIX + REST).

---

## 1. Что FLOX уже даёт (инвентаризация переиспользуемого)

| Блок | Файл(ы) в flox | Как используем в матчинг-движке |
|---|---|---|
| **Fixed-point типы** Price/Quantity/Volume, i128/i256, checked-narrow | `include/flox/common.h`, `util/base/decimal.h`, `util/int/` | Детерминированная арифметика без float. Основа всей цены/количества/notional. Готово к использованию как есть. |
| **Order-модель** со всеми venue-полями | `execution/order.h` | TIF, ExecutionFlags (postOnly/reduceOnly/closePosition/holdSide), triggerPrice, trailing, iceberg (`visibleQuantity`), `clientOrderId`, `accountId`, `orderTag` (OCO). Практически готовая входная модель. |
| **Venue-семантика в enum'ах** | `common.h` | `OrderType` (LIMIT/MARKET/STOP_*/TP_*/TRAILING/ICEBERG), `TimeInForce` (GTC/IOC/FOK/GTD/POST_ONLY), `STPMode` (None/CancelNewest/CancelOldest/CancelBoth/Decrement). Уже спроектировано под sell-side. |
| **L3 order book** (order-by-order, time-price FIFO) | `book/l3/l3_order_book.h` | Фундамент структуры книги: intrusive linked-list уровней, open-addressing хэш id→slot, zero-alloc, fixed capacity. **Дорабатываем** (см. §4.3) — best-price в нём O(n) на cache-miss, а findOrCreateLevel имеет O(n) fallback; для hot-path нужен O(1) ladder. |
| **Disruptor / EventBus** (multi-consumer, sequence barriers, CPU-affinity, busy-backoff) | `util/eventing/event_bus.h`, `util/concurrency/spsc_queue.h` | **Это backbone sequencer'а.** Один publisher (gateway ingress) → matching consumer + market-data consumer + journal consumer + risk consumer, каждый со своим sequence barrier. `FLOX_PROFILE_SCOPE("Disruptor::publish")` подтверждает LMAX-паттерн. |
| **Журнал floxlog** (сегменты, индекс, сжатие, snapshot+delta, mmap/parallel reader, validator, manifest) | `replay/writers/binary_log_writer.h`, `replay/readers/`, `replay/ops/` | **Ключ к надёжности.** Пишем каждый принятый InboundCommand и каждый OutboundEvent в журнал → recovery через replay, регуляторный аудит-трейл, state-machine replication на standby. Сегментация+ротация+индекс уже есть. |
| **Engine / lifecycle** | `engine/engine.h`, `engine/abstract_subsystem.h`, `engine/symbol_registry.h`, `engine/trading_calendar.h` | ISubsystem start/stop, реестр символов, торговый календарь (сессии, halts). |
| **Risk / kill-switch абстракции** | `risk/abstract_risk_manager.h`, `risk/portfolio_risk.h`, `killswitch/abstract_killswitch.h` | Базис pre-trade risk и market-wide kill-switch (halt). |
| **Low-latency логирование и метрики** | `log/atomic_logger.h`, `metrics/` | Async-логгер вне hot-path, метрики латентности/throughput. |
| **CPU topology / affinity / core assignment** | `util/performance/{cpu_topology,cpu_affinity,core_assignment,thread_affinity,busy_backoff}.h` | Пиннинг matching-потока на изолированное ядро, NUMA-aware размещение. |
| **SimulatedExecutor** (maker/taker fill, STP-группы, FOK-семантика, queue-модели, slippage/latency) | `backtest/simulated_executor.h` | **Не** матчинг-движок (симулирует venue против истории для одного участника), но **эталон корректной семантики** fill/STP/FOK и готовый **test-oracle** для проверки нашего матчера. |
| **Net / transport / WS** | `net/abstract_transport.h`, `net/abstract_websocket_client.h`, `external/ixwebsocket` | Транспорт для WS market-data фан-аута и REST/WS шлюзов. |
| **Replay-equivalence харнесс** | `tests/replay-equivalence/` | Готовая методология проверки детерминизма (тот же вход → тот же выход). |
| **Бинды и C API** | `capi/`, `python/`, `node/`, `mcp/` | Для control-plane/адмики и MCP-доступа AI-агентов к venue (листинг, лимиты, наблюдение). Не для hot-path. |

**Вывод:** flox покрывает ~50–60% нетривиального фундамента (типы, модель ордера, Disruptor,
журнал, lifecycle, детерминизм-харнесс). Отсутствует ровно то, что делает *матчинг-движок
матчинг-движком*: сам алгоритм пересечения, order gateway, market-data публикация, HA/recovery-оркестрация,
sharding.

---

## 2. Что нужно построить (gap analysis)

1. **Matching core** — пересечение incoming↔resting, генерация fills, price-time FIFO (+ pro-rata опция),
   применение TIF/STP/post-only/iceberg/stop-триггеров. **Нет в flox.**
2. **Оптимизированная книга** — O(1) best bid/ask и O(1) переход по уровням в пределах тик-диапазона
   (ladder/radix), поверх/вместо `L3OrderBook`.
3. **Sequencer / ingress** — приём команд со всех шлюзов, единая последовательность (gateway sequence number),
   валидация, backpressure, идемпотентность (dedup по clientOrderId).
4. **Pre-trade risk** — price bands (limit-up/down), fat-finger (max qty/notional), max open orders,
   для брокеров: credit/margin/buying-power check, self-match scope.
5. **Order gateways** — OUCH-бинарный, FIX, REST/WS (см. §5), каждый нормализует в InboundCommand.
6. **Market-data publisher** — L1/L2/L3 incremental + периодические снапшоты + trade prints;
   бинарный ITCH-style + WS фан-аут; conflation для медленных подписчиков.
7. **Execution reports / drop-copy** — ExecReport назад участнику (ack/reject/fill/cancel), drop-copy поток.
8. **HA / recovery** — журнал-replay для восстановления книги, primary/hot-standby через
   state-machine replication, детерминированный failover.
9. **Sharding** — маппинг symbol→shard, по потоку на shard; нет cross-symbol матчинга (упрощает).
10. **Session / auth / rate-limit** на шлюзах.
11. **Инструментный реестр** — tick size, lot size, min notional, price bands, торговые часы (частично `symbol_registry` + `trading_calendar`).

---

## 3. Сегменты и Jobs (это влияет на дизайн и на интерфейс)

«Биржи и брокеры» — это **два разных сегмента** с разным приоритетом success criteria.
Это не косметика — это прямо определяет, какой интерфейс лидирует и где точка адопции.

**Сегмент A — операторы бирж (venue operators).**
Core Job: *«I want to match client orders fairly, deterministically and at scale, with a
verifiable audit trail»*. Приоритет критериев: **детерминизм/корректность → латентность →
throughput → регуляторная аудируемость → честность матчинга (fair queue)**. Им нужен
бинарный hot-path, multicast MD, HA, journaling-аудит. Латентность и throughput — первый экран.

**Сегмент B — брокеры (internalizers / broker-dealers).**
Core Job: *«I want to internalize client flow and run a crossing/matching book on my own
inventory with risk controls»*. Приоритет: **лёгкость интеграции → pre-trade risk/credit →
multi-account/отчётность → латентность**. Для них латентность вторична, а **FIX + REST + быстрый
онбординг** — первичны. Точка адопции у брокеров — «я подключился за день по FIX/REST», а не «у меня 5 мкс».

**Следствие для дизайна (subtractive move):** одно матчинг-ядро, но **разные шлюзы лидируют для
разных сегментов**. Не строим два движка — строим одно ядро + tiered gateways. Риск-контроли
(credit/margin) делаем опциональным слоем перед sequencer (нужен брокерам, часто выключен у чистой биржи).

---

## 4. Архитектура

### 4.1 Общая схема потока

```
                         ┌─────────── gateways (нормализация в InboundCommand) ───────────┐
 HFT/colo   ──OUCH/SBE──▶│ binary gw │                                                     │
 институты  ──FIX 4.4───▶│  FIX gw   │──▶ [session/auth] ──▶ [rate-limit] ──▶ [pre-trade   │
 retail/бро ──REST/WS───▶│ REST/WS gw│                                          risk?]     │
                         └───────────────────────────────┬─────────────────────────────────┘
                                                          │ InboundCommand (бинарный, fixed-layout)
                                                          ▼
                                              ┌───────────────────────┐
                                              │   INGRESS SEQUENCER    │  единый gseq, dedup,
                                              │  (Disruptor ring)      │  журналирование ДО матчинга
                                              └───────────┬───────────┘
                             ┌────────────────────────────┼───────────────────────────────┐
                             ▼ (shard 0)                   ▼ (shard 1)             ...      │  routing по symbol→shard
                   ┌──────────────────┐          ┌──────────────────┐
                   │ MATCHING CORE    │          │ MATCHING CORE    │  single-writer поток,
                   │ (1 thread/shard) │          │ (1 thread/shard) │  пиннинг на изол. ядро
                   │  order book +    │          │  order book +    │
                   │  matching algo   │          │  matching algo   │
                   └───────┬──────────┘          └───────┬──────────┘
                           │ OutboundEvent (fill / ack / reject / book-delta / trade)
          ┌────────────────┼─────────────────────┬───────────────────┐  multi-consumer Disruptor
          ▼                ▼                     ▼                   ▼
   ┌─────────────┐  ┌──────────────┐    ┌────────────────┐   ┌──────────────┐
   │ JOURNAL     │  │ EXEC REPORTS │    │ MARKET DATA    │   │ RISK/POSITION│
   │ (floxlog)   │  │ + drop-copy  │    │ publisher      │   │ aggregator   │
   │ recovery+   │  │ → участникам │    │ ITCH/WS + snap │   │ (post-trade) │
   │ audit + HA  │  └──────────────┘    └────────────────┘   └──────────────┘
   └──────┬──────┘
          │ same input sequence replicated
          ▼
   ┌─────────────┐
   │ HOT STANDBY │  state-machine replication: тот же gseq-поток → та же книга
   └─────────────┘
```

Инварианты:
- **Ровно один writer на shard.** Никаких локов в hot-path книги. Всё через Disruptor.
- **Детерминизм:** результат = чистая функция от последовательности InboundCommand. Никакого
  wall-clock/random внутри матчинга (время проставляется sequencer'ом и попадает в журнал как данные).
- **Журналируем ВХОД до матчинга** (для recovery достаточно входа + детерминизма) **и ВЫХОД**
  (для аудита/drop-copy/сверки).

### 4.2 Matching core (single-writer sequenced)

- Один поток на shard владеет: order book, реестром активных ордеров, stop-book (условные),
  счётчиками. Читает из своего Disruptor-барьера, обрабатывает команды строго последовательно.
- Обработка `NewOrder`:
  1. валидация против инструмента (tick/lot/min-notional/price-band) — часть в gateway, финальная тут;
  2. проверка STP-scope против resting того же account/группы;
  3. **matching loop** — пока incoming агрессивен и есть встречная ликвидность на пересекающемся уровне:
     берём head очереди (FIFO), matched = min(remaining, restingQty), эмитим `TradeEvent`
     (maker price), уменьшаем/снимаем resting, уменьшаем incoming;
  4. остаток по TIF: GTC → в книгу; IOC → отменить остаток; FOK → предчек полной ликвидности,
     иначе reject целиком (семантику брать из `SimulatedExecutor` FOK AnyPrice/BookVWAP);
     POST_ONLY → reject если бы пересёкся; iceberg → показать `visibleQuantity`, пополнять из скрытого.
  5. эмитим `ExecReport` (ack/partial/filled) и book-delta.
- `Cancel` / `Replace(Modify)` — O(1) по id-хэшу (уже есть в L3-книге); replace = cancel+add с
  потерей приоритета при росте qty/смене цены (или сохранение при уменьшении qty — конфигурируемо, как на реальных venue).
- **Stop / trigger orders** — отдельный stop-book; триггерятся при пересечении last/mark price,
  затем инжектятся в матчинг как market/limit. Триггер-цену (last vs mark vs index) делаем настройкой инструмента.

### 4.3 Структура книги (доработка L3OrderBook)

`L3OrderBook` берём как основу очередей уровня (intrusive FIFO, zero-alloc, id-хэш), но заменяем
ценовую индексацию на структуру с **O(1) best и O(1) шагом по уровням**:

- **Спот/узкий тик-диапазон:** прямой массив-ladder, индекс = `(price - refPrice)/tickSize`,
  плюс битовая карта занятых уровней (`find_next` через `__builtin_ctzll`) для быстрого поиска
  следующего непустого уровня. Best bid/ask — O(1).
- **Широкий диапазон / derivatives:** двухуровневый radix (bucket → ladder) или bounded flat map
  с курсором best. Fallback O(n)-скан из текущего `findOrCreateLevel` в hot-path недопустим — убираем.
- Уровень — `PriceLevel{ totalQty, head, tail, orderCount }` (order-count нужен для pro-rata и метрик).
- Всё аллоцируется заранее (fixed capacity на shard), zero-alloc в hot-path — инвариант flox сохраняем.

### 4.4 Алгоритмы матчинга

- **Price-time priority (FIFO)** — дефолт. Честно, просто, ожидаемо участниками. Уже согласуется с
  «Time Price FIFO» инвариантом L3-книги.
- **Pro-rata / size-time** — опция для инструментов с толстыми уровнями (часто на деривативах):
  распределение пропорционально размеру resting на топ-уровне, с min-fill и time-остатком.
  Выбор алгоритма — свойство инструмента, а не глобальный флаг.
- Алгоритм за интерфейсом `IMatchPolicy` (одна виртуалка на *инструмент*, не на *ордер* — вне hot-loop),
  чтобы не платить за виртуальный вызов на каждый fill.

### 4.5 Детерминизм и sequencing

- Каждый принятый InboundCommand получает **global sequence number (gseq)** и **timestamp**, проставленные
  ingress-sequencer'ом *до* журналирования. Внутри матчинга — никакого чтения часов/random.
- Trade id / order id — из детерминированных монотонных счётчиков на shard (seedятся из журнала при recovery).
- Это даёт три вещи бесплатно: **recovery** (replay журнала входов), **HA** (тот же вход на standby = та же книга),
  **аудит/спор-резолюшн** (побитово воспроизводимая история). Переиспользуем `tests/replay-equivalence`.

### 4.6 Pre-trade risk (слой перед sequencer, опциональный)

- Дешёвые проверки (price band, max qty/notional, max open orders per account, kill-switch/halt) — в
  gateway/ingress, чтобы мусор не доходил до матчинга. **[СДЕЛАНО]** в движке: tick/lot/min-qty,
  fat-finger `maxOrderQty`/`maxOrderNotional`, LULD-band, для перпов **position limit** `maxPositionQty`
  (реджект `PositionLimitExceeded`, если полный филл увёл бы |позицию| за лимит; reduce-only не блокируется),
  и **max open orders** `maxOpenOrders` per-account (реджект `TooManyOpenOrders` — DoS/риск-гейт на живые
  resting-ордера; слот освобождается отменой). Rate-limit сессий — `flox::RateLimitPolicy` (см. §5).
- Для брокеров: **credit/buying-power/margin** предчек против позиции аккаунта — использует
  `risk/abstract_risk_manager.h` + `portfolio_risk.h`. Может быть выключен для чистой биржи.
- Market-wide halt / kill-switch — через `killswitch/abstract_killswitch.h` + `trading_calendar` (сессии).

---

## 5. Интерфейсы — главный вопрос («REST или как-то ещё»)

**Короткий ответ: не выбирать одно. Hot-path НЕ должен быть REST/JSON.** Стандарт индустрии для
бирж/брокеров — многоуровневый набор протоколов, все терминируют в один внутренний бинарный
InboundCommand. Это единственный способ закрыть *и* производительность, *и* адопцию.

| Уровень | Протокол | Транспорт | Сегмент / назначение | Латентность | Усилие реализации |
|---|---|---|---|---|---|
| **T0 hot path** | **OUCH-style бинарный, SBE (Simple Binary Encoding)** | TCP (colo); опц. kernel-bypass (io_uring / DPDK / OpenOnload) | HFT, маркет-мейкеры, сегмент A | единицы–десятки мкс | высокое |
| **T1 институты** | **FIX 4.4 / FIXT 1.1 (FIX 5.0 SP2)** + drop-copy | TCP (FIX session) | институциональные клиенты, брокеры-агрегаторы, adoption | сотни мкс–мс | среднее (готовые парсеры: QuickFIX, но лучше кастом SBE-FIX для скорости) |
| **T2 адопция/retail** | **REST (JSON) + WebSocket** | HTTP/1.1 + WS | онбординг, retail-брокеры, дашборды, боты | мс+ | низкое |
| **MD hot** | **ITCH-style бинарный incremental** (add/exec/cancel/trade) + периодич. snapshot | UDP multicast (colo) / TCP unicast | data-подписчики сегмента A | десятки мкс | высокое |
| **MD retail** | **WebSocket JSON** (L2 depth, trades, ticker), conflated | WS | retail, дашборды | десятки мс | низкое |
| **Control plane** | **gRPC** (+ существующий FLOX C API / MCP) | HTTP/2 | админ: листинг инструментов, tick/lot, риск-лимиты, halt, наблюдение | — | низкое |

**Порядок реализации (с учётом выбора HFT-colo, см. §13):** OUCH/SBE + multicast MD — **first-class с самого
начала**, параллельно с REST/WS для демо/онбординга; FIX — вторым, для институтов/брокеров. То есть
латентный бинарный стек не откладываем на потом (как было бы при «fast enough»), а ведём его как основную
ветку сегмента A; REST/WS остаётся дешёвым онбординг-каналом сегмента B.
Внимание к риску: спрос на суб-10-мкс — это riskiest assumption #2 (§12); валидируем интервью **параллельно**
со стройкой hot-path, а не «после».

Принципы шлюзов:
- Каждый gateway — отдельный процесс/пул потоков, **вне** matching-потока. Он только парсит,
  аутентифицирует, rate-лимитит, нормализует в InboundCommand и публикует в ingress Disruptor.
- **Backpressure:** Disruptor `tryPublish` с таймаутом уже есть в `event_bus.h`; при переполнении —
  reject с явным кодом, не блокировка матчинга.
- **Идемпотентность:** dedup по `(session, clientOrderId)` на ingress — защита от ретраев/реконнектов.
- FLOX C API / MCP (`flox_capi.h`, `mcp/`) переиспользуем для control-plane и для того, чтобы
  AI-агенты могли наблюдать/управлять venue (листинг, лимиты, halt) — это уникальный «AI-native» козырь flox.

---

## 6. Надёжность / HA / recovery

- **Журналирование (floxlog):** ingress пишет каждый InboundCommand (с gseq+ts) в `BinaryLogWriter`
  *до* передачи в матчинг; матчинг-выход (trades/exec-reports) — во второй журнал для аудита/сверки.
  Сегментация, ротация, индекс, сжатие, snapshot+delta — уже реализованы. **Mark-price и funding —
  тоже sequenced-команды** (`SetMark`/`ApplyFunding` в `InboundCommand`), т.е. журналируются наравне с
  ордерами. Иначе derivatives-recovery разъехалась бы: mark-driven ликвидации и funding-выплаты не
  воспроизвелись бы при replay. Проверено `test_perp_recovery` (крах mark → ликвидация → replay
  восстанавливает позиции И балансы точно).
- **Recovery:** при рестарте — reader (`mmap_reader`/`parallel_reader`) проигрывает журнал входов
  через тот же матчинг → книга восстановлена побитово. Периодические book-снапшоты (`writeBook`)
  ускоряют recovery (replay только с последнего снапшота). **Проверено на уровне денег** (`test_venue`):
  восстановленный ledger совпадает с живым ТОЧНО — per account + venue, base + quote, available + reserved
  (не только поток событий; recovery с верными событиями, но неверными балансами была бы катастрофой).
- **HA (hot standby):** state-machine replication — тот же gseq-поток входов реплицируется на standby,
  standby гоняет тот же детерминированный матчинг → идентичная книга. Failover = standby становится
  primary с последнего подтверждённого gseq. Реплика подтверждает запись в журнал перед ack клиенту
  (настраиваемо: sync/async, trade-off латентность↔durability). **Проверено на уровне денег** (`test_ha`,
  200k команд с settlement): standby, WAL-recovered реплика и post-failover движок реконструируют ledger
  ТОЧНО — per account + venue, base + quote, available + reserved (не только книга/поток событий).
- **Идемпотентный failover:** клиенты после реконнекта запрашивают состояние своих ордеров
  (order/exec-report resend по gseq), dedup по clientOrderId предотвращает дубли.
- **Validator** (`replay/ops/validator.h`) — проверка целостности журнала; **manifest** — метаданные сегментов.

---

## 7. Масштабирование

- **Sharding по символам:** `symbol → shardId` (consistent/статический маппинг). Каждый shard —
  один матчинг-поток на выделенном ядре. Cross-symbol матчинга в CLOB нет → shards полностью независимы,
  масштаб линейный по ядрам/узлам. `multi_symbol_benchmark.cpp` в flox — стартовая точка для замеров.
- **Горизонталь между узлами:** shard-группы на разных машинах; ingress-роутер направляет команду на узел
  владельца символа. Market-data агрегируется на edge-публикаторах.
- **Что НЕ шардим:** аккаунт-риск, где ордера аккаунта идут по разным символам, —
  post-trade позиция агрегируется отдельным risk-consumer'ом (eventual), а pre-trade credit-чек
  на ingress использует кэш buying-power с периодической сверкой (иначе cross-shard consensus в hot-path — дорого).
- **Кросс-маржа (портфельный риск) [СДЕЛАНО]:** `flox-venue/cross_margin.h::CrossMarginManager` —
  ровно этот cross-shard risk-consumer. Один кошелёк залога держит весь перп-портфель аккаунта:
  `equity = wallet + Σ uPnL_s`, `IM = Σ |q_s|·mark_s·imBps_s`, `MM = Σ |q_s|·mark_s·mmBps_s` по всем символам.
  Pre-trade гейт (`canOpen`) допускает ордер только если post-trade IM ≤ equity; вывод залога — гейт
  `canWithdraw`/`withdrawable` (нельзя вывести обеспечение под открытыми позициями: `equity − amount ≥ IM`);
  ликвидация — по
  **агрегатному** equity < MM (прибыль по одному символу фондирует убыток по другому — в этом суть cross
  против isolated в движке). Расчёт PnL/ликвидации/страх-фонда — тот же клиринг-пул, что и в движке
  (value conserved: каждый realize кредитует аккаунт и дебетует venue-пул). Драйвится с risk-потока,
  не с матчинг-потоков. Проверено: `tests/test_cross_margin.cpp` (IM-гейт, cross-offset, агрегатная
  ликвидация, банкротство+страховка, conservation, **funding** — longs pay shorts по портфельным
  позициям, пул в ноль на сбалансированной книге) + фаззы `test_cross_margin_conservation` (100k опов,
  2 символа, 0 breaches).
- **Mark/index price feed (anti-манипуляция) [СДЕЛАНО]:** `flox-venue/index_feed.h`. Mark-цена гонит
  PnL, funding и **ликвидацию**, поэтому её нельзя брать из last trade перпа — на тонкой книге один принт
  дёргает mark и вызывает каскад ликвидаций. Два слоя: `IndexAggregator` — медиана нескольких внешних
  spot-источников с отбросом устаревших (staleness TTL) и грубых выбросов (deviation-фильтр: один
  взломанный/лагающий источник не двигает индекс, но синхронный широкий сдвиг — трекается); `MarkPrice` —
  медиана из {index, last, book-mid}, зажатая в band вокруг индекса. **Book-mid — это impact-mid**
  (`impactPriceRaw`/`impactMidRaw`): VWAP на заполнение фиксированного impact-нотионала вглубь книги, а не
  сырой top-of-book, — dust/спуф на вершине не двигает mark (проверено `test_impact_price`: спуф 90 при
  реальной глубине на 100 → impact ≈ 100). Адаптер `bookImpactMidRaw(book, impactQty)` считает impact-mid
  прямо с живой движковой книги (`levels()` best-first, работает с MatchingBook/LadderBook) — проверено
  `test_book_impact_mid` на реальном движке. Детерминизм: staleness через явный
  `nowNs`, wall-clock не читается. Проверено `tests/test_mark_price.cpp`, включая e2e-кейс
  `test_clamp_prevents_liquidation`: манипулятивный принт 80 при индексе 100 клампится в 98 → аккаунт
  остаётся платёжеспособным, ложной ликвидации нет.
- **Circuit breaker ликвидаций [СДЕЛАНО]:** `CrossMarginManager::setLiquidationsPaused` — при сбое/устаревании
  фида оператор паузит ликвидации, чтобы плохая цена не выкосила книгу массовой ликвидацией. Mark всё равно
  обновляется (PnL/equity живые), но sweep пропускается; после восстановления фида — возобновляется.
  Проверено `test_cross_margin_liquidations_paused` (краш-mark при паузе не ликвидирует; unpause → sweep).
  **Авто-пауза [СДЕЛАНО]:** `MarkFeedDriver` (`flox-venue/mark_feed_driver.h`) связывает `IndexAggregator::hasIndex`
  с circuit breaker'ом: на каждом тике при свежем индексе публикует mark и снимает паузу, при устаревшем
  (feed outage) — сам ставит `setLiquidationsPaused(true)`. Оператору не надо дёргать флаг вручную. Проверено
  `test_mark_feed_driver`: свежий фид → mark+ликвидация, устаревший → авто-пауза (краш не ликвидирует),
  восстановление → sweep.
- **Funding по premium-TWAP [СДЕЛАНО]:** `flox-venue/funding_rate.h::FundingCalculator` расширен интервальным
  путём: `sample(mark,index)` копит премию по всему интервалу, `intervalRate()` считает ставку от
  усреднённой премии (Binance-формула: `premium + clamp(interest − premium, ±band)`, cap). Одиночный
  манипулятивный принт разбавляется усреднением и не двигает выплату (в отличие от снапшот-`rate()`).
  Проверено в `tests/test_funding.cpp` (снапшот vs TWAP при спайке, устойчивая премия, reset).
  **Планировщик** `flox-venue/funding_scheduler.h` замыкает жизненный цикл: семплит премию по интервалу
  и на границе вызывает `engine.applyFunding(rate, mark)` + reset (детерминизм через явный `nowNs`).
  E2E-проверка `tests/test_funding_scheduler.cpp`: long/short пара, положительная премия → лонг платит
  шорту, пул в ноль, аккумулятор сброшен между интервалами.
- **ADL (auto-deleveraging) [СДЕЛАНО]:** бэкстоп после страх-фонда в **обоих** маржин-режимах —
  портфельном `CrossMarginManager` (флаг `autoDeleverage`) и isolated-margin движке (`SymbolConfig::autoDeleverage`,
  `MatchingEngine::autoDeleverageEngine`). Когда банкротство пробивает залог и дефицит ушёл бы в страх-фонд,
  дефицит вместо этого забирается у самых прибыльных контрагентов на противоположной стороне: их выигрышная
  позиция закрывается по mark, а выигрыш подрезается (haircut) — ранжирование по uPnL desc, до покрытия
  дефицита. Событие `Liquidation{adl=true}`. Conservation держится по построению (каждая проводка
  сбалансирована). Проверено `test_cross_margin_adl` (портфель) и `test_perp_engine_adl` (isolated):
  дефицит забран у шорта, страх-фонд не тронут, socialized loss не размазан на всех.
- **Capacity:** цель — сотни тысяч–миллионы ордеров/сек на shard (single-writer, zero-alloc, cache-resident book).
  Реальные числа фиксируем бенчмарками до заявлений (см. §10).

---

## 8. Производительность (инженерные решения)

- **CPU pinning + изоляция ядер:** матчинг-поток на isolated core (`isolcpus`/`nohz_full`), NUMA-local
  память — через `util/performance/{cpu_affinity,core_assignment,cpu_topology}.h`.
- **Zero-alloc hot-path:** вся память книги/очередей — преаллоцированные пулы (`util/memory/pool.h`),
  никаких `new`/`malloc`/exceptions в матчинге. Инвариант L3-книги сохраняем и распространяем на весь core.
- **Busy-poll vs backoff:** Disruptor уже поддерживает busy-backoff; для colo — busy-spin, для эконом-режима — backoff.
- **Cache alignment:** `alignas(64)` на горячих структурах (в L3-книге уже так), false-sharing исключаем.
- **Бинарные fixed-layout сообщения** (SBE) → zero-copy парсинг, без аллокаций/reflection.
- **Логи/метрики вне hot-path:** `atomic_logger` async, метрики — lock-free counters, экспорт отдельным потоком.
- **Целевые метрики (гипотезы, подтвердить бенчами):** tick-to-trade p50 < 5 мкс / p99 < 20 мкс (без сети,
  colo TCP); throughput > 1M orders/s/shard; jitter p99.9 ограничен (важнее среднего для venue).

---

## 9. Наблюдаемость (метрики, мониторинг, обсервабилити)

Venue без наблюдаемости в прод не выходит. Три уровня: **метрики** (числовые ряды),
**трейсинг/аудит** (событийный лог), **алертинг** (пороги → дежурный). Всё вне hot-path:
матчинг-поток только инкрементит lock-free счётчики и пишет в гистограмму, экспорт — отдельным потоком.

- **Latency-гистограмма (`flox-venue/metrics.h::LatencyHistogram`).** Реализована: log2-бакеты
  (`__builtin_clzll`), запись O(1) без аллокаций, чтение перцентилей (p50/p99/p99.9), mean/max.
  Снимается на submit (tick-to-trade внутри core) и на gateway (end-to-end с сетью). Один писатель
  на матчинг-потоке, монитор-поток снапшотит.
- **Событийные счётчики (`Metrics`).** Реализовано: derived из outbound-потока — `accepted`, `trades`,
  `cancels`, `rejects`, `modifies`, `liquidations`, `holds`, кумулятивный `volumeRaw` (traded notional).
  `observe(OutboundEvent)` вызывается на каждом исходящем событии.
- **Gauges (point-in-time состояние) [СДЕЛАНО]:** `Gauges` в `metrics.h` + `prom::render(Metrics, Gauges)` —
  экспорт того, что НЕ выводится из потока событий: баланс страх-фонда (`fme_insurance_fund_raw`), текущая
  funding-ставка (`fme_funding_rate`), open interest (`fme_open_interest_raw`), число открытых позиций и
  живых ордеров, **возраст mark-цены** (`fme_mark_price_age_ns` — алерт на лаг фида) и **состояние
  circuit breaker'а** (`fme_liquidations_paused` — 0/1). Семплируется мониторинг-потоком из
  ledger/risk-manager/feed. Проверено `test_prometheus_gauges`.
  **Сэмплер замкнут на живое состояние [СДЕЛАНО]:** `CrossMarginManager::openInterestRaw/openPositionCount`,
  `MatchingEngine::restingOrderCount`, insurance = `ledger.total(venue)` → заполняют `Gauges` для реального
  `/metrics`. E2E-проверка `tests/test_gauge_sampler.cpp` (перп-OI + spot resting → отрендеренная страница).
- **Reject-rate по причинам [СДЕЛАНО]:** `Metrics::rejectsByReason` (индекс по `RejectReason`) →
  labeled-серия `fme_rejects_by_reason_total{reason="..."}` в Prometheus. Спайк реджектов виден с разбивкой
  по причине (fat-finger / position-limit / LULD / too-many-orders / ...). Проверено в `test_metrics.cpp`.
- **Что ещё экспортировать (venue-KPI):** глубина/спред книги (bestBid/bestAsk, суммарный notional по
  уровням), fill-rate по MM (вход в MMP), возраст самого старого resting-ордера, лаг журнала
  (ingress→durable), лаг standby-реплики (gseq primary − gseq standby), размер очереди Disruptor
  (backpressure), funding/mark-price age, число ликвидаций и покрытие ими.
- **Экспозиция [СДЕЛАНО]:** pull-модель — HTTP `/metrics` в Prometheus-формате
  (`flox-venue/prometheus.h` — текстовый рендер снапшота `Metrics`: counter-блоки + latency-`summary`
  с квантилями p50/p99/p99.9, `_sum`, `_count`; i128 traded-notional). HTTP-сервер
  (`flox-venue/metrics_server.h`) на своём потоке через `SocketAcceptor`, никогда не трогает hot-path:
  `/metrics`, `/healthz` (процесс жив), `/readyz` (журнал durable, реплика в синхроне, книга загружена —
  через инъектируемый readiness-callback, 200/503). Snapshot/ready — колбэки, чтобы вызывающий сам
  выбирал синхронизацию чтения матчинг-thread'ового `Metrics`. Push-альтернатива (statsd/OTLP) — при
  необходимости для окружений без Prometheus. Проверено сквозным скрейпом в `tests/test_metrics.cpp`.
- **Структурный аудит-лог:** каждый матчинг-выход уже журналируется (floxlog, §6) — это же источник
  для аудита/форензики и для дифф-сверки primary↔standby. Трейсинг: сквозной `clientOrderId`/`gseq`
  связывает вход → fills → settlement через все слои (gateway, core, ledger).
- **Алертинг (пороги → дежурный):** p99 tick-to-trade выше SLO, reject-rate spike, лаг журнала/реплики
  выше порога, insurance ниже минимума, рост очереди Disruptor (перегрузка), «зависший» resting-ордер,
  расхождение money-conservation (немедленный critical — см. `test_conservation`), gap в gseq.
  Алерты на симптомы деградации *до* отказа, а не постфактум.
- **Дашборды:** latency (перцентили tick-to-trade и end-to-end), throughput (orders/s, trades/s по shard),
  состояние книги (спред/глубина), риск (open interest, ликвидации, insurance), надёжность (лаг журнала/реплики,
  uptime, failover-события).
- **Вне hot-path — жёсткий инвариант.** Метрики: lock-free counters + гистограмма на стеке потока; логи:
  async (`atomic_logger`); экспорт/скрейп/рендер — отдельным потоком. Матчинг никогда не блокируется на I/O
  наблюдаемости.

**Проверка:** `tests/test_venue.cpp` снимает реальную submit-latency (p50/p99) и сверяет счётчики
(trades/volume) с фактическим потоком событий сквозь весь путь (OUCH → core+ledger+fees → md+journal+resend+metrics).

---

## 10. Тестирование и корректность

- **Детерминизм:** харнесс `tests/replay-equivalence` — один вход, прогон N раз (и на primary+standby) →
  побитово идентичный выход.
- **Test-oracle:** `SimulatedExecutor` как эталон семантики fill/STP/FOK/post-only — прогоняем те же
  последовательности через наш core и сверяем fills.
- **Property-based / conformance:** инварианты книги (sum(level.qty)=Σorders, best bid < best ask всегда,
  FIFO-приоритет не нарушается, notional-consistency через checked i128), STP-модусы, TIF-семантика.
- **Fuzzing сетевых парсеров [СДЕЛАНО]:** `tests/test_parser_fuzz.cpp` — truncation + bit-flip corruption
  + случайные буферы (сотни тысяч итераций на кодек) для **всех wire-парсеров, принимающих сетевой ввод**:
  OUCH, FIX, REST/JSON, **WebSocket frame parser** (`ws::parseFrame` — 64-битные length-заголовки,
  маскирование), **WS handshake** (HTTP-заголовки), **ITCH-декодер** (big-endian поля). Под ASAN+UBSAN.
  **Нашёл и починил реальный UB:** враждебное числовое поле (`"price":5e52`) переполняло
  `Decimal::fromDouble` (double→int64 out-of-range); добавлен saturating `safeDecimal<D>()` на границе
  REST/FIX-парсеров (flox read-only, фикс на нашей стороне). Всё чисто под ASAN+UBSAN.
- **Replay реальных нагрузок** через floxlog: записали продовый вход → гоняем регресс на каждом релизе.
- **Соответствие протоколам:** FIX conformance (session-level: resend, seq-reset, heartbeat, logon/logout);
  OUCH/ITCH — golden-vector тесты.
- **Money-conservation фаззы [СДЕЛАНО]:** после КАЖДОЙ операции сверяем сумму по каждому активу
  (accounts + venue) — spot (200k опов, **весь спектр типов/TIF под ledger: limit/market/cancel/modify/GTD/
  OCO/peg/IOC/FOK/post-only — каждый корректно освобождает/переоформляет/сеттлит reservation**),
  linear-perp без и с ADL (по 100k, + **reserved-инвариант: после осушения книги Σ reserved == Σ
  position-margin — IM-резерв не течёт**), cross-margin 2 символа
  (100k), мультиколлатерал 2 актива с revaluation + collateral-liquidation (100k, сохранение quote И coin),
  0 breaches. ADL и collateral-конвертация фаззятся отдельно — это самая тонкая денежная логика.
- **Sanitizer-прогон [СДЕЛАНО]:** весь верификационный корпус прогнан под санитайзерами. **ASAN+UBSAN:**
  все сетевые парсеры + 2M differential (обе книги, ladder dense-arrays/bitmap) + ~600k conservation
  (ledger/cross-margin/ADL/collateral-conversion) + ~210k integration (perp-venue + multi-symbol soak) —
  ~2.8M движковых опов, чисто (один UB-баг найден и починен, см. выше). **TSAN:** sequenced Disruptor-shard
  (500k команд ingress→matching→journal) и многопоточные шлюзы (TCP/WS/control/UDP) — data races не найдены.
- **Multi-symbol soak [СДЕЛАНО]:** `tests/test_multi_symbol_soak.cpp` — 3 spot-символа как независимые
  шарды за `SymbolRouter`, общий ledger, per-symbol market-data + общие метрики, 150k случайных опов
  (~92k сделок). Проверяет композицию: conservation по каждому активу на каждом шаге, MD-топ-книги ==
  движковой книге, well-formed книги (best bid < best ask). Здесь всплывают баги композиции, которые
  покомпонентные тесты не ловят.
- **Perp-venue harness [СДЕЛАНО]:** `tests/test_perp_venue.cpp` — весь деривативный путь в одном прогоне:
  движок-матчер → Trade-поток → `CrossMarginManager` (расчёт + портфельный риск), index-агрегатор +
  impact-mid с живой книги → `MarkPrice` → `setMark` (ликвидации) + `FundingScheduler` (фандинг), pre-trade
  `canOpen`-гейт, ADL, gauge-сэмплер. 60k опов (~26k сделок, ~59 фандинг-расчётов): collateral conserved на
  каждом шаге (0 breaches), страх-фонд почти цел (ADL поглощает банкротов). Деривативный аналог spot-soak.

---

## 11. Дорожная карта (MVP-как-probe → продукт)

Каждый этап — проверка риск-гипотезы, а не «фича». Дешевле убить/развернуть рано.
Секвенс отражает выбор §13: **HFT-colo + spot & derivatives + обе модели поставки** → hot-path и
деривативы не откладываем.

- **Этап 0 — Matching core (spot, FIFO, single-shard), сразу под латентность. [СДЕЛАНО]**
  Book-ladder O(1) (`ladder_book.h`, zero-alloc), matching loop, TIF (GTC/IOC/FOK)/STP/post-only,
  cancel/replace, **iceberg**, **стопы** (STOP/TAKE_PROFIT market+limit, TRAILING_STOP, каскад по last price),
  детерминизм + журнал (`journal.h`) + replay-recovery, sharded-роутер, бенчмарк (`bench_book`) и
  дифференциальный fuzz (2M ops, ladder == reference). Текущий `engine.submit` со всей venue-логикой:
  ladder ~255 ns/op (~3.9M ops/s), ~1.5× map-reference (раннее ~1.9× было на голом stage-0 матчинге —
  ratio сжался, т.к. фиксированная per-submit venue-стоимость растёт одинаково для обеих книг).
  Осталось в рамках этапа: пиннинг/isolated-cores на реальном железе.
  *Проверено на dev: корректность (94+ проверок, 2M differential); латентность colo — впереди (риск #3).*
- **Этап 1a — Sequenced ingress на настоящем Disruptor. [СДЕЛАНО]** `sequenced_shard.h` на
  `flox::EventBus`: ingress (один consumer = single-writer матчинг) + outbound fan-out + WAL на потоке
  матчинга; тест `sequenced` подтверждает побитовое совпадение с прямым прогоном (~3.1M cmd/s end-to-end).
  Outbound MD-лента через floxlog `BinaryLogWriter` (`tape_recorder.h`, `test_tape`). Осталось: backpressure-политики.
- **Этап 1 — Hot-path стек (сегмент A) + онбординг. [СДЕЛАНО на уровне кодеков]** Бинарный **OUCH/SBE
  order-entry** (`ouch_codec.h`), **ITCH-style MD** (`market_data.h`+`itch_codec.h`, L2 на флоксовом
  `NLevelOrderBook`), **FIX 4.4** (`fix_codec.h`), **REST/JSON** (`rest_json.h`). Осталось: сетевой транспорт
  (TCP/WS-серверы, session/auth/rate-limit), UDP-multicast, busy-poll/isolated cores на железе (риск #2).
- **Этап 2 — Derivatives. [СДЕЛАНО]** Mark-price триггеры стопов (`setMarkPrice`); margin/liquidation через
  **`flox::LiquidationEngine`** (`liquidation_monitor.h`, `test_derivatives`) с маршрутизацией закрывающих
  ордеров в движок; pro-rata как опция инструмента (`test_prorata`). Осталось: funding-feed для перпетуалов,
  pre-trade credit/margin для брокеров.
- **Этап 3 — HA + sharding + kernel-bypass. [СДЕЛАНО (кроме kernel-bypass)]** Hot standby (state-machine
  replication) + failover + recovery — `test_ha`; symbol-sharding — `symbol_router.h`. Осталось: kernel-bypass
  (io_uring/DPDK/OpenOnload) для T0, capacity-бенчи на железе.
- **Этап 4 — Операционка + упаковка поставки. [control-plane СДЕЛАН]** `control_plane.h` (`InstrumentRegistry`):
  листинг, tick/lot/bands, halts, trigger-ref — `test_control_plane`. Осталось: gRPC/MCP-обёртка,
  наблюдаемость, две упаковки (on-prem / managed SaaS).

---

## 12. Riskiest assumptions (проверить до крупных вложений)

Ранжировано по `(вероятность ошибки × цена ошибки) / цена проверки`:

1. **Сегмент+Job:** реально ли брокеры/малые биржи *покупают отдельный matching-движок*, а не берут готовую
   биржевую платформу «под ключ» / white-label CEX? Самый дорогой риск. Проверка: 5–7 интервью с
   операторами/брокерами по AJTBD (не про фичи — про то, за что уже платят и где ломается цепочка).
   Дёшево, до кода.
2. **Латентность как критерий покупки:** нужен ли целевому сегменту суб-10-мкс, или «надёжно и по FIX за день»
   выигрывает? Определяет, T0 (OUCH/kernel-bypass) или T1/T2 (FIX/REST) — первый билд. Проверка: те же интервью + анализ,
   на чём сегодня торгуют.
3. **Correctness/детерминизм под нагрузкой:** держит ли single-writer + журнал заявленный throughput с
   ограниченным p99.9 jitter. Проверка: бенчмарк ядра на этапе 0 (дёшево, рано).
4. **Regulatory/аудит:** достаточно ли floxlog-трейла для требований к venue (аудируемость, спор-резолюшн,
   retention). Проверка: свериться с требованиями целевой юрисдикции до этапа 4.

Дефолт по каждому «кастомному» риску: **снять допущение, а не строить под него** — напр. первую когорту
брокеров подключить по REST/FIX (низкий риск, быстрая адопция), а OUCH/kernel-bypass строить только
если интервью подтвердят спрос на латентность.

---

## 13. Решения (зафиксированы) и оставшиеся вопросы

**Зафиксировано (2026-07-26):**

1. **Класс латентности — HFT-colo (единицы мкс).** OUCH/SBE + multicast MD + kernel-bypass — first-class.
   Zero-alloc/pinning/cache-layout закладываем с этапа 0. Спрос на суб-10-мкс держим как riskiest
   assumption #2 и валидируем интервью **параллельно** со стройкой (не «после»).
2. **Инструменты — spot + derivatives.** Спот в этапах 0–1, деривативы (mark/index, funding, margin,
   liquidation, pro-rata) — этап 2. `backtest/liquidation_engine.h` как база маржи/ликвидаций.
3. **Поставка — обе модели, ядро одно.** On-prem (лицензия + deploy-артефакт + HA-runbook) и managed SaaS
   (мы хостим, клиент по API). Различие только в деплое/конфиге/control-plane — этап 4.

**Осталось решить:**

4. **Матчинг по умолчанию.** FIFO (рекомендую дефолт) с pro-rata как опция инструмента — подтвердить.
5. **Форк vs зависимость от flox.** Рекомендую: flox как upstream-submodule в `external/` (тянуть апдейты
   типов/журнала/Disruptor), новый код — отдельным слоем в `src/matching/`, `src/gateway/`, `src/marketdata/`,
   `src/risk/`. Сейчас flox склонирован в корень как fork-point — надо переоформить (либо submodule, либо
   вычистить лишнее и оставить как vendored base). Подтвердить подход.
6. **Kernel-bypass стек.** io_uring (проще, Linux-native) vs DPDK vs OpenOnload (Solarflare NIC) — зависит от
   целевого железа colo клиентов. Решаем ближе к этапу 3, но железо стоит уточнить у первых клиентов заранее.
```

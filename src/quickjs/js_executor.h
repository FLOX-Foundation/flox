#pragma once

#include "js_strategy.h"

#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace flox
{

// Default capacity of a FloxJsExecutor's event queue. The queue is bounded
// and backpressures producers on overflow -- it never drops an event.
inline constexpr size_t kFloxJsExecutorDefaultQueueCapacity = 65536;

// Runs one FloxJsStrategy on a single thread that it owns for the whole
// lifetime of the object, and serializes every entry into that strategy's
// JSRuntime through one bounded multi-producer, single-consumer queue.
//
// Background: QuickJS is not reentrant -- a JSRuntime may only ever be
// touched by one thread. `FloxJsStrategy` on its own relies on the caller
// to already guarantee that, which holds for `flox_js_runner` and for
// every synchronous unit test in this repo (one thread, one call at a
// time). It does NOT hold for a live `flox_live_engine`: adding a strategy
// subscribes it to three independently-threaded event buses (trades, book
// updates, bars), each of which runs its own consumer thread and calls
// straight into the strategy's callbacks from whatever thread that is.
// Wiring `FloxJsStrategy::getCallbacks()` directly into a live engine means
// up to three threads can enter the same JSRuntime at once. That is not a
// rare hot-reload edge case -- it is the live engine's default
// configuration -- and it corrupts the interpreter's internal object-shape
// table within the first handful of events (confirmed: 10 events is
// enough; a plain single-threaded run of 400,000 events is clean).
//
// The fix is not a mutex around the runtime: that would only re-serialize
// three already-threaded event buses relative to each other, which breaks
// the ordering guarantees the eventing model makes to every other
// subscriber, not just this one, and nothing enforces that every entry
// point -- including finalizers and JS_RunGC -- goes through the same
// lock. Instead: exactly one thread ever owns the runtime, for its entire
// life. Bus callbacks on any producer thread only enqueue; the JS side of
// every callback body -- market data, fills, order updates, start, stop --
// runs exclusively on that one dedicated thread.
//
// Use `FloxJsExecutor` instead of a bare `FloxJsStrategy` for any
// embedding that can call its callbacks from more than one thread (in
// particular: `flox_live_engine_add_strategy`). Single-threaded callers
// (`flox_js_runner`, the unit tests) keep using `FloxJsStrategy` directly;
// see docs/bindings/javascript.md, "Threads".
class FloxJsExecutor
{
 public:
  explicit FloxJsExecutor(const std::string& scriptPath, SymbolRegistry& registry,
                          size_t queueCapacity = kFloxJsExecutorDefaultQueueCapacity);
  ~FloxJsExecutor();

  FloxJsExecutor(const FloxJsExecutor&) = delete;
  FloxJsExecutor& operator=(const FloxJsExecutor&) = delete;

  // Callbacks that only ever enqueue. Safe to call from any thread,
  // including several threads concurrently.
  FloxStrategyCallbacks getCallbacks();

  // Symbols the script registered. Stable after construction (construction
  // blocks until the script has finished loading on the executor thread).
  const std::vector<uint32_t>& symbolIds() const;

  // Runs on the executor thread as a queued command, like everything else.
  void injectHandle(FloxStrategyHandle handle);

  // Blocks the calling thread until every event enqueued strictly before
  // this call has finished dispatching. Producers on the live-engine path
  // never need this; it exists for tests and for hot-reload coordination
  // (drain the old executor before starting the new one).
  void waitIdle();

  // Approximate queue depth. Diagnostics only -- may be stale the instant
  // it is read.
  size_t queueDepthApprox() const;

  // Reads a global int32 value, marshaled through the queue so the read
  // itself runs on the owning thread like everything else. Mainly for
  // tests: production code on the live-engine path has no business
  // reaching into a running strategy's globals from outside.
  int32_t getGlobalInt32(const std::string& name);

 private:
  enum class Kind : uint8_t
  {
    Trade,
    Book,
    Bar,
    Fill,
    OrderUpdate,
    QueuePositionChange,
    MarketPositionChange,
    Start,
    Stop,
    InjectHandle,
    Barrier,
    ReadGlobalInt32
  };

  struct Event
  {
    Kind kind{};
    FloxSymbolContext ctx{};
    FloxTradeData trade{};
    FloxBookData book{};
    FloxBarData bar{};
    FloxOrderEventData orderEv{};
    // FloxOrderEventData::reject_reason is a borrowed const char* that is
    // only valid for the duration of the originating BridgeStrategy call.
    // Copy the text so it survives until the executor thread processes it.
    std::string rejectReasonStorage;
    FloxStrategyHandle handle = nullptr;
    std::shared_ptr<std::promise<void>> barrier;
    std::string globalName;
    std::shared_ptr<std::promise<int32_t>> intResult;
  };

  void push(Event&& ev);
  void run();
  // Take the shutdown exit when the worker thread died instead of stopping:
  // close the queue, wake both condition variables, and break every promise
  // the dead worker will never keep.
  void releaseWaiters() noexcept;
  void dispatchOne(Event& ev, const FloxStrategyCallbacks& cb);
  static Event makeOrderEvent(const FloxSymbolContext* ctx, const FloxOrderEventData* data);

  static void onTrade(void*, const FloxSymbolContext*, const FloxTradeData*);
  static void onBook(void*, const FloxSymbolContext*, const FloxBookData*);
  static void onBar(void*, const FloxSymbolContext*, const FloxBarData*);
  static void onFill(void*, const FloxSymbolContext*, const FloxOrderEventData*);
  static void onOrderUpdate(void*, const FloxSymbolContext*, const FloxOrderEventData*);
  static void onQueuePositionChange(void*, const FloxSymbolContext*, const FloxOrderEventData*);
  static void onMarketPositionChange(void*, const FloxSymbolContext*, const FloxOrderEventData*);
  static void onStart(void*);
  static void onStop(void*);

  std::string _scriptPath;
  SymbolRegistry& _registry;
  size_t _capacity;

  std::unique_ptr<FloxJsStrategy> _strategy;  // constructed on _thread
  std::thread _thread;

  std::mutex _queueMu;
  std::condition_variable _notEmpty;
  std::condition_variable _notFull;
  std::deque<Event> _queue;
  bool _closed = false;

  std::mutex _initMu;
  std::condition_variable _initCv;
  bool _initDone = false;
  std::exception_ptr _initError;

  static const std::vector<uint32_t> kEmptySymbolIds;
};

}  // namespace flox

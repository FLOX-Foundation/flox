#include "js_executor.h"

namespace flox
{

const std::vector<uint32_t> FloxJsExecutor::kEmptySymbolIds;

FloxJsExecutor::FloxJsExecutor(const std::string& scriptPath, SymbolRegistry& registry,
                               size_t queueCapacity)
    : _scriptPath(scriptPath), _registry(registry), _capacity(queueCapacity)
{
  _thread = std::thread(&FloxJsExecutor::run, this);

  std::unique_lock<std::mutex> lock(_initMu);
  _initCv.wait(lock, [this]
               { return _initDone; });
  if (_initError)
  {
    lock.unlock();
    if (_thread.joinable())
    {
      _thread.join();
    }
    std::rethrow_exception(_initError);
  }
}

FloxJsExecutor::~FloxJsExecutor()
{
  {
    std::lock_guard<std::mutex> lock(_queueMu);
    _closed = true;
  }
  _notEmpty.notify_all();
  _notFull.notify_all();
  if (_thread.joinable())
  {
    _thread.join();
  }
}

void FloxJsExecutor::push(Event&& ev)
{
  std::unique_lock<std::mutex> lock(_queueMu);
  // Bounded queue, backpressure on overflow. This fix must never trade
  // a memory-corruption bug for a silent-data-loss one: a full queue
  // blocks the producer (a bus consumer thread) rather than dropping the
  // event.
  _notFull.wait(lock, [this]
                { return _closed || _queue.size() < _capacity; });
  if (_closed)
  {
    return;
  }
  _queue.push_back(std::move(ev));
  lock.unlock();
  _notEmpty.notify_one();
}

void FloxJsExecutor::run()
{
  try
  {
    _strategy = std::make_unique<FloxJsStrategy>(_scriptPath, _registry);
    // The strategy (and its FloxJsEngine) were constructed on this thread,
    // so FloxJsEngine's default owner-thread binding is already correct.
    // rebindOwnerThread() is only needed for embeddings that move
    // construction across threads; call it anyway to make the invariant
    // explicit rather than relying on constructor-thread coincidence.
    _strategy->engine().rebindOwnerThread();
  }
  catch (...)
  {
    std::lock_guard<std::mutex> lock(_initMu);
    _initError = std::current_exception();
    _initDone = true;
    _initCv.notify_all();
    return;
  }

  {
    std::lock_guard<std::mutex> lock(_initMu);
    _initDone = true;
  }
  _initCv.notify_all();

  auto cb = _strategy->getCallbacks();

  for (;;)
  {
    Event ev;
    {
      std::unique_lock<std::mutex> lock(_queueMu);
      _notEmpty.wait(lock, [this]
                     { return _closed || !_queue.empty(); });
      if (_queue.empty())
      {
        // Closed and fully drained.
        break;
      }
      ev = std::move(_queue.front());
      _queue.pop_front();
      lock.unlock();
      _notFull.notify_one();
    }
    dispatchOne(ev, cb);
  }

  // FloxJsStrategy's destructor touches the runtime (it evaluates a
  // cleanup script to drop JS-held references before teardown). It must
  // run here, on the thread that owns the engine -- not later, whenever
  // FloxJsExecutor's own destructor happens to run on the caller's
  // thread. std::unique_ptr's implicit destruction would get that wrong.
  _strategy.reset();
}

void FloxJsExecutor::dispatchOne(Event& ev, const FloxStrategyCallbacks& cb)
{
  switch (ev.kind)
  {
    case Kind::Trade:
      if (cb.on_trade)
      {
        cb.on_trade(cb.user_data, &ev.ctx, &ev.trade);
      }
      break;
    case Kind::Book:
      if (cb.on_book)
      {
        cb.on_book(cb.user_data, &ev.ctx, &ev.book);
      }
      break;
    case Kind::Bar:
      if (cb.on_bar)
      {
        cb.on_bar(cb.user_data, &ev.ctx, &ev.bar);
      }
      break;
    case Kind::Fill:
    case Kind::OrderUpdate:
    case Kind::QueuePositionChange:
    case Kind::MarketPositionChange:
      ev.orderEv.reject_reason =
          ev.rejectReasonStorage.empty() ? nullptr : ev.rejectReasonStorage.c_str();
      if (ev.kind == Kind::Fill && cb.on_fill)
      {
        cb.on_fill(cb.user_data, &ev.ctx, &ev.orderEv);
      }
      else if (ev.kind == Kind::OrderUpdate && cb.on_order_update)
      {
        cb.on_order_update(cb.user_data, &ev.ctx, &ev.orderEv);
      }
      else if (ev.kind == Kind::QueuePositionChange && cb.on_queue_position_change)
      {
        cb.on_queue_position_change(cb.user_data, &ev.ctx, &ev.orderEv);
      }
      else if (ev.kind == Kind::MarketPositionChange && cb.on_market_position_change)
      {
        cb.on_market_position_change(cb.user_data, &ev.ctx, &ev.orderEv);
      }
      break;
    case Kind::Start:
      if (cb.on_start)
      {
        cb.on_start(cb.user_data);
      }
      break;
    case Kind::Stop:
      if (cb.on_stop)
      {
        cb.on_stop(cb.user_data);
      }
      break;
    case Kind::InjectHandle:
      _strategy->injectHandle(ev.handle);
      break;
    case Kind::Barrier:
      break;
    case Kind::ReadGlobalInt32:
    {
      auto* ctx = _strategy->engine().context();
      JSValue v = _strategy->engine().getGlobalProperty(ev.globalName.c_str());
      int32_t out = 0;
      JS_ToInt32(ctx, &out, v);
      JS_FreeValue(ctx, v);
      if (ev.intResult)
      {
        ev.intResult->set_value(out);
      }
      break;
    }
  }
  if (ev.barrier)
  {
    ev.barrier->set_value();
  }
}

FloxStrategyCallbacks FloxJsExecutor::getCallbacks()
{
  FloxStrategyCallbacks cb{};
  cb.on_trade = &FloxJsExecutor::onTrade;
  cb.on_book = &FloxJsExecutor::onBook;
  cb.on_bar = &FloxJsExecutor::onBar;
  cb.on_start = &FloxJsExecutor::onStart;
  cb.on_stop = &FloxJsExecutor::onStop;
  cb.on_fill = &FloxJsExecutor::onFill;
  cb.on_order_update = &FloxJsExecutor::onOrderUpdate;
  cb.on_queue_position_change = &FloxJsExecutor::onQueuePositionChange;
  cb.on_market_position_change = &FloxJsExecutor::onMarketPositionChange;
  cb.user_data = this;
  return cb;
}

const std::vector<uint32_t>& FloxJsExecutor::symbolIds() const
{
  return _strategy ? _strategy->symbolIds() : kEmptySymbolIds;
}

void FloxJsExecutor::injectHandle(FloxStrategyHandle handle)
{
  Event ev;
  ev.kind = Kind::InjectHandle;
  ev.handle = handle;
  push(std::move(ev));
  waitIdle();
}

void FloxJsExecutor::waitIdle()
{
  auto prom = std::make_shared<std::promise<void>>();
  auto fut = prom->get_future();
  Event ev;
  ev.kind = Kind::Barrier;
  ev.barrier = prom;
  push(std::move(ev));
  fut.wait();
}

size_t FloxJsExecutor::queueDepthApprox() const
{
  std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(_queueMu));
  return _queue.size();
}

int32_t FloxJsExecutor::getGlobalInt32(const std::string& name)
{
  auto prom = std::make_shared<std::promise<int32_t>>();
  auto fut = prom->get_future();
  Event ev;
  ev.kind = Kind::ReadGlobalInt32;
  ev.globalName = name;
  ev.intResult = prom;
  push(std::move(ev));
  return fut.get();
}

void FloxJsExecutor::onTrade(void* ud, const FloxSymbolContext* ctx, const FloxTradeData* trade)
{
  auto* self = static_cast<FloxJsExecutor*>(ud);
  Event ev;
  ev.kind = Kind::Trade;
  ev.ctx = *ctx;
  ev.trade = *trade;
  self->push(std::move(ev));
}

void FloxJsExecutor::onBook(void* ud, const FloxSymbolContext* ctx, const FloxBookData* book)
{
  auto* self = static_cast<FloxJsExecutor*>(ud);
  Event ev;
  ev.kind = Kind::Book;
  ev.ctx = *ctx;
  ev.book = *book;
  self->push(std::move(ev));
}

void FloxJsExecutor::onBar(void* ud, const FloxSymbolContext* ctx, const FloxBarData* bar)
{
  auto* self = static_cast<FloxJsExecutor*>(ud);
  Event ev;
  ev.kind = Kind::Bar;
  ev.ctx = *ctx;
  ev.bar = *bar;
  self->push(std::move(ev));
}

FloxJsExecutor::Event FloxJsExecutor::makeOrderEvent(const FloxSymbolContext* ctx,
                                                     const FloxOrderEventData* data)
{
  Event ev;
  ev.ctx = *ctx;
  ev.orderEv = *data;
  if (data->reject_reason)
  {
    ev.rejectReasonStorage = data->reject_reason;
  }
  return ev;
}

void FloxJsExecutor::onFill(void* ud, const FloxSymbolContext* ctx, const FloxOrderEventData* ev)
{
  auto* self = static_cast<FloxJsExecutor*>(ud);
  auto e = makeOrderEvent(ctx, ev);
  e.kind = Kind::Fill;
  self->push(std::move(e));
}

void FloxJsExecutor::onOrderUpdate(void* ud, const FloxSymbolContext* ctx,
                                   const FloxOrderEventData* ev)
{
  auto* self = static_cast<FloxJsExecutor*>(ud);
  auto e = makeOrderEvent(ctx, ev);
  e.kind = Kind::OrderUpdate;
  self->push(std::move(e));
}

void FloxJsExecutor::onQueuePositionChange(void* ud, const FloxSymbolContext* ctx,
                                           const FloxOrderEventData* ev)
{
  auto* self = static_cast<FloxJsExecutor*>(ud);
  auto e = makeOrderEvent(ctx, ev);
  e.kind = Kind::QueuePositionChange;
  self->push(std::move(e));
}

void FloxJsExecutor::onMarketPositionChange(void* ud, const FloxSymbolContext* ctx,
                                            const FloxOrderEventData* ev)
{
  auto* self = static_cast<FloxJsExecutor*>(ud);
  auto e = makeOrderEvent(ctx, ev);
  e.kind = Kind::MarketPositionChange;
  self->push(std::move(e));
}

void FloxJsExecutor::onStart(void* ud)
{
  auto* self = static_cast<FloxJsExecutor*>(ud);
  Event ev;
  ev.kind = Kind::Start;
  self->push(std::move(ev));
  // start() is setup-time, not a hot-path market-data callback; wait so
  // callers that assume "started" means "onStart already ran" (the
  // contract a bare FloxJsStrategy has always had) keep seeing that.
  self->waitIdle();
}

void FloxJsExecutor::onStop(void* ud)
{
  auto* self = static_cast<FloxJsExecutor*>(ud);
  Event ev;
  ev.kind = Kind::Stop;
  self->push(std::move(ev));
  // BridgeStrategy::stop() (and process exit right after it, in
  // flox_js_runner) does not wait for the callback to actually run. Drain
  // here so onStop -- and anything still ahead of it in the queue -- has
  // completed by the time stop() returns, matching the single-threaded
  // FloxJsStrategy contract callers already depend on.
  self->waitIdle();
}

}  // namespace flox

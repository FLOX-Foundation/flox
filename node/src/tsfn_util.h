// node/src/tsfn_util.h -- one ownership and lifetime rule for every threaded
// dispatch path in the addon.
//
// A ThreadSafeFunction is two things at once, and the two have different
// lifetimes. It is a queue that a C++ producer thread pushes work onto, and it
// is a reference that keeps Node's event loop open. Neither half unwinds on
// its own, so both have to be handed back at a point the code picks:
//
//  * The loop reference. A TSFN released only from a destructor is released
//    never. The destructors here belong to ObjectWraps, so they wait for
//    garbage collection, garbage collection waits for the loop to go idle, and
//    the loop cannot go idle while the TSFN holds it open. A script that did
//    nothing but construct a threaded runner ran to its last line and then sat
//    there until something killed it.
//
//  * The queue. Release does not empty it. Items already in it are dispatched
//    normally, on a later tick, well after the destructor that called Release
//    has returned -- so a consumer whose first move is to read through a
//    pointer to its owner reads freed memory.
//
// Hence the split below. The channel is scoped to a run; the owner flag is
// scoped to the owner and is shared with every payload the owner queues, so
// the question "is my owner still there" stays answerable after the owner is
// gone.
//
// The rule each owner follows:
//
//   open    when a producer thread is about to exist, and not before --
//           nothing should hold the loop open while no producer exists
//   close   after the producer threads have been joined; items already queued
//           still run, which is how a strategy's final onStop reaches JS
//   retire  before the owner's storage goes away; queued items then free their
//           payload and return instead of calling into it
//
// Abort() is the other way to get rid of a queue, and it is the wrong one.
// node-addon-api's CallJS returns early when the queue is being torn down --
// env and jsCallback are both null, see napi-inl.h -- which drops the callback
// wrapper on the floor along with the payload captured inside it. Draining
// through Release with the owner flag retired frees both.
//
// Queue bound: every call site used to build its TSFN with max_queue_size 0
// and ignore the status NonBlockingCall returns. Unbounded meant no
// backpressure -- 38 MB of resident growth measured at 200k queued events, an
// abort at 2M when the queue's allocator gave up -- and the ignored status
// meant the caller's heap payload leaked whenever the queue was full or
// closing, because NonBlockingCall's own cleanup owns only the wrapper it
// allocates internally. TsfnChannel::call bounds the queue and frees the
// payload on any non-ok status.

#pragma once
#include <napi.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

// Deliberately NOT wrapped in a namespace: the node addon aggregates
// headers under two different namespaces for historical reasons
// (`node_flox` in strategy.h/data_ops.h/..., `flox_node` in hooks.h), and
// both need these unqualified.

// Bounded so a producer burst pushes back instead of growing without limit;
// ~13 MB worst case at the ~200 bytes/event measured for TradeCallData.
inline constexpr size_t kTsfnMaxQueueSize = 65536;

// Owner-scoped liveness flag. Allocated once per owner, shared with every
// payload that owner queues, and outlives the owner because the payloads hold
// their own share of it.
struct TsfnOwner
{
  std::atomic<bool> _retired{false};

  bool retired() const { return _retired.load(std::memory_order_acquire); }
  void retire() { _retired.store(true, std::memory_order_release); }
};

using TsfnOwnerPtr = std::shared_ptr<TsfnOwner>;

inline TsfnOwnerPtr makeTsfnOwner() { return std::make_shared<TsfnOwner>(); }

// Stand-in payload for calls that carry no data. Routing them through the
// payload form keeps one code path: node-addon-api's payload-free
// NonBlockingCall allocates a wrapper internally and leaks it whenever the
// status comes back non-ok, which is exactly the case a bounded queue makes
// ordinary.
struct TsfnNoPayload
{
};

// Run-scoped queue. Created when a producer thread is about to exist,
// released when the producer threads have been joined.
class TsfnChannel
{
 public:
  TsfnChannel(Napi::Env env, Napi::Function fn, const char* name)
      : _tsfn(Napi::ThreadSafeFunction::New(env, fn, name, kTsfnMaxQueueSize, 1))
  {
  }

  TsfnChannel(const TsfnChannel&) = delete;
  TsfnChannel& operator=(const TsfnChannel&) = delete;

  // JS thread, once the producer threads have been joined. Idempotent, so an
  // explicit stop() followed by destruction releases exactly once. Items
  // already queued are still dispatched; Napi::ThreadSafeFunction is a handle
  // with no destructor of its own, so this object may go away afterwards
  // while the queue drains.
  void close()
  {
    if (!_closed.exchange(true, std::memory_order_acq_rel))
    {
      _tsfn.Release();
    }
  }

  // Producer thread. Takes ownership of `data` either way: a closed or full
  // queue frees it rather than leaking it.
  template <typename T, typename Callback>
  void call(T* data, Callback callback)
  {
    if (_closed.load(std::memory_order_acquire) ||
        _tsfn.NonBlockingCall(data, callback) != napi_ok)
    {
      delete data;
    }
  }

 private:
  Napi::ThreadSafeFunction _tsfn;
  std::atomic<bool> _closed{false};
};

// What an owner holds: the flag for its whole life, the channel for the
// duration of a run.
//
// `_chan` is written only while no producer thread exists -- openChannel()
// runs before the engine starts (or before the owner is handed to a running
// one), closeChannel() runs after the engine has stopped and joined -- so the
// thread create/join pair orders every write against every read.
class TsfnHost
{
 public:
  explicit TsfnHost(Napi::Env env) : _env(env) {}

  TsfnHost(const TsfnHost&) = delete;
  TsfnHost& operator=(const TsfnHost&) = delete;

  ~TsfnHost()
  {
    // Order matters. Retire first, because close() does not empty the queue:
    // whatever is still in it runs on a later tick, by which point this owner
    // and everything it holds are gone.
    _owner->retire();
    closeChannel();
  }

  const TsfnOwnerPtr& owner() const { return _owner; }

  // JS thread. `fn` is what a payload's callback receives as its Function
  // argument; call sites that build their own JS values pass a placeholder.
  void openChannel(Napi::Function fn, const char* name)
  {
    if (!_chan)
    {
      _chan = std::make_unique<TsfnChannel>(_env, fn, name);
    }
  }

  void openChannel(const char* name)
  {
    openChannel(Napi::Function::New(_env, [](const Napi::CallbackInfo&) {}), name);
  }

  // JS thread, after the producer threads have been joined. The channel
  // object is dropped so a later openChannel() builds a fresh one rather than
  // handing out a released queue that silently swallows every call.
  void closeChannel()
  {
    if (_chan)
    {
      _chan->close();
      _chan.reset();
    }
  }

  bool channelOpen() const { return _chan != nullptr; }

  // Producer thread. Frees the payload when no channel is open.
  template <typename T, typename Callback>
  void post(T* data, Callback callback)
  {
    if (_chan)
    {
      _chan->call(data, std::move(callback));
    }
    else
    {
      delete data;
    }
  }

 protected:
  Napi::Env _env;

 private:
  TsfnOwnerPtr _owner{makeTsfnOwner()};
  std::unique_ptr<TsfnChannel> _chan;
};

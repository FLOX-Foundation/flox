#pragma once

extern "C"
{
#include "quickjs.h"
}

#include <chrono>
#include <string>
#include <thread>

namespace flox
{

// Default wall-clock budget for a single JS callback invocation before the
// interrupt handler asks QuickJS to unwind. See `beginWork()`.
inline constexpr std::chrono::milliseconds kFloxJsDefaultInterruptBudget{2000};

// Default cap on the native call stack QuickJS is allowed to use. Bounds
// runaway JS recursion (and OOM-handling recursion inside the interpreter
// itself) well under a std::thread's default native stack, on every
// platform this engine ships on.
inline constexpr size_t kFloxJsDefaultMaxStackSize = 1 * 1024 * 1024;

// A single QuickJS runtime + context. Not reentrant, not thread-safe: a
// JSRuntime may only ever be touched from the thread that owns it.
// `FloxJsEngine` binds an owner thread at construction (or later via
// `rebindOwnerThread()`, for embedders such as `FloxJsExecutor` that build
// the engine on a dedicated background thread) and refuses -- loudly in
// debug builds, with a logged no-op in release -- any call made from a
// different thread. See docs/bindings/javascript.md, "Threads".
class FloxJsEngine
{
 public:
  FloxJsEngine(size_t memoryLimitBytes = 32 * 1024 * 1024,
               size_t maxStackSizeBytes = kFloxJsDefaultMaxStackSize);
  ~FloxJsEngine();

  FloxJsEngine(const FloxJsEngine&) = delete;
  FloxJsEngine& operator=(const FloxJsEngine&) = delete;

  bool eval(const std::string& code, const std::string& filename = "<eval>");
  bool loadFile(const std::string& path);

  JSContext* context() const { return _ctx; }
  JSRuntime* runtime() const { return _rt; }

  std::string getErrorMessage();

  JSValue getGlobalProperty(const char* name) const;
  bool setGlobalProperty(const char* name, JSValue val);

  // Re-binds the owning thread to the caller. Only safe to call before any
  // other thread has ever touched this engine (e.g. as the first action on
  // a freshly-spawned dedicated executor thread that constructed this
  // engine's *owner* elsewhere but wants itself to be the real owner).
  void rebindOwnerThread();
  bool isOwnerThread() const { return std::this_thread::get_id() == _ownerThreadId; }

  // Resets the interrupt-handler clock. Call once at the start of
  // dispatching each externally-triggered event (trade, book update, bar,
  // start, stop, ...) so a script that loops forever inside a single
  // callback is interrupted instead of running unbounded.
  void beginWork();
  void setInterruptBudget(std::chrono::milliseconds budget) { _interruptBudget = budget; }

  // Drains QuickJS's microtask (promise reaction) queue. Must be called
  // after every event dispatch -- otherwise `await`/`.then()` inside a
  // strategy callback never runs, and the queue grows without bound until
  // the heap limit kills the process. Returns the number of jobs run.
  int pumpPendingJobs(int maxIterations = 100000);

 private:
  bool checkOwnerThread(const char* where) const;
  static int interruptHandlerTrampoline(JSRuntime* rt, void* opaque);
  int handleInterrupt();

  JSRuntime* _rt = nullptr;
  JSContext* _ctx = nullptr;
  std::thread::id _ownerThreadId;
  std::chrono::milliseconds _interruptBudget{kFloxJsDefaultInterruptBudget};
  std::chrono::steady_clock::time_point _workStarted;
  bool _workInProgress = false;
};

}  // namespace flox

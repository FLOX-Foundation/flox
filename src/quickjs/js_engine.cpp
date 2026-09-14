#include "js_engine.h"
#include "js_cstring.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>

namespace flox
{

FloxJsEngine::FloxJsEngine(size_t memoryLimitBytes, size_t maxStackSizeBytes)
    : _ownerThreadId(std::this_thread::get_id())
{
  _rt = JS_NewRuntime();
  if (memoryLimitBytes > 0)
  {
    JS_SetMemoryLimit(_rt, memoryLimitBytes);
  }
  if (maxStackSizeBytes > 0)
  {
    JS_SetMaxStackSize(_rt, maxStackSizeBytes);
  }
  JS_SetInterruptHandler(_rt, &FloxJsEngine::interruptHandlerTrampoline, this);
  _ctx = JS_NewContext(_rt);
}

FloxJsEngine::~FloxJsEngine()
{
  // Teardown must happen on the owner thread too: finalizers and JS_RunGC
  // are entries into the runtime like any other.
  checkOwnerThread("~FloxJsEngine");
  if (_ctx)
  {
    JS_FreeContext(_ctx);
  }
  if (_rt)
  {
    JS_FreeRuntime(_rt);
  }
}

void FloxJsEngine::rebindOwnerThread()
{
  _ownerThreadId = std::this_thread::get_id();
}

bool FloxJsEngine::checkOwnerThread(const char* where) const
{
  if (std::this_thread::get_id() == _ownerThreadId)
  {
    return true;
  }
#ifndef NDEBUG
  std::cerr << "[flox-js] FATAL: " << where
            << " entered this JSRuntime from a thread that does not own it. "
               "A JSRuntime may only be used from one thread; see "
               "docs/bindings/javascript.md#threads."
            << std::endl;
  assert(false && "FloxJsEngine entered from a non-owning thread");
  return false;
#else
  std::cerr << "[flox-js] refusing " << where
            << ": called from a thread that does not own this JSRuntime" << std::endl;
  return false;
#endif
}

bool FloxJsEngine::eval(const std::string& code, const std::string& filename)
{
  if (!checkOwnerThread("FloxJsEngine::eval"))
  {
    return false;
  }
  JSValue val = JS_Eval(_ctx, code.c_str(), code.size(), filename.c_str(), JS_EVAL_TYPE_GLOBAL);
  if (JS_IsException(val))
  {
    return false;
  }
  JS_FreeValue(_ctx, val);
  return true;
}

bool FloxJsEngine::loadFile(const std::string& path)
{
  if (!checkOwnerThread("FloxJsEngine::loadFile"))
  {
    return false;
  }
  std::ifstream file(path);
  if (!file.is_open())
  {
    return false;
  }
  std::stringstream buf;
  buf << file.rdbuf();
  return eval(buf.str(), path);
}

std::string FloxJsEngine::getErrorMessage()
{
  if (!checkOwnerThread("FloxJsEngine::getErrorMessage"))
  {
    return "unknown error (wrong thread)";
  }

  // JS_GetException() on a context with nothing pending returns JS_NULL.
  // Reading a "stack" property off that would itself throw a fresh
  // TypeError and leave it pending for the next caller to trip over.
  JSValue exception = JS_GetException(_ctx);
  if (JS_IsNull(exception) || JS_IsUndefined(exception))
  {
    JS_FreeValue(_ctx, exception);
    return "(no pending exception)";
  }

  flox::JsCString msg(_ctx, exception);
  std::string result = msg ? msg : "unknown error";

  JSValue stack = JS_GetPropertyStr(_ctx, exception, "stack");
  if (!JS_IsUndefined(stack) && !JS_IsException(stack))
  {
    flox::JsCString stackStr(_ctx, stack);
    if (stackStr)
    {
      result += "\n";
      result += stackStr;
    }
  }
  JS_FreeValue(_ctx, stack);
  JS_FreeValue(_ctx, exception);
  return result;
}

JSValue FloxJsEngine::getGlobalProperty(const char* name) const
{
  if (!checkOwnerThread("FloxJsEngine::getGlobalProperty"))
  {
    return JS_UNDEFINED;
  }
  JSValue global = JS_GetGlobalObject(_ctx);
  JSValue val = JS_GetPropertyStr(_ctx, global, name);
  JS_FreeValue(_ctx, global);
  return val;
}

bool FloxJsEngine::setGlobalProperty(const char* name, JSValue val)
{
  if (!checkOwnerThread("FloxJsEngine::setGlobalProperty"))
  {
    JS_FreeValue(_ctx, val);
    return false;
  }
  JSValue global = JS_GetGlobalObject(_ctx);
  int ret = JS_SetPropertyStr(_ctx, global, name, val);
  JS_FreeValue(_ctx, global);
  return ret >= 0;
}

void FloxJsEngine::beginWork()
{
  _workStarted = std::chrono::steady_clock::now();
  _workInProgress = _interruptBudget.count() > 0;
}

int FloxJsEngine::interruptHandlerTrampoline(JSRuntime*, void* opaque)
{
  return static_cast<FloxJsEngine*>(opaque)->handleInterrupt();
}

int FloxJsEngine::handleInterrupt()
{
  if (!_workInProgress)
  {
    return 0;
  }
  auto elapsed = std::chrono::steady_clock::now() - _workStarted;
  if (elapsed > _interruptBudget)
  {
    // 1 asks QuickJS to throw and unwind at the next opportunity. A
    // runaway `while (true) {}` inside a strategy callback used to be
    // recoverable only with SIGKILL; this turns it into a catchable
    // exception delivered back through the normal onTrade/onBar error path.
    return 1;
  }
  return 0;
}

int FloxJsEngine::pumpPendingJobs(int maxIterations)
{
  if (!checkOwnerThread("FloxJsEngine::pumpPendingJobs"))
  {
    return 0;
  }
  int ran = 0;
  JSContext* jobCtx = nullptr;
  while (ran < maxIterations && JS_IsJobPending(_rt))
  {
    int rc = JS_ExecutePendingJob(_rt, &jobCtx);
    if (rc < 0)
    {
      // The job threw. That exception is an unhandled promise rejection;
      // report it the same way a synchronous callback error is reported
      // and keep draining the rest of the queue.
      std::cerr << "[flox-js] unhandled rejection: " << getErrorMessage() << std::endl;
    }
    else if (rc == 0)
    {
      break;
    }
    ++ran;
  }
  return ran;
}

}  // namespace flox

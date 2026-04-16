#include "js_engine.h"

#include <fstream>
#include <sstream>

namespace flox
{

FloxJsEngine::FloxJsEngine(size_t memoryLimitBytes)
{
  _rt = JS_NewRuntime();
  if (memoryLimitBytes > 0)
  {
    JS_SetMemoryLimit(_rt, memoryLimitBytes);
  }
  _ctx = JS_NewContext(_rt);
}

FloxJsEngine::~FloxJsEngine()
{
  if (_ctx)
  {
    JS_FreeContext(_ctx);
  }
  if (_rt)
  {
    JS_FreeRuntime(_rt);
  }
}

bool FloxJsEngine::eval(const std::string& code, const std::string& filename)
{
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
  JSValue exception = JS_GetException(_ctx);
  const char* msg = JS_ToCString(_ctx, exception);
  std::string result = msg ? msg : "unknown error";
  JS_FreeCString(_ctx, msg);

  JSValue stack = JS_GetPropertyStr(_ctx, exception, "stack");
  if (!JS_IsUndefined(stack))
  {
    const char* stackStr = JS_ToCString(_ctx, stack);
    if (stackStr)
    {
      result += "\n";
      result += stackStr;
      JS_FreeCString(_ctx, stackStr);
    }
  }
  JS_FreeValue(_ctx, stack);
  JS_FreeValue(_ctx, exception);
  return result;
}

JSValue FloxJsEngine::getGlobalProperty(const char* name) const
{
  JSValue global = JS_GetGlobalObject(_ctx);
  JSValue val = JS_GetPropertyStr(_ctx, global, name);
  JS_FreeValue(_ctx, global);
  return val;
}

bool FloxJsEngine::setGlobalProperty(const char* name, JSValue val)
{
  JSValue global = JS_GetGlobalObject(_ctx);
  int ret = JS_SetPropertyStr(_ctx, global, name, val);
  JS_FreeValue(_ctx, global);
  return ret >= 0;
}

}  // namespace flox

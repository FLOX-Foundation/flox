#pragma once

extern "C"
{
#include "quickjs.h"
}

#include <string>

namespace flox
{

class FloxJsEngine
{
 public:
  FloxJsEngine(size_t memoryLimitBytes = 32 * 1024 * 1024);
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

 private:
  JSRuntime* _rt = nullptr;
  JSContext* _ctx = nullptr;
};

}  // namespace flox

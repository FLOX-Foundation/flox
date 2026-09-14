#pragma once

extern "C"
{
#include "quickjs.h"
}

#include <string>

namespace flox
{

// Owning wrapper around JS_ToCString.
//
// JS_ToCString returns nullptr whenever the value cannot be turned into a
// string: a Symbol, a Proxy whose trap throws, an object with a throwing
// toString, or an allocation failure under JS_SetMemoryLimit. Before this
// type existed every call site had to remember two things -- test the
// result and free it on every exit path -- and an audit of the binding
// surface found the test missing in roughly half of them, each one a
// strlen(nullptr) one script line away.
//
// The type removes the second obligation outright (the destructor frees)
// and makes the first one hard to skip, because the natural spelling of a
// call site now starts with FLOX_JS_CSTRING_OR_THROW below. The implicit
// conversion to const char* is deliberate: it keeps `f(ctx, s)` and
// `s ? s : ""` reading the way they always did, so adopting the type at an
// existing call site is a one-line change that cannot leave a stray free
// behind.
class JsCString
{
 public:
  JsCString(JSContext* ctx, JSValueConst value)
      : _ctx(ctx), _ptr(JS_ToCString(ctx, value))
  {
  }

  // An absent optional argument: no conversion attempted, no exception
  // pending, and `ok()` is false. Use `present()` to tell the two apart.
  JsCString() = default;

  ~JsCString()
  {
    if (_ptr != nullptr)
    {
      JS_FreeCString(_ctx, _ptr);
    }
  }

  JsCString(const JsCString&) = delete;
  JsCString& operator=(const JsCString&) = delete;
  JsCString(JsCString&&) = delete;
  JsCString& operator=(JsCString&&) = delete;

  // True when the conversion produced a string. False means QuickJS has a
  // pending exception the caller should propagate with JS_EXCEPTION,
  // unless the holder was default-constructed.
  bool ok() const { return _ptr != nullptr; }
  bool present() const { return _ctx != nullptr; }

  const char* get() const { return _ptr; }
  operator const char*() const { return _ptr; }

  // For call sites where a missing string is not an error and an empty
  // (or caller-chosen) value carries the same meaning.
  const char* or_else(const char* fallback) const
  {
    return _ptr != nullptr ? _ptr : fallback;
  }
  std::string str() const { return _ptr != nullptr ? std::string(_ptr) : std::string(); }

 private:
  JSContext* _ctx = nullptr;
  const char* _ptr = nullptr;
};

// Converts an optional argument: absent (undefined/null, or past argc)
// yields a holder that is not `present()`, anything else is converted.
inline JsCString jsOptionalCString(JSContext* ctx, int argc, JSValueConst* argv, int index)
{
  if (index >= argc || JS_IsUndefined(argv[index]) || JS_IsNull(argv[index]))
  {
    return JsCString{};
  }
  return JsCString{ctx, argv[index]};
}

}  // namespace flox

// Declares `name` and propagates the pending QuickJS exception when the
// value is not convertible to a string. The exception QuickJS already set
// says what went wrong ("cannot convert symbol to string", or whatever the
// throwing toString threw), so it is passed through rather than replaced.
#define FLOX_JS_CSTRING_OR_THROW(name, ctx, value) \
  ::flox::JsCString name((ctx), (value));          \
  if (!(name).ok())                                \
  {                                                \
    return JS_EXCEPTION;                           \
  }                                                \
  do                                               \
  {                                                \
  } while (0)

// Same, for a loop body or any scope that must clean up before leaving.
#define FLOX_JS_CSTRING_OR(name, ctx, value, action) \
  ::flox::JsCString name((ctx), (value));            \
  if (!(name).ok())                                  \
  {                                                  \
    action;                                          \
  }                                                  \
  do                                                 \
  {                                                  \
  } while (0)

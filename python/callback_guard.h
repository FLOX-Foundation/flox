/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// python/callback_guard.h
//
// One exception policy for every Python callable the engine calls back
// into: the strategy bridge callbacks, the extension hooks and the three
// pre-trade gates. It is written out once in docs/bindings/python.md and
// implemented here; no bridge is allowed its own rules.
//
//   1. The exception stops at the bridge. Unwinding out of a frame with
//      C linkage is undefined behaviour, and on the live path it also
//      kills the bus consumer that was delivering the event, so the
//      engine would lose every later event for that strategy.
//   2. A pre-trade gate that did not answer denies. RiskManager.allow,
//      KillSwitch.check and OrderValidator.validate return kGateDenied
//      when the Python callable raises or returns something that is not
//      a bool -- flox_capi.h:47, "a call that was stopped this way
//      returns its failure value", and a gate's failure value is the one
//      that drops the signal (flox_capi_spec.hpp:2306).
//   3. The description survives. The text of the exception goes to the
//      engine log at error level, which is the same sink
//      flox_py.set_log_callback redirects into Python; with no callback
//      installed the bundled ConsoleLogger writes it to stderr. This is
//      the "leaves a description behind" half of flox_capi.h:47 for a
//      binding that exposes no flox_last_error_* accessor.
//
// Everything here runs with the GIL held -- the caller acquires it
// before invoking the Python callable and still holds it while the error
// is reported.

#pragma once

#include <pybind11/pybind11.h>

#include <cstdint>
#include <exception>
#include <source_location>
#include <string>

#include "flox/log/log.h"
#include "flox/log/log_stream.h"

namespace py = pybind11;

namespace flox_py
{

// What a pre-trade gate answers. Deny is the safe side of the question,
// so it is also the answer when the Python callable never produced one.
inline constexpr uint8_t kGateDenied = 0;
inline constexpr uint8_t kGateAllowed = 1;

namespace detail
{

// Set while an error report is being delivered on this thread. The log
// sink is a Python callable too (loggerBridge), so a sink that raises
// would otherwise report itself forever.
inline thread_local bool g_reportingCallbackError = false;

// Set when the report in flight could not be delivered because the log
// sink itself raised. The outer report reads it and falls back to
// stderr, so a broken sink costs the message its destination, not its
// existence.
inline thread_local bool g_logSinkRaised = false;

struct ReportingScope
{
  ReportingScope() { g_reportingCallbackError = true; }
  ~ReportingScope() { g_reportingCallbackError = false; }
};

inline void reportCallbackError(const char* source, const std::string& what)
{
  const std::string text =
      std::string("flox: ") + (source ? source : "a Python callback") + " raised: " + what;

  if (g_reportingCallbackError)
  {
    // The log sink raised while carrying an earlier report. Stderr is
    // the only place left, for this message and for the one underneath.
    g_logSinkRaised = true;
    PySys_WriteStderr("%s\n", text.c_str());
    return;
  }

  ReportingScope scope;
  bool delivered = false;
  try
  {
    if (flox::isLoggingEnabled())
    {
      g_logSinkRaised = false;
      flox::LogStream(flox::LogLevel::Error) << text;
      delivered = !g_logSinkRaised;
    }
  }
  catch (...)
  {
    delivered = false;
  }
  if (!delivered)
  {
    // Losing the description is not an option.
    PySys_WriteStderr("%s\n", text.c_str());
  }
}

}  // namespace detail

// Run a Python callable so that nothing unwinds past this frame into C.
// Returns true when it completed. `source` names the callback in the
// report; left empty it is taken from the enclosing function. The GIL
// must be held.
template <typename Fn>
inline bool guardCallback(Fn&& fn, const char* source = nullptr,
                          const std::source_location loc = std::source_location::current())
{
  const char* label = source ? source : loc.function_name();
  try
  {
    fn();
    return true;
  }
  catch (py::error_already_set& e)
  {
    // pybind11 has already taken the error off the thread state and put
    // it in `e`, which is why the PyErr_Print() this replaced printed
    // nothing at all. what() formats type, message and traceback.
    detail::reportCallbackError(label, e.what());
  }
  catch (const std::exception& e)
  {
    detail::reportCallbackError(label, e.what());
  }
  catch (...)
  {
    detail::reportCallbackError(label, "unknown C++ exception");
  }
  return false;
}

}  // namespace flox_py

/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Tests for the C-API logger callback (T017).
//
// `flox_set_log_callback(cb, ud)` redirects FLOX_LOG_INFO/WARN/ERROR to
// the user-supplied function. NULL restores the default ConsoleLogger.

#include "flox/capi/flox_capi.h"
#include "flox/log/log.h"

#include <gtest/gtest.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace
{

struct Captured
{
  int32_t level;
  std::string msg;
};

struct State
{
  std::mutex mu;
  std::vector<Captured> entries;
  std::atomic<int> calls{0};
};

void capture_cb(void* ud, int32_t level, const char* msg)
{
  auto* s = static_cast<State*>(ud);
  s->calls.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard<std::mutex> lk(s->mu);
  s->entries.push_back({level, std::string(msg ? msg : "")});
}

}  // namespace

TEST(CapiLogger, CallbackReceivesInfoMessage)
{
  State state;
  flox_set_log_callback(capture_cb, &state);

  FLOX_LOG_INFO("hello info");

  ASSERT_EQ(state.calls.load(), 1);
  std::lock_guard<std::mutex> lk(state.mu);
  ASSERT_EQ(state.entries.size(), 1u);
  EXPECT_EQ(state.entries[0].level, 0);
  EXPECT_EQ(state.entries[0].msg, "hello info");

  flox_set_log_callback(nullptr, nullptr);  // cleanup
}

TEST(CapiLogger, LevelMapping)
{
  State state;
  flox_set_log_callback(capture_cb, &state);

  FLOX_LOG_INFO("i");
  FLOX_LOG_WARN("w");
  FLOX_LOG_ERROR("e");

  std::lock_guard<std::mutex> lk(state.mu);
  ASSERT_EQ(state.entries.size(), 3u);
  EXPECT_EQ(state.entries[0].level, 0);
  EXPECT_EQ(state.entries[0].msg, "i");
  EXPECT_EQ(state.entries[1].level, 1);
  EXPECT_EQ(state.entries[1].msg, "w");
  EXPECT_EQ(state.entries[2].level, 2);
  EXPECT_EQ(state.entries[2].msg, "e");

  flox_set_log_callback(nullptr, nullptr);
}

TEST(CapiLogger, NullCallbackRestoresDefault)
{
  State state;
  flox_set_log_callback(capture_cb, &state);
  FLOX_LOG_INFO("captured");
  ASSERT_EQ(state.calls.load(), 1);

  // Detach. Subsequent logs go to the default ConsoleLogger and don't
  // touch the state.
  flox_set_log_callback(nullptr, nullptr);
  FLOX_LOG_INFO("not captured");

  EXPECT_EQ(state.calls.load(), 1) << "callback was detached; should not fire";
}

TEST(CapiLogger, ReplacingCallbackRoutesToNewTarget)
{
  State a, b;

  flox_set_log_callback(capture_cb, &a);
  FLOX_LOG_INFO("to a");

  flox_set_log_callback(capture_cb, &b);
  FLOX_LOG_INFO("to b");

  EXPECT_EQ(a.calls.load(), 1);
  EXPECT_EQ(b.calls.load(), 1);

  flox_set_log_callback(nullptr, nullptr);
}

TEST(CapiLogger, MessageContentSurvivesStreamFormatting)
{
  // The macros build the message via std::ostringstream <<; verify the
  // composed text reaches the callback intact.
  State state;
  flox_set_log_callback(capture_cb, &state);

  int n = 42;
  double pi = 3.14;
  FLOX_LOG_WARN("n=" << n << " pi=" << pi);

  std::lock_guard<std::mutex> lk(state.mu);
  ASSERT_EQ(state.entries.size(), 1u);
  EXPECT_NE(state.entries[0].msg.find("n=42"), std::string::npos);
  EXPECT_NE(state.entries[0].msg.find("pi=3.14"), std::string::npos);

  flox_set_log_callback(nullptr, nullptr);
}

TEST(CapiLogger, LoggingDisabledMacroSuppressesCallback)
{
  // FLOX_LOG_OFF / ON are part of the public log.h API; verify the
  // callback path respects them.
  State state;
  flox_set_log_callback(capture_cb, &state);

  FLOX_LOG_OFF();
  FLOX_LOG_INFO("suppressed");
  EXPECT_EQ(state.calls.load(), 0);

  FLOX_LOG_ON();
  FLOX_LOG_INFO("delivered");
  EXPECT_EQ(state.calls.load(), 1);

  flox_set_log_callback(nullptr, nullptr);
}

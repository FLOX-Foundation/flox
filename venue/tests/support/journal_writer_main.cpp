/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * A shard that writes a journal and then dies without cleaning up.
 *
 * This exists because Windows has no fork. The POSIX test could make a child
 * that was an exact copy of itself and kill it mid-stream; CreateProcess
 * starts a fresh program instead, so the same scenario needs a second binary
 * and an agreed point to die at. This is that binary.
 *
 * It is used on every platform, not only Windows. A Windows-only mechanism
 * would be exercised only where nobody runs it by hand, which is how a test
 * path rots -- and this one is the harder half of the scenario.
 *
 * Dying means dying: no stop(), no destructors, no flush of whatever the
 * consumer was in the middle of. That is the whole point -- the journal has
 * to survive being abandoned, not being closed.
 */
#include "flox-venue/matching_book.h"
#include "flox-venue/sequenced_shard.h"
#include "support/recovery_scenario.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

using namespace flox;
using namespace flox::venue;

namespace
{
std::string g_cleanExitMarker;

// Written only if this process leaves main normally. _exit and
// TerminateProcess skip atexit handlers, so the marker's ABSENCE is what
// proves the death was a death -- without it the test passes just as happily
// against a helper that shuts down cleanly, and then it is only testing
// recovery from a tidy journal, which eight other tests already cover.
void writeCleanExitMarker()
{
  if (g_cleanExitMarker.empty())
  {
    return;
  }
  std::FILE* f = std::fopen(g_cleanExitMarker.c_str(), "wb");
  if (f != nullptr)
  {
    std::fputs("clean", f);
    std::fclose(f);
  }
}

// Abandon the process where it stands. _exit skips destructors and atexit on
// POSIX; TerminateProcess is the same promise on Windows, and neither gives
// the journal a chance to be tidied on the way out.
[[noreturn]] void dieHard()
{
#if defined(_WIN32)
  ::TerminateProcess(::GetCurrentProcess(), 0);
  ::ExitProcess(0);  // not reached; keeps the compiler content
#else
  ::_exit(0);
#endif
}
}  // namespace

int main(int argc, char** argv)
{
  if (argc < 2)
  {
    std::fprintf(stderr, "usage: %s JOURNAL_PATH\n", argv[0]);
    return 2;
  }
  const std::string path = argv[1];
  g_cleanExitMarker = path + ".clean";
  std::atexit(&writeCleanExitMarker);

  const auto cmds = test::scenarioCommands();
  const size_t half = cmds.size() / 2;

  Ledger led;
  auto shard = std::make_unique<SequencedShard<>>(test::scenarioConfig(), path);
  shard->engine().setLedger(&led, test::kScenarioVenueAccount);
  shard->start();

  for (size_t i = 0; i < half; ++i)
  {
    shard->submit(cmds[i]);
  }
  shard->flush();  // the first half is guaranteed journaled before the death

  for (size_t i = half; i < cmds.size(); ++i)
  {
    shard->submit(cmds[i]);
  }

  dieHard();
}

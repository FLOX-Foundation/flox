/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// The library exported 729 symbols and not one of them said which ABI they
// belonged to, and there was no way to ask why a call had failed. A consumer
// loading the shared library through dlopen or ctypes -- the way the C API
// page suggests -- had neither.

#include "lrvx/capi/lrvx_capi.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <thread>

TEST(CapiDiagnosticsTest, AbiVersionMatchesTheHeaderItWasBuiltAgainst)
{
  EXPECT_EQ(lrvx_capi_abi_version(), LRVX_CAPI_ABI_VERSION);
  EXPECT_GT(lrvx_capi_abi_version(), 0u);
}

TEST(CapiDiagnosticsTest, ANullHandleIsReported)
{
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_last_error_code(), 0);

  EXPECT_EQ(lrvx_registry_symbol_count(nullptr), 0u);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("lrvx_registry_symbol_count"),
            std::string::npos);

  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_last_error_code(), 0);
  EXPECT_STREQ(lrvx_last_error_message(), "");
}

TEST(CapiDiagnosticsTest, ANullArgumentIsReported)
{
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_data_reader_create(nullptr), nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 1);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("data_dir"), std::string::npos);
  lrvx_clear_last_error();
}

TEST(CapiDiagnosticsTest, AStoppedExceptionIsReported)
{
  lrvx_clear_last_error();

  // Asking for a writer under a path that cannot be a directory makes
  // create_directories throw; the boundary catches it.
  LrvxDataWriterHandle w = lrvx_data_writer_create("/dev/null/not/a/directory", 0, 0);
  EXPECT_EQ(w, nullptr);
  EXPECT_EQ(lrvx_last_error_code(), 2);
  EXPECT_NE(std::string(lrvx_last_error_message()).find("lrvx_data_writer_create"),
            std::string::npos);

  lrvx_clear_last_error();
}

TEST(CapiDiagnosticsTest, TheErrorSlotIsPerThread)
{
  lrvx_clear_last_error();
  EXPECT_EQ(lrvx_registry_symbol_count(nullptr), 0u);
  ASSERT_EQ(lrvx_last_error_code(), 1);

  int fromWorker = -1;
  std::thread worker(
      [&]
      {
        fromWorker = lrvx_last_error_code();
        lrvx_clear_last_error();
      });
  worker.join();

  EXPECT_EQ(fromWorker, 0) << "one thread's failure must not show up on another";
  EXPECT_EQ(lrvx_last_error_code(), 1) << "and the worker must not clear it either";

  lrvx_clear_last_error();
}

/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

#include "flox/replay/binary_format_v1.h"

#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

#if FLOX_LZ4_ENABLED
#include <lz4.h>
#endif

namespace flox::replay
{

inline constexpr bool isCompressionAvailable()
{
#if FLOX_LZ4_ENABLED
  return true;
#else
  return false;
#endif
}

inline constexpr bool isCompressionAvailable(CompressionType type)
{
  switch (type)
  {
    case CompressionType::None:
      return true;
    case CompressionType::LZ4:
#if FLOX_LZ4_ENABLED
      return true;
#else
      return false;
#endif
  }
  return false;
}

class Compressor
{
 public:
  static size_t compress(CompressionType type, const void* src, size_t src_size, void* dst,
                         size_t dst_capacity)
  {
    switch (type)
    {
      case CompressionType::None:
        if (dst_capacity < src_size)
        {
          return 0;
        }
        std::memcpy(dst, src, src_size);
        return src_size;

      case CompressionType::LZ4:
#if FLOX_LZ4_ENABLED
        return static_cast<size_t>(
            LZ4_compress_default(static_cast<const char*>(src), static_cast<char*>(dst),
                                 static_cast<int>(src_size), static_cast<int>(dst_capacity)));
#else
        return 0;
#endif
    }
    return 0;
  }

  static size_t decompress(CompressionType type, const void* src, size_t src_size, void* dst,
                           size_t original_size)
  {
    switch (type)
    {
      case CompressionType::None:
        if (src_size != original_size)
        {
          return 0;
        }
        std::memcpy(dst, src, src_size);
        return src_size;

      case CompressionType::LZ4:
#if FLOX_LZ4_ENABLED
      {
        int result =
            LZ4_decompress_safe(static_cast<const char*>(src), static_cast<char*>(dst),
                                static_cast<int>(src_size), static_cast<int>(original_size));
        return result > 0 ? static_cast<size_t>(result) : 0;
      }
#else
        return 0;
#endif
    }
    return 0;
  }

  static size_t maxCompressedSize(CompressionType type, size_t src_size)
  {
    switch (type)
    {
      case CompressionType::None:
        return src_size;

      case CompressionType::LZ4:
#if FLOX_LZ4_ENABLED
        return static_cast<size_t>(LZ4_compressBound(static_cast<int>(src_size)));
#else
        return 0;
#endif
    }
    return 0;
  }

  static std::vector<std::byte> compress(CompressionType type, std::span<const std::byte> src)
  {
    if (src.empty())
    {
      return {};
    }

    size_t max_size = maxCompressedSize(type, src.size());
    if (max_size == 0)
    {
      return {};
    }

    std::vector<std::byte> result(max_size);
    size_t compressed_size = compress(type, src.data(), src.size(), result.data(), max_size);

    if (compressed_size == 0)
    {
      return {};
    }

    result.resize(compressed_size);
    return result;
  }

  static std::vector<std::byte> decompress(CompressionType type, std::span<const std::byte> src,
                                           size_t original_size)
  {
    if (src.empty() || original_size == 0)
    {
      return {};
    }

    std::vector<std::byte> result(original_size);
    size_t decompressed_size =
        decompress(type, src.data(), src.size(), result.data(), original_size);

    if (decompressed_size != original_size)
    {
      return {};
    }

    return result;
  }
};

}  // namespace flox::replay

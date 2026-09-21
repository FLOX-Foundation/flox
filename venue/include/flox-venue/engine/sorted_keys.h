/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include <algorithm>
#include <vector>

namespace flox::venue
{

// Keys of an associative container, ascending. The canonical traversal order
// for every hash and every snapshot section: a std::unordered_map enumerates
// in hash-bucket order, which is a property of the standard library and the
// insertion history rather than of the state, so anything observable has to
// walk the keys sorted instead. Shared by the engine and by the conduct
// components so there is one spelling of "canonical" rather than six.
template <class Map>
std::vector<typename Map::key_type> sortedKeysOf(const Map& m)
{
  std::vector<typename Map::key_type> keys;
  keys.reserve(m.size());
  // order: sorted below, before the vector is handed out
  for (const auto& [k, v] : m)
  {
    (void)v;
    keys.push_back(k);
  }
  std::sort(keys.begin(), keys.end());
  return keys;
}

}  // namespace flox::venue

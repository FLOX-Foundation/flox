/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/replay/recording_metadata.h"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace flox::replay
{

namespace
{

std::string escapeJson(const std::string& s)
{
  std::string result;
  result.reserve(s.size() + 10);
  for (char c : s)
  {
    switch (c)
    {
      case '"':
        result += "\\\"";
        break;
      case '\\':
        result += "\\\\";
        break;
      case '\n':
        result += "\\n";
        break;
      case '\r':
        result += "\\r";
        break;
      case '\t':
        result += "\\t";
        break;
      default:
        result += c;
    }
  }
  return result;
}

std::string trim(const std::string& s)
{
  size_t start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos)
  {
    return "";
  }
  size_t end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

std::string unescapeJson(const std::string& s)
{
  std::string result;
  result.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i)
  {
    if (s[i] == '\\' && i + 1 < s.size())
    {
      switch (s[i + 1])
      {
        case '"':
          result += '"';
          ++i;
          break;
        case '\\':
          result += '\\';
          ++i;
          break;
        case 'n':
          result += '\n';
          ++i;
          break;
        case 'r':
          result += '\r';
          ++i;
          break;
        case 't':
          result += '\t';
          ++i;
          break;
        default:
          result += s[i];
      }
    }
    else
    {
      result += s[i];
    }
  }
  return result;
}

// Simple JSON string value extraction (between quotes after colon)
std::string extractStringValue(const std::string& line, const std::string& key)
{
  auto keyPos = line.find("\"" + key + "\"");
  if (keyPos == std::string::npos)
  {
    return "";
  }

  auto colonPos = line.find(':', keyPos);
  if (colonPos == std::string::npos)
  {
    return "";
  }

  auto firstQuote = line.find('"', colonPos + 1);
  if (firstQuote == std::string::npos)
  {
    return "";
  }

  auto secondQuote = line.find('"', firstQuote + 1);
  while (secondQuote != std::string::npos && line[secondQuote - 1] == '\\')
  {
    secondQuote = line.find('"', secondQuote + 1);
  }

  if (secondQuote == std::string::npos)
  {
    return "";
  }

  return unescapeJson(line.substr(firstQuote + 1, secondQuote - firstQuote - 1));
}

int64_t extractIntValue(const std::string& line, const std::string& key)
{
  auto keyPos = line.find("\"" + key + "\"");
  if (keyPos == std::string::npos)
  {
    return 0;
  }

  auto colonPos = line.find(':', keyPos);
  if (colonPos == std::string::npos)
  {
    return 0;
  }

  std::string numStr;
  for (size_t i = colonPos + 1; i < line.size(); ++i)
  {
    char c = line[i];
    if (c == '-' || (c >= '0' && c <= '9'))
    {
      numStr += c;
    }
    else if (!numStr.empty())
    {
      break;
    }
  }

  if (numStr.empty())
  {
    return 0;
  }

  try
  {
    return std::stoll(numStr);
  }
  catch (...)
  {
    return 0;
  }
}

bool extractBoolValue(const std::string& line, const std::string& key)
{
  auto keyPos = line.find("\"" + key + "\"");
  if (keyPos == std::string::npos)
  {
    return false;
  }

  auto colonPos = line.find(':', keyPos);
  if (colonPos == std::string::npos)
  {
    return false;
  }

  auto rest = line.substr(colonPos + 1);
  rest = trim(rest);
  return rest.find("true") == 0;
}

}  // namespace

bool RecordingMetadata::save(const std::filesystem::path& path) const
{
  std::ofstream file(path);
  if (!file)
  {
    return false;
  }

  file << "{\n";
  file << "  \"recording_id\": \"" << escapeJson(recording_id) << "\",\n";
  file << "  \"description\": \"" << escapeJson(description) << "\",\n";
  file << "\n";
  file << "  \"exchange\": \"" << escapeJson(exchange) << "\",\n";
  file << "  \"exchange_type\": \"" << escapeJson(exchange_type) << "\",\n";
  file << "  \"instrument_type\": \"" << escapeJson(instrument_type) << "\",\n";
  file << "  \"connector_version\": \"" << escapeJson(connector_version) << "\",\n";
  file << "\n";

  // Symbols array
  file << "  \"symbols\": [\n";
  for (size_t i = 0; i < symbols.size(); ++i)
  {
    const auto& sym = symbols[i];
    file << "    {\n";
    file << "      \"symbol_id\": " << sym.symbol_id << ",\n";
    file << "      \"name\": \"" << escapeJson(sym.name) << "\",\n";
    file << "      \"base_asset\": \"" << escapeJson(sym.base_asset) << "\",\n";
    file << "      \"quote_asset\": \"" << escapeJson(sym.quote_asset) << "\",\n";
    file << "      \"price_precision\": " << static_cast<int>(sym.price_precision) << ",\n";
    file << "      \"qty_precision\": " << static_cast<int>(sym.qty_precision) << "\n";
    file << "    }";
    if (i + 1 < symbols.size())
    {
      file << ",";
    }
    file << "\n";
  }
  file << "  ],\n";
  file << "\n";

  file << "  \"has_trades\": " << (has_trades ? "true" : "false") << ",\n";
  file << "  \"has_book_snapshots\": " << (has_book_snapshots ? "true" : "false") << ",\n";
  file << "  \"has_book_deltas\": " << (has_book_deltas ? "true" : "false") << ",\n";
  file << "  \"book_depth\": " << book_depth << ",\n";
  file << "\n";

  file << "  \"recording_start\": \"" << escapeJson(recording_start) << "\",\n";
  file << "  \"recording_end\": \"" << escapeJson(recording_end) << "\",\n";
  file << "\n";

  file << "  \"price_scale\": " << price_scale << ",\n";
  file << "  \"qty_scale\": " << qty_scale << ",\n";
  file << "\n";

  file << "  \"hostname\": \"" << escapeJson(hostname) << "\",\n";
  file << "  \"timezone\": \"" << escapeJson(timezone) << "\",\n";
  file << "  \"flox_version\": \"" << escapeJson(flox_version) << "\",\n";
  file << "\n";

  // Custom fields
  file << "  \"custom\": {\n";
  size_t idx = 0;
  for (const auto& [key, value] : custom)
  {
    file << "    \"" << escapeJson(key) << "\": \"" << escapeJson(value) << "\"";
    if (++idx < custom.size())
    {
      file << ",";
    }
    file << "\n";
  }
  file << "  }\n";

  file << "}\n";

  return file.good();
}

std::optional<RecordingMetadata> RecordingMetadata::load(const std::filesystem::path& path)
{
  std::ifstream file(path);
  if (!file)
  {
    return std::nullopt;
  }

  // Read entire file
  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string content = buffer.str();

  RecordingMetadata meta;

  // Simple line-by-line parsing (not a full JSON parser)
  meta.recording_id = extractStringValue(content, "recording_id");
  meta.description = extractStringValue(content, "description");
  meta.exchange = extractStringValue(content, "exchange");
  meta.exchange_type = extractStringValue(content, "exchange_type");
  meta.instrument_type = extractStringValue(content, "instrument_type");
  meta.connector_version = extractStringValue(content, "connector_version");

  meta.has_trades = extractBoolValue(content, "has_trades");
  meta.has_book_snapshots = extractBoolValue(content, "has_book_snapshots");
  meta.has_book_deltas = extractBoolValue(content, "has_book_deltas");
  meta.book_depth = static_cast<uint16_t>(extractIntValue(content, "book_depth"));

  meta.recording_start = extractStringValue(content, "recording_start");
  meta.recording_end = extractStringValue(content, "recording_end");

  meta.price_scale = extractIntValue(content, "price_scale");
  meta.qty_scale = extractIntValue(content, "qty_scale");

  if (meta.price_scale == 0)
  {
    meta.price_scale = 100000000;
  }
  if (meta.qty_scale == 0)
  {
    meta.qty_scale = 100000000;
  }

  meta.hostname = extractStringValue(content, "hostname");
  meta.timezone = extractStringValue(content, "timezone");
  meta.flox_version = extractStringValue(content, "flox_version");

  // Note: symbols array and custom map parsing would need more complex logic
  // This is a simplified implementation

  return meta;
}

}  // namespace flox::replay

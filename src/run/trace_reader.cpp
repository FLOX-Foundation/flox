/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/run/trace_reader.h"

#include "flox/replay/binary_format_v1.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace flox::run
{

namespace
{

// Minimal JSON pull parser. Just enough to read the manifest produced by the
// recorder. We avoid pulling a JSON library for the engine target so the run
// directory can be parsed in environments where one is not available.
class MiniJson
{
 public:
  explicit MiniJson(std::string_view src) : _src(src), _pos(0) {}

  void skipWs()
  {
    while (_pos < _src.size() && (_src[_pos] == ' ' || _src[_pos] == '\t' ||
                                  _src[_pos] == '\n' || _src[_pos] == '\r' ||
                                  _src[_pos] == ','))
    {
      ++_pos;
    }
  }

  bool match(char c)
  {
    skipWs();
    if (_pos < _src.size() && _src[_pos] == c)
    {
      ++_pos;
      return true;
    }
    return false;
  }

  std::string parseString()
  {
    skipWs();
    if (_pos >= _src.size() || _src[_pos] != '"')
    {
      throw std::runtime_error("manifest parse: expected string");
    }
    ++_pos;
    std::string out;
    while (_pos < _src.size() && _src[_pos] != '"')
    {
      if (_src[_pos] == '\\' && _pos + 1 < _src.size())
      {
        char nxt = _src[_pos + 1];
        switch (nxt)
        {
          case '"':
            out += '"';
            break;
          case '\\':
            out += '\\';
            break;
          case 'n':
            out += '\n';
            break;
          case 'r':
            out += '\r';
            break;
          case 't':
            out += '\t';
            break;
          case 'u':
          {
            if (_pos + 5 >= _src.size())
            {
              throw std::runtime_error("manifest parse: truncated \\u escape");
            }
            std::string hex(_src.substr(_pos + 2, 4));
            unsigned code = std::stoul(hex, nullptr, 16);
            if (code < 0x80)
            {
              out += static_cast<char>(code);
            }
            else
            {
              // Manifest does not use non-ASCII; degrade safely.
              out += '?';
            }
            _pos += 4;
            break;
          }
          default:
            out += nxt;
            break;
        }
        _pos += 2;
        continue;
      }
      out += _src[_pos];
      ++_pos;
    }
    if (_pos >= _src.size())
    {
      throw std::runtime_error("manifest parse: unterminated string");
    }
    ++_pos;
    return out;
  }

  long long parseNumber()
  {
    skipWs();
    size_t start = _pos;
    if (_pos < _src.size() && (_src[_pos] == '-' || _src[_pos] == '+'))
    {
      ++_pos;
    }
    while (_pos < _src.size() && (std::isdigit(static_cast<unsigned char>(_src[_pos])) ||
                                  _src[_pos] == '.'))
    {
      ++_pos;
    }
    return std::stoll(std::string(_src.substr(start, _pos - start)));
  }

  void skipValue()
  {
    skipWs();
    if (_pos >= _src.size())
    {
      return;
    }
    char c = _src[_pos];
    if (c == '"')
    {
      parseString();
    }
    else if (c == '{' || c == '[')
    {
      char open = c;
      char close = (c == '{') ? '}' : ']';
      int depth = 1;
      ++_pos;
      while (_pos < _src.size() && depth > 0)
      {
        if (_src[_pos] == '"')
        {
          parseString();
          continue;
        }
        if (_src[_pos] == open)
        {
          ++depth;
        }
        else if (_src[_pos] == close)
        {
          --depth;
        }
        ++_pos;
      }
    }
    else
    {
      while (_pos < _src.size() && _src[_pos] != ',' && _src[_pos] != '}' &&
             _src[_pos] != ']')
      {
        ++_pos;
      }
    }
  }

 private:
  std::string_view _src;
  size_t _pos;
};

RecordKind kindFromString(const std::string& s)
{
  if (s == "signals")
  {
    return RecordKind::Signal;
  }
  if (s == "orders")
  {
    return RecordKind::OrderEvent;
  }
  if (s == "fills")
  {
    return RecordKind::Fill;
  }
  return RecordKind::Unknown;
}

}  // namespace

TraceReader::TraceReader(const std::string& path) : _root(path)
{
  loadManifest();
}

TraceReader::~TraceReader() = default;

void TraceReader::loadManifest()
{
  std::filesystem::path manifest_path = std::filesystem::path(_root) / "manifest.json";
  std::ifstream in(manifest_path);
  if (!in)
  {
    throw std::runtime_error("trace reader: missing manifest at " + manifest_path.string());
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  std::string src = buf.str();
  MiniJson p(src);
  if (!p.match('{'))
  {
    throw std::runtime_error("trace reader: manifest must be a JSON object");
  }
  while (true)
  {
    p.skipWs();
    if (p.match('}'))
    {
      break;
    }
    std::string key = p.parseString();
    if (!p.match(':'))
    {
      throw std::runtime_error("trace reader: expected ':' after key " + key);
    }
    if (key == "schema_version")
    {
      _manifest.schema_version = static_cast<uint32_t>(p.parseNumber());
    }
    else if (key == "format_version")
    {
      _manifest.format_version = static_cast<uint32_t>(p.parseNumber());
    }
    else if (key == "strategy_id")
    {
      _manifest.strategy_id = p.parseString();
    }
    else if (key == "strategy_hash")
    {
      _manifest.strategy_hash = p.parseString();
    }
    else if (key == "run_started_ns")
    {
      _manifest.run_started_ns = p.parseNumber();
    }
    else if (key == "run_ended_ns")
    {
      _manifest.run_ended_ns = p.parseNumber();
    }
    else if (key == "tape_refs")
    {
      if (!p.match('['))
      {
        throw std::runtime_error("trace reader: tape_refs must be array");
      }
      while (true)
      {
        p.skipWs();
        if (p.match(']'))
        {
          break;
        }
        if (!p.match('{'))
        {
          throw std::runtime_error("trace reader: tape ref must be object");
        }
        TapeRef ref;
        while (true)
        {
          p.skipWs();
          if (p.match('}'))
          {
            break;
          }
          std::string k = p.parseString();
          if (!p.match(':'))
          {
            throw std::runtime_error("trace reader: expected ':' in tape ref");
          }
          if (k == "path")
          {
            ref.path = p.parseString();
          }
          else if (k == "content_hash")
          {
            ref.content_hash = p.parseString();
          }
          else if (k == "first_event_ns")
          {
            ref.first_event_ns = p.parseNumber();
          }
          else if (k == "last_event_ns")
          {
            ref.last_event_ns = p.parseNumber();
          }
          else
          {
            p.skipValue();
          }
        }
        _manifest.tape_refs.push_back(std::move(ref));
      }
    }
    else if (key == "segments")
    {
      if (!p.match('['))
      {
        throw std::runtime_error("trace reader: segments must be array");
      }
      while (true)
      {
        p.skipWs();
        if (p.match(']'))
        {
          break;
        }
        if (!p.match('{'))
        {
          throw std::runtime_error("trace reader: segment must be object");
        }
        TraceManifest::Segment seg;
        while (true)
        {
          p.skipWs();
          if (p.match('}'))
          {
            break;
          }
          std::string k = p.parseString();
          if (!p.match(':'))
          {
            throw std::runtime_error("trace reader: expected ':' in segment");
          }
          if (k == "name")
          {
            seg.name = p.parseString();
          }
          else if (k == "record_kind")
          {
            seg.record_kind = kindFromString(p.parseString());
          }
          else if (k == "size_bytes")
          {
            seg.size_bytes = static_cast<uint64_t>(p.parseNumber());
          }
          else if (k == "first_event_ns")
          {
            seg.first_event_ns = p.parseNumber();
          }
          else if (k == "last_event_ns")
          {
            seg.last_event_ns = p.parseNumber();
          }
          else if (k == "event_count")
          {
            seg.event_count = static_cast<uint32_t>(p.parseNumber());
          }
          else
          {
            p.skipValue();
          }
        }
        _manifest.segments.push_back(std::move(seg));
      }
    }
    else
    {
      p.skipValue();
    }
  }
  if (_manifest.format_version != kRunFormatVersion)
  {
    throw std::runtime_error("trace reader: unsupported format version");
  }
}

std::optional<TraceManifest::Segment> TraceReader::findSegment(RecordKind kind) const
{
  for (const auto& seg : _manifest.segments)
  {
    if (seg.record_kind == kind)
    {
      return seg;
    }
  }
  return std::nullopt;
}

std::vector<uint8_t> TraceReader::readSegmentBytes(const std::string& name) const
{
  std::filesystem::path full = std::filesystem::path(_root) / name;
  std::ifstream in(full, std::ios::binary);
  if (!in)
  {
    throw std::runtime_error("trace reader: cannot open segment " + full.string());
  }
  in.seekg(0, std::ios::end);
  size_t sz = static_cast<size_t>(in.tellg());
  in.seekg(0);
  std::vector<uint8_t> buf(sz);
  if (sz > 0)
  {
    in.read(reinterpret_cast<char*>(buf.data()), sz);
  }
  return buf;
}

std::vector<OwnedSignal> TraceReader::readAllSignals()
{
  std::vector<OwnedSignal> out;
  auto seg = findSegment(RecordKind::Signal);
  if (!seg)
  {
    return out;
  }
  auto bytes = readSegmentBytes(seg->name);
  if (bytes.size() < sizeof(RunSegmentHeader))
  {
    throw std::runtime_error("trace reader: signal segment shorter than header");
  }
  RunSegmentHeader hdr{};
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
  if (!hdr.isValid() || hdr.record_kind != static_cast<uint8_t>(RecordKind::Signal))
  {
    throw std::runtime_error("trace reader: signal segment header invalid");
  }
  size_t cursor = sizeof(hdr);
  out.reserve(hdr.event_count);
  for (uint32_t i = 0; i < hdr.event_count; ++i)
  {
    if (cursor + sizeof(flox::replay::FrameHeader) > bytes.size())
    {
      throw std::runtime_error("trace reader: truncated frame header");
    }
    flox::replay::FrameHeader fh{};
    std::memcpy(&fh, bytes.data() + cursor, sizeof(fh));
    cursor += sizeof(fh);
    if (fh.type != static_cast<uint8_t>(FrameType::Signal))
    {
      throw std::runtime_error("trace reader: unexpected frame type in signal segment");
    }
    if (cursor + fh.size > bytes.size())
    {
      throw std::runtime_error("trace reader: truncated signal payload");
    }
    uint32_t crc = flox::replay::Crc32::compute(bytes.data() + cursor, fh.size);
    if (crc != fh.crc32)
    {
      throw std::runtime_error("trace reader: signal frame crc mismatch");
    }
    if (fh.size < sizeof(SignalRecord))
    {
      throw std::runtime_error("trace reader: signal frame smaller than record");
    }
    SignalRecord rec{};
    std::memcpy(&rec, bytes.data() + cursor, sizeof(rec));
    OwnedSignal os;
    os.run_ts_ns = rec.run_ts_ns;
    os.feed_ts_ns = rec.feed_ts_ns;
    os.signal_id = rec.signal_id;
    os.flags = rec.flags;
    os.strength_raw = rec.strength_raw;
    size_t off = cursor + sizeof(rec);
    if (rec.name_len > 0)
    {
      os.name.assign(reinterpret_cast<const char*>(bytes.data() + off), rec.name_len);
      off += rec.name_len;
    }
    os.symbol_ids.resize(rec.symbol_count);
    for (uint16_t s = 0; s < rec.symbol_count; ++s)
    {
      uint32_t sid = 0;
      std::memcpy(&sid, bytes.data() + off, sizeof(sid));
      os.symbol_ids[s] = sid;
      off += sizeof(sid);
    }
    if (rec.payload_len > 0)
    {
      os.payload.assign(bytes.data() + off, bytes.data() + off + rec.payload_len);
    }
    out.push_back(std::move(os));
    cursor += fh.size;
  }
  return out;
}

std::vector<OwnedOrderEvent> TraceReader::readAllOrderEvents()
{
  std::vector<OwnedOrderEvent> out;
  auto seg = findSegment(RecordKind::OrderEvent);
  if (!seg)
  {
    return out;
  }
  auto bytes = readSegmentBytes(seg->name);
  if (bytes.size() < sizeof(RunSegmentHeader))
  {
    throw std::runtime_error("trace reader: order segment shorter than header");
  }
  RunSegmentHeader hdr{};
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
  if (!hdr.isValid() || hdr.record_kind != static_cast<uint8_t>(RecordKind::OrderEvent))
  {
    throw std::runtime_error("trace reader: order segment header invalid");
  }
  size_t cursor = sizeof(hdr);
  out.reserve(hdr.event_count);
  for (uint32_t i = 0; i < hdr.event_count; ++i)
  {
    if (cursor + sizeof(flox::replay::FrameHeader) > bytes.size())
    {
      throw std::runtime_error("trace reader: truncated order frame header");
    }
    flox::replay::FrameHeader fh{};
    std::memcpy(&fh, bytes.data() + cursor, sizeof(fh));
    cursor += sizeof(fh);
    if (fh.type != static_cast<uint8_t>(FrameType::OrderEvent))
    {
      throw std::runtime_error("trace reader: unexpected frame type in order segment");
    }
    if (cursor + fh.size > bytes.size())
    {
      throw std::runtime_error("trace reader: truncated order payload");
    }
    uint32_t crc = flox::replay::Crc32::compute(bytes.data() + cursor, fh.size);
    if (crc != fh.crc32)
    {
      throw std::runtime_error("trace reader: order frame crc mismatch");
    }
    if (fh.size < sizeof(OrderEventRecord))
    {
      throw std::runtime_error("trace reader: order frame smaller than record");
    }
    OrderEventRecord rec{};
    std::memcpy(&rec, bytes.data() + cursor, sizeof(rec));
    OwnedOrderEvent oe;
    oe.run_ts_ns = rec.run_ts_ns;
    oe.feed_ts_ns = rec.feed_ts_ns;
    oe.order_id = rec.order_id;
    oe.parent_signal_id = rec.parent_signal_id;
    oe.price_raw = rec.price_raw;
    oe.qty_raw = rec.qty_raw;
    oe.symbol_id = rec.symbol_id;
    oe.event_kind = static_cast<OrderEventKind>(rec.event_kind);
    oe.side = rec.side;
    oe.order_type = rec.order_type;
    oe.flags = rec.flags;
    if (rec.reason_len > 0)
    {
      oe.reason.assign(reinterpret_cast<const char*>(bytes.data() + cursor + sizeof(rec)),
                       rec.reason_len);
    }
    out.push_back(std::move(oe));
    cursor += fh.size;
  }
  return out;
}

std::vector<FillRecord> TraceReader::readAllFills()
{
  std::vector<FillRecord> out;
  auto seg = findSegment(RecordKind::Fill);
  if (!seg)
  {
    return out;
  }
  auto bytes = readSegmentBytes(seg->name);
  if (bytes.size() < sizeof(RunSegmentHeader))
  {
    throw std::runtime_error("trace reader: fill segment shorter than header");
  }
  RunSegmentHeader hdr{};
  std::memcpy(&hdr, bytes.data(), sizeof(hdr));
  if (!hdr.isValid() || hdr.record_kind != static_cast<uint8_t>(RecordKind::Fill))
  {
    throw std::runtime_error("trace reader: fill segment header invalid");
  }
  size_t cursor = sizeof(hdr);
  out.reserve(hdr.event_count);
  for (uint32_t i = 0; i < hdr.event_count; ++i)
  {
    if (cursor + sizeof(flox::replay::FrameHeader) > bytes.size())
    {
      throw std::runtime_error("trace reader: truncated fill frame header");
    }
    flox::replay::FrameHeader fh{};
    std::memcpy(&fh, bytes.data() + cursor, sizeof(fh));
    cursor += sizeof(fh);
    if (fh.type != static_cast<uint8_t>(FrameType::Fill))
    {
      throw std::runtime_error("trace reader: unexpected frame type in fill segment");
    }
    if (cursor + fh.size > bytes.size())
    {
      throw std::runtime_error("trace reader: truncated fill payload");
    }
    uint32_t crc = flox::replay::Crc32::compute(bytes.data() + cursor, fh.size);
    if (crc != fh.crc32)
    {
      throw std::runtime_error("trace reader: fill frame crc mismatch");
    }
    if (fh.size < sizeof(FillRecord))
    {
      throw std::runtime_error("trace reader: fill frame too small");
    }
    FillRecord rec{};
    std::memcpy(&rec, bytes.data() + cursor, sizeof(rec));
    out.push_back(rec);
    cursor += fh.size;
  }
  return out;
}

}  // namespace flox::run

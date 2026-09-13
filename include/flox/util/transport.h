/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */
#pragma once

#include <unistd.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace flox::net
{

// Upper bound on a single length-prefixed frame. Venue messages are tiny; this
// only has to be generous enough for a batched snapshot while bounding the
// allocation a 4-byte length prefix can trigger.
inline constexpr uint32_t kMaxFrame = 16u << 20;  // 16 MiB

inline bool writeAll(int fd, const uint8_t* p, size_t n)
{
  size_t off = 0;
  while (off < n)
  {
    const ssize_t w = ::write(fd, p + off, n - off);
    if (w <= 0)
    {
      return false;
    }
    off += static_cast<size_t>(w);
  }
  return true;
}

inline bool readAll(int fd, uint8_t* p, size_t n)
{
  size_t off = 0;
  while (off < n)
  {
    const ssize_t r = ::read(fd, p + off, n - off);
    if (r <= 0)
    {
      return false;  // EOF or error
    }
    off += static_cast<size_t>(r);
  }
  return true;
}

// Framing: [u32 big-endian length][payload].
inline bool writeFrame(int fd, const uint8_t* p, size_t n)
{
  const uint8_t hdr[4] = {static_cast<uint8_t>(n >> 24), static_cast<uint8_t>(n >> 16),
                          static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n)};
  return writeAll(fd, hdr, 4) && writeAll(fd, p, n);
}

inline bool readFrame(int fd, std::vector<uint8_t>& out)
{
  uint8_t hdr[4];
  if (!readAll(fd, hdr, 4))
  {
    return false;
  }
  const uint32_t len = (static_cast<uint32_t>(hdr[0]) << 24) | (static_cast<uint32_t>(hdr[1]) << 16) |
                       (static_cast<uint32_t>(hdr[2]) << 8) | static_cast<uint32_t>(hdr[3]);
  // Reject a hostile length prefix before allocating: a 4-byte header must not be
  // able to make us reserve up to 4 GiB (per-connection amplification DoS / OOM).
  if (len > kMaxFrame)
  {
    return false;
  }
  out.resize(len);
  return len == 0 || readAll(fd, out.data(), len);
}

// Resumable framed reader.
//
// readFrame above is all-or-nothing: a short read throws away the bytes it has
// already taken out of the kernel buffer. On a socket with a receive timeout --
// which every gateway sets, so its session timers and shutdown can run -- a
// frame that straddles the timeout leaves the stream offset by exactly those
// bytes. The next length prefix is then read from the middle of a message, and
// the decoder is handed frames the peer never sent. Ordinary TCP fragmentation
// with a pause longer than the tick is enough to cause it.
//
// FrameReader keeps the partial header and body across calls, so a timeout is
// resumable: the caller services its timers and comes back to the same frame.
// The TLS and WebSocket gateways keep equivalent state of their own; this is
// that state for the plain framed transport.
class FrameReader
{
 public:
  enum class Status : uint8_t
  {
    Frame,       // `out` holds one complete payload
    Incomplete,  // read timed out; whatever arrived is kept for the next call
    Closed,      // peer closed, a read error, or a length prefix past kMaxFrame
  };

  Status read(int fd, std::vector<uint8_t>& out)
  {
    lastBytes_ = 0;
    if (!haveLen_)
    {
      const Status s = fill(fd, hdr_, 4, hdrOff_);
      if (s != Status::Frame)
      {
        return s;
      }
      const uint32_t len = (static_cast<uint32_t>(hdr_[0]) << 24) |
                           (static_cast<uint32_t>(hdr_[1]) << 16) |
                           (static_cast<uint32_t>(hdr_[2]) << 8) | static_cast<uint32_t>(hdr_[3]);
      if (len > kMaxFrame)
      {
        return Status::Closed;  // hostile length prefix, rejected before allocating
      }
      body_.resize(len);
      bodyOff_ = 0;
      haveLen_ = true;
    }
    if (!body_.empty())
    {
      const Status s = fill(fd, body_.data(), body_.size(), bodyOff_);
      if (s != Status::Frame)
      {
        return s;
      }
    }
    out.swap(body_);  // swap rather than copy: both buffers keep their capacity
    body_.clear();
    hdrOff_ = 0;
    bodyOff_ = 0;
    haveLen_ = false;
    return Status::Frame;
  }

  // Bytes taken from the socket during the last read call. Zero after a timeout
  // means the peer sent nothing at all in that window, which is what an idle
  // timeout is about; non-zero means it is mid-frame and alive.
  size_t bytesRead() const noexcept { return lastBytes_; }

  // True while a frame is partly read.
  bool inProgress() const noexcept { return haveLen_ || hdrOff_ != 0; }

 private:
  Status fill(int fd, uint8_t* p, size_t n, size_t& off)
  {
    while (off < n)
    {
      errno = 0;
      const ssize_t r = ::read(fd, p + off, n - off);
      if (r > 0)
      {
        off += static_cast<size_t>(r);
        lastBytes_ += static_cast<size_t>(r);
        continue;
      }
      if (r == 0)
      {
        return Status::Closed;  // EOF
      }
      if (errno == EINTR)
      {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        return Status::Incomplete;
      }
      return Status::Closed;
    }
    return Status::Frame;
  }

  uint8_t hdr_[4]{};
  size_t hdrOff_{0};
  std::vector<uint8_t> body_;
  size_t bodyOff_{0};
  size_t lastBytes_{0};
  bool haveLen_{false};
};

// Kernel-bypass backend. The POSIX helpers above are the reference. On Linux
// with FME_IO_URING the gateway drives io_uring (submission/completion rings,
// no per-syscall overhead); DPDK / OpenOnload plug in the same way in colo.
// Built only on Linux with liburing (link -luring); the portable build stays
// dependency-free. Untested off Linux -- this is the real integration code.
#if defined(__linux__) && defined(FME_IO_URING)
#include <liburing.h>

class IoUring
{
 public:
  explicit IoUring(unsigned entries = 256) { io_uring_queue_init(entries, &ring_, 0); }
  ~IoUring() { io_uring_queue_exit(&ring_); }

  bool readAll(int fd, uint8_t* p, size_t n)
  {
    size_t off = 0;
    while (off < n)
    {
      io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
      io_uring_prep_read(sqe, fd, p + off, static_cast<unsigned>(n - off), 0);
      io_uring_submit(&ring_);
      io_uring_cqe* cqe = nullptr;
      if (io_uring_wait_cqe(&ring_, &cqe) < 0 || cqe->res <= 0)
      {
        if (cqe)
        {
          io_uring_cqe_seen(&ring_, cqe);
        }
        return false;
      }
      off += static_cast<size_t>(cqe->res);
      io_uring_cqe_seen(&ring_, cqe);
    }
    return true;
  }

  bool writeAll(int fd, const uint8_t* p, size_t n)
  {
    size_t off = 0;
    while (off < n)
    {
      io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
      io_uring_prep_write(sqe, fd, p + off, static_cast<unsigned>(n - off), 0);
      io_uring_submit(&ring_);
      io_uring_cqe* cqe = nullptr;
      if (io_uring_wait_cqe(&ring_, &cqe) < 0 || cqe->res <= 0)
      {
        if (cqe)
        {
          io_uring_cqe_seen(&ring_, cqe);
        }
        return false;
      }
      off += static_cast<size_t>(cqe->res);
      io_uring_cqe_seen(&ring_, cqe);
    }
    return true;
  }

 private:
  io_uring ring_{};
};
#endif

}  // namespace flox::net

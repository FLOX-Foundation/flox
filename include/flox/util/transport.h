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

/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#pragma once

// AF_XDP backend for IReceivePath. The kernel keeps the NIC; an XDP program
// (libxdp attaches its default redirector) short-circuits frames into a
// user-space UMEM ring before sk_buff allocation. Compared to the socket
// backend this removes the kernel network stack and the syscall per batch
// from the receive path; compared to DPDK the host stays a normal Linux
// machine.
//
// Delivery semantics match UdpSocketReceivePath: the callback receives the
// UDP payload, not the raw Ethernet frame. Frames that are not UDP, or that
// miss the configured port filter, are counted and dropped.
//
// Requires: Linux, libxdp/libbpf, CAP_NET_RAW + CAP_BPF (or root), and a
// build with -DFLOX_ENABLE_AF_XDP=ON. Generic (SKB) mode works on any
// driver including veth; native mode needs driver support and is the
// deployment choice, not the default.

#if defined(__linux__) && FLOX_AF_XDP_ENABLED

#include <linux/if_ether.h>
#include <linux/if_link.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <sys/mman.h>
#include <xdp/xsk.h>

#include <cstring>
#include <string>

#include "flox/net/receive_path.h"

namespace flox::net
{

class AfXdpReceivePath : public IReceivePath
{
 public:
  struct Config
  {
    std::string ifname;
    uint32_t queueId{0};
    uint16_t udpPortFilter{0};  // 0 = accept every UDP port
    uint32_t frameCount{4096};  // UMEM frames of XSK_UMEM__DEFAULT_FRAME_SIZE
    bool skbMode{true};         // generic XDP; native needs driver support
  };

  explicit AfXdpReceivePath(const Config& cfg) : _cfg(cfg)
  {
    const size_t frameSize = XSK_UMEM__DEFAULT_FRAME_SIZE;
    _umemSize = size_t(cfg.frameCount) * frameSize;
    _umemArea = ::mmap(nullptr, _umemSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (_umemArea == MAP_FAILED)
    {
      _umemArea = nullptr;
      return;
    }

    // Size the fill and completion rings to hold every frame. The default
    // ring size is 2048; leaving it default while reserving frameCount>2048
    // makes the reserve fail, the fill ring stay empty, and the kernel has
    // nowhere to deliver -- zero packets with no error.
    xsk_umem_config ucfg{};
    ucfg.fill_size = cfg.frameCount;
    ucfg.comp_size = cfg.frameCount;
    ucfg.frame_size = frameSize;
    ucfg.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;
    ucfg.flags = 0;
    if (xsk_umem__create(&_umem, _umemArea, _umemSize, &_fill, &_comp, &ucfg) != 0)
    {
      return;
    }

    xsk_socket_config scfg{};
    scfg.rx_size = cfg.frameCount;
    scfg.tx_size = 0;
    scfg.xdp_flags = cfg.skbMode ? XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE;
    scfg.bind_flags = XDP_COPY | XDP_USE_NEED_WAKEUP;

    if (xsk_socket__create(&_xsk, cfg.ifname.c_str(), cfg.queueId, _umem, &_rx,
                           nullptr, &scfg) != 0)
    {
      return;
    }
    _xskFd = xsk_socket__fd(_xsk);

    // Hand frames to the fill ring: the kernel writes arriving frames into
    // them and returns them through the rx ring. Reserve one less than the
    // ring so it is never completely full.
    const uint32_t fillCount = cfg.frameCount;
    uint32_t idx = 0;
    if (xsk_ring_prod__reserve(&_fill, fillCount, &idx) == fillCount)
    {
      for (uint32_t i = 0; i < fillCount; ++i)
      {
        *xsk_ring_prod__fill_addr(&_fill, idx + i) = uint64_t(i) * frameSize;
      }
      xsk_ring_prod__submit(&_fill, fillCount);
    }
    _valid = true;
  }

  ~AfXdpReceivePath() override
  {
    if (_xsk)
    {
      xsk_socket__delete(_xsk);
    }
    if (_umem)
    {
      xsk_umem__delete(_umem);
    }
    if (_umemArea)
    {
      ::munmap(_umemArea, _umemSize);
    }
  }

  AfXdpReceivePath(const AfXdpReceivePath&) = delete;
  AfXdpReceivePath& operator=(const AfXdpReceivePath&) = delete;

  bool valid() const noexcept { return _valid; }
  uint64_t droppedNonUdp() const noexcept { return _droppedNonUdp; }

  RxTimestampMode timestampMode() const override
  {
    // Frames carry no kernel timestamp on this path; the poll loop stamps
    // with the monotonic clock at dequeue.
    return RxTimestampMode::SOFTWARE;
  }

  int poll(const std::function<void(const Datagram&)>& cb, int maxPackets) override
  {
    if (!_valid)
    {
      return 0;
    }

    uint32_t idx = 0;
    const uint32_t got =
        xsk_ring_cons__peek(&_rx, static_cast<uint32_t>(maxPackets), &idx);
    if (got == 0)
    {
      // With XDP_USE_NEED_WAKEUP the kernel will not process the fill ring
      // until poked once it has flagged a wakeup. A non-blocking recvfrom is
      // the documented kick.
      if (xsk_ring_prod__needs_wakeup(&_fill))
      {
        ::recvfrom(_xskFd, nullptr, 0, MSG_DONTWAIT, nullptr, nullptr);
      }
      return 0;
    }

    const int64_t now = performance::monotonicNs();
    int delivered = 0;

    for (uint32_t i = 0; i < got; ++i)
    {
      const xdp_desc* desc = xsk_ring_cons__rx_desc(&_rx, idx + i);
      const auto* frame =
          static_cast<const uint8_t*>(xsk_umem__get_data(_umemArea, desc->addr));

      const auto payload = udpPayload(frame, desc->len);
      if (payload.data() != nullptr)
      {
        Datagram d;
        d.payload = payload;
        d.rxTimestampNs = now;
        cb(d);
        ++delivered;
      }
      else
      {
        ++_droppedNonUdp;
      }

      // Return the frame to the fill ring for reuse.
      uint32_t fidx = 0;
      if (xsk_ring_prod__reserve(&_fill, 1, &fidx) == 1)
      {
        *xsk_ring_prod__fill_addr(&_fill, fidx) = desc->addr;
        xsk_ring_prod__submit(&_fill, 1);
      }
    }

    xsk_ring_cons__release(&_rx, got);
    return delivered;
  }

 private:
  std::span<const std::byte> udpPayload(const uint8_t* frame, uint32_t len) const
  {
    if (len < sizeof(ethhdr) + sizeof(iphdr) + sizeof(udphdr))
    {
      return {};
    }
    const auto* eth = reinterpret_cast<const ethhdr*>(frame);
    if (ntohs(eth->h_proto) != ETH_P_IP)
    {
      return {};
    }
    const auto* ip = reinterpret_cast<const iphdr*>(frame + sizeof(ethhdr));
    if (ip->protocol != IPPROTO_UDP)
    {
      return {};
    }
    const size_t ipLen = size_t(ip->ihl) * 4;
    const auto* udp = reinterpret_cast<const udphdr*>(frame + sizeof(ethhdr) + ipLen);
    if (_cfg.udpPortFilter != 0 && ntohs(udp->dest) != _cfg.udpPortFilter)
    {
      return {};
    }
    const size_t header = sizeof(ethhdr) + ipLen + sizeof(udphdr);
    const size_t udpLen = ntohs(udp->len);
    if (udpLen < sizeof(udphdr) || header + (udpLen - sizeof(udphdr)) > len)
    {
      return {};
    }
    return {reinterpret_cast<const std::byte*>(frame + header),
            udpLen - sizeof(udphdr)};
  }

  Config _cfg;
  void* _umemArea{nullptr};
  size_t _umemSize{0};
  xsk_umem* _umem{nullptr};
  xsk_socket* _xsk{nullptr};
  xsk_ring_prod _fill{};
  xsk_ring_cons _comp{};
  xsk_ring_cons _rx{};
  int _xskFd{-1};
  bool _valid{false};
  uint64_t _droppedNonUdp{0};
};

}  // namespace flox::net

#endif  // __linux__ && FLOX_AF_XDP_ENABLED

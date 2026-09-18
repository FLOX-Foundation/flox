/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Receiving somebody else's feed with this venue's machinery.
 *
 * The socket, the receive timeout, the gap detector and the resequencing
 * buffer are about datagrams arriving out of order on a multicast group. None
 * of that is particular to how a venue lays out its bytes -- but until the
 * decoder became a parameter, all of it came welded to this venue's SBE
 * layout, and a consumer of another feed could reuse none of it.
 *
 * The decoder here is deliberately NOT SBE: a different byte order, a
 * different field order, a different size. If the seam were only nominal, this
 * file would not compile, let alone sequence.
 */
#include "flox-venue/md_recovery.h"
#include "flox-venue/udp_multicast.h"

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

using namespace flox;
using namespace flox::venue;

namespace
{

constexpr SymbolId SYM = 42;

// A foreign wire format: big-endian, fields in an order this venue never
// uses, and -- deliberately -- LARGER than SbeMdCodec's largest datagram.
// Larger is the direction that matters: a subscriber sizing its buffer from
// its own codec instead of the one it was given would truncate every foreign
// datagram, and a smaller foreign format would hide that behind a buffer that
// happened to be big enough.
struct ForeignCodec
{
  static_assert(SbeMdCodec::kMaxSize < 128, "the foreign format must stay the larger of the two");
  static constexpr size_t kMaxSize = 128;

  static void put64(uint8_t* p, uint64_t v)
  {
    for (int i = 0; i < 8; ++i)
    {
      p[i] = static_cast<uint8_t>(v >> (56 - 8 * i));  // big-endian, on purpose
    }
  }
  static uint64_t get64(const uint8_t* p)
  {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
    {
      v = (v << 8) | p[i];
    }
    return v;
  }

  // epoch, seq, symbol, type -- an order nothing else here uses.
  static void encode(const MdMessage& m, std::vector<uint8_t>& out)
  {
    out.assign(kMaxSize, 0);
    put64(out.data() + 0, m.epoch);
    put64(out.data() + 8, m.seq);
    put64(out.data() + 16, static_cast<uint64_t>(m.symbol));
    out[24] = static_cast<uint8_t>(m.type);
    // A marker at the very end: a datagram cut short by an undersized buffer
    // loses this, and decode refuses it.
    out[kMaxSize - 1] = 0xA5;
  }

  static bool decode(const uint8_t* p, size_t n, MdMessage& out)
  {
    if (n != kMaxSize || p[kMaxSize - 1] != 0xA5)
    {
      return false;  // truncated, or not ours
    }
    out = MdMessage{};
    out.epoch = get64(p + 0);
    out.seq = get64(p + 8);
    out.symbol = static_cast<SymbolId>(get64(p + 16));
    out.type = static_cast<MdType>(p[24]);
    return true;
  }
};

MdMessage msg(uint64_t seq, uint64_t epoch = 1)
{
  MdMessage m;
  m.type = MdType::AddOrder;
  m.seq = seq;
  m.symbol = SYM;
  m.epoch = epoch;
  return m;
}

// A publisher for the foreign format: the transport is the venue's, the bytes
// are not.
struct ForeignPublisher
{
  net::Handle fd{net::kInvalid};
  ::sockaddr_in dst{};

  bool open(uint16_t port)
  {
    fd = net::openSocket(net::Kind::Udp);
    return net::valid(fd) && net::parseAddress("127.0.0.1", dst, port);
  }
  bool send(const MdMessage& m)
  {
    std::vector<uint8_t> b;
    ForeignCodec::encode(m, b);
    return net::sendTo(fd, b.data(), b.size(), dst) == static_cast<long>(b.size());
  }
  ~ForeignPublisher() { net::closeSocket(fd); }
};

}  // namespace

// The whole point: a subscriber built on a foreign decoder sequences through
// the venue's gap detector, and the holes it reports are the foreign stream's.
TEST(MdDecoderSeam, AForeignLayoutSequencesThroughTheVenuesGapDetector)
{
  UdpMdSubscriberT<ForeignCodec> sub;
  ASSERT_TRUE(sub.join("239.255.42.7", 0, /*multicast=*/false));
  sub.setTimeout(500);

  GapDetector gd;
  std::vector<std::pair<uint64_t, uint64_t>> gaps;  // (from, to)
  sub.setGapDetector(&gd, [&](SymbolId, uint64_t from, uint64_t to)
                     { gaps.emplace_back(from, to); });

  ForeignPublisher pub;
  ASSERT_TRUE(pub.open(static_cast<uint16_t>(sub.port())));

  // 1, 2, then a hole, then 5 -- and 3,4 arriving late, out of order.
  ASSERT_TRUE(pub.send(msg(1)));
  ASSERT_TRUE(pub.send(msg(2)));
  ASSERT_TRUE(pub.send(msg(5)));
  ASSERT_TRUE(pub.send(msg(4)));
  ASSERT_TRUE(pub.send(msg(3)));

  std::vector<uint64_t> delivered;
  MdMessage got;
  for (int i = 0; i < 5 && sub.recv(got); ++i)
  {
    delivered.push_back(got.seq);
    EXPECT_EQ(got.symbol, SYM) << "the foreign decoder's symbol survived the seam";
  }

  // In order, despite arriving 1,2,5,4,3 -- the resequencing is the venue's
  // and knows nothing about the format it just sequenced.
  const std::vector<uint64_t> expected{1, 2, 3, 4, 5};
  EXPECT_EQ(delivered, expected);
}

// A decoder that does not fill seq must break sequencing LOUDLY. Silence here
// would be the worst outcome: a consumer would believe it had a complete
// stream while holes went by unreported.
TEST(MdDecoderSeam, ADecoderThatLeavesSeqUnsetCannotSequence)
{
  struct SeqlessCodec : ForeignCodec
  {
    static bool decode(const uint8_t* p, size_t n, MdMessage& out)
    {
      if (!ForeignCodec::decode(p, n, out))
      {
        return false;
      }
      out.seq = 0;  // the defect under test
      return true;
    }
  };

  UdpMdSubscriberT<SeqlessCodec> sub;
  ASSERT_TRUE(sub.join("239.255.42.8", 0, /*multicast=*/false));
  sub.setTimeout(300);

  GapDetector gd;
  int gapCalls = 0;
  sub.setGapDetector(&gd, [&](SymbolId, uint64_t, uint64_t)
                     { ++gapCalls; });

  ForeignPublisher pub;
  ASSERT_TRUE(pub.open(static_cast<uint16_t>(sub.port())));
  ASSERT_TRUE(pub.send(msg(1)));
  ASSERT_TRUE(pub.send(msg(2)));
  ASSERT_TRUE(pub.send(msg(3)));

  // Every message claims seq 0, so after the first one the detector has
  // nothing it can put in order: the stream stalls rather than quietly
  // delivering messages it cannot sequence.
  int deliveredCount = 0;
  MdMessage got;
  while (deliveredCount < 3 && sub.recv(got))
  {
    ++deliveredCount;
    EXPECT_EQ(got.seq, 0u);
  }
  EXPECT_LT(deliveredCount, 3)
      << "a decoder that never sets seq delivered a full in-order stream: the detector was blind";
}

// The default is still the venue's own feed, spelled the way every caller
// spells it. A seam that quietly changed the default would be a different
// change than the one intended.
TEST(MdDecoderSeam, TheDefaultSubscriberIsStillTheVenuesOwn)
{
  static_assert(std::is_same_v<UdpMdSubscriber, UdpMdSubscriberT<SbeMdCodec>>,
                "the unparameterised name must still mean this venue's format");
  static_assert(std::is_same_v<RecoveringMdSubscriber, RecoveringMdSubscriberT<UdpMdSubscriber>>,
                "and so must the recovering one");
  SUCCEED();
}

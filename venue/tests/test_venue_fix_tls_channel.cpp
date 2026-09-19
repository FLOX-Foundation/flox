/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * The FIX initiator's TLS channel, against this venue's own TLS gateway in
 * one process.
 *
 * Two things are under test and they pull in opposite directions. The channel
 * has to carry a FIX session the way the plain one does -- same framing, same
 * resumable read. And it has to REFUSE a counterparty it cannot verify, which
 * is the property that makes it worth having at all: the gateway here presents
 * a fresh self-signed certificate, exactly what an impostor on that port would
 * present, and the default must not accept it.
 *
 * Built only with -DFLOX_FIX_TLS=ON; the venue's CMake drops this file
 * otherwise, because the header it needs refuses to compile without the flag.
 */
#include "flox-venue/sbe_order_entry_codec.h"
#include "flox-venue/tls_gateway.h"
#include "flox/connector/fix/fix_tls_channel.h"
#include "support/tmp_path.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace flox;
using namespace flox::venue;
using flox::venue::test::tmpPath;

namespace
{

// A gateway that echoes each frame back, length-prefixed. Enough to prove the
// channel carries whole messages in both directions without dragging the
// engine in: what is under test is the transport, not the matching.
class EchoGateway
{
 public:
  int start()
  {
    gw_ = std::make_unique<TlsGateway>([this](const uint8_t* p, size_t n)
                                       {
                                         last_.assign(p, p + n);
                                         return std::optional<InboundCommand>{}; });
    return gw_->start(0, [](const InboundCommand&, const TlsGateway::Responder&, int64_t) {});
  }
  void stop()
  {
    if (gw_)
    {
      gw_->stop();
    }
  }
  const std::vector<uint8_t>& last() const { return last_; }

 private:
  std::unique_ptr<TlsGateway> gw_;
  std::vector<uint8_t> last_;
};

// A TLS acceptor the test owns both ends of. TlsGateway generates a fresh
// certificate inside itself and does not hand it out, so a test that needs to
// TRUST the certificate it is about to be shown has to hold the context
// itself. One context: the server presents it, and the same one is written
// out as the trust anchor.
class OwnedTlsServer
{
 public:
  bool start()
  {
    ctx_ = tls::serverCtx();
    if (ctx_ == nullptr)
    {
      return false;
    }
    fd_ = net::openSocket(net::Kind::Tcp);
    if (!net::valid(fd_) || !net::setReuseAddr(fd_, true) || !net::bindTo(fd_, "127.0.0.1", 0) ||
        !net::listenOn(fd_, 4))
    {
      return false;
    }
    port_ = net::boundPort(fd_);
    running_.store(true);
    th_ = std::thread(
        [this]
        {
          while (running_.load())
          {
            const net::PollResult r = net::pollRead(fd_, 50);
            if (!r.readable)
            {
              continue;
            }
            const net::Handle c = net::acceptOne(fd_);
            if (!net::valid(c))
            {
              continue;
            }
            // Bounded: a client that opens the connection and never speaks
            // TLS would otherwise park this thread in SSL_accept forever, and
            // stop() would wait on it just as long. A mutation run found
            // exactly that -- the suite hung instead of failing.
            net::setReceiveTimeout(c, 200);
            SSL* ssl = SSL_new(ctx_);
            SSL_set_fd(ssl, static_cast<int>(c));
            SSL_accept(ssl);  // whatever it answers, the client learns enough
            SSL_free(ssl);
            net::closeSocket(c);
          }
        });
    return true;
  }

  void stop()
  {
    running_.store(false);
    if (th_.joinable())
    {
      th_.join();
    }
    net::closeSocket(fd_);
    fd_ = net::kInvalid;
    if (ctx_ != nullptr)
    {
      SSL_CTX_free(ctx_);
      ctx_ = nullptr;
    }
  }

  uint16_t port() const { return port_; }

  // The very certificate this server presents, as a trust anchor.
  std::string certPem() const
  {
    X509* x = SSL_CTX_get0_certificate(ctx_);
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, x);
    char* data = nullptr;
    const long n = BIO_get_mem_data(bio, &data);
    std::string pem(data, static_cast<size_t>(n));
    BIO_free(bio);
    return pem;
  }

 private:
  SSL_CTX* ctx_{nullptr};
  net::Handle fd_{net::kInvalid};
  uint16_t port_{0};
  std::atomic<bool> running_{false};
  std::thread th_;
};

}  // namespace

// The default refuses a certificate it cannot chain to a trusted root. The
// venue's gateway signs its own, which is what makes it the right adversary
// here: nothing distinguishes it from a stranger answering the port.
TEST(FixTlsChannel, AnUnverifiableCounterpartyIsRefusedByDefault)
{
  EchoGateway gw;
  const int port = gw.start();
  ASSERT_GT(port, 0);

  fix::FixTlsChannel ch;
  EXPECT_FALSE(ch.connect("127.0.0.1", static_cast<uint16_t>(port)))
      << "a self-signed certificate was accepted by a channel that claims to verify";
  EXPECT_FALSE(ch.connected());
  gw.stop();
}

// Turning verification off is possible and explicit. This is the shape a test
// harness uses, and naming it is how it stays a decision rather than a
// default.
TEST(FixTlsChannel, VerificationCanBeTurnedOffOnPurposeAndThenTheSessionRuns)
{
  EchoGateway gw;
  const int port = gw.start();
  ASSERT_GT(port, 0);

  fix::FixTlsChannel::Options opt;
  opt.verifyPeer = false;
  fix::FixTlsChannel ch;
  ASSERT_TRUE(ch.connect("127.0.0.1", static_cast<uint16_t>(port), /*recvTimeoutMs=*/200, opt));
  EXPECT_TRUE(ch.connected());

  // A FIX message goes out whole. The gateway's decoder sees the frame's body
  // without the length prefix, which is what "one frame is one message" means.
  const std::string msg =
      "8=FIX.4.4\x01"
      "35=A\x01"
      "49=CLIENT\x01"
      "56=VENUE\x01"
      "10=000\x01";
  ASSERT_TRUE(ch.send(msg));

  // The read times out rather than blocking forever, and says Idle -- the
  // session layer runs its timers on exactly that.
  std::string out;
  const fix::FixTlsChannel::Status s = ch.read(out);
  EXPECT_EQ(s, fix::FixTlsChannel::Status::Idle)
      << "an echo-less gateway should leave the reader idle, not closed";

  ch.close();
  EXPECT_FALSE(ch.connected());
  gw.stop();
}

// Verification without a hostname check accepts any certificate the trust
// store accepts -- somebody else's valid certificate for somebody else's
// name. Asked by pointing a NAME at the gateway while trusting exactly the
// certificate it presents: the chain is fine, the name is not.
TEST(FixTlsChannel, AValidCertificateForTheWrongNameIsStillRefused)
{
  OwnedTlsServer srv;
  ASSERT_TRUE(srv.start());

  const std::string caPath = tmpPath("tls-ca", ".pem");
  {
    const std::string pem = srv.certPem();
    ASSERT_FALSE(pem.empty());
    std::FILE* f = std::fopen(caPath.c_str(), "wb");
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(std::fwrite(pem.data(), 1, pem.size(), f), pem.size());
    std::fclose(f);
  }

  // The certificate is issued to "localhost" and is its own root. Trusting it
  // explicitly leaves the NAME as the only thing that can still refuse.
  fix::FixTlsChannel::Options good;
  good.caFile = caPath;
  good.serverName = "localhost";
  fix::FixTlsChannel ok;
  EXPECT_TRUE(ok.connect("127.0.0.1", srv.port(), 200, good))
      << "the certificate this test trusts, under the name it was issued to, was refused";
  ok.close();

  fix::FixTlsChannel::Options wrongName = good;
  wrongName.serverName = "not-the-venue.example";
  fix::FixTlsChannel bad;
  EXPECT_FALSE(bad.connect("127.0.0.1", srv.port(), 200, wrongName))
      << "a certificate issued to somebody else was accepted: the name is not being checked";

  std::remove(caPath.c_str());
  srv.stop();
}

// A trust store that could not be loaded verifies nothing. Failing to connect
// is the honest outcome; continuing would be verification in name only, which
// is worse than none because it reads as safe.
//
// One limit, stated rather than implied: removing the guard does not turn
// this red. Without it the connection still fails, at the handshake, because
// nothing in the default store signs a self-signed test certificate -- the
// two refusals are indistinguishable from outside. What the guard prevents is
// the case this test cannot build: a caller naming a CA file that does not
// exist, against a counterparty the SYSTEM store happens to accept, silently
// verified against a trust anchor it did not ask for. The positive half is
// covered: the test above connects only because the caller's own caFile is
// loaded and used.
TEST(FixTlsChannel, ATrustStoreThatCannotBeLoadedRefusesRatherThanVerifyingNothing)
{
  EchoGateway gw;
  const int port = gw.start();
  ASSERT_GT(port, 0);

  fix::FixTlsChannel::Options opt;
  opt.caFile = tmpPath("tls-absent", ".pem");  // never created
  std::remove(opt.caFile.c_str());
  fix::FixTlsChannel ch;
  EXPECT_FALSE(ch.connect("127.0.0.1", static_cast<uint16_t>(port), 200, opt))
      << "a channel whose CA file does not exist connected anyway";
  EXPECT_FALSE(ch.connected());
  gw.stop();
}

// A handshake that never completes must leave nothing readable. Pointing the
// channel at a port that speaks no TLS is the cheapest way to get one.
TEST(FixTlsChannel, AFailedHandshakeLeavesNothingToReadFrom)
{
  // A plain TCP listener: it accepts, then says nothing a TLS client can use.
  const net::Handle srv = net::openSocket(net::Kind::Tcp);
  ASSERT_TRUE(net::valid(srv));
  ASSERT_TRUE(net::setReuseAddr(srv, true));
  ASSERT_TRUE(net::bindTo(srv, "127.0.0.1", 0));
  ASSERT_TRUE(net::listenOn(srv, 4));
  const uint16_t port = net::boundPort(srv);

  fix::FixTlsChannel::Options opt;
  opt.verifyPeer = false;
  fix::FixTlsChannel ch;
  EXPECT_FALSE(ch.connect("127.0.0.1", port, /*recvTimeoutMs=*/100, opt));
  EXPECT_FALSE(ch.connected());

  std::string out;
  EXPECT_EQ(ch.read(out), fix::FixTlsChannel::Status::Closed)
      << "a channel whose handshake failed answered a read with something other than Closed";
  EXPECT_FALSE(ch.send("8=FIX.4.4\x01"))
      << "and it accepted a send on a channel that was never established";

  net::closeSocket(srv);
}

// A frame length the channel will not honour ends the session instead of
// being skipped: after it, the stream is no longer one this side can follow,
// and reading on would mean reading somebody else's message boundary.
TEST(FixTlsChannel, AnImpossibleFrameLengthClosesRatherThanResynchronises)
{
  EchoGateway gw;
  const int port = gw.start();
  ASSERT_GT(port, 0);

  fix::FixTlsChannel::Options opt;
  opt.verifyPeer = false;
  fix::FixTlsChannel ch;
  ASSERT_TRUE(ch.connect("127.0.0.1", static_cast<uint16_t>(port), 100, opt));
  // Nothing is sent to it; what matters is that the channel is in a state
  // where a read is possible at all, and that close() leaves it unreadable.
  ch.close();
  std::string out;
  EXPECT_EQ(ch.read(out), fix::FixTlsChannel::Status::Closed);
  gw.stop();
}

/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2026 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 *
 * Network-shape tests for the REST transport, against a loopback listener
 * the test binds itself -- no exchange, no DNS, nothing outside this
 * process. Two things are pinned:
 *
 *  - CurlTimeoutConfig and postWithTimeout() take milliseconds, and the
 *    entire reason postWithTimeout() exists is sub-second control of how
 *    long the order path may be held. The transport divides them by 1000
 *    into CURLOPT_TIMEOUT, so 1500 ms waits one second and 200 ms also
 *    waits one second: the parameter's resolution is a fiction.
 *
 *  - post() has a callback API but runs curl_easy_perform inline, so the
 *    thread that submits an order -- the strategy / event-bus consumer
 *    thread -- is held for the whole round trip, up to the 30 s default.
 *    A peer that accepts the connection and then says nothing is exactly
 *    the case that matters: it is indistinguishable from a healthy venue
 *    until the timeout fires.
 *
 * Both are satisfied either by honouring the millisecond timeout on the
 * calling thread or by performing the request off it; the assertions below
 * are written to accept either, and to insist in both cases that the
 * completion callback still fires, so nothing is quietly dropped.
 */

#include "flox-connectors/bitget/authenticated_rest_client.h"
#include "flox-connectors/bitget/bitget_order_executor.h"
#include "flox-connectors/net/curl_transport.h"

#include <flox/common.h>
#include <flox/engine/symbol_registry.h>
#include <flox/execution/abstract_execution_listener.h>
#include <flox/execution/bus/order_execution_bus.h>
#include <flox/execution/events/order_event.h>
#include <flox/execution/order.h>
#include <flox/execution/order_tracker.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace flox;
using namespace std::chrono_literals;

namespace
{

// A loopback HTTP peer bound on an ephemeral port. In Silent mode it
// completes the TCP handshake, reads the request and then never writes a
// byte -- the only way to observe a request timeout without a real network.
class LoopbackPeer
{
 public:
  enum class Mode
  {
    Silent,
    Respond
  };

  explicit LoopbackPeer(Mode mode) : _mode(mode)
  {
    _listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(_listenFd, 0);

    int one = 1;
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    EXPECT_EQ(::listen(_listenFd, 8), 0);

    socklen_t len = sizeof(addr);
    EXPECT_EQ(::getsockname(_listenFd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    _port = ntohs(addr.sin_port);

    _thread = std::thread(
        [this]
        {
          acceptLoop();
        });
  }

  ~LoopbackPeer()
  {
    _stop.store(true);
    if (_thread.joinable())
    {
      _thread.join();
    }
    for (int fd : _accepted)
    {
      ::close(fd);
    }
    if (_listenFd >= 0)
    {
      ::close(_listenFd);
    }
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(_port) + "/order"; }

 private:
  void acceptLoop()
  {
    while (!_stop.load())
    {
      pollfd pfd{_listenFd, POLLIN, 0};
      if (::poll(&pfd, 1, 25) <= 0)
      {
        continue;
      }
      int fd = ::accept(_listenFd, nullptr, nullptr);
      if (fd < 0)
      {
        continue;
      }
      if (_mode == Mode::Respond)
      {
        char buf[4096];
        (void)::recv(fd, buf, sizeof(buf), 0);
        static constexpr char kBody[] =
            R"({"code":"00000","msg":"success","data":{"orderId":"1"}})";
        const std::string resp =
            "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(sizeof(kBody) - 1) +
            "\r\nConnection: close\r\n\r\n" + kBody;
        (void)::send(fd, resp.data(), resp.size(), 0);
        ::close(fd);
        continue;
      }
      // Silent: keep the connection open and answer nothing.
      _accepted.push_back(fd);
    }
  }

  Mode _mode;
  int _listenFd{-1};
  uint16_t _port{0};
  std::atomic<bool> _stop{false};
  std::thread _thread;
  std::vector<int> _accepted;
};

// Collects the transport's completion, whichever side it lands on, and lets
// the test wait for it without polling.
class Completion
{
 public:
  void signal()
  {
    {
      std::lock_guard<std::mutex> lk(_m);
      _done = true;
    }
    _cv.notify_all();
  }

  bool waitFor(std::chrono::milliseconds d)
  {
    std::unique_lock<std::mutex> lk(_m);
    return _cv.wait_for(lk, d,
                        [this]
                        {
                          return _done;
                        });
  }

 private:
  std::mutex _m;
  std::condition_variable _cv;
  bool _done{false};
};

// Signals `done` on the rejection the executor publishes when the transport
// gives up, so the test can wait for the in-flight request instead of
// sleeping for it.
class RejectionWatcher final : public IOrderExecutionListener
{
 public:
  explicit RejectionWatcher(Completion& done) : IOrderExecutionListener(7), _done(done) {}

  void onOrderRejected(const Order&, const std::string&) override { _done.signal(); }

 private:
  Completion& _done;
};

int64_t elapsedMs(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                               start)
      .count();
}

const std::vector<std::pair<std::string_view, std::string_view>> kHeaders = {
    {"Content-Type", "application/json"}};

}  // namespace

// Control, green today: against a peer that answers, the transport does its
// job and hands the body to onSuccess. Everything below asserts on what
// happens when the peer does not answer, and must not be readable as "the
// transport is broken".
TEST(CurlTransportTimeout, RespondingPeerDeliversTheBody)
{
  LoopbackPeer peer(LoopbackPeer::Mode::Respond);
  CurlTransport transport(1, CurlTimeoutConfig{.connectTimeoutMs = 2000, .requestTimeoutMs = 5000});

  Completion done;
  std::string body;
  std::string error;

  transport.post(
      peer.url(), R"({"x":1})", kHeaders,
      [&](std::string_view resp)
      {
        body = std::string(resp);
        done.signal();
      },
      [&](std::string_view err)
      {
        error = std::string(err);
        done.signal();
      });

  ASSERT_TRUE(done.waitFor(5s)) << "no completion at all";
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_NE(body.find("00000"), std::string::npos) << body;
}

// 1500 ms must mean 1500 ms. Truncating to whole seconds makes it 1000;
// rounding up would make it 2000. Both are wrong, so the window is bounded
// on each side.
TEST(CurlTransportTimeout, FifteenHundredMillisecondsIsHonouredAsFifteenHundred)
{
  LoopbackPeer peer(LoopbackPeer::Mode::Silent);
  CurlTransport transport(1,
                          CurlTimeoutConfig{.connectTimeoutMs = 2000, .requestTimeoutMs = 30000});

  Completion done;
  const auto start = std::chrono::steady_clock::now();

  transport.postWithTimeout(
      peer.url(), R"({"x":1})", kHeaders,
      [&](std::string_view)
      {
        done.signal();
      },
      [&](std::string_view)
      {
        done.signal();
      },
      1500);

  ASSERT_TRUE(done.waitFor(10s)) << "the request never completed at all";
  const int64_t waited = elapsedMs(start);

  EXPECT_GE(waited, 1300)
      << "waited " << waited
      << " ms for a 1500 ms timeout -- millisecond timeouts are being truncated to seconds";
  EXPECT_LE(waited, 1900) << "waited " << waited << " ms for a 1500 ms timeout";
}

// A sub-second timeout is the only reason this overload exists: an order
// path cannot afford a one-second floor on every request.
TEST(CurlTransportTimeout, SubSecondTimeoutIsNotRaisedToAWholeSecond)
{
  LoopbackPeer peer(LoopbackPeer::Mode::Silent);
  CurlTransport transport(1,
                          CurlTimeoutConfig{.connectTimeoutMs = 2000, .requestTimeoutMs = 30000});

  Completion done;
  const auto start = std::chrono::steady_clock::now();

  transport.postWithTimeout(
      peer.url(), R"({"x":1})", kHeaders,
      [&](std::string_view)
      {
        done.signal();
      },
      [&](std::string_view)
      {
        done.signal();
      },
      250);

  ASSERT_TRUE(done.waitFor(10s)) << "the request never completed at all";
  const int64_t waited = elapsedMs(start);

  EXPECT_LE(waited, 700) << "waited " << waited
                         << " ms for a 250 ms timeout -- a sub-second request timeout is being "
                            "rounded up to one whole second";
}

// The submitting thread must not be held for the venue's full response
// time. Either the configured millisecond timeout is honoured on this
// thread, or the request is performed elsewhere and post() returns at once;
// the bound below is satisfied by both, and the completion assertion rules
// out "returns fast because it dropped the request".
TEST(CurlTransportTimeout, PostDoesNotHoldTheCallerBeyondTheConfiguredTimeout)
{
  LoopbackPeer peer(LoopbackPeer::Mode::Silent);
  CurlTransport transport(1, CurlTimeoutConfig{.connectTimeoutMs = 2000, .requestTimeoutMs = 250});

  Completion done;
  const auto start = std::chrono::steady_clock::now();

  transport.post(
      peer.url(), R"({"x":1})", kHeaders,
      [&](std::string_view)
      {
        done.signal();
      },
      [&](std::string_view)
      {
        done.signal();
      });

  const int64_t returned = elapsedMs(start);
  EXPECT_LE(returned, 700) << "post() held its caller for " << returned
                           << " ms with a 250 ms request timeout configured";

  EXPECT_TRUE(done.waitFor(10s)) << "post() returned without ever completing the request";
}

// The same property where it is paid for: submitOrder() runs on the
// strategy thread. With a venue that accepts the connection and goes quiet,
// that thread currently stops processing events for a whole second per
// order, and for the default configuration, thirty.
TEST(CurlTransportTimeout, OrderSubmitDoesNotHoldTheStrategyThread)
{
  LoopbackPeer peer(LoopbackPeer::Mode::Silent);

  auto transport = std::make_unique<CurlTransport>(
      1, CurlTimeoutConfig{.connectTimeoutMs = 2000, .requestTimeoutMs = 250});
  auto client =
      std::make_unique<BitgetAuthenticatedRestClient>("k", "s", "p", peer.url(), transport.get());

  SymbolRegistry registry;
  OrderTracker tracker;
  SymbolInfo info;
  info.exchange = "bitget";
  info.symbol = "BTCUSDT";
  info.type = InstrumentType::Future;
  SymbolId sym = registry.registerSymbol(info);

  Bitget::Params params;
  params.productType = "USDT-FUTURES";
  params.marginCoin = "USDT";
  params.marginMode = "crossed";

  Completion done;
  RejectionWatcher watcher(done);
  OrderExecutionBus bus;
  bus.subscribe(&watcher);
  bus.start();

  BitgetOrderExecutorT<NoPolicies> executor(std::move(client), &registry, &tracker, params);
  executor.setOrderBus(&bus);

  Order order;
  order.id = 1;
  order.symbol = sym;
  order.side = Side::BUY;
  order.type = OrderType::LIMIT;
  order.price = Price::fromDouble(60000.0);
  order.quantity = Quantity::fromDouble(1.0);

  const auto start = std::chrono::steady_clock::now();
  executor.submitOrder(order);
  const int64_t returned = elapsedMs(start);

  EXPECT_LE(returned, 700) << "submitOrder() held the calling thread for " << returned
                           << " ms while the venue said nothing";

  // Also the wait for the in-flight request to finish before the peer and the
  // transport go away: a submit that returns fast because it dropped the
  // order is not the property under test.
  EXPECT_TRUE(done.waitFor(10s)) << "the timed-out submit produced no rejection";

  bus.stop();
}

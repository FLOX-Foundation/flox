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
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace flox;
using namespace std::chrono_literals;

namespace
{

// A loopback HTTP peer bound on an ephemeral port, one handler thread per
// accepted connection. Silent completes the handshake, reads the request and
// then never writes a byte -- the only way to observe a request timeout
// without a real network. Respond answers at once; SlowRespond holds the
// request open for a fixed delay, which is what makes send ordering and
// concurrency observable: with one sender thread no two requests are ever in
// the peer's hands at the same time.
class LoopbackPeer
{
 public:
  enum class Mode
  {
    Silent,
    Respond,
    SlowRespond
  };

  explicit LoopbackPeer(Mode mode, std::chrono::milliseconds delay = std::chrono::milliseconds(0))
      : _mode(mode), _delay(delay)
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
    for (auto& h : _handlers)
    {
      if (h.joinable())
      {
        h.join();
      }
    }
  }

  std::string url() const { return "http://127.0.0.1:" + std::to_string(_port) + "/order"; }

  // Request bodies in the order the peer read them off the wire.
  std::vector<std::string> bodies()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _bodies;
  }

  std::size_t requestCount()
  {
    std::lock_guard<std::mutex> lk(_m);
    return _bodies.size();
  }

  // The highest number of requests the peer held at the same moment. One
  // sender thread can never make this exceed 1.
  int maxConcurrent() const { return _maxConcurrent.load(); }

  bool waitForRequests(std::size_t n, std::chrono::milliseconds d)
  {
    std::unique_lock<std::mutex> lk(_m);
    return _cv.wait_for(lk, d,
                        [&]
                        {
                          return _bodies.size() >= n;
                        });
  }

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
      _handlers.emplace_back(
          [this, fd]
          {
            handle(fd);
          });
    }
    // Closed only once every handler is done with its own descriptor.
    if (_listenFd >= 0)
    {
      ::close(_listenFd);
      _listenFd = -1;
    }
  }

  // Reads one HTTP request off fd. Returns the body, or an empty string if
  // the peer was stopped before a complete request arrived.
  std::string readRequest(int fd)
  {
    std::string raw;
    std::size_t contentLength = 0;
    std::size_t headerEnd = 0;
    bool haveHeaders = false;

    while (!_stop.load())
    {
      if (haveHeaders && raw.size() >= headerEnd + contentLength)
      {
        return raw.substr(headerEnd, contentLength);
      }

      pollfd pfd{fd, POLLIN, 0};
      if (::poll(&pfd, 1, 25) <= 0)
      {
        continue;
      }
      char buf[4096];
      ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0)
      {
        return {};
      }
      raw.append(buf, static_cast<std::size_t>(n));

      if (!haveHeaders)
      {
        const auto pos = raw.find("\r\n\r\n");
        if (pos == std::string::npos)
        {
          continue;
        }
        haveHeaders = true;
        headerEnd = pos + 4;
        const auto clPos = raw.find("Content-Length:");
        if (clPos != std::string::npos)
        {
          contentLength =
              static_cast<std::size_t>(std::strtoul(raw.c_str() + clPos + 15, nullptr, 10));
        }
      }
    }
    return {};
  }

  void handle(int fd)
  {
    const std::string body = readRequest(fd);
    {
      std::lock_guard<std::mutex> lk(_m);
      _bodies.push_back(body);
    }
    _cv.notify_all();

    const int now = ++_concurrent;
    int seen = _maxConcurrent.load();
    while (now > seen && !_maxConcurrent.compare_exchange_weak(seen, now))
    {
    }

    if (_mode == Mode::Silent)
    {
      // Hold the connection open and answer nothing until the test is done.
      while (!_stop.load())
      {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    }
    else
    {
      if (_delay.count() > 0)
      {
        const auto until = std::chrono::steady_clock::now() + _delay;
        while (!_stop.load() && std::chrono::steady_clock::now() < until)
        {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
      static constexpr char kBody[] = R"({"code":"00000","msg":"success","data":{"orderId":"1"}})";
      const std::string resp =
          "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(sizeof(kBody) - 1) +
          "\r\nConnection: close\r\n\r\n" + kBody;
      (void)::send(fd, resp.data(), resp.size(), 0);
    }

    --_concurrent;
    ::close(fd);
  }

  Mode _mode;
  std::chrono::milliseconds _delay;
  int _listenFd{-1};
  uint16_t _port{0};
  std::atomic<bool> _stop{false};
  std::atomic<int> _concurrent{0};
  std::atomic<int> _maxConcurrent{0};
  std::thread _thread;
  std::vector<std::thread> _handlers;
  std::mutex _m;
  std::condition_variable _cv;
  std::vector<std::string> _bodies;
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

// The asynchronous shape itself, which the bounded-timeout assertions above
// cannot tell apart from a synchronous call that simply gives up sooner:
// post() hands the request over and comes back within a handful of
// milliseconds even though the peer will hold the request for the whole
// configured timeout, and the completion arrives later, on a different
// thread. Both halves matter -- a fast return that completed inline would
// mean the request never happened, and a completion on the caller's thread
// would mean the caller was the one performing it.
TEST(CurlTransportTimeout, PostReturnsAtOnceAndCompletesOnAnotherThread)
{
  LoopbackPeer peer(LoopbackPeer::Mode::Silent);
  CurlTransport transport(1, CurlTimeoutConfig{.connectTimeoutMs = 2000, .requestTimeoutMs = 3000});

  Completion done;
  const auto callerThread = std::this_thread::get_id();
  std::thread::id completionThread{};
  int64_t completedAt = -1;

  const auto start = std::chrono::steady_clock::now();
  transport.post(
      peer.url(), R"({"x":1})", kHeaders,
      [&](std::string_view)
      {
        completionThread = std::this_thread::get_id();
        completedAt = elapsedMs(start);
        done.signal();
      },
      [&](std::string_view)
      {
        completionThread = std::this_thread::get_id();
        completedAt = elapsedMs(start);
        done.signal();
      });

  const int64_t returned = elapsedMs(start);
  EXPECT_LE(returned, 150) << "post() took " << returned
                           << " ms to hand a request over to a peer that holds it for 3000 ms";

  ASSERT_TRUE(done.waitFor(15s)) << "the request never completed";
  EXPECT_NE(completionThread, callerThread)
      << "the completion ran on the calling thread, so the request was performed there";
  EXPECT_GE(completedAt, 2400) << "the completion arrived after " << completedAt
                               << " ms, well before the 3000 ms request timeout -- the request "
                                  "was not actually attempted for that long";
}

// One sender thread is a contract, not an implementation detail: it is what
// makes two requests leave in the order they were handed over, so a cancel
// submitted after a place cannot overtake it. The peer holds each request for
// 150 ms, so a second sender would be observable twice over -- as an
// overlapping pair and, sooner or later, as a swapped order.
TEST(CurlTransportTimeout, TwoPostsLeaveInHandoverOrder)
{
  LoopbackPeer peer(LoopbackPeer::Mode::SlowRespond, 150ms);
  // Four pooled handles: a pool of one would serialise the senders by itself
  // and hide the very thing this test is here to observe.
  CurlTransport transport(4, CurlTimeoutConfig{.connectTimeoutMs = 2000, .requestTimeoutMs = 5000});

  Completion first;
  Completion second;

  transport.post(
      peer.url(), R"({"seq":"place"})", kHeaders,
      [&](std::string_view)
      {
        first.signal();
      },
      [&](std::string_view)
      {
        first.signal();
      });
  transport.post(
      peer.url(), R"({"seq":"cancel"})", kHeaders,
      [&](std::string_view)
      {
        second.signal();
      },
      [&](std::string_view)
      {
        second.signal();
      });

  ASSERT_TRUE(first.waitFor(15s));
  ASSERT_TRUE(second.waitFor(15s));
  ASSERT_TRUE(peer.waitForRequests(2, 5s));

  const auto bodies = peer.bodies();
  ASSERT_EQ(bodies.size(), 2u);
  EXPECT_EQ(bodies[0], R"({"seq":"place"})") << "the second request overtook the first";
  EXPECT_EQ(bodies[1], R"({"seq":"cancel"})");
  EXPECT_EQ(peer.maxConcurrent(), 1)
      << "two requests were in flight at once, so they are no longer ordered by the handover";
}

// The handoff queue is bounded, and its bound is a refusal, not a drop. A
// venue that stops draining must be answered -- an order refused by a full
// queue that says nothing leaves the caller believing it is in flight. The
// same goes for whatever is still queued at stop(): it never reached the
// venue, and the caller has to be told.
TEST(CurlTransportTimeout, FullSendQueueRefusesAndStopAnswersTheRemainder)
{
  LoopbackPeer peer(LoopbackPeer::Mode::Silent);
  auto transport = std::make_unique<CurlTransport>(
      1, CurlTimeoutConfig{.connectTimeoutMs = 2000, .requestTimeoutMs = 1500},
      CurlDispatchConfig{.senderThreads = 1, .maxQueueDepth = 2});

  std::mutex m;
  std::vector<std::string> errors;
  int successes = 0;
  auto onError = [&](std::string_view e)
  {
    std::lock_guard<std::mutex> lk(m);
    errors.emplace_back(e);
  };
  auto onSuccess = [&](std::string_view)
  {
    std::lock_guard<std::mutex> lk(m);
    ++successes;
  };

  // The first request occupies the only sender for the whole 1500 ms.
  transport->post(peer.url(), R"({"n":0})", kHeaders, onSuccess, onError);
  ASSERT_TRUE(peer.waitForRequests(1, 5s)) << "the sender never picked the first request up";

  // Two more fill the queue exactly to its bound.
  transport->post(peer.url(), R"({"n":1})", kHeaders, onSuccess, onError);
  transport->post(peer.url(), R"({"n":2})", kHeaders, onSuccess, onError);

  {
    std::lock_guard<std::mutex> lk(m);
    EXPECT_TRUE(errors.empty()) << "a request inside the bound was refused: " << errors.front();
  }

  // The fourth is over the bound and must come back refused, on the spot.
  transport->post(peer.url(), R"({"n":3})", kHeaders, onSuccess, onError);

  std::string overflow;
  {
    std::lock_guard<std::mutex> lk(m);
    ASSERT_EQ(errors.size(), 1u)
        << "a request past a queue bound of two was neither queued nor refused";
    overflow = errors.front();
  }
  EXPECT_NE(overflow.find("queue is full"), std::string::npos) << overflow;

  // stop() has to answer the two still sitting in the queue.
  transport->stop();

  std::vector<std::string> finalErrors;
  {
    std::lock_guard<std::mutex> lk(m);
    finalErrors = errors;
    EXPECT_EQ(successes, 0) << "a silent peer answered nothing, so nothing may report success";
  }

  ASSERT_EQ(finalErrors.size(), 4u)
      << "every one of the four requests must be answered exactly once; " << finalErrors.size()
      << " answers arrived";
  const auto stopped = static_cast<std::size_t>(
      std::count_if(finalErrors.begin(), finalErrors.end(),
                    [](const std::string& e)
                    {
                      return e.find("stopped before the request was sent") != std::string::npos;
                    }));
  EXPECT_EQ(stopped, 2u) << "the requests still queued at stop() were not answered";

  // Only the first request ever left the process.
  EXPECT_EQ(peer.requestCount(), 1u) << "a refused or stopped request reached the venue anyway";

  transport.reset();
}

// The request outlives the call. post() copies what it is given and returns;
// by the time the sender thread reaches it the caller's buffer may be gone,
// which is the difference between sending the order that was placed and
// sending whatever now occupies that memory.
TEST(CurlTransportTimeout, QueuedRequestOwnsTheBodyItWasGiven)
{
  LoopbackPeer peer(LoopbackPeer::Mode::SlowRespond, 200ms);
  CurlTransport transport(4, CurlTimeoutConfig{.connectTimeoutMs = 2000, .requestTimeoutMs = 5000},
                          CurlDispatchConfig{.senderThreads = 1, .maxQueueDepth = 8});

  Completion blockerDone;
  Completion realDone;

  // Occupies the single sender so the request below is still in the queue
  // while the caller's buffer is being destroyed.
  transport.post(
      peer.url(), R"({"blocker":true})", kHeaders,
      [&](std::string_view)
      {
        blockerDone.signal();
      },
      [&](std::string_view)
      {
        blockerDone.signal();
      });
  ASSERT_TRUE(peer.waitForRequests(1, 5s));

  const std::string original = R"({"clientOid":"4242","price":"60000.5"})";
  std::string body = original;
  transport.post(
      peer.url(), body, kHeaders,
      [&](std::string_view)
      {
        realDone.signal();
      },
      [&](std::string_view)
      {
        realDone.signal();
      });

  // Overwritten in place and then released: a borrowed view reads this.
  body.assign(original.size(), 'X');
  body.clear();
  body.shrink_to_fit();

  ASSERT_TRUE(blockerDone.waitFor(15s));
  ASSERT_TRUE(realDone.waitFor(15s));
  ASSERT_TRUE(peer.waitForRequests(2, 5s));

  const auto bodies = peer.bodies();
  ASSERT_EQ(bodies.size(), 2u);
  EXPECT_EQ(bodies[1], original) << "the venue received a body the caller had already destroyed";
}

// The connect timeout is in milliseconds too, and it is the one that answers
// for a venue address that swallows the SYN -- the request timeout is the
// outer bound, not the useful one. 192.0.2.1 is RFC 5737 TEST-NET-1: routable
// nowhere, reserved for exactly this. A host that answers it immediately
// (no route at all) cannot show the difference, so the test says so and skips
// rather than passing on evidence it does not have.
TEST(CurlTransportTimeout, ConnectTimeoutIsBoundedInMilliseconds)
{
  {
    // Probe: does a connect to TEST-NET-1 hang, or is it refused at once?
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(fd, 0);
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = ::inet_addr("192.0.2.1");
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    bool answersImmediately = (rc == 0);
    if (rc < 0 && errno == EINPROGRESS)
    {
      pollfd pfd{fd, POLLOUT, 0};
      answersImmediately = ::poll(&pfd, 1, 400) > 0;
    }
    else if (rc < 0)
    {
      answersImmediately = true;
    }
    ::close(fd);
    if (answersImmediately)
    {
      GTEST_SKIP() << "this host answers 192.0.2.1 immediately instead of leaving the connect "
                      "pending, so a connect timeout is not observable here";
    }
  }

  CurlTransport transport(1, CurlTimeoutConfig{.connectTimeoutMs = 300, .requestTimeoutMs = 30000});

  Completion done;
  std::string error;
  const auto start = std::chrono::steady_clock::now();

  transport.post(
      "http://192.0.2.1/order", R"({"x":1})", kHeaders,
      [&](std::string_view)
      {
        done.signal();
      },
      [&](std::string_view e)
      {
        error = std::string(e);
        done.signal();
      });

  ASSERT_TRUE(done.waitFor(40s)) << "the connect was never given up on at all";
  const int64_t waited = elapsedMs(start);
  EXPECT_LE(waited, 1500) << "gave up on an unroutable address after " << waited
                          << " ms with a 300 ms connect timeout configured -- the connect budget "
                             "is not being honoured in milliseconds";
  EXPECT_FALSE(error.empty()) << "an unreachable address must be reported as an error";
}

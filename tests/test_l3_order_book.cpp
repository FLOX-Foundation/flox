/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

#include "flox/book/l3/l3_order_book.h"
#include "flox/common.h"

#include <gtest/gtest.h>
#include <cstddef>

#ifdef FLOX_UNIT_TEST
namespace flox
{
class L3OrderBookProbe
{
 public:
  using Index = std::uint32_t;

  template <std::size_t N>
  static Index levelHead(const L3OrderBook<N>& book, Price price, Side side)
  {
    const auto& level = (side == Side::BUY) ? book.bids_ : book.asks_;
    for (Index i = 0; i < N; ++i)
    {
      if (level[i].price == price)
      {
        return level[i].head;
      }
    }
    return L3OrderBook<>::kInvalid;
  }

  template <std::size_t N>
  static OrderId slotId(const L3OrderBook<N>& book, Index idx)
  {
    return book.orders_[idx].id;
  }

  template <std::size_t N>
  static Index hashBucketIndex(const L3OrderBook<N>& book, OrderId id)
  {
    for (Index i = 0; i < 2 * N; ++i)
    {
      if (book.orderIndices_[i].id == id)
      {
        return i;
      }
    }
    return L3OrderBook<>::kInvalid;
  }

  template <std::size_t N>
  static Index next(const L3OrderBook<N>& book, Index idx)
  {
    return book.orders_[idx].next;
  }

  template <std::size_t N>
  static Index prev(const L3OrderBook<N>& book, Index idx)
  {
    return book.orders_[idx].prev;
  }

  template <std::size_t N>
  static Index kInvalid(const L3OrderBook<N>& book)
  {
    return book.kInvalid;
  }
};
}  // namespace flox
#endif

using namespace flox;

// Lifecycle Tests
TEST(L3OrderBookTest, BookStartsEmpty)
{
  L3OrderBook<> orderBook;
  ASSERT_EQ(orderBook.bestBid(), std::nullopt);
  ASSERT_EQ(orderBook.bestAsk(), std::nullopt);
}

TEST(L3OrderBookTest, Add)
{
  L3OrderBook<> orderBook;
  Price price = Price::fromDouble(100.0);
  OrderStatus status = OrderStatus::NotFound;

  status = orderBook.addOrder(OrderId{12345678901231}, price, Quantity::fromDouble(1.0), Side::BUY);
  ASSERT_EQ(status, OrderStatus::Ok);

  status = orderBook.addOrder(OrderId{12345678901232}, price, Quantity::fromDouble(1.0), Side::BUY);
  ASSERT_EQ(status, OrderStatus::Ok);

  status = orderBook.addOrder(OrderId{12345678901233}, price, Quantity::fromDouble(1.0), Side::BUY);
  ASSERT_EQ(status, OrderStatus::Ok);

  ASSERT_EQ(orderBook.bidAtPrice(price), Quantity::fromDouble(3.0));

  orderBook.addOrder(OrderId{12345678901234}, price, Quantity::fromDouble(1.0), Side::BUY);
  ASSERT_EQ(orderBook.bidAtPrice(price), Quantity::fromDouble(4.0));

  orderBook.addOrder(OrderId{12345678901235}, Price::fromDouble(102.5), Quantity::fromDouble(1.0), Side::BUY);
  ASSERT_EQ(*orderBook.bestBid(), Price::fromDouble(102.5));
}

TEST(L3OrderBookTest, Remove)
{
  L3OrderBook<> orderBook;
  OrderStatus status = OrderStatus::NotFound;

  orderBook.addOrder(OrderId{12345678901231}, Price::fromDouble(99.0), Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901232}, Price::fromDouble(100.0), Quantity::fromDouble(1.0), Side::BUY);

  status = orderBook.removeOrder(OrderId{12345678901232});

  ASSERT_EQ(status, OrderStatus::Ok);
  ASSERT_EQ(*orderBook.bestBid(), Price::fromDouble(99.0));
  ASSERT_EQ(orderBook.bidAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(0.0));

  orderBook.removeOrder(OrderId{12345678901231});
  ASSERT_EQ(orderBook.bestBid(), std::nullopt);
}

TEST(L3OrderBookTest, Modify)
{
  L3OrderBook<> orderBook;
  Price price = Price::fromDouble(100.0);
  OrderStatus status = OrderStatus::NotFound;

  orderBook.addOrder(OrderId{12345678901231}, price, Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901232}, price, Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901233}, price, Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901234}, price, Quantity::fromDouble(1.0), Side::BUY);
  ASSERT_EQ(orderBook.bidAtPrice(price), Quantity::fromDouble(4.0));

  status = orderBook.modifyOrder(12345678901234, Quantity::fromDouble(2.0));

  ASSERT_EQ(status, OrderStatus::Ok);
  ASSERT_EQ(orderBook.bidAtPrice(price), Quantity::fromDouble(5.0));
}

// Proven by behavior under capacity pressure
TEST(L3OrderBookTest, ReuseFreedSlot)
{
  L3OrderBook<4> orderBook;
  Price price = Price::fromDouble(100.0);

  orderBook.addOrder(OrderId{12345678901231}, price, Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901232}, price, Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901233}, price, Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901234}, price, Quantity::fromDouble(1.0), Side::BUY);

  orderBook.removeOrder(OrderId{12345678901234});
  const auto status = orderBook.addOrder(OrderId{12345678901235}, Price::fromDouble(101.0), Quantity::fromDouble(1.0), Side::BUY);
  ASSERT_EQ(status, OrderStatus::Ok);
  ASSERT_EQ(*orderBook.bestBid(), Price::fromDouble(101.0));
}

// Corner cases
TEST(L3OrderBookTest, AddExistingOrder)
{
  L3OrderBook<> orderBook;
  orderBook.addOrder(OrderId{12345678901234}, Price::fromDouble(100.0), Quantity::fromDouble(8.2), Side::BUY);

  const auto status = orderBook.addOrder(OrderId{12345678901234}, Price::fromDouble(100.0), Quantity::fromDouble(1.0), Side::BUY);

  ASSERT_EQ(status, OrderStatus::Extant);
  ASSERT_EQ(orderBook.bidAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(8.2));
}

TEST(L3OrderBookTest, AddToFullBook)
{
  L3OrderBook<4> orderBook;
  const auto price = Price::fromDouble(100.0);

  orderBook.addOrder(OrderId{12345678901231}, price, Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901232}, price, Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901233}, price, Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901234}, price, Quantity::fromDouble(1.0), Side::BUY);

  OrderStatus status = OrderStatus::Ok;

  status = orderBook.addOrder(OrderId{12345678901235}, Price::fromDouble(100.01), Quantity::fromDouble(1.0), Side::BUY);
  ASSERT_EQ(status, OrderStatus::NoCapacity);
  ASSERT_EQ(*orderBook.bestBid(), Price::fromDouble(100.0));
  ASSERT_EQ(orderBook.bidAtPrice(Price::fromDouble(100.01)), Quantity{});

  status = orderBook.addOrder(OrderId{12345678901235}, Price::fromDouble(100.0), Quantity::fromDouble(1.0), Side::BUY);
  ASSERT_EQ(status, OrderStatus::NoCapacity);
  ASSERT_EQ(*orderBook.bestBid(), Price::fromDouble(100.0));
  ASSERT_EQ(orderBook.bidAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(4.0));
}

TEST(L3OrderBookTest, RemoveNonExistingOrder)
{
  L3OrderBook<4> orderBook;
  const auto price = Price::fromDouble(100.0);

  orderBook.addOrder(OrderId{12345678901231}, price, Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901232}, price, Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901233}, price, Quantity::fromDouble(1.0), Side::BUY);
  orderBook.addOrder(OrderId{12345678901234}, price, Quantity::fromDouble(1.0), Side::BUY);

  OrderStatus status = OrderStatus::Ok;

  status = orderBook.removeOrder(OrderId{12345678901235});
  ASSERT_EQ(status, OrderStatus::NotFound);
  ASSERT_EQ(*orderBook.bestBid(), Price::fromDouble(100.0));
  ASSERT_EQ(orderBook.bidAtPrice(Price::fromDouble(100.01)), Quantity{});

  status = orderBook.removeOrder(OrderId{12345678901235});
  ASSERT_EQ(status, OrderStatus::NotFound);
  ASSERT_EQ(*orderBook.bestBid(), Price::fromDouble(100.0));
  ASSERT_EQ(orderBook.bidAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(4.0));
}

TEST(L3OrderBookTest, ModifyNonExistingOrder)
{
  L3OrderBook<> book;
  const auto status = book.modifyOrder(OrderId{1000}, Quantity::fromDouble(100.0));
  ASSERT_EQ(status, OrderStatus::NotFound);
  ASSERT_EQ(book.bestBid(), std::nullopt);
  ASSERT_EQ(book.bestAsk(), std::nullopt);
}

// Invariants
TEST(L3OrderBookTest, TimePriceFIFO)
{
  Price price = Price::fromDouble(100.0);
  Quantity qty = Quantity::fromDouble(5.0);
  Side side = Side::SELL;
  OrderId id{12345678901230};
  constexpr size_t N = 8192;

  L3OrderBook<N> book;

  for (size_t i = 0; i < N; ++i)
  {
    book.addOrder(id, price, qty, side);
    id += 1;
  }

  auto idx = L3OrderBookProbe::levelHead(book, price, side);
  size_t assertions{0};

  while (idx != L3OrderBookProbe::kInvalid(book))
  {
    auto nxt = L3OrderBookProbe::next(book, idx);

    if (nxt != L3OrderBookProbe::kInvalid(book))
    {
      ASSERT_TRUE(L3OrderBookProbe::slotId(book, idx) + 1 == L3OrderBookProbe::slotId(book, nxt));
      assertions++;
    }
    idx = nxt;
  }
  ASSERT_EQ(assertions, N - 1);
}

TEST(L3OrderBookTest, BestBid)
{
  L3OrderBook<> book;
  Price price = Price::fromDouble(100.0);
  Quantity qty = Quantity::fromDouble(5.0);
  Side side = Side::BUY;
  OrderId id{12345678901230};

  book.addOrder(id, price, qty, side);
  id++;
  price = Price::fromDouble(150.0);
  book.addOrder(id, price, qty, side);
  ASSERT_EQ(*book.bestBid(), Price::fromDouble(150.0));
  ASSERT_EQ(book.bidAtPrice(Price::fromDouble(150.0)), Quantity::fromDouble(5.0));

  book.removeOrder(id);
  ASSERT_EQ(*book.bestBid(), Price::fromDouble(100.0));
}

TEST(L3OrderBookTest, BestAsk)
{
  L3OrderBook<> book;
  Price price = Price::fromDouble(100.0);
  Quantity qty = Quantity::fromDouble(5.0);
  Side side = Side::SELL;
  OrderId id{12345678901230};

  book.addOrder(id, price, qty, side);
  id++;
  price = Price::fromDouble(150.0);
  book.addOrder(id, price, qty, side);
  ASSERT_EQ(*book.bestAsk(), Price::fromDouble(100.0));
  ASSERT_EQ(book.askAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(5.0));
  id--;
  book.removeOrder(id);
  ASSERT_EQ(*book.bestAsk(), Price::fromDouble(150.0));
}

TEST(L3OrderBookTest, BidQuantityAtPrice)
{
  L3OrderBook<> book;

  book.addOrder(OrderId{1}, Price::fromDouble(100.0), Quantity::fromDouble(10.0), Side::BUY);
  book.addOrder(OrderId{2}, Price::fromDouble(100.0), Quantity::fromDouble(5.2), Side::BUY);
  ASSERT_EQ(book.bidAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(15.2));

  book.addOrder(OrderId{3}, Price::fromDouble(150.0), Quantity::fromDouble(10.0), Side::BUY);
  book.addOrder(OrderId{4}, Price::fromDouble(150.0), Quantity::fromDouble(20.1), Side::BUY);
  ASSERT_EQ(book.bidAtPrice(Price::fromDouble(150.0)), Quantity::fromDouble(30.1));

  book.addOrder(OrderId{5}, Price::fromDouble(200.0), Quantity::fromDouble(10.0), Side::BUY);
  book.addOrder(OrderId{6}, Price::fromDouble(200.0), Quantity::fromDouble(50.0), Side::BUY);
  ASSERT_EQ(book.bidAtPrice(Price::fromDouble(200.0)), Quantity::fromDouble(60.0));

  for (std::uint64_t i = 1; i < 6; i += 2)
  {
    book.removeOrder(OrderId{i});
  }

  ASSERT_EQ(book.bidAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(5.2));
  ASSERT_EQ(book.bidAtPrice(Price::fromDouble(150.0)), Quantity::fromDouble(20.1));
  ASSERT_EQ(book.bidAtPrice(Price::fromDouble(200.0)), Quantity::fromDouble(50.0));
}

TEST(L3OrderBookTest, AskQuantityAtPrice)
{
  L3OrderBook<> book;

  book.addOrder(OrderId{1}, Price::fromDouble(100.0), Quantity::fromDouble(10.0), Side::SELL);
  book.addOrder(OrderId{2}, Price::fromDouble(100.0), Quantity::fromDouble(5.2), Side::SELL);
  ASSERT_EQ(book.askAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(15.2));

  book.addOrder(OrderId{3}, Price::fromDouble(150.0), Quantity::fromDouble(10.0), Side::SELL);
  book.addOrder(OrderId{4}, Price::fromDouble(150.0), Quantity::fromDouble(20.1), Side::SELL);
  ASSERT_EQ(book.askAtPrice(Price::fromDouble(150.0)), Quantity::fromDouble(30.1));

  book.addOrder(OrderId{5}, Price::fromDouble(200.0), Quantity::fromDouble(10.0), Side::SELL);
  book.addOrder(OrderId{6}, Price::fromDouble(200.0), Quantity::fromDouble(50.0), Side::SELL);
  ASSERT_EQ(book.askAtPrice(Price::fromDouble(200.0)), Quantity::fromDouble(60.0));

  for (std::uint64_t i = 1; i < 6; i += 2)
  {
    book.removeOrder(OrderId{i});
  }

  ASSERT_EQ(book.askAtPrice(Price::fromDouble(100.0)), Quantity::fromDouble(5.2));
  ASSERT_EQ(book.askAtPrice(Price::fromDouble(150.0)), Quantity::fromDouble(20.1));
  ASSERT_EQ(book.askAtPrice(Price::fromDouble(200.0)), Quantity::fromDouble(50.0));
}

TEST(L3OrderBookTest, PreferTombstoneIdx)
{
  L3OrderBook<10> book;
  Price price = Price::fromDouble(100.0);
  Quantity qty = Quantity::fromDouble(5.0);
  Side side = Side::SELL;

  // load the book with hash collisions
  book.addOrder(OrderId{7}, price, qty, side);  // orderIndices_ index = 7 since index = id % (2 * N)
  ASSERT_EQ(L3OrderBookProbe::hashBucketIndex(book, OrderId{7}), 7);
  book.addOrder(OrderId{27}, price, qty, side);  // orderIndices_ index = 8
  ASSERT_EQ(L3OrderBookProbe::hashBucketIndex(book, OrderId{27}), 8);
  book.addOrder(OrderId{47}, price, qty, side);  // orderIndices_ index = 9
  ASSERT_EQ(L3OrderBookProbe::hashBucketIndex(book, OrderId{47}), 9);

  // create a tombstone at index 8
  book.removeOrder(OrderId{27});

  // load the book with another collision
  book.addOrder(OrderId{67}, price, qty, side);

  // now check that this newly placed order is position at the tombstone index 8;
  ASSERT_EQ(L3OrderBookProbe::hashBucketIndex(book, OrderId{67}), 8);
}

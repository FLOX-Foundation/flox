// python/composite_book_bindings.h

#pragma once

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "bindings_common.h"
#include "flox/book/composite_book_matrix.h"
#include "flox/book/events/book_update_event.h"
#include "flox/common.h"

#include <memory>
#include <memory_resource>

namespace py = pybind11;

namespace
{

using namespace flox;

class PyCompositeBookMatrix
{
 public:
  explicit PyCompositeBookMatrix(size_t stalenessThresholdMs)
      : _stalenessThresholdNs(static_cast<int64_t>(stalenessThresholdMs) * 1'000'000LL)
  {
  }

  void updateBook(uint16_t exchange, uint32_t symbol,
                  py::array_t<double, py::array::c_style | py::array::forcecast> bidPx,
                  py::array_t<double, py::array::c_style | py::array::forcecast> bidQty,
                  py::array_t<double, py::array::c_style | py::array::forcecast> askPx,
                  py::array_t<double, py::array::c_style | py::array::forcecast> askQty,
                  int64_t recvNs, bool isDelta)
  {
    size_t nb = bidPx.size();
    size_t na = askPx.size();
    // Same gap as PyOrderBook::applyUpdate: bidQty/askQty were read up to
    // nb/na with no check that they actually have that many elements.
    checkSameSize(nb, static_cast<size_t>(bidQty.size()), "bid_px and bid_qty size");
    checkSameSize(na, static_cast<size_t>(askQty.size()), "ask_px and ask_qty size");
    const auto* bp = bidPx.data();
    const auto* bq = bidQty.data();
    const auto* ap = askPx.data();
    const auto* aq = askQty.data();

    std::byte buf[32768];
    std::pmr::monotonic_buffer_resource res(buf, sizeof(buf));
    BookUpdateEvent ev(&res);
    ev.update.symbol = symbol;
    ev.update.type = isDelta ? BookUpdateType::DELTA : BookUpdateType::SNAPSHOT;
    ev.sourceExchange = exchange;
    ev.recvNs = MonoNanos::fromRaw(static_cast<uint64_t>(recvNs));

    ev.update.bids.reserve(nb);
    for (size_t i = 0; i < nb; ++i)
    {
      ev.update.bids.push_back({Price::fromDouble(bp[i]), Quantity::fromDouble(bq[i])});
    }
    ev.update.asks.reserve(na);
    for (size_t i = 0; i < na; ++i)
    {
      ev.update.asks.push_back({Price::fromDouble(ap[i]), Quantity::fromDouble(aq[i])});
    }

    _matrix.onBookUpdate(ev);
  }

  py::object bestBid(uint32_t symbol) const
  {
    auto q = _matrix.bestBid(symbol);
    if (!q.valid)
    {
      return py::none();
    }
    py::dict d;
    d["price"] = Price::fromRaw(q.priceRaw).toDouble();
    d["quantity"] = Quantity::fromRaw(q.qtyRaw).toDouble();
    d["exchange"] = q.exchange;
    return d;
  }

  py::object bestAsk(uint32_t symbol) const
  {
    auto q = _matrix.bestAsk(symbol);
    if (!q.valid)
    {
      return py::none();
    }
    py::dict d;
    d["price"] = Price::fromRaw(q.priceRaw).toDouble();
    d["quantity"] = Quantity::fromRaw(q.qtyRaw).toDouble();
    d["exchange"] = q.exchange;
    return d;
  }

  py::object bidForExchange(uint32_t symbol, uint16_t exchange) const
  {
    auto q = _matrix.bidForExchange(symbol, exchange);
    if (!q.valid)
    {
      return py::none();
    }
    py::dict d;
    d["price"] = Price::fromRaw(q.priceRaw).toDouble();
    d["quantity"] = Quantity::fromRaw(q.qtyRaw).toDouble();
    d["exchange"] = q.exchange;
    return d;
  }

  py::object askForExchange(uint32_t symbol, uint16_t exchange) const
  {
    auto q = _matrix.askForExchange(symbol, exchange);
    if (!q.valid)
    {
      return py::none();
    }
    py::dict d;
    d["price"] = Price::fromRaw(q.priceRaw).toDouble();
    d["quantity"] = Quantity::fromRaw(q.qtyRaw).toDouble();
    d["exchange"] = q.exchange;
    return d;
  }

  bool hasArbitrageOpportunity(uint32_t symbol) const
  {
    return _matrix.hasArbitrageOpportunity(symbol);
  }

  py::object spread(uint32_t symbol) const
  {
    int64_t raw = _matrix.spreadRaw(symbol);
    if (raw == 0)
    {
      return py::none();
    }
    return py::cast(Price::fromRaw(raw).toDouble());
  }

  void markStale(uint16_t exchange, uint32_t symbol)
  {
    _matrix.markStale(exchange, symbol);
  }

  void markExchangeStale(uint16_t exchange)
  {
    _matrix.markExchangeStale(exchange);
  }

  void checkStaleness(int64_t nowNs)
  {
    _matrix.checkStaleness(nowNs, _stalenessThresholdNs);
  }

 private:
  CompositeBookMatrix<4> _matrix;
  int64_t _stalenessThresholdNs;
};

}  // namespace

inline void bindCompositeBook(py::module_& m)
{
  py::class_<PyCompositeBookMatrix>(m, "CompositeBookMatrix",
                                    "Cross-exchange composite order book. "
                                    "Tracks best bid/ask across up to 4 exchanges per symbol.")
      .def(py::init<size_t>(),
           "Create a CompositeBookMatrix with staleness threshold in milliseconds",
           py::arg("staleness_threshold_ms") = 5000)
      .def("update_book", &PyCompositeBookMatrix::updateBook,
           "Feed a book update from an exchange. is_delta=False (the default) replaces "
           "both sides of that exchange's top-of-book wholesale, as a real snapshot "
           "would. is_delta=True only updates the side(s) present in bid_prices/"
           "ask_prices; a side not present in a delta call is left exactly as it was, "
           "not cleared.",
           py::arg("exchange"), py::arg("symbol"),
           py::arg("bid_prices"), py::arg("bid_quantities"),
           py::arg("ask_prices"), py::arg("ask_quantities"),
           py::arg("recv_ns") = 0, py::arg("is_delta") = false)
      .def("best_bid", &PyCompositeBookMatrix::bestBid,
           "Best bid across all exchanges (dict with price, quantity, exchange) or None",
           py::arg("symbol"))
      .def("best_ask", &PyCompositeBookMatrix::bestAsk,
           "Best ask across all exchanges (dict with price, quantity, exchange) or None",
           py::arg("symbol"))
      .def("bid_for_exchange", &PyCompositeBookMatrix::bidForExchange,
           "Best bid on a specific exchange or None",
           py::arg("symbol"), py::arg("exchange"))
      .def("ask_for_exchange", &PyCompositeBookMatrix::askForExchange,
           "Best ask on a specific exchange or None",
           py::arg("symbol"), py::arg("exchange"))
      .def("has_arbitrage_opportunity", &PyCompositeBookMatrix::hasArbitrageOpportunity,
           "Check if best bid on one exchange > best ask on another",
           py::arg("symbol"))
      .def("spread", &PyCompositeBookMatrix::spread,
           "Composite spread (best ask - best bid) or None",
           py::arg("symbol"))
      .def("mark_stale", &PyCompositeBookMatrix::markStale,
           "Mark a specific exchange+symbol as stale",
           py::arg("exchange"), py::arg("symbol"))
      .def("mark_exchange_stale", &PyCompositeBookMatrix::markExchangeStale,
           "Mark all symbols on an exchange as stale",
           py::arg("exchange"))
      .def("check_staleness", &PyCompositeBookMatrix::checkStaleness,
           "Check all entries against staleness threshold using current timestamp",
           py::arg("now_ns"));
}

/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "amm_curve_bindings.h"
#include "dex_amount_bindings.h"

#include "lrvx/backtest/constant_product_curve.h"
#include "lrvx/connector/amm_dex_connector.h"
#include "lrvx/replay/pool_state_tape.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace lrvx
{

namespace py = pybind11;

// PoolReplay -- the result of replaying a pool-state tape. The pool state was derived
// by replaying the deltas through the exact curve; drift_count is the number of
// Checkpoints that disagreed with the replayed state, trade_count the swaps applied,
// and curve() the final pool as an AmmCurve (an owned copy).
class PyPoolReplay
{
 public:
  PyPoolReplay(const std::vector<uint8_t>& bytes, std::size_t baseIdx, std::size_t quoteIdx,
               unsigned baseDec, unsigned quoteDec)
      : _seed(std::make_unique<lrvx::ConstantProductCurve>(lrvx::u256(1), lrvx::u256(1), 1, 1)),
        _conn(std::make_unique<lrvx::AmmDexConnector>("amm", lrvx::SymbolId{1}, *_seed, baseIdx,
                                                      quoteIdx, baseDec, quoteDec, 1,
                                                      lrvx::u256(1))),
        _replay(std::make_unique<lrvx::PoolStateReplay>(*_conn))
  {
    _conn->setCallbacks([](const lrvx::BookUpdateEvent&) {},
                        [this](const lrvx::TradeEvent&)
                        { ++_trades; });
    _replay->run(bytes);
  }

  std::size_t driftCount() const { return _replay->driftCount(); }
  std::size_t tradeCount() const { return _trades; }

  PyAmmCurve curve() const
  {
    const lrvx::INTokenCurve* c = _replay->curve();
    if (c == nullptr)
    {
      throw py::value_error("pool replay: the tape had no checkpoint, so there is no curve");
    }
    return PyAmmCurve(c->clone());
  }

 private:
  std::unique_ptr<lrvx::INTokenCurve> _seed;
  std::unique_ptr<lrvx::AmmDexConnector> _conn;
  std::unique_ptr<lrvx::PoolStateReplay> _replay;
  std::size_t _trades{0};
};

// PoolTape -- build a pool-state tape (a delta log) then replay it. Amounts are int
// (the u256 boundary). Build a descriptor, then alternating checkpoints and swaps in
// timestamp order, then call replay().
class PyPoolTape
{
 public:
  void descriptorConstantProduct(uint64_t feeNum, uint64_t feeDen, uint8_t baseDec,
                                 uint8_t quoteDec)
  {
    lrvx::PoolStateWriter(_bytes).descriptorConstantProduct(feeNum, feeDen, baseDec, quoteDec);
  }
  void descriptorRaydiumCp(uint64_t tradeFeeRate, uint64_t creatorFeeRate, bool creatorFeeOnInput,
                           uint8_t baseDec, uint8_t quoteDec)
  {
    lrvx::PoolStateWriter(_bytes).descriptorRaydiumCp(tradeFeeRate, creatorFeeRate,
                                                      creatorFeeOnInput, baseDec, quoteDec);
  }
  void descriptorClmm(uint8_t venue, uint32_t feePips, uint8_t baseDec, uint8_t quoteDec)
  {
    lrvx::PoolStateWriter(_bytes).descriptorClmm(static_cast<lrvx::PoolVenue>(venue), feePips,
                                                 baseDec, quoteDec);
  }
  void checkpoint(int64_t ts, const py::int_& reserve0, const py::int_& reserve1)
  {
    lrvx::PoolStateWriter(_bytes).checkpoint(ts, toU256(reserve0), toU256(reserve1));
  }
  void checkpointClmm(int64_t ts, const py::int_& sqrtPrice, const py::int_& liquidity,
                      const py::list& ticks)
  {
    std::vector<lrvx::ClTick> ts2;
    ts2.reserve(ticks.size());
    for (const py::handle& t : ticks)
    {
      auto pair = t.cast<std::pair<py::int_, py::int_>>();
      ts2.push_back({toU256(pair.first), lrvx::i256::fromDec(decimalFromInt(pair.second))});
    }
    lrvx::PoolStateWriter(_bytes).checkpointClmm(ts, toU256(sqrtPrice), toU256(liquidity), ts2);
  }
  void swap(int64_t ts, bool baseForQuote, const py::int_& amountIn)
  {
    lrvx::PoolStateWriter(_bytes).swap(ts, baseForQuote, toU256(amountIn));
  }

  PyPoolReplay replay(std::size_t baseIdx, std::size_t quoteIdx, unsigned baseDec,
                      unsigned quoteDec) const
  {
    return PyPoolReplay(_bytes, baseIdx, quoteIdx, baseDec, quoteDec);
  }

 private:
  static lrvx::u256 toU256(const py::int_& v) { return lrvx::u256::fromDec(decimalFromInt(v)); }

  std::vector<uint8_t> _bytes;
};

inline void bindPoolTape(py::module_& m)
{
  py::class_<PyPoolReplay>(m, "PoolReplay")
      .def("drift_count", &PyPoolReplay::driftCount)
      .def("trade_count", &PyPoolReplay::tradeCount)
      .def("curve", &PyPoolReplay::curve);

  py::class_<PyPoolTape>(m, "PoolTape")
      .def(py::init<>())
      .def("descriptor_constant_product", &PyPoolTape::descriptorConstantProduct, py::arg("fee_num"),
           py::arg("fee_den"), py::arg("base_dec"), py::arg("quote_dec"))
      .def("descriptor_raydium_cp", &PyPoolTape::descriptorRaydiumCp, py::arg("trade_fee_rate"),
           py::arg("creator_fee_rate"), py::arg("creator_fee_on_input"), py::arg("base_dec"),
           py::arg("quote_dec"))
      .def("descriptor_clmm", &PyPoolTape::descriptorClmm, py::arg("venue"), py::arg("fee_pips"),
           py::arg("base_dec"), py::arg("quote_dec"))
      .def("checkpoint", &PyPoolTape::checkpoint, py::arg("ts_ns"), py::arg("reserve0"),
           py::arg("reserve1"))
      .def("checkpoint_clmm", &PyPoolTape::checkpointClmm, py::arg("ts_ns"), py::arg("sqrt_price"),
           py::arg("liquidity"), py::arg("ticks") = py::list())
      .def("swap", &PyPoolTape::swap, py::arg("ts_ns"), py::arg("base_for_quote"),
           py::arg("amount_in"))
      .def("replay", &PyPoolTape::replay, py::arg("base_idx") = 0, py::arg("quote_idx") = 1,
           py::arg("base_dec") = 18, py::arg("quote_dec") = 18);
}

}  // namespace lrvx

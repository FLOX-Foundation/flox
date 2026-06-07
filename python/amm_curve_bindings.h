/*
 * Flox Engine
 * Developed by FLOX Foundation (https://github.com/FLOX-Foundation)
 *
 * Copyright (c) 2025 FLOX Foundation
 * Licensed under the MIT License. See LICENSE file in the project root for
 * full license information.
 */

#pragma once

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "dex_amount_bindings.h"

#include "flox/backtest/concentrated_liquidity_curve.h"
#include "flox/backtest/constant_product_curve.h"
#include "flox/backtest/ntoken_curve.h"
#include "flox/backtest/raydium_cp_curve.h"

#include <memory>
#include <utility>
#include <vector>

namespace flox_py
{

namespace py = pybind11;

// AmmCurve -- the exact AMM pool curve (INTokenCurve) for Python. Amounts are plain
// `int` (the u256 boundary, lossless to 2^256 - 1). Construct per venue with the
// pool's parameters, then price a swap exactly:
//
//   pool = AmmCurve.constant_product(r0, r1, 997, 1000)
//   out  = pool.amount_out(0, 1, 10**18)   # token0 in -> token1 out, to the wei
//
// amount_out does not move the pool; apply_swap moves it and returns the output.
class PyAmmCurve
{
 public:
  explicit PyAmmCurve(std::unique_ptr<flox::INTokenCurve> c) : _c(std::move(c)) {}

  static PyAmmCurve constantProduct(const py::int_& reserve0, const py::int_& reserve1,
                                    uint64_t feeNum, uint64_t feeDen)
  {
    return PyAmmCurve(std::make_unique<flox::ConstantProductCurve>(
        toU256(reserve0), toU256(reserve1), feeNum, feeDen));
  }

  static PyAmmCurve raydiumCp(const py::int_& reserve0, const py::int_& reserve1,
                              uint64_t tradeFeeRate, uint64_t creatorFeeRate, bool creatorFeeOnInput)
  {
    return PyAmmCurve(std::make_unique<flox::RaydiumCpCurve>(
        toU256(reserve0), toU256(reserve1), tradeFeeRate, creatorFeeRate, creatorFeeOnInput));
  }

  // ticks: a list of (sqrt_ratio: int, liquidity_net: int) for the initialized ticks,
  // empty for an in-range swap that crosses none.
  static PyAmmCurve uniswapV3(const py::int_& sqrtPriceX96, const py::int_& liquidity,
                              uint32_t feePips, const py::list& ticks)
  {
    std::vector<flox::ClTick> ts;
    ts.reserve(ticks.size());
    for (const py::handle& t : ticks)
    {
      auto pair = t.cast<std::pair<py::int_, py::int_>>();
      ts.push_back({toU256(pair.first), toI256(pair.second)});
    }
    return PyAmmCurve(std::make_unique<flox::ConcentratedLiquidityCurve>(
        toU256(sqrtPriceX96), toU256(liquidity), feePips, std::move(ts)));
  }

  std::size_t tokenCount() const { return _c->tokenCount(); }

  py::int_ amountOut(std::size_t i, std::size_t j, const py::int_& amountIn) const
  {
    return fromU256(_c->amountOut(i, j, toU256(amountIn)));
  }

  py::int_ applySwap(std::size_t i, std::size_t j, const py::int_& amountIn)
  {
    return fromU256(_c->applySwap(i, j, toU256(amountIn)));
  }

  py::list balances() const
  {
    py::list out;
    for (const flox::u256& b : _c->balances())
    {
      out.append(fromU256(b));
    }
    return out;
  }

  PyAmmCurve clone() const { return PyAmmCurve(_c->clone()); }

 private:
  static flox::u256 toU256(const py::int_& v) { return flox::u256::fromDec(decimalFromInt(v)); }
  static flox::i256 toI256(const py::int_& v) { return flox::i256::fromDec(decimalFromInt(v)); }
  static py::int_ fromU256(const flox::u256& v) { return intFromDecimal(v.toDec()); }

  std::unique_ptr<flox::INTokenCurve> _c;
};

inline void bindAmmCurve(py::module_& m)
{
  py::class_<PyAmmCurve>(m, "AmmCurve")
      .def_static("constant_product", &PyAmmCurve::constantProduct, py::arg("reserve0"),
                  py::arg("reserve1"), py::arg("fee_num") = 997, py::arg("fee_den") = 1000)
      .def_static("raydium_cp", &PyAmmCurve::raydiumCp, py::arg("reserve0"), py::arg("reserve1"),
                  py::arg("trade_fee_rate"), py::arg("creator_fee_rate") = 0,
                  py::arg("creator_fee_on_input") = true)
      .def_static("uniswap_v3", &PyAmmCurve::uniswapV3, py::arg("sqrt_price_x96"),
                  py::arg("liquidity"), py::arg("fee_pips"), py::arg("ticks") = py::list())
      .def("token_count", &PyAmmCurve::tokenCount)
      .def("amount_out", &PyAmmCurve::amountOut, py::arg("i"), py::arg("j"), py::arg("amount_in"))
      .def("apply_swap", &PyAmmCurve::applySwap, py::arg("i"), py::arg("j"), py::arg("amount_in"))
      .def("balances", &PyAmmCurve::balances)
      .def("clone", &PyAmmCurve::clone);
}

}  // namespace flox_py

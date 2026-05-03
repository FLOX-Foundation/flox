"""
Forward-looking labels (research-only). Distinct from indicators: feeding these into a live update loop is a look-ahead-bias bug.
"""
from __future__ import annotations
import numpy
import numpy.typing
import typing
__all__: list[str] = ['future_ctc_volatility', 'future_linear_slope', 'future_return']
def future_ctc_volatility(close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], horizon: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def future_linear_slope(close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], horizon: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...
def future_return(close: typing.Annotated[numpy.typing.ArrayLike, numpy.float64], horizon: typing.SupportsInt | typing.SupportsIndex) -> numpy.typing.NDArray[numpy.float64]:
    ...

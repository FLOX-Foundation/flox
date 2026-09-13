"""
Inline-SVG heatmap rendering (used by flox_py.report).
"""
from __future__ import annotations
import collections.abc
import typing
__all__: list[str] = ['heatmap_html', 'write_heatmap']
def heatmap_html(z: collections.abc.Sequence[collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex]], row_labels: collections.abc.Sequence[str] = [], col_labels: collections.abc.Sequence[str] = [], title: str = '', x_axis_name: str = '', y_axis_name: str = '', metric_name: str = '') -> str:
    """
    Render a self-contained HTML heatmap and return the string. z is a list[list[float]], row-major.
    """
def write_heatmap(path: str, z: collections.abc.Sequence[collections.abc.Sequence[typing.SupportsFloat | typing.SupportsIndex]], row_labels: collections.abc.Sequence[str] = [], col_labels: collections.abc.Sequence[str] = [], title: str = '', x_axis_name: str = '', y_axis_name: str = '', metric_name: str = '') -> None:
    """
    Render and write a heatmap HTML to disk.
    """

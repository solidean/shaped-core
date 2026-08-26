"""Sorted, disjoint, inclusive integer intervals.

The coverage engine asks the same four questions over and over — union, intersect, subtract, count —
and a diff's line numbers are dense runs, so intervals answer them in a few hundred entries where a set of ints would need hundreds of thousands.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable, Iterator


def _normalize(spans: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    """Sort, merge and drop empties, which is the invariant every IntervalList holds."""
    ordered = sorted((a, b) for a, b in spans if b >= a)
    merged: list[tuple[int, int]] = []
    for start, end in ordered:
        if merged and start <= merged[-1][1] + 1:
            if end > merged[-1][1]:
                merged[-1] = (merged[-1][0], end)
        else:
            merged.append((start, end))
    return merged


@dataclass(frozen=True)
class IntervalList:
    """A set of integers held as inclusive runs, always sorted, disjoint and merged."""

    spans: tuple[tuple[int, int], ...] = ()

    @staticmethod
    def of(spans: Iterable[tuple[int, int]]) -> IntervalList:
        return IntervalList(tuple(_normalize(spans)))

    @staticmethod
    def from_numbers(numbers: Iterable[int]) -> IntervalList:
        return IntervalList.of((n, n) for n in numbers)

    @staticmethod
    def span(start: int, end: int) -> IntervalList:
        return IntervalList.of([(start, end)])

    def __bool__(self) -> bool:
        return bool(self.spans)

    def __iter__(self) -> Iterator[tuple[int, int]]:
        return iter(self.spans)

    def __len__(self) -> int:
        """The number of integers covered, which is what every coverage percentage divides by."""
        return sum(end - start + 1 for start, end in self.spans)

    def contains(self, n: int) -> bool:
        for start, end in self.spans:
            if start <= n <= end:
                return True
            if start > n:
                break
        return False

    def union(self, other: IntervalList) -> IntervalList:
        return IntervalList.of([*self.spans, *other.spans])

    def intersect(self, other: IntervalList) -> IntervalList:
        out: list[tuple[int, int]] = []
        i = j = 0
        while i < len(self.spans) and j < len(other.spans):
            a_start, a_end = self.spans[i]
            b_start, b_end = other.spans[j]
            start, end = max(a_start, b_start), min(a_end, b_end)
            if start <= end:
                out.append((start, end))
            if a_end < b_end:
                i += 1
            else:
                j += 1
        return IntervalList(tuple(out))

    def subtract(self, other: IntervalList) -> IntervalList:
        out: list[tuple[int, int]] = []
        for start, end in self.spans:
            cursor = start
            for b_start, b_end in other.spans:
                if b_end < cursor:
                    continue
                if b_start > end:
                    break
                if b_start > cursor:
                    out.append((cursor, min(end, b_start - 1)))
                cursor = max(cursor, b_end + 1)
                if cursor > end:
                    break
            if cursor <= end:
                out.append((cursor, end))
        return IntervalList(tuple(out))

    def shift(self, delta: int) -> IntervalList:
        return IntervalList(tuple((a + delta, b + delta) for a, b in self.spans))

    def gaps_closed(self, gap: int) -> IntervalList:
        """The same set with runs closer than `gap` merged, for grouping neighbouring edits into one change."""
        if gap <= 0 or not self.spans:
            return self
        out = [self.spans[0]]
        for start, end in self.spans[1:]:
            if start - out[-1][1] - 1 <= gap:
                out[-1] = (out[-1][0], max(out[-1][1], end))
            else:
                out.append((start, end))
        return IntervalList(tuple(out))

    def format(self) -> str:
        return ",".join(f"{a}-{b}" if a != b else str(a) for a, b in self.spans)
